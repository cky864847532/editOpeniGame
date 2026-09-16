#ifndef iGameDataCodeciGameDecodeAdapter_h
#define iGameDataCodeciGameDecodeAdapter_h


#include "DataCodec/API/Output/DecodedData.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"


#include "DataCodec/Filter/Adapter/iGameCellTypeMapping.h"
#include "DataCodec/Common/Views/TopologyViews.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include "iGameAttributeSet.h"
#include "iGameCell.h"
#include "iGameCellArray.h"
#include "iGameCellType.h"
#include "iGameFlatArray.h"
#include "iGameFaceTable.h"
#include "iGameIdArray.h"
#include "iGameLagrangeUnstructuredMesh.h"
#include "iGamePointSet.h"
#include "iGameStructuredMesh.h"
#include "iGameSurfaceMesh.h"
#include "iGameType.h"
#include "iGameUnstructuredMesh.h"
#include "iGameVolumeMesh.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include <span>
#include <string>
#include <utility>
#include <vector>

IGAME_NAMESPACE_BEGIN

class iGameDecodeAdapter final {
public:
    using IndexType = ::datacodec::IndexType;
    using MeshType = ::datacodec::MeshType;
    using PolyhedronTopologyView = ::datacodec::PolyhedronTopologyView;
    using AttrStorageParams = ::datacodec::AttrStorageParams;
    using DataType = ::datacodec::DataType;
    using AttrRole = ::datacodec::AttrRole;
    using AttrAttachment = ::datacodec::AttrAttachment;
    using CellTypeRaw = ::datacodec::CellTypeRaw;
    using CellTypeCodecEntry = ::datacodec::CellTypeCodecEntry;
    using CellTypeMappingMode = ::datacodec::CellTypeMappingMode;
    using CellTypeFamilyCode = ::datacodec::CellTypeFamilyCode;
    using CellTypeLocalCode = ::datacodec::CellTypeLocalCode;

    iGameDecodeAdapter() = default;
    explicit iGameDecodeAdapter(DataObject::Pointer output) : m_output(std::move(output)) {}

    // 在核心完成之后接管结果，原生封装不参与解码执行
    bool Import(const ::datacodec::DecodedLeaf& leaf, bool attributesOnly = false,
                std::string* error = nullptr) {
        if (!attributesOnly) {
            if (!SetMeshType(leaf.meshType, error)) { return false; }
            if (leaf.geometry.dataType != DataType::Float32 || leaf.geometry.dimension != 3u) {
                return Fail(error, "iGame Points requires Float32 geometry with three components");
            }
            auto points = Points::New();
            if (!AdoptTyped(points->ConvertToArray(), leaf.geometry.values, 3, error)) { return false; }
            auto pointSet = DynamicCast<PointSet>(m_output);
            if (!pointSet) { return Fail(error, "decoded geometry requires a point set"); }
            pointSet->SetPoints(points);
            const auto& topology = leaf.topology;
            using Kind = ::datacodec::DecodedTopology::Kind;
            if (topology.kind == Kind::Connectivity) {
                m_pendingCellCount = topology.cellCount;
                m_pendingConnectivityCount = topology.connectivity.size() / sizeof(IndexType);
                m_pendingConnectivityIds = IdArray::New();
                if (!AdoptTyped(m_pendingConnectivityIds, topology.connectivity, 1, error)) { return false; }
                if (!topology.offsets.empty()) {
                    m_pendingOffsets = UnsignedIntArray::New();
                    if (!AdoptTyped(m_pendingOffsets, topology.offsets, 1, error)) { return false; }
                }
                if (!topology.cellTypes.empty()) {
                    m_cellTypes = UnsignedIntArray::New();
                    if (!AdoptTyped(m_cellTypes, topology.cellTypes, 1, error)) { return false; }
                }
                if (!topology.polynomialOrders.empty() && !WriteCellPolynomialOrdersRange(0u,
                    reinterpret_cast<const std::uint16_t*>(topology.polynomialOrders.data()),
                    topology.polynomialOrders.size() / sizeof(std::uint16_t), error)) { return false; }
                if (!EndTopology(error)) { return false; }
            } else if (topology.kind == Kind::Structured) {
                if (!SetStructuredAxisSize(topology.structuredAxisSize.data(), error)) { return false; }
            } else if (topology.kind == Kind::Polyhedron) {
                if (!ImportPolyhedron(topology, error)) { return false; }
            }
        }
        for (const auto& attribute : leaf.attributes) {
            const auto index = attribute.sourceIndex;
            if (NativeAttributeIndex(m_output, index) >= 0) { continue; }
            auto array = CreateArray(attribute.metadata.dataType);
            if (!array || !AdoptAttribute(array, attribute, error)) { return false; }
            array->SetName(attribute.metadata.name);
            if (index >= m_pendingAttributes.size()) {
                m_pendingAttributes.resize(index + 1u);
                m_pendingAttributeMeta.resize(index + 1u);
            }
            m_pendingAttributes[index] = array;
            m_pendingAttributeMeta[index] = attribute.metadata;
            if (!EndAttribute(index, error)) { return false; }
        }
        return Commit(error);
    }

    // 为解码后的 mesh type 创建目标原生网格对象
    bool SetMeshType(MeshType type, std::string* error = nullptr) {
        ReleaseOutputState();
        m_meshType = type;
        m_output = DataObject::CreateDataObject(ToNativeMeshType(type));
        if (m_output == nullptr) {
            return Fail(error, "failed to create iGame output object");
        }
        return true;
    }

    // 开始写入普通拓扑 range
    bool BeginTopology(
        const std::size_t cellCount,
        const std::size_t connectivityCount,
        const bool hasOffsets,
        std::string* error = nullptr) {
        m_pendingCellCount = cellCount;
        m_pendingConnectivityCount = connectivityCount;
        m_pendingConnectivityIds = IdArray::New();
        if (m_pendingConnectivityIds == nullptr) {
            return Fail(error, "failed to allocate native connectivity array");
        }
        m_pendingConnectivityIds->SetNumberOfIds(connectivityCount);
        m_pendingOffsets = nullptr;
        if (hasOffsets) {
            m_pendingOffsets = UnsignedIntArray::New();
            if (m_pendingOffsets == nullptr) {
                return Fail(error, "failed to allocate native topology offset array");
            }
            m_pendingOffsets->Resize(cellCount + 1u);
        }

        m_cellArray = nullptr;
        m_cellTypes = nullptr;
        ReleasePolynomialOrders();
        return true;
    }

    // 写入 cell polynomial order range
    bool WriteCellPolynomialOrdersRange(
        const std::size_t offset,
        const std::uint16_t* data,
        const std::size_t count,
        std::string* error = nullptr) {
        if (count == 0u) {
            return true;
        }
        if (data == nullptr) {
            return Fail(error, "cell polynomial order range write requires input data");
        }
        if (offset > m_pendingCellCount || count > m_pendingCellCount - offset) {
            return Fail(error, "cell polynomial order range write is outside the target cells");
        }
        if (m_cellPolynomialOrders.empty()) {
            m_cellPolynomialOrders.resize(m_pendingCellCount, 0);
            m_polynomialOrdersCapacity.Observe(m_cellPolynomialOrders);
        }
        std::copy(data, data + count, m_cellPolynomialOrders.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    // 结束普通拓扑 range 写入
    bool EndTopology(std::string* error = nullptr) {
        if (m_pendingCellCount != 0u &&
            (m_pendingConnectivityIds == nullptr || m_pendingConnectivityCount == 0u)) {
            return Fail(error, "topology commit requires connectivity data");
        }
        m_cellArray = CellArray::New();
        if (m_pendingCellCount != 0u && m_cellArray == nullptr) {
            return Fail(error, "failed to build iGame cell array");
        }
        if (m_cellArray != nullptr && m_pendingCellCount != 0u) {
            if (m_pendingOffsets != nullptr) {
                m_cellArray->SetData(m_pendingConnectivityIds, m_pendingOffsets);
            } else {
                const auto fixedCellSize = static_cast<int>(
                    m_pendingConnectivityCount / m_pendingCellCount);
                if (fixedCellSize <= 0) {
                    return Fail(error, "topology commit has an invalid fixed cell size");
                }
                m_cellArray->SetData(m_pendingConnectivityIds, fixedCellSize);
            }
        }
        CommitCellArray();
        ReleasePolynomialOrders();
        m_pendingConnectivityIds = nullptr;
        m_pendingOffsets = nullptr;
        m_pendingConnectivityCount = 0u;
        m_pendingCellCount = 0u;
        return true;
    }

    // 写入 structured mesh 的轴尺寸
    bool SetStructuredAxisSize(const int size[3], std::string* error = nullptr) {
        const auto structuredMesh = DynamicCast<StructuredMesh>(m_output);
        if (structuredMesh == nullptr || size == nullptr) {
            return Fail(error, "structured axis size requires structured output and input size");
        }

        igIndex axisSize[3]{
            static_cast<igIndex>(size[0]),
            static_cast<igIndex>(size[1]),
            static_cast<igIndex>(size[2]),
        };
        structuredMesh->SetDimensionSize(axisSize);
        structuredMesh->GenStructuredCellConnectivities();
        return true;
    }

    // 判断当前目标 mesh 是否支持 polyhedron 组装
    bool SupportsPolyhedronTopology() const {
        return m_meshType == MeshType::VolumeMesh || m_meshType == MeshType::UnstructuredMesh ||
            m_meshType == MeshType::PolyhedronMesh;
    }

    // 开始按 chunk 写入 polyhedron 拓扑
    bool BeginPolyhedronTopology(const std::size_t cellCount, std::string* error = nullptr) {
        if (!SupportsPolyhedronTopology()) {
            return Fail(error, "target mesh does not support polyhedron topology");
        }
        m_polyCellCount = cellCount;
        m_polyFaceTable = nullptr;
        m_polyCellFaces = nullptr;
        m_polyCells = nullptr;
        m_polyCellTypes = nullptr;
        if (m_meshType == MeshType::VolumeMesh) {
            m_polyFaceTable = FaceTable::New();
            m_polyCellFaces = CellArray::New();
            if (m_polyFaceTable == nullptr || m_polyCellFaces == nullptr) {
                return Fail(error, "failed to allocate volume polyhedron buffers");
            }
            return true;
        }
        m_polyCells = CellArray::New();
        m_polyCellTypes = UnsignedIntArray::New();
        if (m_polyCells == nullptr || m_polyCellTypes == nullptr) {
            return Fail(error, "failed to allocate unstructured polyhedron buffers");
        }
        m_polyCellTypes->Resize(cellCount);
        std::fill(
            m_polyCellTypes->RawPointer(),
            m_polyCellTypes->RawPointer() + cellCount,
            static_cast<unsigned int>(IG_POLYHEDRON));
        return true;
    }

    // 写入一段 polyhedron 局部拓扑
    bool WritePolyhedronCellBatch(
        const std::size_t firstCell,
        const PolyhedronTopologyView& batch,
        std::string* error = nullptr) {
        if (!SupportsPolyhedronTopology() || !HasPolyhedronData(batch)) {
            return Fail(error, "polyhedron batch requires supported target and valid batch data");
        }
        if (firstCell >= m_polyCellCount && m_polyCellCount != 0u) {
            return Fail(error, "polyhedron batch first cell is outside the target cell count");
        }
        if (m_meshType == MeshType::VolumeMesh) {
            AppendVolumePolyhedronChunk(batch);
            return true;
        }
        AppendUnstructuredPolyhedronChunk(batch);
        return true;
    }

    // 结束 polyhedron chunk 写入
    bool EndPolyhedronTopology(std::string* error = nullptr) {
        if (!SupportsPolyhedronTopology()) {
            return Fail(error, "target mesh does not support polyhedron topology");
        }
        if (m_meshType == MeshType::VolumeMesh) {
            const auto volumeMesh = DynamicCast<VolumeMesh>(m_output);
            if (volumeMesh != nullptr && m_polyFaceTable != nullptr && m_polyCellFaces != nullptr) {
                volumeMesh->InitVolumesWithPolyhedron(m_polyFaceTable->GetOutput(), m_polyCellFaces);
                m_cellArray = volumeMesh->GetCellArray();
                m_cellTypes = nullptr;
                ReleasePolynomialOrders();
            } else {
                return Fail(error, "failed to commit volume polyhedron topology");
            }
        } else if (m_polyCells != nullptr && m_polyCellTypes != nullptr) {
            m_cellArray = m_polyCells;
            m_cellTypes = m_polyCellTypes;
            ReleasePolynomialOrders();
            CommitCellArray();
        } else {
            return Fail(error, "failed to commit unstructured polyhedron topology");
        }
        m_polyFaceTable = nullptr;
        m_polyCellFaces = nullptr;
        m_polyCells = nullptr;
        m_polyCellTypes = nullptr;
        m_polyCellCount = 0u;
        return true;
    }

    // 结束属性 range 写入并挂接到属性集
    bool EndAttribute(const std::size_t attrIndex, std::string* error = nullptr) {
        if (m_output == nullptr ||
            m_output->GetAttributeSet() == nullptr ||
            attrIndex >= m_pendingAttributes.size() ||
            attrIndex >= m_pendingAttributeMeta.size() ||
            m_pendingAttributes[attrIndex] == nullptr) {
            return Fail(error, "attribute end requires a pending attribute");
        }

        const auto& meta = m_pendingAttributeMeta[attrIndex];
        const auto nativeIndex = m_output->GetAttributeSet()->AddAttribute(
            ToNativeAttrRole(meta.type),
            ToNativeAttrAttachment(meta.attachmentType),
            m_pendingAttributes[attrIndex]);
        if (nativeIndex < 0) {
            return Fail(error, "failed to append decoded attribute to the output object");
        }
        if (attrIndex >= m_nativeAttributeIndices.size()) {
            m_nativeAttributeIndices.resize(attrIndex + 1u, -1);
        }
        m_nativeAttributeIndices[attrIndex] = static_cast<int>(nativeIndex);
        m_output->GetMetadata()->AddInt("DataCodec.AttributeIndex." + std::to_string(attrIndex),
            static_cast<int>(nativeIndex));
        m_pendingAttributes[attrIndex] = nullptr;
        return true;
    }

    // 提交组装后的 iGame 输出对象
    bool Commit(std::string* error = nullptr) {
        if (m_failed) {
            return Fail(error, m_failureMessage.empty() ? "decode adapter is in failed state" : m_failureMessage);
        }
        CommitCellArray();
        if (m_output == nullptr) {
            return Fail(error, "decode adapter has no output object");
        }
        return true;
    }

    [[nodiscard]] DataObject::Pointer TakeDataObject() const { return m_output; }

    [[nodiscard]] int NativeAttributeIndex(const std::size_t attrIndex) const noexcept {
        return attrIndex < m_nativeAttributeIndices.size()
            ? m_nativeAttributeIndices[attrIndex]
            : -1;
    }

    // 映射归属于本次输出对象，初始属性与补充属性使用相同发布路径
    [[nodiscard]] static int NativeAttributeIndex(const DataObject::Pointer& output, std::size_t sourceIndex) {
        int index = -1;
        if (output == nullptr || output->GetMetadata() == nullptr || output->GetAttributeSet() == nullptr ||
            !output->GetMetadata()->GetInt("DataCodec.AttributeIndex." + std::to_string(sourceIndex), index) ||
            index < 0 || static_cast<std::size_t>(index) >= output->GetAttributeSet()->GetNumberOfAttributes()) { return -1; }
        return index;
    }


    [[nodiscard]] std::span<const ::datacodec::BufferCapacitySample> CapacitySamples() const noexcept {
        return {&m_polynomialOrdersCapacity, 1u};
    }

    void ResetOutput() {
        ReleaseOutputState();
    }

    void Abort() {
        ReleaseOutputState();
    }

private:
    template<class TArray>
    static bool AdoptTyped(TArray array, const ::datacodec::DecodedBuffer& buffer, int dimension,
                           std::string* error) {
        using Value = std::remove_pointer_t<decltype(array->RawPointer())>;
        if (buffer.size() % sizeof(Value) != 0u) {
            return ::datacodec::validation::AssignError(error, "decoded array has a partial scalar");
        }
        if (buffer.empty() && !buffer.Owner()) {
            if constexpr (requires { array->SetDimension(dimension); }) { array->SetDimension(dimension); }
            return true;
        }
        return array->AdoptArray(buffer.Owner(), reinterpret_cast<Value*>(const_cast<std::uint8_t*>(buffer.data())),
            dimension, buffer.size() / sizeof(Value), buffer.capacity() / sizeof(Value)) ||
            ::datacodec::validation::AssignError(error, "decoded array ownership is invalid");
    }

    static bool AdoptAttribute(ArrayObject::Pointer array, const ::datacodec::DecodedAttribute& attribute,
                               std::string* error) {
        const auto adopt = [&](auto typed) { return AdoptTyped(typed, attribute.values, attribute.metadata.dimension, error); };
        switch (attribute.metadata.dataType) {
            case DataType::Float32: return adopt(DynamicCast<FloatArray>(array));
            case DataType::Float64: return adopt(DynamicCast<DoubleArray>(array));
            case DataType::Int8: return adopt(DynamicCast<CharArray>(array));
            case DataType::UInt8: return adopt(DynamicCast<UnsignedCharArray>(array));
            case DataType::Int16: return adopt(DynamicCast<ShortArray>(array));
            case DataType::UInt16: return adopt(DynamicCast<UnsignedShortArray>(array));
            case DataType::Int32: return adopt(DynamicCast<IntArray>(array));
            case DataType::UInt32: return adopt(DynamicCast<UnsignedIntArray>(array));
            case DataType::Int64: return adopt(DynamicCast<LongLongArray>(array));
            case DataType::UInt64: return adopt(DynamicCast<UnsignedLongLongArray>(array));
            default: return ::datacodec::validation::AssignError(error, "unsupported decoded attribute type");
        }
    }

    bool ImportPolyhedron(const ::datacodec::DecodedTopology& topology, std::string* error) {
        if (!BeginPolyhedronTopology(topology.cellCount, error)) { return false; }
        const auto offsets = [](const ::datacodec::DecodedBuffer& counts) {
            const auto count = counts.size() / sizeof(IndexType);
            const auto* values = reinterpret_cast<const IndexType*>(counts.data());
            std::vector<IndexType> result(count + 1u, 0u);
            for (std::size_t i = 0u; i < count; ++i) {
                if (values[i] > std::numeric_limits<IndexType>::max() - result[i]) {
                    throw std::overflow_error("polyhedron offsets exceed index capacity");
                }
                result[i + 1u] = result[i] + values[i];
            }
            return result;
        };
        const auto vertices = offsets(topology.uniqueVertexCounts);
        const auto faces = offsets(topology.cellFaceCounts);
        const auto faceVertices = offsets(topology.faceVertexCounts);
        const PolyhedronTopologyView view{
            .cellVertexOffsets = vertices.data(), .cellVertexOffsetCount = vertices.size(),
            .cellUniqueVertexIds = reinterpret_cast<const IndexType*>(topology.cellUniqueVertexIds.data()),
            .cellUniqueVertexIdCount = topology.cellUniqueVertexIds.size() / sizeof(IndexType),
            .cellFaceOffsets = faces.data(), .cellFaceOffsetCount = faces.size(),
            .faceVertexOffsets = faceVertices.data(), .faceVertexOffsetCount = faceVertices.size(),
            .localFaceVertexIds = reinterpret_cast<const IndexType*>(topology.localFaceVertexIds.data()),
            .localFaceVertexIdCount = topology.localFaceVertexIds.size() / sizeof(IndexType),
        };
        return WritePolyhedronCellBatch(0u, view, error) && EndPolyhedronTopology(error);
    }

    void ReleaseOutputState() {
        ResetPartialState();
        m_output = nullptr;
    }

    bool Fail(std::string* error, std::string message) {
        m_failed = true;
        m_failureMessage = std::move(message);
        return ::datacodec::validation::AssignError(error, m_failureMessage);
    }

    void ReleasePolynomialOrders() noexcept {
        m_polynomialOrdersCapacity.Observe(m_cellPolynomialOrders);
        ::datacodec::ReleaseVectorStorage(m_cellPolynomialOrders);
        m_polynomialOrdersCapacity.Observe(m_cellPolynomialOrders);
    }

    void ResetPartialState() {
        m_failed = false;
        m_failureMessage.clear();
        m_cellArray = nullptr;
        m_cellTypes = nullptr;
        ReleasePolynomialOrders();
        m_pendingAttributes.clear();
        m_pendingAttributeMeta.clear();
        m_nativeAttributeIndices.clear();
        m_pendingCellCount = 0u;
        m_pendingConnectivityIds = nullptr;
        m_pendingOffsets = nullptr;
        m_pendingConnectivityCount = 0u;
        m_polyFaceTable = nullptr;
        m_polyCellFaces = nullptr;
        m_polyCells = nullptr;
        m_polyCellTypes = nullptr;
        m_polyCellCount = 0u;
    }

    // 仅供 decode adapter 使用的 codec 到 iGame 枚举桥接
    static IGenum ToNativeMeshType(const MeshType meshType) {
        switch (meshType) {
            case MeshType::SurfaceMesh:
                return IG_SURFACE_MESH;
            case MeshType::VolumeMesh:
                return IG_VOLUME_MESH;
            case MeshType::StructuredMesh:
                return IG_STRUCTURED_MESH;
            case MeshType::UnstructuredMesh:
            case MeshType::PolyhedronMesh:
                return IG_UNSTRUCTURED_MESH;
            case MeshType::PointSet:
            default:
                return IG_POINT_SET;
        }
    }

    static IGenum ToNativeAttrRole(const AttrRole attrRole) {
        switch (attrRole) {
            case AttrRole::Vector:
                return IG_VECTOR;
            case AttrRole::Normal:
                return IG_NORMAL;
            case AttrRole::TexCoord:
                return IG_TCOORD;
            case AttrRole::Tensor:
                return IG_TENSOR;
            case AttrRole::Color:
                return IG_RGB;
            case AttrRole::Scalar:
            case AttrRole::Unknown:
            default:
                return IG_SCALAR;
        }
    }

    static IGenum ToNativeAttrAttachment(const AttrAttachment attachmentType) {
        return attachmentType == AttrAttachment::Cell ? IG_CELL : IG_POINT;
    }

    static ArrayObject::Pointer CreateArray(const DataType dataType) {
        switch (dataType) {
            case DataType::Float32:
                return FloatArray::New();
            case DataType::Float64:
                return DoubleArray::New();
            case DataType::Int8:
                return CharArray::New();
            case DataType::UInt8:
                return UnsignedCharArray::New();
            case DataType::Int16:
                return ShortArray::New();
            case DataType::UInt16:
                return UnsignedShortArray::New();
            case DataType::Int32:
                return IntArray::New();
            case DataType::UInt32:
                return UnsignedIntArray::New();
            case DataType::Int64:
                return LongLongArray::New();
            case DataType::UInt64:
                return UnsignedLongLongArray::New();
            default:
                return nullptr;
        }
    }

    // 把解码后的原始字节拷进带类型的原生数组 range
    // 在尝试重建 polyhedron 之前做基础形状校验
    static bool HasPolyhedronData(const PolyhedronTopologyView& topology) {
        return topology.cellVertexOffsets != nullptr &&
            topology.cellVertexOffsetCount > 0 &&
            topology.cellUniqueVertexIds != nullptr &&
            topology.cellFaceOffsets != nullptr &&
            topology.cellFaceOffsetCount > 0 &&
            topology.faceVertexOffsets != nullptr &&
            topology.faceVertexOffsetCount > 0 &&
            topology.localFaceVertexIds != nullptr;
    }

    // 把当前准备好的拓扑缓冲区写入目标原生网格对象
    void CommitCellArray() {
        if (m_output == nullptr || m_cellArray == nullptr) {
            return;
        }

        if (ShouldCommitAsLagrangeMesh()) {
            CommitLagrangeCellArray();
            return;
        }

        switch (m_meshType) {
            case MeshType::SurfaceMesh: {
                const auto surfaceMesh = DynamicCast<SurfaceMesh>(m_output);
                if (surfaceMesh != nullptr) {
                    surfaceMesh->SetFaces(m_cellArray);
                }
                break;
            }
            case MeshType::VolumeMesh: {
                const auto volumeMesh = DynamicCast<VolumeMesh>(m_output);
                if (volumeMesh != nullptr) {
                    volumeMesh->SetVolumes(m_cellArray);
                }
                break;
            }
            case MeshType::UnstructuredMesh:
            case MeshType::PolyhedronMesh: {
                const auto unstructuredMesh = DynamicCast<UnstructuredMesh>(m_output);
                if (unstructuredMesh != nullptr && m_cellTypes != nullptr) {
                    unstructuredMesh->SetCells(m_cellArray, m_cellTypes);
                }
                break;
            }
            default:
                break;
        }
    }

    // 把一个局部 polyhedron 面展开成绝对点 id 列表
    static std::vector<igIndex> ExpandPolyhedronFace(
        const PolyhedronTopologyView& topology,
        const std::size_t cellVertexBegin,
        const std::size_t faceIndex) {
        std::vector<igIndex> facePointIds;
        if (faceIndex + 1 >= topology.faceVertexOffsetCount) {
            return facePointIds;
        }

        const auto localBegin = static_cast<std::size_t>(topology.faceVertexOffsets[faceIndex]);
        const auto localEnd = static_cast<std::size_t>(topology.faceVertexOffsets[faceIndex + 1]);
        facePointIds.reserve(localEnd - localBegin);
        for (std::size_t localIndex = localBegin; localIndex < localEnd; ++localIndex) {
            const auto localVertexId = static_cast<std::size_t>(topology.localFaceVertexIds[localIndex]);
            if (cellVertexBegin + localVertexId >= topology.cellUniqueVertexIdCount) {
                continue;
            }
            facePointIds.push_back(static_cast<igIndex>(topology.cellUniqueVertexIds[cellVertexBegin + localVertexId]));
        }
        return facePointIds;
    }

    // 追加一个 polyhedron chunk 到 volume mesh 的面表
    void AppendVolumePolyhedronChunk(const PolyhedronTopologyView& topology) {
        if (m_polyFaceTable == nullptr || m_polyCellFaces == nullptr ||
            topology.cellVertexOffsetCount == 0 || topology.cellFaceOffsetCount == 0) {
            return;
        }

        const auto cellCount = topology.cellVertexOffsetCount - 1;

        for (std::size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            const auto cellVertexBegin = static_cast<std::size_t>(topology.cellVertexOffsets[cellIndex]);
            const auto faceBegin = static_cast<std::size_t>(topology.cellFaceOffsets[cellIndex]);
            const auto faceEnd = static_cast<std::size_t>(topology.cellFaceOffsets[cellIndex + 1]);

            std::vector<igIndex> cellFaceIds;
            cellFaceIds.reserve(faceEnd - faceBegin);
            for (std::size_t faceIndex = faceBegin; faceIndex < faceEnd; ++faceIndex) {
                auto facePointIds = ExpandPolyhedronFace(topology, cellVertexBegin, faceIndex);
                if (facePointIds.empty()) {
                    continue;
                }
                auto currentFaceId = m_polyFaceTable->IsFace(facePointIds.data(), static_cast<int>(facePointIds.size()));
                if (currentFaceId < 0) {
                    m_polyFaceTable->InsertFace(facePointIds.data(), static_cast<int>(facePointIds.size()));
                    currentFaceId = m_polyFaceTable->GetNumberOfFaces() - 1;
                }
                cellFaceIds.push_back(currentFaceId);
            }
            m_polyCellFaces->AddCellIds(cellFaceIds.data(), static_cast<int>(cellFaceIds.size()));
        }
    }

    [[nodiscard]] bool ShouldCommitAsLagrangeMesh() const noexcept {
        return m_cellArray != nullptr &&
            m_cellTypes != nullptr &&
            !m_cellPolynomialOrders.empty() &&
            m_cellPolynomialOrders.size() == m_pendingCellCount;
    }

    void CommitLagrangeCellArray() {
        auto lagrangeMesh = LagrangeUnstructuredMesh::New();
        if (lagrangeMesh == nullptr) {
            return;
        }

        if (const auto pointSet = DynamicCast<PointSet>(m_output); pointSet != nullptr) {
            lagrangeMesh->SetPoints(pointSet->GetPoints());
        }
        if (m_output->GetAttributeSet() != nullptr) {
            lagrangeMesh->SetAttributeSet(m_output->GetAttributeSet());
        }

        const auto* rawCellTypes = m_cellTypes != nullptr ? m_cellTypes->RawPointer() : nullptr;
        if (rawCellTypes == nullptr) {
            return;
        }

        for (std::size_t cellIndex = 0u; cellIndex < m_pendingCellCount; ++cellIndex) {
            const igIndex* cellIds = nullptr;
            const auto cellSize = m_cellArray->GetCellIds(cellIndex, cellIds);
            if (cellSize <= 0 || cellIds == nullptr) {
                continue;
            }
            lagrangeMesh->AddCell(
                const_cast<igIndex*>(cellIds),
                cellSize,
                static_cast<IGenum>(rawCellTypes[cellIndex]),
                static_cast<int>(m_cellPolynomialOrders[cellIndex]));
        }

        m_output = lagrangeMesh;
    }

    // 追加一个 polyhedron chunk 到 unstructured cell array
    void AppendUnstructuredPolyhedronChunk(const PolyhedronTopologyView& topology) {
        if (m_polyCells == nullptr || topology.cellVertexOffsetCount == 0 || topology.cellFaceOffsetCount == 0) {
            return;
        }

        const auto cellCount = topology.cellVertexOffsetCount - 1;
        for (std::size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            const auto cellVertexBegin = static_cast<std::size_t>(topology.cellVertexOffsets[cellIndex]);
            const auto faceBegin = static_cast<std::size_t>(topology.cellFaceOffsets[cellIndex]);
            const auto faceEnd = static_cast<std::size_t>(topology.cellFaceOffsets[cellIndex + 1]);

            std::vector<igIndex> expandedCell;
            expandedCell.push_back(static_cast<igIndex>(faceEnd - faceBegin));
            for (std::size_t faceIndex = faceBegin; faceIndex < faceEnd; ++faceIndex) {
                auto facePointIds = ExpandPolyhedronFace(topology, cellVertexBegin, faceIndex);
                expandedCell.push_back(static_cast<igIndex>(facePointIds.size()));
                expandedCell.insert(expandedCell.end(), facePointIds.begin(), facePointIds.end());
            }
            m_polyCells->AddCellIds(expandedCell.data(), static_cast<int>(expandedCell.size()));
        }
    }

    // 当前目标网格类型
    MeshType m_meshType{MeshType::PointSet};
    // 正在组装的原生输出对象
    DataObject::Pointer m_output;
    // 暂存的原生 cell array
    CellArray::Pointer m_cellArray;
    // 暂存的原生 cell type 数组
    UnsignedIntArray::Pointer m_cellTypes;
    // 暂存的逐 cell 阶数流
    std::vector<std::uint16_t> m_cellPolynomialOrders;
    ::datacodec::BufferCapacitySample m_polynomialOrdersCapacity{"adapter.decode.polynomial_orders"};
    // 正在写入的 cell 数
    std::size_t m_pendingCellCount{0u};
    // 正在写入的原生 connectivity
    IdArray::Pointer m_pendingConnectivityIds;
    // connectivity 元素数量
    std::size_t m_pendingConnectivityCount{0u};
    // 正在写入的原生 offsets
    UnsignedIntArray::Pointer m_pendingOffsets;
    // 正在写入的属性对象
    std::vector<ArrayObject::Pointer> m_pendingAttributes;
    // 正在写入的属性元数据
    std::vector<AttrStorageParams> m_pendingAttributeMeta;
    // DataCodec 属性索引到原生 AttributeSet 索引的稳定映射
    std::vector<int> m_nativeAttributeIndices;
    // 正在分块组装的 polyhedron 面表
    FaceTable::Pointer m_polyFaceTable;
    // 正在分块组装的 volume polyhedron cell-face 数组
    CellArray::Pointer m_polyCellFaces;
    // 正在分块组装的 unstructured polyhedron cell 数组
    CellArray::Pointer m_polyCells;
    // 正在分块组装的 unstructured polyhedron cell type 数组
    UnsignedIntArray::Pointer m_polyCellTypes;
    // polyhedron 总 cell 数
    std::size_t m_polyCellCount{0u};
    bool m_failed{false};
    std::string m_failureMessage;
};

IGAME_NAMESPACE_END

#endif


