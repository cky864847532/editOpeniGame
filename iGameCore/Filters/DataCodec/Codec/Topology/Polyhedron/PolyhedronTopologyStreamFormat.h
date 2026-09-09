#ifndef DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYSTREAMFORMAT_H
#define DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYSTREAMFORMAT_H

#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace datacodec::polyhedron {

enum class PolyhedronBufferSample : std::size_t {
    UniqueIds,
    FaceVertexCounts,
    LocalIds,
    OverflowPoints,
    OverflowLocals,
    CellVertexOffsets,
    CellFaceOffsets,
    FaceVertexOffsets,
    UniqueCounts,
    CellFaceCounts,
    FaceCountWindow,
    StreamReadWindow,
    IndexWriteWindow,
    StreamUniqueCounts,
    StreamUniqueIds,
    StreamCellFaces,
    StreamFaceVertices,
    StreamLocalIds,
    Count,
};

// 固定清单覆盖实际自有数组，流与批次各自保存独立取样身份
struct PolyhedronCapacitySamples {
    std::array<BufferCapacitySample, static_cast<std::size_t>(PolyhedronBufferSample::Count)> values{{
        BufferCapacitySample{"polyhedron.unique_ids"},
        BufferCapacitySample{"polyhedron.face_vertex_counts"},
        BufferCapacitySample{"polyhedron.local_ids"},
        BufferCapacitySample{"polyhedron.overflow_points"},
        BufferCapacitySample{"polyhedron.overflow_locals"},
        BufferCapacitySample{"polyhedron.cell_vertex_offsets"},
        BufferCapacitySample{"polyhedron.cell_face_offsets"},
        BufferCapacitySample{"polyhedron.face_vertex_offsets"},
        BufferCapacitySample{"polyhedron.unique_counts"},
        BufferCapacitySample{"polyhedron.cell_face_counts"},
        BufferCapacitySample{"polyhedron.face_count_window"},
        BufferCapacitySample{"polyhedron.stream_read_window"},
        BufferCapacitySample{"polyhedron.index_write_window"},
        BufferCapacitySample{"polyhedron.stream.unique_counts"},
        BufferCapacitySample{"polyhedron.stream.unique_ids"},
        BufferCapacitySample{"polyhedron.stream.cell_faces"},
        BufferCapacitySample{"polyhedron.stream.face_vertices"},
        BufferCapacitySample{"polyhedron.stream.local_ids"},
    }};

    template<class T>
    void Observe(const PolyhedronBufferSample kind, const T& storage) noexcept {
        values[static_cast<std::size_t>(kind)].Observe(storage);
    }
};

enum class PolyhedronTopologyStreamKind : std::uint8_t {
    UniqueVertexCounts = 0,
    CellUniqueVertexIds = 1,
    CellFaceCounts = 2,
    FaceVertexCounts = 3,
    LocalFaceVertexIds = 4,
};

enum class PolyhedronTopologyStreamCodec : std::uint16_t {
    // 保留 v1 wire tag，0 曾用于已移除的 Raw 编码
    Varint = 1,
    SegmentedBitpack = 2,
};

inline const char* PolyhedronTopologyStreamKindName(const PolyhedronTopologyStreamKind kind) {
    switch (kind) {
        case PolyhedronTopologyStreamKind::UniqueVertexCounts:
            return "UniqueVertexCounts";
        case PolyhedronTopologyStreamKind::CellUniqueVertexIds:
            return "CellUniqueVertexIds";
        case PolyhedronTopologyStreamKind::CellFaceCounts:
            return "CellFaceCounts";
        case PolyhedronTopologyStreamKind::FaceVertexCounts:
            return "FaceVertexCounts";
        case PolyhedronTopologyStreamKind::LocalFaceVertexIds:
            return "LocalFaceVertexIds";
    }
    return "Unknown";
}

struct PolyhedronTopologyStreamHeader {
    std::uint16_t flags{0};
    std::uint32_t streamCount{0};
    std::uint64_t cellCount{0};
    std::uint64_t faceCount{0};
    std::uint64_t uniqueVertexIdCount{0};
    std::uint64_t localFaceVertexIdCount{0};
};

struct PolyhedronTopologyStreamSchedule {
    PolyhedronTopologyStreamKind kind{PolyhedronTopologyStreamKind::UniqueVertexCounts};
    PolyhedronTopologyStreamCodec codec{PolyhedronTopologyStreamCodec::Varint};
    std::uint16_t flags{0};
    std::uint16_t reserved{0};
    std::uint32_t meshIndexPadding{0};
    std::uint64_t elementCount{0};
    std::uint64_t auxiliaryStreamByteSize{0};
    std::uint64_t streamByteSize{0};
};

inline constexpr std::uint16_t kPolyhedronTopologyStreamFlagHasAuxBytes = 1u << 0;

inline constexpr std::array<PolyhedronTopologyStreamKind, 5> kStatefulPolyhedronTopologyStreamOrder{
    PolyhedronTopologyStreamKind::UniqueVertexCounts,
    PolyhedronTopologyStreamKind::CellFaceCounts,
    PolyhedronTopologyStreamKind::FaceVertexCounts,
    PolyhedronTopologyStreamKind::CellUniqueVertexIds,
    PolyhedronTopologyStreamKind::LocalFaceVertexIds,
};

inline std::size_t StatefulPolyhedronTopologyStreamIndex(const PolyhedronTopologyStreamKind kind) noexcept {
    for (std::size_t index = 0; index < kStatefulPolyhedronTopologyStreamOrder.size(); ++index) {
        if (kStatefulPolyhedronTopologyStreamOrder[index] == kind) {
            return index;
        }
    }
    return kStatefulPolyhedronTopologyStreamOrder.size();
}

} // namespace datacodec::polyhedron

#endif
