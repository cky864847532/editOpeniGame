# ElevationFilter 使用说明

## 功能简介

ElevationFilter（高程标量场过滤器，DIME #19）将每个点沿方向向量 **d** 的投影 `h = p·d̂`（d̂ 为归一化方向）按**固定标尺区间 [Low, High]** 参数化为 `t = (h − Low) / (High − Low)`，并**夹断到 [0, 1]**，生成名为 `"Elevation"` 的点标量属性（数组名可通过 `SetArrayName` 自定义）。取色范围锚定 `[0, 1]`。

**标尺语义与 ParaView / vtkElevationFilter 一致**：

- 标尺固定、**不随数据自适应**——在标尺不变的情况下移动/修改点位，输出值与模型颜色都会改变（这是与"数据 min-max 归一化"旧实现的本质区别）；
- 低于标尺下限输出 0，高于标尺上限输出 1（饱和在端色），任何输入都不产生 NaN；
- 渲染、标量面板、色条按锚定范围 [0, 1] 着色：模型占据标尺哪一段，颜色就占据色带哪一段。

**独立输出语义**：过滤器生成**新的输出数据对象**，几何（点/面/单元）与输入共享，属性集独立；Elevation 数组挂在输出对象的属性集上，**输入对象保持原样**。输出对象在模型树中作为独立节点展示（名称为 `原名_Elevation`），便于与原始数据对比、单独删除而不影响输入。

典型用途：地形高程着色、沿任意方向生成梯度标量场、给点云添加投影坐标等。

## 与 ParaView Elevation 的参数对应

| ParaView 表单项 | 本过滤器 API | 说明 |
| --- | --- | --- |
| Scalar Direction（低点→高点连线方向） | `SetDirection(dx, dy, dz)` | 投影方向向量 |
| Low Point / High Point | `SetRulerRange(low, high)` | 标尺区间 = 低点/高点在方向上的投影值（线段垂直位置无关，仅沿方向分量有效） |
| Scalar Range | 固定 [0, 1] | 输出值与取色范围恒为 [0, 1]，无需设置 |

## 调用方法

```cpp
#include "Elevation/iGameElevationFilter.h"

auto filter = iGame::ElevationFilter::New();
filter->SetInput(mesh);                  // PointSet 派生类型：SurfaceMesh / UnstructuredMesh / 点云等
filter->SetDirection(0.f, 0.f, 1.f);     // 投影方向，默认 +Z（可选；零向量被拒绝）
filter->SetRulerRange(0.0, 3.0);         // 投影标尺区间，默认 [0,1]（可选；要求 low < high）
filter->SetArrayName("Elevation");       // 输出数组名（可选）
if (filter->Execute()) {
    auto output = filter->GetOutput();   // 独立输出对象（类型与输入一致）
    // output->GetAttributeSet() 中含 IG_SCALAR/IG_POINT 的 "Elevation" 数组
}
```

## 使用示例

**UI 方式**：菜单【算法处理】→【高程 (elevation)】，在弹出的参数面板中输入方向向量 (X, Y, Z) 与标尺下限/上限（默认记住上次输入的标尺值），点击应用。执行完成后模型树中出现 `<模型名>_Elevation` 独立节点，可在标量场面板选择 `Elevation` 查看着色效果，原模型节点不受影响。

**代码方式**（见 `Examples/Filter/Elevation/TestElevation.cpp`）：

```cpp
// 斜面网格 z = 0,1,2,3，沿 +Z 投影，标尺 [0,3]：输出 {0, 1/3, 2/3, 1}
auto mesh = MakeSlopeMesh();               // 4 点 2 三角形，z = x + 2y
auto filter = iGame::ElevationFilter::New();
filter->SetRulerRange(0.0, 3.0);
filter->SetInput(mesh);
filter->Execute();                          // 输出 = {0, 1/3, 2/3, 1}
auto output = filter->GetOutput();          // 独立对象，mesh 上没有 Elevation 数组

// 同一标尺 [0,3]、整体上移 3 的相同网格：h = 3,4,5,6 → 全部饱和输出 {1,1,1,1}
// ——标尺不变、点位改变，输出与颜色随之改变（ParaView 兼容语义）
```

## 注意事项

1. **输入不被修改**：Elevation 数组只挂在输出对象上；对同一输入重复执行不会累积污染输入（覆盖语义：输出属性集中同名旧数组被替换）。
2. **几何共享**：输出对象与输入共享点/面/单元指针（浅共享），不复制几何数据；修改一方几何会同时影响另一方。若需完全独立请自行深拷贝。
3. **方向向量无需归一化**：公共缩放会在归一化中被约去，(1,1,0) 与 (2,2,0) 输出一致。
4. **饱和而非特判**：平面网格（所有投影值相同）不再走退化特判，按标尺公式正常计算——如 z=5 平面在标尺 [0,1] 下全输出 1、标尺 [4,6] 下全输出 0.5。
5. **取色范围锚定 [0,1]**：Elevation 属性挂显式 dataRange {0,1}，不按数据重算。若在颜色管理器中手动"重算范围"，范围将按当前数据重新推导（此时着色回到数据 min-max 语义），重新执行过滤器即可恢复锚定。
6. **参数校验**：零向量和 low ≥ high 的标尺会被拒绝并保持原值，`SetDirection` 返回 `false` 可用于提示用户。
7. **输出类型**：SurfaceMesh / VolumeMesh / StructuredMesh 输入得到 SurfaceMesh 输出（共享面）；UnstructuredMesh 输入得到 UnstructuredMesh 输出（共享单元）；裸 PointSet（点云）得到 PointSet 输出。
8. **配套测试**：`Examples/Filter/Elevation/TestElevation.cpp`（构建目标 `testElevation`，12 个用例），配套模型 `Examples/Models/ElevationSlopeTerrain.vtk`、`ElevationTerraces.vtk`。
