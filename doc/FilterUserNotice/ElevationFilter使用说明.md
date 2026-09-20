# ElevationFilter 使用说明

## 功能简介

ElevationFilter（高程标量场过滤器，DIME #19）将每个点沿方向向量 **d** 的投影长度 `h = p·d` 线性映射到输出区间 `[Low, High]`，生成名为 `"Elevation"` 的点标量属性（数组名可通过 `SetArrayName` 自定义）。

**独立输出语义**：过滤器生成**新的输出数据对象**，几何（点/面/单元）与输入共享，属性集独立；Elevation 数组挂在输出对象的属性集上，**输入对象保持原样**。输出对象在模型树中作为独立节点展示（名称为 `原名_Elevation`），便于与原始数据对比、单独删除而不影响输入。

典型用途：地形高程着色、沿任意方向生成梯度标量场、给点云添加投影坐标等。

## 调用方法

```cpp
#include "Elevation/iGameElevationFilter.h"

auto filter = iGame::ElevationFilter::New();
filter->SetInput(mesh);                  // PointSet 派生类型：SurfaceMesh / UnstructuredMesh / 点云等
filter->SetDirection(0.f, 0.f, 1.f);     // 投影方向，默认 +Z（可选；零向量被拒绝）
filter->SetOutputRange(0.0, 1.0);        // 输出区间，默认 [0,1]（可选；要求 low < high）
filter->SetArrayName("Elevation");       // 输出数组名（可选）
if (filter->Execute()) {
    auto output = filter->GetOutput();   // 独立输出对象（类型与输入一致）
    // output->GetAttributeSet() 中含 IG_SCALAR/IG_POINT 的 "Elevation" 数组
}
```

## 使用示例

**UI 方式**：菜单【算法处理】→【高程 (elevation)】，在弹出的参数面板中输入方向向量 (X, Y, Z) 与输出区间上下限，点击应用。执行完成后模型树中出现 `<模型名>_Elevation` 独立节点，可在标量场面板选择 `Elevation` 查看着色效果，原模型节点不受影响。

**代码方式**（见 `Examples/Filter/Elevation/TestElevation.cpp`）：

```cpp
// 斜面网格沿 +Z 投影：h = 0,1,2,3 → 归一化到 [0,1]
auto mesh = MakeSlopeMesh();               // 4 点 2 三角形，z = x + 2y
auto filter = iGame::ElevationFilter::New();
filter->SetInput(mesh);
filter->Execute();                          // 输出 = {0, 1/3, 2/3, 1}
auto output = filter->GetOutput();          // 独立对象，mesh 上没有 Elevation 数组
```

## 注意事项

1. **输入不被修改**：Elevation 数组只挂在输出对象上；对同一输入重复执行不会累积污染输入（覆盖语义：输出属性集中同名旧数组被替换）。
2. **几何共享**：输出对象与输入共享点/面/单元指针（浅共享），不复制几何数据；修改一方几何会同时影响另一方。若需完全独立请自行深拷贝。
3. **方向向量无需归一化**：公共缩放会在归一化中被约去，(1,1,0) 与 (2,2,0) 输出一致。
4. **平面退化**：网格垂直于方向（所有投影值相同）时降级输出常量 `Low`，不产生 NaN。
5. **参数校验**：零向量和 low ≥ high 的区间会被拒绝并保持原值，`SetDirection` 返回 `false` 可用于提示用户。
6. **输出类型**：SurfaceMesh / VolumeMesh / StructuredMesh 输入得到 SurfaceMesh 输出（共享面）；UnstructuredMesh 输入得到 UnstructuredMesh 输出（共享单元）；裸 PointSet（点云）得到 PointSet 输出。
7. **配套测试**：`Examples/Filter/Elevation/TestElevation.cpp`（构建目标 `testElevation`，9 个用例），配套模型 `Examples/Models/ElevationSlopeTerrain.vtk`、`ElevationTerraces.vtk`。
