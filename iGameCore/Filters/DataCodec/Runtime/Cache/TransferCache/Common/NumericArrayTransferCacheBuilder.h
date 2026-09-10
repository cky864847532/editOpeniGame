#ifndef DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_COMMON_NUMERICARRAYTRANSFERCACHEBUILDER_H
#define DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_COMMON_NUMERICARRAYTRANSFERCACHEBUILDER_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockEncode.h"
#include "DataCodec/Codec/NumericArray/NumericArrayReader.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
namespace encodeimpl {

struct NumericArrayTransferCacheStats {
    std::size_t encodeBlockCount{0};
    std::uint64_t logicalRawBytes{0};
    std::uint64_t maxEncodeBlockRawBytes{0};
    std::uint64_t maxEncodeBlockResidentBytes{0};
    std::uint64_t maxEncodedBlockBytes{0};
    std::uint64_t maxBlockResidentBytes{0};
    std::uint64_t fragmentBytes{0};
};

struct NumericArrayTransferCacheResult {
    std::shared_ptr<bytestore::IByteSource> transferCache;
    NumericArrayTransferCacheStats stats;
    std::vector<NumericArrayBlockLayoutParams> blockLayouts;
};

struct NumericArrayTransferCacheRuntime {
    std::function<void(std::chrono::nanoseconds)> recordFloatingPointEncodeDuration;
    callback::CapacityCallback recordCapacitySamples;
    bool parallelInputRead{false};
};

inline bool FinalizeNumericArrayTransferCacheResult(
    NumericArrayTransferCacheResult& result,
    std::string* error) {
    if (result.transferCache == nullptr) {
        return validation::AssignError(error, "numeric array transfer cache is null");
    }
    result.stats.fragmentBytes = result.transferCache->ByteSizeHint();
    return true;
}


struct NumericEncodeBlockInput {
    std::uint32_t elementOffset{0u};
    std::uint32_t elementCount{0u};
    ScratchByteBuffer raw;
    std::span<const RegionRun> regionRuns;
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
};

struct NumericEncodeBlockOutput {
    NumericArrayBlockHeader header;
    NumericArrayBlockLayoutParams layout;
    ScratchByteBuffer payload;
    std::uint64_t rawBytes{0u};
    std::uint64_t rawCapacity{0u};
    std::chrono::nanoseconds encodeDuration{};
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
};

struct NumericEncodeCursor {
    const numericarray::NumericArrayReader& reader;
    std::span<const RegionRun> sortedRegionRuns;
    std::size_t nextOffset{0u};

    bool HasMore() const noexcept { return nextOffset < reader.source.layout.elementCount; }
    bool ReadNext(NumericEncodeBlockInput& block, ScratchByteBufferPool& scratch, std::string* error,
                  const bool readValues = true) {
        block.elementOffset = static_cast<std::uint32_t>(nextOffset);
        block.elementCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            numericarray::kSpatialBlockElementCount, reader.source.layout.elementCount - nextOffset));
        auto* orderSample = block.capacitySamples ? &block.capacitySamples->values[
            static_cast<std::size_t>(numericarray::NumericBufferSample::ReaderOrder)] : nullptr;
        if (readValues && !reader.ReadElements(nextOffset, block.elementCount, scratch, block.raw, error, orderSample)) {
            return false;
        }
        block.regionRuns = numericarray::FindIntersectingRegionRuns(
            sortedRegionRuns, block.elementOffset, block.elementCount);
        nextOffset += block.elementCount;
        return true;
    }
};

struct NumericEncodeRegionState {
    std::shared_ptr<bytestore::MemoryStore> owner;
    std::span<const RegionRun> runs;
    numericarray::PreparedRegionPrecision precision;
};

inline bool PrepareNumericEncodeRegions(
    const numericarray::NumericArrayBlockParams& params, const ParamSize totalElementCount,
    DataCodecExecutionResources& resources, const HeavyPhaseLease& phase,
    bytestore::ByteStoreSession& session, NumericEncodeRegionState& state, std::string* error) {
    state = {};
    const auto& control = *params.regionControl;
    if (!numericarray::ValidateRegionControlForEncode(control, error)) { return false; }
    if (control.regions.empty()) { return true; }
    if (params.regionRuns == nullptr || params.regionRuns->empty()) {
        return validation::AssignError(error, "region residual encoding requires region runs");
    }
    const auto count = params.regionRuns->size();
    std::size_t byteSize = 0u;
    if (!validation::CheckedMulSizeT(count, sizeof(RegionRun), byteSize, "field region runs", error)) {
        return false;
    }
    auto owner = std::static_pointer_cast<bytestore::MemoryStore>(session.CreateSizedStore(
        bytestore::ByteStorePurpose::Contiguous, byteSize, "numeric_region_runs", error));
    if (!owner) { return false; }
    const auto bytes = owner->WritableBytes();
    if (bytes.size() != byteSize || reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(RegionRun) != 0u) {
        return validation::AssignError(error, "field region storage is not an aligned complete array");
    }
    const auto writable = std::span<RegionRun>(reinterpret_cast<RegionRun*>(bytes.data()), count);
    if (!RunTerminalWork(resources, phase, [&](WorkerContext&) {
            std::string localError;
            std::copy(params.regionRuns->begin(), params.regionRuns->end(), writable.begin());
            if (!numericarray::SortAndValidateRegionRunsForEncode(
                    writable, totalElementCount, control.regions.size(), &localError) ||
                !numericarray::PrepareRegionPrecision(control, state.precision, &localError)) {
                resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "numeric-region-prepare", "PrepareNumericEncodeRegions", localError));
                return false;
            }
            return true;
        })) { return false; }
    if (!owner->Seal(error)) { return false; }
    state.runs = writable;
    state.owner = std::move(owner);
    return true;
}

inline bool ComputeNumericEncodeBlock(
    const numericarray::NumericArrayBlockParams& sourceParams, const NumericEncodeBlockInput& block,
    const numericarray::PreparedRegionPrecision& preparedPrecision,
    NumericEncodeBlockOutput& output, ScratchByteBufferPool& scratchBytePool,
    const bool collectTiming, std::string* error, numericarray::NumericArrayCompressorState* compressorState) {
    auto params = sourceParams;
    params.capacitySamples = output.capacitySamples ? &*output.capacitySamples : nullptr;
    if (params.capacitySamples != nullptr) {
        params.capacitySamples->Observe(numericarray::NumericBufferSample::Raw, block.raw.Bytes());
    }
    const auto* regionControl = params.regionControl;
    const auto blockElementOffset = block.elementOffset;
    const auto blockElementCount = block.elementCount;
    const auto& rawBlockBytes = block.raw;
    output.payload = scratchBytePool.Acquire(0u);
    auto& componentBundleBytes = output.payload.Bytes();
    auto& header = output.header;
    auto& layout = output.layout;
    NumericArrayBytesCodec blockBytesCodec{NumericArrayBytesCodec::NumericArrayCodec};
    const auto encodeStartTime = callback::StartTiming(collectTiming);
    bool encodedBlockOk = false;
    if (regionControl->regions.empty()) {
        const auto& defaultCompressor = regionControl->defaultPrecision.compressor;
        std::vector<NumericArrayComponentLayoutParams> componentLayouts;
        encodedBlockOk = numericarray::ResolveEncodedNumericArrayBlockBytes(
                params,
                defaultCompressor,
                blockElementCount,
                rawBlockBytes.Span(),
                componentBundleBytes,
                blockBytesCodec,
                error,
                &scratchBytePool,
                &componentLayouts, compressorState);
        if (!encodedBlockOk) {
            return false;
        }
        if (componentBundleBytes.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return validation::AssignError(error, "numeric array block bundle exceeds current block format");
        }
        header = NumericArrayBlockHeader{
            .mode = NumericArrayBlockMode::NonReference,
            .referenceKind = NumericArrayReferenceKind::None,
            .codecId = NumericArrayReferenceCodecId::NonReference,
            .localParentFieldIndex = 0xFFFFu,
            .elementOffset = blockElementOffset,
            .elementCount = blockElementCount,
            .encodedByteLength = static_cast<std::uint32_t>(componentBundleBytes.size()),
            .bytesCodec = blockBytesCodec,
        };
        layout = MakeNumericArrayBlockLayoutParams(header, {}, {});
        layout.componentLayouts = std::move(componentLayouts);
        layout.backgroundCompressor = defaultCompressor;
    } else {
        encodedBlockOk = numericarray::ResolveEncodedLayeredResidualNumericArrayBlockBytes(
                params,
                preparedPrecision,
                blockElementOffset,
                blockElementCount,
                rawBlockBytes.Span(),
                block.regionRuns,
                componentBundleBytes,
                blockBytesCodec,
                layout,
                error,
                &scratchBytePool, compressorState);
        if (!encodedBlockOk) {
            return false;
        }
        if (!MakeNumericArrayBlockHeader(layout, header, error)) {
            return false;
        }
    }
    if (collectTiming) { output.encodeDuration = callback::ElapsedNanoseconds(encodeStartTime); }
    output.rawBytes = rawBlockBytes.Bytes().size();
    output.rawCapacity = VectorCapacityBytes(rawBlockBytes.Bytes());
    return true;
}

inline bool BuildNumericArrayTransferCache(
    const numericarray::NumericArrayBlockParams& params,
    const numericarray::NumericArrayReader& reader,
    DataCodecExecutionResources& resources,
    NumericArrayTransferCacheResult& result,
    bytestore::ByteStoreSession& byteStoreSession,
    std::string* error = nullptr,
    const std::string& storeLabel = "numeric_array_transfer",
    const NumericArrayTransferCacheRuntime* runtime = nullptr) {
    result = {};

    if (!numericarray::ValidateNumericArrayBlockParams(params, error)) {
        return false;
    }
    auto phase = WaitForHeavyPhase(resources);
    if (!phase) { return false; }
    const auto* regionControl = params.regionControl;
    if (regionControl == nullptr) {
        return validation::AssignError(error, "numeric array transfer cache requires region precision control");
    }

    const auto componentCount = params.componentCount;
    std::size_t checkedTupleBytes = 0u;
    if (!validation::CheckedMulSizeT(
            componentCount,
            params.valueSize,
            checkedTupleBytes,
            "numeric array tuple bytes",
            error)) {
        return false;
    }

    const auto elementCount = reader.source.layout.elementCount;
    if (elementCount > std::numeric_limits<std::uint32_t>::max()) {
        return validation::AssignError(error, "numeric array element count exceeds uint32 field capacity");
    }
    if (elementCount != 0u && checkedTupleBytes == 0u) {
        return validation::AssignError(error, "numeric array transfer cache requires a non-empty tuple layout");
    }
    NumericEncodeRegionState regions;
    if (!PrepareNumericEncodeRegions(params, elementCount, resources, *phase, byteStoreSession, regions, error)) {
        return false;
    }
    auto bodyTransferCache = bytestore::CreateAppendableByteStore(byteStoreSession, storeLabel, error);
    if (!bodyTransferCache) { return false; }
    result.stats.logicalRawBytes = validation::SaturatingMulU64(elementCount, checkedTupleBytes);
    if (elementCount == 0u) {
        if (!bodyTransferCache->Seal(error)) { return false; }
        result.transferCache = std::move(bodyTransferCache);
        return FinalizeNumericArrayTransferCacheResult(result, error);
    }
    const bool collectTiming = runtime != nullptr && runtime->recordFloatingPointEncodeDuration &&
        !numericarray::IsIntegerNumericArrayDataType(params.dataType);
    NumericEncodeCursor cursor{reader, regions.runs};
    const bool parallelRead = runtime != nullptr && runtime->parallelInputRead && reader.SupportsParallelMemoryRead();
    bytestore::AppendableByteStoreWriter transferWriter(bodyTransferCache, resources);
    std::size_t committedElements = 0u;
    phase.reset();
    resources.SetWorkType({.path = ResourceWorkPath::NumericEncode,
        .codec = numericarray::IsIntegerNumericArrayDataType(params.dataType) ? 1u : 0u,
        .scalar = static_cast<std::uint32_t>(params.dataType),
        .components = static_cast<std::uint32_t>(params.componentCount),
        .blockElements = numericarray::kSpatialBlockElementCount,
        .referencePath = params.regionControl->regions.empty() ? 0u :
            static_cast<std::uint32_t>(NumericArrayBlockMode::LayeredResidual)});
    const bool success = RunOrderedBlocks<NumericEncodeBlockInput, NumericEncodeBlockOutput>(
        resources,
        [&] { return cursor.HasMore(); },
        [&](NumericEncodeBlockInput& block) {
            if (runtime != nullptr && runtime->recordCapacitySamples) { block.capacitySamples.emplace(); }
            return cursor.ReadNext(block, resources.Scratch(), error, !parallelRead);
        },
        [&](NumericEncodeBlockInput& block, NumericEncodeBlockOutput& output, WorkerContext& worker) {
            std::string localError;
            if (parallelRead) {
                auto* sample = block.capacitySamples ? &block.capacitySamples->values[
                    static_cast<std::size_t>(numericarray::NumericBufferSample::ReaderOrder)] : nullptr;
                if (!reader.ReadElements(block.elementOffset, block.elementCount, worker.Scratch(), block.raw,
                        &localError, sample)) {
                    resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                        "numeric-block-read", "BuildNumericArrayTransferCache", localError));
                    return false;
                }
            }
            output.capacitySamples = block.capacitySamples;
            if (!ComputeNumericEncodeBlock(params, block, regions.precision, output,
                    worker.Scratch(), collectTiming, &localError, &worker.NumericCompressor())) {
                resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "numeric-block-encode", "ComputeNumericEncodeBlock", localError));
                return false;
            }
            return true;
        },
        [&](NumericEncodeBlockOutput& block) {
            if (block.header.elementOffset != committedElements ||
                block.header.elementCount > elementCount - committedElements) {
                return validation::AssignError(error, "numeric block does not match the next commit range");
            }
            if (!WriteNumericArrayBlock(transferWriter, block.header, {}, {}, block.payload.Span(), error)) {
                return false;
            }
            result.blockLayouts.push_back(std::move(block.layout));
            committedElements += block.header.elementCount;
            // 最后一块的 Seal 属于提交动作，原槽位覆盖全部结果消费
            if (committedElements == elementCount && !bodyTransferCache->Seal(error)) { return false; }
            auto& stats = result.stats;
            stats.maxEncodeBlockRawBytes = std::max(stats.maxEncodeBlockRawBytes, block.rawBytes);
            stats.maxEncodeBlockResidentBytes = std::max(stats.maxEncodeBlockResidentBytes, block.rawCapacity);
            stats.maxBlockResidentBytes = std::max(stats.maxBlockResidentBytes,
                block.rawCapacity + VectorCapacityBytes(block.payload.Bytes()));
            stats.maxEncodedBlockBytes = std::max<std::uint64_t>(stats.maxEncodedBlockBytes, block.payload.Bytes().size());
            ++stats.encodeBlockCount;
            if (block.capacitySamples && runtime != nullptr && runtime->recordCapacitySamples) {
                try { runtime->recordCapacitySamples(block.capacitySamples->values); }
                catch (...) { resources.RecordDiagnosticExportFailure(); }
            }
            if (collectTiming) { runtime->recordFloatingPointEncodeDuration(block.encodeDuration); }
            return true;
        });
    if (!success) { return false; }
    result.transferCache = std::move(bodyTransferCache);
    return FinalizeNumericArrayTransferCacheResult(result, error);
}

} // namespace encodeimpl
} // namespace datacodec

#endif
