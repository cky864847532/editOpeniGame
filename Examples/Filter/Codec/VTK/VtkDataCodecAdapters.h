#ifndef IGAME_EXAMPLES_VTK_DATACODEC_ADAPTERS_H
#define IGAME_EXAMPLES_VTK_DATACODEC_ADAPTERS_H

#include <DataCodec/API/Output/DecodedData.h>
#include <DataCodec/API/Adapter/IEncodeAdapter.h>

#include <vtkSmartPointer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class vtkUnstructuredGrid;

namespace vtk_datacodec_example {

class VtkDataCodecEncodeAdapter final : public ::datacodec::IEncodeAdapter {
public:
    using IndexType = ::datacodec::IndexType;
    using MeshType = ::datacodec::MeshType;
    using ScalarType = ::datacodec::ScalarType;
    using TopologyInputDescriptor = ::datacodec::TopologyInputDescriptor;
    using CellTypeRaw = ::datacodec::CellTypeRaw;
    using CellTypeCodecEntry = ::datacodec::CellTypeCodecEntry;
    using CellTypeMappingMode = ::datacodec::CellTypeMappingMode;
    using CellTypeFamilyCode = ::datacodec::CellTypeFamilyCode;
    using CellTypeLocalCode = ::datacodec::CellTypeLocalCode;

    static std::unique_ptr<VtkDataCodecEncodeAdapter> Create(
        vtkUnstructuredGrid* input,
        std::string* error = nullptr);

    ~VtkDataCodecEncodeAdapter() override;

    VtkDataCodecEncodeAdapter(const VtkDataCodecEncodeAdapter&) = delete;
    VtkDataCodecEncodeAdapter& operator=(const VtkDataCodecEncodeAdapter&) = delete;

    MeshType GetMeshType() const override;
    std::string GetName() const override;

    std::size_t GetNumberOfPoints() const override;
    void GetPoint(std::size_t index, double output[3]) const override;
    ScalarType GetPointScalarType() const override;
    const float* TryGetPointsF32() const override;
    const double* TryGetPointsF64() const override;

    bool DescribeTopology(
        TopologyInputDescriptor& output,
        std::string* error = nullptr) const override;
    std::size_t GetNumberOfCells() const override;
    std::size_t GetCellIdBufferSize() const override;
    const IndexType* GetCellIdBufferPtr() const override;
    const IndexType* GetCellIdOffsetPtr() const override;
    bool IsFixedCellSize() const override;
    int GetFixedCellSize() const override;
    const IndexType* GetCellTypesPtr() const override;

    std::size_t GetCellFaceBufferSize() const override;

    std::size_t GetNumberOfPointAttrs() const override;
    const ::datacodec::IEncodeAttrView& GetPointAttr(std::size_t index) const override;
    std::size_t GetNumberOfCellAttrs() const override;
    const ::datacodec::IEncodeAttrView& GetCellAttr(std::size_t index) const override;

    bool ResolveCellType(CellTypeRaw rawType, CellTypeCodecEntry& entry) const override;
    CellTypeMappingMode GetCellTypeMappingMode() const override;
    bool ResolveCellSizeFromPolynomialOrder(
        CellTypeRaw rawType,
        std::uint16_t order,
        int& size) const override;
    bool EncodeCellTypeFamilyLocal(
        CellTypeRaw rawType,
        CellTypeFamilyCode& familyCode,
        CellTypeLocalCode& familyLocalCode) const override;
    bool DecodeCellTypeFamilyLocal(
        CellTypeFamilyCode familyCode,
        CellTypeLocalCode familyLocalCode,
        CellTypeRaw& rawType) const override;

    void ResetInput() override;
    void Abort() override;

private:
    class Impl;

    VtkDataCodecEncodeAdapter();
    bool Initialize(vtkUnstructuredGrid* input, std::string* error);

    std::unique_ptr<Impl> m_impl;
};

std::shared_ptr<const ::datacodec::ICellTypeMapping> MakeVtkCellTypeMapping();

class VtkDataCodecDecodeAdapter final {
public:
    bool Import(const ::datacodec::DecodedLeaf& leaf, std::string* error = nullptr);
    vtkSmartPointer<vtkUnstructuredGrid> TakeOutput();
private:
    vtkSmartPointer<vtkUnstructuredGrid> m_output;
};

} // 命名空间 vtk_datacodec_example

#endif