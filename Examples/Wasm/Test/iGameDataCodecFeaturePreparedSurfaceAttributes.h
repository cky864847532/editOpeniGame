#ifndef iGameDataCodecFeaturePreparedSurfaceAttributes_h
#define iGameDataCodecFeaturePreparedSurfaceAttributes_h


#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"

#include "iGameAttributeSet.h"
#include "iGameCellArray.h"
#include "iGameFlatArray.h"
#include "iGamePoints.h"
#include "iGameScalarsToColors.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"
#include "ModelSurface/iGameModelGeometryFilter.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

namespace iGame::datacodec_test {

[[nodiscard]] inline bool NearlyEqual(
    const double left,
    const double right,
    const double tolerance = 1e-9) {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] inline FloatArray::Pointer BuildScalarArray(
    const char* name,
    const std::vector<float>& values) {
    auto array = FloatArray::New();
    array->SetName(name);
    array->SetDimension(1);
    array->Reserve(static_cast<IGsize>(values.size()));
    for (const auto value : values) { array->AddValue(value); }
    return array;
}

[[nodiscard]] inline bool TestRobustRangeCollapseRecovery() {
    std::vector<float> sparseValues(995u, 0.0f);
    sparseValues.insert(sparseValues.end(), {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});

    auto mapper = ScalarsToColors::New();
    mapper->InitRangeRobust(BuildScalarArray("sparse_non_zero", sparseValues), 0);
    const auto sparseRange = mapper->GetRange();
    if (!NearlyEqual(sparseRange[0], 1.0) || !NearlyEqual(sparseRange[1], 5.0)) {
        std::cerr << "robust range did not recover the non-zero distribution\n";
        return false;
    }

    mapper->InitRangeRobust(BuildScalarArray("constant", std::vector<float>(128u, 42.0f)), 0);
    const auto constantRange = mapper->GetRange();
    if (!(constantRange[0] < 42.0 && constantRange[1] > 42.0)) {
        std::cerr << "constant robust range did not receive symmetric padding\n";
        return false;
    }
    return true;
}

[[nodiscard]] inline bool TestPreparedSurfacePointAndCellAttributes() {
    auto source = UnstructuredMesh::New();
    auto sourcePoints = Points::New();
    sourcePoints->AddPoint(0.0f, 0.0f, 0.0f);
    sourcePoints->AddPoint(1.0f, 0.0f, 0.0f);
    sourcePoints->AddPoint(0.0f, 1.0f, 0.0f);
    sourcePoints->AddPoint(0.0f, 0.0f, 1.0f);
    source->SetPoints(sourcePoints);

    auto sourceCells = CellArray::New();
    const igIndex tetra[] = {0, 1, 2, 3};
    sourceCells->AddCellIds(tetra, 4);
    sourceCells->AddCellIds(tetra, 4);
    auto sourceTypes = UnsignedIntArray::New();
    sourceTypes->AddValue(IG_TETRA);
    sourceTypes->AddValue(IG_TETRA);
    source->SetCells(sourceCells, sourceTypes);

    auto surface = SurfaceMesh::New();
    auto surfacePoints = Points::New();
    surfacePoints->AddPoint(1.0f, 0.0f, 0.0f);
    surfacePoints->AddPoint(0.0f, 1.0f, 0.0f);
    surfacePoints->AddPoint(0.0f, 0.0f, 0.0f);
    surface->SetPoints(surfacePoints);
    auto surfaceFaces = CellArray::New();
    const igIndex face0[] = {0, 1, 2};
    const igIndex face1[] = {2, 1, 0};
    surfaceFaces->AddCellIds(face0, 3);
    surfaceFaces->AddCellIds(face1, 3);
    surface->SetFaces(surfaceFaces);
    surface->SetAttributeSet(AttributeSet::New());

    auto pointMap = FlatArray<igIndex>::New();
    pointMap->SetDimension(1);
    pointMap->Resize(4u);
    pointMap->SetValue(0u, 2.0);
    pointMap->SetValue(1u, 0.0);
    pointMap->SetValue(2u, 1.0);
    pointMap->SetValue(3u, -1.0);
    auto faceToCellMap = std::make_shared<std::vector<igIndex>>(
        std::initializer_list<igIndex>{1, 0});
    source->SetPreparedSurfaceMesh(surface, pointMap, faceToCellMap);

    auto lateAttributes = AttributeSet::New();
    lateAttributes->AddAttribute(
        IG_SCALAR,
        IG_POINT,
        BuildScalarArray("late_point", {11.0f, 22.0f, 33.0f, 44.0f}));
    lateAttributes->AddAttribute(
        IG_SCALAR,
        IG_CELL,
        BuildScalarArray("late_cell", {100.0f, 200.0f}));
    source->SetAttributeSet(lateAttributes);

    if (!source->ViewCloudPicture(nullptr, 0, 0, false)) {
        std::cerr << "prepared surface point attribute could not be selected\n";
        return false;
    }
    source->ConvertToDrawableData();

    auto* mappedAttributes = surface->GetAttributeSet();
    if (mappedAttributes == nullptr || mappedAttributes->GetNumberOfAttributes() != 2u) {
        std::cerr << "prepared surface did not receive both late attributes\n";
        return false;
    }
    const auto& mappedPoint = mappedAttributes->GetAttribute(0);
    const auto& mappedCell = mappedAttributes->GetAttribute(1);
    if (mappedPoint.attachmentType != IG_POINT ||
        mappedPoint.pointer == nullptr ||
        mappedPoint.pointer->GetNumberOfElements() != 3u ||
        !NearlyEqual(mappedPoint.pointer->GetValue(0u), 22.0) ||
        !NearlyEqual(mappedPoint.pointer->GetValue(1u), 33.0) ||
        !NearlyEqual(mappedPoint.pointer->GetValue(2u), 11.0)) {
        std::cerr << "prepared surface point attribute mapping is incorrect\n";
        return false;
    }
    if (mappedCell.attachmentType != IG_CELL ||
        mappedCell.pointer == nullptr ||
        mappedCell.pointer->GetNumberOfElements() != 2u ||
        !NearlyEqual(mappedCell.pointer->GetValue(0u), 200.0) ||
        !NearlyEqual(mappedCell.pointer->GetValue(1u), 100.0)) {
        std::cerr << "prepared surface cell attribute mapping is incorrect\n";
        return false;
    }

    surface->ConvertToDrawableData();
    if (surface->IsActiveColorBufferCellBased() ||
        surface->GetActiveColorBufferElementCount() == 0u ||
        surface->GetActiveColorBufferUpdateId() == 0u) {
        std::cerr << "prepared surface point color buffer diagnostics are invalid\n";
        return false;
    }

    if (!source->ViewCloudPicture(nullptr, 1, 0, false)) {
        std::cerr << "prepared surface cell attribute could not be selected\n";
        return false;
    }
    source->ConvertToDrawableData();
    surface->ConvertToDrawableData();
    if (!surface->IsActiveColorBufferCellBased() ||
        surface->GetActiveColorBufferElementCount() == 0u ||
        surface->GetActiveColorBufferUpdateId() == 0u) {
        std::cerr << "prepared surface cell color buffer diagnostics are invalid\n";
        return false;
    }
    ::datacodec::DataCodecExecutionResources root(::datacodec::ResolvedResourceConfiguration{
        {0u, 1u, 1u}, 0u, 1u, false, true, true});
    const std::weak_ptr<std::vector<igIndex>> weakMap = faceToCellMap;
    DataObject::Pointer retainedOutput;
    {
        iGameFramePackageDecodeAssembly assembly;
        ::datacodec::DecodedData frame;
        frame.leaves.push_back(::datacodec::DecodedLeaf{.path = "leaf", .name = "prepared"});
        iGameDecodeAdapter adapter(source);
        std::string error;
        if (!assembly.BeginFramePackage(frame, &error) || !assembly.CommitLeaf(frame.leaves.front(), adapter, &error) ||
            !assembly.EndFramePackage(&error)) {
            std::cerr << "prepared source frame assembly failed: " << error << '\n';
            return false;
        }
        retainedOutput = assembly.Output();
        source = nullptr;
        assembly.AbortFramePackage();
    }
    if (!retainedOutput || weakMap.expired() || faceToCellMap.use_count() != 2u ||
        root.StorageCapacity()->Snapshot().reservedBytes != 0u) {
        std::cerr << "frame payload did not retain the prepared source and its shared face map after assembly retirement\n";
        return false;
    }
    retainedOutput = nullptr;
    if (faceToCellMap.use_count() != 1u) {
        std::cerr << "final frame payload did not release the prepared source face map\n";
        return false;
    }
    faceToCellMap.reset();
    if (!weakMap.expired() || surface->GetNumberOfPoints() != 3u ||
        root.StorageCapacity()->Snapshot().reservedBytes != 0u) {
        std::cerr << "prepared map did not retire independently of the retained host surface\n";
        return false;
    }
    return true;
}

[[nodiscard]] inline bool TestSurfaceAttributeRanges() {
    auto input = AttributeSet::New();
    std::vector<DoubleArray::Pointer> expectedRanges;
    for (int field = 0; field < 24; ++field) {
        auto values = FloatArray::New();
        values->SetName("field_" + std::to_string(field));
        values->SetDimension(3);
        for (int tuple = 0; tuple < 3; ++tuple) {
            values->AddElement({static_cast<float>(tuple - field),
                static_cast<float>(tuple + field + 1), static_cast<float>(2 * tuple - field)});
        }
        AttributeSet::Attribute reference;
        reference.pointer = values;
        expectedRanges.push_back(reference.GetDataRange());
        input->AddAttribute(IG_VECTOR, field % 2 == 0 ? IG_POINT : IG_CELL, values,
            field == 0 ? expectedRanges.back() : nullptr);
    }
    auto filter = ModelGeometryFilter::New();
    std::vector<igIndex> faceToCell{2, 0, 2};
    AttributeSet::Pointer output;
    filter->CompositeCellAttribute(faceToCell, input, output);
    if (output == nullptr || output->GetNumberOfAttributes() != expectedRanges.size()) { return false; }
    for (int field = 0; field < static_cast<int>(expectedRanges.size()); ++field) {
        auto& attribute = output->GetAttribute(field);
        const auto range = attribute.GetDataRange();
        if (range != input->GetAttribute(field).dataRange ||
            attribute.pointer->GetName() != input->GetAttribute(field).pointer->GetName()) { return false; }
        for (IGsize value = 0; value < range->GetNumberOfValues(); ++value) {
            if (range->GetValue(value) != expectedRanges[field]->GetValue(value)) { return false; }
        }
        for (IGsize tuple = 0; tuple < faceToCell.size(); ++tuple) {
            for (int component = 0; component < 3; ++component) {
                const auto sourceTuple = field % 2 == 0 ? tuple : static_cast<IGsize>(faceToCell[tuple]);
                if (attribute.pointer->GetElementValue(tuple, component) !=
                    input->GetAttribute(field).pointer->GetElementValue(sourceTuple, component)) { return false; }
            }
        }
    }
    return output->GetAttribute(0).dataRange == expectedRanges.front();
}

[[nodiscard]] inline int RunDataCodecFeaturePreparedSurfaceAttributes() {
    if (!TestSurfaceAttributeRanges()) {
        std::cerr << "surface attribute ranges or tuple mapping changed\n";
        return 1;
    }


    if (!TestRobustRangeCollapseRecovery()) { return 1; }
    if (!TestPreparedSurfacePointAndCellAttributes()) { return 1; }
    return 0;
}

}

#endif
