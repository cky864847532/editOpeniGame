# 大型 CGNS 编码性能分析（2026-09-10）

## 结论及证据强度

当前样本的首要分析对象是属性编码链：本轮 Adaptive 编码及写出 26.421 秒，131 个属性阶段累计 19.156 秒，占 72.5%。固定 8 线程为 26.609 秒，二者相差 0.188 秒（约 0.7%）。本轮没有观察到自适应控制相对固定 8 线程的明显性能损失。

固定 4 线程为 33.230 秒，固定 8 线程相对它减少 6.621 秒，其中属性阶段减少 5.947 秒。增加计算并发在这个范围内有实际作用。8 线程以上的扩展效果尚未实测。

代码已确认属性阶段顺序执行、字段内部块并行、块读入和有序提交共用 driver。这些结构提供了进一步分析入口。输入重排、reference、等待和提交各自的耗时尚未分别测出，现阶段不能将其中某项认定为已证实的主因。

用户记忆中的改造前约 25 秒缺少同一构建、配置及测量边界的对照记录，性能回退及其归因均尚未确认。本轮未修改生产编解码算法。

优化范围限定为 DataCodec 内部的属性编码组织，包括输入重排、缓冲复用、reference 数据准备、块任务调度及结果提交。Libpressio 和其他具体属性编码器的实现、算法选择及编码参数保持不变。reference 预测算法本身也不属于本轮优化范围。Morton 建序、拓扑及包封装的阶段耗时仅用于解释全程构成。

## 实验口径

执行环境为 Windows，Ryzen 7 7840H，8 个物理核、16 个逻辑处理器，系统报告物理内存 33,522,085,888 B。工作分支为 `datacodec/dev`，基于提交 `73674de7f` 及工作区中的模块计时改动。

构建采用 CMake/Ninja、UCRT64 Clang、C++20，配置为 RelWithDebInfo，优化选项 `-O2 -g -DNDEBUG`。本轮继续使用已核实的 CLion 环境脚本。此结果属于启用优化的构建；纯 Release 配置尚未作为本轮对照运行。

输入是此前核实并持续测量的 `car_3800W_Fluent_node_HDF5-0001.cgns`，原文件 10,571,784,552 B，8,034,973 点、39,494,600 单元，131 个 Float64 单分量属性，属性值总数 1,052,581,463。用户早先给出的分层路径与实际磁盘路径不同，本轮沿用实际存在的同一输入文件。

三组使用默认无损、全部属性、默认 Morton 和启用字段间 reference 的配置，均走直接 LeafPackage 路径。资源配置分别是 Fixed 4、Fixed 8、Adaptive；Fixed 两组只显式指定计算线程上限，内存容量仍按当时环境自动确定。执行顺序为 Fixed 4 → Fixed 8 → Adaptive，每组独立进程，每组一次。

测速入口为 `Test/Runner/DataCodecFileBenchmark.cpp`，调用形式为：

```text
iGameDataCodecFileBenchmark input.cgns new-output.igc --threads 4
iGameDataCodecFileBenchmark input.cgns new-output.igc --threads 8
iGameDataCodecFileBenchmark input.cgns new-output.igc
```

最后一种使用 Adaptive。新增的 `--threads N` 为本轮实验入口参数，同一配置同时用于随后解码。

- CGNS 载入单独计时，包括项目 reader 的附带处理；源属性枚举完成后才启动编码计时
- 编码墙钟覆盖输出对象建立、适配器建立、Encode 调用、文件写出及局部对象销毁；包含已有诊断 sink 的同步日志输出
- 进程 CPU 秒通过 Windows GetProcessTimes 的 kernel+user 差值获得，查询边界紧邻编码墙钟边界，包含该进程区间内所有线程的 CPU 消耗；查询失败留空
- 解码前释放源对象和 reader；解码包括恢复全部属性及项目对象构建
- 未清空操作系统文件缓存，写出完成不代表强制刷入物理介质；未进行系统后台负载、频率和温度的严格隔离
- 三组编码、解码均成功，exit code 均为 0，解码缓存命中均为 0，规模比较均通过
- 规模比较覆盖点、单元、属性及属性值数量，本轮没有逐值或逐连接验证大型文件
- 三组输出均为 2,204,353,773 B；相同大小不构成逐字节相同的证明

## 对照结果

| 指标 | Fixed 4 | Fixed 8 | Adaptive |
| --- | ---: | ---: | ---: |
| CGNS 载入，秒 | 26.686 | 26.157 | 25.962 |
| 编码及文件写出，秒 | 33.230 | 26.609 | 26.421 |
| 编码区间进程 CPU，秒 | 119.984 | 130.812 | 129.828 |
| CPU 秒 / 编码墙钟秒 | 3.611 | 4.916 | 4.914 |
| 解码，秒 | 14.543 | 7.639 | 7.513 |
| 编码前可用物理内存，B | 8,971,821,056 | 8,659,185,664 | 8,592,596,992 |

CPU 秒 / 墙钟秒表示整个区间平均使用的逻辑 CPU 等价值，不能据此推断同时活跃 worker 数、锁等待时间或内存带宽饱和。Fixed 8 的 CPU 总消耗高于 Fixed 4，同时墙钟缩短；线程调度、缓存行为和硬件频率等影响仍需进一步区分。

| 编码模块，毫秒 | Fixed 4 | Fixed 8 | Adaptive | Adaptive 占编码墙钟 |
| --- | ---: | ---: | ---: | ---: |
| 131 个 PointAttributeStage 累计 | 25152.948 | 19205.466 | 19156.165 | 72.50% |
| TopoStage | 3427.345 | 2832.619 | 2809.122 | 10.63% |
| PointSpatialPartition.Morton | 2435.692 | 2522.907 | 2383.834 | 9.02% |
| PackageWrite.Leaf | 1941.017 | 1835.283 | 1825.389 | 6.91% |
| GeometryStage | 231.906 | 171.023 | 208.642 | 0.79% |
| ParamsEncodeStage | 15.895 | 12.561 | 11.970 | 0.05% |

此表只选当前串行执行链上可区分的顶层区间，属性阶段按各自区间累加。拓扑子阶段、进度区间和 worker 统计不再加入此表，避免重复计算。微量准备和阶段间工作由请求总计时覆盖，未强行分摊。

Adaptive 最慢的 5 个属性阶段 metadata 索引为 0、1、2、34、129，分别为 333.602、267.937、259.491、258.527、256.541 毫秒，合计 1.376 秒，仅占整条属性链约 7.2%。这里没有一个数秒级的单字段异常支配全局；优化应优先覆盖反复执行的公共路径。索引来自 `PointAttributeStage::StageIndex()`，不直接当作外部属性名或原始属性序号。

普通浮点已提交块均为 7749 个，worker 计算区间累计分别为 34.668、37.841、38.061 秒。这项既有统计只覆盖普通浮点路径，reference 路径有独立实现。它既不是全部属性计算时间，也不是进程 CPU 时间；禁止用它减去属性墙钟推算等待成本。

上一轮同样启用模块计时的 Adaptive 为 27.246 秒，其中属性 19.732 秒。本轮 26.421 秒尚未对应任何生产优化。这些数据说明单次测试存在波动，需要在后续验证实际改动时安排交替顺序的重复对照。

## 代码定位与优化优先级

### 1. 属性输入准备和 reference：优先测量公共路径

`Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h` 的 `NumericEncodeCursor::ReadNext` 在 driver 中通过 `NumericArrayBlockReader::ReadElements` 准备当前块。基础块为 65536 tuple。

`Codec/NumericArray/NumericArrayReader.h` 的非 identity orderProvider 路径按块构建 orderBlock 并调用 ReadRange。CompactAOS 重排路径逐 tuple 解析索引、检查范围、复制 tupleBytes。对本样本的单分量 Float64，一个 tuple 为 8 B。131 个字段重复访问同一几何次序，累计处理约 10.53 亿个属性标量；reference 的取样、源读取和编码还可能增加数据访问。

优先验证输入准备在属性阶段中的占比。确认热点后，可在现有槽位生命周期内研究重用读入工作区、有限顺序窗口复用、常见单分量类型的复制路径。索引合法性和缓冲容量验证仍应保留。不提前整批复制全部属性，不通过增大基础块逃避调度成本。

`Codec/Attributes/AttributeEncode.h` 在 BuildAttributeReferenceDecision 中确保 attachment 的 reference schedule 已建立，再解析当前字段来源。首次属性处理可包含该 schedule 的初始化成本。EncodeFieldPayload 选择普通路径或 reference 路径，现有普通浮点累计计时未覆盖后者。需要分别测量 schedule 建立、普通字段处理和 reference 字段处理，才能解释完整的 19.156 秒。关闭 reference 会改变压缩策略及输出大小，只适合作为算法拆分实验，其速度变化不能直接记为等配置优化收益。

### 2. 块 driver 与属性串行边界：确认气泡后再改执行结构

`Runtime/Execution/ParallelExecution.h` 的 RunOrderedBlocks 优先提交已完成的队首，退休后继续循环；取得槽位后由 driver 读入，再提交 worker 计算。读入和提交在同一个 driver 上执行，worker 仅执行 compute。每块还建立 BlockRecord 与 TerminalWork，并经过资源根的状态更新和唤醒。

`Workflow/Encode/EncodePipeline.h` 的 ExecuteStageDag 调用 ExecuteStageDagSerial，每轮只选第一个 ready stage。当前字段之间顺序执行，字段内部块并行。这会使每个字段经历一次流水线启动与排空。

改造起点 `df6f459dd` 中的 ExecuteStageDag 存在并行 stage 分支，多个 ready 属性有机会并行；旧普通数值 cache builder 在一个字段内顺序循环块。并行层次确有改变。改造前 25 秒尚无同条件实测，不能把这项结构变化直接等同于已确认的性能回退原因。

下一步需要区分 driver 的有效读入、有效提交、准入等待、有序完成等待，以及 compute 工作量。等待观测需说明状态边界，队首等待也可能与其他 worker 的有效计算重叠。若输入准备或启动排空气泡显著，再在同一资源根及有界槽位中研究流水线重叠。保持取消、顺序提交和失败清理约束，避免恢复旧的多套调度管理。

### 3. Morton、拓扑与封装：作为全程背景保留

Morton 约 2.384 秒、拓扑约 2.809 秒。本次表格只识别其总占比，没有将尚未测出的排序、重排或具体拓扑压缩成本视为确定热点。这些模块不在当前优化范围内；属性读取时应用已有 Morton 次序的过程属于属性输入准备，仍在范围内。

PackageWrite.Leaf 约 1.825 秒，覆盖字段外层处理、复制、封装及写出，不能直接解释为磁盘 I/O。本轮保持包封装及外层压缩策略不变。

### 4. 自适应策略：当前保留

DataCodecResourceController 的健康启动计算额度上限为 8，采样周期 250 毫秒，初始增长间隔 500 毫秒。当前许多属性阶段只有约一两百毫秒，阶段/工作类型边界与观察周期的关系值得在资源轨迹中确认。

本轮未记录每次 C/S 变动，不能声称 Adaptive 全程固定在 8、发生了某次降额或已经达到最佳并发。Fixed 8 与 Adaptive 的近似结果使策略调整暂时排在属性路径分析之后。下一轮可增加 Fixed 16 的少量对照，评估当前机器的上端扩展性，不直接把 16 写成全设备默认值。

## 后续验证方式

从本轮 26.421 秒达到 25 秒需减少 1.421 秒；若其他阶段保持不变，相当于属性链缩短约 7.4%。从上一轮 27.246 秒出发，需减少 2.246 秒，相当于其属性链约 11.4%。这些是目标换算，尚不构成优化收益承诺。

下一轮只在属性边界汇总少量诊断：schedule 建立、读入、普通/reference compute、提交及等待。沿用 RunRecordEmitter 和 StageTiming/Message，按字段或请求汇总输出，避免逐元素及逐块刷日志；driver 区间与并行 compute 区间分别标识，不相加构造请求时间。

有明确候选后，使用同一优化构建、同一输入及算法参数，按修改前后交替顺序测量多次，比较中位数及波动范围。具体编码器只作为保持配置不变的计算调用边界，计时可以包围调用，优化不进入其实现。一起检查输出规模、压缩大小和相关正确性测试。CPU 时间用于辅助解释，内存峰值需独立采样，当前进程生命周期峰值包含 CGNS 载入，不能直接当作 DataCodec 编码峰值。

## 留存材料及完成范围

原始日志位于仓库 logs 目录：

- datacodec-encode-analysis-build-20260910.log
- datacodec-encode-analysis-fixed4-20260910-113653.log
- datacodec-encode-analysis-fixed8-20260910-113653.log
- datacodec-encode-analysis-adaptive-20260910-113653.log

三份输出保留在输入文件同目录，名称为 `car_3800W_analysis_{fixed4,fixed8,adaptive}_20260910-113653.igc`。

本轮新增测速参数、阶段 scope 输出及进程 CPU 区间测量；FileBenchmark 集中构建成功，三次完整编解码实验成功。此前模块插桩的核心 CTest 已完成 9/9，本轮未重复整套回归。生产编解码优化尚未实施，当前改动尚未提交。旧设计文档保持原状。
