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

// 主测试网格：4 点 2 三角形，z = x + 2y（斜面）
//   p0(0,0,0)  p1(1,0,1)  p2(0,1,2)  p3(1,1,3)
// 沿 Z 投影 h = 0,1,2,3；沿 (1,1,0) 投影 h = x+y = 0,1,1,2
SurfaceMesh::Pointer MakeSlopeMesh() {
    auto points = Points::New();
    points->AddPoint(0.f, 0.f, 0.f);
    points->AddPoint(1.f, 0.f, 1.f);
    points->AddPoint(0.f, 1.f, 2.f);
    points->AddPoint(1.f, 1.f, 3.f);

    auto faces = CellArray::New();
    faces->AddCellId3(0, 1, 2);
    faces->AddCellId3(1, 3, 2);

    auto mesh = SurfaceMesh::New();
    mesh->SetPoints(points);
    mesh->SetFaces(faces);
    return mesh;
}

// 平面网格：所有点 z = 5（沿 Z 投影退化）
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

// 用例 1：默认方向 +Z、默认范围 [0,1]（端点钉扎 + 中间值）——独立输出上校验
void TestAxisMapping() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "axis mapping");
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0},
                "axis mapping");
}

// 用例 2：自定义输出范围 [10, 20]
void TestCustomRange() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    filter->SetOutputRange(10.0, 20.0);
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "custom range");
    CheckValues(FindElevationArray(filter->GetOutput()),
                {10.0, 10.0 + 10.0 / 3.0, 10.0 + 20.0 / 3.0, 20.0}, "custom range");
}

// 用例 3：任意方向 (1,1,0)——h = x+y = 0,1,1,2 → 0,0.5,0.5,1
void TestArbitraryDirection() {
    auto mesh = MakeSlopeMesh();
    auto filter = ElevationFilter::New();
    Check(filter->SetDirection(1.f, 1.f, 0.f), "SetDirection should accept (1,1,0).");
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed.");
    CheckIndependentOutput(filter, mesh, "arbitrary direction");
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 0.5, 0.5, 1.0},
                "arbitrary direction");
}

// 用例 4：缩放不变性——(2,2,0) 与 (1,1,0) 输出完全一致
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

// 用例 5：平面网格沿 Z 投影退化——全部输出 Low，无 NaN
void TestFlatMeshDegenerate() {
    auto mesh = MakeFlatMesh();
    auto filter = ElevationFilter::New();
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed on flat mesh.");
    CheckIndependentOutput(filter, mesh, "flat mesh");
    CheckValues(FindElevationArray(filter->GetOutput()), {0.0, 0.0, 0.0, 0.0}, "flat mesh");

    // 自定义范围时降级值跟随 Low
    auto mesh2 = MakeFlatMesh();
    auto filter2 = ElevationFilter::New();
    filter2->SetOutputRange(10.0, 20.0);
    filter2->SetInput(mesh2);
    Check(filter2->Execute(), "Execute should succeed on flat mesh.");
    CheckIndependentOutput(filter2, mesh2, "flat mesh custom Low");
    CheckValues(FindElevationArray(filter2->GetOutput()), {10.0, 10.0, 10.0, 10.0},
                "flat mesh custom Low");
}

// 用例 6：非法输入防御——零向量、非法范围被拒绝且不影响执行结果
void TestInvalidInputs() {
    auto filter = ElevationFilter::New();
    Check(!filter->SetDirection(0.f, 0.f, 0.f), "zero direction must be rejected.");
    Check(filter->GetDirection()[2] == 1.f, "rejected input must keep old direction.");

    filter->SetOutputRange(1.0, 0.0);
    Check(filter->GetLowValue() == 0.0 && filter->GetHighValue() == 1.0,
          "invalid range must be rejected.");
    filter->SetOutputRange(5.0, 5.0);
    Check(filter->GetLowValue() == 0.0 && filter->GetHighValue() == 1.0,
          "empty range must be rejected.");
}

// ===== 模型文件用例（Examples/Models 下的配套测试模型）=====

// 用例 7：斜坡地形模型 + 任意方向 (1,1,0) + 默认范围 [0,1]
// 模型 ElevationSlopeTerrain.vtk：11x11 顶点，x,y ∈ {0..10}，z = 0.1*(x+y)
// 沿 (1,1,0) 投影 h = x+y ∈ [0,20]，期望 elevation = (x+y)/20（端点钉扎：(0,0)→0，(10,10)→1）
void TestSlopeModelArbitraryDirection() {
    auto mesh = LoadPointSetModel(SlopeModelPath);
    Check(mesh->GetNumberOfPoints() == 121, "slope model must have 121 points.");

    auto filter = ElevationFilter::New();
    Check(filter->SetDirection(1.f, 1.f, 0.f), "SetDirection should accept (1,1,0).");
    filter->SetInput(mesh);
    Check(filter->Execute(), "Execute should succeed on the slope model.");
    CheckIndependentOutput(filter, mesh, "slope model");

    auto array = FindElevationArray(filter->GetOutput());
    Check(array != nullptr, "slope model: Elevation array is missing.");
    Check(array->GetNumberOfElements() == 121, "slope model: unexpected element count.");

    const float* values = array->RawPointer();
    for (IGsize i = 0; i < 121; ++i) {
        const Point& p = mesh->GetPoint(i);
        const double expected = (p[0] + p[1]) / 20.0;
        if (std::fabs(values[i] - expected) > 1e-5) {
            throw std::runtime_error("slope model: point " + std::to_string(i) +
                                     " elevation is " + std::to_string(values[i]) +
                                     ", expected " + std::to_string(expected));
        }
    }
    // 端点钉扎：首点 (0,0) → 0；末点 (10,10) → 1
    Check(std::fabs(values[0]) < 1e-6, "slope model: first point must map to 0.");
    Check(std::fabs(values[120] - 1.0) < 1e-6, "slope model: last point must map to 1.");
}

// 用例 8：梯田地形模型 + 默认方向 +Z + 范围 [10,20]
// 模型 ElevationTerraces.vtk：11x11 顶点，z = floor((x+y)/4) ∈ {0..5} 六层台阶
// h = z 为精确整数，期望 elevation = 10 + 2z ∈ {10,12,...,20}，且恰好出现六个离散值
void TestTerracesModelAxisMapping() {
    auto mesh = LoadPointSetModel(TerracesModelPath);
    Check(mesh->GetNumberOfPoints() == 121, "terraces model must have 121 points.");

    auto filter = ElevationFilter::New();
    filter->SetOutputRange(10.0, 20.0);
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
        const double expected = 10.0 + 2.0 * std::floor((p[0] + p[1]) / 4.0);
        if (std::fabs(values[i] - expected) > 1e-5) {
            throw std::runtime_error("terraces model: point " + std::to_string(i) +
                                     " elevation is " + std::to_string(values[i]) +
                                     ", expected " + std::to_string(expected));
        }
        distinct.insert(values[i]);
    }
    Check(distinct.size() == 6, "terraces model: expected exactly six terrace levels.");
}

// 用例 9：独立输出语义——输出为新对象、输入不被污染、几何与输入一致、类型保持、重复执行安全
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

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"TestAxisMapping", TestAxisMapping},
        {"TestCustomRange", TestCustomRange},
        {"TestArbitraryDirection", TestArbitraryDirection},
        {"TestScaleInvariance", TestScaleInvariance},
        {"TestFlatMeshDegenerate", TestFlatMeshDegenerate},
        {"TestInvalidInputs", TestInvalidInputs},
        {"TestSlopeModelArbitraryDirection", TestSlopeModelArbitraryDirection},
        {"TestTerracesModelAxisMapping", TestTerracesModelAxisMapping},
        {"TestIndependentOutput", TestIndependentOutput},
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
