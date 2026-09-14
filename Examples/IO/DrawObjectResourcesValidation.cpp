// CPU-only resource/lifetime regression tests. Never create a GL context,
// upload a buffer, draw a frame, or load a model file.
#include "iGameDrawObject.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* name) {
    if (!condition) { throw std::runtime_error(name); }
    std::cout << "PASS " << name << '\n';
}

class MetadataBuffer final : public iGame::GLBuffer {
public:
    using Pointer = iGame::SmartPointer<MetadataBuffer>;
    static Pointer New() { return new MetadataBuffer; }
    void SetMetadataHandle(unsigned handle) { m_Handle = handle; }
};

class ProbeMeshleter final : public iGame::Meshleter {
public:
    using Pointer = iGame::SmartPointer<ProbeMeshleter>;
    static Pointer New() { return new ProbeMeshleter; }
    void SetMetadataBuffer(iGame::GLBuffer::Pointer buffer) {
#ifdef GL_SUPPORTS_MESH_SHADER
        m_ColorBuffer = buffer;
#else
        m_CellColorVBO = buffer; // Previously omitted by ReleaseGpuBuffers.
#endif
    }
};

class ProbeSurface final : public iGame::SurfaceMesh {
public:
    using Pointer = iGame::SmartPointer<ProbeSurface>;
    static Pointer New() { return new ProbeSurface; }
    bool* destroyed{nullptr};
    void SelectCp() { m_AttributeIndex = 0; m_AttributeDimension = 0; m_UseColor = true; m_AttributeChanged = true; }
    void AliasOriginalArrays(iGame::FloatArray::Pointer cp) {
        m_Positions = m_Points->ConvertToArray();
        m_Colors = cp;
    }
    void AddFallbackSelfCycle() { m_RenderableMesh.SimplifiedMesh = this; }
    void SetTestMeshleter(iGame::Meshleter::Pointer meshleter) { m_RenderableMesh.mMeshleter = meshleter; }
    bool DerivedStateEmpty() const {
        return !m_RenderableMesh.SurfaceMesh && !m_RenderableMesh.SimplifiedMesh && !m_RenderableMesh.mMeshleter &&
               m_Positions->GetNumberOfValues() == 0 && m_Colors->GetNumberOfValues() == 0 &&
               m_TriangleIndices->GetNumberOfValues() == 0 && m_TriangleEdgeMasks->GetNumberOfValues() == 0 &&
               !m_Flag && m_ReConvertToDrawableData && m_AttributeChanged && m_ForceGpuBufferUpload;
    }
    bool CpuDrawRebuilt() const {
        return m_Positions->GetNumberOfValues() == 9 && m_TriangleIndices->GetNumberOfValues() == 3 &&
               m_Colors->GetNumberOfValues() == 12 && m_TriangleEdgeMasks->GetNumberOfValues() == 1 &&
               !m_ReConvertToDrawableData && m_ForceGpuBufferUpload;
    }
private:
    ~ProbeSurface() override { if (destroyed) { *destroyed = true; } }
};

void CheckCpuReleaseAndRebuild() {
    bool destroyed = false;
    auto root = iGame::DrawObject::New();
    auto surface = ProbeSurface::New();
    surface->destroyed = &destroyed;
    surface->SetAutoBuildInteractionLod(false);
    auto points = iGame::Points::New();
    points->AddPoint(0.f, 0.f, 0.f);
    points->AddPoint(1.f, 0.f, 0.f);
    points->AddPoint(0.f, 1.f, 0.f);
    auto faces = iGame::CellArray::New();
    faces->AddCellId3(0, 1, 2);
    auto cp = iGame::FloatArray::New();
    cp->SetName("PressureCoefficient");
    cp->AddValue(-1.f); cp->AddValue(0.f); cp->AddValue(1.f);
    surface->SetPoints(points);
    surface->SetFaces(faces);
    surface->GetAttributeSet()->AddAttribute(IG_SCALAR, IG_POINT, cp);
    surface->SelectCp();
    root->AddSubDataObject(surface);
    surface->ConvertToDrawableData(); // CPU arrays only, including real meshleter input cycle.
    surface->AddFallbackSelfCycle();
    surface->AliasOriginalArrays(cp);
    surface->SetRenderWithMeshlet(true);

    const auto pointsTime = points->GetMTime().GetMTime();
    const auto coordinatesTime = points->ConvertToArray()->GetMTime().GetMTime();
    const auto facesTime = faces->GetMTime().GetMTime();
    const auto cpTime = cp->GetMTime().GetMTime();
    const auto coordinates = points->RawPointer();
    const auto connections = faces->GetCellIdArray()->RawPointer();
    auto* attributes = surface->GetAttributeSet();
    Require(!root->HasGpuResources(), "cpu-conversion-does-not-allocate-GPU-resources");
    root->ReleaseDrawableResources();
    Require(!root->HasGpuResources() && surface->DerivedStateEmpty(), "recursive-release-clears-derived-arrays-and-self-cycles");
    Require(surface->GetPoints().get() == points.get() && surface->GetFaces() == faces.get() &&
            surface->GetAttributeSet() == attributes && attributes->GetAttribute(0).pointer.get() == cp.get(),
            "release-preserves-original-object-and-array-identities");
    Require(points->RawPointer() == coordinates && faces->GetCellIdArray()->RawPointer() == connections &&
            points->GetNumberOfPoints() == 3 && faces->GetNumberOfCells() == 1 && cp->GetNumberOfValues() == 3 &&
            points->GetPoint(1)[0] == 1.f && connections[2] == 2 && cp->GetValue(0) == -1.f && cp->GetValue(2) == 1.f,
            "release-does-not-reset-aliased-original-storage");
    Require(points->GetMTime().GetMTime() == pointsTime &&
            points->ConvertToArray()->GetMTime().GetMTime() == coordinatesTime &&
            faces->GetMTime().GetMTime() == facesTime && cp->GetMTime().GetMTime() == cpTime,
            "release-preserves-raw-data-timestamps");
    Require(surface->GetRenderWithMeshlet() && surface->IsUseColor() && surface->GetAttributeIndex() == 0,
            "release-preserves-scalar-and-meshlet-display-selection");
    root->ReleaseDrawableResources();
    Require(surface->DerivedStateEmpty() && !root->HasGpuResources(), "CPU-only-release-is-idempotent-without-context");
    surface->SetRenderWithMeshlet(false);
    surface->ConvertToDrawableData();
    Require(surface->CpuDrawRebuilt() && !root->HasGpuResources(), "same-parsed-surface-rebuilds-geometry-and-Cp-with-next-upload-forced");
    root->ReleaseDrawableResources();

    // Read-only HasGpuResources inspects handle metadata. Reset the synthetic
    // handle before every assertion/destructor: no GL entry point is invoked.
    auto metadata = MetadataBuffer::New();
    auto meshleter = ProbeMeshleter::New();
    meshleter->SetInput(surface);
    meshleter->SetMetadataBuffer(metadata);
    surface->SetTestMeshleter(meshleter);
    metadata->SetMetadataHandle(123);
    const bool detected = root->HasGpuResources();
    metadata->SetMetadataHandle(0);
    Require(detected, "GPU-metadata-inspection-reaches-composite-child-meshleter-cell-buffer");
    root->ReleaseDrawableResources();
    Require(!meshleter->GetInput() && !meshleter->HasGpuResources(), "release-clears-even-externally-held-meshleter-input");
    root->ClearSubDataObject();
    surface = nullptr;
    Require(destroyed, "released-surface-is-destroyed-after-last-external-owner");
}

void CheckUnstructuredShellRelease() {
    auto mesh = iGame::UnstructuredMesh::New();
    mesh->SetAutoBuildInteractionLod(false);
    auto points = iGame::Points::New();
    points->AddPoint(0.f, 0.f, 0.f); points->AddPoint(1.f, 0.f, 0.f); points->AddPoint(0.f, 1.f, 0.f);
    auto cells = iGame::CellArray::New(); cells->AddCellId3(0, 1, 2);
    auto types = iGame::UnsignedIntArray::New(); types->AddValue(iGame::IG_TRIANGLE);
    mesh->SetPoints(points); mesh->SetCells(cells, types);
    mesh->ConvertToDrawableData();
    auto firstShell = mesh->GetRenderableObject();
    Require(firstShell.get() != mesh.get(), "unstructured-surface-shell-created-without-GL");
    firstShell->ConvertToDrawableData();
    mesh->ReleaseDrawableResources();
    Require(!mesh->HasGpuResources() && !firstShell->HasGpuResources() &&
            mesh->GetRenderableObject().get() == mesh.get() && mesh->GetCells().get() == cells.get() &&
            mesh->GetPoints().get() == points.get(), "release-clears-derived-shell-without-discarding-source-grid");
    mesh->ConvertToDrawableData();
    Require(mesh->GetRenderableObject().get() != mesh.get() && mesh->GetRenderableObject().get() != firstShell.get(),
            "unstructured-source-recreates-a-fresh-renderable-shell");
    mesh->ReleaseDrawableResources();
}
}

int main() {
    try {
        CheckCpuReleaseAndRebuild();
        CheckUnstructuredShellRelease();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
