#include "VtkDataCodecAdapters.h"

#include <DataCodec/API/Adapter/ICellTypeMapping.h>
#include <DataCodec/Common/DataCodecTypes.h>

#include <vtkAbstractArray.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkVariant.h>


#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCellType.h>
#include <vtkDataArray.h>
#include <vtkDataSetAttributes.h>
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkIdTypeArray.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnstructuredGrid.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vtk_datacodec_example {
namespace {

using ::datacodec::AttrAttachment;
using ::datacodec::AttrRole;
using ::datacodec::CellTypeCodecEntry;
using ::datacodec::CellTypeFamilyCode;
using ::datacodec::CellTypeLocalCode;
using ::datacodec::CellTypeMappingMode;
using ::datacodec::CellTypeNativeMappingEntry;
using ::datacodec::CellTypeRaw;
using ::datacodec::CodecCellTypeId;
using ::datacodec::DataType;

constexpr std::array<CellTypeNativeMappingEntry, 10> kVtkCellTypeMappings{{
    {CodecCellTypeId::Vertex, static_cast<CellTypeRaw>(VTK_VERTEX)},
    {CodecCellTypeId::Line, static_cast<CellTypeRaw>(VTK_LINE)},
    {CodecCellTypeId::PolyLine, static_cast<CellTypeRaw>(VTK_POLY_LINE)},
    {CodecCellTypeId::Triangle, static_cast<CellTypeRaw>(VTK_TRIANGLE)},
    {CodecCellTypeId::Quad, static_cast<CellTypeRaw>(VTK_QUAD)},
    {CodecCellTypeId::Polygon, static_cast<CellTypeRaw>(VTK_POLYGON)},
    {CodecCellTypeId::Tetra, static_cast<CellTypeRaw>(VTK_TETRA)},
    {CodecCellTypeId::Hexahedron, static_cast<CellTypeRaw>(VTK_HEXAHEDRON)},
    {CodecCellTypeId::Prism, static_cast<CellTypeRaw>(VTK_WEDGE)},
    {CodecCellTypeId::Pyramid, static_cast<CellTypeRaw>(VTK_PYRAMID)},
}};

constexpr bool HasUniqueVtkCellTypeMappings() {
    for (std::size_t index = 0; index < kVtkCellTypeMappings.size(); ++index) {
        for (std::size_t other = index + 1; other < kVtkCellTypeMappings.size(); ++other) {
            if (kVtkCellTypeMappings[index].codecType == kVtkCellTypeMappings[other].codecType ||
                kVtkCellTypeMappings[index].rawType == kVtkCellTypeMappings[other].rawType) {
                return false;
            }
        }
    }
    return true;
}

static_assert(HasUniqueVtkCellTypeMappings());

bool Fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

class VtkCellTypeMapping final : public ::datacodec::ICellTypeMapping {
public:
    CellTypeMappingMode GetCellTypeMappingMode() const override {
        return CellTypeMappingMode::FamilyLocal;
    }

    bool ResolveCellType(
        const CellTypeRaw rawType,
        CellTypeCodecEntry& entry) const override {
        return ::datacodec::ResolveMappedCellType(kVtkCellTypeMappings, rawType, entry);
    }

    bool ResolveCellSizeFromPolynomialOrder(
        CellTypeRaw,
        std::uint16_t,
        int& size) const override {
        size = -1;
        return false;
    }

    bool EncodeCellTypeFamilyLocal(
        const CellTypeRaw rawType,
        CellTypeFamilyCode& familyCode,
        CellTypeLocalCode& familyLocalCode) const override {
        return ::datacodec::EncodeMappedCellTypeFamilyLocal(
            kVtkCellTypeMappings,
            rawType,
            familyCode,
            familyLocalCode);
    }

    bool DecodeCellTypeFamilyLocal(
        const CellTypeFamilyCode familyCode,
        const CellTypeLocalCode familyLocalCode,
        CellTypeRaw& rawType) const override {
        return ::datacodec::DecodeMappedCellTypeFamilyLocal(
            kVtkCellTypeMappings,
            familyCode,
            familyLocalCode,
            rawType);
    }
};

bool TryMapVtkDataType(const int vtkType, DataType& output) {
    switch (vtkType) {
        case VTK_FLOAT:
            output = DataType::Float32;
            return true;
        case VTK_DOUBLE:
            output = DataType::Float64;
            return true;
        default:
            output = DataType::Float32;
            return false;
    }
}

AttrRole ResolveAttributeRole(vtkDataSetAttributes* attributes, const int arrayIndex) {
    if (attributes == nullptr) {
        return AttrRole::Unknown;
    }
    switch (attributes->IsArrayAnAttribute(arrayIndex)) {
        case vtkDataSetAttributes::SCALARS:
            return AttrRole::Scalar;
        case vtkDataSetAttributes::VECTORS:
            return AttrRole::Vector;
        case vtkDataSetAttributes::NORMALS:
            return AttrRole::Normal;
        case vtkDataSetAttributes::TCOORDS:
            return AttrRole::TexCoord;
        case vtkDataSetAttributes::TENSORS:
            return AttrRole::Tensor;
        default:
            return AttrRole::Unknown;
    }
}


class VtkEncodeAttributeView final : public ::datacodec::IEncodeAttrView {
public:
    VtkEncodeAttributeView(
        vtkDataArray* array,
        const AttrAttachment attachment,
        const AttrRole role)
        : m_array(array), m_attachment(attachment), m_role(role) {
        m_supported = m_array != nullptr && TryMapVtkDataType(m_array->GetDataType(), m_dataType);
    }

    std::string GetName() const override {
        const auto* name = m_array != nullptr ? m_array->GetName() : nullptr;
        return name != nullptr ? name : std::string{};
    }

    DataType GetDataType() const override { return m_dataType; }
    bool IsDataTypeSupported() const override { return m_supported; }
    AttrRole GetRole() const override { return m_role; }
    AttrAttachment GetAttachType() const override { return m_attachment; }

    int GetComponentCount() const override {
        return m_array != nullptr ? m_array->GetNumberOfComponents() : 0;
    }

    std::size_t GetElementCount() const override {
        return m_array != nullptr
            ? static_cast<std::size_t>(m_array->GetNumberOfTuples())
            : 0u;
    }

    const void* TryGetRawPtr() const override {
        if (m_array == nullptr || !m_array->HasStandardMemoryLayout() || m_array->GetNumberOfValues() == 0) {
            return nullptr;
        }
        return m_array->GetVoidPointer(0);
    }

    bool GetTupleBytes(const std::size_t index, void* output, std::string* error = nullptr) const override {
        if (m_array == nullptr || output == nullptr || index >= GetElementCount()) {
            return Fail(error, "VTK attribute tuple byte range is invalid");
        }
        if (TryGetRawPtr() != nullptr) {
            return IEncodeAttrView::GetTupleBytes(index, output, error);
        }
        auto* bytes = static_cast<std::uint8_t*>(output);
        for (int component = 0; component < GetComponentCount(); ++component) {
            const auto value = m_array->GetVariantValue(static_cast<vtkIdType>(index) * GetComponentCount() + component);
            if (m_dataType == DataType::Float32) {
                const auto typed = value.ToFloat();
                std::memcpy(bytes + static_cast<std::size_t>(component) * sizeof(typed), &typed, sizeof(typed));
            } else if (m_dataType == DataType::Float64) {
                const auto typed = value.ToDouble();
                std::memcpy(bytes + static_cast<std::size_t>(component) * sizeof(typed), &typed, sizeof(typed));
            } else { return Fail(error, "VTK array does not support exact tuple access"); }
        }
        return true;
    }

private:
    vtkSmartPointer<vtkDataArray> m_array;
    AttrAttachment m_attachment{AttrAttachment::Point};
    AttrRole m_role{AttrRole::Unknown};
    DataType m_dataType{DataType::Float32};
    bool m_supported{false};
};

int ToVtkAttributeRole(const AttrRole role) {
    switch (role) {
        case AttrRole::Scalar:
        case AttrRole::Color:
            return vtkDataSetAttributes::SCALARS;
        case AttrRole::Vector:
            return vtkDataSetAttributes::VECTORS;
        case AttrRole::Normal:
            return vtkDataSetAttributes::NORMALS;
        case AttrRole::TexCoord:
            return vtkDataSetAttributes::TCOORDS;
        case AttrRole::Tensor:
            return vtkDataSetAttributes::TENSORS;
        case AttrRole::Unknown:
            return -1;
    }
    return -1;
}

} // namespace

class VtkDataCodecEncodeAdapter::Impl {
public:
    vtkSmartPointer<vtkUnstructuredGrid> input;
    std::vector<IndexType> connectivity;
    std::vector<IndexType> offsets;
    std::vector<IndexType> cellTypes;
    std::vector<std::unique_ptr<VtkEncodeAttributeView>> pointAttributes;
    std::vector<std::unique_ptr<VtkEncodeAttributeView>> cellAttributes;
    VtkCellTypeMapping cellTypeMapping;
};

VtkDataCodecEncodeAdapter::VtkDataCodecEncodeAdapter()
    : m_impl(std::make_unique<Impl>()) {}

VtkDataCodecEncodeAdapter::~VtkDataCodecEncodeAdapter() = default;

std::unique_ptr<VtkDataCodecEncodeAdapter> VtkDataCodecEncodeAdapter::Create(
    vtkUnstructuredGrid* input,
    std::string* error) {
    auto adapter = std::unique_ptr<VtkDataCodecEncodeAdapter>(new VtkDataCodecEncodeAdapter());
    if (!adapter->Initialize(input, error)) {
        return nullptr;
    }
    return adapter;
}

bool VtkDataCodecEncodeAdapter::Initialize(
    vtkUnstructuredGrid* input,
    std::string* error) {
    ResetInput();
    if (input == nullptr) {
        return Fail(error, "VTK encode adapter requires vtkUnstructuredGrid input");
    }

    const auto pointCount = input->GetNumberOfPoints();
    const auto cellCount = input->GetNumberOfCells();
    if (pointCount < 0 || cellCount < 0) {
        return Fail(error, "VTK grid reports a negative point or cell count");
    }
    if (static_cast<std::uint64_t>(pointCount) > std::numeric_limits<IndexType>::max() ||
        static_cast<std::uint64_t>(cellCount) > std::numeric_limits<IndexType>::max()) {
        return Fail(error, "VTK grid exceeds the DataCodec 32-bit topology limit");
    }

    auto* points = input->GetPoints();
    auto* pointArray = points != nullptr ? points->GetData() : nullptr;
    if (pointCount != 0 && pointArray == nullptr) {
        return Fail(error, "VTK grid has no point coordinate array");
    }
    if (pointArray != nullptr && pointArray->GetDataType() != VTK_FLOAT &&
        pointArray->GetDataType() != VTK_DOUBLE) {
        return Fail(error, "VTK point coordinates must use float or double storage");
    }

    m_impl->input = input;
    m_impl->offsets.reserve(static_cast<std::size_t>(cellCount) + 1u);
    m_impl->cellTypes.reserve(static_cast<std::size_t>(cellCount));
    m_impl->offsets.push_back(0u);

    for (vtkIdType cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        const auto rawCellType = static_cast<CellTypeRaw>(input->GetCellType(cellIndex));
        CellTypeCodecEntry entry;
        if (!m_impl->cellTypeMapping.ResolveCellType(rawCellType, entry)) {
            std::ostringstream message;
            message << "unsupported VTK cell type " << rawCellType
                    << " at cell " << cellIndex;
            ResetInput();
            return Fail(error, message.str());
        }

        vtkIdType cellPointCount = 0;
        const vtkIdType* cellPointIds = nullptr;
        input->GetCellPoints(cellIndex, cellPointCount, cellPointIds);
        if (cellPointCount < 0 || (cellPointCount != 0 && cellPointIds == nullptr)) {
            ResetInput();
            return Fail(error, "VTK grid returned invalid cell connectivity");
        }
        if (static_cast<std::uint64_t>(cellPointCount) >
            std::numeric_limits<IndexType>::max() - m_impl->connectivity.size()) {
            ResetInput();
            return Fail(error, "VTK connectivity exceeds the DataCodec 32-bit topology limit");
        }

        for (vtkIdType localIndex = 0; localIndex < cellPointCount; ++localIndex) {
            const auto pointId = cellPointIds[localIndex];
            if (pointId < 0 || pointId >= pointCount ||
                static_cast<std::uint64_t>(pointId) > std::numeric_limits<IndexType>::max()) {
                ResetInput();
                return Fail(error, "VTK connectivity contains an invalid point id");
            }
            m_impl->connectivity.push_back(static_cast<IndexType>(pointId));
        }
        m_impl->offsets.push_back(static_cast<IndexType>(m_impl->connectivity.size()));
        m_impl->cellTypes.push_back(rawCellType);
    }

    const auto collectAttributes = [this, error](
        vtkDataSetAttributes* attributes,
        const AttrAttachment attachment,
        const vtkIdType expectedTupleCount,
        std::vector<std::unique_ptr<VtkEncodeAttributeView>>& output) {
        if (attributes == nullptr) {
            return true;
        }
        const auto arrayCount = attributes->GetNumberOfArrays();
        output.reserve(static_cast<std::size_t>(std::max(arrayCount, 0)));
        for (int arrayIndex = 0; arrayIndex < arrayCount; ++arrayIndex) {
            auto* abstractArray = attributes->GetAbstractArray(arrayIndex);
            auto* array = vtkDataArray::SafeDownCast(abstractArray);
            const auto* arrayName = abstractArray != nullptr ? abstractArray->GetName() : nullptr;
            const std::string resolvedName = arrayName != nullptr ? arrayName : "<unnamed>";
            if (array == nullptr) {
                return Fail(error, "VTK attribute '" + resolvedName + "' is not a numeric array");
            }
            DataType dataType;
            if (!TryMapVtkDataType(array->GetDataType(), dataType)) {
                return Fail(error, "VTK attribute '" + resolvedName + "' must use float or double storage");
            }
            if (array->GetNumberOfComponents() <= 0 ||
                array->GetNumberOfTuples() != expectedTupleCount) {
                return Fail(error, "VTK attribute '" + resolvedName + "' has an invalid tuple layout");
            }
            output.push_back(std::make_unique<VtkEncodeAttributeView>(
                array,
                attachment,
                ResolveAttributeRole(attributes, arrayIndex)));
        }
        return true;
    };

    if (!collectAttributes(
            input->GetPointData(),
            AttrAttachment::Point,
            pointCount,
            m_impl->pointAttributes) ||
        !collectAttributes(
            input->GetCellData(),
            AttrAttachment::Cell,
            cellCount,
            m_impl->cellAttributes)) {
        ResetInput();
        return false;
    }

    return true;
}

VtkDataCodecEncodeAdapter::MeshType VtkDataCodecEncodeAdapter::GetMeshType() const {
    return MeshType::UnstructuredMesh;
}

std::string VtkDataCodecEncodeAdapter::GetName() const {
    return "vtkUnstructuredGrid";
}

std::size_t VtkDataCodecEncodeAdapter::GetNumberOfPoints() const {
    return m_impl->input != nullptr
        ? static_cast<std::size_t>(m_impl->input->GetNumberOfPoints())
        : 0u;
}

void VtkDataCodecEncodeAdapter::GetPoint(
    const std::size_t index,
    double output[3]) const {
    if (output == nullptr) {
        return;
    }
    output[0] = 0.0;
    output[1] = 0.0;
    output[2] = 0.0;
    if (m_impl->input == nullptr || index >= GetNumberOfPoints()) {
        return;
    }
    m_impl->input->GetPoint(static_cast<vtkIdType>(index), output);
}

VtkDataCodecEncodeAdapter::ScalarType
VtkDataCodecEncodeAdapter::GetPointScalarType() const {
    if (m_impl->input != nullptr && m_impl->input->GetPoints() != nullptr &&
        m_impl->input->GetPoints()->GetDataType() == VTK_FLOAT) {
        return ScalarType::Float32;
    }
    return ScalarType::Float64;
}

const float* VtkDataCodecEncodeAdapter::TryGetPointsF32() const {
    if (m_impl->input == nullptr || m_impl->input->GetPoints() == nullptr) {
        return nullptr;
    }
    auto* array = m_impl->input->GetPoints()->GetData();
    if (array == nullptr || array->GetDataType() != VTK_FLOAT ||
        !array->HasStandardMemoryLayout() || array->GetNumberOfValues() == 0) {
        return nullptr;
    }
    return static_cast<const float*>(array->GetVoidPointer(0));
}

const double* VtkDataCodecEncodeAdapter::TryGetPointsF64() const {
    if (m_impl->input == nullptr || m_impl->input->GetPoints() == nullptr) {
        return nullptr;
    }
    auto* array = m_impl->input->GetPoints()->GetData();
    if (array == nullptr || array->GetDataType() != VTK_DOUBLE ||
        !array->HasStandardMemoryLayout() || array->GetNumberOfValues() == 0) {
        return nullptr;
    }
    return static_cast<const double*>(array->GetVoidPointer(0));
}

bool VtkDataCodecEncodeAdapter::DescribeTopology(
    TopologyInputDescriptor& output,
    std::string* error) const {
    output = {};
    if (m_impl->input == nullptr) {
        return Fail(error, "VTK encode adapter has no input grid");
    }
    output.pointCount = GetNumberOfPoints();
    output.cellCount = GetNumberOfCells();
    output.connectivityCount = GetCellIdBufferSize();
    output.connectivity = ::datacodec::TopologyValueSource::CompactArray;
    output.offsets = ::datacodec::TopologyValueSource::CompactArray;
    output.cellSize = ::datacodec::TopologyCellSizeSource::Offsets;
    output.cellTypes = ::datacodec::TopologyValueSource::CompactArray;
    return true;
}

std::size_t VtkDataCodecEncodeAdapter::GetNumberOfCells() const {
    return m_impl->cellTypes.size();
}

std::size_t VtkDataCodecEncodeAdapter::GetCellIdBufferSize() const {
    return m_impl->connectivity.size();
}

const VtkDataCodecEncodeAdapter::IndexType*
VtkDataCodecEncodeAdapter::GetCellIdBufferPtr() const {
    return m_impl->connectivity.empty() ? nullptr : m_impl->connectivity.data();
}

const VtkDataCodecEncodeAdapter::IndexType*
VtkDataCodecEncodeAdapter::GetCellIdOffsetPtr() const {
    return m_impl->offsets.empty() ? nullptr : m_impl->offsets.data();
}

bool VtkDataCodecEncodeAdapter::IsFixedCellSize() const { return false; }
int VtkDataCodecEncodeAdapter::GetFixedCellSize() const { return -1; }

const VtkDataCodecEncodeAdapter::IndexType*
VtkDataCodecEncodeAdapter::GetCellTypesPtr() const {
    return m_impl->cellTypes.empty() ? nullptr : m_impl->cellTypes.data();
}

std::size_t VtkDataCodecEncodeAdapter::GetCellFaceBufferSize() const { return 0u; }

std::size_t VtkDataCodecEncodeAdapter::GetNumberOfPointAttrs() const {
    return m_impl->pointAttributes.size();
}

const ::datacodec::IEncodeAttrView&
VtkDataCodecEncodeAdapter::GetPointAttr(const std::size_t index) const {
    return *m_impl->pointAttributes.at(index);
}

std::size_t VtkDataCodecEncodeAdapter::GetNumberOfCellAttrs() const {
    return m_impl->cellAttributes.size();
}

const ::datacodec::IEncodeAttrView&
VtkDataCodecEncodeAdapter::GetCellAttr(const std::size_t index) const {
    return *m_impl->cellAttributes.at(index);
}

bool VtkDataCodecEncodeAdapter::ResolveCellType(
    const CellTypeRaw rawType,
    CellTypeCodecEntry& entry) const {
    return m_impl->cellTypeMapping.ResolveCellType(rawType, entry);
}

VtkDataCodecEncodeAdapter::CellTypeMappingMode
VtkDataCodecEncodeAdapter::GetCellTypeMappingMode() const {
    return m_impl->cellTypeMapping.GetCellTypeMappingMode();
}

bool VtkDataCodecEncodeAdapter::ResolveCellSizeFromPolynomialOrder(
    const CellTypeRaw rawType,
    const std::uint16_t order,
    int& size) const {
    return m_impl->cellTypeMapping.ResolveCellSizeFromPolynomialOrder(rawType, order, size);
}

bool VtkDataCodecEncodeAdapter::EncodeCellTypeFamilyLocal(
    const CellTypeRaw rawType,
    CellTypeFamilyCode& familyCode,
    CellTypeLocalCode& familyLocalCode) const {
    return m_impl->cellTypeMapping.EncodeCellTypeFamilyLocal(
        rawType,
        familyCode,
        familyLocalCode);
}

bool VtkDataCodecEncodeAdapter::DecodeCellTypeFamilyLocal(
    const CellTypeFamilyCode familyCode,
    const CellTypeLocalCode familyLocalCode,
    CellTypeRaw& rawType) const {
    return m_impl->cellTypeMapping.DecodeCellTypeFamilyLocal(
        familyCode,
        familyLocalCode,
        rawType);
}

void VtkDataCodecEncodeAdapter::ResetInput() {
    m_impl->input = nullptr;
    m_impl->connectivity.clear();
    m_impl->offsets.clear();
    m_impl->cellTypes.clear();
    m_impl->pointAttributes.clear();
    m_impl->cellAttributes.clear();
}

void VtkDataCodecEncodeAdapter::Abort() { ResetInput(); }

namespace {
vtkSmartPointer<vtkDataArray> AdoptArray(
    const ::datacodec::DecodedBuffer& buffer, DataType type, int components,
    std::size_t tuples) {
    const auto width = ::datacodec::DataTypeSize(type);
    if (components <= 0 || width == 0 ||
        tuples > static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max()) /
            static_cast<std::size_t>(components) ||
        tuples > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(components) / width ||
        buffer.size() != tuples * static_cast<std::size_t>(components) * width) {
        return nullptr;
    }
    const auto vtkType = type == DataType::Float32 ? VTK_FLOAT : type == DataType::Float64 ? VTK_DOUBLE : VTK_VOID;
    if (vtkType == VTK_VOID) { return nullptr; }
    vtkSmartPointer<vtkDataArray> array;
    array.TakeReference(vtkDataArray::CreateDataArray(vtkType));
    if (!array) { return nullptr; }
    auto lifetime = vtkSmartPointer<vtkCallbackCommand>::New();
    lifetime->SetClientData(new std::shared_ptr<const void>(buffer.Owner()));
    lifetime->SetClientDataDeleteCallback([](void* state) {
        delete static_cast<std::shared_ptr<const void>*>(state);
    });
    lifetime->SetCallback([](vtkObject*, unsigned long, void*, void*) {});
    array->AddObserver(vtkCommand::DeleteEvent, lifetime);
    array->SetNumberOfComponents(components);
    array->SetVoidArray(const_cast<std::uint8_t*>(buffer.data()),
        static_cast<vtkIdType>(buffer.size() / width), 1);
    return array;
}
} // 匿名命名空间

std::shared_ptr<const ::datacodec::ICellTypeMapping> MakeVtkCellTypeMapping() {
    return std::make_shared<VtkCellTypeMapping>();
}

bool VtkDataCodecDecodeAdapter::Import(
    const ::datacodec::DecodedLeaf& leaf, std::string* error) {
    using ::datacodec::IndexType;
    using Kind = ::datacodec::DecodedTopology::Kind;
    m_output = nullptr;
    if (leaf.meshType != ::datacodec::MeshType::UnstructuredMesh ||
        leaf.geometry.dimension != 3u) {
        return Fail(error, "VTK import requires an unstructured mesh with three point components");
    }
    auto pointValues = AdoptArray(leaf.geometry.values, leaf.geometry.dataType, 3,
                                  leaf.geometry.pointCount);
    if (!pointValues) { return Fail(error, "VTK geometry storage or scalar type is invalid"); }
    auto output = vtkSmartPointer<vtkUnstructuredGrid>::New();
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetData(pointValues);
    output->SetPoints(points);
    const auto& topology = leaf.topology;
    if (topology.kind != Kind::Connectivity || !topology.polynomialOrders.empty()) {
        return Fail(error, "VTK import requires ordinary connectivity topology");
    }
    const auto count = topology.cellCount;
    const auto ids = topology.connectivity.size() / sizeof(IndexType);
    const auto limit = static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max());
    if (count >= limit || ids > limit ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(IndexType) - 1u ||
        topology.connectivity.size() % sizeof(IndexType) != 0u ||
        topology.cellTypes.size() != count * sizeof(IndexType) ||
        (!topology.offsets.empty() && topology.offsets.size() != (count + 1u) * sizeof(IndexType)) ||
        (topology.offsets.empty() && ((count == 0u && ids != 0u) ||
                                     (count != 0u && ids % count != 0u)))) {
        return Fail(error, "VTK topology array lengths are invalid");
    }
    const auto* connectivity = reinterpret_cast<const IndexType*>(topology.connectivity.data());
    const auto* offsets = reinterpret_cast<const IndexType*>(topology.offsets.data());
    const auto* cellTypes = reinterpret_cast<const IndexType*>(topology.cellTypes.data());
    auto vtkOffsets = vtkSmartPointer<vtkIdTypeArray>::New();
    auto vtkIds = vtkSmartPointer<vtkIdTypeArray>::New();
    auto vtkTypes = vtkSmartPointer<vtkUnsignedCharArray>::New();
    vtkOffsets->SetNumberOfValues(static_cast<vtkIdType>(count + 1u));
    vtkIds->SetNumberOfValues(static_cast<vtkIdType>(ids));
    vtkTypes->SetNumberOfValues(static_cast<vtkIdType>(count));
    std::size_t previous = 0u;
    for (std::size_t index = 0u; index <= count; ++index) {
        const auto value = offsets ? static_cast<std::size_t>(offsets[index])
                                  : (count ? index * (ids / count) : 0u);
        if (value < previous || value > ids || (index == 0u && value != 0u) ||
            (index == count && value != ids)) {
            return Fail(error, "VTK topology offsets do not match connectivity");
        }
        static_cast<vtkIdType*>(vtkOffsets->GetVoidPointer(0))[index] = static_cast<vtkIdType>(value);
        previous = value;
    }
    for (std::size_t index = 0u; index < ids; ++index) {
        if (connectivity[index] >= leaf.geometry.pointCount) {
            return Fail(error, "VTK topology references an invalid point");
        }
        static_cast<vtkIdType*>(vtkIds->GetVoidPointer(0))[index] = static_cast<vtkIdType>(connectivity[index]);
    }
    VtkCellTypeMapping mapping;
    for (std::size_t index = 0u; index < count; ++index) {
        CellTypeCodecEntry entry;
        if (cellTypes[index] > std::numeric_limits<unsigned char>::max() ||
            !mapping.ResolveCellType(cellTypes[index], entry)) {
            return Fail(error, "VTK topology has an unsupported cell type");
        }
        static_cast<unsigned char*>(vtkTypes->GetVoidPointer(0))[index] = static_cast<unsigned char>(cellTypes[index]);
    }
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    cells->SetData(vtkOffsets, vtkIds);
    output->SetCells(vtkTypes, cells);
    for (const auto& attribute : leaf.attributes) {
        const auto& meta = attribute.metadata;
        auto array = AdoptArray(attribute.values, meta.dataType, meta.dimension, meta.elementCount);
        if (!array) { return Fail(error, "VTK attribute storage or scalar type is invalid"); }
        array->SetName(meta.name.c_str());
        vtkDataSetAttributes* target = meta.attachmentType == AttrAttachment::Point
            ? static_cast<vtkDataSetAttributes*>(output->GetPointData())
            : static_cast<vtkDataSetAttributes*>(output->GetCellData());
        const auto index = target->AddArray(array);
        const auto role = ToVtkAttributeRole(meta.type);
        if (index < 0 || (role >= 0 && target->SetActiveAttribute(index, role) < 0)) {
            return Fail(error, "failed to attach decoded VTK attribute");
        }
    }
    m_output = output;
    return true;
}

vtkSmartPointer<vtkUnstructuredGrid> VtkDataCodecDecodeAdapter::TakeOutput() {
    auto output = m_output;
    m_output = nullptr;
    return output;
}

} // 命名空间 vtk_datacodec_example