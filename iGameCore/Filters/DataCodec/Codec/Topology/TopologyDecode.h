#ifndef DATACODEC_CODEC_TOPOLOGY_TOPOLOGYDECODE_H
#define DATACODEC_CODEC_TOPOLOGY_TOPOLOGYDECODE_H

#include "DataCodec/API/Adapter/IDecodeTopologyBlockObserver.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyDecode.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyDecodeInputReader.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamDecode.h"
#include "DataCodec/Codec/Topology/TopologyDecodeCacheSink.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

using TopologyDecodeResult = CodecStatus;

struct TopologyDecodeTimingEvent {
    std::string name;
    double elapsedMs{0.0};
    std::string scope;
};

using TopologyDecodeTimingCallback = std::function<void(const TopologyDecodeTimingEvent&)>;

struct TopologyDecodeData {
    const CodecStorageParams& storageParams;
};

struct TopologyDecodeCache {
    const CacheResources& cacheResources;
    bytestore::ByteStoreSession& byteStoreSession;
    DecodedTopologyCache& topology;
};

struct TopologyDecodeContext {
    TopologyDecodeTimingCallback timingCallback;
    std::shared_ptr<IDecodeTopologyBlockObserver> topologyBlockObserver;
    callback::CapacityCallback recordCapacitySamples;
};

struct TopologyDecodeRuntime {
    TopologyDecodeData data;
    TopologyDecodeCache cache;
    TopologyDecodeContext context;
};

namespace detail {

inline TopologyDecodeResult MakeTopologyDecodeSuccess() {
    return CodecStatus::Ok();
}

inline TopologyDecodeResult MakeTopologyDecodeFailure(
    const CodecErrorCode code,
    std::string message) {
    return CodecStatus::Failure(code, std::move(message));
}

inline TopologyDecodeResult ValidateTopologyConsumed(
    const std::uint64_t position,
    const std::uint64_t expected,
    std::string* error) {
    if (!validation::ValidateExactConsumed(position, expected, "topology field", error)) {
        return MakeTopologyDecodeFailure(
            CodecErrorCode::DecodeFailure,
            error != nullptr ? *error : "topology field bytes were not consumed exactly");
    }
    return MakeTopologyDecodeSuccess();
}

inline void RecordTopologyDecodeTiming(
    const TopologyDecodeTimingCallback& timingCallback,
    std::string name,
    const callback::PhaseTimePoint startTime,
    std::string scope = {}) {
    callback::InvokeTimingEvent(timingCallback, TopologyDecodeTimingEvent{
        std::move(name),
        callback::ElapsedMilliseconds(startTime),
        std::move(scope),
    });
}

inline void ForwardConnectivityTopologyDecodeTiming(
    const TopologyDecodeTimingCallback& timingCallback,
    const topocodec::ConnectivityTopologyDecodeTimingEvent& event) {
    if (!timingCallback) {
        return;
    }
    timingCallback(TopologyDecodeTimingEvent{
        "TopoDecodeCoreStage." + event.name,
        event.elapsedMs,
        event.scope,
    });
}

inline topocodec::ConnectivityTopologyEncodedMetadata MakeConnectivityTopologyEncodedMetadata(
    const TopologyConnectivityBlockLayoutParams& block) noexcept {
    topocodec::ConnectivityTopologyEncodedMetadata metadata;
    metadata.connectivityByteCount = block.connectivityByteCount;
    metadata.cellSizeByteCount = block.cellSizeByteCount;
    metadata.cellPolynomialOrderByteCount = block.cellPolynomialOrderByteCount;
    metadata.cellTypeByteCount = block.cellTypeByteCount;
    return metadata;
}

struct TopologyBlockInput {
    std::size_t index{0u};
    topocodec::ConnectivityTopologyDecodeInputReader encoded;
};

struct TopologyBlockOutput {
    std::size_t index{0u};
    topocodec::PreparedConnectivityDecodedBlock decoded;
    topocodec::ConnectivityDecodeMemoryLayout memory;
    double computeMs{0.0};
    std::optional<topocodec::TopologyBlockCapacitySamples> capacitySamples;
};

template<typename TValue, typename TWrite>
inline bool WriteTopologyBlockWindows(DataCodecExecutionResources& root,
    const std::span<const TValue> values, const std::size_t base, TWrite&& write) {
    constexpr auto window = kIoWindowBytes / sizeof(TValue);
    for (std::size_t offset = 0u; offset < values.size();) {
        if (root.Stopped()) { return false; }
        const auto count = std::min<std::size_t>(window, values.size() - offset);
        if (!write(base + offset, values.subspan(offset, count))) { return false; }
        offset += count;
    }
    return true;
}

template<typename TStream>
inline bool DecodeConnectivityTopologyBlocksToCache(
    const TopoStorageParams& topo, TopologyDecodeRuntime& runtime,
    TStream& stream, std::string* error = nullptr) {
    auto& root = runtime.cache.cacheResources.Run();
    const auto& blocks = topo.connectivityLayout.blockLayouts;
    if (blocks.empty()) { return validation::AssignError(error, "connectivity topology block layout is empty"); }
    std::size_t cells = 0u, indices = 0u, points = 0u;
    if (!TryParamSizeToSizeT(topo.cellCount, cells) || !TryParamSizeToSizeT(topo.cellBufferSize, indices) ||
        !TryParamSizeToSizeT(runtime.data.storageParams.geomParams.elementCount, points)) {
        return validation::AssignError(error, "topology counts exceed local size capacity");
    }
    std::uint64_t nextCell = 0u, nextIndex = 0u, payloadBytes = 0u;
    bool singleRecord = false, hasOrders = false;
    for (const auto& layout : blocks) {
        if (layout.cellOffset != nextCell || layout.connectivityOffset != nextIndex ||
            !validation::CheckedAddU64(nextCell, layout.cellCount, nextCell, "topology block cells", error) ||
            !validation::CheckedAddU64(nextIndex, layout.connectivityCount, nextIndex, "topology block indices", error) ||
            nextCell > cells || nextIndex > indices) {
            return validation::AssignError(error, "topology blocks do not form contiguous ranges");
        }
        for (const auto bytes : {layout.connectivityByteCount, layout.cellSizeByteCount,
                layout.cellPolynomialOrderByteCount, layout.cellTypeByteCount}) {
            if (!validation::CheckedAddU64(payloadBytes, bytes, payloadBytes, "topology input payload", error)) { return false; }
        }
        std::size_t checkedBytes = 0u, offsetCount = 0u;
        if (!validation::CheckedAddSizeT(static_cast<std::size_t>(layout.cellCount), 1u,
                offsetCount, "topology block offset count", error) ||
            !validation::CheckedMulSizeT(offsetCount, sizeof(IndexType), checkedBytes, "topology block offsets", error) ||
            !validation::CheckedMulSizeT(static_cast<std::size_t>(layout.connectivityCount), sizeof(IndexType),
                checkedBytes, "topology block connectivity", error)) { return false; }
        singleRecord |= layout.cellCount > numericarray::kSpatialBlockElementCount;
        hasOrders |= layout.cellPolynomialOrderByteCount != 0u;
    }
    if (nextCell != cells || nextIndex != indices || payloadBytes != topo.binaryCount) {
        return validation::AssignError(error, "topology block totals do not match field metadata");
    }
    const bool hasOffsets = topo.fixedCellSize <= 0;
    const bool hasTypes = topo.hasCellTypes != 0u;
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return false; }
    auto& cache = runtime.cache.topology;
    bool completed = false;
    struct OutputGuard {
        DecodedTopologyCache& cache;
        bool& completed;
        ~OutputGuard() { if (!completed) { cache.Release(); } }
    } outputGuard{cache, completed};
    topology::CacheTopologyDecodeSink sink(cache, runtime.cache.byteStoreSession);
    if (!sink.BeginConnectivityTopology(cells, indices, hasOffsets, hasTypes, hasOrders, error)) { return false; }
    auto observer = runtime.context.topologyBlockObserver;
    bool observerStarted = false, observerEnded = false;
    struct ObserverGuard {
        std::shared_ptr<IDecodeTopologyBlockObserver>& observer;
        DataCodecExecutionResources& root;
        bool& started;
        bool& ended;
        ~ObserverGuard() {
            if (!observer || !started || ended) { return; }
            ended = true;
            try { (void)observer->EndConnectivityTopology(nullptr); }
            catch (...) { RecordExecutionException(root, "topology-observer-cleanup"); }
        }
    } observerGuard{observer, root, observerStarted, observerEnded};
    if (observer) {
        observerStarted = true;
        if (!observer->BeginConnectivityTopology({blocks.size(), points, cells,
                static_cast<int>(topo.fixedCellSize), hasOffsets, hasTypes}, error)) { return false; }
    }
    std::size_t cursor = 0u, committed = 0u;
    const auto begin = stream.Position();
    phase.reset();
    root.SetWorkType({.path = ResourceWorkPath::ConnectivityDecode,
        .blockElements = numericarray::kSpatialBlockElementCount});
    topocodec::ConnectivityDecodeMemoryLayout nextMemory;
    completed = RunOrderedBlocks<TopologyBlockInput, TopologyBlockOutput>(root,
        [&] { return cursor < blocks.size(); },
        [&](TopologyBlockInput& input, const SlotLease& slot, DecodeBlockWorkspace& workspace) {
            input.index = cursor++;
            // 顺序字段的外层解压复用原槽位并取得计算额度
            return RunTerminalWork(root, slot, [&](WorkerContext&) {
                return input.encoded.LoadFrom(stream, MakeConnectivityTopologyEncodedMetadata(blocks[input.index]),
                    runtime.cache.cacheResources, workspace, nextMemory, error);
            });
        },
        [&](const TopologyBlockInput& input, TopologyBlockOutput& output, WorkerContext&, DecodeBlockWorkspace& workspace) {
            const auto& shape = blocks[input.index];
            output.memory = topocodec::MakeConnectivityDecodeMemoryLayout(shape, static_cast<int>(topo.fixedCellSize), hasTypes);
            auto& memory = output.memory;
            output.decoded.connectivity = workspace.View<IndexType>(memory.connectivity);
            output.decoded.offsets = workspace.View<IndexType>(memory.offsets);
            output.decoded.cellTypes = workspace.View<IndexType>(memory.types);
            output.decoded.polynomialOrders = workspace.View<std::uint16_t>(memory.orders);
            auto scratchBytes = workspace.View<std::uint8_t>(memory.scratch);
            scratchBytes.resize(scratchBytes.capacity());
            ArrayWorkspace scratch(scratchBytes.Span());
            output.index = input.index;
            if (runtime.context.recordCapacitySamples) {
                output.capacitySamples.emplace();
                input.encoded.ObserveCapacities(*output.capacitySamples);
            }
            const auto& layout = blocks[input.index];
            const auto start = callback::StartTiming(static_cast<bool>(runtime.context.timingCallback));
            std::string localError;
            if (!topocodec::DecodeConnectivityTopologyBlock(input.encoded, points,
                    static_cast<std::size_t>(layout.cellCount), static_cast<std::size_t>(layout.connectivityCount),
                    static_cast<int>(topo.fixedCellSize), hasTypes, output.decoded, &localError, {},
                    output.capacitySamples ? &*output.capacitySamples : nullptr, &scratch)) {
                root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                    "topology-block-decode", "DecodeConnectivityTopologyBlock", localError));
                return false;
            }
            output.computeMs = callback::ElapsedMilliseconds(start);
            return true;
        },
        [&](TopologyBlockOutput& output, DecodeBlockWorkspace& workspace) {
            if (output.index != committed) { return validation::AssignError(error, "topology blocks committed out of order"); }
            const auto& layout = blocks[output.index];
            auto& data = output.decoded;
            const auto cellBase = static_cast<std::size_t>(layout.cellOffset);
            const auto indexBase = static_cast<std::size_t>(layout.connectivityOffset);
            if (!WriteTopologyBlockWindows<IndexType>(root, data.connectivity, indexBase,
                    [&](auto offset, auto values) { return sink.WriteConnectivityRange(offset, values, error); }) ||
                !WriteTopologyBlockWindows<IndexType>(root, data.cellTypes, cellBase,
                    [&](auto offset, auto values) { return sink.WriteCellTypesRange(offset, values, error); }) ||
                !WriteTopologyBlockWindows<std::uint16_t>(root, data.polynomialOrders, cellBase,
                    [&](auto offset, auto values) { return sink.WriteCellPolynomialOrdersRange(offset, values, error); })) { return false; }
            if (hasOffsets) {
                const auto first = cellBase == 0u ? 0u : 1u;
                auto scratchBytes = workspace.View<std::uint8_t>(output.memory.scratch);
                scratchBytes.resize(scratchBytes.capacity());
                ArrayWorkspace scratch(scratchBytes.Span());
                auto adjusted = scratch.Take<IndexType>(std::min<std::size_t>(kIoWindowBytes / sizeof(IndexType), data.offsets.size() - first));
                adjusted.resize(std::min<std::size_t>(kIoWindowBytes / sizeof(IndexType), data.offsets.size() - first));
                if (output.capacitySamples) {
                    output.capacitySamples->Observe(topocodec::TopologyBufferSample::AdjustedOffsets, adjusted);
                }
                for (std::size_t offset = first; offset < data.offsets.size();) {
                    if (root.Stopped()) { return false; }
                    const auto count = std::min(adjusted.size(), data.offsets.size() - offset);
                    for (std::size_t i = 0u; i < count; ++i) {
                        if (indexBase > std::numeric_limits<IndexType>::max() ||
                            data.offsets[offset + i] > std::numeric_limits<IndexType>::max() - indexBase) {
                            return validation::AssignError(error, "topology global offset exceeds index capacity");
                        }
                        adjusted[i] = static_cast<IndexType>(indexBase + data.offsets[offset + i]);
                    }
                    if (!sink.WriteOffsetsRange(cellBase + offset, std::span<const IndexType>(adjusted).first(count), error)) { return false; }
                    offset += count;
                }
            }
            if (observer && !observer->ObserveConnectivityBlock({output.index, cellBase,
                    static_cast<int>(topo.fixedCellSize), workspace.TransferOutput(), data.connectivity.Span(),
                    data.offsets.Span(), data.cellTypes.Span()}, error)) { return false; }
            if (output.capacitySamples && runtime.context.recordCapacitySamples) {
                try { runtime.context.recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { root.RecordDiagnosticExportFailure(); }
            }
            if (runtime.context.timingCallback) {
                runtime.context.timingCallback({"TopoDecodeCoreStage.connectivity.block_decode",
                    output.computeMs, "block=" + std::to_string(output.index)});
            }
            if (++committed != blocks.size()) { return true; }
            if (!validation::ValidateExactConsumed(stream.Position() - begin, topo.binaryCount,
                    "topology blocks", error) || !sink.EndConnectivityTopology(error)) { return false; }
            if (observer) {
                observerEnded = true;
                if (!observer->EndConnectivityTopology(error)) { return false; }
            }
            return true;
        }, singleRecord, nullptr, [&] {
            nextMemory = topocodec::MakeConnectivityDecodeMemoryLayout(blocks[cursor],
                static_cast<int>(topo.fixedCellSize), hasTypes);
            return nextMemory;
        });
    return completed;
}

template<typename TStream>
inline bool DecodeConnectivityTopologyStreamToCache(
    const TopoStorageParams& topo, TopologyDecodeRuntime& runtime,
    TStream& stream, std::string* error = nullptr) {
    return DecodeConnectivityTopologyBlocksToCache(topo, runtime, stream, error);
}


} // namespace detail

inline bool DecodeStructuredTopologyToCache(
    const CodecStorageParams& storageParams,
    DecodedTopologyCache& topology) {
    if (!storageParams.topoParams.isStructured) {
        return false;
    }
    topology.InitializeStructured(storageParams.structuredMeshParams.axisSize);
    return true;
}

template<typename TStream>
inline TopologyDecodeResult DecodeTopologyFieldToCache(
    TopologyDecodeRuntime& decodeRuntime,
    TStream& stream) {
    const auto& storageParams = decodeRuntime.data.storageParams;
    const auto& cacheResources = decodeRuntime.cache.cacheResources;
    auto& byteStoreSession = decodeRuntime.cache.byteStoreSession;
    auto& topology = decodeRuntime.cache.topology;
    const auto& timingCallback = decodeRuntime.context.timingCallback;
    const auto begin = stream.Position();
    std::uint64_t end = 0u;
    std::string error;
    const auto rangeStart = callback::StartTiming(timingCallback);
    if (!validation::CheckedAddU64(
            begin,
            storageParams.topoParams.binaryCount,
            end,
            "topology field range",
            &error)) {
        return detail::MakeTopologyDecodeFailure(
            CodecErrorCode::DecodeFailure,
            error);
    }
    detail::RecordTopologyDecodeTiming(
        timingCallback,
        "TopoDecodeCoreStage.range",
        rangeStart,
        "begin=" + std::to_string(begin) +
            ";bytes=" + std::to_string(storageParams.topoParams.binaryCount));

    if (DecodeStructuredTopologyToCache(storageParams, topology)) {
        return detail::ValidateTopologyConsumed(stream.Position(), end, &error);
    }

    if (storageParams.topoParams.isPolyhedron != 0u) {
        const auto polyhedronStart = callback::StartTiming(timingCallback);
        if (!polyhedron::DecodePolyhedronTopologyStreamsToCache(
                cacheResources,
                byteStoreSession,
                topology,
                storageParams.topoParams,
                stream,
                &error, decodeRuntime.context.recordCapacitySamples)) {
            return detail::MakeTopologyDecodeFailure(
                CodecErrorCode::InvalidTopology,
                "failed to decode polyhedron topology streams: " + error);
        }
        detail::RecordTopologyDecodeTiming(
            timingCallback,
            "TopoDecodeCoreStage.polyhedron.decode",
            polyhedronStart,
            "cells=" + std::to_string(storageParams.topoParams.cellCount) +
                ";binaryBytes=" + std::to_string(storageParams.topoParams.binaryCount));
        return detail::ValidateTopologyConsumed(stream.Position(), end, &error);
    }
    if (!detail::DecodeConnectivityTopologyStreamToCache(
            storageParams.topoParams,
            decodeRuntime,
            stream,
            &error)) {
        return detail::MakeTopologyDecodeFailure(
            CodecErrorCode::PipelineFailure,
            "failed to decode topology stream: " + error);
    }
    return detail::ValidateTopologyConsumed(stream.Position(), end, &error);
}

} // namespace datacodec

#endif
