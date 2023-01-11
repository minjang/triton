#ifndef TRITON_CONVERSION_TRITONGPU_TO_LLVM_BASE_H
#define TRITON_CONVERSION_TRITONGPU_TO_LLVM_BASE_H

// TODO: refactor so that it doesn't fail if Allocation.h
// is included after utility.h (due to conflict in `store` macro
// and <atomic>
#include "triton/Analysis/Allocation.h"

//
#include "Utility.h"
#include "mlir/IR/TypeUtilities.h"
#include "triton/Analysis/AxisInfo.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::LLVM::SharedMemoryObject;
using ::mlir::triton::gpu::BlockedEncodingAttr;
using ::mlir::triton::gpu::MmaEncodingAttr;
using ::mlir::triton::gpu::SliceEncodingAttr;
// FuncOpConversion/FuncOpConversionBase is borrowed from
// https://github.com/llvm/llvm-project/blob/fae656b2dd80246c3c6f01e9c77c49560368752c/mlir/lib/Conversion/FuncToLLVM/FuncToLLVM.cpp#L276
// since it is not exposed on header files in mlir v14
// TODO(Superjomn): remove the code when MLIR v15.0 is included.
// All the rights are reserved by the LLVM community.

struct FuncOpConversionBase : public ConvertOpToLLVMPattern<FuncOp> {
private:
  /// Only retain those attributes that are not constructed by
  /// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out argument
  /// attributes.
  static void filterFuncAttributes(ArrayRef<NamedAttribute> attrs,
                                   bool filterArgAttrs,
                                   SmallVectorImpl<NamedAttribute> &result) {
    for (const auto &attr : attrs) {
      if (attr.getName() == SymbolTable::getSymbolAttrName() ||
          attr.getName() == FunctionOpInterface::getTypeAttrName() ||
          attr.getName() == "std.varargs" ||
          (filterArgAttrs &&
           attr.getName() == FunctionOpInterface::getArgDictAttrName()))
        continue;
      result.push_back(attr);
    }
  }

  /// Helper function for wrapping all attributes into a single DictionaryAttr
  static auto wrapAsStructAttrs(OpBuilder &b, ArrayAttr attrs) {
    return DictionaryAttr::get(b.getContext(),
                               b.getNamedAttr("llvm.struct_attrs", attrs));
  }

protected:
  using ConvertOpToLLVMPattern<FuncOp>::ConvertOpToLLVMPattern;

  // Convert input FuncOp to LLVMFuncOp by using the LLVMTypeConverter provided
  // to this legalization pattern.
  LLVM::LLVMFuncOp
  convertFuncOpToLLVMFuncOp(FuncOp funcOp,
                            ConversionPatternRewriter &rewriter) const {
    // Convert the original function arguments. They are converted using the
    // LLVMTypeConverter provided to this legalization pattern.
    auto varargsAttr = funcOp->getAttrOfType<BoolAttr>("func.varargs");
    TypeConverter::SignatureConversion result(funcOp.getNumArguments());
    auto llvmType = getTypeConverter()->convertFunctionSignature(
        funcOp.getType(), varargsAttr && varargsAttr.getValue(), result);
    if (!llvmType)
      return nullptr;

    // Propagate argument/result attributes to all converted arguments/result
    // obtained after converting a given original argument/result.
    SmallVector<NamedAttribute, 4> attributes;
    filterFuncAttributes(funcOp->getAttrs(), /*filterArgAttrs=*/true,
                         attributes);
    if (ArrayAttr resAttrDicts = funcOp.getAllResultAttrs()) {
      assert(!resAttrDicts.empty() && "expected array to be non-empty");
      auto newResAttrDicts =
          (funcOp.getNumResults() == 1)
              ? resAttrDicts
              : rewriter.getArrayAttr(
                    {wrapAsStructAttrs(rewriter, resAttrDicts)});
      attributes.push_back(rewriter.getNamedAttr(
          FunctionOpInterface::getResultDictAttrName(), newResAttrDicts));
    }
    if (ArrayAttr argAttrDicts = funcOp.getAllArgAttrs()) {
      SmallVector<Attribute, 4> newArgAttrs(
          llvmType.cast<LLVM::LLVMFunctionType>().getNumParams());
      for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i) {
        auto mapping = result.getInputMapping(i);
        assert(mapping && "unexpected deletion of function argument");
        for (size_t j = 0; j < mapping->size; ++j)
          newArgAttrs[mapping->inputNo + j] = argAttrDicts[i];
      }
      attributes.push_back(
          rewriter.getNamedAttr(FunctionOpInterface::getArgDictAttrName(),
                                rewriter.getArrayAttr(newArgAttrs)));
    }
    for (const auto &pair : llvm::enumerate(attributes)) {
      if (pair.value().getName() == "llvm.linkage") {
        attributes.erase(attributes.begin() + pair.index());
        break;
      }
    }

    // Create an LLVM function, use external linkage by default until MLIR
    // functions have linkage.
    LLVM::Linkage linkage = LLVM::Linkage::External;
    if (funcOp->hasAttr("llvm.linkage")) {
      auto attr =
          funcOp->getAttr("llvm.linkage").dyn_cast<mlir::LLVM::LinkageAttr>();
      if (!attr) {
        funcOp->emitError()
            << "Contains llvm.linkage attribute not of type LLVM::LinkageAttr";
        return nullptr;
      }
      linkage = attr.getLinkage();
    }
    auto newFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
        funcOp.getLoc(), funcOp.getName(), llvmType, linkage,
        /*dsoLocal*/ false, attributes);
    rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());
    if (failed(rewriter.convertRegionTypes(&newFuncOp.getBody(), *typeConverter,
                                           &result)))
      return nullptr;

    return newFuncOp;
  }
};

using IndexCacheKeyT = std::pair<Attribute, SmallVector<int64_t>>;

struct CacheKeyDenseMapInfo {
  static IndexCacheKeyT getEmptyKey() {
    auto *pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return std::make_pair(
        mlir::Attribute(static_cast<mlir::Attribute::ImplType *>(pointer)),
        SmallVector<int64_t>{});
  }
  static IndexCacheKeyT getTombstoneKey() {
    auto *pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return std::make_pair(
        mlir::Attribute(static_cast<mlir::Attribute::ImplType *>(pointer)),
        SmallVector<int64_t>{std::numeric_limits<int64_t>::max()});
  }
  static unsigned getHashValue(IndexCacheKeyT key) {
    return llvm::hash_combine(
        mlir::hash_value(key.first),
        llvm::hash_combine_range(key.second.begin(), key.second.end()));
  }
  static bool isEqual(IndexCacheKeyT LHS, IndexCacheKeyT RHS) {
    return LHS == RHS;
  }
};

class ConvertTritonGPUOpToLLVMPatternBase {
public:
  // Two levels of value cache in emitting indices calculation:
  // Key: pair<layout, shape>
  struct IndexCacheInfo {
    DenseMap<IndexCacheKeyT, SmallVector<Value>, CacheKeyDenseMapInfo>
        *baseIndexCache;
    DenseMap<IndexCacheKeyT, SmallVector<SmallVector<Value>>,
             CacheKeyDenseMapInfo> *indexCache;
    OpBuilder::InsertPoint *indexInsertPoint;
  };

  explicit ConvertTritonGPUOpToLLVMPatternBase(LLVMTypeConverter &typeConverter)
      : converter(&typeConverter) {}

  explicit ConvertTritonGPUOpToLLVMPatternBase(LLVMTypeConverter &typeConverter,
                                               const Allocation *allocation,
                                               Value smem)
      : converter(&typeConverter), allocation(allocation), smem(smem) {}

  explicit ConvertTritonGPUOpToLLVMPatternBase(LLVMTypeConverter &typeConverter,
                                               const Allocation *allocation,
                                               Value smem,
                                               IndexCacheInfo indexCacheInfo)
      : converter(&typeConverter), indexCacheInfo(indexCacheInfo),
        allocation(allocation), smem(smem) {}

  LLVMTypeConverter *getTypeConverter() const { return converter; }

  static Value
  getStructFromSharedMemoryObject(Location loc,
                                  const SharedMemoryObject &smemObj,
                                  ConversionPatternRewriter &rewriter) {
    auto elems = smemObj.getElems();
    auto types = smemObj.getTypes();
    auto structTy =
        LLVM::LLVMStructType::getLiteral(rewriter.getContext(), types);
    return getStructFromElements(loc, elems, rewriter, structTy);
  }

  Value getThreadId(ConversionPatternRewriter &rewriter, Location loc) const {
    auto llvmIndexTy = this->getTypeConverter()->getIndexType();
    auto cast = rewriter.create<UnrealizedConversionCastOp>(
        loc, TypeRange{llvmIndexTy},
        ValueRange{rewriter.create<::mlir::gpu::ThreadIdOp>(
            loc, rewriter.getIndexType(), ::mlir::gpu::Dimension::x)});
    Value threadId = cast.getResult(0);
    return threadId;
  }

  // -----------------------------------------------------------------------
  // Shared memory utilities
  // -----------------------------------------------------------------------
  template <typename T>
  Value getSharedMemoryBase(Location loc, ConversionPatternRewriter &rewriter,
                            T value) const {

    auto ptrTy = LLVM::LLVMPointerType::get(
        this->getTypeConverter()->convertType(rewriter.getI8Type()), 3);
    auto bufferId = allocation->getBufferId(value);
    assert(bufferId != Allocation::InvalidBufferId && "BufferId not found");
    size_t offset = allocation->getOffset(bufferId);
    Value offVal = idx_val(offset);
    Value base = gep(ptrTy, smem, offVal);
    return base;
  }

  DenseMap<unsigned, Value> getSwizzledSharedPtrs(Location loc, unsigned inVec, RankedTensorType srcTy,
                                         triton::gpu::SharedEncodingAttr resSharedLayout,
                                         Type resElemTy,
                                         SharedMemoryObject smemObj,
                                         ConversionPatternRewriter &rewriter,
                                         SmallVectorImpl<Value>& offsetVals,
                                         SmallVectorImpl<Value>& srcStrides
                                         ) const {

    auto dstPtrTy = ptr_ty(resElemTy, 3);
    auto dstOffset = dot(rewriter, loc, offsetVals, smemObj.strides);
    Value dstPtrBase = gep(dstPtrTy, smemObj.base, dstOffset);

    auto srcEncoding = srcTy.getEncoding();
    auto srcShape = srcTy.getShape();
    unsigned outVec = resSharedLayout.getVec();
    unsigned minVec = std::min(outVec, inVec);
    unsigned numElems = triton::gpu::getElemsPerThread(srcTy);
    unsigned perPhase = resSharedLayout.getPerPhase();
    unsigned maxPhase = resSharedLayout.getMaxPhase();
    auto sizePerThread = triton::gpu::getSizePerThread(srcEncoding);
    auto threadsPerCTA = triton::gpu::getThreadsPerCTA(srcEncoding);
    auto inOrder = triton::gpu::getOrder(srcEncoding);

    // If perPhase * maxPhase > threadsPerCTA, we will have elements
    // that share the same tile indices. The index calculation will
    // be cached.
    unsigned numSwizzleRows = std::max<unsigned>(
        (perPhase * maxPhase) / threadsPerCTA[inOrder[1]], 1);
    // A sharedLayout encoding has a "vec" parameter.
    // On the column dimension, if inVec > outVec, it means we have to divide
    // single vector read into multiple ones
    unsigned numVecCols = std::max<unsigned>(inVec / outVec, 1);

    auto srcIndices = emitIndices(loc, rewriter, srcEncoding, srcShape);


    DenseMap<unsigned, Value> ret;
    DenseMap<std::pair<unsigned, unsigned>, Value> tileOffsetMap;
    for (unsigned elemIdx = 0; elemIdx < numElems; elemIdx += minVec) {
      // minVec = 2, inVec = 4, outVec = 2
      //   baseOffsetCol = 0   baseOffsetCol = 0
      //   tileVecIdxCol = 0   tileVecIdxCol = 1
      //                -/\-   -/\-
      //               [|x x| |x x| x x x x x]
      //               [|x x| |x x| x x x x x]
      // baseOffsetRow [|x x| |x x| x x x x x]
      //               [|x x| |x x| x x x x x]
      unsigned vecIdx = elemIdx / minVec;
      unsigned vecIdxCol = vecIdx % (sizePerThread[inOrder[0]] / minVec);
      unsigned vecIdxRow = vecIdx / (sizePerThread[inOrder[0]] / minVec);
      unsigned baseOffsetCol =
          vecIdxCol / numVecCols * numVecCols * threadsPerCTA[inOrder[0]];
      unsigned baseOffsetRow = vecIdxRow / numSwizzleRows * numSwizzleRows *
                           threadsPerCTA[inOrder[1]];
      unsigned tileVecIdxCol = vecIdxCol % numVecCols;
      unsigned tileVecIdxRow = vecIdxRow % numSwizzleRows;

      if (!tileOffsetMap.count({tileVecIdxRow, tileVecIdxCol})) {
        // Swizzling
        // Since the swizzling index is related to outVec, and we know minVec
        // already, inVec doesn't matter
        //
        // (Numbers represent row indices)
        // Example1:
        // outVec = 2, inVec = 2, minVec = 2
        // outVec = 2, inVec = 4, minVec = 2
        //     | [1 2] [3 4] [5 6] ... |
        //     | [3 4] [1 2] [7 8] ... |
        //     | [5 6] [7 8] [1 2] ... |
        // Example2:
        // outVec = 4, inVec = 2, minVec = 2
        //     | [1 2 3 4] [5 6 7 8] [9 10 11 12] ... |
        //     | [5 6 7 8] [1 2 3 4] [13 14 15 16] ... |
        //     | [9 10 11 12] [13 14 15 16] [1 2 3 4] ... |
        auto srcIdx = srcIndices[tileVecIdxRow * sizePerThread[inOrder[0]]];
        Value phase = urem(udiv(srcIdx[inOrder[1]], i32_val(perPhase)),
                           i32_val(maxPhase));
        // srcShape and smemObj.shape maybe different if smemObj is a
        // slice of the original shared memory object.
        // So we need to use the original shape to compute the offset
        Value rowOffset = mul(srcIdx[inOrder[1]], srcStrides[inOrder[1]]);
        Value colOffset =
            add(srcIdx[inOrder[0]], i32_val(tileVecIdxCol * minVec));
        Value swizzleIdx = udiv(colOffset, i32_val(outVec));
        Value swizzleColOffset =
            add(mul(xor_(swizzleIdx, phase), i32_val(outVec)),
                urem(colOffset, i32_val(outVec)));
        Value tileOffset = add(rowOffset, swizzleColOffset);
        tileOffsetMap[{tileVecIdxRow, tileVecIdxCol}] =
            gep(dstPtrTy, dstPtrBase, tileOffset);
      }

      // 16 * 8 = 128bits
      auto maxBitWidth =
          std::max<unsigned>(128, resElemTy.getIntOrFloatBitWidth());
      auto vecBitWidth = resElemTy.getIntOrFloatBitWidth() * minVec;
      auto bitWidth = std::min<unsigned>(maxBitWidth, vecBitWidth);
      auto numWords = vecBitWidth / bitWidth;
      auto numWordElems = bitWidth / resElemTy.getIntOrFloatBitWidth();

      Value tileOffset = tileOffsetMap[{tileVecIdxRow, tileVecIdxCol}];
      Value baseOffset =
          add(mul(i32_val(baseOffsetRow), srcStrides[inOrder[1]]),
              i32_val(baseOffsetCol));
      Value basePtr = gep(dstPtrTy, tileOffset, baseOffset);
      ret[elemIdx] = basePtr;
    }
    return ret;
  }

  bool isMmaToDotShortcut(MmaEncodingAttr &mmaLayout,
                        triton::gpu::DotOperandEncodingAttr &dotOperandLayout) const {
    // dot_op<opIdx=0, parent=#mma> = #mma
    // when #mma = MmaEncoding<version=2, warpsPerCTA=[..., 1]>
    return mmaLayout.getWarpsPerCTA()[1] == 1 &&
           dotOperandLayout.getOpIdx() == 0 &&
           dotOperandLayout.getParent() == mmaLayout;
  }

  void storeDistributedToShared(Value src, Value llSrc,
                              ArrayRef<Value> dstStrides,
                              ArrayRef<SmallVector<Value>> srcIndices,
                              Value dst, Value smemBase, Type elemTy,
                              Location loc,
                              ConversionPatternRewriter &rewriter) const {
    auto srcTy = src.getType().cast<RankedTensorType>();
    auto srcShape = srcTy.getShape();
    assert(srcShape.size() == 2 && "Unexpected rank of storeDistributedToShared");
    auto dstTy = dst.getType().cast<RankedTensorType>();
    auto srcDistributedLayout = srcTy.getEncoding();
    if (auto mmaLayout = srcDistributedLayout.dyn_cast<MmaEncodingAttr>()) {
      assert((!mmaLayout.isVolta()) &&
             "ConvertLayout MMAv1->Shared is not suppported yet");
    }
    auto dstSharedLayout = dstTy.getEncoding().cast<triton::gpu::SharedEncodingAttr>();
    auto dstElemTy = dstTy.getElementType();
    auto inOrd = triton::gpu::getOrder(srcDistributedLayout);
    auto outOrd = dstSharedLayout.getOrder();
    unsigned inVec =
        inOrd == outOrd ? triton::gpu::getContigPerThread(srcDistributedLayout)[inOrd[0]] : 1;
    unsigned outVec = dstSharedLayout.getVec();
    unsigned minVec = std::min(outVec, inVec);
    unsigned perPhase = dstSharedLayout.getPerPhase();
    unsigned maxPhase = dstSharedLayout.getMaxPhase();
    unsigned numElems = triton::gpu::getElemsPerThread(srcTy);
    assert(numElems == srcIndices.size());
    auto inVals = LLVM::getElementsFromStruct(loc, llSrc, rewriter);
    auto wordTy = vec_ty(elemTy, minVec);
    auto elemPtrTy = ptr_ty(elemTy);
    Value outVecVal = i32_val(outVec);
    Value minVecVal = i32_val(minVec);
    Value word;

    SmallVector<Value> srcStrides = {dstStrides[0], dstStrides[1]};
    SmallVector<Value> offsetVals = {i32_val(0), i32_val(0)};
    SharedMemoryObject smemObj(smemBase, srcStrides, offsetVals);

    DenseMap<unsigned, Value> sharedPtrs = getSwizzledSharedPtrs(loc, inVec, srcTy, dstSharedLayout, dstElemTy, smemObj, rewriter, offsetVals, srcStrides);

    std::map<unsigned, Value> cache0;
    std::map<unsigned, Value> cache1;
    for (unsigned i = 0; i < numElems; ++i) {
      if (i % minVec == 0)
        word = undef(wordTy);
      word = insert_element(wordTy, word, inVals[i], i32_val(i % minVec));
      if (i % minVec == minVec - 1) {
        // step 1: recover the multidim_index from the index of
        SmallVector<Value> multiDimIdx = srcIndices[i];
        Value dynIdx0 = multiDimIdx[outOrd[0]];
        Value staIdx0 = i32_val(0);
        Value dynIdx1 = multiDimIdx[outOrd[1]];
        Value staIdx1 = i32_val(0);
        Value stride0 = dstStrides[outOrd[0]];
        Value stride1 = dstStrides[outOrd[1]];
        // if (auto addOp = dyn_cast<LLVM::AddOp>(dynIdx0.getDefiningOp()))
        //   if (auto cstRhs =
        //           dyn_cast<LLVM::ConstantOp>(addOp.getRhs().getDefiningOp())) {
        //     unsigned rhsVal =
        //         cstRhs.getValue().cast<IntegerAttr>().getValue().getSExtValue();
        //     unsigned key = (rhsVal / outVec) % maxPhase;
        //     if (cache0.find(key) == cache0.end())
        //       cache0[key] = dynIdx0;
        //     dynIdx0 = cache0[key];
        //     staIdx0 =
        //         i32_val((rhsVal) / (outVec * maxPhase) * (outVec * maxPhase));
        //   }
        // if (auto addOp = dyn_cast<LLVM::AddOp>(dynIdx1.getDefiningOp()))
        //   if (auto cstRhs =
        //           dyn_cast<LLVM::ConstantOp>(addOp.getRhs().getDefiningOp())) {
        //     unsigned rhsVal =
        //         cstRhs.getValue().cast<IntegerAttr>().getValue().getSExtValue();
        //     unsigned key = rhsVal % maxPhase;
        //     if (cache1.find(key) == cache1.end())
        //         cache1[key] = dynIdx1;
        //     dynIdx1 = cache1[key];
        //     staIdx1 = addOp.getRhs();
        //     staIdx1 = i32_val((rhsVal) / (maxPhase) * (maxPhase));
        //   }

        // offset along non-contiguous dimension
        Value off1 = mul(dynIdx1, stride1);
        // swizzled offset along contiguous dimension
        Value phaseId = urem(udiv(dynIdx1, i32_val(perPhase)), i32_val(maxPhase));
        Value off0 = xor_(udiv(dynIdx0, outVecVal), phaseId);
        off0 = mul(off0, outVecVal);
        Value remained = urem(dynIdx0, outVecVal);
        remained = udiv(remained, minVecVal);
        off0 = add(off0, mul(remained, minVecVal));
        Value offset = add(off1, mul(off0, stride0));
        Value staOffset = add(mul(staIdx1, stride1), mul(staIdx0, stride0));
        // add static offset
        offset = add(offset, staOffset);

        // // step 3: store
        Value smemAddr = gep(elemPtrTy, smemBase, offset);
        // Value smemAddr = sharedPtrs[i/minVec*minVec];
        smemAddr = bitcast(smemAddr, ptr_ty(wordTy, 3));
        store(word, smemAddr);
      }
    }
  }


  // -----------------------------------------------------------------------
  // Utilities
  // -----------------------------------------------------------------------

  // Convert an \param index to a multi-dim coordinate given \param shape and
  // \param order.
  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape,
                                 ArrayRef<unsigned> order) const {
    unsigned rank = shape.size();
    assert(rank == order.size());
    auto reordered = reorder(shape, order);
    auto reorderedMultiDim = delinearize(rewriter, loc, linear, reordered);
    SmallVector<Value> multiDim(rank);
    for (unsigned i = 0; i < rank; ++i) {
      multiDim[order[i]] = reorderedMultiDim[i];
    }
    return multiDim;
  }

  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape) const {
    unsigned rank = shape.size();
    assert(rank > 0);
    SmallVector<Value> multiDim(rank);
    if (rank == 1) {
      multiDim[0] = linear;
    } else {
      Value remained = linear;
      for (auto &&en : llvm::enumerate(shape.drop_back())) {
        Value dimSize = idx_val(en.value());
        multiDim[en.index()] = urem(remained, dimSize);
        remained = udiv(remained, dimSize);
      }
      multiDim[rank - 1] = remained;
    }
    return multiDim;
  }

  Value linearize(ConversionPatternRewriter &rewriter, Location loc,
                  ArrayRef<Value> multiDim, ArrayRef<unsigned> shape,
                  ArrayRef<unsigned> order) const {
    return linearize(rewriter, loc, reorder<Value>(multiDim, order),
                     reorder<unsigned>(shape, order));
  }

  Value linearize(ConversionPatternRewriter &rewriter, Location loc,
                  ArrayRef<Value> multiDim, ArrayRef<unsigned> shape) const {
    auto rank = multiDim.size();
    Value linear = idx_val(0);
    if (rank > 0) {
      linear = multiDim.back();
      for (auto [dim, dimShape] :
           llvm::reverse(llvm::zip(multiDim.drop_back(), shape.drop_back()))) {
        Value dimSize = idx_val(dimShape);
        linear = add(mul(linear, dimSize), dim);
      }
    }
    return linear;
  }

  Value dot(ConversionPatternRewriter &rewriter, Location loc,
            ArrayRef<Value> offsets, ArrayRef<Value> strides) const {
    assert(offsets.size() == strides.size());
    Value ret = idx_val(0);
    for (auto [offset, stride] : llvm::zip(offsets, strides)) {
      ret = add(ret, mul(offset, stride));
    }
    return ret;
  }

  struct SmallVectorKeyInfo {
    static unsigned getHashValue(const SmallVector<unsigned> &key) {
      return llvm::hash_combine_range(key.begin(), key.end());
    }
    static bool isEqual(const SmallVector<unsigned> &lhs,
                        const SmallVector<unsigned> &rhs) {
      return lhs == rhs;
    }
    static SmallVector<unsigned> getEmptyKey() {
      return SmallVector<unsigned>();
    }
    static SmallVector<unsigned> getTombstoneKey() {
      return {std::numeric_limits<unsigned>::max()};
    }
  };

  // -----------------------------------------------------------------------
  // Get offsets / indices for any layout
  // -----------------------------------------------------------------------

  SmallVector<Value> emitBaseIndexForLayout(Location loc,
                                            ConversionPatternRewriter &rewriter,
                                            const Attribute &layout,
                                            ArrayRef<int64_t> shape) const {
    IndexCacheKeyT key = std::make_pair(layout, llvm::to_vector(shape));
    auto cache = indexCacheInfo.baseIndexCache;
    assert(cache && "baseIndexCache is nullptr");
    auto insertPt = indexCacheInfo.indexInsertPoint;
    if (cache->count(key) > 0) {
      return cache->lookup(key);
    } else {
      ConversionPatternRewriter::InsertionGuard guard(rewriter);
      restoreInsertionPointIfSet(insertPt, rewriter);
      SmallVector<Value> result;
      if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
        result =
            emitBaseIndexForBlockedLayout(loc, rewriter, blockedLayout, shape);
      } else if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
        if (mmaLayout.isVolta())
          result = emitBaseIndexForMmaLayoutV1(loc, rewriter, mmaLayout, shape);
        if (mmaLayout.isAmpere())
          result = emitBaseIndexForMmaLayoutV2(loc, rewriter, mmaLayout, shape);
      } else {
        llvm_unreachable("unsupported emitBaseIndexForLayout");
      }
      cache->insert(std::make_pair(key, result));
      *insertPt = rewriter.saveInsertionPoint();
      return result;
    }
  }

  SmallVector<SmallVector<unsigned>>
  emitOffsetForLayout(const Attribute &layout, ArrayRef<int64_t> shape) const {
    if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>())
      return emitOffsetForBlockedLayout(blockedLayout, shape);
    if (auto mmaLayout = layout.dyn_cast<MmaEncodingAttr>()) {
      if (mmaLayout.isVolta())
        return emitOffsetForMmaLayoutV1(mmaLayout, shape);
      if (mmaLayout.isAmpere())
        return emitOffsetForMmaLayoutV2(mmaLayout, shape);
    }
    llvm_unreachable("unsupported emitOffsetForLayout");
  }

  // -----------------------------------------------------------------------
  // Emit indices
  // -----------------------------------------------------------------------
  SmallVector<SmallVector<Value>> emitIndices(Location loc,
                                              ConversionPatternRewriter &b,
                                              const Attribute &layout,
                                              ArrayRef<int64_t> shape) const {
    IndexCacheKeyT key(layout, llvm::to_vector(shape));
    auto cache = indexCacheInfo.indexCache;
    assert(cache && "indexCache is nullptr");
    auto insertPt = indexCacheInfo.indexInsertPoint;
    if (cache->count(key) > 0) {
      return cache->lookup(key);
    } else {
      ConversionPatternRewriter::InsertionGuard guard(b);
      restoreInsertionPointIfSet(insertPt, b);
      SmallVector<SmallVector<Value>> result;
      if (auto blocked = layout.dyn_cast<BlockedEncodingAttr>()) {
        result = emitIndicesForDistributedLayout(loc, b, blocked, shape);
      } else if (auto mma = layout.dyn_cast<MmaEncodingAttr>()) {
        result = emitIndicesForDistributedLayout(loc, b, mma, shape);
      } else if (auto slice = layout.dyn_cast<SliceEncodingAttr>()) {
        result = emitIndicesForSliceLayout(loc, b, slice, shape);
      } else {
        llvm_unreachable(
            "emitIndices for layouts other than blocked & slice not "
            "implemented yet");
      }
      cache->insert(std::make_pair(key, result));
      *insertPt = b.saveInsertionPoint();
      return result;
    }
  }

private:
  void restoreInsertionPointIfSet(OpBuilder::InsertPoint *insertPt,
                                  ConversionPatternRewriter &rewriter) const {
    if (insertPt->isSet()) {
      rewriter.restoreInsertionPoint(*insertPt);
    } else {
      auto func =
          rewriter.getInsertionPoint()->getParentOfType<LLVM::LLVMFuncOp>();
      rewriter.setInsertionPointToStart(&func.getBody().front());
    }
  }

  // -----------------------------------------------------------------------
  // Blocked layout indices
  // -----------------------------------------------------------------------

  // Get an index-base for each dimension for a \param blocked_layout.
  SmallVector<Value>
  emitBaseIndexForBlockedLayout(Location loc,
                                ConversionPatternRewriter &rewriter,
                                const BlockedEncodingAttr &blocked_layout,
                                ArrayRef<int64_t> shape) const {
    Value threadId = getThreadId(rewriter, loc);
    Value warpSize = idx_val(32);
    Value laneId = urem(threadId, warpSize);
    Value warpId = udiv(threadId, warpSize);
    auto sizePerThread = blocked_layout.getSizePerThread();
    auto threadsPerWarp = blocked_layout.getThreadsPerWarp();
    auto warpsPerCTA = blocked_layout.getWarpsPerCTA();
    auto order = blocked_layout.getOrder();
    unsigned rank = shape.size();

    // delinearize threadId to get the base index
    SmallVector<Value> multiDimWarpId =
        delinearize(rewriter, loc, warpId, warpsPerCTA, order);
    SmallVector<Value> multiDimThreadId =
        delinearize(rewriter, loc, laneId, threadsPerWarp, order);

    SmallVector<Value> multiDimBase(rank);
    for (unsigned k = 0; k < rank; ++k) {
      // Wrap around multiDimWarpId/multiDimThreadId incase
      // shape[k] > shapePerCTA[k]
      auto maxWarps =
          ceil<unsigned>(shape[k], sizePerThread[k] * threadsPerWarp[k]);
      auto maxThreads = ceil<unsigned>(shape[k], sizePerThread[k]);
      multiDimWarpId[k] = urem(multiDimWarpId[k], idx_val(maxWarps));
      multiDimThreadId[k] = urem(multiDimThreadId[k], idx_val(maxThreads));
      // multiDimBase[k] = (multiDimThreadId[k] +
      //                    multiDimWarpId[k] * threadsPerWarp[k]) *
      //                   sizePerThread[k];
      Value threadsPerWarpK = idx_val(threadsPerWarp[k]);
      Value sizePerThreadK = idx_val(sizePerThread[k]);
      multiDimBase[k] =
          mul(sizePerThreadK, add(multiDimThreadId[k],
                                  mul(multiDimWarpId[k], threadsPerWarpK)));
    }
    return multiDimBase;
  }

  SmallVector<SmallVector<unsigned>>
  emitOffsetForBlockedLayout(const BlockedEncodingAttr &blockedLayout,
                             ArrayRef<int64_t> shape) const {
    auto sizePerThread = blockedLayout.getSizePerThread();
    auto threadsPerWarp = blockedLayout.getThreadsPerWarp();
    auto warpsPerCTA = blockedLayout.getWarpsPerCTA();
    auto order = blockedLayout.getOrder();

    unsigned rank = shape.size();
    SmallVector<unsigned> shapePerCTA = getShapePerCTA(blockedLayout);
    SmallVector<unsigned> tilesPerDim(rank);
    for (unsigned k = 0; k < rank; ++k)
      tilesPerDim[k] = ceil<unsigned>(shape[k], shapePerCTA[k]);

    SmallVector<SmallVector<unsigned>> offset(rank);
    for (unsigned k = 0; k < rank; ++k) {
      // 1 block in minimum if shape[k] is less than shapePerCTA[k]
      for (unsigned blockOffset = 0; blockOffset < tilesPerDim[k];
           ++blockOffset)
        for (unsigned warpOffset = 0; warpOffset < warpsPerCTA[k]; ++warpOffset)
          for (unsigned threadOffset = 0; threadOffset < threadsPerWarp[k];
               ++threadOffset)
            for (unsigned elemOffset = 0; elemOffset < sizePerThread[k];
                 ++elemOffset)
              offset[k].push_back(blockOffset * sizePerThread[k] *
                                      threadsPerWarp[k] * warpsPerCTA[k] +
                                  warpOffset * sizePerThread[k] *
                                      threadsPerWarp[k] +
                                  threadOffset * sizePerThread[k] + elemOffset);
    }

    unsigned elemsPerThread = blockedLayout.getElemsPerThread(shape);
    unsigned totalSizePerThread = product<unsigned>(sizePerThread);
    SmallVector<SmallVector<unsigned>> reorderedOffset(elemsPerThread);
    for (unsigned n = 0; n < elemsPerThread; ++n) {
      unsigned linearNanoTileId = n / totalSizePerThread;
      unsigned linearNanoTileElemId = n % totalSizePerThread;
      SmallVector<unsigned> multiDimNanoTileId =
          getMultiDimIndex<unsigned>(linearNanoTileId, tilesPerDim, order);
      SmallVector<unsigned> multiDimNanoTileElemId = getMultiDimIndex<unsigned>(
          linearNanoTileElemId, sizePerThread, order);
      for (unsigned k = 0; k < rank; ++k) {
        unsigned reorderedMultiDimId =
            multiDimNanoTileId[k] *
                (sizePerThread[k] * threadsPerWarp[k] * warpsPerCTA[k]) +
            multiDimNanoTileElemId[k];
        reorderedOffset[n].push_back(offset[k][reorderedMultiDimId]);
      }
    }
    return reorderedOffset;
  }

  // -----------------------------------------------------------------------
  // Mma layout indices
  // -----------------------------------------------------------------------

  SmallVector<Value>
  emitBaseIndexForMmaLayoutV1(Location loc, ConversionPatternRewriter &rewriter,
                              const MmaEncodingAttr &mmaLayout,
                              ArrayRef<int64_t> shape) const {
    llvm_unreachable("emitIndicesForMmaLayoutV1 not implemented");
  }

  SmallVector<SmallVector<unsigned>>
  emitOffsetForMmaLayoutV1(const MmaEncodingAttr &mmaLayout,
                           ArrayRef<int64_t> shape) const {
    SmallVector<SmallVector<unsigned>> ret;

    for (unsigned i = 0; i < shape[0]; i += getShapePerCTA(mmaLayout)[0]) {
      for (unsigned j = 0; j < shape[1]; j += getShapePerCTA(mmaLayout)[1]) {
        ret.push_back({i, j});
        ret.push_back({i, j + 1});
        ret.push_back({i + 2, j});
        ret.push_back({i + 2, j + 1});
        ret.push_back({i, j + 8});
        ret.push_back({i, j + 9});
        ret.push_back({i + 2, j + 8});
        ret.push_back({i + 2, j + 9});
      }
    }
    return ret;
  }

  SmallVector<Value>
  emitBaseIndexForMmaLayoutV2(Location loc, ConversionPatternRewriter &rewriter,
                              const MmaEncodingAttr &mmaLayout,
                              ArrayRef<int64_t> shape) const {
    auto _warpsPerCTA = mmaLayout.getWarpsPerCTA();
    assert(_warpsPerCTA.size() == 2);
    SmallVector<Value> warpsPerCTA = {idx_val(_warpsPerCTA[0]),
                                      idx_val(_warpsPerCTA[1])};
    Value threadId = getThreadId(rewriter, loc);
    Value warpSize = idx_val(32);
    Value laneId = urem(threadId, warpSize);
    Value warpId = udiv(threadId, warpSize);
    Value warpId0 = urem(warpId, warpsPerCTA[0]);
    Value warpId1 = urem(udiv(warpId, warpsPerCTA[0]), warpsPerCTA[1]);
    Value offWarp0 = mul(warpId0, idx_val(16));
    Value offWarp1 = mul(warpId1, idx_val(8));

    SmallVector<Value> multiDimBase(2);
    multiDimBase[0] = add(udiv(laneId, idx_val(4)), offWarp0);
    multiDimBase[1] = add(mul(idx_val(2), urem(laneId, idx_val(4))), offWarp1);
    return multiDimBase;
  }

  SmallVector<SmallVector<unsigned>>
  emitOffsetForMmaLayoutV2(const MmaEncodingAttr &mmaLayout,
                           ArrayRef<int64_t> shape) const {
    SmallVector<SmallVector<unsigned>> ret;

    for (unsigned i = 0; i < shape[0]; i += getShapePerCTA(mmaLayout)[0]) {
      for (unsigned j = 0; j < shape[1]; j += getShapePerCTA(mmaLayout)[1]) {
        ret.push_back({i, j});
        ret.push_back({i, j + 1});
        ret.push_back({i + 8, j});
        ret.push_back({i + 8, j + 1});
      }
    }
    return ret;
  }

  // Emit indices calculation within each ConversionPattern, and returns a
  // [elemsPerThread X rank] index matrix.

  // TODO: [phil] redundant indices computation do not appear to hurt
  // performance much, but they could still significantly slow down
  // computations.
  SmallVector<SmallVector<Value>> emitIndicesForDistributedLayout(
      Location loc, ConversionPatternRewriter &rewriter,
      const Attribute &layout, ArrayRef<int64_t> shape) const {

    // step 1, delinearize threadId to get the base index
    auto multiDimBase = emitBaseIndexForLayout(loc, rewriter, layout, shape);
    // step 2, get offset of each element
    auto offset = emitOffsetForLayout(layout, shape);
    // step 3, add offset to base, and reorder the sequence of indices to
    // guarantee that elems in the same sizePerThread are adjacent in order
    unsigned rank = shape.size();
    unsigned elemsPerThread = offset.size();
    SmallVector<SmallVector<Value>> multiDimIdx(elemsPerThread,
                                                SmallVector<Value>(rank));
    for (unsigned n = 0; n < elemsPerThread; ++n)
      for (unsigned k = 0; k < rank; ++k)
        multiDimIdx[n][k] = add(multiDimBase[k], idx_val(offset[n][k]));

    return multiDimIdx;
  }

  SmallVector<SmallVector<Value>>
  emitIndicesForSliceLayout(Location loc, ConversionPatternRewriter &rewriter,
                            const SliceEncodingAttr &sliceLayout,
                            ArrayRef<int64_t> shape) const {
    auto parent = sliceLayout.getParent();
    unsigned dim = sliceLayout.getDim();
    size_t rank = shape.size();
    auto parentIndices =
        emitIndices(loc, rewriter, parent, sliceLayout.paddedShape(shape));
    unsigned numIndices = parentIndices.size();
    SmallVector<SmallVector<Value>> resultIndices;
    for (unsigned i = 0; i < numIndices; ++i) {
      SmallVector<Value> indices = parentIndices[i];
      indices.erase(indices.begin() + dim);
      resultIndices.push_back(indices);
    }
    return resultIndices;
  }

protected:
  LLVMTypeConverter *converter;
  const Allocation *allocation;
  Value smem;
  IndexCacheInfo indexCacheInfo;
};

template <typename SourceOp>
class ConvertTritonGPUOpToLLVMPattern
    : public ConvertOpToLLVMPattern<SourceOp>,
      public ConvertTritonGPUOpToLLVMPatternBase {
public:
  using OpAdaptor = typename SourceOp::Adaptor;

  explicit ConvertTritonGPUOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                           PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<SourceOp>(typeConverter, benefit),
        ConvertTritonGPUOpToLLVMPatternBase(typeConverter) {}

  explicit ConvertTritonGPUOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                           const Allocation *allocation,
                                           Value smem,
                                           PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<SourceOp>(typeConverter, benefit),
        ConvertTritonGPUOpToLLVMPatternBase(typeConverter, allocation, smem) {}

  explicit ConvertTritonGPUOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                           const Allocation *allocation,
                                           Value smem,
                                           IndexCacheInfo indexCacheInfo,
                                           PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<SourceOp>(typeConverter, benefit),
        ConvertTritonGPUOpToLLVMPatternBase(typeConverter, allocation, smem,
                                            indexCacheInfo) {}

protected:
  LLVMTypeConverter *getTypeConverter() const {
    return ((ConvertTritonGPUOpToLLVMPatternBase *)this)->getTypeConverter();
  }
};

#endif
