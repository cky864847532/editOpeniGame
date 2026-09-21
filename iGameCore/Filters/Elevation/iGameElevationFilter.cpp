#include "Elevation/iGameElevationFilter.h"

#include "iGameAttributeSet.h"
#include "iGameFlatArray.h"
#include "iGameMacro.h"
#include "iGamePointSet.h"
#include "iGameSurfaceMesh.h"
#include "iGameUnstructuredMesh.h"

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

void ElevationFilter::SetRulerRange(double low, double high) {
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
                    "scalar ruler = [{}, {}])",
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

    // 方向归一化：只保留指向（公共缩放被约去，保证缩放不变性）
    const Vector3f dir = m_Direction / static_cast<float>(dNorm);

    auto elevArr = FloatArray::New();
    elevArr->SetName(m_ArrayName);
    elevArr->SetDimension(1);
    elevArr->Resize(nPoints);

    // 标尺语义（与 ParaView / vtkElevationFilter 一致）：
    //   h = p·d̂（投影）→ t = (h − Low)/(High − Low)（标尺参数化）→ 夹断到 [0,1]
    // 标尺固定、不随数据自适应：移动/修改点位会改变输出值与着色；
    // 低于标尺下限输出 0、高于上限输出 1（饱和），任何输入都不产生 NaN。
    const double span = m_High - m_Low;  // SetRulerRange 已保证 span > 0
    for (IGsize i = 0; i < nPoints; ++i) {
        const double h = mesh->GetPoint(i).dot(dir);
        double t = (h - m_Low) / span;
        if (t < 0.0) { t = 0.0; }
        else if (t > 1.0) { t = 1.0; }
        elevArr->SetValue(i, static_cast<float>(t));
    }

    // 取色范围锚定 [0,1]：为 Elevation 属性挂显式 dataRange（行 0 = 模长范围、
    // 行 1 = 分量范围，均为 {0,1}）。渲染 / 标量面板 / 色条读取该范围着色，
    // 颜色分布与标尺语义一致：模型占标尺哪段，颜色就占色带哪段，超出饱和在端色
    auto elevRange = DoubleArray::New();
    elevRange->SetDimension(2);
    elevRange->Resize(2);
    elevRange->SetElement(0, {0.0, 1.0});
    elevRange->SetElement(1, {0.0, 1.0});

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
                resultAttrSet->AddAttribute(src.type, src.attachmentType, copied,
                                             src.GetDataRange());
            }
        }
    }
    resultAttrSet->AddScalar(IG_POINT, elevArr, elevRange);

    IGAME_CORE_INFO("ElevationFilter: Added {} (IG_SCALAR/IG_POINT) to independent output, "
                    "elements = {}, values clamped to [0, 1], data range anchored to [0, 1]",
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
