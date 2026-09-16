#ifndef DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYEMIT_H
#define DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYEMIT_H

#include "DataCodec/Workflow/Decode/IDecodeAdapter.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedIndexCache.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DecodeStageMemory.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronDecodeMemoryPlan.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamFormat.h"
#include "DataCodec/Common/Views/TopologyViews.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>
namespace datacodec::polyhedron {
struct PolyhedronCellBatchScratch {
    FixedArrayView<IndexType> cellVertexOffsets;
    FixedArrayView<IndexType> cellFaceOffsets;
    FixedArrayView<IndexType> faceVertexOffsets;
    FixedArrayView<IndexType> cellUniqueVertexIds;
    FixedArrayView<IndexType> localFaceVertexIds;
};

inline bool ReadPolyhedronBatchRange(
    const DecodedIndexCache& source, DataCodecExecutionResources& root,
    const std::size_t first, const std::span<IndexType> values, std::string* error) {
    constexpr std::size_t window = kIoWindowBytes / sizeof(IndexType);
    for (std::size_t offset = 0u; offset < values.size();) {
        if (root.Stopped()) { return false; }
        const auto count = std::min(window, values.size() - offset);
        if (!source.ReadRangeInto(first + offset, values.subspan(offset, count), error)) { return false; }
        offset += count;
    }
    return true;
}

inline bool AppendPolyhedronOffset(FixedArrayView<IndexType>& offsets,
    const IndexType count, std::string* error) {
    if (count < 0 || count > std::numeric_limits<IndexType>::max() - offsets.back()) {
        return validation::AssignError(error, "polyhedron batch offset exceeds index capacity");
    }
    offsets.push_back(offsets.back() + count);
    return true;
}

struct PolyhedronBatchOutput {
    PolyhedronBatchRange range;
    PolyhedronCellBatchScratch scratch;
    std::optional<PolyhedronCapacitySamples> capacitySamples;
};

inline bool BuildPolyhedronCellBatch(
    DataCodecExecutionResources& root, const PolyhedronBatchRange& range,
    const DecodedIndexCache& uniqueCounts, const DecodedIndexCache& cellFaces,
    const DecodedIndexCache& faceVertices, const DecodedIndexCache& uniqueIds,
    const DecodedIndexCache& localIds, PolyhedronBatchOutput& output, DecodeBlockWorkspace& workspace, std::string* error) {
    output.range = range;
    auto& scratch = output.scratch;
    const auto memory = MakePolyhedronBatchMemoryLayout(range);
    scratch.cellVertexOffsets = workspace.View<IndexType>(memory.output[0]);
    scratch.cellFaceOffsets = workspace.View<IndexType>(memory.output[1]);
    scratch.faceVertexOffsets = workspace.View<IndexType>(memory.output[2]);
    scratch.cellUniqueVertexIds = workspace.View<IndexType>(memory.output[3]);
    scratch.localFaceVertexIds = workspace.View<IndexType>(memory.output[4]);
    auto counts = workspace.View<IndexType>(memory.counts);
    auto faces = workspace.View<IndexType>(memory.faces);
    counts.resize(range.cellCount); faces.resize(range.cellCount);
    if (!ReadPolyhedronBatchRange(uniqueCounts, root, range.firstCell, counts, error) ||
        !ReadPolyhedronBatchRange(cellFaces, root, range.firstCell, faces, error)) { return false; }
    scratch.cellVertexOffsets.reserve(range.cellCount + 1u);
    scratch.cellFaceOffsets.reserve(range.cellCount + 1u);
    scratch.cellVertexOffsets.push_back(0u);
    scratch.cellFaceOffsets.push_back(0u);
    for (std::size_t cell = 0u; cell < range.cellCount; ++cell) {
        if (!AppendPolyhedronOffset(scratch.cellVertexOffsets, counts[cell], error) ||
            !AppendPolyhedronOffset(scratch.cellFaceOffsets, faces[cell], error)) { return false; }
    }
    const auto faceCount = static_cast<std::size_t>(scratch.cellFaceOffsets.back());
    const auto uniqueCount = static_cast<std::size_t>(scratch.cellVertexOffsets.back());
    if (range.firstFace > faceVertices.Count() || faceCount > faceVertices.Count() - range.firstFace ||
        range.firstUnique > uniqueIds.Count() || uniqueCount > uniqueIds.Count() - range.firstUnique) {
        return validation::AssignError(error, "polyhedron batch exceeds the complete count streams");
    }
    std::size_t offsetCount = 0u, bytes = 0u;
    if (!validation::CheckedAddSizeT(faceCount, 1u, offsetCount, "polyhedron face offsets", error) ||
        !validation::CheckedMulSizeT(offsetCount, sizeof(IndexType), bytes, "polyhedron face offset bytes", error)) { return false; }
    scratch.faceVertexOffsets.reserve(offsetCount);
    scratch.faceVertexOffsets.push_back(0u);
    // 固定窗口扫描当前批次面计数，逐步建立实际偏移
    auto faceWindow = workspace.View<IndexType>(memory.faceWindow);
    faceWindow.resize(faceWindow.capacity());
    for (std::size_t cursor = 0u; cursor < faceCount;) {
        if (root.Stopped()) { return false; }
        const auto count = std::min(faceWindow.size(), faceCount - cursor);
        const auto window = std::span<IndexType>(faceWindow).first(count);
        if (!faceVertices.ReadRangeInto(range.firstFace + cursor, window, error)) { return false; }
        for (const auto value : window) {
            if (!AppendPolyhedronOffset(scratch.faceVertexOffsets, value, error)) { return false; }
        }
        cursor += count;
    }
    const auto localCount = static_cast<std::size_t>(scratch.faceVertexOffsets.back());
    if (faceCount != range.faceCount || uniqueCount != range.uniqueCount || localCount != range.localCount) {
        return validation::AssignError(error, "polyhedron batch differs from admitted shape");
    }
    if (range.firstLocal > localIds.Count() || localCount > localIds.Count() - range.firstLocal ||
        !validation::CheckedMulSizeT(uniqueCount, sizeof(IndexType), bytes, "polyhedron unique id bytes", error) ||
        !validation::CheckedMulSizeT(localCount, sizeof(IndexType), bytes, "polyhedron local id bytes", error)) {
        return validation::AssignError(error, "polyhedron batch id range exceeds capacity");
    }
    scratch.cellUniqueVertexIds.resize(uniqueCount);
    scratch.localFaceVertexIds.resize(localCount);
    if (!ReadPolyhedronBatchRange(uniqueIds, root, range.firstUnique, scratch.cellUniqueVertexIds, error) ||
        !ReadPolyhedronBatchRange(localIds, root, range.firstLocal, scratch.localFaceVertexIds, error)) { return false; }
    if (output.capacitySamples) {
        auto& samples = *output.capacitySamples;
        samples.Observe(PolyhedronBufferSample::CellVertexOffsets, scratch.cellVertexOffsets);
        samples.Observe(PolyhedronBufferSample::CellFaceOffsets, scratch.cellFaceOffsets);
        samples.Observe(PolyhedronBufferSample::FaceVertexOffsets, scratch.faceVertexOffsets);
        samples.Observe(PolyhedronBufferSample::UniqueIds, scratch.cellUniqueVertexIds);
        samples.Observe(PolyhedronBufferSample::LocalIds, scratch.localFaceVertexIds);
        samples.Observe(PolyhedronBufferSample::UniqueCounts, counts);
        samples.Observe(PolyhedronBufferSample::CellFaceCounts, faces);
        samples.Observe(PolyhedronBufferSample::FaceCountWindow, faceWindow);
    }
    return true;
}

inline bool EmitPolyhedronCacheToAdapter(
    const CacheResources& runtime, IDecodeAdapter& adapter,
    const PolyhedronTopologyStreamHeader& header, const DecodedIndexCache& uniqueCounts,
    const DecodedIndexCache& cellFaces, const DecodedIndexCache& faceVertices,
    const DecodedIndexCache& uniqueIds, const DecodedIndexCache& localIds,
    std::uint64_t& batchCount, std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    if (!adapter.SupportsPolyhedronTopology()) {
        return validation::AssignError(error, "decode adapter does not support polyhedron topology");
    }
    if (header.cellCount != uniqueCounts.Count() || header.cellCount != cellFaces.Count() ||
        header.faceCount != faceVertices.Count() || header.uniqueVertexIdCount != uniqueIds.Count() ||
        header.localFaceVertexIdCount != localIds.Count()) {
        return validation::AssignError(error, "polyhedron complete stores do not match their header");
    }
    for (const auto count : {header.cellCount, header.faceCount, header.uniqueVertexIdCount, header.localFaceVertexIdCount}) {
        if (count > std::numeric_limits<std::size_t>::max()) {
            return validation::AssignError(error, "polyhedron stream count exceeds local size capacity");
        }
    }
    auto& root = runtime.Run();
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return false; }
    bool begun = false, ended = false;
    struct Guard {
        IDecodeAdapter& adapter;
        DataCodecExecutionResources& root;
        bool& begun;
        bool& ended;
        ~Guard() {
            if (!begun || ended) { return; }
            ended = true;
            try { (void)adapter.EndPolyhedronTopology(nullptr); }
            catch (...) { RecordExecutionException(root, "polyhedron-adapter-cleanup"); }
        }
    } guard{adapter, root, begun, ended};
    begun = true;
    if (!adapter.BeginPolyhedronTopology(static_cast<std::size_t>(header.cellCount), error)) { return false; }
    batchCount = 0u;
    if (header.cellCount == 0u) {
        if (header.faceCount != 0u || header.uniqueVertexIdCount != 0u || header.localFaceVertexIdCount != 0u) {
            return validation::AssignError(error, "empty polyhedron cells carry nonempty streams");
        }
        ended = true;
        return adapter.EndPolyhedronTopology(error);
    }
    PolyhedronBatchRange cursor, nextRange;
    TopoStorageParams shape;
    shape.cellCount = header.cellCount;
    shape.polyhedronFaceVertexCount = header.faceCount;
    FixedByteBacking descriptionMemory;
    if (!PrepareDecodeStageMemory(root, PolyhedronDescriptionWindowBytes(shape), descriptionMemory, error)) { return false; }
    auto descriptionWindow = std::span<IndexType>(reinterpret_cast<IndexType*>(descriptionMemory.Span().data()),
        descriptionMemory.size / sizeof(IndexType));
    phase.reset();
    root.SetWorkType({.path = ResourceWorkPath::PolyhedronEmit,
        .blockElements = numericarray::kSpatialBlockElementCount});
    return RunOrderedBlocks<PolyhedronBatchRange, PolyhedronBatchOutput>(root,
        [&] { return cursor.firstCell < header.cellCount; },
        [&](PolyhedronBatchRange& input) {
            input = nextRange;
            return true;
        },
        [&](const PolyhedronBatchRange& input, PolyhedronBatchOutput& output, WorkerContext&, DecodeBlockWorkspace& workspace) {
            std::string localError;
            if (recordCapacitySamples) { output.capacitySamples.emplace(); }
            if (!BuildPolyhedronCellBatch(root, input, uniqueCounts, cellFaces, faceVertices,
                    uniqueIds, localIds, output, workspace, &localError)) {
                root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                    "polyhedron-batch-build", "BuildPolyhedronCellBatch", localError));
                return false;
            }
            return true;
        },
        [&](PolyhedronBatchOutput& output) {
            auto& scratch = output.scratch;
            PolyhedronTopologyView batch;
            batch.cellVertexOffsets = scratch.cellVertexOffsets.data();
            batch.cellVertexOffsetCount = scratch.cellVertexOffsets.size();
            batch.cellUniqueVertexIds = scratch.cellUniqueVertexIds.data();
            batch.cellUniqueVertexIdCount = scratch.cellUniqueVertexIds.size();
            batch.cellFaceOffsets = scratch.cellFaceOffsets.data();
            batch.cellFaceOffsetCount = scratch.cellFaceOffsets.size();
            batch.faceVertexOffsets = scratch.faceVertexOffsets.data();
            batch.faceVertexOffsetCount = scratch.faceVertexOffsets.size();
            batch.localFaceVertexIds = scratch.localFaceVertexIds.data();
            batch.localFaceVertexIdCount = scratch.localFaceVertexIds.size();
            if (!adapter.WritePolyhedronCellBatch(output.range.firstCell, batch, error)) { return false; }
            ++batchCount;
            if (output.capacitySamples && recordCapacitySamples) {
                try { recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { root.RecordDiagnosticExportFailure(); }
            }
            cursor.firstCell += output.range.cellCount;
            cursor.firstFace += scratch.faceVertexOffsets.size() - 1u;
            cursor.firstUnique += scratch.cellUniqueVertexIds.size();
            cursor.firstLocal += scratch.localFaceVertexIds.size();
            if (cursor.firstCell != header.cellCount) { return true; }
            if (cursor.firstFace != header.faceCount || cursor.firstUnique != header.uniqueVertexIdCount ||
                cursor.firstLocal != header.localFaceVertexIdCount) {
                return validation::AssignError(error, "polyhedron assembly did not consume every stream");
            }
            ended = true;
            return adapter.EndPolyhedronTopology(error);
        }, true, nullptr, [&] {
            nextRange = cursor;
            const auto reader = [&](const DecodedIndexCache& source) {
                return [&](std::size_t first, std::span<IndexType> values, std::string* detail) {
                    return ReadPolyhedronBatchRange(source, root, first, values, detail);
                };
            };
            if (!DescribePolyhedronBatch(nextRange, header.cellCount, reader(uniqueCounts), reader(cellFaces),
                    reader(faceVertices), descriptionWindow, error) ||
                nextRange.firstFace > header.faceCount || nextRange.faceCount > header.faceCount - nextRange.firstFace ||
                nextRange.firstUnique > header.uniqueVertexIdCount || nextRange.uniqueCount > header.uniqueVertexIdCount - nextRange.firstUnique ||
                nextRange.firstLocal > header.localFaceVertexIdCount || nextRange.localCount > header.localFaceVertexIdCount - nextRange.firstLocal) {
                throw std::invalid_argument("polyhedron batch description exceeds complete stores");
            }
            return MakePolyhedronBatchMemoryLayout(nextRange);
        });
}


} // namespace datacodec::polyhedron

#endif
