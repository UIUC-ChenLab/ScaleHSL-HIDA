//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#include "Transforms/Passes.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/Transforms/LoopUtils.h"

using namespace std;
using namespace mlir;
using namespace scalehls;

namespace {
struct AffineLoopPerfection
    : public AffineLoopPerfectionBase<AffineLoopPerfection> {
  void runOnOperation() override;
};
} // namespace

void AffineLoopPerfection::runOnOperation() {
  auto module = getOperation();
  auto builder = OpBuilder(module);

  // Walk through all functions and loops.
  for (auto func : module.getOps<FuncOp>()) {
    for (auto forOp : func.getOps<mlir::AffineForOp>()) {
      // Walk through all inner loops.
      SmallVector<mlir::AffineForOp, 4> loops;
      forOp.walk([&](mlir::AffineForOp loop) {
        if (!loops.empty()) {
          // Make sure the current loop is a sequential nested loop.
          // TODO: support parallel loops perfection? This tends to be much
          // complicated than a pure sequential loop stack, but seems possible.
          if (loop != loops.back().getParentOp()) {
            forOp.emitError("contains parallel inner loops, not supported");
            return;
          }
          auto innermostLoop = loops.front();

          // Collect all operations before the inner loop.
          SmallVector<Operation *, 4> frontOps;
          for (auto &op : loop.getBody()->getOperations()) {
            if (&op != loops.back().getOperation())
              frontOps.push_back(&op);
            else
              break;
          }

          // All operations before the inner loop should be moved to the
          // innermost loop, they are collected in frontOps.
          if (!frontOps.empty()) {
            // Create AffineIf in the front of the innermost loop.
            SmallVector<AffineExpr, 4> ifExprs;
            SmallVector<bool, 4> ifEqFlags;
            SmallVector<Value, 4> ifOperands;
            unsigned idx = 0;
            for (auto innerLoop : loops) {
              // Ensure constant lower bound.
              // TODO: Support variable bound. This is definitely possible.
              if (!innerLoop.hasConstantLowerBound()) {
                innerLoop.emitError(
                    "has non-constant lower bound, not supported");
                return;
              }

              // Create all components required by constructing if operation.
              ifExprs.push_back(
                  getAffineDimExpr(idx++, module.getContext()) -
                  getAffineConstantExpr(innerLoop.getConstantLowerBound(),
                                        module.getContext()));
              ifEqFlags.push_back(true);
              ifOperands.push_back(innerLoop.getInductionVar());
            }
            auto ifCondition = IntegerSet::get(idx, 0, ifExprs, ifEqFlags);

            // Set builder insertion point and create AffineIf operation.
            builder.setInsertionPointToStart(innermostLoop.getBody());
            auto ifOp = builder.create<mlir::AffineIfOp>(
                module.getLoc(), ifCondition, ifOperands,
                /*withElseRegion=*/false);

            // Move all operations in frontOps into the innermost loop. Note
            // that if the operation has result, it will always be executed.
            // However, if the operation doesn't have result (e.g. AffineStore
            // operation), it will be putted into the generated AffineIf
            // operation and conditionally executed.
            for (auto op : frontOps) {
              if (op->getNumResults())
                op->moveBefore(ifOp);
              else
                op->moveBefore(ifOp.getThenBlock()->getTerminator());
            }
          }

          // Collect all operations after the inner loop.
          SmallVector<Operation *, 4> backOps;
          auto &opList = loop.getBody()->getOperations();
          for (auto opIt = opList.rbegin(); opIt != opList.rend(); ++opIt) {
            auto &op = *opIt;
            if (!isa<mlir::AffineYieldOp>(op)) {
              if (&op != loops.back().getOperation())
                backOps.push_back(&op);
              else
                break;
            }
          }

          // All operations after the inner loop should be moved to the
          // innermost loop, they are collected in backOps.
          if (!backOps.empty()) {
            // Create AffineIf in the back of the innermost loop (before the
            // terminator).
            SmallVector<AffineExpr, 4> ifExprs;
            SmallVector<bool, 4> ifEqFlags;
            SmallVector<Value, 4> ifOperands;
            unsigned idx = 0;
            for (auto innerLoop : loops) {
              // Ensure constant upper bound.
              // TODO: Support variable bound. This is definitely possible.
              if (!innerLoop.hasConstantUpperBound()) {
                innerLoop.emitError(
                    "has non-constant upper bound, not supported");
                return;
              }

              // Create all components required by constructing if operation.
              ifExprs.push_back(
                  getAffineConstantExpr(innerLoop.getConstantUpperBound() - 1,
                                        module.getContext()) -
                  getAffineDimExpr(idx++, module.getContext()));
              ifEqFlags.push_back(true);
              ifOperands.push_back(innerLoop.getInductionVar());
            }
            auto ifCondition = IntegerSet::get(idx, 0, ifExprs, ifEqFlags);

            // Set builder insertion point and create AffineIf operation.
            builder.setInsertionPoint(innermostLoop.getBody()->getTerminator());
            auto ifOp = builder.create<mlir::AffineIfOp>(
                module.getLoc(), ifCondition, ifOperands,
                /*withElseRegion=*/false);

            // Move all operations in backOps into the innermost loop. Note
            // that if the operation has result, it will always be executed.
            // However, if the operation doesn't have result (e.g. AffineStore
            // operation), it will be putted into the generated AffineIf
            // operation and conditionally executed.
            for (auto opIt = backOps.rbegin(); opIt < backOps.rend(); ++opIt) {
              auto op = *opIt;
              if (op->getNumResults())
                op->moveBefore(ifOp);
              else
                op->moveBefore(ifOp.getThenBlock()->getTerminator());
            }
          }
        }
        loops.push_back(loop);
      });
    }
  }
}

std::unique_ptr<mlir::Pass> scalehls::createAffineLoopPerfectionPass() {
  return std::make_unique<AffineLoopPerfection>();
}
