#ifndef DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_REFERENCETRANSFERCACHEBUILDER_H
#define DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_REFERENCETRANSFERCACHEBUILDER_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockFormat.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Codec/Reference/NumericArrayReferenceBytes.h"
#include "DataCodec/Codec/Reference/ReferenceCodec.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/ReferenceControlParams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <type_traits>
namespace datacodec {
namespace numericarrayreference {

struct NumericArrayReferenceSourceData {
    NumericArrayReferenceCandidate candidate;
    NumericArrayStorageParams meta;
    numericarray::NumericArraySource source;

    [[nodiscard]] bool HasReference() const noexcept {
        return candidate.HasReference();
    }
};

struct NumericArrayReferenceTransferControl {
    double affineBlockRSquared{0.95};
    TemporalPredictorControlParams predictor;
    ReferenceSelectionMode selectionMode{ReferenceSelectionMode::Auto};
    ReferenceAutoSelectionStrategy autoSelectionStrategy{
        ReferenceAutoSelectionStrategy::Exact};
    std::function<bool(
        const NumericArrayStorageParams&,
        std::span<const std::uint8_t>,
        const numericarray::NumericArrayReader&,
        const NumericArrayStorageParams&,
        ScratchByteBufferPool&,
        std::uint32_t,
        std::uint32_t,
        NumericArrayReferenceKind,
        std::uint16_t,
        std::int32_t&,
        std::string*, numericarray::NumericArrayCompressorState*)> selectPredictorOffset;
};

struct OrdinaryNumericArrayEncodedBlock {
    NumericArrayBlockHeader header;
    NumericArrayBlockLayoutParams layout;
    std::vector<std::uint8_t> bytes;
};

inline constexpr std::size_t kReferenceProbeElementCount = 4096u;

inline std::uint64_t EstimateNumericArrayBlockStoredBytes(
    const NumericArrayBlockLayoutParams& layout) noexcept {
    auto bytes = static_cast<std::uint64_t>(layout.encodedByteLength);
    bytes = validation::SaturatingAddU64(
        bytes,
        validation::SaturatingMulU64(
            static_cast<std::uint64_t>(layout.alpha.size() + layout.beta.size()),
            sizeof(double)));
    bytes = validation::SaturatingAddU64(
        bytes,
        validation::SaturatingMulU64(
            static_cast<std::uint64_t>(layout.componentLayouts.size()),
            sizeof(NumericArrayComponentLayoutParams)));
    for (const auto& option : layout.backgroundCompressor.options) {
        bytes = validation::SaturatingAddU64(
            bytes,
            static_cast<std::uint64_t>(option.first.size() + sizeof(double)));
    }
    return bytes;
}

inline bool BuildUniformNumericArrayTupleSample(
    const std::span<const std::uint8_t> sourceBytes,
    const std::size_t elementCount,
    const std::size_t tupleBytes,
    const std::size_t sampleElementCount,
    ScratchByteBufferPool& scratchBytePool,
    ScratchByteBuffer& sample,
    std::string* error = nullptr) {
    sample.Release();
    std::size_t expectedSourceBytes = 0u;
    std::size_t sampleByteCount = 0u;
    if (sampleElementCount == 0u || sampleElementCount > elementCount || tupleBytes == 0u ||
        !validation::CheckedMulSizeT(
            elementCount,
            tupleBytes,
            expectedSourceBytes,
            "reference probe source bytes",
            error) ||
        !validation::CheckedMulSizeT(
            sampleElementCount,
            tupleBytes,
            sampleByteCount,
            "reference probe sample bytes",
            error)) {
        if (error != nullptr && error->empty()) {
            validation::AssignError(error, "reference probe shape is invalid");
        }
        return false;
    }
    if (sourceBytes.size() != expectedSourceBytes) {
        return validation::AssignError(error, "reference probe source byte size is invalid");
    }
    const auto requestedBytes = static_cast<std::uint64_t>(sampleByteCount);
    sample = scratchBytePool.Acquire(
        sampleByteCount);
    auto& sampleBytes = sample.Bytes();
    for (std::size_t sampleIndex = 0u; sampleIndex < sampleElementCount; ++sampleIndex) {
        const auto sourceIndex = sampleElementCount == 1u
            ? std::size_t{0u}
            : static_cast<std::size_t>(
                (static_cast<std::uint64_t>(sampleIndex) *
                    static_cast<std::uint64_t>(elementCount - 1u)) /
                static_cast<std::uint64_t>(sampleElementCount - 1u));
        std::memcpy(
            sampleBytes.data() + sampleIndex * tupleBytes,
            sourceBytes.data() + sourceIndex * tupleBytes,
            tupleBytes);
    }
    return true;
}

inline bool BuildOrdinaryNumericArrayEncodedBlock(
    const NumericArrayStorageParams& meta,
    const CompressorConfig& defaultCompressor,
    const std::uint32_t elementOffset,
    const std::uint32_t elementCount,
    const std::span<const std::uint8_t> currentBytes,
    ScratchByteBufferPool& scratchBytePool,
    OrdinaryNumericArrayEncodedBlock& block,
    std::string* error = nullptr,
    numericarray::NumericArrayCompressorState* compressorState = nullptr,
    numericarray::NumericArrayBlockCapacitySamples* capacitySamples = nullptr) {
    block = {};
    numericarray::NumericArrayBlockParams params;
    if (!numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params, error)) {
        return false;
    }
    NumericArrayBytesCodec bytesCodec{NumericArrayBytesCodec::NumericArrayCodec};
    params.capacitySamples = capacitySamples;
    std::vector<NumericArrayComponentLayoutParams> componentLayouts;
    if (!numericarray::ResolveEncodedNumericArrayBlockBytes(
            params,
            defaultCompressor,
            elementCount,
            currentBytes,
            block.bytes,
            bytesCodec,
            error,
            &scratchBytePool,
            &componentLayouts, compressorState)) {
        return false;
    }
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(numericarray::NumericBufferSample::OrdinaryCandidate, block.bytes);
    }
    if (block.bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return validation::AssignError(error, "ordinary numeric array block exceeds current block format");
    }
    block.header = NumericArrayBlockHeader{
        .mode = NumericArrayBlockMode::NonReference,
        .referenceKind = NumericArrayReferenceKind::None,
        .codecId = NumericArrayReferenceCodecId::NonReference,
        .localParentFieldIndex = 0xFFFFu,
        .elementOffset = elementOffset,
        .elementCount = elementCount,
        .encodedByteLength = static_cast<std::uint32_t>(block.bytes.size()),
        .bytesCodec = bytesCodec,
    };
    block.layout = MakeNumericArrayBlockLayoutParams(block.header, {}, {});
    block.layout.backgroundCompressor = defaultCompressor;
    block.layout.componentLayouts = std::move(componentLayouts);
    return true;
}

inline bool ValidateIntegerReferenceCodecAndLayout(
    const NumericArrayStorageParams& meta,
    const NumericArrayStorageParams& referenceMeta,
    const NumericArrayReferenceCodecId codecId,
    std::string* error = nullptr) {
    if (!numericarray::IsIntegerNumericArrayDataType(meta.dataType)) {
        return true;
    }
    if (codecId != NumericArrayReferenceCodecId::Wavelet) {
        return validation::AssignError(error, "integer numeric array reference requires wavelet codec");
    }
    if (referenceMeta.dataType != meta.dataType ||
        referenceMeta.dimension != meta.dimension ||
        referenceMeta.elementCount != meta.elementCount) {
        return validation::AssignError(
            error,
            "integer wavelet reference requires matching data type, value size, dimension, and element count");
    }
    return true;
}


struct ReferenceEncodeBlockInput {
    std::uint32_t elementOffset{0u};
    std::uint32_t elementCount{0u};
    ScratchByteBuffer current;
    ScratchByteBuffer reference;
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
};

struct ReferenceEncodeBlockOutput {
    std::variant<OrdinaryNumericArrayEncodedBlock, NumericArrayReferenceEncodedBlock> block;
    NumericArrayBlockLayoutParams layout;
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
    std::optional<numericarray::NumericArrayBlockCapacitySamples> probeCapacitySamples;
};

inline void ObserveReferenceCandidateCapacity(
    numericarray::NumericArrayBlockCapacitySamples* samples,
    const NumericArrayReferenceEncodedBlock& block) noexcept {
    if (samples == nullptr) { return; }
    std::visit([&](const auto& fields) {
        if constexpr (std::is_same_v<std::decay_t<decltype(fields)>, WaveletReferenceBlockFields>) {
            samples->Observe(numericarray::NumericBufferSample::ReferenceCandidate, fields.waveletBytes.Bytes());
        } else {
            samples->Observe(numericarray::NumericBufferSample::ReferenceCandidate, fields.deltaBytes.Bytes());
        }
    }, block.fields);
}

inline bool ComputeReferenceEncodeBlock(
    const NumericArrayStorageParams& meta, const CompressorConfig& defaultCompressor,
    const NumericArrayReferenceSourceData& referenceData,
    const numericarray::NumericArrayReader& referenceReader,
    const NumericArrayReferenceCodecId codecId, const NumericArrayReferenceTransferControl& control,
    const ReferenceEncodeBlockInput& input, ReferenceEncodeBlockOutput& output,
    ScratchByteBufferPool& scratchBytePool, std::string* error,
    numericarray::NumericArrayCompressorState* compressorState) {
    auto* capacitySamples = output.capacitySamples ? &*output.capacitySamples : nullptr;
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(numericarray::NumericBufferSample::Raw, input.current.Bytes());
        capacitySamples->Observe(numericarray::NumericBufferSample::ReferencePrimary, input.reference.Bytes());
    }
    const auto* codec = ResolveNumericArrayReferenceCodec(codecId);
    if (codec == nullptr) { return validation::AssignError(error, "unsupported numeric array reference codec id"); }
    const auto referenceKind = ToNumericArrayReferenceKind(referenceData.candidate.scope);
    const auto localParentFieldIndex = referenceData.candidate.localParentFieldIndex;
    const auto elementOffset = static_cast<std::size_t>(input.elementOffset);
    const auto localElementCount = static_cast<std::size_t>(input.elementCount);
    const auto blockElementOffset = input.elementOffset;
    const auto blockElementCount = input.elementCount;
    const auto& currentStaged = input.current;
    const auto tupleBytes = localElementCount == 0u ? 0u : currentStaged.Span().size() / localElementCount;
    std::string referenceError;
    std::int32_t predictorOffset = 0;
    ScratchByteBuffer predictorStaged;
    const auto& referenceStaged = codecId == NumericArrayReferenceCodecId::Predictor
        ? predictorStaged : input.reference;
    if (codecId == NumericArrayReferenceCodecId::Predictor) {
        if (control.selectPredictorOffset &&
            !control.selectPredictorOffset(
                meta,
                currentStaged.Span(),
                referenceReader,
                referenceData.meta,
                scratchBytePool,
                blockElementOffset,
                blockElementCount,
                referenceKind,
                localParentFieldIndex,
                predictorOffset,
                &referenceError,
                compressorState)) {
            return validation::AssignError(
                error,
                "reference predictor selection failed: " + referenceError);
        }
        if (!BuildNumericArrayPredictorReferenceBlockBytes(
                referenceReader,
                referenceData.meta,
                meta,
                scratchBytePool,
                elementOffset,
                localElementCount,
                predictorOffset,
                predictorStaged,
                &referenceError)) {
            return validation::AssignError(
                error,
                "reference predictor staging failed: " + referenceError);
        }
    }
    if (!ValidateNumericArrayRawByteSpan(
            meta,
            referenceStaged.Span(),
            localElementCount,
            "reference numeric array spatial block",
            &referenceError)) {
        return validation::AssignError(
            error,
            "reference spatial block validation failed: " + referenceError);
    }

    if (capacitySamples != nullptr && codecId == NumericArrayReferenceCodecId::Predictor) {
        capacitySamples->Observe(numericarray::NumericBufferSample::ReferenceShifted, predictorStaged.Bytes());
    }
    const NumericArrayReferenceCodecEncodeInput fullReferenceInput{
        .meta = meta,
        .defaultCompressor = defaultCompressor,
        .scratchBytePool = scratchBytePool,
        .control = NumericArrayReferenceCodecControl{
            .affineBlockRSquared = control.affineBlockRSquared,
        },
        .currentBytes = currentStaged.Span(),
        .referenceBytes = referenceStaged.Span(),
        .elementOffset = blockElementOffset,
        .elementCount = blockElementCount,
        .componentCount = static_cast<std::size_t>(std::max(meta.dimension, 0)),
        .referenceKind = referenceKind,
        .localParentFieldIndex = localParentFieldIndex,
        .predictorOffset = predictorOffset,
        .compressorState = compressorState,
        .capacitySamples = capacitySamples,
    };

    NumericArrayReferencePreparedBlock preparedReference;
    const auto prepareResult = codec->PrepareBlock(
        fullReferenceInput,
        preparedReference,
        &referenceError);
    if (prepareResult.IsFailed()) {
        return validation::AssignError(
            error,
            "reference spatial block preparation failed: " + referenceError);
    }

    const auto publishOrdinaryBlock = [&](OrdinaryNumericArrayEncodedBlock& block) {
        output.layout = std::move(block.layout);
        output.block = std::move(block);
        return true;
    };
    const auto publishReferenceBlock = [&](
        NumericArrayReferenceEncodedBlock& block, NumericArrayBlockLayoutParams& layout) {
        output.layout = std::move(layout);
        output.block = std::move(block);
        return true;
    };
    const auto buildFullOrdinary = [&](OrdinaryNumericArrayEncodedBlock& block) {
        return BuildOrdinaryNumericArrayEncodedBlock(
            meta,
            defaultCompressor,
            blockElementOffset,
            blockElementCount,
            currentStaged.Span(),
            scratchBytePool,
            block,
            error, compressorState, capacitySamples);
    };
    const auto buildFullReference = [&](NumericArrayReferenceEncodedBlock& block) {
        referenceError.clear();
        const auto result = codec->EncodePreparedBlock(
            fullReferenceInput,
            preparedReference,
            block,
            &referenceError);
        if (result.IsFailed()) {
            return validation::AssignError(
                error,
                "reference spatial block encoding failed: " + referenceError);
        }
        if (result.IsRejected()) {
            return validation::AssignError(
                error,
                "prepared reference spatial block was unexpectedly rejected");
        }
        ObserveReferenceCandidateCapacity(capacitySamples, block);
        return true;
    };

    if (prepareResult.IsRejected()) {
        if (control.selectionMode == ReferenceSelectionMode::Forced) {
            return validation::AssignError(
                error,
                std::string("forced reference spatial block was rejected: ") +
                    NumericArrayReferenceRejectReasonName(prepareResult.rejectReason));
        }
        OrdinaryNumericArrayEncodedBlock ordinaryBlock;
        if (!buildFullOrdinary(ordinaryBlock) || !publishOrdinaryBlock(ordinaryBlock)) {
            return false;
        }
        return true;
    }

    if (control.selectionMode == ReferenceSelectionMode::Forced) {
        NumericArrayReferenceEncodedBlock referenceBlock;
        if (!buildFullReference(referenceBlock)) {
            return false;
        }
        auto referenceLayout = MakeNumericArrayReferenceBlockLayout(referenceBlock);
        if (!publishReferenceBlock(referenceBlock, referenceLayout)) {
            return false;
        }
        return true;
    }

    bool useReference = false;
    const bool useBoundedProbe =
        control.autoSelectionStrategy == ReferenceAutoSelectionStrategy::BoundedProbe &&
        referenceKind == NumericArrayReferenceKind::IntraArray &&
        localElementCount > kReferenceProbeElementCount;

    OrdinaryNumericArrayEncodedBlock ordinaryBlock;
    NumericArrayReferenceEncodedBlock referenceBlock;
    NumericArrayBlockLayoutParams referenceLayout;
    if (!useBoundedProbe) {
        if (!buildFullOrdinary(ordinaryBlock) || !buildFullReference(referenceBlock)) {
            return false;
        }
        referenceLayout = MakeNumericArrayReferenceBlockLayout(referenceBlock);
        useReference = EstimateNumericArrayBlockStoredBytes(referenceLayout) <
            EstimateNumericArrayBlockStoredBytes(ordinaryBlock.layout);
    } else {
        // 探测与完整候选可能同时存活，使用独立取样身份
        if (capacitySamples != nullptr) { output.probeCapacitySamples.emplace(); }
        auto* probeSamples = output.probeCapacitySamples ? &*output.probeCapacitySamples : nullptr;
        ScratchByteBuffer currentSample;
        ScratchByteBuffer referenceSample;
        if (!BuildUniformNumericArrayTupleSample(
                currentStaged.Span(),
                localElementCount,
                tupleBytes,
                kReferenceProbeElementCount,
                scratchBytePool,
                currentSample,
                error) ||
            !BuildUniformNumericArrayTupleSample(
                referenceStaged.Span(),
                localElementCount,
                tupleBytes,
                kReferenceProbeElementCount,
                scratchBytePool,
                referenceSample,
                error)) {
            return false;
        }

        if (probeSamples != nullptr) {
            probeSamples->Observe(numericarray::NumericBufferSample::Raw, currentSample.Bytes());
            probeSamples->Observe(numericarray::NumericBufferSample::ReferencePrimary, referenceSample.Bytes());
        }
        OrdinaryNumericArrayEncodedBlock ordinaryProbe;
        if (!BuildOrdinaryNumericArrayEncodedBlock(
                meta,
                defaultCompressor,
                blockElementOffset,
                static_cast<std::uint32_t>(kReferenceProbeElementCount),
                currentSample.Span(),
                scratchBytePool,
                ordinaryProbe,
                error, compressorState, probeSamples)) {
            return false;
        }
        const NumericArrayReferenceCodecEncodeInput probeInput{
            .meta = meta,
            .defaultCompressor = defaultCompressor,
            .scratchBytePool = scratchBytePool,
            .control = NumericArrayReferenceCodecControl{
                .affineBlockRSquared = control.affineBlockRSquared,
            },
            .currentBytes = currentSample.Span(),
            .referenceBytes = referenceSample.Span(),
            .elementOffset = blockElementOffset,
            .elementCount = static_cast<std::uint32_t>(kReferenceProbeElementCount),
            .componentCount = static_cast<std::size_t>(std::max(meta.dimension, 0)),
            .referenceKind = referenceKind,
            .localParentFieldIndex = localParentFieldIndex,
            .predictorOffset = predictorOffset,
            .compressorState = compressorState,
            .capacitySamples = probeSamples,
        };
        NumericArrayReferencePreparedBlock probePreparedReference;
        referenceError.clear();
        const auto probePrepareResult = codec->PrepareBlock(
            probeInput,
            probePreparedReference,
            &referenceError);
        if (probePrepareResult.IsFailed()) {
            return validation::AssignError(
                error,
                "reference probe preparation failed: " + referenceError);
        }
        NumericArrayReferenceEncodedBlock referenceProbe;
        if (probePrepareResult.IsEncoded()) {
            referenceError.clear();
            const auto probeResult = codec->EncodePreparedBlock(
                probeInput,
                probePreparedReference,
                referenceProbe,
                &referenceError);
            if (probeResult.IsFailed()) {
                return validation::AssignError(
                    error,
                    "reference probe encoding failed: " + referenceError);
            }
            if (probeResult.IsRejected()) {
                return validation::AssignError(
                    error,
                    "prepared reference probe was unexpectedly rejected");
            }
            ObserveReferenceCandidateCapacity(probeSamples, referenceProbe);
            const auto probeLayout = MakeNumericArrayReferenceBlockLayout(referenceProbe);
            useReference = EstimateNumericArrayBlockStoredBytes(probeLayout) <
                EstimateNumericArrayBlockStoredBytes(ordinaryProbe.layout);
        }

        if (useReference) {
            if (!buildFullReference(referenceBlock)) {
                return false;
            }
            referenceLayout = MakeNumericArrayReferenceBlockLayout(referenceBlock);
        } else if (!buildFullOrdinary(ordinaryBlock)) {
            return false;
        }
    }

    if (useReference) {
        if (!publishReferenceBlock(referenceBlock, referenceLayout)) {
            return false;
        }
    } else if (!publishOrdinaryBlock(ordinaryBlock)) {
        return false;
    }
    return true;
}

inline bool BuildNumericArrayReferenceTransferCache(
    const NumericArrayStorageParams& meta,
    const CompressorConfig& defaultCompressor,
    const numericarray::NumericArraySource& currentSource,
    const NumericArrayReferenceSourceData& referenceData,
    const NumericArrayReferenceCodecId codecId,
    const NumericArrayReferenceTransferControl& control,
    DataCodecExecutionResources& resources,
    std::shared_ptr<bytestore::IByteSource>& transferCache,
    bytestore::ByteStoreSession& byteStoreSession,
    std::vector<NumericArrayBlockLayoutParams>* blockLayouts = nullptr,
    std::string* error = nullptr,
    const std::string& storeLabel = "numeric_array_reference_transfer",
    const callback::CapacityCallback& recordCapacitySamples = {},
    const bool parallelInputRead = false) {
    transferCache.reset();
    if (blockLayouts != nullptr) { blockLayouts->clear(); }
    numericarray::NumericArrayReader currentReader, referenceReader;
    if (!numericarray::BuildNumericArrayReader(currentSource, currentReader, error) ||
        !numericarray::BuildNumericArrayReader(referenceData.source, referenceReader, error) ||
        !ValidateIntegerReferenceCodecAndLayout(meta, referenceData.meta, codecId, error)) { return false; }
    const auto elementCount = currentReader.source.layout.elementCount;
    if (elementCount > std::numeric_limits<std::uint32_t>::max() || meta.elementCount != elementCount ||
        referenceData.meta.elementCount != meta.elementCount ||
        referenceReader.source.layout.elementCount != meta.elementCount) {
        return validation::AssignError(error, "reference numeric arrays must share a valid spatial domain");
    }
    std::size_t valueSize = 0u, tupleBytes = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), valueSize) ||
        !validation::CheckedMulSizeT(static_cast<std::size_t>(std::max(meta.dimension, 0)), valueSize,
            tupleBytes, "reference tuple bytes", error)) { return false; }
    if (elementCount != 0u && (tupleBytes == 0u || currentReader.ElementBytes() != tupleBytes)) {
        return validation::AssignError(error, "current numeric array tuple size does not match metadata");
    }
    if (ResolveNumericArrayReferenceCodec(codecId) == nullptr) {
        return validation::AssignError(error, "unsupported numeric array reference codec id");
    }
    // reference 读取使用已准备的数据视图，源文件分块不参与当前编码批次选择
    auto phase = WaitForHeavyPhase(resources);
    if (!phase) { return false; }
    auto bodyTransferCache = bytestore::CreateAppendableByteStore(byteStoreSession, storeLabel, error);
    if (!bodyTransferCache) { return false; }
    if (elementCount == 0u) {
        if (!bodyTransferCache->Seal(error)) { return false; }
        transferCache = std::move(bodyTransferCache);
        return true;
    }
    bytestore::AppendableByteStoreWriter writer(bodyTransferCache, resources);
    std::size_t nextOffset = 0u, committedElements = 0u;
    const bool parallelRead = parallelInputRead && currentReader.SupportsParallelMemoryRead() &&
        referenceReader.SupportsParallelMemoryRead();
    const auto readInputs = [&](ReferenceEncodeBlockInput& block, ScratchByteBufferPool& scratch,
                                std::string* readError) {
        const auto sample = [&](const numericarray::NumericBufferSample kind) -> BufferCapacitySample* {
            return block.capacitySamples ? &block.capacitySamples->values[static_cast<std::size_t>(kind)] : nullptr;
        };
        if (!currentReader.ReadElements(block.elementOffset, block.elementCount, scratch, block.current, readError,
                sample(numericarray::NumericBufferSample::ReaderOrder)) ||
            !ValidateNumericArrayRawByteSpan(meta, block.current.Span(), block.elementCount,
                "current numeric array spatial block", readError)) { return false; }
        return codecId == NumericArrayReferenceCodecId::Predictor ||
            referenceReader.ReadElements(block.elementOffset, block.elementCount, scratch, block.reference, readError,
                sample(numericarray::NumericBufferSample::ReferenceReaderOrder));
    };
    phase.reset();
    resources.SetWorkType({.path = ResourceWorkPath::ReferenceEncode,
        .codec = static_cast<std::uint32_t>(codecId),
        .scalar = static_cast<std::uint32_t>(currentReader.source.layout.dataType),
        .components = static_cast<std::uint32_t>(meta.dimension),
        .blockElements = numericarray::kSpatialBlockElementCount,
        .referencePath = static_cast<std::uint32_t>(codecId)});
    const bool success = RunOrderedBlocks<ReferenceEncodeBlockInput, ReferenceEncodeBlockOutput>(
        resources,
        [&] { return nextOffset < elementCount; },
        [&](ReferenceEncodeBlockInput& block) {
            block.elementOffset = static_cast<std::uint32_t>(nextOffset);
            block.elementCount = static_cast<std::uint32_t>(std::min<std::size_t>(
                numericarray::kSpatialBlockElementCount, elementCount - nextOffset));
            if (recordCapacitySamples) { block.capacitySamples.emplace(); }
            if (!parallelRead && !readInputs(block, resources.Scratch(), error)) { return false; }
            nextOffset += block.elementCount;
            return true;
        },
        [&](ReferenceEncodeBlockInput& block, ReferenceEncodeBlockOutput& output, WorkerContext& worker) {
            std::string localError;
            if (parallelRead && !readInputs(block, worker.Scratch(), &localError)) {
                resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "reference-block-read", "BuildNumericArrayReferenceTransferCache", localError));
                return false;
            }
            output.capacitySamples = block.capacitySamples;
            if (!ComputeReferenceEncodeBlock(meta, defaultCompressor, referenceData, referenceReader, codecId,
                    control, block, output, worker.Scratch(), &localError, &worker.NumericCompressor())) {
                resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "reference-block-encode", "ComputeReferenceEncodeBlock", localError));
                return false;
            }
            return true;
        },
        [&](ReferenceEncodeBlockOutput& output) {
            if (output.layout.elementOffset != committedElements ||
                output.layout.elementCount > elementCount - committedElements) {
                return validation::AssignError(error, "reference block does not match the next commit range");
            }
            const bool written = std::visit([&](const auto& block) {
                using Block = std::decay_t<decltype(block)>;
                if constexpr (std::is_same_v<Block, OrdinaryNumericArrayEncodedBlock>) {
                    return WriteNumericArrayBlock(writer, block.header, {}, {}, block.bytes, error);
                } else {
                    return WriteNumericArrayReferenceEncodedBlock(writer, block, error);
                }
            }, output.block);
            if (!written) { return false; }
            if (output.capacitySamples && recordCapacitySamples) {
                try { recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { resources.RecordDiagnosticExportFailure(); }
            }
            if (output.probeCapacitySamples && recordCapacitySamples) {
                try { recordCapacitySamples(output.probeCapacitySamples->values); }
                catch (...) { resources.RecordDiagnosticExportFailure(); }
            }
            committedElements += output.layout.elementCount;
            if (blockLayouts != nullptr) { blockLayouts->push_back(std::move(output.layout)); }
            return committedElements != elementCount || bodyTransferCache->Seal(error);
        });
    if (!success) {
        if (error != nullptr && error->empty()) {
            validation::AssignError(error, "reference block flow failed");
        }
        return false;
    }
    transferCache = std::move(bodyTransferCache);
    return true;
}

} // namespace numericarrayreference
} // namespace datacodec

#endif
