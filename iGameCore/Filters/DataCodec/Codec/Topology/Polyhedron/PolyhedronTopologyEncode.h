#ifndef DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYENCODE_H
#define DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYENCODE_H

#include "DataCodec/API/Adapter/IEncodeAdapter.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/TransferCache/Common/TopologyTransferCache.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Codec/Remap/RemapOrderSource.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamEncoder.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyRemap.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

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
#include <string_view>
#include <vector>
namespace datacodec::polyhedron {

inline constexpr std::size_t kSmallCellLinearLookupThreshold = 24u;

struct PolyhedronCellWorkWorkspace {
    std::span<IndexType> localIndexTable;
    std::vector<IndexType> overflowPointIds;
    std::vector<IndexType> overflowLocalIds;

    void Prepare(const std::size_t overflowReserve) {
        overflowPointIds.clear();
        overflowLocalIds.clear();
        overflowPointIds.reserve(overflowReserve);
        overflowLocalIds.reserve(overflowReserve);
    }
};

struct PolyhedronCellWorkBuffer {
    std::vector<IndexType> uniqueVertexIds;
    std::vector<IndexType> faceVertexCounts;
    std::vector<IndexType> localFaceVertexIds;

    void Clear() noexcept {
        uniqueVertexIds.clear();
        faceVertexCounts.clear();
        localFaceVertexIds.clear();
    }
};

struct PolyhedronTopologyData {
    const IEncodeAdapter& adapter;
    const RemapOrderSource& pointInverseOrderSource;
    const RemapOrderSource& cellOrderSource;
};

struct PolyhedronTopologyExecution {
    DataCodecExecutionResources& resources;
};

struct PolyhedronTopologyCache {
    bytestore::ByteStoreSession& byteStoreSession;
};

struct PolyhedronTopologyContext {
    std::function<void(double)> progressCallback;
    std::function<void(std::string_view, double)> phaseTimingCallback;
    callback::CapacityCallback recordCapacitySamples;
};

struct PolyhedronTopologyEncodeInput {
    PolyhedronTopologyData data;
    PolyhedronTopologyExecution execution;
    PolyhedronTopologyCache cache;
    PolyhedronTopologyContext context;
};

struct PolyhedronTopologyEncodeResult {
    TopoStorageParams topo;
    std::shared_ptr<bytestore::IByteSource> transferCache;
};

inline void InvokePolyhedronTopologyProgress(
    const PolyhedronTopologyEncodeInput& input,
    const double normalized) {
    callback::InvokeProgress(input.context.progressCallback, normalized);
}

inline void AppendPolyhedronWorkPointLinear(
    PolyhedronCellWorkBuffer& workBuffer,
    const IndexType pointId) {
    for (std::size_t localIndex = 0; localIndex < workBuffer.uniqueVertexIds.size(); ++localIndex) {
        if (workBuffer.uniqueVertexIds[localIndex] == pointId) {
            workBuffer.localFaceVertexIds.push_back(static_cast<IndexType>(localIndex));
            return;
        }
    }

    const auto localId = static_cast<IndexType>(workBuffer.uniqueVertexIds.size());
    workBuffer.uniqueVertexIds.push_back(pointId);
    workBuffer.localFaceVertexIds.push_back(localId);
}

inline void AppendPolyhedronWorkPointHash(
    PolyhedronCellWorkBuffer& workBuffer,
    PolyhedronCellWorkWorkspace& workspace,
    const IndexType pointId) {
    if (static_cast<std::size_t>(pointId) < workspace.localIndexTable.size()) {
        const auto storedLocalId = workspace.localIndexTable[pointId];
        if (storedLocalId != std::numeric_limits<IndexType>::max() &&
            static_cast<std::size_t>(storedLocalId) < workBuffer.uniqueVertexIds.size() &&
            workBuffer.uniqueVertexIds[storedLocalId] == pointId) {
            workBuffer.localFaceVertexIds.push_back(storedLocalId);
            return;
        }

        const auto localId = static_cast<IndexType>(workBuffer.uniqueVertexIds.size());
        workspace.localIndexTable[pointId] = localId;
        workBuffer.uniqueVertexIds.push_back(pointId);
        workBuffer.localFaceVertexIds.push_back(localId);
        return;
    }

    for (std::size_t index = 0; index < workspace.overflowPointIds.size(); ++index) {
        if (workspace.overflowPointIds[index] == pointId) {
            workBuffer.localFaceVertexIds.push_back(workspace.overflowLocalIds[index]);
            return;
        }
    }

    const auto localId = static_cast<IndexType>(workBuffer.uniqueVertexIds.size());
    workspace.overflowPointIds.push_back(pointId);
    workspace.overflowLocalIds.push_back(localId);
    workBuffer.uniqueVertexIds.push_back(pointId);
    workBuffer.localFaceVertexIds.push_back(localId);
}

// [DC防护:领域] polyhedron 编码前校验局部拓扑偏移、引用范围和面单元关系
inline bool ValidateAdapterPolyhedronTopology(
    const IEncodeAdapter& adapter,
    const IndexType* cellFaceIds,
    const IndexType* cellFaceOffsets,
    const IndexType* faceVertexIds,
    const IndexType* faceVertexOffsets,
    const IRemapProvider* pointRemapInverse,
    const std::span<std::uint8_t> visitedFaces,
    bool& needsLocalIndexTable,
    DataCodecExecutionResources& root,
    std::string* error = nullptr) {
    needsLocalIndexTable = false;
    const auto cellCount = adapter.GetNumberOfCells();
    const auto faceCount = adapter.GetNumberOfFaces();
    const auto pointCount = adapter.GetNumberOfPoints();
    const auto cellFaceIdCount = adapter.GetCellFaceBufferSize();
    const auto faceVertexIdCount = adapter.GetFaceIdBufferSize();
    if (cellCount == 0u) {
        return true;
    }
    if (faceCount == 0u || pointCount == 0u) {
        return validation::AssignError(error, "polyhedron adapter topology has cells without faces or points");
    }
    if ((cellFaceIdCount != 0u && cellFaceIds == nullptr) ||
        (faceVertexIdCount != 0u && faceVertexIds == nullptr)) {
        return validation::AssignError(error, "polyhedron adapter topology id buffer is missing");
    }

    std::size_t finalCellFaceOffset = 0u;
    std::size_t finalFaceVertexOffset = 0u;
    if (!ValidatePolyhedronOffsetRange(
            cellFaceOffsets,
            cellCount + 1u,
            cellFaceIdCount,
            "polyhedron adapter cell-face offsets",
            finalCellFaceOffset,
            error) ||
        !ValidatePolyhedronOffsetRange(
            faceVertexOffsets,
            faceCount + 1u,
            faceVertexIdCount,
            "polyhedron adapter face-vertex offsets",
            finalFaceVertexOffset,
            error)) {
        return false;
    }
    (void)finalCellFaceOffset;
    (void)finalFaceVertexOffset;

    if (visitedFaces.size() != faceCount) {
        return validation::AssignError(error, "polyhedron visited-face owner has an unexpected size");
    }
    std::fill(visitedFaces.begin(), visitedFaces.end(), 0u);
    for (std::size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        if (root.Stopped()) { return false; }
        std::size_t cellVertexReferences = 0u;
        const auto faceBegin = static_cast<std::size_t>(cellFaceOffsets[cellIndex]);
        const auto faceEnd = static_cast<std::size_t>(cellFaceOffsets[cellIndex + 1u]);
        if (faceEnd <= faceBegin) {
            return validation::AssignError(error, "polyhedron adapter topology contains a cell without faces");
        }

        for (std::size_t faceCursor = faceBegin; faceCursor < faceEnd; ++faceCursor) {
            const auto faceId = static_cast<std::size_t>(cellFaceIds[faceCursor]);
            if (faceId >= faceCount) {
                return validation::AssignError(
                    error,
                    "polyhedron adapter topology contains an out-of-range face id");
            }
            const auto vertexBegin = static_cast<std::size_t>(faceVertexOffsets[faceId]);
            const auto vertexEnd = static_cast<std::size_t>(faceVertexOffsets[faceId + 1u]);
            if (vertexEnd < vertexBegin || vertexEnd - vertexBegin < 3u) {
                return validation::AssignError(
                    error,
                    "polyhedron adapter topology contains a face with fewer than three vertices");
            }
            if (!validation::CheckedAddSizeT(cellVertexReferences, vertexEnd - vertexBegin,
                    cellVertexReferences, "polyhedron cell vertex references", error)) { return false; }
            needsLocalIndexTable |= cellVertexReferences > kSmallCellLinearLookupThreshold;
            if (visitedFaces[faceId] != 0u) { continue; }
            for (std::size_t vertexCursor = vertexBegin; vertexCursor < vertexEnd; ++vertexCursor) {
                IndexType translatedPointId = 0u;
                if (!TryTranslateCellLocalPointId(
                        faceVertexIds[vertexCursor],
                        pointCount,
                        pointRemapInverse,
                        translatedPointId,
                        error)) {
                    return false;
                }
            }
            visitedFaces[faceId] = 1u;
        }
    }
    for (const auto visited : visitedFaces) {
        if (visited == 0u) {
            return validation::AssignError(error, "polyhedron adapter topology contains an unreferenced face");
        }
    }
    return true;
}

inline bool BuildAdapterPolyhedronCellWorkBuffer(
    PolyhedronCellWorkBuffer& workBuffer,
    const std::span<const IndexType> faceIds,
    const IndexType* faceVertexIds,
    const IndexType* faceVertexOffsets,
    const std::size_t faceCount,
    const std::size_t pointCount,
    const IRemapProvider* pointRemapInverse,
    PolyhedronCellWorkWorkspace& workspace,
    std::string* error = nullptr) {
    workBuffer.Clear();

    std::size_t totalFaceVertexCount = 0u;
    for (const auto rawFaceId : faceIds) {
        if (rawFaceId < 0 || static_cast<std::size_t>(rawFaceId) >= faceCount) {
            return validation::AssignError(
                error,
                "polyhedron adapter topology contains an out-of-range face id");
        }
        const auto faceId = static_cast<std::size_t>(rawFaceId);
        const auto start = static_cast<std::size_t>(faceVertexOffsets[faceId]);
        const auto end = static_cast<std::size_t>(faceVertexOffsets[faceId + 1u]);
        if (end < start || end - start < 3u) {
            return validation::AssignError(
                error,
                "polyhedron adapter topology contains an invalid face-vertex range");
        }
        if (!validation::CheckedAddSizeT(
                totalFaceVertexCount,
                end - start,
                totalFaceVertexCount,
                "polyhedron adapter total face vertex count",
                error)) {
            return false;
        }
    }

    workBuffer.faceVertexCounts.reserve(faceIds.size());
    workBuffer.localFaceVertexIds.reserve(totalFaceVertexCount);
    workBuffer.uniqueVertexIds.reserve(std::min<std::size_t>(totalFaceVertexCount, pointCount));
    if (totalFaceVertexCount > kSmallCellLinearLookupThreshold) {
        const auto expected = pointRemapInverse == nullptr ? pointCount : pointRemapInverse->Size();
        if (workspace.localIndexTable.size() != expected) {
            return validation::AssignError(error, "polyhedron local index table was not prepared before block admission");
        }
        workspace.Prepare(totalFaceVertexCount);
    }

    for (const auto rawFaceId : faceIds) {
        const auto faceId = static_cast<std::size_t>(rawFaceId);
        const auto faceLocalBegin = workBuffer.localFaceVertexIds.size();
        const auto start = static_cast<std::size_t>(faceVertexOffsets[faceId]);
        const auto end = static_cast<std::size_t>(faceVertexOffsets[faceId + 1u]);
        for (std::size_t cursor = start; cursor < end; ++cursor) {
            IndexType pointId = 0u;
            if (!TryTranslateCellLocalPointId(
                    faceVertexIds[cursor],
                    pointCount,
                    pointRemapInverse,
                    pointId,
                    error)) {
                return false;
            }
            if (totalFaceVertexCount <= kSmallCellLinearLookupThreshold) {
                AppendPolyhedronWorkPointLinear(workBuffer, pointId);
            } else {
                AppendPolyhedronWorkPointHash(workBuffer, workspace, pointId);
            }
        }
        workBuffer.faceVertexCounts.push_back(
            static_cast<IndexType>(workBuffer.localFaceVertexIds.size() - faceLocalBegin));
    }
    return true;
}

inline bool CompletePolyhedronTopologyStreamSpooler(
    topology::PolyhedronTopologyStreamSpooler& streams,
    const PolyhedronTopologyStreamStats& stats,
    std::string* error = nullptr) {
    return streams.CompleteStream(
            PolyhedronTopologyStreamKind::UniqueVertexCounts,
            PolyhedronTopologyStreamCodec::Varint,
            stats.cellCount,
            0u,
            error) &&
        streams.CompleteStream(
            PolyhedronTopologyStreamKind::CellFaceCounts,
            PolyhedronTopologyStreamCodec::Varint,
            stats.cellCount,
            0u,
            error) &&
        streams.CompleteStream(
            PolyhedronTopologyStreamKind::FaceVertexCounts,
            PolyhedronTopologyStreamCodec::Varint,
            stats.faceCount,
            0u,
            error) &&
        streams.CompleteStream(
            PolyhedronTopologyStreamKind::CellUniqueVertexIds,
            PolyhedronTopologyStreamCodec::Varint,
            stats.uniqueVertexIdCount,
            0u,
            error) &&
        streams.CompleteStream(
            PolyhedronTopologyStreamKind::LocalFaceVertexIds,
            PolyhedronTopologyStreamCodec::SegmentedBitpack,
            stats.localFaceVertexIdCount,
            0u,
            error);
}

inline void UpdatePolyhedronStorageParamsFromStreamStats(
    TopoStorageParams& topo,
    const PolyhedronTopologyStreamStats& stats) {
    topo.cellBufferSize = stats.localFaceVertexIdCount;
    topo.polyhedronVertexCount = stats.uniqueVertexIdCount;
    topo.polyhedronFaceVertexCount = stats.faceCount;
}

struct PolyhedronEncodeBatchResult {
    std::size_t nextCell{0u};
    std::optional<PolyhedronCapacitySamples> capacitySamples;
};

inline bool EncodePolyhedronTopologyToTransferCache(
    const PolyhedronTopologyEncodeInput& input,
    PolyhedronTopologyEncodeResult& result, std::string* error = nullptr) {
    result = {};
    auto& root = input.execution.resources;
    auto& session = input.cache.byteStoreSession;
    const auto& adapter = input.data.adapter;
    if (!adapter.IsPolyhedronMesh()) { return validation::AssignError(error, "adapter is not a polyhedron mesh"); }
    const auto cells = adapter.GetNumberOfCells();
    const auto faces = adapter.GetNumberOfFaces();
    const auto points = adapter.GetNumberOfPoints();
    const auto* cellFaceIds = adapter.GetCellFaceBufferPtr();
    const auto* cellFaceOffsets = adapter.GetCellFaceOffsetPtr();
    const auto* faceVertexIds = adapter.GetFaceIdBufferPtr();
    const auto* faceVertexOffsets = adapter.GetFaceIdOffsetPtr();
    const auto* inverse = input.data.pointInverseOrderSource.Provider();
    const auto* order = input.data.cellOrderSource.Provider();
    std::size_t checked = 0u;
    if (!validation::CheckedAddSizeT(cells, 1u, checked, "polyhedron cell offset count", error) ||
        !validation::CheckedAddSizeT(faces, 1u, checked, "polyhedron face offset count", error)) { return false; }
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return false; }
    bool needsTable = false;
    auto visited = std::dynamic_pointer_cast<bytestore::MemoryStore>(
        session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, faces,
            "polyhedron_visited_faces", error));
    if (!visited) { return false; }
    if (!RunTerminalWork(root, *phase, [&](WorkerContext&) {
            auto bytes = visited->WritableBytes();
            return ValidateAdapterPolyhedronTopology(adapter, cellFaceIds, cellFaceOffsets,
                faceVertexIds, faceVertexOffsets, inverse, bytes, needsTable, root, error);
        })) { return false; }
    visited.reset();
    std::shared_ptr<bytestore::MemoryStore> tableOwner;
    std::span<IndexType> table;
    if (needsTable) {
        const auto count = inverse == nullptr ? points : inverse->Size();
        if (count != points || !validation::CheckedMulSizeT(count, sizeof(IndexType), checked,
                "polyhedron local index table bytes", error)) {
            return validation::AssignError(error, "polyhedron remap point domain does not match the adapter");
        }
        tableOwner = std::dynamic_pointer_cast<bytestore::MemoryStore>(
            session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, checked, "polyhedron_local_index", error));
        if (!tableOwner) { return false; }
        table = {reinterpret_cast<IndexType*>(tableOwner->WritableBytes().data()), count};
        if (!RunTerminalWork(root, *phase, [&](WorkerContext&) {
                std::fill(table.begin(), table.end(), std::numeric_limits<IndexType>::max());
                return true;
            })) { return false; }
    }
    auto streams = topology::MakePolyhedronTopologyStreamSpooler(session, root, error);
    if (!streams) { return false; }
    std::optional<PolyhedronTopologyStreamEncoder> encoder;
    const auto recordStreamCapacities = [&] {
        if (!input.context.recordCapacitySamples) { return; }
        PolyhedronCapacitySamples samples;
        encoder->ObserveCapacities(samples);
        try { input.context.recordCapacitySamples(samples.values); }
        catch (...) { root.RecordDiagnosticExportFailure(); }
    };
    TopoStorageParams topo;
    topo.cellCount = cells;
    topo.isPolyhedron = true;
    const auto finishStreams = [&] {
        if (!encoder->Finish(error)) { return false; }
        const auto stats = encoder->Stats();
        if (stats.cellCount != cells) {
            return validation::AssignError(error, "polyhedron encoded cell count does not match its input");
        }
        if (!CompletePolyhedronTopologyStreamSpooler(*streams, stats, error)) { return false; }
        UpdatePolyhedronStorageParamsFromStreamStats(topo, stats);
        if (!topology::ExportPolyhedronTopologyStreamLayouts(*streams, topo.polyhedronStreamLayouts, error)) { return false; }
        auto source = topology::BuildPolyhedronTopologyStreamTransferCache(*streams, error);
        if (!source) { return false; }
        topo.binaryCount = source->ByteSizeHint();
        result.topo = std::move(topo);
        result.transferCache = std::move(source);
        encoder.reset();
        table = {};
        tableOwner.reset();
        InvokePolyhedronTopologyProgress(input, 0.98);
        return true;
    };
    const auto finish = [&] {
        if (finishStreams()) { return true; }
        root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
            "polyhedron-stream-finalize", "EncodePolyhedronTopologyToTransferCache",
            error != nullptr && !error->empty() ? std::string_view(*error) : "polyhedron stream finalization failed"));
        return false;
    };
    if (cells == 0u) {
        encoder.emplace(*streams);
        recordStreamCapacities();
        return finish();
    }
    std::size_t cursor = 0u;
    phase.reset();
    root.SetWorkType({.path = ResourceWorkPath::PolyhedronEncode,
        .blockElements = numericarray::kSpatialBlockElementCount});
    const bool success = RunOrderedBlocks<std::size_t, PolyhedronEncodeBatchResult>(root,
        [&] { return cursor < cells; },
        [&](std::size_t& first) {
            first = cursor;
            if (!encoder) {
                encoder.emplace(*streams);
                recordStreamCapacities();
            }
            return true;
        },
        [&](const std::size_t first, PolyhedronEncodeBatchResult& output, WorkerContext&) {
            if (input.context.recordCapacitySamples) { output.capacitySamples.emplace(); }
            const auto count = std::min<std::size_t>(numericarray::kSpatialBlockElementCount, cells - first);
            PolyhedronCellWorkWorkspace workspace{.localIndexTable = table};
            PolyhedronCellWorkBuffer work;
            std::string localError;
            const auto failed = [&] {
                root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "polyhedron-cell-encode", "EncodePolyhedronTopologyToTransferCache", localError));
                return false;
            };
            for (std::size_t cell = first; cell < first + count; ++cell) {
                if (root.Stopped()) { return false; }
                std::size_t oldCell = 0u;
                if (!TryResolveOrderedCellIndex(order, cells, cell, oldCell, &localError)) { return failed(); }
                const auto begin = static_cast<std::size_t>(cellFaceOffsets[oldCell]);
                const auto end = static_cast<std::size_t>(cellFaceOffsets[oldCell + 1u]);
                if (!BuildAdapterPolyhedronCellWorkBuffer(work, {cellFaceIds + begin, end - begin},
                        faceVertexIds, faceVertexOffsets, faces, points, inverse, workspace, &localError) ||
                    !encoder->AppendCell({work.uniqueVertexIds, work.faceVertexCounts, work.localFaceVertexIds}, &localError)) {
                    return failed();
                }
                if (output.capacitySamples) {
                    auto& samples = *output.capacitySamples;
                    samples.Observe(PolyhedronBufferSample::UniqueIds, work.uniqueVertexIds);
                    samples.Observe(PolyhedronBufferSample::FaceVertexCounts, work.faceVertexCounts);
                    samples.Observe(PolyhedronBufferSample::LocalIds, work.localFaceVertexIds);
                    samples.Observe(PolyhedronBufferSample::OverflowPoints, workspace.overflowPointIds);
                    samples.Observe(PolyhedronBufferSample::OverflowLocals, workspace.overflowLocalIds);
                }
            }
            output.nextCell = first + count;
            return true;
        },
        [&](PolyhedronEncodeBatchResult& output) {
            cursor = output.nextCell;
            if (output.capacitySamples && input.context.recordCapacitySamples) {
                try { input.context.recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { root.RecordDiagnosticExportFailure(); }
            }
            InvokePolyhedronTopologyProgress(input, 0.05 + 0.82 * static_cast<double>(cursor) / cells);
            return cursor != cells || finish();
        }, true);
    if (!success) { result = {}; }
    return success;
}

} // namespace datacodec::polyhedron

#endif
