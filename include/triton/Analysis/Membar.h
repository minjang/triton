#ifndef TRITON_ANALYSIS_MEMBAR_H
#define TRITON_ANALYSIS_MEMBAR_H

#include "Allocation.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace mlir {

class OpBuilder;

struct BlockInfo {
  using BufferIdSetT = Allocation::BufferIdSetT;

  BufferIdSetT syncReadBuffers;
  BufferIdSetT syncWriteBuffers;

  BlockInfo() = default;
  BlockInfo(const BufferIdSetT &syncReadBuffers,
            const BufferIdSetT &syncWriteBuffers)
      : syncReadBuffers(syncReadBuffers), syncWriteBuffers(syncWriteBuffers) {}

  /// Unions two BlockInfo objects.
  BlockInfo &join(const BlockInfo &other) {
    syncReadBuffers.insert(other.syncReadBuffers.begin(),
                           other.syncReadBuffers.end());
    syncWriteBuffers.insert(other.syncWriteBuffers.begin(),
                            other.syncWriteBuffers.end());
    return *this;
  }

  /// Returns true if buffers in two BlockInfo objects are intersected.
  bool isIntersected(const BlockInfo &other, Allocation *allocation) const {
    return /*RAW*/ isIntersected(syncWriteBuffers, other.syncReadBuffers,
                                 allocation) ||
           /*WAR*/
           isIntersected(syncReadBuffers, other.syncWriteBuffers, allocation) ||
           /*WAW*/
           isIntersected(syncWriteBuffers, other.syncWriteBuffers, allocation);
  }

  /// Clears the buffers because a barrier is inserted.
  void sync() {
    syncReadBuffers.clear();
    syncWriteBuffers.clear();
  }

  /// Compares two BlockInfo objects.
  bool operator==(const BlockInfo &other) const {
    return syncReadBuffers == other.syncReadBuffers &&
           syncWriteBuffers == other.syncWriteBuffers;
  }

  bool operator!=(const BlockInfo &other) const { return !(*this == other); }

private:
  /// Returns true if buffers in two sets are intersected.
  bool isIntersected(const BufferIdSetT &lhs, const BufferIdSetT &rhs,
                     Allocation *allocation) const {
    return std::any_of(lhs.begin(), lhs.end(), [&](auto lhsId) {
      return std::any_of(rhs.begin(), rhs.end(), [&](auto rhsId) {
        return allocation->isIntersected(lhsId, rhsId);
      });
    });
  }
};

//===----------------------------------------------------------------------===//
// Shared Memory Barrier Analysis
//===----------------------------------------------------------------------===//
class MembarAnalysis {
public:
  using FuncBlockInfoMapT = CallGraph<BlockInfo>::FuncDataMapT;
  /// Creates a new Membar analysis that generates the shared memory barrier
  /// in the following circumstances:
  /// - RAW: If a shared memory write is followed by a shared memory read, and
  /// their addresses are intersected, a barrier is inserted.
  /// - WAR: If a shared memory read is followed by a shared memory read, and
  /// their addresses are intersected, a barrier is inserted.
  /// The following circumstances do not require a barrier:
  /// - WAW: not possible because overlapped memory allocation is not allowed.
  /// - RAR: no write is performed.
  /// Temporary storage of operations such as Reduce are considered as both
  /// a shared memory read. If the temporary storage is written but not read,
  /// it is considered as the problem of the operation itself but not the membar
  /// analysis.
  /// The following circumstances are not considered yet:
  /// - Double buffers
  /// - N buffers
  MembarAnalysis() = default;
  MembarAnalysis(Allocation *allocation) : allocation(allocation) {}

  /// Runs the membar analysis to the given operation, inserts a barrier if
  /// necessary.
  void run(FuncBlockInfoMapT &funcBlockInfoMapT);

  BlockInfo &getBlockInfo() { return funcBlockInfo; }

private:
  /// Applies the barrier analysis based on the SCF dialect, in which each
  /// region has a single basic block only.
  /// Example:
  /// region1
  ///   op1
  ///   op2 (scf.if)
  ///      region2
  ///        op3
  ///        op4
  ///      region3
  ///        op5
  ///        op6
  ///   op7
  /// TODO: Explain why we don't use ForwardAnalysis:
  void resolve(FunctionOpInterface funcOp, FuncBlockInfoMapT *funcBlockInfoMap,
               OpBuilder *builder);

  /// Updates the BlockInfo operation based on the operation.
  void update(Operation *operation, BlockInfo *blockInfo,
              FuncBlockInfoMapT *funcBlockInfoMap, OpBuilder *builder);

  /// Collects the successors of the terminator
  void visitTerminator(Operation *operation, SmallVector<Block *> &successors);

private:
  Allocation *allocation = nullptr;
  BlockInfo funcBlockInfo;
};

/// Postorder traversal on the callgraph to insert membar instructions
/// of each function.
/// Each function maintains a BlockInfo map that includes all potential buffers
/// after returning. This way users do not have to explicitly insert membars
/// before and after function calls, but might be a bit conservative.
class ModuleMembarAnalysis : public CallGraph<BlockInfo> {
public:
  ModuleMembarAnalysis(ModuleAllocation *moduleAllocation)
      : CallGraph<BlockInfo>(moduleAllocation->getModuleOp()),
        moduleAllocation(moduleAllocation) {}

  void run() {
    walk<WalkOrder::PreOrder, WalkOrder::PostOrder>(
        // Pre-order walk callback
        [](CallOpInterface callOp, FunctionOpInterface funcOp) {},
        // Post-order walk callback
        [&](FunctionOpInterface funcOp,
            CallGraph<BlockInfo>::FuncDataMapT &funcBlockInfoMap) {
          auto *allocation = moduleAllocation->getFuncData(funcOp);
          auto [it, inserted] =
              funcBlockInfoMap.try_emplace(funcOp, BlockInfo());
          if (inserted) {
            MembarAnalysis analysis(allocation);
            analysis.run(funcBlockInfoMap);
          }
        });
  }

private:
  ModuleAllocation *moduleAllocation;
};

} // namespace mlir

#endif // TRITON_ANALYSIS_MEMBAR_H
