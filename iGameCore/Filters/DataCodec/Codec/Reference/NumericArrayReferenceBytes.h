#ifndef DATACODEC_CODEC_REFERENCE_NUMERICARRAYREFERENCEBYTES_H
#define DATACODEC_CODEC_REFERENCE_NUMERICARRAYREFERENCEBYTES_H

#include "DataCodec/Codec/NumericArray/NumericArrayReader.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
namespace datacodec::numericarrayreference {

inline bool ValidateNumericArrayRawByteSpan(
    const NumericArrayStorageParams& meta,
    const std::span<const std::uint8_t> bytes,
    const std::string_view label,
    std::string* error = nullptr) {
    std::size_t localValueSize = 0u;
    std::size_t localElementCount = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize) ||
        !TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
        return validation::AssignError(error, std::string(label) + " metadata exceeds this platform size limit");
    }
    std::size_t tupleBytes = 0u;
    std::size_t expectedBytes = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(std::max(meta.dimension, 0)),
            localValueSize,
            tupleBytes,
            std::string(label) + " tuple byte count",
            error) ||
        !validation::CheckedMulSizeT(
            localElementCount,
            tupleBytes,
            expectedBytes,
            std::string(label) + " byte size",
            error)) {
        return false;
    }
    if (bytes.size() != expectedBytes) {
        return validation::AssignError(error, std::string(label) + " byte size does not match numeric array layout");
    }
    return true;
}

inline bool ValidateNumericArrayRawByteSpan(
    const NumericArrayStorageParams& meta,
    const std::span<const std::uint8_t> bytes,
    const std::size_t elementCount,
    const std::string_view label,
    std::string* error = nullptr) {
    std::size_t localValueSize = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize)) {
        return validation::AssignError(error, std::string(label) + " metadata exceeds this platform size limit");
    }
    std::size_t tupleBytes = 0u;
    std::size_t expectedBytes = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(std::max(meta.dimension, 0)),
            localValueSize,
            tupleBytes,
            std::string(label) + " tuple byte count",
            error) ||
        !validation::CheckedMulSizeT(
            elementCount,
            tupleBytes,
            expectedBytes,
            std::string(label) + " byte size",
            error)) {
        return false;
    }
    if (bytes.size() != expectedBytes) {
        return validation::AssignError(error, std::string(label) + " byte size does not match numeric array layout");
    }
    return true;
}

template<typename TValue>
inline bool BuildNumericArrayNormalizedResampledSourceRangeBytesTyped(
    const numericarray::NumericArrayReader& referenceReader,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayStorageParams& targetMeta,
    ScratchByteBufferPool& scratchBytePool,
    const std::size_t targetElementOffset,
    const std::size_t targetElementCount,
    ScratchByteBuffer& outputBytes,
    std::string* error = nullptr) {
    outputBytes.Release();
    const auto componentCount = static_cast<std::size_t>(std::max(targetMeta.dimension, 0));
    if (componentCount == 0u) {
        return validation::AssignError(error, "numeric array resample component count is invalid");
    }
    if (referenceMeta.dimension != targetMeta.dimension ||
        referenceMeta.dataType != targetMeta.dataType) {
        return validation::AssignError(error, "numeric array resample metadata does not match");
    }
    std::size_t localTargetElementCount = 0u;
    std::size_t localReferenceElementCount = 0u;
    if (!TryParamSizeToSizeT(targetMeta.elementCount, localTargetElementCount) ||
        !TryParamSizeToSizeT(referenceMeta.elementCount, localReferenceElementCount)) {
        return validation::AssignError(error, "numeric array resample metadata exceeds this platform size limit");
    }
    if (targetElementOffset > localTargetElementCount ||
        targetElementCount > localTargetElementCount - targetElementOffset) {
        return validation::AssignError(error, "numeric array resample target range is out of bounds");
    }
    if (targetElementCount == 0u) {
        return true;
    }
    if (localReferenceElementCount == 0u) {
        return validation::AssignError(error, "numeric array resample reference field is empty");
    }

    std::size_t tupleBytes = 0u;
    if (!validation::CheckedMulSizeT(
            componentCount,
            sizeof(TValue),
            tupleBytes,
            "numeric array resample tuple bytes",
            error)) {
        return false;
    }
    const auto resolveReferencePosition = [&](const std::size_t targetIndex) -> double {
        if (localTargetElementCount <= 1u || localReferenceElementCount <= 1u) {
            return 0.0;
        }
        const auto normalized =
            static_cast<double>(targetIndex) / static_cast<double>(localTargetElementCount - 1u);
        return normalized * static_cast<double>(localReferenceElementCount - 1u);
    };

    std::size_t outputByteCountSizeT = 0u;
    if (!validation::CheckedMulSizeT(
            targetElementCount,
            tupleBytes,
            outputByteCountSizeT,
            "numeric array resample output bytes",
            error)) {
        return false;
    }
    const auto outputByteCount = static_cast<std::uint64_t>(outputByteCountSizeT);
    outputBytes = scratchBytePool.Acquire(
        static_cast<std::size_t>(outputByteCount));
    const auto windowTuples = std::max<std::size_t>(1u, kIoWindowBytes / tupleBytes);
    const auto lastTargetIndex = targetElementOffset + targetElementCount - 1u;
    const auto lastNeededIndex = std::min(
        static_cast<std::size_t>(std::floor(resolveReferencePosition(lastTargetIndex))) + 1u,
        localReferenceElementCount - 1u);
    ScratchByteBuffer referenceWindow;
    auto leftTuple = scratchBytePool.Acquire(tupleBytes);
    auto rightTuple = scratchBytePool.Acquire(tupleBytes);
    std::size_t windowBegin = 0u, windowCount = 0u;
    const auto readTuple = [&](const std::size_t index, const std::size_t nextTargetIndex,
                               ScratchByteBuffer& tuple) {
        if (index < windowBegin || index - windowBegin >= windowCount) {
            auto count = std::min(windowTuples, lastNeededIndex - index + 1u);
            // 稀疏映射只读相邻 tuple，避免每个目标点搬运整个空隙窗口
            if (nextTargetIndex <= lastTargetIndex) {
                const auto nextIndex = static_cast<std::size_t>(
                    std::floor(resolveReferencePosition(nextTargetIndex)));
                if (nextIndex > index && nextIndex - index >= windowTuples) {
                    count = std::min<std::size_t>(count, 2u);
                }
            }
            if (!referenceReader.ReadElements(index, count, scratchBytePool, referenceWindow, error)) {
                return false;
            }
            if (referenceWindow.Span().size() != count * tupleBytes) {
                return validation::AssignError(error, "numeric array resample window byte size mismatch");
            }
            windowBegin = index;
            windowCount = count;
        }
        std::memcpy(tuple.Bytes().data(), referenceWindow.Span().data() + (index - windowBegin) * tupleBytes,
            tupleBytes);
        return true;
    };
    auto* outputValues = reinterpret_cast<TValue*>(outputBytes.Bytes().data());
    for (std::size_t localTargetIndex = 0; localTargetIndex < targetElementCount; ++localTargetIndex) {
        const auto targetIndex = targetElementOffset + localTargetIndex;
        const auto referencePosition = resolveReferencePosition(targetIndex);
        const auto leftIndex = static_cast<std::size_t>(std::floor(referencePosition));
        const auto rightIndex = std::min(leftIndex + 1u, localReferenceElementCount - 1u);
        if (!readTuple(leftIndex, targetIndex + 1u, leftTuple) ||
            !readTuple(rightIndex, targetIndex + 1u, rightTuple)) { return false; }
        const auto* leftValues = reinterpret_cast<const TValue*>(leftTuple.Span().data());
        const auto* rightValues = reinterpret_cast<const TValue*>(rightTuple.Span().data());
        const auto weightRight = referencePosition - static_cast<double>(leftIndex);
        const auto weightLeft = 1.0 - weightRight;
        for (std::size_t componentIndex = 0; componentIndex < componentCount; ++componentIndex) {
            const auto leftValue =
                static_cast<double>(leftValues[componentIndex]);
            const auto rightValue =
                static_cast<double>(rightValues[componentIndex]);
            outputValues[localTargetIndex * componentCount + componentIndex] =
                static_cast<TValue>(leftValue * weightLeft + rightValue * weightRight);
        }
    }
    return true;
}

inline bool BuildNumericArrayNormalizedResampledSourceRangeBytes(
    const numericarray::NumericArrayReader& referenceReader,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayStorageParams& targetMeta,
    ScratchByteBufferPool& scratchBytePool,
    const std::size_t targetElementOffset,
    const std::size_t targetElementCount,
    ScratchByteBuffer& outputBytes,
    std::string* error = nullptr) {
    if (NumericArrayValueSize(targetMeta) == sizeof(float)) {
        return BuildNumericArrayNormalizedResampledSourceRangeBytesTyped<float>(
            referenceReader,
            referenceMeta,
            targetMeta,
            scratchBytePool,
            targetElementOffset,
            targetElementCount,
            outputBytes,
            error);
    }
    if (NumericArrayValueSize(targetMeta) == sizeof(double)) {
        return BuildNumericArrayNormalizedResampledSourceRangeBytesTyped<double>(
            referenceReader,
            referenceMeta,
            targetMeta,
            scratchBytePool,
            targetElementOffset,
            targetElementCount,
            outputBytes,
            error);
    }
    return validation::AssignError(error, "numeric array resample requires float32 or float64 data");
}

inline bool BuildNumericArrayReferenceRangeBytes(
    const numericarray::NumericArrayReader& referenceReader,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayStorageParams& targetMeta,
    ScratchByteBufferPool& scratchBytePool,
    const std::size_t targetElementOffset,
    const std::size_t targetElementCount,
    ScratchByteBuffer& outputBytes,
    std::string* error = nullptr) {
    if (referenceMeta.elementCount == targetMeta.elementCount) {
        return referenceReader.ReadElements(
            targetElementOffset,
            targetElementCount,
            scratchBytePool,
            outputBytes,
            error);
    }
    return BuildNumericArrayNormalizedResampledSourceRangeBytes(
        referenceReader,
        referenceMeta,
        targetMeta,
        scratchBytePool,
        targetElementOffset,
        targetElementCount,
        outputBytes,
        error);
}

inline std::size_t ClampNumericArrayShiftedReferenceIndex(
    const std::size_t elementIndex,
    const std::int32_t predictorOffset,
    const std::size_t elementCount) {
    if (elementCount == 0u) {
        return 0u;
    }
    const auto shifted = static_cast<std::int64_t>(elementIndex) + static_cast<std::int64_t>(predictorOffset);
    return static_cast<std::size_t>(std::clamp<std::int64_t>(
        shifted,
        0,
        static_cast<std::int64_t>(elementCount - 1u)));
}

inline bool BuildNumericArrayShiftedPredictorBlockBytesFromRange(
    const std::span<const std::uint8_t> referenceRangeBytes,
    const std::size_t firstReferenceIndex,
    const NumericArrayStorageParams& meta,
    ScratchByteBufferPool& scratchBytePool,
    const std::size_t elementOffset,
    const std::size_t elementCount,
    const std::int32_t predictorOffset,
    ScratchByteBuffer& outputBytes,
    std::string* error = nullptr) {
    outputBytes.Release();
    std::size_t localValueSize = 0u;
    std::size_t localElementCount = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize) ||
        !TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
        return validation::AssignError(error, "numeric array predictor metadata exceeds this platform size limit");
    }
    std::size_t tupleBytes = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(std::max(meta.dimension, 0)),
            localValueSize,
            tupleBytes,
            "numeric array predictor tuple size",
            error)) {
        return false;
    }
    if (tupleBytes == 0u) {
        return validation::AssignError(error, "numeric array predictor tuple size is invalid");
    }
    if (elementOffset > localElementCount || elementCount > localElementCount - elementOffset) {
        return validation::AssignError(error, "numeric array predictor target range is out of bounds");
    }
    std::size_t outputByteCountSizeT = 0u;
    if (!validation::CheckedMulSizeT(
            elementCount,
            tupleBytes,
            outputByteCountSizeT,
            "numeric array predictor output bytes",
            error)) {
        return false;
    }
    const auto outputByteCount = static_cast<std::uint64_t>(outputByteCountSizeT);
    outputBytes = scratchBytePool.Acquire(
        static_cast<std::size_t>(outputByteCount));
    auto& bytes = outputBytes.Bytes();
    for (std::size_t localIndex = 0; localIndex < elementCount; ++localIndex) {
        const auto sourceIndex = ClampNumericArrayShiftedReferenceIndex(
            elementOffset + localIndex,
            predictorOffset,
            localElementCount);
        const auto localSourceIndex = sourceIndex - firstReferenceIndex;
        const auto sourceByteOffset = localSourceIndex * tupleBytes;
        const auto targetByteOffset = localIndex * tupleBytes;
        if (sourceByteOffset > referenceRangeBytes.size() ||
            tupleBytes > referenceRangeBytes.size() - sourceByteOffset) {
            outputBytes.Release();
            return validation::AssignError(error, "numeric array predictor reference range is incomplete");
        }
        std::memcpy(bytes.data() + targetByteOffset, referenceRangeBytes.data() + sourceByteOffset, tupleBytes);
    }
    return true;
}

inline bool BuildNumericArrayPredictorReferenceBlockBytes(
    const numericarray::NumericArrayReader& referenceReader,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayStorageParams& targetMeta,
    ScratchByteBufferPool& scratchBytePool,
    const std::size_t elementOffset,
    const std::size_t elementCount,
    const std::int32_t predictorOffset,
    ScratchByteBuffer& outputBytes,
    std::string* error = nullptr) {
    if (elementCount == 0u) {
        outputBytes.Release();
        return true;
    }
    std::size_t localTargetElementCount = 0u;
    if (!TryParamSizeToSizeT(targetMeta.elementCount, localTargetElementCount)) {
        return validation::AssignError(error, "numeric array predictor target metadata exceeds this platform size limit");
    }
    const auto firstTargetIndex =
        ClampNumericArrayShiftedReferenceIndex(elementOffset, predictorOffset, localTargetElementCount);
    const auto lastTargetIndex = ClampNumericArrayShiftedReferenceIndex(
        elementOffset + elementCount - 1u,
        predictorOffset,
        localTargetElementCount);
    const auto targetRangeCount = lastTargetIndex - firstTargetIndex + 1u;
    ScratchByteBuffer referenceRangeBytes;
    if (!BuildNumericArrayReferenceRangeBytes(
            referenceReader,
            referenceMeta,
            targetMeta,
            scratchBytePool,
            firstTargetIndex,
            targetRangeCount,
            referenceRangeBytes,
            error)) {
        return false;
    }
    std::size_t localValueSize = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(targetMeta), localValueSize)) {
        return validation::AssignError(error, "numeric array predictor value size exceeds this platform size limit");
    }
    std::size_t tupleBytes = 0u;
    std::size_t expectedOutputBytes = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(std::max(targetMeta.dimension, 0)),
            localValueSize,
            tupleBytes,
            "numeric array predictor tuple bytes",
            error) ||
        !validation::CheckedMulSizeT(
            elementCount,
            tupleBytes,
            expectedOutputBytes,
            "numeric array predictor output bytes",
            error)) {
        return false;
    }
    if (referenceRangeBytes.Bytes().size() == expectedOutputBytes) {
        outputBytes = std::move(referenceRangeBytes);
        return true;
    }
    return BuildNumericArrayShiftedPredictorBlockBytesFromRange(
        referenceRangeBytes.Span(),
        firstTargetIndex,
        targetMeta,
        scratchBytePool,
        elementOffset,
        elementCount,
        predictorOffset,
        outputBytes,
        error);
}

} // namespace datacodec::numericarrayreference

#endif
