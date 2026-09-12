#ifndef DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEDECODE_H
#define DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEDECODE_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Codec/Attributes/AttributeDecodePlan.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedAttributeCacheSet.h"
#include "DataCodec/Runtime/Cache/DecodeCache/ReferenceCacheNumericArraySource.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Codec/Reference/DecodedReference.h"
#include "DataCodec/Codec/Reference/NumericArrayReferenceBytes.h"
#include "DataCodec/Codec/Reference/ReferenceCodec.h"
#include "DataCodec/Codec/Reference/PreparedDecodeReference.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockReader.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace datacodec {
namespace decodeimpl {

namespace detail {

inline bool ResolveDecodedBlockBytes(
    const AttrStorageParams& meta,
    const ParsedNumericArrayBlock& block,
    MutableArray<std::uint8_t> decodedBytes,
    std::string* error = nullptr,
    numericarray::NumericArrayCompressorState* compressorState = nullptr,
    numericarray::NumericArrayBlockCapacitySamples* capacitySamples = nullptr,
    ArrayWorkspace* workspace = nullptr) {
    numericarray::NumericArrayBlockParams params;
    if (!numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params, error)) {
        return false;
    }
    params.capacitySamples = capacitySamples;
    params.workspace = workspace;
    if (block.header.mode == NumericArrayBlockMode::LayeredResidual) {
        return numericarray::ResolveDecodedLayeredResidualNumericArrayBlockBytes(
            params,
            block.header.elementCount,
            block.backgroundCompressor,
            block.backgroundEncodedByteLength,
            block.componentLayouts,
            block.regionLayers,
            block.bytes,
            decodedBytes,
            error, compressorState);
    }
    return numericarray::ResolveDecodedNumericArrayBlockBytes(
        params,
        block.backgroundCompressor,
        block.header.elementCount,
        block.header.bytesCodec,
        block.componentLayouts,
        block.bytes,
        decodedBytes,
        error, compressorState);
}

inline bool TryFindReferenceAttrIndex(
    const CodecStorageParams& storageParams,
    const AttrStorageParams& targetMeta,
    std::size_t& referenceIndex) {
    for (std::size_t attrIndex = 0; attrIndex < storageParams.attrParams.size(); ++attrIndex) {
        const auto& candidate = storageParams.attrParams[attrIndex];
        if (candidate.name == targetMeta.name &&
            candidate.attachmentType == targetMeta.attachmentType &&
            candidate.dataType == targetMeta.dataType &&
            candidate.dimension == targetMeta.dimension) {
            referenceIndex = attrIndex;
            return true;
        }
    }
    referenceIndex = 0;
    return false;
}

inline bool UsesNonReferenceCodec(const ParsedNumericArrayBlock& block) {
    return block.header.codecId == NumericArrayReferenceCodecId::NonReference ||
        block.header.mode == NumericArrayBlockMode::NonReference;
}

template<typename TReferenceDecoder>
inline bool EnsureTemporalReferencesDecoded(
    DecodedAttributeReference* attributeKeyFrameReference,
    const AttrStorageParams& targetMeta,
    TReferenceDecoder& referenceDecoder,
    std::string* error = nullptr);

struct AttributePayloadRange {
    std::size_t attrIndex{0u};
    std::uint64_t offset{0u};
    std::uint64_t byteCount{0u};
};

class AttributePayloadRangeStream final {
public:
    AttributePayloadRangeStream(
        std::span<const std::uint8_t> bytes,
        const std::uint64_t begin,
        const std::uint64_t byteCount)
        : m_bytes(bytes),
          m_backingByteSize(static_cast<std::uint64_t>(bytes.size())),
          m_begin(begin),
          m_byteCount(byteCount) {}

    AttributePayloadRangeStream(
        const bytestore::IByteSource& source,
        const std::uint64_t begin,
        const std::uint64_t byteCount)
        : m_source(&source),
          m_backingByteSize(source.ByteSizeHint()),
          m_begin(begin),
          m_byteCount(byteCount) {}

    [[nodiscard]] std::uint64_t Position() const noexcept { return m_position; }

    bool ReadBytes(void* target, const std::size_t byteCount, std::string* error = nullptr) {
        if (byteCount == 0u) {
            return true;
        }
        if (target == nullptr) {
            return validation::AssignError(error, "attribute range stream target is null");
        }
        if (m_position > m_byteCount || byteCount > m_byteCount - m_position) {
            return validation::AssignError(error, "attribute range stream read exceeds payload range");
        }
        std::uint64_t absoluteOffset = 0u;
        if (!validation::CheckedAddU64(
                m_begin,
                m_position,
                absoluteOffset,
                "attribute range stream absolute offset",
                error)) {
            return false;
        }
        if (absoluteOffset > m_backingByteSize ||
            byteCount > m_backingByteSize - absoluteOffset) {
            return validation::AssignError(error, "attribute range stream read exceeds backing bytes");
        }
        if (m_source != nullptr) {
            if (!m_source->Read(
                    absoluteOffset,
                    std::span<std::uint8_t>(static_cast<std::uint8_t*>(target), byteCount),
                    error)) {
                return false;
            }
        } else {
            std::memcpy(
                target,
                m_bytes.data() + static_cast<std::size_t>(absoluteOffset),
                byteCount);
        }
        if (!validation::CheckedAddU64(
                m_position,
                static_cast<std::uint64_t>(byteCount),
                m_position,
                "attribute range stream position",
                error)) {
            return false;
        }
        return true;
    }

    bool ReadVector(std::vector<std::uint8_t>& output, const std::size_t byteCount, std::string* error = nullptr) {
        output.assign(byteCount, 0u);
        return byteCount == 0u || ReadBytes(output.data(), output.size(), error);
    }

    template<typename T>
    bool ReadScalar(T& value, std::string* error = nullptr) {
        value = {};
        return ReadBytes(&value, sizeof(T), error);
    }

    bool Skip(const std::uint64_t byteCount, std::string* error = nullptr) {
        if (m_position > m_byteCount || byteCount > m_byteCount - m_position) {
            return validation::AssignError(error, "attribute range stream skip exceeds payload range");
        }
        if (!validation::CheckedAddU64(
                m_position,
                byteCount,
                m_position,
                "attribute range stream position",
                error)) {
            return false;
        }
        return true;
    }

private:
    std::span<const std::uint8_t> m_bytes;
    const bytestore::IByteSource* m_source{nullptr};
    std::uint64_t m_backingByteSize{0u};
    std::uint64_t m_begin{0u};
    std::uint64_t m_byteCount{0u};
    std::uint64_t m_position{0u};
};

inline std::uint64_t AttributePayloadBackingByteSize(
    const std::span<const std::uint8_t> bytes) noexcept {
    return static_cast<std::uint64_t>(bytes.size());
}

inline std::uint64_t AttributePayloadBackingByteSize(
    const bytestore::IByteSource& source) noexcept {
    return source.ByteSizeHint();
}

inline bool ResolveAttributePayloadRanges(
    const CodecStorageParams& storageParams,
    std::vector<AttributePayloadRange>& payloadRanges,
    std::string* error = nullptr) {
    payloadRanges.clear();
    std::vector<std::size_t> payloadOrder;
    if (!ResolveAttributePayloadOrder(storageParams, payloadOrder, error)) {
        return false;
    }
    payloadRanges.reserve(payloadOrder.size());
    std::uint64_t offset = 0u;
    for (const auto attrIndex : payloadOrder) {
        const auto payloadBytes = static_cast<std::uint64_t>(storageParams.attrParams[attrIndex].binaryCount);
        if (!validation::CanAddU64(offset, payloadBytes)) {
            validation::AssignError(error, "attribute payload ranges exceed stream address space");
            return false;
        }
        payloadRanges.push_back(AttributePayloadRange{
            attrIndex,
            offset,
            payloadBytes,
        });
        if (!validation::CheckedAddU64(
                offset,
                payloadBytes,
                offset,
                "attribute payload ranges",
                error)) {
            return false;
        }
    }
    return true;
}

inline bool WriteDecodedAttributeBlock(
    DecodedAttributeCacheSet& attributes,
    const std::size_t attrIndex,
    const AttrStorageParams& meta,
    const NumericArrayBlockHeader& header,
    const std::span<const std::uint8_t> decodedBlockBytes,
    std::string* error = nullptr) {
    std::size_t localValueSize = 0u;
    std::size_t localElementCount = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize) ||
        !TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
        return validation::AssignError(error, "attribute metadata exceeds this platform size limit");
    }
    const auto componentCount = static_cast<std::size_t>(std::max(meta.dimension, 0));
    std::size_t tupleBytes = 0u;
    if (!validation::CheckedMulSizeT(
            componentCount,
            localValueSize,
            tupleBytes,
            "attribute block tuple bytes",
            error)) {
        return false;
    }
    std::size_t expectedBytes = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(header.elementCount),
            tupleBytes,
            expectedBytes,
            "attribute block decoded bytes",
            error)) {
        return false;
    }
    if (decodedBlockBytes.size() != expectedBytes) {
        return validation::AssignError(error, "attribute block decoded size does not match range");
    }
    std::size_t byteOffset = 0u;
    if (!validation::CheckedMulSizeT(
            static_cast<std::size_t>(header.elementOffset),
            tupleBytes,
            byteOffset,
            "attribute block decoded byte offset",
            error)) {
        return false;
    }
    std::size_t fieldBytes = 0u;
    if (!validation::CheckedMulSizeT(
            localElementCount,
            tupleBytes,
            fieldBytes,
            "attribute decoded field bytes",
            error)) {
        return false;
    }
    if (byteOffset > fieldBytes ||
        decodedBlockBytes.size() > fieldBytes - byteOffset) {
        return validation::AssignError(error, "attribute block decoded range exceeds field size");
    }

    const auto windowCount = std::max<std::size_t>(1u, kIoWindowBytes / std::max<std::size_t>(tupleBytes, 1u));
    for (std::size_t offset = 0u; offset < header.elementCount;) {
        const auto count = std::min<std::size_t>(windowCount, header.elementCount - offset);
        if (!attributes.WriteAttributeRange(attrIndex, header.elementOffset + offset, count,
                decodedBlockBytes.data() + offset * tupleBytes, count * tupleBytes, error)) { return false; }
        offset += count;
    }
    return true;
}

struct AttributeReferenceRangeObservation {
    ParamSize worksetBytes{0u};
    ParamSize resampledWorksetBytes{0u};
    ParamSize predictorRangeStagingBytes{0u};
    ParamSize predictorShiftedCopyBytes{0u};
    bool temporal{false};
    bool intra{false};
    bool resampled{false};
    bool predictorZeroOffset{false};
    bool predictorContinuousShift{false};
    bool predictorBoundaryClamp{false};
};

inline ParamSize SaturatingReferenceMetricMultiply(
    const ParamSize left,
    const ParamSize right) noexcept {
    if (left != 0u && right > std::numeric_limits<ParamSize>::max() / left) {
        return std::numeric_limits<ParamSize>::max();
    }
    return left * right;
}

inline void ObserveAttributeReferenceRange(
    const NumericArrayReferenceKind referenceKind,
    const NumericArrayReferenceCodecId codecId,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayStorageParams& targetMeta,
    const ParsedNumericArrayBlock& block,
    const auto& referenceBytes,
    AttributeReferenceRangeObservation* observation) {
    if (observation == nullptr) {
        return;
    }
    *observation = {};
    observation->temporal = referenceKind == NumericArrayReferenceKind::TemporalKeyFrame;
    observation->intra = referenceKind == NumericArrayReferenceKind::IntraArray;
    observation->worksetBytes = static_cast<ParamSize>(referenceBytes.Span().size());
    observation->resampled = referenceMeta.elementCount != targetMeta.elementCount;
    if (observation->resampled) {
        observation->resampledWorksetBytes = observation->worksetBytes;
    }
    if (codecId != NumericArrayReferenceCodecId::Predictor) {
        return;
    }
    if (block.header.elementCount == 0u) {
        return;
    }

    std::size_t targetElementCount = 0u;
    std::size_t targetValueSize = 0u;
    if (!TryParamSizeToSizeT(targetMeta.elementCount, targetElementCount) ||
        !TryParamSizeToSizeT(NumericArrayValueSize(targetMeta), targetValueSize) ||
        targetElementCount == 0u) {
        return;
    }
    const auto firstReferenceIndex = numericarrayreference::ClampNumericArrayShiftedReferenceIndex(
        static_cast<std::size_t>(block.header.elementOffset),
        block.header.predictorOffset,
        targetElementCount);
    const auto lastReferenceIndex = numericarrayreference::ClampNumericArrayShiftedReferenceIndex(
        static_cast<std::size_t>(block.header.elementOffset) +
            static_cast<std::size_t>(block.header.elementCount) - 1u,
        block.header.predictorOffset,
        targetElementCount);
    const auto stagedElementCount = lastReferenceIndex - firstReferenceIndex + 1u;
    const auto tupleBytes = SaturatingReferenceMetricMultiply(
        static_cast<ParamSize>(std::max(targetMeta.dimension, 0)),
        static_cast<ParamSize>(targetValueSize));
    observation->predictorRangeStagingBytes = SaturatingReferenceMetricMultiply(
        static_cast<ParamSize>(stagedElementCount),
        tupleBytes);
    if (block.header.predictorOffset == 0) {
        observation->predictorZeroOffset = true;
        return;
    }
    if (observation->predictorRangeStagingBytes == observation->worksetBytes) {
        observation->predictorContinuousShift = true;
        return;
    }
    observation->predictorBoundaryClamp = true;
    observation->predictorShiftedCopyBytes = observation->worksetBytes;
}

struct AttributeDecodeTimingDetail {
    std::size_t attrIndex{0u};
    std::string name;
    AttrAttachment attachmentType{AttrAttachment::Point};
    DataType dataType{DataType::Float32};
    ParamSize elementCount{0u};
    ParamSize binaryCount{0u};
    ParamSize rawValueBytes{0u};
    ParamSize encodedBlockBytes{0u};
    ParamSize backgroundEncodedBytes{0u};
    std::int32_t dimension{0};
    ParamSize valueSize{0u};
    std::size_t blockCount{0u};
    std::size_t nonReferenceBlocks{0u};
    std::size_t referenceBlocks{0u};
    std::size_t intraReferenceBlocks{0u};
    std::size_t temporalReferenceBlocks{0u};
    std::size_t affineReferenceBlocks{0u};
    std::size_t waveletReferenceBlocks{0u};
    std::size_t predictorReferenceBlocks{0u};
    std::size_t layeredResidualBlocks{0u};
    std::size_t regionLayerCount{0u};
    std::size_t componentLayoutCount{0u};
    double elapsedMs{0.0};
    double payloadBlockReadMs{0.0};
    double ordinaryDecodeMs{0.0};
    double referenceResolveMs{0.0};
    double temporalKeyReferenceEnsureMs{0.0};
    double referenceRangeResolveMs{0.0};
    double referenceDecodeMs{0.0};
    double affineReferenceDecodeMs{0.0};
    double waveletReferenceDecodeMs{0.0};
    double predictorReferenceDecodeMs{0.0};
    double cacheWriteMs{0.0};
    ParamSize ordinaryDecodedBytes{0u};
    ParamSize referenceDecodedBytes{0u};
    ParamSize referenceBytesRead{0u};
    ParamSize intraReferenceWorksetBytes{0u};
    ParamSize temporalReferenceWorksetBytes{0u};
    ParamSize resampledReferenceWorksetBytes{0u};
    ParamSize predictorRangeStagingBytes{0u};
    ParamSize predictorShiftedCopyBytes{0u};
    ParamSize waveletLowBlobBytes{0u};
    ParamSize waveletHighBlobBytes{0u};
    ParamSize cacheWriteBytes{0u};
    std::size_t temporalKeyFieldCacheHits{0u};
    std::size_t temporalKeyFieldCacheMisses{0u};
    std::size_t predictorZeroOffsetBlocks{0u};
    std::size_t predictorContinuousShiftBlocks{0u};
    std::size_t predictorBoundaryClampBlocks{0u};
};

struct AttributeDecodeWorkBreakdown {
    double payloadBlockReadMs{0.0};
    double ordinaryDecodeMs{0.0};
    double referenceResolveMs{0.0};
    double temporalKeyReferenceEnsureMs{0.0};
    double referenceRangeResolveMs{0.0};
    double referenceDecodeMs{0.0};
    double affineReferenceDecodeMs{0.0};
    double waveletReferenceDecodeMs{0.0};
    double predictorReferenceDecodeMs{0.0};
    double cacheWriteMs{0.0};
    ParamSize ordinaryDecodedBytes{0u};
    ParamSize referenceDecodedBytes{0u};
    ParamSize referenceBytesRead{0u};
    ParamSize intraReferenceWorksetBytes{0u};
    ParamSize temporalReferenceWorksetBytes{0u};
    ParamSize resampledReferenceWorksetBytes{0u};
    ParamSize predictorRangeStagingBytes{0u};
    ParamSize predictorShiftedCopyBytes{0u};
    ParamSize waveletLowBlobBytes{0u};
    ParamSize waveletHighBlobBytes{0u};
    ParamSize cacheWriteBytes{0u};
    std::size_t temporalKeyFieldCacheHits{0u};
    std::size_t temporalKeyFieldCacheMisses{0u};
    std::size_t predictorZeroOffsetBlocks{0u};
    std::size_t predictorContinuousShiftBlocks{0u};
    std::size_t predictorBoundaryClampBlocks{0u};
};

inline ParamSize SaturatingParamSizeMultiply(const ParamSize left, const ParamSize right) noexcept {
    if (left != 0u && right > std::numeric_limits<ParamSize>::max() / left) {
        return std::numeric_limits<ParamSize>::max();
    }
    return left * right;
}

inline ParamSize SaturatingParamSizeAdd(const ParamSize left, const ParamSize right) noexcept {
    if (right > std::numeric_limits<ParamSize>::max() - left) {
        return std::numeric_limits<ParamSize>::max();
    }
    return left + right;
}

inline void AccumulateAttributeReferenceRangeObservation(
    AttributeDecodeWorkBreakdown& workBreakdown,
    const AttributeReferenceRangeObservation& observation) noexcept {
    if (observation.intra) {
        workBreakdown.intraReferenceWorksetBytes = SaturatingParamSizeAdd(
            workBreakdown.intraReferenceWorksetBytes,
            observation.worksetBytes);
    }
    if (observation.temporal) {
        workBreakdown.temporalReferenceWorksetBytes = SaturatingParamSizeAdd(
            workBreakdown.temporalReferenceWorksetBytes,
            observation.worksetBytes);
    }
    workBreakdown.resampledReferenceWorksetBytes = SaturatingParamSizeAdd(
        workBreakdown.resampledReferenceWorksetBytes,
        observation.resampledWorksetBytes);
    workBreakdown.predictorRangeStagingBytes = SaturatingParamSizeAdd(
        workBreakdown.predictorRangeStagingBytes,
        observation.predictorRangeStagingBytes);
    workBreakdown.predictorShiftedCopyBytes = SaturatingParamSizeAdd(
        workBreakdown.predictorShiftedCopyBytes,
        observation.predictorShiftedCopyBytes);
    workBreakdown.predictorZeroOffsetBlocks += observation.predictorZeroOffset ? 1u : 0u;
    workBreakdown.predictorContinuousShiftBlocks += observation.predictorContinuousShift ? 1u : 0u;
    workBreakdown.predictorBoundaryClampBlocks += observation.predictorBoundaryClamp ? 1u : 0u;
}

inline bool IsTemporalReferenceFieldCached(
    const DecodedAttributeReference* attributeKeyFrameReference,
    const AttrStorageParams& targetMeta) {
    if (attributeKeyFrameReference == nullptr ||
        attributeKeyFrameReference->store == nullptr) {
        return false;
    }
    std::size_t referenceAttrIndex = 0u;
    return TryFindReferenceAttrIndex(
               attributeKeyFrameReference->reference.storageParams,
               targetMeta,
               referenceAttrIndex) &&
        attributeKeyFrameReference->store->Complete(referenceAttrIndex);
}

inline AttributeDecodeTimingDetail BuildAttributeDecodeTimingDetail(
    const std::size_t attrIndex,
    const AttrStorageParams& meta,
    const double elapsedMs,
    const AttributeDecodeWorkBreakdown& workBreakdown = {}) {
    AttributeDecodeTimingDetail detail;
    detail.attrIndex = attrIndex;
    detail.name = meta.name;
    detail.attachmentType = meta.attachmentType;
    detail.dataType = meta.dataType;
    detail.elementCount = meta.elementCount;
    detail.binaryCount = meta.binaryCount;
    detail.dimension = meta.dimension;
    detail.valueSize = NumericArrayValueSize(meta);
    detail.elapsedMs = elapsedMs;
    detail.payloadBlockReadMs = workBreakdown.payloadBlockReadMs;
    detail.ordinaryDecodeMs = workBreakdown.ordinaryDecodeMs;
    detail.referenceResolveMs = workBreakdown.referenceResolveMs;
    detail.temporalKeyReferenceEnsureMs = workBreakdown.temporalKeyReferenceEnsureMs;
    detail.referenceRangeResolveMs = workBreakdown.referenceRangeResolveMs;
    detail.referenceDecodeMs = workBreakdown.referenceDecodeMs;
    detail.affineReferenceDecodeMs = workBreakdown.affineReferenceDecodeMs;
    detail.waveletReferenceDecodeMs = workBreakdown.waveletReferenceDecodeMs;
    detail.predictorReferenceDecodeMs = workBreakdown.predictorReferenceDecodeMs;
    detail.cacheWriteMs = workBreakdown.cacheWriteMs;
    detail.ordinaryDecodedBytes = workBreakdown.ordinaryDecodedBytes;
    detail.referenceDecodedBytes = workBreakdown.referenceDecodedBytes;
    detail.referenceBytesRead = workBreakdown.referenceBytesRead;
    detail.intraReferenceWorksetBytes = workBreakdown.intraReferenceWorksetBytes;
    detail.temporalReferenceWorksetBytes = workBreakdown.temporalReferenceWorksetBytes;
    detail.resampledReferenceWorksetBytes = workBreakdown.resampledReferenceWorksetBytes;
    detail.predictorRangeStagingBytes = workBreakdown.predictorRangeStagingBytes;
    detail.predictorShiftedCopyBytes = workBreakdown.predictorShiftedCopyBytes;
    detail.waveletLowBlobBytes = workBreakdown.waveletLowBlobBytes;
    detail.waveletHighBlobBytes = workBreakdown.waveletHighBlobBytes;
    detail.cacheWriteBytes = workBreakdown.cacheWriteBytes;
    detail.temporalKeyFieldCacheHits = workBreakdown.temporalKeyFieldCacheHits;
    detail.temporalKeyFieldCacheMisses = workBreakdown.temporalKeyFieldCacheMisses;
    detail.predictorZeroOffsetBlocks = workBreakdown.predictorZeroOffsetBlocks;
    detail.predictorContinuousShiftBlocks = workBreakdown.predictorContinuousShiftBlocks;
    detail.predictorBoundaryClampBlocks = workBreakdown.predictorBoundaryClampBlocks;
    const auto tupleWidth = static_cast<ParamSize>(std::max(meta.dimension, 0));
    detail.rawValueBytes = SaturatingParamSizeMultiply(
        SaturatingParamSizeMultiply(meta.elementCount, tupleWidth),
        NumericArrayValueSize(meta));

    detail.blockCount = meta.blockLayouts.size();
    for (const auto& layout : meta.blockLayouts) {
        detail.encodedBlockBytes = SaturatingParamSizeAdd(detail.encodedBlockBytes, layout.encodedByteLength);
        detail.backgroundEncodedBytes = SaturatingParamSizeAdd(
            detail.backgroundEncodedBytes,
            layout.backgroundEncodedByteLength);
        detail.regionLayerCount += layout.regionLayers.size();
        detail.componentLayoutCount += layout.componentLayouts.size();
        if (layout.referenceKind == NumericArrayReferenceKind::None) {
            ++detail.nonReferenceBlocks;
        } else {
            ++detail.referenceBlocks;
        }
        if (layout.referenceKind == NumericArrayReferenceKind::IntraArray) {
            ++detail.intraReferenceBlocks;
        } else if (layout.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame) {
            ++detail.temporalReferenceBlocks;
        }

        if (layout.mode == NumericArrayBlockMode::AffineReference) {
            ++detail.affineReferenceBlocks;
        } else if (layout.mode == NumericArrayBlockMode::WaveletReference) {
            ++detail.waveletReferenceBlocks;
        } else if (layout.mode == NumericArrayBlockMode::PredictorReference) {
            ++detail.predictorReferenceBlocks;
        } else if (layout.mode == NumericArrayBlockMode::LayeredResidual) {
            ++detail.layeredResidualBlocks;
        }
    }
    return detail;
}

using AttributeDecodeTimingCallback = std::function<void(const AttributeDecodeTimingDetail&)>;

struct AttributeDecodeData {
    const CodecStorageParams& storageParams;
    DecodedAttributeReference* attributeKeyFrameReference{nullptr};
};

struct AttributeDecodeCache {
    const CacheResources& cacheResources;
    bytestore::ByteStoreSession& byteStoreSession;
    DecodedAttributeCacheSet& attributes;
};

struct AttributeDecodeContext {
    callback::CapacityCallback recordCapacitySamples;
    AttributeDecodeTimingCallback timingCallback;
};

struct AttributePayloadDecodeRuntime {
    AttributeDecodeData data;
    AttributeDecodeCache cache;
    AttributeDecodeContext context;
};

struct AttributeDecodedBlock {
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
    NumericArrayBlockHeader header;
    FixedScratchBuffer bytes;
    AttributeDecodeWorkBreakdown work;
};

inline void AccumulateAttributeDecodeWork(
    AttributeDecodeWorkBreakdown& total, const AttributeDecodeWorkBreakdown& block) noexcept {
    total.payloadBlockReadMs += block.payloadBlockReadMs;
    total.ordinaryDecodeMs += block.ordinaryDecodeMs;
    total.referenceResolveMs += block.referenceResolveMs;
    total.temporalKeyReferenceEnsureMs += block.temporalKeyReferenceEnsureMs;
    total.referenceRangeResolveMs += block.referenceRangeResolveMs;
    total.referenceDecodeMs += block.referenceDecodeMs;
    total.affineReferenceDecodeMs += block.affineReferenceDecodeMs;
    total.waveletReferenceDecodeMs += block.waveletReferenceDecodeMs;
    total.predictorReferenceDecodeMs += block.predictorReferenceDecodeMs;
    total.cacheWriteMs += block.cacheWriteMs;
    total.ordinaryDecodedBytes = SaturatingParamSizeAdd(total.ordinaryDecodedBytes, block.ordinaryDecodedBytes);
    total.referenceDecodedBytes = SaturatingParamSizeAdd(total.referenceDecodedBytes, block.referenceDecodedBytes);
    total.referenceBytesRead = SaturatingParamSizeAdd(total.referenceBytesRead, block.referenceBytesRead);
    total.intraReferenceWorksetBytes = SaturatingParamSizeAdd(total.intraReferenceWorksetBytes, block.intraReferenceWorksetBytes);
    total.temporalReferenceWorksetBytes = SaturatingParamSizeAdd(total.temporalReferenceWorksetBytes, block.temporalReferenceWorksetBytes);
    total.resampledReferenceWorksetBytes = SaturatingParamSizeAdd(total.resampledReferenceWorksetBytes, block.resampledReferenceWorksetBytes);
    total.predictorRangeStagingBytes = SaturatingParamSizeAdd(total.predictorRangeStagingBytes, block.predictorRangeStagingBytes);
    total.predictorShiftedCopyBytes = SaturatingParamSizeAdd(total.predictorShiftedCopyBytes, block.predictorShiftedCopyBytes);
    total.waveletLowBlobBytes = SaturatingParamSizeAdd(total.waveletLowBlobBytes, block.waveletLowBlobBytes);
    total.waveletHighBlobBytes = SaturatingParamSizeAdd(total.waveletHighBlobBytes, block.waveletHighBlobBytes);
    total.cacheWriteBytes = SaturatingParamSizeAdd(total.cacheWriteBytes, block.cacheWriteBytes);
    total.temporalKeyFieldCacheHits = SaturatingParamSizeAdd(total.temporalKeyFieldCacheHits, block.temporalKeyFieldCacheHits);
    total.temporalKeyFieldCacheMisses = SaturatingParamSizeAdd(total.temporalKeyFieldCacheMisses, block.temporalKeyFieldCacheMisses);
    total.predictorZeroOffsetBlocks = SaturatingParamSizeAdd(total.predictorZeroOffsetBlocks, block.predictorZeroOffsetBlocks);
    total.predictorContinuousShiftBlocks = SaturatingParamSizeAdd(total.predictorContinuousShiftBlocks, block.predictorContinuousShiftBlocks);
    total.predictorBoundaryClampBlocks = SaturatingParamSizeAdd(total.predictorBoundaryClampBlocks, block.predictorBoundaryClampBlocks);
}

inline bool ComputeAttributeDecodedBlock(
    const CodecStorageParams& storageParams, const AttrStorageParams& meta,
    const DecodedAttributeCacheSet& attributes, DecodedAttributeReference* attributeKeyFrameReference,
    const numericarray::NumericArrayBlockPayload& input, AttributeDecodedBlock& output,
    WorkerContext& worker, DecodeBlockWorkspace& workspace, const bool collectTiming, std::string* error) {
    auto scratchBytes = workspace.View<std::uint8_t>(input.memory.scratch);
    scratchBytes.resize(scratchBytes.capacity());
    ArrayWorkspace scratch(scratchBytes.Span());
    const auto block = numericarray::MakeParsedBlockView(input);
    output.header = input.header;
    output.bytes = FixedScratchBuffer(workspace.View<std::uint8_t>(input.memory.raw));
    auto& decodedBlockBytes = output.bytes.Bytes();
    auto* capacitySamples = output.capacitySamples ? &*output.capacitySamples : nullptr;
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(numericarray::NumericBufferSample::EncodedInput, input.bytes.Bytes());
    }
    auto* workBreakdown = collectTiming ? &output.work : nullptr;
    if (UsesNonReferenceCodec(block)) {
        const auto decodeStart = callback::StartTiming(workBreakdown != nullptr);
        if (!ResolveDecodedBlockBytes(meta, block, decodedBlockBytes, error, &worker.NumericCompressor(), capacitySamples, &scratch)) {
            return false;
        }
        if (workBreakdown != nullptr) {
            workBreakdown->ordinaryDecodeMs += callback::ElapsedMilliseconds(decodeStart);
            workBreakdown->ordinaryDecodedBytes = SaturatingParamSizeAdd(
                workBreakdown->ordinaryDecodedBytes,
                static_cast<ParamSize>(decodedBlockBytes.size()));
        }
    } else {
        const auto referenceResolveStart = callback::StartTiming(workBreakdown != nullptr);
        const auto referenceRangeResolveStart = referenceResolveStart;
        std::size_t referenceIndex = block.header.localParentFieldIndex;
        const DecodedAttributeCacheSet* referenceStore = &attributes;
        const CodecStorageParams* referenceStorage = &storageParams;
        if (block.header.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame) {
            if (!attributeKeyFrameReference || !attributeKeyFrameReference->store ||
                !TryFindReferenceAttrIndex(attributeKeyFrameReference->reference.storageParams, meta, referenceIndex)) {
                return validation::AssignError(error, "temporal attribute reference is unavailable");
            }
            referenceStore = attributeKeyFrameReference->store.get();
            referenceStorage = &attributeKeyFrameReference->reference.storageParams;
        }
        if (referenceIndex >= referenceStorage->attrParams.size() || !referenceStore->Complete(referenceIndex)) {
            return validation::AssignError(error, "attribute reference must be complete before admission");
        }
        const auto& referenceMeta = referenceStorage->attrParams[referenceIndex];
        numericarray::NumericArraySource referenceSource;
        numericarray::NumericArrayReader referenceReader;
        auto preparedReference = FixedScratchBuffer(workspace.View<std::uint8_t>(input.memory.reference));
        if (!BuildDecodedAttributeCacheNumericArraySource(*referenceStore, referenceIndex, referenceMeta, referenceSource, error) ||
            !numericarray::BuildNumericArrayReader(referenceSource, referenceReader, error) ||
            !PrepareDecodeReference(referenceReader, referenceMeta, meta, block.header,
                preparedReference.Bytes(), scratch, error)) { return false; }
        const auto* referenceBytes = &preparedReference;
        const std::size_t referenceElementOffset = 0u;
        AttributeReferenceRangeObservation rangeObservation;
        ObserveAttributeReferenceRange(block.header.referenceKind, block.header.codecId,
            referenceMeta, meta, block, preparedReference, &rangeObservation);
        if (capacitySamples != nullptr) {
            capacitySamples->Observe(numericarray::NumericBufferSample::ReferencePrimary, preparedReference.Bytes());
        }
        if (workBreakdown != nullptr) {
            workBreakdown->referenceResolveMs += callback::ElapsedMilliseconds(referenceResolveStart);
            workBreakdown->referenceRangeResolveMs +=
                callback::ElapsedMilliseconds(referenceRangeResolveStart);
            workBreakdown->referenceBytesRead = SaturatingParamSizeAdd(
                workBreakdown->referenceBytesRead,
                static_cast<ParamSize>(referenceBytes->Span().size()));
            AccumulateAttributeReferenceRangeObservation(*workBreakdown, rangeObservation);
        }

        const auto* codec = ResolveNumericArrayReferenceCodec(block.header.codecId);
        if (codec == nullptr) {
            return validation::AssignError(error, "unsupported attribute reference codec");
        }
        const auto referenceDecodeStart = callback::StartTiming(workBreakdown != nullptr);
        NumericArrayReferenceCodecDecodeTelemetry codecTelemetry;
        if (!codec->DecodeBlock(
                NumericArrayReferenceCodecDecodeInput{
                    .meta = meta,
                    .block = block,
                    .referenceBytes = referenceBytes->Span(),
                    .referenceElementOffset = referenceElementOffset,
                    .telemetry = workBreakdown != nullptr ? &codecTelemetry : nullptr,
                    .compressorState = &worker.NumericCompressor(),
                    .capacitySamples = capacitySamples,
                    .workspace = &scratch,
                },
                decodedBlockBytes,
                error)) {
            return false;
        }
        if (workBreakdown != nullptr) {
            const auto referenceDecodeMs = callback::ElapsedMilliseconds(referenceDecodeStart);
            workBreakdown->referenceDecodeMs += referenceDecodeMs;
            switch (block.header.codecId) {
                case NumericArrayReferenceCodecId::Affine:
                    workBreakdown->affineReferenceDecodeMs += referenceDecodeMs;
                    break;
                case NumericArrayReferenceCodecId::Wavelet:
                    workBreakdown->waveletReferenceDecodeMs += referenceDecodeMs;
                    break;
                case NumericArrayReferenceCodecId::Predictor:
                    workBreakdown->predictorReferenceDecodeMs += referenceDecodeMs;
                    break;
                case NumericArrayReferenceCodecId::NonReference:
                default:
                    break;
            }
            workBreakdown->referenceDecodedBytes = SaturatingParamSizeAdd(
                workBreakdown->referenceDecodedBytes,
                static_cast<ParamSize>(decodedBlockBytes.size()));
            workBreakdown->waveletLowBlobBytes = SaturatingParamSizeAdd(
                workBreakdown->waveletLowBlobBytes,
                codecTelemetry.waveletLowBlobBytes);
            workBreakdown->waveletHighBlobBytes = SaturatingParamSizeAdd(
                workBreakdown->waveletHighBlobBytes,
                codecTelemetry.waveletHighBlobBytes);
        }
    }

    return true;
}

template<typename TPayloadBacking, typename TReferenceDecoder>
inline bool DecodeSingleAttributeRangeToCache(
    const CodecStorageParams& storageParams, const CacheResources& runtime,
    DecodedAttributeCacheSet& attributes, DecodedAttributeReference* attributeKeyFrameReference,
    const TPayloadBacking& payloadBacking, const AttributePayloadRange& payloadRange,
    TReferenceDecoder& referenceDecoder, AttributeDecodeWorkBreakdown* workBreakdown = nullptr,
    std::string* error = nullptr, const callback::CapacityCallback& recordCapacitySamples = {}) {
    if (payloadRange.attrIndex >= storageParams.attrParams.size()) {
        return validation::AssignError(error, "attribute payload range index is out of range");
    }
    const auto& meta = storageParams.attrParams[payloadRange.attrIndex];
    const auto backingBytes = AttributePayloadBackingByteSize(payloadBacking);
    if (payloadRange.byteCount != meta.binaryCount || payloadRange.offset > backingBytes ||
        payloadRange.byteCount > backingBytes - payloadRange.offset) {
        return validation::AssignError(error, "attribute payload range does not match its backing and metadata");
    }
    numericarray::NumericArrayBlockParams params;
    if (!numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params, error)) { return false; }
    AttributePayloadRangeStream stream(payloadBacking, payloadRange.offset, payloadRange.byteCount);
    numericarray::NumericDecodeCursor<AttributePayloadRangeStream> cursor{stream, params, meta.blockLayouts, runtime};
    if (!cursor.Prepare(error)) { return false; }
    std::uint64_t payloadBytes = 0u;
    bool needsTemporal = false;
    for (const auto& layout : meta.blockLayouts) {
        if (!validation::CheckedAddU64(payloadBytes, layout.encodedByteLength, payloadBytes,
                "attribute block payload bytes", error)) { return false; }
        needsTemporal |= layout.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame;
        if (layout.referenceKind == NumericArrayReferenceKind::IntraArray &&
            !attributes.Complete(layout.localParentFieldIndex)) {
            return validation::AssignError(error, "attribute intra-field parent must be complete before block admission");
        }
    }
    if (payloadBytes != payloadRange.byteCount) {
        return validation::AssignError(error, "attribute block payload sizes do not cover the field");
    }
    // 前驱准备先于目标 Begin 与块准入，禁止计算任务等待同池解码依赖
    if (needsTemporal) {
        if (attributeKeyFrameReference == nullptr) {
            return validation::AssignError(error, "attribute temporal reference is missing");
        }
        const auto start = callback::StartTiming(workBreakdown != nullptr);
        const bool cached = IsTemporalReferenceFieldCached(attributeKeyFrameReference, meta);
        if (!EnsureTemporalReferencesDecoded(attributeKeyFrameReference, meta, referenceDecoder, error)) { return false; }
        if (workBreakdown != nullptr) {
            workBreakdown->temporalKeyReferenceEnsureMs += callback::ElapsedMilliseconds(start);
            workBreakdown->temporalKeyFieldCacheHits += cached ? 1u : 0u;
            workBreakdown->temporalKeyFieldCacheMisses += cached ? 0u : 1u;
        }
    }
    auto& root = runtime.Run();
    auto phase = WaitForHeavyPhase(root);
    if (!phase || !attributes.BeginAttribute(payloadRange.attrIndex, meta, error)) { return false; }
    struct FieldGuard {
        DecodedAttributeCacheSet& fields;
        std::size_t index;
        ~FieldGuard() { if (!fields.Complete(index)) { fields.ReleaseFieldBytes(index); } }
    } guard{attributes, payloadRange.attrIndex};
    if (params.elementCount == 0u) { return attributes.EndAttribute(payloadRange.attrIndex, error); }
    ParamSize committed = 0u;
    phase.reset();
    numericarray::NumericDecodeMemoryLayout nextMemory;
    return RunOrderedBlocks<numericarray::NumericArrayBlockPayload, AttributeDecodedBlock>(
        root, [&] { return cursor.HasMore(); },
        [&](numericarray::NumericArrayBlockPayload& input, const SlotLease&, DecodeBlockWorkspace& workspace) {
            const auto start = callback::StartTiming(workBreakdown != nullptr);
            if (!cursor.ReadNext(input, nextMemory, workspace, error)) { return false; }
            if (workBreakdown != nullptr) { workBreakdown->payloadBlockReadMs += callback::ElapsedMilliseconds(start); }
            return true;
        },
        [&](const numericarray::NumericArrayBlockPayload& input, AttributeDecodedBlock& output, WorkerContext& worker, DecodeBlockWorkspace& workspace) {
            std::string localError;
            if (recordCapacitySamples) { output.capacitySamples.emplace(); }
            if (!ComputeAttributeDecodedBlock(storageParams, meta, attributes, attributeKeyFrameReference,
                    input, output, worker, workspace, workBreakdown != nullptr, &localError)) {
                root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                    "attribute-block-decode", "ComputeAttributeDecodedBlock", localError));
                return false;
            }
            return true;
        },
        [&](AttributeDecodedBlock& output) {
            if (output.header.elementOffset != committed) {
                return validation::AssignError(error, "attribute commit range is not contiguous");
            }
            const auto start = callback::StartTiming(workBreakdown != nullptr);
            if (!WriteDecodedAttributeBlock(attributes, payloadRange.attrIndex, meta,
                    output.header, output.bytes.Span(), error)) { return false; }
            if (workBreakdown != nullptr) {
                AccumulateAttributeDecodeWork(*workBreakdown, output.work);
                workBreakdown->cacheWriteMs += callback::ElapsedMilliseconds(start);
                workBreakdown->cacheWriteBytes = SaturatingParamSizeAdd(
                    workBreakdown->cacheWriteBytes, output.bytes.Span().size());
            }
            if (output.capacitySamples && recordCapacitySamples) {
                output.capacitySamples->Observe(numericarray::NumericBufferSample::Output, output.bytes.Bytes());
                try { recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { root.RecordDiagnosticExportFailure(); }
            }
            committed += output.header.elementCount;
            if (committed != meta.elementCount) { return true; }
            if (stream.Position() != payloadRange.byteCount) {
                return validation::AssignError(error, "attribute decode consumed an unexpected payload size");
            }
            return attributes.EndAttribute(payloadRange.attrIndex, error);
        }, cursor.singleRecord, [&] { return cursor.NextWorkType(ResourceWorkPath::AttributeDecode); }, [&] {
            const auto& layout = meta.blockLayouts[cursor.nextBlock];
            const AttrStorageParams* referenceMeta = nullptr;
            if (layout.referenceKind == NumericArrayReferenceKind::IntraArray) {
                referenceMeta = &storageParams.attrParams.at(layout.localParentFieldIndex);
            } else if (layout.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame) {
                std::size_t index = 0u;
                if (!attributeKeyFrameReference ||
                    !TryFindReferenceAttrIndex(attributeKeyFrameReference->reference.storageParams, meta, index)) {
                    throw std::invalid_argument("attribute memory plan reference is unavailable");
                }
                referenceMeta = &attributeKeyFrameReference->reference.storageParams.attrParams[index];
            }
            nextMemory = numericarray::MakeNumericDecodeMemoryLayout(meta, layout, referenceMeta, false);
            return nextMemory;
        });
}

template<typename TPayloadBacking, typename TReferenceDecoder>
inline bool DecodeAttributePayloadRangesToCache(
    AttributePayloadDecodeRuntime& decodeRuntime,
    const TPayloadBacking& payloadBacking,
    const std::span<const std::size_t> targetAttrIndices,
    TReferenceDecoder& referenceDecoder,
    std::string* error = nullptr) {
    const auto& storageParams = decodeRuntime.data.storageParams;
    auto* attributeKeyFrameReference = decodeRuntime.data.attributeKeyFrameReference;
    const auto& cacheResources = decodeRuntime.cache.cacheResources;
    auto& byteStoreSession = decodeRuntime.cache.byteStoreSession;
    auto& attributes = decodeRuntime.cache.attributes;
    const auto& timingCallback = decodeRuntime.context.timingCallback;
    if (!attributes.IsInitialized() &&
        !attributes.Initialize(
            storageParams,
            byteStoreSession,
            error)) {
        return false;
    }
    std::vector<AttributePayloadRange> payloadRanges;
    if (!ResolveAttributePayloadRanges(storageParams, payloadRanges, error)) {
        return false;
    }
    if (payloadRanges.empty() || targetAttrIndices.empty()) {
        return true;
    }

    const auto attrCount = storageParams.attrParams.size();
    std::vector<AttributePayloadRange> rangeByAttr(attrCount);
    std::vector<std::uint8_t> hasRange(attrCount, 0u);
    for (const auto& range : payloadRanges) {
        if (range.attrIndex >= attrCount) {
            return validation::AssignError(error, "attribute payload range index is out of range");
        }
        rangeByAttr[range.attrIndex] = range;
        hasRange[range.attrIndex] = 1u;
    }

    for (std::size_t attrIndex = 0; attrIndex < attrCount; ++attrIndex) {
        if (hasRange[attrIndex] == 0u) {
            return validation::AssignError(error, "attribute payload range is missing");
        }
    }

    std::vector<std::size_t> executionOrder;
    if (!ResolveAttributeExecutionOrder(storageParams, targetAttrIndices, executionOrder, error)) { return false; }

    const bool collectTiming = static_cast<bool>(timingCallback);
    for (const auto attrIndex : executionOrder) {
        if (attributes.Complete(attrIndex)) { continue; }
        if (cacheResources.Run().Stopped()) { return false; }
        AttributeDecodeWorkBreakdown work;
        const auto start = callback::StartTiming(collectTiming);
        if (!DecodeSingleAttributeRangeToCache(storageParams, cacheResources, attributes, attributeKeyFrameReference,
                payloadBacking, rangeByAttr[attrIndex], referenceDecoder, collectTiming ? &work : nullptr, error,
                decodeRuntime.context.recordCapacitySamples)) {
            return false;
        }
        if (collectTiming) {
            timingCallback(BuildAttributeDecodeTimingDetail(attrIndex, storageParams.attrParams[attrIndex],
                callback::ElapsedMilliseconds(start), work));
        }
    }
    for (const auto attrIndex : targetAttrIndices) {
        if (attrIndex >= attrCount || !attributes.Complete(attrIndex)) {
            return validation::AssignError(error, "requested attribute was not decoded");
        }
    }
    return true;
}

template<typename TReferenceDecoder>
inline bool EnsureTemporalReferencesDecoded(
    DecodedAttributeReference* attributeKeyFrameReference,
    const AttrStorageParams& targetMeta,
    TReferenceDecoder& referenceDecoder,
    std::string* error) {
    if (attributeKeyFrameReference != nullptr &&
        !referenceDecoder(*attributeKeyFrameReference, targetMeta, "key frame", error)) {
        if (error != nullptr && !error->empty()) {
            return validation::AssignError(error, "failed to decode key-frame reference: " + *error);
        }
        return validation::AssignError(error, "failed to decode key-frame reference");
    }
    return true;
}

} // namespace detail

} // namespace decodeimpl
} // namespace datacodec

#endif
