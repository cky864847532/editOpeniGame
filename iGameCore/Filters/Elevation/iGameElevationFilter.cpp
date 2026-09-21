#include "Elevation/iGameElevationFilter.h"

#include "iGameAttributeSet.h"
#include "iGameFlatArray.h"
#include "iGameMacro.h"
#include "iGamePointSet.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"

#include <limits>
#include <string>
#include <vector>

IGAME_NAMESPACE_BEGIN

ElevationFilter::ElevationFilter() {
    SetNumberOfInputs(1);
    SetNumberOfOutputs(1);
}

bool ElevationFilter::SetDirection(float dx, float dy, float dz) {
    if (dx == 0.f && dy == 0.f && dz == 0.f) { return false; }
    if (m_Direction[0] != dx || m_Direction[1] != dy || m_Direction[2] != dz) {
        m_Direction = Vector3f(dx, dy, dz);
        this->Modified();
    }
    return true;
}

bool ElevationFilter::SetDirection(const Vector3f& d) {
    return SetDirection(d[0], d[1], d[2]);
}

void ElevationFilter::SetOutputRange(double low, double high) {
    if (low >= high) { return; }
    if (m_Low != low || m_High != high) {
        m_Low = low;
        m_High = high;
        this->Modified();
    }
}

void ElevationFilter::SetArrayName(const std::string& name) {
    if (m_ArrayName != name) {
        m_ArrayName = name;
        this->Modified();
    }
}

bool ElevationFilter::Execute() {
    const double dNorm = m_Direction.norm();
    IGAME_CORE_INFO("ElevationFilter: Execute() start (direction = ({}, {}, {}), "
                    "output range = [{}, {}])",
                    m_Direction[0], m_Direction[1], m_Direction[2], m_Low, m_High);

    auto obj = GetInput(0);
    if (obj == nullptr) {
        igError("ElevationFilter: GetInput(0) is nullptr!");
        return false;
    }
    auto mesh = DynamicCast<PointSet>(obj);
    if (mesh == nullptr) {
        igError("ElevationFilter: DynamicCast<PointSet> failed!");
        return false;
    }
    if (dNorm == 0.0) {
        igError("ElevationFilter: direction vector is zero!");
        return false;
    }

    const IGsize nPoints = mesh->GetNumberOfPoints();
    if (nPoints == 0) {
        igError("ElevationFilter: No points in mesh!");
        return false;
    }

    // 第一趟：求投影范围 [hMin, hMax]
    double hMin = std::numeric_limits<double>::max();
    double hMax = std::numeric_limits<double>::lowest();
    for (IGsize i = 0; i < nPoints; ++i) {
        const double h = mesh->GetPoint(i).dot(m_Direction);
        if (h < hMin) { hMin = h; }
        if (h > hMax) { hMax = h; }
    }
    IGAME_CORE_INFO("ElevationFilter: projection range: [{}, {}]", hMin, hMax);

    // 投影退化（网格垂直于方向）：降级输出常量 Low，避免除零产生 NaN
    const bool degenerate = (hMax <= hMin);
    if (degenerate) {
        IGAME_CORE_INFO("ElevationFilter: WARNING - all projections are identical "
                        "(flat mesh); every elevation value will be {}", m_Low);
    }

    auto elevArr = FloatArray::New();
    elevArr->SetName(m_ArrayName);
    elevArr->SetDimension(1);
    elevArr->Resize(nPoints);

    // 第二趟：仿射映射 h ∈ [hMin, hMax] → [Low, High]
    const double srcSpan = hMax - hMin;
    const double dstSpan = m_High - m_Low;
    for (IGsize i = 0; i < nPoints; ++i) {
        double out = m_Low;
        if (!degenerate) {
            const double h = mesh->GetPoint(i).dot(m_Direction);
            out = m_Low + (h - hMin) / srcSpan * dstSpan;
        }
        elevArr->SetValue(i, out);
    }

    // ===== 独立输出：不修改输入对象 =====
    // 继承语义：输出新的数据对象，几何（点/面/单元）与输入共享；
    // 结果属性集 = 输入属性集的拷贝（跳过与输出数组同名的旧数组，即覆盖语义）+ 新增 Elevation 数组。
    // 输入对象保持原样（不挂 Elevation 数组），输出可在模型树中作为独立节点展示。
    // 注意：不能用 DeleteAttribute 标记删除（渲染路径按索引遍历会解引用空指针），
    // 拷贝时直接跳过同名旧数组，保证结果属性集不含 isDeleted 残留项。
    auto inputAttrSet = mesh->GetAttributeSet();
    auto resultAttrSet = AttributeSet::New();
    if (inputAttrSet != nullptr) {
        auto allAttributes = inputAttrSet->GetAllAttributes();
        if (allAttributes != nullptr) {
            for (IGsize i = 0; i < allAttributes->GetNumberOfElements(); ++i) {
                auto& src = allAttributes->GetElement(i);
                if (src.IsNone()) { continue; }
                // 覆盖语义：跳过与输出数组同名的旧数组
                if (src.pointer->GetName() == m_ArrayName) { continue; }
                ArrayObject::Pointer copied;
                if (DynamicCast<FloatArray>(src.pointer) != nullptr) {
                    auto p = FloatArray::New();
                    p->DeepCopy(DynamicCast<FloatArray>(src.pointer));
                    p->SetName(src.pointer->GetName());
                    copied = p;
                } else if (DynamicCast<DoubleArray>(src.pointer) != nullptr) {
                    auto p = DoubleArray::New();
                    p->DeepCopy(DynamicCast<DoubleArray>(src.pointer));
                    p->SetName(src.pointer->GetName());
                    copied = p;
                } else {
                    copied = src.pointer;  // 其他类型共享指针（只读属性，安全）
                }
                resultAttrSet->AddAttribute(src.type, src.attachmentType, copied);
            }
        }
    }
    resultAttrSet->AddAttribute(IG_SCALAR, IG_POINT, elevArr);

    IGAME_CORE_INFO("ElevationFilter: Added {} (IG_SCALAR/IG_POINT) to independent output, "
                    "elements = {}",
                    m_ArrayName, elevArr->GetNumberOfElements());

    // 按输入的具体网格类型派生输出对象（几何共享、属性独立）
    if (auto unstructured = DynamicCast<UnstructuredMesh>(mesh); unstructured != nullptr) {
        auto result = UnstructuredMesh::New();
        result->SetName(unstructured->GetName() + "_Elevation");
        result->SetPoints(unstructured->GetPoints());
        result->SetCells(unstructured->GetCells(),
                         UnsignedIntArray::Pointer(unstructured->GetCellTypes()));
        result->SetAttributeSet(resultAttrSet);
        SetOutput(0, result);
        IGAME_CORE_INFO("ElevationFilter: Execute() done (independent UnstructuredMesh output)");
        return true;
    }

    // SurfaceMesh 及其派生类（VolumeMesh / StructuredMesh）都走表面网格分支，
    // 共享输入的几何指针，不修改输入拓扑
    if (auto surface = DynamicCast<SurfaceMesh>(mesh); surface != nullptr) {
        auto result = SurfaceMesh::New();
        result->SetName(surface->GetName() + "_Elevation");
        result->SetPoints(surface->GetPoints());
        result->SetFaces(surface->GetFaces());
        result->SetAttributeSet(resultAttrSet);
        SetOutput(0, result);
        IGAME_CORE_INFO("ElevationFilter: Execute() done (independent SurfaceMesh output)");
        return true;
    }

    // 兜底：裸 PointSet（点云）只共享点几何
    {
        auto result = PointSet::New();
        result->SetName(mesh->GetName() + "_Elevation");
        result->SetPoints(mesh->GetPoints());
        result->SetAttributeSet(resultAttrSet);
        SetOutput(0, result);
        IGAME_CORE_INFO("ElevationFilter: Execute() done (independent PointSet output)");
    }
    return true;
}

IGAME_NAMESPACE_END
