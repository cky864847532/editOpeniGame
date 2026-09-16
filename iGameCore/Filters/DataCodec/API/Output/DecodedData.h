#ifndef DATACODEC_API_OUTPUT_DECODEDDATA_H
#define DATACODEC_API_OUTPUT_DECODEDDATA_H

#include "DataCodec/API/Output/DecodedBuffer.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include <array>
#include <string>
#include <vector>

namespace datacodec {

struct DecodedGeometry {
    DataType dataType{DataType::Float32};
    std::size_t pointCount{0u};
    std::size_t dimension{3u};
    DecodedBuffer values;
};

struct DecodedTopology {
    enum class Kind : std::uint8_t { None, Structured, Connectivity, Polyhedron };
    Kind kind{Kind::None};
    std::size_t cellCount{0u};
    std::array<int, 3u> structuredAxisSize{};
    DecodedBuffer connectivity;
    DecodedBuffer offsets;
    DecodedBuffer cellTypes;
    DecodedBuffer polynomialOrders;
    // 多面体保留逐单元及逐面的计数流，原生封装按需展开
    DecodedBuffer uniqueVertexCounts;
    DecodedBuffer cellFaceCounts;
    DecodedBuffer faceVertexCounts;
    DecodedBuffer cellUniqueVertexIds;
    DecodedBuffer localFaceVertexIds;
};

struct DecodedAttribute {
    std::size_t sourceIndex{0u};
    AttrStorageParams metadata;
    DecodedBuffer values;
};

struct DecodedLeaf {
    BlockPath path;
    std::string name;
    MeshType meshType{MeshType::PointSet};
    std::uint32_t topologyOwnerFrameIndex{0u};
    TopologyOwnershipMode topologyMode{TopologyOwnershipMode::Owned};
    DecodedGeometry geometry;
    DecodedTopology topology;
    std::vector<DecodedAttribute> attributes;
};

struct DecodedBranch {
    BlockPath path;
    std::string name;
};

// 层级和原始数组均属于结果，会话的按需属性状态独立持有
struct DecodedData {
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    std::string rootName;
    std::vector<DecodedBranch> branches;
    std::vector<DecodedLeaf> leaves;
};

} // 命名空间 datacodec
#endif
