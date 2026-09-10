# DataCodec 模块计时

计时沿用 RunRecordEmitter → RunStageTimingRecord → IRunRecordSink，以及 TelemetrySessionSink → DataCodecProcessReportJson 的既有输出链。状态、阶段计时和内存采集分别由 sink 的订阅控制。

## 已有覆盖

编解码流水线记录各阶段实际经过时间，包括重排、几何、拓扑、属性及提交。拓扑还有子阶段和进度区间计时，属性解码具有 payload 准备及逐属性计时。普通浮点属性编码已在 worker 中测量块计算时间，由 driver 有序提交时累计 count、totalNs、maxNs。

普通浮点块统计的输出条件现使用 StageTiming 订阅，去除原 ResourceUsage 订阅门槛。统计经既有 AttributeEncodeTiming 消息输出，消息标记 `worker-elapsed-sum-not-wall-time`。它只覆盖普通浮点属性块的已提交计算结果，不代表全部 reference、整数或失败块，也不是进程 CPU 时间。并行工作区间有重叠，totalNs 可以大于请求实际经过时间。

## 新增的少量区间

| 记录名称 | 覆盖边界 |
| --- | --- |
| EncodePrepare | 叶工作区重置、存储参数和数据视图准备，以及 transfer cache 布局配置 |
| PackageBundle.Leaf | 叶字段 bundle 导出，包含阶段准入及所有权整理 |
| PackageWrite.Leaf | 直接叶包封装、字段外层压缩或复制、写出及封装结束，包含阶段准入 |
| PackageFields.Frame | 帧内一个叶包的外层字段处理，包含阶段准入及源 owner 整理；Raw 模式也记录实际调用 |
| PackageWrite.Frame | FramePackageIO::WriteToSink 调用，包含实际包写入及其 Finalize；调用前的准入和完整输出容量准备不在区间内 |
| DecodePackageInspect | 公共包解码流程的 InspectPackage 调用；直接使用已提供帧 metadata 的路径不执行此项 |
| AttributeStorePrepare | 初次解码或属性补充时，直接属性目标存储的选择、准备和接入 |

每个实际调用产生一条 `scope=module-wall` 记录，不在块或元素循环中新增日志。无工作调用可能产生接近零的时间。上述边界不覆盖 Qt 模型树、GPU 上传、绘制及整个应用的打开体验。

## 开关与报告语义

- 原始记录消费者在 `IRunRecordSink::Interests()` 中订阅 `RunRecordKind::StageTiming`；读取普通浮点块汇总消息还需订阅 Message。无需订阅 ResourceUsage 或请求 MemoryTrace
- 现有 IGDC 控制台日志及文件日志沿用已有设置和输出设施；本次不增加开关、线程或另一套日志文件格式
- 新区间通过 ScopedRunStageTiming 读取 steady_clock。无 StageTiming 订阅时不读取时钟、不分配名称字符串。启用时字符串构造和投递均在既有 TryExport 保护中完成
- 返回失败或异常展开时记录退出前已经经过的时间。计时自身不代表操作成功，成功状态以对应 run 的结果为准；可选诊断导出失败只标记诊断不完整
- ProcessSummary 保留新模块名称。JSON 报告在原有分类节点下输出具名模块子节点和 `inclusive-wall-including-waits` 说明，沿用既有报告结构
- 模块时间包含区间内的等待和子调用。它们不保证彼此互斥，不相加构造分类时间或整次请求时间；请求总时间继续采用 RunEnd 的独立测量
- 现有拓扑或属性细粒度记录可能在 ProcessSummary 中折叠。需要原始细节时由消费者直接接收 StageTiming 或使用 Full 捕获，保留既有记录数量上限

## 验证范围

Telemetry 测试覆盖正常订阅、提前返回、异常展开、关闭时零分配、诊断分配失败隔离、ProcessSummary 留存以及 JSON 中模块身份和时间语义。核心回归用于验证插桩接入后的编解码路径；大文件阶段记录用于确认实际记录能经已有 sink 输出，不将单次耗时变化认定为优化收益。

2026-09-10 使用 RelWithDebInfo（Clang、C++20、`-O2 -g -DNDEBUG`），加载已核实的 CLion 环境后集中构建测试目标及 FileBenchmark。核心 CTest 9/9 通过，16.55 秒；沿用本轮范围排除 ResourceModes 的极小请求测速及 QtResourceControls。日志为 logs/datacodec-module-timing-build-20260910.log、logs/datacodec-module-timing-ctest-20260910.log。

同一大型 CGNS 使用默认无损、全部 131 属性和 Adaptive 自动资源配置再运行一次。现有 FileBenchmark sink 订阅 StageTiming 和 Message，不请求内存审计。日志为 logs/datacodec-module-timing-car-20260910-112405.log。

| 指标 | 实测 |
| --- | ---: |
| 源文件载入 | 26.735 s |
| 编码及文件写出 | 27.246 s |
| EncodePrepare | 0.489 ms |
| 131 个 PointAttributeStage 记录累计 | 19731.561 ms |
| PackageWrite.Leaf | 1878.157 ms |
| 普通浮点已提交块数量 | 7749 |
| 普通浮点块 worker 区间累计 | 38441.0259 ms |
| 普通浮点单块最大区间 | 53.7327 ms |
| 完整解码调用 | 7.906 s |
| DecodePackageInspect | 0.112 ms |
| AttributeStorePrepare | 1245.130 ms |

编码和解码均成功，结果缓存命中 0，输出 2,204,353,773 B，规模检查通过。未逐元素比较该大文件，也未清空操作系统文件缓存。该输入走直接叶包路径，帧包专用区间在本次大文件日志中不出现。输出文件 car_3800W_module_timing_20260910-112405.igc 保留在输入目录。

本次只增加计时和诊断输出。27.246 s 与此前 27.374 s 是两次单独测量，不能据此声称性能提升或给出插桩开销上界。worker 累计时间与上述流水线墙钟区间存在重叠，禁止相加形成总时间。
