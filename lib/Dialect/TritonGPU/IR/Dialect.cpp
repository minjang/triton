#include "triton/Dialect/Triton/IR/Dialect.h"

#include <numeric>

#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "triton/Analysis/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.cpp.inc"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::triton::gpu;

// Utility
namespace mlir {
namespace triton {

static Type getI1SameShapeFromTensorOrTensorPtr(Type type) {
  auto i1Type = IntegerType::get(type.getContext(), 1);
  if (auto tensorType = type.dyn_cast<RankedTensorType>()) {
    return RankedTensorType::get(tensorType.getShape(), i1Type,
                                 tensorType.getEncoding());
  } else if (auto ptrType = type.dyn_cast<triton::PointerType>()) {
    Type pointeeType = ptrType.getPointeeType();
    if (auto tensorType = pointeeType.dyn_cast<RankedTensorType>()) {
      return RankedTensorType::get(tensorType.getShape(), i1Type,
                                   tensorType.getEncoding());
    }
  }
  return Type();
}

namespace gpu {

// TODO: Inheritance of layout attributes
// so that all distributed layouts implement
// these utilities

unsigned getTotalElemsPerThread(Attribute layout, ArrayRef<int64_t> shape,
                                Type eltTy) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return blockedLayout.getTotalElemsPerThread(shape, eltTy);
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    return sliceLayout.getTotalElemsPerThread(shape, eltTy);
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    return mmaLayout.getTotalElemsPerThread(shape, eltTy);
  } else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>()) {
    return sharedLayout.getTotalElemsPerThread(shape, eltTy);
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    return dotLayout.getTotalElemsPerThread(shape, eltTy);
  } else {
    assert(0 && "getElemsPerThread not implemented");
    return 0;
  }
}

SmallVector<unsigned> getElemsPerThread(Attribute layout,
                                        ArrayRef<int64_t> shape, Type eltTy) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return blockedLayout.getElemsPerThread(shape, eltTy);
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    return sliceLayout.getElemsPerThread(shape, eltTy);
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    return mmaLayout.getElemsPerThread(shape, eltTy);
  } else {
    assert(0 && "getElemsPerThread not implemented");
    return SmallVector<unsigned>();
  }
}

SmallVector<unsigned> getElemsPerThread(Type type) {
  if (type.isIntOrIndexOrFloat() || type.isa<triton::PointerType>())
    return SmallVector<unsigned>(1, 1);
  auto tensorType = type.cast<RankedTensorType>();
  return getElemsPerThread(tensorType.getEncoding(), tensorType.getShape(),
                           tensorType.getElementType());
}

unsigned getTotalElemsPerThread(Type type) {
  if (type.isIntOrIndexOrFloat() || type.isa<triton::PointerType>())
    return 1;
  auto tensorType = type.cast<RankedTensorType>();
  return getTotalElemsPerThread(tensorType.getEncoding(), tensorType.getShape(),
                                tensorType.getElementType());
}

SmallVector<unsigned> getThreadsPerWarp(Attribute layout) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return SmallVector<unsigned>(blockedLayout.getThreadsPerWarp().begin(),
                                 blockedLayout.getThreadsPerWarp().end());
  }
  if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    if (mmaLayout.isVolta())
      return {4, 8};
    if (mmaLayout.isAmpere())
      return {8, 4};
    if (mmaLayout.isHopper())
      return {8, 4};
  }
  if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parent = sliceLayout.getParent();
    auto parentThreadsPerWarp = getThreadsPerWarp(parent);
    assert(parentThreadsPerWarp.size() == 2 &&
           "getThreadsPerWarp only implemented for 2D slice layout");
    SmallVector<unsigned> threadsPerWarp = parentThreadsPerWarp;
    threadsPerWarp.erase(threadsPerWarp.begin() + sliceLayout.getDim());
    for (unsigned i = 0; i < threadsPerWarp.size(); i++)
      threadsPerWarp[i] *= parentThreadsPerWarp[sliceLayout.getDim()];
    return threadsPerWarp;
  }
  assert(0 && "getThreadsPerWarp not implemented");
  return {};
}

SmallVector<unsigned>
getThreadsPerWarpWithUniqueData(Attribute layout,
                                ArrayRef<int64_t> tensorShape) {
  if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentLayout = sliceLayout.getParent();
    auto parentShape = sliceLayout.paddedShape(tensorShape);
    auto parentThreadsPerWarp =
        getThreadsPerWarpWithUniqueData(parentLayout, parentShape);
    SmallVector<unsigned> threadsPerWarp = parentThreadsPerWarp;
    threadsPerWarp.erase(threadsPerWarp.begin() + sliceLayout.getDim());
    return threadsPerWarp;
  }
  auto threadsPerWarp = getThreadsPerWarp(layout);
  assert(threadsPerWarp.size() == tensorShape.size() &&
         "layout and tensor shape must have the same rank");
  for (unsigned i = 0; i < threadsPerWarp.size(); i++) {
    threadsPerWarp[i] = std::min<unsigned>(threadsPerWarp[i], tensorShape[i]);
  }

  return threadsPerWarp;
}

SmallVector<unsigned> getWarpsPerCTA(Attribute layout) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return SmallVector<unsigned>(blockedLayout.getWarpsPerCTA().begin(),
                                 blockedLayout.getWarpsPerCTA().end());
  }
  if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    return SmallVector<unsigned>(mmaLayout.getWarpsPerCTA().begin(),
                                 mmaLayout.getWarpsPerCTA().end());
  }
  if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parent = sliceLayout.getParent();
    auto parentWarpsPerCTA = getWarpsPerCTA(parent);
    assert(parentWarpsPerCTA.size() == 2 &&
           "getWarpsPerCTA only implemented for 2D slice layout");
    SmallVector<unsigned> warpsPerCTA = parentWarpsPerCTA;
    warpsPerCTA.erase(warpsPerCTA.begin() + sliceLayout.getDim());
    for (unsigned i = 0; i < warpsPerCTA.size(); i++)
      warpsPerCTA[i] *= parentWarpsPerCTA[sliceLayout.getDim()];
    return warpsPerCTA;
  }
  assert(0 && "getWarpsPerCTA not implemented");
  return {};
}

SmallVector<unsigned>
getWarpsPerCTAWithUniqueData(Attribute layout, ArrayRef<int64_t> tensorShape) {
  if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentLayout = sliceLayout.getParent();
    auto parentShape = sliceLayout.paddedShape(tensorShape);
    auto parentWarpsPerCTA =
        getWarpsPerCTAWithUniqueData(parentLayout, parentShape);
    SmallVector<unsigned> warpsPerCTA = parentWarpsPerCTA;
    warpsPerCTA.erase(warpsPerCTA.begin() + sliceLayout.getDim());
    return warpsPerCTA;
  }
  auto warpsPerCTA = getWarpsPerCTA(layout);
  assert(warpsPerCTA.size() == tensorShape.size() &&
         "layout and tensor shape must have the same rank");
  for (unsigned i = 0; i < warpsPerCTA.size(); i++) {
    auto sizePerWarp =
        getSizePerThread(layout)[i] * getThreadsPerWarp(layout)[i];
    auto maxWarpsPerDim = ceil<unsigned>(tensorShape[i], sizePerWarp);
    warpsPerCTA[i] = std::min<unsigned>(warpsPerCTA[i], maxWarpsPerDim);
  }

  return warpsPerCTA;
}

SmallVector<unsigned> getSizePerThread(Attribute layout) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return SmallVector<unsigned>(blockedLayout.getSizePerThread().begin(),
                                 blockedLayout.getSizePerThread().end());
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto sizePerThread = getSizePerThread(sliceLayout.getParent());
    sizePerThread.erase(sizePerThread.begin() + sliceLayout.getDim());
    return sizePerThread;
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    if (mmaLayout.isAmpere()) {
      return {2, 2};
    } else if (mmaLayout.isVolta()) {
      return {1, 2};
    } else if (mmaLayout.isHopper()) {
      auto instrShape = mmaLayout.getInstrShape();
      // TODO(thomas): what are those magic numbers?
      return SmallVector<unsigned>{instrShape[0] * 4 / 32, instrShape[1] / 4};
    } else {
      llvm_unreachable("Unexpected mma version");
    }
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    auto parentLayout = dotLayout.getParent();
    assert(parentLayout && "DotOperandEncodingAttr must have a parent");
    if (auto parentMmaLayout = parentLayout.dyn_cast<MmaEncodingAttr>()) {
      assert(parentMmaLayout.isAmpere() &&
             "mmaLayout version = 1 is not implemented yet");
      auto opIdx = dotLayout.getOpIdx();
      if (opIdx == 0) {
        return {2, 4};
      } else if (opIdx == 1) {
        return {4, 1};
      } else {
        assert(0 && "DotOperandEncodingAttr opIdx must be 0 or 1");
        return {};
      }
    } else {
      assert(0 && "DotOperandEncodingAttr non-MmaEncodingAttr parent not "
                  "supported yet");
      return {};
    }
  } else {
    assert(0 && "getSizePerThread not implemented");
    return {};
  }
}

SmallVector<unsigned> getContigPerThread(Attribute layout) {
  if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    assert(mmaLayout.isVolta() || mmaLayout.isAmpere() || mmaLayout.isHopper());
    return {1, 2};
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentLayout = sliceLayout.getParent();
    return getContigPerThread(parentLayout);
  } else {
    return getSizePerThread(layout);
  }
}

SmallVector<unsigned> getUniqueContigPerThread(Attribute layout,
                                               ArrayRef<int64_t> shape) {
  // If slice layout, call recursively on parent layout, and drop
  // sliced dim
  if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentLayout = sliceLayout.getParent();
    auto parentShape = sliceLayout.paddedShape(shape);
    auto parentUniqueContigPerThread =
        getUniqueContigPerThread(parentLayout, parentShape);
    parentUniqueContigPerThread.erase(parentUniqueContigPerThread.begin() +
                                      sliceLayout.getDim());
    return parentUniqueContigPerThread;
  }
  // Base case
  auto rank = shape.size();
  SmallVector<unsigned> ret(rank);
  auto contigPerThread = getContigPerThread(layout);
  assert(contigPerThread.size() == rank && "Unexpected contigPerThread size");
  for (int d = 0; d < rank; ++d) {
    ret[d] = std::min<unsigned>(shape[d], contigPerThread[d]);
  }
  return ret;
}

SmallVector<unsigned> getThreadsPerCTA(Attribute layout) {
  SmallVector<unsigned> threads;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    for (int d = 0, n = blockedLayout.getOrder().size(); d < n; ++d)
      threads.push_back(blockedLayout.getThreadsPerWarp()[d] *
                        blockedLayout.getWarpsPerCTA()[d]);
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    if (mmaLayout.isAmpere()) {
      threads = {8 * mmaLayout.getWarpsPerCTA()[0],
                 4 * mmaLayout.getWarpsPerCTA()[1]};
    } else
      assert(0 && "Unimplemented usage of MmaEncodingAttr");
  } else {
    assert(0 && "Unimplemented usage of getThreadsPerCTA");
  }

  return threads;
}

SmallVector<unsigned> getShapePerCTATile(Attribute layout,
                                         ArrayRef<int64_t> tensorShape) {
  SmallVector<unsigned> shape;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    for (unsigned d = 0, n = blockedLayout.getOrder().size(); d < n; ++d)
      shape.push_back(blockedLayout.getSizePerThread()[d] *
                      blockedLayout.getThreadsPerWarp()[d] *
                      blockedLayout.getWarpsPerCTA()[d]);
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    shape = getShapePerCTATile(sliceLayout.getParent(), tensorShape);
    shape.erase(shape.begin() + sliceLayout.getDim());
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    if (mmaLayout.isAmpere())
      return {16 * mmaLayout.getWarpsPerCTA()[0],
              8 * mmaLayout.getWarpsPerCTA()[1]};
    if (mmaLayout.isVolta()) {
      assert(!tensorShape.empty() && "Volta needs the tensorShape");
      if (tensorShape.size() == 1) // must be SliceEncoding
        return {static_cast<unsigned>(tensorShape[0]),
                static_cast<unsigned>(tensorShape[0])};
      return {static_cast<unsigned>(tensorShape[0]),
              static_cast<unsigned>(tensorShape[1])};
    }
    if (mmaLayout.isHopper()) {
      auto instrShape = mmaLayout.getInstrShape();
      return {16 * mmaLayout.getWarpsPerCTA()[0],
              instrShape[1] * mmaLayout.getWarpsPerCTA()[1]};
    }
    assert(0 && "Unexpected MMA layout version found");
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    auto parentLayout = dotLayout.getParent();
    assert(parentLayout && "DotOperandEncodingAttr must have a parent");
    if (auto parentMmaLayout = parentLayout.dyn_cast<MmaEncodingAttr>()) {
      assert(parentMmaLayout.isAmpere() &&
             "mmaLayout version = 1 is not implemented yet");
      auto parentShapePerCTATile =
          getShapePerCTATile(parentLayout, tensorShape);
      auto opIdx = dotLayout.getOpIdx();
      if (opIdx == 0) {
        return {parentShapePerCTATile[0], 16};
      } else if (opIdx == 1) {
        return {16, parentShapePerCTATile[1]};
      } else {
        assert(0 && "DotOperandEncodingAttr opIdx must be 0 or 1");
      }
    } else {
      assert(0 && "DotOperandEncodingAttr non-MmaEncodingAttr parent not "
                  "supported yet");
    }
  } else {
    assert(0 && "Unimplemented usage of getShapePerCTATile");
  }
  return shape;
}

namespace {

/* Utility function used by getOrder and getCTAOrder of SliceEncodingAttr.
 * Erase dim and decrease all values larger than dim by 1.
 * Example:    order = [0, 2, 4, 3, 1], dim = 2
 *          resOrder = [0,    3, 2, 1]
 */
SmallVector<unsigned> eraseOrder(ArrayRef<unsigned> order, unsigned dim) {
  unsigned rank = order.size();
  assert(dim < rank && "Invalid dim to erase");
  SmallVector<unsigned> resOrder;
  for (unsigned i : order)
    if (i < dim)
      resOrder.push_back(i);
    else if (i > dim)
      resOrder.push_back(i - 1);
  return resOrder;
}

} // namespace

SmallVector<unsigned> getOrder(Attribute layout) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    return SmallVector<unsigned>(blockedLayout.getOrder().begin(),
                                 blockedLayout.getOrder().end());
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    return {1, 0};
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    return {1, 0};
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    SmallVector<unsigned> parentOrder = getOrder(sliceLayout.getParent());
    unsigned dim = sliceLayout.getDim();
    SmallVector<unsigned> order;
    for (unsigned d : parentOrder) {
      if (d == dim)
        continue;
      else if (d > dim)
        order.push_back(d - 1);
      else
        order.push_back(d);
    }
    return order;
  } else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>()) {
    return SmallVector<unsigned>(sharedLayout.getOrder().begin(),
                                 sharedLayout.getOrder().end());
  } else {
    assert(0 && "Unimplemented usage of getOrder");
  }
  return {};
};

CTALayoutAttr getCTALayout(Attribute layout) {
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>())
    return blockedLayout.getCTALayout();
  else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>())
    return CTALayoutAttr::get(layout.getContext(), getCTAsPerCGA(sliceLayout),
                              getCTASplitNum(sliceLayout),
                              getCTAOrder(sliceLayout));
  else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>())
    return mmaLayout.getCTALayout();
  else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>())
    return CTALayoutAttr::get(layout.getContext(), getCTAsPerCGA(dotLayout),
                              getCTASplitNum(dotLayout),
                              getCTAOrder(dotLayout));
  else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>())
    return sharedLayout.getCTALayout();
  else
    assert(0 && "Unimplemented usage of getCTALayout");
  return {};
}

SmallVector<unsigned> getCTAsPerCGA(Attribute layout) {
  ArrayRef<unsigned> ref;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>())
    ref = blockedLayout.getCTALayout().getCTAsPerCGA();
  else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentCTAsPerCGA = getCTAsPerCGA(sliceLayout.getParent());
    if (parentCTAsPerCGA[sliceLayout.getDim()] == 1) {
      parentCTAsPerCGA.erase(parentCTAsPerCGA.begin() + sliceLayout.getDim());
      return parentCTAsPerCGA;
    }
    /* For getCTAsPerCGA of a slice layout, we have two choices:
     * (1) Return CTAsPerCGA of its parent. This is not a perfect solution
     * because the rank of the returned CTAsPerCGA does not match the rank of
     * tensorShape.
     * (2) Get CTAsPerCGA of its parent and erase the sliced dim. This is not a
     * perfect solution because the product of the returned CTAsPerCGA might not
     * match numCTAs.
     * To avoid introducing inconsistencies to the shape and
     * layout system, the usage of directly getting CTAsPerCGA of a slice layout
     * in which the sliced dim is not 1 is banned. You should always consider
     * slice layout as a special case and use getCTAsPerCGA(layout.getParent())
     * in the branch where layout is an instance of SliceEncodingAttr. This is
     * inconvenient but safe.
     */
    assert(0 && "getCTAsPerCGA for SliceEncodingAttr is not well-defined");
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>())
    ref = mmaLayout.getCTALayout().getCTAsPerCGA();
  else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>())
    return getCTAsPerCGA(dotLayout.getParent());
  else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>())
    ref = sharedLayout.getCTALayout().getCTAsPerCGA();
  else
    assert(0 && "Unimplemented usage of getCTAsPerCGA");
  return SmallVector<unsigned>(ref.begin(), ref.end());
}

SmallVector<unsigned> getCTASplitNum(Attribute layout) {
  SmallVector<unsigned> res;

  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    res.assign(blockedLayout.getCTALayout().getCTASplitNum().begin(),
               blockedLayout.getCTALayout().getCTASplitNum().end());
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    res = getCTASplitNum(sliceLayout.getParent());
    res.erase(res.begin() + sliceLayout.getDim());
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    res.assign(mmaLayout.getCTALayout().getCTASplitNum().begin(),
               mmaLayout.getCTALayout().getCTASplitNum().end());
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    res = getCTASplitNum(dotLayout.getParent());
    assert(res.size() == 2 && "Invalid dotLayout");

    // Do not split CTA in K dimension
    dotLayout.getOpIdx() == 0 ? res[1] = 1 : res[0] = 1;
  } else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>()) {
    res.assign(sharedLayout.getCTALayout().getCTASplitNum().begin(),
               sharedLayout.getCTALayout().getCTASplitNum().end());
  } else {
    assert(false && "Unimplemented usage of getCTASplitNum");
  }

  return res;
}

SmallVector<unsigned> getCTAOrder(Attribute layout) {
  ArrayRef<unsigned> ref;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
    ref = blockedLayout.getCTALayout().getCTAOrder();
  } else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>()) {
    auto parentCTAOrder = getCTAOrder(sliceLayout.getParent());
    return eraseOrder(parentCTAOrder, sliceLayout.getDim());
  } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
    ref = mmaLayout.getCTALayout().getCTAOrder();
  } else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>()) {
    return getCTAOrder(dotLayout.getParent());
  } else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>()) {
    ref = sharedLayout.getCTALayout().getCTAOrder();
  } else {
    assert(0 && "Unimplemented usage of getCTAOrder");
  }
  return SmallVector<unsigned>(ref.begin(), ref.end());
}

SmallVector<int64_t> getShapePerCTA(ArrayRef<unsigned> CTASplitNum,
                                    ArrayRef<int64_t> shape) {
  unsigned rank = shape.size();
  SmallVector<int64_t> shapePerCTA(rank);
  for (unsigned i = 0; i < rank; ++i) {
    // This wrapping rule must be consistent with emitCTAOffsetForLayout
    unsigned splitNum = std::min<unsigned>(shape[i], CTASplitNum[i]);
    shapePerCTA[i] = shape[i] / splitNum;
  }
  return shapePerCTA;
}

SmallVector<int64_t> getShapePerCTA(Attribute layout, ArrayRef<int64_t> shape) {
  if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>()) {
    // Special logic for pipeline pass, where shape is 3D and CTALayout is 2D.
    // The first dim of shape is numStages. This is a work around, otherwise too
    // many places would have to be modified in pipeline pass. Maybe we need to
    // refactor this logic in the future.
    auto CTASplitNum = sharedLayout.getCTALayout().getCTASplitNum();
    if (shape.size() == CTASplitNum.size() + 1) {
      auto res = getShapePerCTA(CTASplitNum, shape.drop_front());
      res.insert(res.begin(), shape.front());
      return res;
    }
  }
  return getShapePerCTA(getCTASplitNum(layout), shape);
}

SmallVector<int64_t> getShapePerCTA(Type type) {
  auto tensorType = type.cast<RankedTensorType>();
  return getShapePerCTA(tensorType.getEncoding(), tensorType.getShape());
}

unsigned getNumWarpsPerCTA(Attribute layout) {
  ArrayRef<unsigned> warpsPerCTA;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>())
    warpsPerCTA = blockedLayout.getWarpsPerCTA();
  else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>())
    return getNumWarpsPerCTA(sliceLayout.getParent());
  else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>())
    warpsPerCTA = mmaLayout.getWarpsPerCTA();
  else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>())
    return getNumWarpsPerCTA(dotLayout.getParent());
  else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>())
    assert(0 && "Cannot get numWarps from SharedEncodingAttr");
  else
    assert(0 && "Unimplemented usage of getNumWarpsPerCTA");
  return product<unsigned>(warpsPerCTA);
}

unsigned getNumCTAs(Attribute layout) {
  ArrayRef<unsigned> CTAsPerCGA;
  if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>())
    CTAsPerCGA = blockedLayout.getCTALayout().getCTAsPerCGA();
  else if (auto sliceLayout = layout.dyn_cast<SliceEncodingAttr>())
    return getNumCTAs(sliceLayout.getParent());
  else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>())
    CTAsPerCGA = mmaLayout.getCTALayout().getCTAsPerCGA();
  else if (auto dotLayout = layout.dyn_cast<DotOperandEncodingAttr>())
    return getNumCTAs(dotLayout.getParent());
  else if (auto sharedLayout = layout.dyn_cast<SharedEncodingAttr>())
    CTAsPerCGA = sharedLayout.getCTALayout().getCTAsPerCGA();
  else
    assert(0 && "Unimplemented usage of getNumCTAs");
  return product<unsigned>(CTAsPerCGA);
}

bool isaDistributedLayout(Attribute layout) {
  return layout.isa<BlockedEncodingAttr>() || layout.isa<MmaEncodingAttr>() ||
         layout.isa<SliceEncodingAttr>();
}

bool isSharedEncoding(Value value) {
  auto type = value.getType();
  if (auto tensorType = type.dyn_cast<RankedTensorType>()) {
    auto encoding = tensorType.getEncoding();
    return encoding && encoding.isa<triton::gpu::SharedEncodingAttr>();
  }
  return false;
}

bool isExpensiveCat(CatOp cat, Attribute targetEncoding) {
  // If the new elements per thread is less than the old one, we will need to do
  // convert encoding that goes through shared memory anyway. So we consider it
  // as expensive.
  auto tensorTy = cat.getResult().getType().cast<RankedTensorType>();
  auto totalElemsPerThread = gpu::getTotalElemsPerThread(tensorTy);
  auto shape = tensorTy.getShape();
  auto elemTy = tensorTy.getElementType();
  auto newTotalElemsPerThread =
      gpu::getTotalElemsPerThread(targetEncoding, shape, elemTy);
  return newTotalElemsPerThread < totalElemsPerThread;
}

} // namespace gpu
} // namespace triton
} // namespace mlir

static LogicalResult parseIntAttrValue(AsmParser &parser, Attribute attr,
                                       unsigned &value, StringRef desc) {
  auto intAttr = attr.dyn_cast<IntegerAttr>();
  if (!intAttr) {
    parser.emitError(parser.getNameLoc(), "expected an integer type in ")
        << desc;
    return failure();
  }
  if (intAttr.getType().isSignedInteger()) {
    int64_t attrVal = intAttr.getSInt();
    if (attrVal < 0) {
      parser.emitError(parser.getNameLoc(),
                       "expected an unsigned integer value in ")
          << desc;
      return failure();
    }
    value = attrVal;
  } else if (intAttr.getType().isSignlessInteger()) {
    int64_t attrVal = intAttr.getInt();
    if (attrVal < 0) {
      parser.emitError(parser.getNameLoc(),
                       "expected an unsigned integer value in ")
          << desc;
      return failure();
    }
    value = attrVal;
  } else {
    value = intAttr.getUInt();
  }
  return success();
}

static LogicalResult parseBoolAttrValue(AsmParser &parser, Attribute attr,
                                        bool &value, StringRef desc) {
  auto boolAttr = attr.dyn_cast<BoolAttr>();
  if (!boolAttr) {
    parser.emitError(parser.getNameLoc(), "expected an bool type in ") << desc;
    return failure();
  }
  value = boolAttr.getValue();
  return success();
}

// parse an array of integers
static LogicalResult parseIntArrayAttr(AsmParser &parser,
                                       const NamedAttribute &attr,
                                       SmallVector<unsigned> &res,
                                       StringRef desc) {
  auto arrayAttr = attr.getValue().dyn_cast<ArrayAttr>();
  if (!arrayAttr) {
    parser.emitError(parser.getNameLoc(), "expected an array for ") << desc;
    return failure();
  }
  for (Attribute i : arrayAttr) {
    unsigned value;
    if (parseIntAttrValue(parser, i, value, desc).failed())
      return failure();
    res.push_back(value);
  }
  return success();
};

static LogicalResult parseUInt(AsmParser &parser, const NamedAttribute &attr,
                               unsigned &value, StringRef desc) {
  return parseIntAttrValue(parser, attr.getValue(), value, desc);
};

static LogicalResult parseBool(AsmParser &parser, const NamedAttribute &attr,
                               bool &value, StringRef desc) {
  return parseBoolAttrValue(parser, attr.getValue(), value, desc);
};

//===----------------------------------------------------------------------===//
// Attribute methods
//===----------------------------------------------------------------------===//
#define GET_ATTRDEF_CLASSES
#include "triton/Dialect/TritonGPU/IR/TritonGPUAttrDefs.cpp.inc"

SliceEncodingAttr BlockedEncodingAttr::squeeze(int axis) {
  return SliceEncodingAttr::get(getContext(), axis, *this);
}
SmallVector<unsigned>
BlockedEncodingAttr::getElemsPerThread(ArrayRef<int64_t> shape,
                                       Type eltTy) const {
  size_t rank = shape.size();
  auto sizePerThread = getSizePerThread();
  auto warpsPerCTA = getWarpsPerCTA();
  auto threadsPerWarp = getThreadsPerWarp();
  auto shapePerCTA = getShapePerCTA(*this, shape);
  assert(rank == sizePerThread.size() &&
         "unexpected rank in BlockedEncodingAttr::getElemsPerThread");
  SmallVector<unsigned> elemsPerThread(rank);
  for (size_t i = 0; i < rank; ++i) {
    unsigned t = sizePerThread[i] * threadsPerWarp[i] * warpsPerCTA[i];
    elemsPerThread[i] = ceil<unsigned>(shapePerCTA[i], t) * sizePerThread[i];
  }
  return elemsPerThread;
}
unsigned BlockedEncodingAttr::getTotalElemsPerThread(ArrayRef<int64_t> shape,
                                                     Type eltTy) const {
  return product<unsigned>(getElemsPerThread(shape, eltTy));
}

template <class T>
SmallVector<T> SliceEncodingAttr::paddedShape(ArrayRef<T> shape) const {
  size_t rank = shape.size();
  unsigned dim = getDim();
  SmallVector<T> retShape(rank + 1);
  for (unsigned d = 0; d < rank + 1; ++d) {
    if (d < dim)
      retShape[d] = shape[d];
    else if (d == dim)
      retShape[d] = 1;
    else
      retShape[d] = shape[d - 1];
  }
  return retShape;
}
template SmallVector<unsigned>
SliceEncodingAttr::paddedShape<unsigned>(ArrayRef<unsigned> shape) const;
template SmallVector<int64_t>
SliceEncodingAttr::paddedShape<int64_t>(ArrayRef<int64_t> shape) const;

SmallVector<unsigned>
SliceEncodingAttr::getElemsPerThread(ArrayRef<int64_t> shape,
                                     Type eltTy) const {
  auto parent = getParent();
  auto parentElemsPerThread =
      ::getElemsPerThread(parent, paddedShape(shape), eltTy);
  parentElemsPerThread.erase(parentElemsPerThread.begin() + getDim());
  return parentElemsPerThread;
}
unsigned SliceEncodingAttr::getTotalElemsPerThread(ArrayRef<int64_t> shape,
                                                   Type eltTy) const {
  return product<unsigned>(getElemsPerThread(shape, eltTy));
}

SmallVector<unsigned>
MmaEncodingAttr::getElemsPerThread(ArrayRef<int64_t> shape, Type eltTy) const {
  size_t rank = shape.size();
  assert(rank == 2 && "Unexpected rank of mma layout");
  assert((isVolta() || isAmpere() || isHopper()) &&
         "For MmaEncodingAttr only version 1~3 is supported");

  auto shapePerCTA = getShapePerCTA(getCTALayout().getCTASplitNum(), shape);

  SmallVector<unsigned> elemsPerThread(rank);
  if (isVolta()) {
    auto [isARow, isBRow, isAVec4, isBVec4, id] = decodeVoltaLayoutStates();
    static constexpr std::array<unsigned, 2> fpw{{2, 2}};
    unsigned packSize0 = (isARow || isAVec4) ? 1 : 2;
    unsigned packSize1 = (isBRow && !isBVec4) ? 2 : 1;
    unsigned repM = 2 * packSize0;
    unsigned repN = 2 * packSize1;
    unsigned spwM = fpw[0] * 4 * repM;
    unsigned spwN = fpw[1] * 4 * repN;
    unsigned wptM = getWarpsPerCTA()[0];
    unsigned wptN = getWarpsPerCTA()[1];
    unsigned resM = repM * std::max<int>(1, shapePerCTA[0] / (spwM * wptM));
    unsigned resN = 2 * repN * std::max<int>(1, shapePerCTA[1] / (spwN * wptN));
    elemsPerThread[0] = resM;
    elemsPerThread[1] = resN;
  } else if (isAmpere()) {
    unsigned elemsRow =
        ceil<unsigned>(shapePerCTA[0], 16 * getWarpsPerCTA()[0]) * 2;
    unsigned elemsCol =
        ceil<unsigned>(shapePerCTA[1], 8 * getWarpsPerCTA()[1]) * 2;
    elemsPerThread[0] = elemsRow;
    elemsPerThread[1] = elemsCol;
  } else if (isHopper()) {
    auto wpt = getWarpsPerCTA();
    auto instrMNK = getInstrShape();
    int repM = ceil<unsigned>(shapePerCTA[0], instrMNK[0] * wpt[0]);
    int repN = ceil<unsigned>(shapePerCTA[1], instrMNK[1] * wpt[1]);
    elemsPerThread[0] = 2 * repM;
    elemsPerThread[1] = (instrMNK[1] / 4) * repN;
  } else {
    llvm_unreachable("Unexpected mma version");
  }

  return elemsPerThread;
}

unsigned
MmaEncodingAttr::getElemsPerThreadOfOperand(int opIdx,
                                            ArrayRef<int64_t> shape) const {
  size_t rank = shape.size();
  assert(rank == 2 && "Unexpected rank of mma layout");
  auto shapePerCTA = getShapePerCTA(*this, shape);
  int res = 0;
  if (isVolta()) {
    llvm_unreachable(
        "getElemsPerThreadOfOperand() not supported for version 1");
  } else if (isAmpere()) {
    llvm_unreachable(
        "getElemsPerThreadOfOperand() not supported for version 2");
  } else if (isHopper()) {
    auto wpt = getWarpsPerCTA();
    auto instrMNK = getInstrShape();
    if (opIdx == 0) {
      int repM = ceil<unsigned>(shapePerCTA[0], instrMNK[0] * wpt[0]);
      int repK = ceil<unsigned>(shapePerCTA[1], instrMNK[2]);
      return 8 * repM * repK;

    } else if (opIdx == 1) {
      int repK = ceil<unsigned>(shapePerCTA[0], instrMNK[2]);
      int repN = ceil<unsigned>(shapePerCTA[1], instrMNK[1] * wpt[1]);
      // benzh@ here need more check
      return 4 * std::max<int>(instrMNK[1] / 32, 1) * repK * repN;
    }
  }
  return res;
}

unsigned MmaEncodingAttr::getTotalElemsPerThread(ArrayRef<int64_t> shape,
                                                 Type eltTy) const {
  return product<unsigned>(getElemsPerThread(shape, eltTy));
}

SmallVector<unsigned>
SharedEncodingAttr::getElemsPerThread(ArrayRef<int64_t> shape,
                                      Type eltTy) const {
  llvm_unreachable("getElemsPerThread is not supported for shared layout");
  return SmallVector<unsigned>();
}
unsigned SharedEncodingAttr::getTotalElemsPerThread(ArrayRef<int64_t> shape,
                                                    Type eltTy) const {
  llvm_unreachable("getElemsPerThread is not supported for shared layout");
  return 0;
}

SmallVector<int64_t>
DotOperandEncodingAttr::getMMAv2Rep(ArrayRef<int64_t> shape,
                                    int bitwidth) const {
  auto mmaParent = getParent().cast<MmaEncodingAttr>();
  SmallVector<int> shapePerWarp = {16, 8, 4 * 64 / bitwidth};
  auto warpsPerCTA = getParent().cast<MmaEncodingAttr>().getWarpsPerCTA();
  assert(mmaParent.isAmpere());
  if (getOpIdx() == 0)
    return {std::max<int64_t>(1, shape[0] / (shapePerWarp[0] * warpsPerCTA[0])),
            std::max<int64_t>(1, shape[1] / shapePerWarp[2])};
  else {
    assert(getOpIdx() == 1);
    return {
        std::max<int64_t>(1, shape[0] / shapePerWarp[2]),
        std::max<int64_t>(1, shape[1] / (shapePerWarp[1] * warpsPerCTA[1]))};
  }
}

SmallVector<unsigned>
DotOperandEncodingAttr::getElemsPerThread(ArrayRef<int64_t> shape,
                                          Type eltTy) const {
  llvm_unreachable("getElemsPerThread is not supported for dot operand");
  return SmallVector<unsigned>();
}

unsigned DotOperandEncodingAttr::getTotalElemsPerThread(ArrayRef<int64_t> shape,
                                                        Type eltTy) const {
  auto shapePerCTA = getShapePerCTA(*this, shape);
  if (auto mmaParent = getParent().dyn_cast<MmaEncodingAttr>()) {
    int warpsPerCTAM = mmaParent.getWarpsPerCTA()[0];
    int warpsPerCTAN = mmaParent.getWarpsPerCTA()[1];
    // H100
    if (mmaParent.isHopper()) {
      if (eltTy.isF16())
        return mmaParent.getTotalElemsPerThread(shape, eltTy);
    }
    // A100
    if (mmaParent.isAmpere()) {
      auto rep = getMMAv2Rep(shapePerCTA, eltTy.getIntOrFloatBitWidth());
      if (getOpIdx() == 0)
        return 4 * rep[0] * rep[1];
      if (getOpIdx() == 1)
        return 4 * rep[0] * std::max<int>(rep[1] / 2, 1);
    }
    // V100
    if (mmaParent.isVolta()) {
      bool isRow = getMMAv1IsRow();
      bool isVec4 = getMMAv1IsVec4();
      if (getOpIdx() == 0) {
        int packSizeM = (isRow || isVec4) ? 1 : 2;
        int repM = 2 * packSizeM;
        int spwM = 2 * 4 * repM;
        int numM = getMMAv1NumOuter(shape);
        int NK = shape[1];
        int vec = 2 * repM;
        // Here we mimic the logic in loadA, the result cannot be calculated
        // directly.
        llvm::DenseSet<std::pair<int, int>> visited;
        auto ld = [&](int m, int k) {
          visited.insert({m, k});
          if (vec > 4) {
            if (isRow)
              visited.insert({m, k + 4});
            else
              visited.insert({m + 1, k});
          }
        };
        for (unsigned k = 0; k < NK; k += 4)
          for (unsigned m = 0; m < numM / 2; ++m)
            if (!visited.count({m, k}))
              ld(m, k);
        return visited.size() * 2;
      }
      if (getOpIdx() == 1) {
        int packSizeN = (isRow && !isVec4) ? 2 : 1;
        int repN = 2 * packSizeN;
        int spwN = 2 * 4 * repN;
        int numN = getMMAv1NumOuter(shape);
        int vec = 2 * repN;

        int NK = shape[0];
        // Here we mimic the logic in loadA, the result cannot be calculated
        // directly.
        llvm::DenseSet<std::pair<int, int>> visited;
        int elemsPerLd = vec > 4 ? 4 : 2;
        auto ld = [&](int n, int k) {
          visited.insert({n, k});
          if (vec > 4) {
            if (isRow)
              visited.insert({n + 1, k});
            else
              visited.insert({n, k + 4});
          }
        };

        for (unsigned k = 0; k < NK; k += 4)
          for (unsigned n = 0; n < numN / 2; ++n) {
            if (!visited.count({n, k}))
              ld(n, k);
          }

        return visited.size() * 2;
      }
    }
  }
  if (auto blockedLayout = getParent().dyn_cast<BlockedEncodingAttr>()) {
    auto shapePerCTATile = getShapePerCTATile(blockedLayout);
    auto order = blockedLayout.getOrder();
    auto sizePerThread = getSizePerThread(blockedLayout);

    int K = getOpIdx() == 0 ? shapePerCTA[1] : shapePerCTA[0];
    int otherDim = getOpIdx() == 1 ? shapePerCTA[1] : shapePerCTA[0];

    bool isM = getOpIdx() == 0;

    int mSizePerThread =
        order[0] == 1 ? sizePerThread[order[1]] : sizePerThread[order[0]];
    int nSizePerThread =
        order[0] == 0 ? sizePerThread[order[1]] : sizePerThread[order[0]];
    int sizePerThreadMN = isM ? mSizePerThread : nSizePerThread;

    int mShapePerCTATile =
        order[0] == 1 ? shapePerCTATile[order[1]] : shapePerCTATile[order[0]];
    int nShapePerCTATile =
        order[0] == 0 ? shapePerCTATile[order[1]] : shapePerCTATile[order[0]];
    int shapePerCTAMNTile = isM ? mShapePerCTATile : nShapePerCTATile;

    return K * std::max<int>(otherDim / shapePerCTAMNTile, 1) * sizePerThreadMN;
  }
  llvm_unreachable("unknown dot operand parent layout");
  return 0;
}

//===----------------------------------------------------------------------===//
// Blocked Encoding
//===----------------------------------------------------------------------===//

Attribute BlockedEncodingAttr::parse(AsmParser &parser, Type type) {
  if (parser.parseLess().failed())
    return {};
  // Parse the data as a dictionary
  DictionaryAttr dict;
  if (parser.parseAttribute(dict).failed())
    return {};
  if (parser.parseGreater().failed())
    return {};

  SmallVector<unsigned> sizePerThread;
  SmallVector<unsigned> threadsPerWarp;
  SmallVector<unsigned> warpsPerCTA;
  SmallVector<unsigned> order;
  SmallVector<unsigned> CTAsPerCGA;
  SmallVector<unsigned> CTASplitNum;
  SmallVector<unsigned> CTAOrder;

  for (const NamedAttribute &attr : dict) {
    if (attr.getName() == "sizePerThread") {
      if (parseIntArrayAttr(parser, attr, sizePerThread,
                            "number of elements per thread")
              .failed())
        return {};
    } else if (attr.getName() == "threadsPerWarp") {
      if (parseIntArrayAttr(parser, attr, threadsPerWarp,
                            "number of threads per warp")
              .failed())
        return {};
    } else if (attr.getName() == "warpsPerCTA") {
      if (parseIntArrayAttr(parser, attr, warpsPerCTA,
                            "number of warps per CTA")
              .failed())
        return {};
    } else if (attr.getName() == "order") {
      if (parseIntArrayAttr(parser, attr, order, "order").failed())
        return {};
    } else if (attr.getName() == "CTAsPerCGA") {
      if (parseIntArrayAttr(parser, attr, CTAsPerCGA, "CTAsPerCGA").failed())
        return {};
    } else if (attr.getName() == "CTASplitNum") {
      if (parseIntArrayAttr(parser, attr, CTASplitNum, "CTASplitNum").failed())
        return {};
    } else if (attr.getName() == "CTAOrder") {
      if (parseIntArrayAttr(parser, attr, CTAOrder, "CTAOrder").failed())
        return {};
    } else {
      parser.emitError(parser.getNameLoc(), "unexpected key: ")
          << attr.getName().strref();
      return {};
    }
  }

  auto CTALayout = CTALayoutAttr::get(parser.getContext(), CTAsPerCGA,
                                      CTASplitNum, CTAOrder);

  return parser.getChecked<BlockedEncodingAttr>(parser.getContext(),
                                                sizePerThread, threadsPerWarp,
                                                warpsPerCTA, order, CTALayout);
}

void BlockedEncodingAttr::print(mlir::AsmPrinter &printer) const {
  printer << "<{"
          << "sizePerThread = [" << getSizePerThread() << "]"
          << ", threadsPerWarp = [" << getThreadsPerWarp() << "]"
          << ", warpsPerCTA = [" << getWarpsPerCTA() << "]"
          << ", order = [" << getOrder() << "]"
          << ", CTAsPerCGA = [" << getCTALayout().getCTAsPerCGA() << "]"
          << ", CTASplitNum = [" << getCTALayout().getCTASplitNum() << "]"
          << ", CTAOrder = [" << getCTALayout().getCTAOrder() << "]"
          << "}>";
}

//===----------------------------------------------------------------------===//
// MMA encoding
//===----------------------------------------------------------------------===//

Attribute MmaEncodingAttr::parse(AsmParser &parser, Type type) {
  if (parser.parseLess().failed())
    return {};
  DictionaryAttr dict;
  if (parser.parseAttribute(dict).failed())
    return {};
  if (parser.parseGreater().failed())
    return {};

  unsigned versionMajor = 0;
  unsigned versionMinor = 0;
  SmallVector<unsigned> warpsPerCTA;
  SmallVector<unsigned> CTAsPerCGA;
  SmallVector<unsigned> CTASplitNum;
  SmallVector<unsigned> CTAOrder;
  SmallVector<unsigned> instrShape;

  for (const NamedAttribute &attr : dict) {
    if (attr.getName() == "versionMajor") {
      if (parseUInt(parser, attr, versionMajor, "versionMajor").failed())
        return {};
    }
    if (attr.getName() == "versionMinor") {
      if (parseUInt(parser, attr, versionMinor, "versionMinor").failed())
        return {};
    }
    if (attr.getName() == "warpsPerCTA") {
      if (parseIntArrayAttr(parser, attr, warpsPerCTA, "warpsPerCTA").failed())
        return {};
    }
    if (attr.getName() == "CTAsPerCGA") {
      if (parseIntArrayAttr(parser, attr, CTAsPerCGA, "CTAsPerCGA").failed())
        return {};
    }
    if (attr.getName() == "CTASplitNum") {
      if (parseIntArrayAttr(parser, attr, CTASplitNum, "CTASplitNum").failed())
        return {};
    }
    if (attr.getName() == "CTAOrder") {
      if (parseIntArrayAttr(parser, attr, CTAOrder, "CTAOrder").failed())
        return {};
    }
    if (attr.getName() == "instrShape") {
      if (parseIntArrayAttr(parser, attr, instrShape, "instrShape").failed()) {
        return {};
      }
    }
  }

  auto CTALayout = CTALayoutAttr::get(parser.getContext(), CTAsPerCGA,
                                      CTASplitNum, CTAOrder);

  return parser.getChecked<MmaEncodingAttr>(parser.getContext(), versionMajor,
                                            versionMinor, warpsPerCTA,
                                            CTALayout, instrShape);
}

void MmaEncodingAttr::print(AsmPrinter &printer) const {
  printer << "<{"
          << "versionMajor = " << getVersionMajor() << ", "
          << "versionMinor = " << getVersionMinor() << ", "
          << "warpsPerCTA = [" << getWarpsPerCTA() << "], "
          << "CTAsPerCGA = [" << getCTALayout().getCTAsPerCGA() << "], "
          << "CTASplitNum = [" << getCTALayout().getCTASplitNum() << "], "
          << "CTAOrder = [" << getCTALayout().getCTAOrder() << "], "
          << "instrShape = [" << getInstrShape() << "]"
          << "}>";
}

//===----------------------------------------------------------------------===//
// Sliced Encoding
//===----------------------------------------------------------------------===//

Attribute SliceEncodingAttr::parse(AsmParser &parser, Type type) {
  if (parser.parseLess().failed())
    return {};
  NamedAttrList attrs;
  if (parser.parseOptionalAttrDict(attrs).failed())
    return {};
  if (parser.parseGreater().failed())
    return {};
  unsigned dim = attrs.get("dim").cast<IntegerAttr>().getInt();
  Attribute parent = attrs.get("parent");
  return parser.getChecked<SliceEncodingAttr>(parser.getContext(), dim, parent);
}

void SliceEncodingAttr::print(mlir::AsmPrinter &printer) const {
  printer << "<{"
          << "dim = " << getDim() << ", "
          << "parent = " << getParent() << "}>";
}

//===----------------------------------------------------------------------===//
// Shared encoding
//===----------------------------------------------------------------------===//

Attribute SharedEncodingAttr::parse(AsmParser &parser, Type type) {
  if (parser.parseLess().failed())
    return {};
  // Parse the data as a dictionary
  DictionaryAttr dict;
  if (parser.parseAttribute(dict).failed())
    return {};
  if (parser.parseGreater().failed())
    return {};

  unsigned vec = 0;
  unsigned perPhase = 0;
  unsigned maxPhase = 0;
  SmallVector<unsigned> order;
  SmallVector<unsigned> CTAsPerCGA;
  SmallVector<unsigned> CTASplitNum;
  SmallVector<unsigned> CTAOrder;
  bool hasLeadingOffset = false;

  for (const NamedAttribute &attr : dict) {
    if (attr.getName() == "vec") {
      if (parseUInt(parser, attr, vec, "vec").failed())
        return {};
    } else if (attr.getName() == "perPhase") {
      if (parseUInt(parser, attr, perPhase, "perPhase").failed())
        return {};
    } else if (attr.getName() == "maxPhase") {
      if (parseUInt(parser, attr, maxPhase, "maxPhase").failed())
        return {};
    } else if (attr.getName() == "order") {
      if (parseIntArrayAttr(parser, attr, order, "order").failed())
        return {};
    } else if (attr.getName() == "CTAsPerCGA") {
      if (parseIntArrayAttr(parser, attr, CTAsPerCGA, "CTAsPerCGA").failed())
        return {};
    } else if (attr.getName() == "CTASplitNum") {
      if (parseIntArrayAttr(parser, attr, CTASplitNum, "CTASplitNum").failed())
        return {};
    } else if (attr.getName() == "CTAOrder") {
      if (parseIntArrayAttr(parser, attr, CTAOrder, "CTAOrder").failed())
        return {};
    } else if (attr.getName() == "hasLeadingOffset") {
      if (parseBool(parser, attr, hasLeadingOffset, "hasLeadingOffset")
              .failed())
        return {};
    } else {
      parser.emitError(parser.getNameLoc(), "unexpected key: ")
          << attr.getName().strref();
      return {};
    }
  }

  auto CTALayout = CTALayoutAttr::get(parser.getContext(), CTAsPerCGA,
                                      CTASplitNum, CTAOrder);

  return parser.getChecked<SharedEncodingAttr>(parser.getContext(), vec,
                                               perPhase, maxPhase, order,
                                               CTALayout, hasLeadingOffset);
}

void SharedEncodingAttr::print(AsmPrinter &printer) const {
  printer << "<{"
          << "vec = " << getVec() << ", "
          << "perPhase = " << getPerPhase() << ", "
          << "maxPhase = " << getMaxPhase() << ", "
          << "order = [" << getOrder() << "], "
          << "CTAsPerCGA = [" << getCTALayout().getCTAsPerCGA() << "], "
          << "CTASplitNum = [" << getCTALayout().getCTASplitNum() << "], "
          << "CTAOrder = [" << getCTALayout().getCTAOrder() << "], "
          << "hasLeadingOffset = " << getHasLeadingOffset() << "}>";
}

//===----------------------------------------------------------------------===//
// Mma encoding
//===----------------------------------------------------------------------===//

bool MmaEncodingAttr::isVolta() const { return getVersionMajor() == 1; }

bool MmaEncodingAttr::isTuring() const {
  return getVersionMajor() == 2 && getVersionMinor() == 1;
}

bool MmaEncodingAttr::isAmpere() const { return getVersionMajor() == 2; }

bool MmaEncodingAttr::isHopper() const { return getVersionMajor() == 3; }

// Get [isARow, isBRow, isAVec4, isBVec4, id] from versionMinor
std::tuple<bool, bool, bool, bool, int>
MmaEncodingAttr::decodeVoltaLayoutStates() const {
  unsigned versionMinor = getVersionMinor();
  bool isARow = versionMinor & (1 << 0);
  bool isBRow = versionMinor & (1 << 1);
  bool isAVec4 = versionMinor & (1 << 2);
  bool isBVec4 = versionMinor & (1 << 3);

  int id = 0;
  for (int i = numBitsToHoldMmaV1ID - 1; i >= 0; --i)
    id = (id << 1) + static_cast<bool>(versionMinor & (1 << (4 + i)));

  return std::make_tuple(isARow, isBRow, isAVec4, isBVec4, id);
}

//===----------------------------------------------------------------------===//
// DotOperand Encoding
//===----------------------------------------------------------------------===//
Attribute DotOperandEncodingAttr::parse(AsmParser &parser, Type type) {
  if (parser.parseLess().failed())
    return {};
  NamedAttrList attrs;
  if (parser.parseOptionalAttrDict(attrs).failed())
    return {};
  if (parser.parseGreater().failed())
    return {};
  unsigned opIdx = attrs.get("opIdx").cast<IntegerAttr>().getInt();
  Attribute parent = attrs.get("parent");
  auto mmaParent = parent.dyn_cast<MmaEncodingAttr>();
  unsigned kWidth = 0;
  Attribute _kWidth = attrs.get("kWidth");
  if (_kWidth) {
    if (!mmaParent || mmaParent.isVolta()) {
      auto loc = parser.getNameLoc();
      parser.emitError(loc, "kWidth only supported for MMAv2+ parent");
      return Attribute();
    }
    kWidth = _kWidth.cast<IntegerAttr>().getInt();
  }
  return parser.getChecked<DotOperandEncodingAttr>(parser.getContext(), opIdx,
                                                   parent, kWidth);
}

void DotOperandEncodingAttr::print(mlir::AsmPrinter &printer) const {
  auto mmaParent = getParent().dyn_cast<MmaEncodingAttr>();
  printer << "<{"
          << "opIdx = " << getOpIdx() << ", parent = " << getParent();
  if (mmaParent && mmaParent.isAmpere())
    printer << ", kWidth = " << getKWidth();
  printer << "}>";
}

bool DotOperandEncodingAttr::getMMAv1IsRow() const {
  auto [isARow, isBRow, _0, _1, _2] =
      getParent().cast<MmaEncodingAttr>().decodeVoltaLayoutStates();
  return getOpIdx() == 0 ? isARow : isBRow;
}

bool DotOperandEncodingAttr::getMMAv1IsVec4() const {
  auto [_0, _1, isAVec4, isBVec4, _2] =
      getParent().cast<MmaEncodingAttr>().decodeVoltaLayoutStates();
  return getOpIdx() == 0 ? isAVec4 : isBVec4;
}

SmallVector<int> DotOperandEncodingAttr::getMMAv1Rep() const {
  auto [isARow, isBRow, isAVec4, isBVec4, _] =
      getParent().cast<MmaEncodingAttr>().decodeVoltaLayoutStates();
  // A
  if (getOpIdx() == 0) {
    int packSize = (isARow || isAVec4) ? 1 : 2;
    return {2 * packSize, 0, 1};
  }
  // B
  else {
    int packSize = (isBRow && !isBVec4) ? 2 : 1;
    return {0, 2 * packSize, 1};
  }
}

SmallVector<int> DotOperandEncodingAttr::getMMAv1ShapePerWarp() const {
  auto rep = getMMAv1Rep();
  if (getOpIdx() == 0) {
    return {8 * rep[0], 0, 1};
  } else {
    return {0, 8 * rep[1], 1};
  }
}

int DotOperandEncodingAttr::getMMAv1Vec() const {
  size_t opIdx = getOpIdx();
  return 2 * getMMAv1Rep()[opIdx];
}

int DotOperandEncodingAttr::getMMAv1NumOuter(ArrayRef<int64_t> shape) const {
  auto spw = getMMAv1ShapePerWarp();
  auto rep = getMMAv1Rep();
  auto warpsPerCTA = getParent().cast<MmaEncodingAttr>().getWarpsPerCTA();
  if (getOpIdx() == 0) {
    return rep[0] * shape[0] / (spw[0] * warpsPerCTA[0]);
  } else {
    return rep[1] * shape[1] / (spw[1] * warpsPerCTA[1]);
  }
}

//===----------------------------------------------------------------------===//
// InsertSliceOp / InsertSliceAsyncOp
//===----------------------------------------------------------------------===//

template <class OpT>
ParseResult parseInsertSliceOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand, 8> allOperands;
  Type srcType, dstType;
  SMLoc allOperandLoc = parser.getCurrentLocation();
  if (parser.parseOperandList(allOperands) ||
      parser.parseOptionalAttrDict(result.attributes) || parser.parseColon() ||
      parser.parseCustomTypeWithFallback(srcType) || parser.parseArrow() ||
      parser.parseCustomTypeWithFallback(dstType))
    return failure();
  result.addTypes(dstType);

  SmallVector<Type> operandTypes;
  operandTypes.push_back(srcType); // src
  operandTypes.push_back(dstType); // dst
  operandTypes.push_back(
      IntegerType::get(parser.getBuilder().getContext(), 32)); // index

  int hasMask = 0, hasOther = 0;
  if (allOperands.size() >= 4) {
    operandTypes.push_back(
        triton::getI1SameShapeFromTensorOrTensorPtr(srcType)); // mask
    hasMask = 1;
  }
  if (allOperands.size() >= 5) {
    operandTypes.push_back(triton::getPointeeType(srcType)); // other
    hasOther = 1;
  }

  if (parser.resolveOperands(allOperands, operandTypes, allOperandLoc,
                             result.operands))
    return failure();

  // Deduce operand_segment_sizes from the number of the operands.
  auto operandSegmentSizesAttrName =
      OpT::getOperandSegmentSizesAttrName(result.name);
  result.addAttribute(
      operandSegmentSizesAttrName,
      parser.getBuilder().getDenseI32ArrayAttr({1, 1, 1, hasMask, hasOther}));
  return success();
}

template <class OpT>
void printInsertSliceOp(OpAsmPrinter &printer, OpT insertSliceOp) {
  printer << " ";
  printer << insertSliceOp.getOperation()->getOperands();
  // "operand_segment_sizes" can be deduced, so we don't print it.
  printer.printOptionalAttrDict(
      insertSliceOp->getAttrs(),
      {insertSliceOp.getOperandSegmentSizesAttrName()});
  printer << " : ";
  printer.printStrippedAttrOrType(insertSliceOp.getSrc().getType());
  printer << " -> ";
  printer.printStrippedAttrOrType(insertSliceOp.getDst().getType());
}

ParseResult InsertSliceOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseInsertSliceOp<InsertSliceOp>(parser, result);
}

void InsertSliceOp::print(OpAsmPrinter &printer) {
  printInsertSliceOp<InsertSliceOp>(printer, *this);
}

ParseResult InsertSliceAsyncOp::parse(OpAsmParser &parser,
                                      OperationState &result) {
  return parseInsertSliceOp<InsertSliceAsyncOp>(parser, result);
}

void InsertSliceAsyncOp::print(OpAsmPrinter &printer) {
  printInsertSliceOp<InsertSliceAsyncOp>(printer, *this);
}

//===----------------------------------------------------------------------===//
// ASM Interface (i.e.: alias)
//===----------------------------------------------------------------------===//

class TritonGPUOpAsmInterface : public OpAsmDialectInterface {
public:
  using OpAsmDialectInterface::OpAsmDialectInterface;

  AliasResult getAlias(Attribute attr, raw_ostream &os) const override {
    if (auto mmaAttr = attr.dyn_cast<MmaEncodingAttr>()) {
      os << "mma";
      return AliasResult::FinalAlias;
    } else if (auto sharedAttr = attr.dyn_cast<SharedEncodingAttr>()) {
      os << "shared";
      return AliasResult::FinalAlias;
    } else if (auto blockedAttr = attr.dyn_cast<BlockedEncodingAttr>()) {
      os << "blocked";
      return AliasResult::FinalAlias;
    } /* else if (auto sliceAttr = attr.dyn_cast<SliceEncodingAttr>()) {
      os << "slice";
      return AliasResult::FinalAlias;
    } */
    return OpAsmDialectInterface::getAlias(attr, os);
  }
};

struct TritonGPUInferLayoutInterface
    : public triton::DialectInferLayoutInterface {
  using DialectInferLayoutInterface::DialectInferLayoutInterface;

  LogicalResult
  inferReduceOpEncoding(Attribute operandEncoding, unsigned axis,
                        Attribute &resultEncoding) const override {
    resultEncoding = SliceEncodingAttr::get(getDialect()->getContext(), axis,
                                            operandEncoding);
    return success();
  }

  LogicalResult inferTransOpEncoding(Attribute operandEncoding,
                                     Attribute &resultEncoding) const override {
    SharedEncodingAttr sharedEncoding =
        operandEncoding.dyn_cast<SharedEncodingAttr>();
    if (!sharedEncoding)
      return failure();
    SmallVector<unsigned> retOrder(sharedEncoding.getOrder().begin(),
                                   sharedEncoding.getOrder().end());
    std::reverse(retOrder.begin(), retOrder.end());
    // TODO(Qingyi): Need to check whether CTAOrder should also be reversed.
    // This is not a problem for tests where numCTAs = 1.
    resultEncoding = SharedEncodingAttr::get(
        getDialect()->getContext(), sharedEncoding.getVec(),
        sharedEncoding.getPerPhase(), sharedEncoding.getMaxPhase(), retOrder,
        sharedEncoding.getCTALayout(), sharedEncoding.getHasLeadingOffset());
    return mlir::success();
  }

  LogicalResult
  inferExpandDimsOpEncoding(Attribute operandEncoding, unsigned axis,
                            Attribute &resultEncoding,
                            std::optional<Location> location) const override {
    auto sliceEncoding = operandEncoding.dyn_cast<SliceEncodingAttr>();
    if (!sliceEncoding)
      return emitOptionalError(
          location, "ExpandDimsOp operand encoding must be SliceEncodingAttr");
    if (sliceEncoding.getDim() != axis)
      return emitOptionalError(
          location, "Incompatible slice dimension for ExpandDimsOp operand");
    resultEncoding = sliceEncoding.getParent();
    return success();
  }

  LogicalResult
  inferDotOpEncoding(Attribute operandEncoding, unsigned opIdx,
                     Attribute retEncoding,
                     std::optional<Location> location) const override {
    auto mmaRetEncoding = retEncoding.dyn_cast<MmaEncodingAttr>();
    if (mmaRetEncoding && mmaRetEncoding.isHopper()) {
      auto dotOpEnc = operandEncoding.dyn_cast<DotOperandEncodingAttr>();
      if (!operandEncoding.isa<SharedEncodingAttr>() &&
          !(opIdx == 0 && dotOpEnc && dotOpEnc.getOpIdx() == 0 &&
            dotOpEnc.getParent().isa<MmaEncodingAttr>())) {
        return emitOptionalError(
            location, "unexpected operand layout for MmaEncodingAttr v3");
      }
    } else if (auto dotOpEnc =
                   operandEncoding.dyn_cast<DotOperandEncodingAttr>()) {
      if (opIdx != dotOpEnc.getOpIdx())
        return emitOptionalError(location, "Wrong opIdx");
      if (retEncoding != dotOpEnc.getParent())
        return emitOptionalError(location, "Incompatible parent encoding");
    } else
      return emitOptionalError(
          location, "Dot's a/b's encoding should be of DotOperandEncodingAttr");
    return success();
  }

  LogicalResult
  verifyDotOpEncodingCompatibility(Operation *op, Attribute operandEncodingA,
                                   Attribute operandEncodingB) const override {
    auto aEncoding =
        operandEncodingA.dyn_cast<triton::gpu::DotOperandEncodingAttr>();
    auto bEncoding =
        operandEncodingB.dyn_cast<triton::gpu::DotOperandEncodingAttr>();
    if (!aEncoding && !bEncoding)
      return mlir::success();
    auto mmaAEncoding =
        aEncoding.getParent().dyn_cast_or_null<MmaEncodingAttr>();
    if (mmaAEncoding && mmaAEncoding.isHopper())
      return success();
    // Verify that the encodings are valid.
    if (!aEncoding || !bEncoding)
      return op->emitError("mismatching encoding between A and B operands");
    if (aEncoding.getKWidth() != bEncoding.getKWidth())
      return op->emitError("mismatching kWidth between A and B operands");
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Canonicalizer
//===----------------------------------------------------------------------===//

struct CanonicalizeConvertFromView
    : public mlir::OpRewritePattern<triton::ViewOp> {

  CanonicalizeConvertFromView(MLIRContext *context)
      : OpRewritePattern<triton::ViewOp>(context, 1) {}

  mlir::LogicalResult
  matchAndRewrite(triton::ViewOp op, PatternRewriter &rewriter) const override {
    Operation *arg = op->getOperand(0).getDefiningOp();
    if (!arg)
      return mlir::failure();
    // view(convert) -> view
    if (auto convert = dyn_cast<ConvertLayoutOp>(arg)) {
      rewriter.replaceOpWithNewOp<triton::ViewOp>(
          op, op->getResult(0).getType(), convert.getOperand());
      return mlir::success();
    }
    return mlir::failure();
  }
};

struct CanonicalizeConvertFromConvert
    : public mlir::OpRewritePattern<ConvertLayoutOp> {

  CanonicalizeConvertFromConvert(mlir::MLIRContext *context)
      : OpRewritePattern<ConvertLayoutOp>(context, 1) {}

  mlir::LogicalResult
  matchAndRewrite(ConvertLayoutOp op,
                  mlir::PatternRewriter &rewriter) const override {
    // we don't handle conversions to DotOperandEncodingAttr
    // this is a heuristics to accommodate fused attention
    auto srcType = op.getOperand().getType().cast<RankedTensorType>();
    auto dstType = op.getType().cast<RankedTensorType>();
    if (dstType.getEncoding().isa<triton::gpu::DotOperandEncodingAttr>() &&
        srcType.getEncoding().isa<triton::gpu::MmaEncodingAttr>())
      return mlir::failure();
    // for hopper MMAv3
    if (!op.use_empty()) {
      bool hasDotUser = false;
      for (Operation *dot : op.getResult().getUsers())
        if (isa<triton::DotOp>(dot))
          hasDotUser = true;

      if (hasDotUser) {
        if (dstType.getEncoding().isa<triton::gpu::SharedEncodingAttr>() &&
            srcType.getEncoding().isa<triton::gpu::MmaEncodingAttr>())
          return mlir::failure();
      }
    }

    // convert to the same layout -- we can delete
    if (op->getResultTypes() == op->getOperandTypes()) {
      rewriter.replaceOp(op, op->getOperands());
      return mlir::success();
    }
    Operation *arg = op->getOperand(0).getDefiningOp();
    // block argument
    if (!arg)
      return mlir::failure();
    // cvt(view) -> view
    if (auto view = dyn_cast<triton::ViewOp>(arg)) {
      rewriter.replaceOpWithNewOp<triton::ViewOp>(
          op, op->getResult(0).getType(), view.getResult());
      return mlir::success();
    }
    // cvt(cat) -> cat
    if (auto cat = dyn_cast<triton::CatOp>(arg)) {
      auto encoding =
          op->getResult(0).getType().cast<RankedTensorType>().getEncoding();
      if (isExpensiveCat(cat, encoding))
        return mlir::failure();
      rewriter.replaceOpWithNewOp<triton::CatOp>(op, op->getResult(0).getType(),
                                                 cat.getOperands());
      return mlir::success();
    }
    // cvt(alloc_tensor(x), type2) -> alloc_tensor(x, type2)
    auto alloc_tensor = dyn_cast<triton::gpu::AllocTensorOp>(arg);
    if (alloc_tensor) {
      if (!triton::gpu::isSharedEncoding(op->getResult(0))) {
        return mlir::failure();
      }
      rewriter.replaceOpWithNewOp<triton::gpu::AllocTensorOp>(
          op, op->getResult(0).getType());
      return mlir::success();
    }
    // cvt(insert_slice(x), type2) -> insert_slice(cvt(x, type2))
    auto insert_slice = dyn_cast<triton::gpu::InsertSliceAsyncOp>(arg);
    if (insert_slice) {
      if (!triton::gpu::isSharedEncoding(op->getResult(0))) {
        return mlir::failure();
      }
      auto newType = op->getResult(0).getType().cast<RankedTensorType>();
      // Ensure that the new insert_slice op is placed in the same place as
      // the old insert_slice op. Otherwise, the new insert_slice op may be
      // placed after the async_wait op, which is not allowed.
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(insert_slice);
      auto newArg = rewriter.create<triton::gpu::ConvertLayoutOp>(
          op->getLoc(), newType, insert_slice.getDst());
      rewriter.replaceOpWithNewOp<triton::gpu::InsertSliceAsyncOp>(
          op, newType, insert_slice.getSrc(), newArg.getResult(),
          insert_slice.getIndex(), insert_slice.getMask(),
          insert_slice.getOther(), insert_slice.getCache(),
          insert_slice.getEvict(), insert_slice.getIsVolatile(),
          insert_slice.getAxis());
      return mlir::success();
    }
    // cvt(extract_slice(x), type2) -> extract_slice(cvt(x, type2))
    auto extract_slice = dyn_cast<triton::gpu::ExtractSliceOp>(arg);
    if (extract_slice) {
      if (!triton::gpu::isSharedEncoding(op->getResult(0))) {
        return mlir::failure();
      }
      auto origType =
          extract_slice.getSource().getType().cast<RankedTensorType>();
      auto newType = RankedTensorType::get(
          origType.getShape(), origType.getElementType(),
          op->getResult(0).getType().cast<RankedTensorType>().getEncoding());
      auto origResType = op->getResult(0).getType().cast<RankedTensorType>();
      auto resType = RankedTensorType::get(
          origResType.getShape(), origResType.getElementType(),
          extract_slice.getType().cast<RankedTensorType>().getEncoding());
      // Ensure that the new extract_slice op is placed in the same place as
      // the old extract_slice op. Otherwise, the new extract_slice op may be
      // placed after the async_wait op, which is not allowed.
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPoint(extract_slice);
      auto newArg = rewriter.create<triton::gpu::ConvertLayoutOp>(
          op->getLoc(), newType, extract_slice.getSource());
      rewriter.replaceOpWithNewOp<triton::gpu::ExtractSliceOp>(
          op, resType, newArg.getResult(), extract_slice.getOffsets(),
          extract_slice.getSizes(), extract_slice.getStrides(),
          extract_slice.getStaticOffsets(), extract_slice.getStaticSizes(),
          extract_slice.getStaticStrides());
      return mlir::success();
    }

    // cvt(cvt(x, type1), type2) -> cvt(x, type2)
    if (llvm::isa<triton::gpu::ConvertLayoutOp>(arg)) {
      if (arg->getOperand(0).getDefiningOp() &&
          !triton::gpu::isSharedEncoding(arg->getOperand(0)) &&
          triton::gpu::isSharedEncoding(op.getOperand()) &&
          !triton::gpu::isSharedEncoding(op.getResult())) {
        return mlir::failure();
      }
      if (triton::gpu::isSharedEncoding(op.getOperand()) &&
          triton::gpu::isSharedEncoding(op.getResult())) {
        return mlir::failure();
      }
      auto srcType = op.getOperand().getType().cast<RankedTensorType>();
      auto srcShared =
          srcType.getEncoding().dyn_cast<triton::gpu::SharedEncodingAttr>();
      if (srcShared && srcShared.getVec() > 1)
        return mlir::failure();
      rewriter.replaceOpWithNewOp<triton::gpu::ConvertLayoutOp>(
          op, op->getResultTypes().front(), arg->getOperand(0));
      return mlir::success();
    }
    // cvt(type1, splat(type2, x)) -> splat(type1, x)
    if (auto splat = llvm::dyn_cast<triton::SplatOp>(arg)) {
      rewriter.replaceOpWithNewOp<triton::SplatOp>(op, op->getResultTypes(),
                                                   splat.getSrc());
      return mlir::success();
    }
    // cvt(type1, make_range(type2, x)) -> make_range(type1, x)
    if (auto range = llvm::dyn_cast<triton::MakeRangeOp>(arg)) {
      rewriter.replaceOpWithNewOp<triton::MakeRangeOp>(
          op, op->getResultTypes(), range.getStart(), range.getEnd());
      return mlir::success();
    }
    // cvt(type, constant) -> constant
    if (auto cst = llvm::dyn_cast<arith::ConstantOp>(arg))
      if (auto ret = cst.getValue().dyn_cast<SplatElementsAttr>()) {
        auto ty = op->getResultTypes().front().cast<ShapedType>();
        auto newRet =
            SplatElementsAttr::get(ty, ret.getSplatValue<Attribute>());
        rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, newRet);
        return mlir::success();
      }
    return mlir::failure();
  }
};

void ConvertLayoutOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                  MLIRContext *context) {
  patterns.add<CanonicalizeConvertFromConvert>(context);
  patterns.add<CanonicalizeConvertFromView>(context);
}

//===----------------------------------------------------------------------===//

/// Build an ExtractSliceOp with mixed static and dynamic entries and custom
/// result type. If the type passed is nullptr, it is inferred.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result,
                           RankedTensorType resultType, Value source,
                           ArrayRef<OpFoldResult> offsets,
                           ArrayRef<OpFoldResult> sizes,
                           ArrayRef<OpFoldResult> strides,
                           ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  auto sourceRankedTensorType = source.getType().cast<RankedTensorType>();
  build(b, result, resultType, source, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
  result.addAttributes(attrs);
}

//===----------------------------------------------------------------------===//

void TritonGPUDialect::initialize() {
  registerTypes();

  addAttributes<
#define GET_ATTRDEF_LIST
#include "triton/Dialect/TritonGPU/IR/TritonGPUAttrDefs.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "triton/Dialect/TritonGPU/IR/Ops.cpp.inc"
#include "triton/Dialect/TritonGPU/IR/OpsEnums.cpp.inc"
      >();
  addInterfaces<TritonGPUOpAsmInterface>();
  addInterfaces<TritonGPUInferLayoutInterface>();
}

#define GET_OP_CLASSES
#include "triton/Dialect/TritonGPU/IR/Ops.cpp.inc"

// verify TritonGPU ops
LogicalResult TritonGPUDialect::verifyOperationAttribute(Operation *op,
                                                         NamedAttribute attr) {
  // TODO: fill this.
  return success();
}
