#include "VtkDataCodecAdapters.h"
#include <DataCodec/API/Entry/DataCodecEncodeEntry.h>
#include <DataCodec/API/Entry/DataCodecDecodeEntry.h>
#include <vtkCellType.h>
#include <vtkDoubleArray.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkUnstructuredGrid.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        auto input = vtkSmartPointer<vtkUnstructuredGrid>::New();
        auto points = vtkSmartPointer<vtkPoints>::New();
        points->SetDataTypeToDouble();
        points->InsertNextPoint(std::nextafter(1.0, 2.0), 0, 0);
        points->InsertNextPoint(16777217.0, 0, 0);
        points->InsertNextPoint(0, 1, 0);
        input->SetPoints(points);
        const vtkIdType ids[]{0, 1, 2};
        input->InsertNextCell(VTK_TRIANGLE, 3, ids);
        auto field = vtkSmartPointer<vtkDoubleArray>::New();
        field->SetName("field");
        field->SetNumberOfTuples(3);
        const double fieldValues[]{1.0, 2.0, 3.0};
        std::memcpy(field->GetVoidPointer(0), fieldValues, sizeof(fieldValues));
        input->GetPointData()->AddArray(field);
        std::string error;
        std::shared_ptr<vtk_datacodec_example::VtkDataCodecEncodeAdapter> adapter =
            vtk_datacodec_example::VtkDataCodecEncodeAdapter::Create(input, &error);
        if (!adapter) { throw std::runtime_error(error); }
        datacodec::EncodeRequest request{
            .input = datacodec::EncodeInput::LeafAdapter(adapter),
            .output = datacodec::EncodeOutput::Memory(),
            .resources = {.mode = datacodec::CodecResourceMode::Unlimited, .maxComputeThreads = 1u}};
        request.configuration.pipelineControl.pointOrder = datacodec::EncodePointOrderMode::Original;
        auto encoded = datacodec::Encode(request);
        if (!encoded.success) { throw std::runtime_error("VTK encoding failed"); }
        auto decoded = datacodec::DecodePackage({
            .input = datacodec::EncodedInput::Memory(std::move(encoded.encodedBytes)),
            .cellTypeMapping = vtk_datacodec_example::MakeVtkCellTypeMapping()});
        if (!decoded.success || decoded.output.leaves.size() != 1u) {
            throw std::runtime_error("VTK decoding failed");
        }
        auto& leaf = decoded.output.leaves.front();
        vtk_datacodec_example::VtkDataCodecDecodeAdapter consumer;
        if (!consumer.Import(leaf, &error)) { throw std::runtime_error(error); }
        auto output = consumer.TakeOutput();
        if (output->GetPoints()->GetDataType() != VTK_DOUBLE ||
            output->GetPoints()->GetData()->GetVoidPointer(0) != leaf.geometry.values.data() ||
            output->GetPointData()->GetArray("field")->GetVoidPointer(0) != leaf.attributes.front().values.data()) {
            throw std::runtime_error("VTK result narrowed or copied completed arrays");
        }
        std::weak_ptr<const void> lifetime = leaf.geometry.values.Owner();
        decoded = {};
        double actual[3];
        output->GetPoint(0, actual);
        if (lifetime.expired() || actual[0] != std::nextafter(1.0, 2.0) ||
            output->GetNumberOfCells() != 1 || output->GetCellType(0) != VTK_TRIANGLE) {
            throw std::runtime_error("VTK result lost its original values or storage");
        }
        output = nullptr;
        if (!lifetime.expired()) { throw std::runtime_error("VTK final release retained the array"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
