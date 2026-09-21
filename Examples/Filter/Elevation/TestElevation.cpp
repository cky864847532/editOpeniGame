#include <Elevation/iGameElevationFilter.h>

#include "iGameAttributeSet.h"
#include "iGameCellArray.h"
#include "iGameFileIO.h"
#include "iGameFlatArray.h"
#include "iGamePointSet.h"
#include "iGamePoints.h"
#include "iGameSurfaceMesh.h"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace iGame;

// ===== 模型文件路径（相对构建目录；Examples/CMakeLists.txt 会把 Models/ 复制过去）=====
std::string SlopeModelPath = "Models/ElevationSlopeTerrain.vtk";
std::string TerracesModelPath = "Models/ElevationTerraces.vtk";

// 主测试网格：4 点 2 三角形，z = x + 2y + dz（斜面）
//   p0(0,0,dz)  p1(1,0,1+dz)  p2(0,1,2+dz)  p3(1,1,3+dz)
// 沿 Z 投影 h = 0,1,2,3 + dz；沿 (1,1,0) 归一化投影 h = (x+y)/√2
// dz 偏移参数用于验证绝对标尺语义：标尺不变、平移点位 → 输出改变
SurfaceMesh::Pointer MakeSlopeMesh(double dz = 0.0) {
    auto points = Points::New();
    points->AddPoint(0.f, 0.f, static_cast<float>(dz));
    points->AddPoint(1.f, 0.f, static_cast<float>(1.0 + dz));
    points->AddPoint(0.f, 1.f, static_cast<float>(2.0 + dz));
    points->AddPoint(1.f, 1.f, static_cast<float>(3.0 + dz));

    auto faces = CellArray::New();
    faces->AddCellId3(0, 1, 2);
    faces->AddCellId3(1, 3, 2);

    auto mesh = SurfaceMesh::New();
    mesh->SetPoints(points);
    mesh->SetFaces(faces);
    return mesh;
}

// 平面网格：所有点 z = 5（沿 Z 投影恒定，验证标尺饱和 / 居中行为）
SurfaceMesh::Pointer MakeFlatMesh() {
    auto points = Points::New();
    points->AddPoint(0.f, 0.f, 5.f);
    points->AddPoint(1.f, 0.f, 5.f);
    points->AddPoint(0.f, 1.f, 5.f);
    points->AddPoint(1.f, 1.f, 5.f);

    auto faces = CellArray::New();
    faces->AddCellId3(0, 1, 2);
    faces->AddCellId3(1, 3, 2);

    auto mesh = SurfaceMesh::New();
    mesh->SetPoints(points);
    mesh->SetFaces(faces);
    return mesh;
}

void Check(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

FloatArray::Pointer FindElevationArray(DataObject::Pointer object) {
    if (!object || !object->GetAttributeSet()) { return nullptr; }
    auto* attributes = object->GetAttributeSet();
    for (IGsize i = 0; i < attributes->GetNumberOfAttributes(); ++i) {
        auto& attribute = attributes->GetAttribute(i);
        if (attribute.isDeleted || !attribute.pointer) { continue; }
        if (attribute.attachmentType != IG_POINT) { continue; }
        if (attribute.pointer->GetName() == "Elevation") {
            return DynamicCast<FloatArray>(attribute.pointer);
        }
    }
    return nullptr;
}

// 找到输出对象上的 Elevation 属性（返回 Attribute 本体，用于读取 dataRange）
const AttributeSet::Attribute* FindElevationAttribute(DataObject::Pointer object) {
    if (!object || !object->GetAttributeSet()) { return nullptr; }
    auto* attributes = object->GetAttributeSet();
    for (IGsize i = 0; i < attributes->GetNumberOfAttributes(); ++i) {
        auto& attribute = attributes->GetAttribute(i);
        if (attribute.isDeleted || !attribute.pointer) { continue; }
        if (attribute.attachmentType != IG_POINT) { continue; }
        if (attribute.pointer->GetName() == "Elevation") { return &attribute; }
    }
    return nullptr;
}

// 独立输出通用断言：输出非空、与输入不同对象、输入未被 Elevation 数组污染、点数一致
void CheckIndependentOutput(const ElevationFilter::Pointer& filter,
                            const PointSet::Pointer& mesh, const std::string& label) {
    auto output = filter->GetOutput();
    Check(output != nullptr, label + ": filter output is missing.");
    Check(output.GetPointer() != mesh.GetPointer(),
          label + ": output must be an independent object, not the input itself.");
    Check(FindElevationArray(mesh) == nullptr,
          label + ": input mesh must not be polluted with the Elevation array.");
    auto outputMesh = DynamicCast<PointSet>(output);
    Check(outputMesh != nullptr, label + ": output must be a PointSet.");
    Check(outputMesh->GetNumberOfPoints() == mesh->GetNumberOfPoints(),
          label + ": output must keep the same number of points.");
}

void CheckValues(const FloatArray::Pointer& array, const std::vector<double>& expected,
                 const std::string& label) {
    Check(array != nullptr, label + ": Elevation array is missing.");
    Check(array->GetDimension() == 1, label + ": dimension must be 1.");
    Check(array->GetNumberOfElements() == expected.size(),
          label + ": unexpected element count.");
    const float* values = array->RawPointer();
    for (IGsize i = 0; i < expected.size(); ++i) {
        if (std::fabs(values[i] - expected[i]) > 1e-6) {
            throw std::runtime_error(label + ": value " + std::to_string(i) +
                                     " is " + std::to_string(values[i]) +
                                     ", expected " + std::to_string(expected[i]));
        }
    }
}

// 读取模型文件并转型为 PointSet（ElevationFilter 的输入类型）
PointSet::Pointer LoadPointSetModel(const std::string& path) {
    auto object = FileIO::ReadFile(path);
    Check(object != nullptr, "failed to read model file: " + path);
    auto pointSet = DynamicCast<PointSet>(object);
    Check(pointSet != nullptr, path + " is not a PointSet-compatible mesh.");
    return pointSet;
}

// 用例 1：默认标尺 [0,1] + 默认方向 +Z——斜面 h = 0,1,2,3
// → {0, 1, 1, 1}（后两点高于标尺上限，饱和在 1）
void TestAxisMapping() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "axis mapping");
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 1.0, 1.0, 1.0},
                "axis mapping");
}

// 用例 2：标尺 [0,3] 恰好跨满数据——h = 0,1,2,3 → {0, 1/3, 2/3, 1}
void TestRulerStraddle() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetRulerRange(0.0, 3.0);
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "ruler straddle");
    CheckValues(FindElevationArray(filter->GetOutput()),
                {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0}, "ruler straddle");
}

// 用例 3：双向夹断——标尺 [1,2]，h = 0,1,2,3 → t = -1,0,1,2 → 夹断 {0, 0, 1, 1}
void TestClamping() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetRulerRange(1.0, 2.0);
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "clamping");
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 0.0, 1.0, 1.0},
                "clamping");
}

// 用例 4：任意方向 (1,1,0) + 默认标尺 [0,1]——h = (x+y)/√2 = 0, 0.7071, 0.7071, 1.4142
// → {0, 0.7071, 0.7071, 1}（末点高于标尺上限饱和）
void TestArbitraryDirection() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    Check(filter->SetDirection(1.f, 1.f, 0.f), "SetDirection should accept (1,1,0).");
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "arbitrary direction");
    const double s = 1.0 / std::sqrt(2.0);
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, s, s, 1.0},
                "arbitrary direction");
}

// 用例 5：缩放不变性——(2,2,0) 与 (1,1,0) 输出完全一致（默认标尺 [0,1]）
void TestScaleInvariance() {
    auto meshA = MakeSlopeMesh();
    auto filterA = ElevationFilter::New();
    filterA->SetDirection(1.f, 1.f, 0.f);
    filterA->SetInput(meshA);
    Check(filterA->Execute(), "Execute should succeed.");

    auto meshB = MakeSlopeMesh();
    auto filterB = ElevationFilter::New();
    filterB->SetDirection(2.f, 2.f, 0.f);
    filterB->SetInput(meshB);
    Check(filterB->Execute(), "Execute should succeed.");

    auto arrayA = FindElevationArray(filterA->GetOutput());
    auto arrayB = FindElevationArray(filterB->GetOutput());
    Check(arrayA && arrayB, "both arrays must exist.");
    const float* a = arrayA->RawPointer();
    const float* b = arrayB->RawPointer();
    const IGsize count = arrayA->GetNumberOfElements();
    for (IGsize i = 0; i < count; ++i) {
        Check(std::fabs(a[i] - b[i]) < 1e-6,
              "scale invariance: outputs of (1,1,0) and (2,2,0) must match.");
    }
}

// 用例 6：平面网格 z=5（投影恒定，无退化特判，按标尺公式计算）——
// 标尺 [0,1] → 全 1（高于上限饱和）；标尺 [5,10] → 全 0（等于下限）；
// 标尺 [4,6] → 全 0.5（居中）。任何情况都不产生 NaN
void TestFlatMesh() {
    {
        auto mesh = MakeFlatMesh();
        auto filter = ElevationFilter::New();  // 默认标尺 [0,1]
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed on flat mesh.");
        CheckIndependentOutput(filter, mesh, "flat mesh above ruler");
        CheckValues(FindElevationArray(filter->GetOutput()), {1.0, 1.0, 1.0, 1.0},
                    "flat mesh above ruler");
    }
    {
        auto mesh = MakeFlatMesh();
        auto filter = ElevationFilter::New();
        filter->SetRulerRange(5.0, 10.0);
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed on flat mesh.");
        CheckIndependentOutput(filter, mesh, "flat mesh at ruler low");
        CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 0.0, 0.0, 0.0},
                    "flat mesh at ruler low");
    }
    {
        auto mesh = MakeFlatMesh();
        auto filter = ElevationFilter::New();
        filter->SetRulerRange(4.0, 6.0);
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed on flat mesh.");
        CheckIndependentOutput(filter, mesh, "flat mesh inside ruler");
        CheckValues(FindElevationArray(filter->GetOutput()), {0.5, 0.5, 0.5, 0.5},
                    "flat mesh inside ruler");
    }
}

// 用例 7：非法输入防御——零向量、非法标尺被拒绝且保持默认值
void TestInvalidInputs() {
    auto filter = ElevationFilter::New();
    Check(!filter->SetDirection(0.f, 0.f, 0.f), "zero direction must be rejected.");
    Check(filter->GetDirection()[2] == 1.f, "rejected input must keep old direction.");

    filter->SetRulerRange(1.0, 0.0);
    Check(filter->GetRulerLow() == 0.0 && filter->GetRulerHigh() == 1.0,
          "invalid ruler must be rejected.");
    filter->SetRulerRange(5.0, 5.0);
    Check(filter->GetRulerLow() == 0.0 && filter->GetRulerHigh() == 1.0,
          "empty ruler must be rejected.");
}

// 用例 8：绝对标尺语义（实验室需求核心）——标尺固定 [0,3]，仅平移点位 → 输出改变
//   meshA(dz=0)： h = 0,1,2,3  → t = {0, 1/3, 2/3, 1}
//   meshB(dz=+3)：h = 3,4,5,6  → t 全部 ≥ 1 → 饱和 {1,1,1,1}
//   meshC(dz=-1)：h = -1,0,1,2 → t = {-1/3, 0, 1/3, 2/3} → 夹断 {0, 0, 1/3, 2/3}
// 三个网格形状相同、仅位置不同；同一标尺下输出各不相同——证明标尺不随数据自适应，
// 即"方向与 range 不变、改变 point，颜色会改变"（与 ParaView 行为一致）
void TestAbsoluteRuler() {
    {
        auto mesh = MakeSlopeMesh(0.0);
        auto filter = ElevationFilter::New();
        filter->SetRulerRange(0.0, 3.0);
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed.");
        CheckValues(FindElevationArray(filter->GetOutput()),
                    {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0}, "absolute ruler dz=0");
    }
    {
        auto mesh = MakeSlopeMesh(3.0);
        auto filter = ElevationFilter::New();
        filter->SetRulerRange(0.0, 3.0);
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed.");
        CheckValues(FindElevationArray(filter->GetOutput()), {1.0, 1.0, 1.0, 1.0},
                    "absolute ruler dz=+3 (all above ruler)");
    }
    {
        auto mesh = MakeSlopeMesh(-1.0);
        auto filter = ElevationFilter::New();
        filter->SetRulerRange(0.0, 3.0);
        filter->SetInput(mesh);
        Check(filter->Execute(), "Execute should succeed.");
        CheckValues(FindElevationArray(filter->GetOutput()),
                    {0.0, 0.0, 1.0 / 3.0, 2.0 / 3.0},
                    "absolute ruler dz=-1 (first point below ruler)");
    }
}

// 用例 9：斜坡地形模型 + 默认方向 +Z + 默认标尺 [0,1]
// 模型 ElevationSlopeTerrain.vtk：11x11 顶点，x,y ∈ {0..10}，z = 0.1*(x+y)
// 期望 elevation = clamp(0.1*(x+y), 0, 1)（x+y > 10 的区域饱和在 1）
void TestSlopeModelClampedMapping() {
    auto mesh = LoadPointSetModel(SlopeModelPath);
    Check(mesh->GetNumberOfPoints() == 121, "slope model must have 121 points.");

    auto filter = ElevationFilter::New();
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed on the slope model.");
    CheckIndependentOutput(filter, mesh, "slope model");

    auto array = FindElevationArray(filter->GetOutput());
    Check(array != nullptr, "slope model: Elevation array is missing.");
    Check(array->GetNumberOfElements() == 121, "slope model: unexpected element count.");

    const float* values = array->RawPointer();
    for (IGsize i = 0; i < 121; ++i) {
        const Point& p = mesh->GetPoint(i);
        double expected = 0.1 * (p[0] + p[1]);
        if (expected < 0.0) { expected = 0.0; }
        else if (expected > 1.0) { expected = 1.0; }
        if (std::fabs(values[i] - expected) > 1e-5) {
            throw std::runtime_error("slope model: point " + std::to_string(i) +
                                     " elevation is " + std::to_string(values[i]) +
                                     ", expected " + std::to_string(expected));
        }
    }
    // 端点钉扎：首点 (0,0) → 0；远端 (10,10) 处 z = 2 > 标尺上限 → 饱和 1
    Check(std::fabs(values[0]) < 1e-6, "slope model: first point must map to 0.");
    Check(std::fabs(values[120] - 1.0) < 1e-6,
          "slope model: last point must saturate at 1.");
}

// 用例 10：梯田地形模型 + 默认方向 +Z + 标尺 [0,5]
// 模型 ElevationTerraces.vtk：11x11 顶点，z = floor((x+y)/4) ∈ {0..5} 六层台阶
// 期望 elevation = z/5，恰好出现六个离散值 {0, 0.2, 0.4, 0.6, 0.8, 1}
void TestTerracesModelRulerMapping() {
    auto mesh = LoadPointSetModel(TerracesModelPath);
    Check(mesh->GetNumberOfPoints() == 121, "terraces model must have 121 points.");

    auto filter = ElevationFilter::New();
    filter->SetRulerRange(0.0, 5.0);
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed on the terraces model.");
    CheckIndependentOutput(filter, mesh, "terraces model");

    auto array = FindElevationArray(filter->GetOutput());
    Check(array != nullptr, "terraces model: Elevation array is missing.");
    Check(array->GetNumberOfElements() == 121, "terraces model: unexpected element count.");

    const float* values = array->RawPointer();
    std::set<double> distinct;
    for (IGsize i = 0; i < 121; ++i) {
        const Point& p = mesh->GetPoint(i);
        const double expected = std::floor((p[0] + p[1]) / 4.0) / 5.0;
        if (std::fabs(values[i] - expected) > 1e-5) {
            throw std::runtime_error("terraces model: point " + std::to_string(i) +
                                     " elevation is " + std::to_string(values[i]) +
                                     ", expected " + std::to_string(expected));
        }
        distinct.insert(values[i]);
    }
    Check(distinct.size() == 6, "terraces model: expected exactly six terrace levels.");
}

// 用例 11：独立输出语义——输出为新对象、输入不被污染、几何与输入一致、
// 类型保持、重复执行安全（覆盖语义下仍只有一个 Elevation 数组）
void TestIndependentOutput() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");

    auto output = filter->GetOutput();
    Check(output != nullptr, "output must not be null.");
    Check(output.GetPointer() != mesh.GetPointer(),
          "output must be a different object from input.");
    Check(FindElevationArray(mesh) == nullptr,
          "input must not be polluted with the Elevation array.");
    Check(FindElevationArray(output) != nullptr,
          "output must contain the Elevation array.");

    // 类型保持：SurfaceMesh 输入 → SurfaceMesh 输出
    Check(DynamicCast<SurfaceMesh>(output) != nullptr,
          "SurfaceMesh input should yield SurfaceMesh output.");

    // 几何一致：输出共享输入的点几何
    auto outputMesh = DynamicCast<PointSet>(output);
    Check(outputMesh->GetNumberOfPoints() == mesh->GetNumberOfPoints(),
          "output must keep the same number of points.");
    for (IGsize i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        Check((outputMesh->GetPoint(i) - mesh->GetPoint(i)).norm() < 1e-6,
              "output geometry must match input geometry.");
    }

    // 重复执行（覆盖语义）：输入仍不被污染，输出仍是独立对象且只含一个 Elevation 数组
    Check(filter->Execute(), "second Execute should succeed.");
    auto output2 = filter->GetOutput();
    Check(output2 != nullptr && output2.GetPointer() != mesh.GetPointer(),
          "second run must still produce an independent output.");
    Check(FindElevationArray(mesh) == nullptr, "input must remain unpolluted after re-run.");

    int elevationCount = 0;
    auto* attributes = output2->GetAttributeSet();
    for (IGsize i = 0; i < attributes->GetNumberOfAttributes(); ++i) {
        auto& attribute = attributes->GetAttribute(i);
        if (attribute.isDeleted || !attribute.pointer) { continue; }
        if (attribute.pointer->GetName() == "Elevation") { ++elevationCount; }
    }
    Check(elevationCount == 1, "output must contain exactly one Elevation array.");
}

// 用例 12：取色范围锚定——数据未触满标尺时，dataRange 仍为 [0,1]（非数据推导）
// meshC（dz=-1）+ 标尺 [0,3]：输出 {0, 0, 1/3, 2/3}，实际数据范围 [0, 2/3]；
// Elevation 属性的 dataRange 行 0（模长范围）/ 行 1（分量范围）均应保持 {0,1}，
// 证明渲染取色范围锚定在标尺语义的 [0,1]，而不是按数据 min-max 重算
void TestDataRangeAnchor() {
    auto mesh = MakeSlopeMesh(-1.0);
    auto filter = ElevationFilter::New();
    filter->SetRulerRange(0.0, 3.0);
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");

    // 数据实际范围 [0, 2/3]，未触满标尺
    CheckValues(FindElevationArray(filter->GetOutput()),
                {0.0, 0.0, 1.0 / 3.0, 2.0 / 3.0}, "data range anchor values");

    const auto* attribute = FindElevationAttribute(filter->GetOutput());
    Check(attribute != nullptr, "data range anchor: attribute is missing.");
    auto range = attribute->dataRange;
    Check(range != nullptr, "data range anchor: explicit dataRange must be attached.");
    Check(range->GetDimension() == 2 && range->GetNumberOfElements() == 2,
          "data range anchor: dataRange layout must be 2 elements x 2 dims.");
    // 扁平索引：0/1 = 行 0（模长范围 min/max），2/3 = 行 1（分量范围 min/max）
    Check(std::fabs(range->GetValue(0) - 0.0) < 1e-9 &&
              std::fabs(range->GetValue(1) - 1.0) < 1e-9 &&
              std::fabs(range->GetValue(2) - 0.0) < 1e-9 &&
              std::fabs(range->GetValue(3) - 1.0) < 1e-9,
          "data range anchor: dataRange must be anchored to [0,1].");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"TestAxisMapping", TestAxisMapping},
        {"TestRulerStraddle", TestRulerStraddle},
        {"TestClamping", TestClamping},
        {"TestArbitraryDirection", TestArbitraryDirection},
        {"TestScaleInvariance", TestScaleInvariance},
        {"TestFlatMesh", TestFlatMesh},
        {"TestInvalidInputs", TestInvalidInputs},
        {"TestAbsoluteRuler", TestAbsoluteRuler},
        {"TestSlopeModelClampedMapping", TestSlopeModelClampedMapping},
        {"TestTerracesModelRulerMapping", TestTerracesModelRulerMapping},
        {"TestIndependentOutput", TestIndependentOutput},
        {"TestDataRangeAnchor", TestDataRangeAnchor},
    };

    int failed = 0;
    for (const auto& [name, fn] : tests) {
        try {
            fn();
            std::cout << "[PASS] " << name << std::endl;
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << name << ": " << e.what() << std::endl;
        }
    }

    if (failed == 0) {
        std::cout << "All " << tests.size() << " ElevationFilter tests passed." << std::endl;
    } else {
        std::cout << failed << " of " << tests.size()
                  << " ElevationFilter tests failed." << std::endl;
    }
    return failed == 0 ? 0 : 1;
}
