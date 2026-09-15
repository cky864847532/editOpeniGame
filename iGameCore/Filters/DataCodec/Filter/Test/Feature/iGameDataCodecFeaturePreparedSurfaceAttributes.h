#ifndef iGameDataCodecFeaturePreparedSurfaceAttributes_h
#define iGameDataCodecFeaturePreparedSurfaceAttributes_h

#include "DataCodec/Filter/Adapter/iGamePreparedSurfaceDecodeAdapter.h"
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
    ::datacodec::IDecodedFramePayload::Pointer payload;
    {
        iGameFramePackageDecodeAssembly assembly;
        ::datacodec::FramePackage frame;
        frame.leaves.push_back(::datacodec::FramePackageLeafRecord{.path = "leaf", .name = "prepared"});
        iGameDecodeAdapter adapter(source);
        std::string error;
        if (!assembly.BeginFramePackage(frame, &error) || !assembly.CommitLeaf(frame.leaves.front(), adapter, &error) ||
            !assembly.EndFramePackage(&error)) {
            std::cerr << "prepared source frame assembly failed: " << error << '\n';
            return false;
        }
        payload = assembly.Payload();
        source = nullptr;
        assembly.AbortFramePackage();
    }
    auto nativePayload = std::dynamic_pointer_cast<iGameDecodedFramePayload>(payload);
    if (!nativePayload || nativePayload->Output() == nullptr || weakMap.expired() || faceToCellMap.use_count() != 2u ||
        root.StorageCapacity()->Snapshot().reservedBytes != 0u) {
        std::cerr << "frame payload did not retain the prepared source and its shared face map after assembly retirement\n";
        return false;
    }
    payload.reset();
    nativePayload.reset();
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

[[nodiscard]] inline bool TestSynchronousPreparedSurfaceObserver() {
    const ::datacodec::IndexType vertices[]{0u, 1u, 2u, 3u};
    const ::datacodec::IndexType cellTypes[]{IG_TETRA};
    iGamePreparedSurfaceDecodeAdapter observer;
    const ::datacodec::ConnectivityTopologyDecodeInfo info{
        .blockCount = 1u, .pointCount = 4u, .cellCount = 1u, .fixedCellSize = 4, .hasCellTypes = true};
    std::string error;
    if (!observer.BeginConnectivityTopology(info, &error)) { return false; }
    ::datacodec::DecodedConnectivityTopologyBlock block{
        .blockIndex = 0u, .cellOffset = 0u, .fixedCellSize = 4,
        .connectivity = vertices, .cellTypes = cellTypes};
    if (!observer.ObserveConnectivityBlock(std::move(block), &error) ||
        !observer.EndConnectivityTopology(&error)) { return false; }
    auto source = UnstructuredMesh::New();
    auto points = Points::New();
    points->AddPoint(0.0f, 0.0f, 0.0f);
    points->AddPoint(1.0f, 0.0f, 0.0f);
    points->AddPoint(0.0f, 1.0f, 0.0f);
    points->AddPoint(0.0f, 0.0f, 1.0f);
    source->SetPoints(points);
    auto cells = CellArray::New();
    const igIndex tetra[]{0, 1, 2, 3};
    cells->AddCellIds(tetra, 4);
    auto types = UnsignedIntArray::New();
    types->AddValue(IG_TETRA);
    source->SetCells(cells, types);
    if (!observer.AttachPreparedSurface(source, &error) ||
        observer.Summary().find("surface-faces=4") == std::string::npos) { return false; }

    iGamePreparedSurfaceDecodeAdapter failed;
    if (!failed.BeginConnectivityTopology(info, &error)) { return false; }
    ::datacodec::DecodedConnectivityTopologyBlock invalid{
        .fixedCellSize = 4, .connectivity = std::span(vertices).first(1u), .cellTypes = cellTypes};
    if (failed.ObserveConnectivityBlock(std::move(invalid), &error) ||
        failed.EndConnectivityTopology(&error) || failed.AttachPreparedSurface(source, &error)) { return false; }
    if (!failed.BeginConnectivityTopology(info, &error)) { return false; }
    ::datacodec::DecodedConnectivityTopologyBlock outOfOrder{
        .blockIndex = 1u, .cellOffset = 0u, .fixedCellSize = 4,
        .connectivity = vertices, .cellTypes = cellTypes};
    if (failed.ObserveConnectivityBlock(std::move(outOfOrder), &error) ||
        failed.EndConnectivityTopology(&error)) { return false; }
    if (!failed.BeginConnectivityTopology(info, &error)) { return false; }
    ::datacodec::DecodedConnectivityTopologyBlock restarted{
        .blockIndex = 0u, .cellOffset = 0u, .fixedCellSize = 4,
        .connectivity = vertices, .cellTypes = cellTypes};
    return failed.ObserveConnectivityBlock(std::move(restarted), &error) &&
        failed.EndConnectivityTopology(&error) && failed.AttachPreparedSurface(source, &error) &&
        !failed.AttachPreparedSurface(source, &error);
}

[[nodiscard]] inline bool TestPreparedSurfaceOnCodecWorkers() {
    // 两个四面体共用一个面，逆序投递后应消去公共面并保留六个外表面
    const ::datacodec::IndexType vertices[2][4]{{0u, 1u, 2u, 3u}, {0u, 2u, 1u, 4u}};
    const ::datacodec::IndexType cellTypes[]{IG_TETRA};
    for (const std::size_t workers : {1u, 2u}) {
        ::datacodec::DataCodecExecutionResources run(::datacodec::ResolvedResourceConfiguration{
            {4096u, workers, workers + 1u}, 4096u, workers, true, true});
        ::datacodec::CodecRunScope scope(run);
        iGamePreparedSurfaceDecodeAdapter observer;
        std::string error;
        if (!scope || !observer.SupportsConcurrentBlocks() ||
            !observer.BeginConnectivityTopology({.blockCount = 2u, .pointCount = 5u,
                .cellCount = 2u, .fixedCellSize = 4, .hasCellTypes = true,
                .workerCapacity = run.WorkerCapacity()}, &error)) { return false; }
        std::size_t next = 0u;
        const bool success = ::datacodec::RunOrderedBlocks<std::size_t, std::size_t>(run,
            [&] { return next < 2u; },
            [&](std::size_t& index) { index = 1u - next++; return true; },
            [&](const std::size_t index, std::size_t&, ::datacodec::WorkerContext& worker) {
                // 并发任务直接检查 worker 身份，非阻塞诊断快照允许因锁竞争暂时不可用
                if (run.IsDriverThread() || worker.Index() >= workers || worker.ComputeUnits() != 1u) { return false; }
                std::string localError;
                return observer.ObserveConnectivityBlock({.blockIndex = index, .cellOffset = index,
                    .fixedCellSize = 4, .connectivity = vertices[index], .cellTypes = cellTypes,
                    .workerIndex = worker.Index()}, &localError);
            }, [](std::size_t&) { return true; });
        if (!observer.EndConnectivityTopology(&error) || !scope.Finish(success)) { return false; }
        auto source = UnstructuredMesh::New();
        auto points = Points::New();
        points->AddPoint(0.0f, 0.0f, 0.0f);
        points->AddPoint(1.0f, 0.0f, 0.0f);
        points->AddPoint(0.0f, 1.0f, 0.0f);
        points->AddPoint(0.0f, 0.0f, 1.0f);
        points->AddPoint(0.0f, 0.0f, -1.0f);
        source->SetPoints(points);
        if (!observer.AttachPreparedSurface(source, &error) ||
            observer.Summary().find("surface-faces=6") == std::string::npos ||
            observer.Summary().find("surface-points=5") == std::string::npos) { return false; }
        ::datacodec::ResourceDebugSnapshot snapshot;
        if (!run.TryCopyResourceDebugSnapshot(snapshot) || snapshot.admittedBlocks != 0u ||
            snapshot.activeComputeUnits != 0u) { return false; }
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
    if (!TestSynchronousPreparedSurfaceObserver()) {
        std::cerr << "synchronous prepared surface observer contract failed\n";
        return 1;
    }
    if (!TestPreparedSurfaceOnCodecWorkers()) {
        std::cerr << "prepared surface codec-worker execution failed\n";
        return 1;
    }
    if (!TestRobustRangeCollapseRecovery()) { return 1; }
    if (!TestPreparedSurfacePointAndCellAttributes()) { return 1; }
    return 0;
}

}

#endif
