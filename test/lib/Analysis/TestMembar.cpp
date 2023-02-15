#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/Dialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Conversion/SCFToStandard/SCFToStandard.h"
#include "triton/Analysis/Allocation.h"
#include "triton/Analysis/Membar.h"

using namespace mlir;

namespace {

struct TestMembarPass
    : public PassWrapper<TestMembarPass, OperationPass<FuncOp>> {

  // LLVM15+
  // MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TestMembarPass);

  StringRef getArgument() const final { return "test-print-membar"; }
  StringRef getDescription() const final {
    return "print the result of the allocation pass";
  }

  void runOnOperation() override {
    Operation *operation = getOperation();
    auto &os = llvm::errs();
    // Convert to std::string can remove quotes from op_name
    auto opName = SymbolTable::getSymbolName(operation).getValue().str();
    os << opName << "\n";
    // Lower the module to cf dialect
    auto *context = operation->getContext();
    RewritePatternSet scfPatterns(context);
    mlir::populateLoopToStdConversionPatterns(scfPatterns);
    mlir::ConversionTarget scfTarget(*context);
    scfTarget.addIllegalOp<scf::ForOp, scf::IfOp, scf::ParallelOp, scf::WhileOp,
                           scf::ExecuteRegionOp>();
    scfTarget.markUnknownOpDynamicallyLegal([](Operation *) { return true; });
    if (failed(applyPartialConversion(operation, scfTarget, std::move(scfPatterns))))
      return;

    Allocation allocation(operation);
    MembarAnalysis membarPass(&allocation);
    membarPass.run();

    //size_t operationId = 0;
    //operation->walk<WalkOrder::PreOrder>([&](Operation *op) {
    //  if (isa<gpu::BarrierOp>(op)) {
    //    os << "Membar " << operationId << "\n";
    //  }
    //  if (op->getNumRegions() == 0) {
    //    // Don't count parent Operation to simplify the test.
    //    operationId++;
    //  }
    //  return;
    //});
  }
};

} // namespace

namespace mlir {
namespace test {
void registerTestMembarPass() { PassRegistration<TestMembarPass>(); }
} // namespace test
} // namespace mlir
