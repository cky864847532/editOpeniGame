#ifndef DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONDECODEMEMORYPLAN_H
#define DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONDECODEMEMORYPLAN_H

#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Runtime/Execution/DecodeBlockWorkspace.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"

namespace datacodec::polyhedron {

struct PolyhedronBatchRange {
    std::size_t firstCell{0u}, cellCount{0u}, firstFace{0u}, firstUnique{0u}, firstLocal{0u};
    std::size_t faceCount{0u}, uniqueCount{0u}, localCount{0u};
};
struct PolyhedronBatchMemoryLayout : DecodeBlockMemoryPlan {
    std::array<DecodeMemoryRange, 5u> output;
    DecodeMemoryRange counts, faces, faceWindow;
};
inline PolyhedronBatchMemoryLayout MakePolyhedronBatchMemoryLayout(const PolyhedronBatchRange& range) {
    using Region = DecodeMemoryRegion;
    PolyhedronBatchMemoryLayout plan;
    plan.block = range.firstCell;
    const std::array<std::size_t, 5u> counts{DecodeBlockMemoryPlan::Add(range.cellCount, 1u),
        DecodeBlockMemoryPlan::Add(range.cellCount, 1u), DecodeBlockMemoryPlan::Add(range.faceCount, 1u),
        range.uniqueCount, range.localCount};
    for (std::size_t i = 0u; i < counts.size(); ++i) { plan.output[i] = plan.Append<IndexType>(Region::Output, counts[i]); }
    plan.counts = plan.Append<IndexType>(Region::Temporary, range.cellCount);
    plan.faces = plan.Append<IndexType>(Region::Temporary, range.cellCount);
    plan.faceWindow = plan.Append<IndexType>(Region::Temporary,
        std::min<std::size_t>(range.faceCount, kIoWindowBytes / sizeof(IndexType)));
    (void)plan.TotalBytes();
    return plan;
}

inline std::size_t PolyhedronDescriptionWindowBytes(const TopoStorageParams& topo) {
    return DecodeBlockMemoryPlan::Multiply(sizeof(IndexType), static_cast<std::size_t>(std::min<std::uint64_t>(
        kIoWindowBytes / sizeof(IndexType), std::max(topo.cellCount, topo.polyhedronFaceVertexCount))));
}

// 运行时的缓存游标和独立分析的计数流共用固定批次累加规则
template<class Counts, class Faces, class Vertices>
bool DescribePolyhedronBatch(PolyhedronBatchRange& range, std::uint64_t totalCells,
    Counts&& counts, Faces&& faces, Vertices&& vertices, std::span<IndexType> window, std::string* error) {
    range.cellCount = static_cast<std::size_t>(std::min<std::uint64_t>(numericarray::kSpatialBlockElementCount,
        totalCells - range.firstCell));
    const auto sum = [&](auto&& read, std::size_t first, std::size_t count, std::size_t& total) {
        total = 0u;
        if (count != 0u && window.empty()) { return false; }
        for (std::size_t offset = 0u; offset < count;) {
            const auto n = std::min(window.size(), count - offset);
            auto values = window.first(n);
            if (!read(first + offset, values, error)) { return false; }
            for (auto value : values) {
                if (value < 0) { return false; }
                total = DecodeBlockMemoryPlan::Add(total, static_cast<std::size_t>(value));
            }
            offset += n;
        }
        return total <= std::numeric_limits<IndexType>::max();
    };
    return sum(counts, range.firstCell, range.cellCount, range.uniqueCount) &&
        sum(faces, range.firstCell, range.cellCount, range.faceCount) &&
        sum(vertices, range.firstFace, range.faceCount, range.localCount);
}

inline std::size_t PolyhedronSequentialWorkBytes(const TopoStorageParams& topo) {
    using Region = DecodeMemoryRegion;
    std::size_t peak = 0u;
    const std::array<std::uint64_t, 5u> counts{topo.cellCount, topo.cellCount,
        topo.polyhedronFaceVertexCount, topo.polyhedronVertexCount, topo.cellBufferSize};
    if (topo.polyhedronStreamLayouts.size() != counts.size()) { throw std::invalid_argument("polyhedron stream layout count mismatch"); }
    for (std::size_t i = 0u; i < counts.size(); ++i) {
        DecodeBlockMemoryPlan plan;
        plan.Append<std::uint8_t>(Region::Temporary, std::min<std::uint64_t>(kIoWindowBytes, topo.polyhedronStreamLayouts[i].encodedByteLength));
        plan.Append<IndexType>(Region::Temporary, std::min<std::uint64_t>(kIoWindowBytes / sizeof(IndexType), counts[i]));
        peak = std::max(peak, plan.TotalBytes());
    }
    DecodeBlockMemoryPlan validation;
    validation.Append<IndexType>(Region::Temporary, std::min<std::uint64_t>(kIoWindowBytes / (2u * sizeof(IndexType)), topo.cellCount));
    validation.Append<IndexType>(Region::Temporary, std::min<std::uint64_t>(kIoWindowBytes / (2u * sizeof(IndexType)), topo.cellCount));
    validation.Append<IndexType>(Region::Temporary, std::min<std::uint64_t>(kIoWindowBytes / (2u * sizeof(IndexType)), topo.polyhedronFaceVertexCount));
    return std::max(peak, validation.TotalBytes());
}

}
#endif
