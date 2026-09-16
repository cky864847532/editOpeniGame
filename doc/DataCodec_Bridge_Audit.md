# DataCodec 桥接完整审查报告

审查日期：2026-09-16

代码基准：d4cf74a157e5a999d5c1efed07be271c03a11365

分支：datacodec/dev-commit-tidy-up

性质：静态源码审查与整改建议，仅修改报告

边界修订：DataCodec 负责编解码与原始解码结果；抽壳、表面缓存、表面持久化及其执行资源归 Wasm Example。Filter 统一提供路径与内存对象模式，本机路径 IO 的 mmap/文件映射支持由 DataCodec 自有模块实现。内存输入采用只读借用与共享所有权；完成结果向调用方转移所有权，取消核心直接填充外部原生数组的接口。单次操作结束释放内部缓存、工作区及文件映射资源。iGame 自行完成其智能指针的交接，DataCodec 不要求外部实现 reader/sink、ByteStore、payload、租约或所有权转接接口。本版据此统一 R09、R15–R16、生命周期与验收标准，并在第 10.7 节集中描述迁移方案。

## 1. 总体结论

**DataCodec 自身的内存、线程和原始数据缓存应全面内聚；Wasm Example 独立负责抽壳、派生表面、表面缓存与持久化。当前桥接尚未完整落实这两个边界。**

DataCodec 已经拥有内置资源预算、块计算线程、任务协调器、参考数据缓存、完整帧缓存和编码输入缓存。正式请求参数中未发现外部 allocator、memory provider、thread pool、executor、frame cache 的注入入口。

剩余问题集中在七个方面：

1. **编解码基础设施边界仍需收窄。** 原生解码输出 adapter 实现完整 ByteStore 后端协议。Qt 与 Wasm 各自的后台调用包装属于正常宿主用法，统一异步 API 作为 R10 的可选设计项单独讨论。
2. **表面业务进入了解码桥接。** PreparedSurface 模式、抽壳 adapter、表面附着和摘要位于 DataCodec 桥接层；抽壳工作区依赖 codec worker 索引。
3. **原始结果与展示结果需要明确区分。** 播放属性源依赖原始帧缓存命中；浏览器表面命中仅恢复展示对象，Example 需要据此声明能力并管理后续原始数据访问。
4. **存在确定的读写与类型合同问题。** 几何输出固定 Float32、默认属性 getter 经 double 中转、普通单叶读取丢失按需属性会话、Writer 保留不匹配的缓冲区保存接口。
5. **公开边界仍暴露内部模型。** API 引用会话实现和包存储结构；部分缓存配置与统计缺少有效实现；诊断输出存在直接调用用户回调的异常边界缺口。
6. **标准字节 IO 的入口仍需简化。** 编码已有内存结果；文件编码和文件/内存解码仍通过调用方构造 reader/sink 接入。Filter 统一提供路径/内存入口，DataCodec 内部管理 IO 对象，并在自有文件 IO 中实现 mmap 支持；普通与多帧采用同一规则。
7. **输入共享与输出转移的合同尚未统一。** 编码输入入口仍借用 adapter 裸指针；播放结果通过外部 payload 与组装器持有原生对象及帧会话；编码内存结果仍包装 MemoryStore 与容量预算状态。结果所有权需要与执行资源生命周期解耦。

**建议由 DataCodec 提供标准文件/内存字节 IO 和可转移所有权的具体结果；保留原始数据视图、单元类型映射和 UI 输出接口；iGame 在接收结果后自行组装原生对象及交接智能指针；codec 自身的存储、执行与缓存管理内聚；表面业务及其缓存策略完整划归 Wasm Example。**

## 2. 审查标准与范围

### 2.1 采用的边界

| 内容 | 最终责任 |
| --- | --- |
| 压缩文件读取与写出 | DataCodec 提供支持 mmap/文件映射的自有 IO，调用方传入路径 |
| 文件映射窗口、文件扩展、刷新、解除映射与句柄关闭 | DataCodec 内部及平台层实现，按最后使用时机和文件完成合同释放 |
| 压缩字节的内存输入与编码结果提取 | 输入共享所有权；DataCodec 将完成结果的所有权转移给调用方 |
| 网络、数据库、自定义容器的上层封装 | 调用方取得压缩字节或消费编码结果；标准接入无需实现 codec reader/sink |
| 浏览器 File 的分段读取与必要输入生命周期 | DataCodec 的平台适配提供现成实现，保留按范围读取能力 |
| 原始数据的带类型视图、块树遍历、单元类型映射 | 允许调用方实现；输入数据及访问所需状态共同保持共享所有权 |
| 解码完成结果及数据所有权 | DataCodec 提供具体拥有型结果并转交调用方，结果只保留自身存储和释放所需状态 |
| iGame、VTK 等原生对象封装与智能指针交接 | 各框架在接收结果后自行实现；DataCodec 不定义外部转接器、payload 或租约实现接口 |
| 编解码临时工作区、内部存储选择、预算记账与回收 | DataCodec 实现 |
| 编解码内部任务、工作线程、队列、取消与任务结束等待 | DataCodec 实现；宿主可以在自己的后台线程中调用同步入口 |
| 编码输入复用、参考帧、原始解码结果缓存、淘汰与失效 | DataCodec 实现 |
| 原始数据的源身份、内容版本 | 调用方可提供描述信息，codec 缓存行为由 DataCodec 决定；iGame 原生对象表示由 iGame 管理 |
| 抽壳、表面派生计算及其工作区/执行调度 | Wasm Example 负责，可调用 iGame 通用算法 |
| 表面缓存、跨页表面任务协调、OPFS 表面持久化 | Wasm Example 实现并管理 |
| GUI 事件循环、把完成结果派发到 UI 线程 | 宿主实现 |
| 当前展示模型、GPU 资源、与编解码无关的渲染状态 | 宿主实现 |

原始业务数据通过共享 owner 与只读视图接入，覆盖数据、getter 与访问所需元数据的寿命；读取期间保持内容稳定。压缩字节层的标准入口直接接受路径或内存描述，reader/sink 的实现与构造由 DataCodec 承担。结果由核心完成后转交，单次操作不因调用方保留结果而延长缓存和工作区寿命。iGame 智能指针交接完全在 iGame 自有代码中完成。调用方无需实现预算器、缓存、工作区管理器、线程池或所有权交接模块。资源上限、缓存开关等值参数可以保留。Example 可以为自己的抽壳与展示业务管理独立资源。

编码的原始内存输入与解码的压缩字节内存输入共同遵循第 6.10 节：以视图描述如何读取，以共享 owner 保证寿命，以容量描述支持内存观察。外部输入与核心自有分配分别记账，路径输入的读取缓冲由 DataCodec 自行管理。

本报告的“原始解码结果”指按输入数据语义恢复的几何、拓扑和属性；有损编码按既定精度合同恢复。“派生表面”指对原始数据进一步抽壳得到的展示数据。源数据本来就是 SurfaceMesh 时，DataCodec 继续正常编码/解码该原始网格类型。

### 2.2 覆盖与证据限制

已检查 DataCodec API、Runtime、Storage、Workflow 的相关入口，以及 Filter 下的 Adapter、Playback、Output、Telemetry、Localization、Wasm 桥接；向外追踪了 IGDC Reader/Writer、帧序列 IO、StreamingData、ModelSurface、Qt 读取/压缩/播放/属性管理、Wasm C++/页面/SharedWorker、VTK 示例。

本报告覆盖已定位的桥接合同和实际调用链。第三方压缩库内部、通用渲染器全部实现、无关格式的 Reader/Writer 未做逐行审计。CodeGraph 用于初始定位，结论以当前工作树源码核对结果为准。

未编译、未运行应用、未执行测试或性能测量。下文“确认”表示源码能够确定控制流或类型转换；峰值内存、竞态发生概率和用户环境中的复现情况均未实测。

为便于定位，下文 DC 表示 iGameCore/Filters/DataCodec，IO 表示 iGameCore/IO。所有行号对应上述代码基准。

## 3. 全部桥接职责清单

| 桥接面 | 接口与主要实现 | 当前承担的职责 | 审查结论 |
| --- | --- | --- | --- |
| 单对象编码输入 | IEncodeAdapter、IEncodeAttrView → iGameEncodeAdapter | 几何、属性、拓扑视图；必要的多面体表示整理 | 保留业务适配；统一共享输入寿命；修正 double 默认中转 |
| 块树编码输入 | IBlockTreeAdapter → iGameBlockTreeAdapter | 叶子枚举、层级、名称、叶子 adapter | 职责合理 |
| 单元类型 | ICellTypeMapping → iGameCellTypeMapping | 原生单元编号与通用拓扑语义映射 | 职责合理，保持显式类型检查 |
| 编码帧序列输入 | IFrameSequenceEncodeSource → IGDCFrameSequenceEncodeSource | 逐帧读取及数据 adapter 生成 | 属于获准保留的业务输入 |
| 编码帧序列输出 | IFrameSequenceOutputSink → IGDCFrameSequenceOutputSink | 文件命名、逐帧输出、提交与失败清理 | 移除外部输出 sink 接入；核心按路径/命名描述完成文件输出，内存模式交付各帧编码结果 |
| 字节输入输出 | IByteRangeReader、IByteRangeOutput | ReadAt、WriteAt、Finalize、连续视图和输入 owner | 归内部及平台实现；Filter 接收路径/内存，文件映射由核心自有 IO 管理，见 R15、第 10.7 节 |
| 单对象解码输出 | IDecodeAdapter → iGameDecodeAdapter | 原生几何、拓扑、属性数组及对象提交 | 取消核心向外部原生数组直写；核心交付具体拥有型结果，iGame 自行接管与封装 |
| 帧包与块树组装 | IFramePackageDecodeAssembly、IDecodedFrameAssemblyFactory → iGameFramePackageDecodeAssembly | 原生树、叶子、拓扑复用及完整帧 payload | 核心交付原始层级与数据；原生树组装留在 iGame，移除外部 payload/组装工厂要求 |
| 完整帧持有 | DecodedFrameLease、IDecodedFramePayload | 结果生命周期、输出形式、容量描述 | 以具体拥有型结果交付；单次完成转移所有权并释放内部状态；持续会话独立持有 |
| 普通读取会话 | PackageDecodeSession → DataCodecDataObjectDecodeSession → IGDCReader | 解码、输入缓存、按需属性会话 | 单叶 Reader 未保留会话；有失效配置 |
| 普通写出 | Encode → IGDCWriter → FileWriter | 直接写文件及 iGame 文件框架接入 | 流式主路径合理；继承合同有缺陷 |
| 多帧播放 | PlaybackSession → DataCodecStreamingFrameProvider → StreamingData | 帧请求、展示通知、内部预取 | 内部缓存已存在；宿主缓存控制合同失配 |
| 按需属性 | IDecodedFrameAttributeAccess → DecodedFrameAttributeDataSource；IGDCAttributeDataSource | 属性目录、后台准备、原生数组提交 | 帧身份与 LRU 命中耦合，需修正 |
| PreparedSurface | IDecodeTopologyBlockObserver → iGamePreparedSurfaceDecodeAdapter → ModelGeometryDecodedSurfaceBuilder | 在 codec worker 上构建边界面 | 表面专用流程迁 Example，解除 codec worker 依赖 |
| 帧展示 | iGameFramePresentationBridge | 元数据、拓扑绘制数据复用 | 属于展示适配；帧编号存在窄化 |
| 记录与报告 | IRunRecordSink、DataCodecOutputSinks → iGame 输出与采集实现 | 日志、进度、遥测、报告文件 | 保留输出合同；统一异常隔离 |
| Qt 读取与压缩 | igQtFileLoader、igQtDataCodecCompressionWidget | 参数、创建后台线程、结果入场景 | 宿主后台调用方式可保留；统一异步 API 属于可选整合 |
| Qt 播放与属性 | igQtAnimationWidget、igQtAttributeDataSourceManager | 后台请求、取消、UI 提交 | 保持核心内部任务管理与 UI 派发边界；外围包装按需整合 |
| 配置与本地化 | CodecResourceParams、DataCodecIOSettings、Qt 设置、HostMessage | 参数持久化、语言和宿主消息 | 错误码/阶段标识驱动判断；DataCodec 生成编解码文本，宿主选择语言并显示 |
| Wasm IO | WasmBrowserFileByteRangeReader、datacodec_browser_files.js | 浏览器注册、分段读取、JS/Wasm 数据交付 | 由 DataCodec 平台适配提供，应用使用现成输入入口 |
| Wasm 任务与模型 | iGameWasmDataCodecBridge、iGameWasmDecodedModelRegistry、main_wasm.cpp | async driver、任务状态、模型与源关联 | std::async 包装可保留；Example 管理组合业务任务与模型注册 |
| Wasm 跨页与持久化 | cache.js、index.html | SharedWorker LRU、任务协调、OPFS 表面缓存 | 表面业务归 Example，单独定义结果能力与策略 |
| VTK 示例 | VtkDataCodecAdapters | VTK 类型与拓扑数组映射 | 无外部资源模块注入；有全量中间数组 |
| 构建边界 | DataCodecCore、CheckDataCodecDependencies | 核心与 iGame/Qt 分离 | 主体已独立，公开头文件边界需继续收窄 |

## 4. 优先处理的功能与数据正确性问题

### R01 · 高：几何解码接口固定为 Float32

**确认：支持 Float64 输入的通用核心会在解码输出阶段主动降为 Float32。**

IEncodeAdapter 可以提供 Float32、Float64 几何视图，GeometryEncode 保留源标量类型。IDecodeAdapter 的 WritePointsRange 只接收 float 指针；DecodedGeometryCache 按 float 计算输出容量；GeometryDecode 对非 Float32 数据逐值执行 static_cast<float>。

第三方 double 几何输入即使采用无损编码，往返后的输出仍受 Float32 精度限制。iGame Points 当前使用 FloatArray，iGame 原生 float 输入通常不会新增这一层精度损失。

处理要求：几何输出合同携带原始标量类型、分量数、布局和字节视图；核心按源类型解码。原生输出需要转换时显式声明目标表示与转换策略，并让调用方能够知道精度变化。

证据：DC/API/Adapter/IEncodeAdapter.h:138；DC/API/Adapter/IDecodeAdapter.h:49；DC/Codec/Geometry/GeometryEncode.h:63；DC/Codec/Geometry/GeometryDecode.h:62、237、277；DC/Runtime/Cache/DecodeCache/DecodedGeometryCache.h:43。

### R02 · 高：属性 getter 默认经 double 中转

**确认：非连续属性的默认路径可能损坏 Int64/UInt64 数据，并产生逐 tuple 临时分配。**

IEncodeAttrView::GetTupleBytes 默认创建 vector<double>，调用 GetTuple，再按属性类型转回字节。超过 double 精确整数范围的数值，例如 2^53+1，无法保持原值。每个 tuple 的 vector 分配也是可避免的开销。

iGame 常见原生数组通过 TryGetRawPtr 借用内存，直接绕过此默认路径。风险明确作用于依赖默认 GetTupleBytes 的第三方或 getter-only 输入。

处理要求：将原始标量字节读取设为基础合同；允许连续、带步长和分段视图；double 数值视图作为显式转换能力。删除默认原类型→double→原类型的通用实现。

证据：DC/API/Adapter/IEncodeAdapter.h:55、65、85、94；DC/Common/Views/ArrayViews.h:264；DC/Filter/Adapter/iGameEncodeAdapter.h:155。

### R03 · 高：普通单叶读取丢失按需属性会话

**确认：普通单叶包在 loadAllAvailableAttributes=false 时返回几何/拓扑，Reader 没有交付后续属性源。**

IGDCReader 的帧序列分支保存 sequenceResult.attributeDataSource。普通包分支调用 DecodeDataCodecDataObject，后者创建局部 DataCodecDataObjectDecodeSession，Open 返回后立即销毁会话；Reader 仅保存 output。Qt 的按需属性分支随后调用 GetAttributeDataSource，得到空值。

触发条件是进入普通包分支并选择按需属性。多帧包内每帧只有一个叶子的场景具有独立播放会话，需要单独验收。

处理要求：选择按需属性时显式交付独立的属性访问会话，由会话共享持有输入、属性目录和继续解码所需状态。已交付结果独立拥有数据。一次性读取完成所选数据后收束任务；按需模式的会话由调用方明确保留和关闭。复用已有 PackageDecodeSession 与 iGame 属性桥接，统一普通包和帧包的交付语义。

证据：IO/IGDC/iGameIGDCReader.cpp:568、592、635、657；DC/Filter/Adapter/iGameDataCodecDataObjectBridge.cpp:172；Qt/src/IQCore/igQtFileLoader.cpp:331。

### R04 · 高：播放按需属性依赖完整帧缓存命中

**确认：已有可展示帧的属性入口会因缓存关闭或目标帧不在 LRU 中而丢失。**

DecodedFrameAttributeDataSource::ForFrameObject 从展示对象取 frameIndex，再调用 ForFrameIndex。ForFrameIndex 仅执行 FindCachedFrame，缓存未命中即返回空。Qt 换帧时无论 nextSource 是否存在都会释放旧属性源。

DataCodecStreamingFrameProvider::RequestFrame 将完整结果转换成 DataObject 返回，当前接口没有一并交付帧租约和属性访问句柄。当前帧的业务可用性因此依赖可选缓存。

处理要求：每次交付明确拥有数据的帧结果；按需属性操作通过独立、显式持有的会话及帧身份执行。结果不反向持有会话，属性可用性独立于 LRU 命中。清理或淘汰缓存后，已交付数据继续有效；显式属性会话保持打开时可继续读取属性。关闭会话后释放内部状态，已有数据继续由调用方拥有。

证据：DC/Filter/Adapter/iGameDecodedFrameAttributeDataSource.cpp:56、68、77；DC/Filter/Playback/iGameDataCodecStreamingFrameProvider.cpp:18；Qt/src/IQWidgets/igQtAnimationWidget.cpp:295；DC/API/Adapter/IDecodedFrameAttributeAccess.h:36。

### R05 · 高：Writer 缓冲区接口可截断已写完的文件

**确认：IGDCWriter::GenerateBuffers 实际直接写文件，其后调用继承的 SaveBufferDataToFile 会用空缓冲区覆盖文件。**

GenerateBuffers 调用 EncodeToFile；成功后 m_Buffers 被清空。FileWriter::SaveBufferDataToFile 使用 wb 打开目标文件，再遍历 m_Buffers。组合调用可将完成的输出截断为零字节。

普通 IGDCWriter::Execute 直接进入 EncodeToFile，其正常路径没有执行后续缓冲区保存。本项问题位于保留下来的公开继承合同。

处理要求：使文件框架明确支持直接流式写出；IGDCWriter 退出不匹配的缓冲区生成/保存合同。内存输出通过 EncodeOutput::Memory 返回 EncodedBuffer，文件输出继续 WriteAt/Finalize，避免为兼容旧合同添加整包拷贝。

证据：IO/IGDC/iGameIGDCWriter.cpp:280、294、406、444；IO/iGameFileWriter.cpp:30、58、62。

### R06 · Example 合同：表面缓存结果需要明确声明展示能力

**确认：跨页或持久化缓存恢复表面展示对象，当前 artifact 不含原解码会话和属性目录。这个范围符合派生表面缓存的定位。**

index.html 命中 persistentSurfaceCache 或 SharedWorker artifact 后直接调用 loadSharedSurfaceData，并结束加载。artifact 只有 positions、triangles、edgeMasks。C++ 创建 SurfaceMesh，把 sourceIdentity 放入模型注册表，未恢复 codec 会话。属性目录路径要求 registry entry 的 codec 非空。

三个展示数组可满足表面展示请求。原拓扑处理、按需属性、再次编码等操作需要原始数据或对应会话。Example 应区分“表面已可展示”和“原始数据可用”，按所请求的业务能力选择结果。需要原始数据时由 Example 调用 DataCodec，并管理表面点/面到原数据的映射。

此路径还存在明确的重复持有：JS 字节进入 C++ string 临时缓冲区后，positions 又复制到 Points 与独立 FloatArray 两份长期数组。Points::ConvertToArray 已能返回自身 FloatArray，可用于消除同值双份表示。跨 JS/Wasm 内存域的数据交付仍有独立成本。

处理要求：表面命中可直接满足展示请求，全部逻辑留在 Example。原始解码结果的复用继续由 DataCodec 管理。Example 的模型状态、UI 能力、缓存键和统计明确区分两类结果；重复点坐标由 Example 导入路径合并。表面对象未持有 codec 会话这一事实单独不构成 DataCodec 缺陷。

证据：Examples/Wasm/index.html:2479、2539、2545、2553；Examples/Wasm/main_wasm.cpp:2874、4650、4661、4679、4710；iGameCore/Core/Common/iGamePoints.cpp:82。

## 5. 资源与接口边界问题

### R07 · 必须分离：PreparedSurface 专用流程进入 DataCodec 桥接

**确认：DataCodec 的 Wasm 桥接直接选择抽壳模式、创建表面 adapter、设置原始结果缓存策略字段并附着表面结果。抽壳执行还依赖 codec 的 worker 布局。**

IDecodeTopologyBlockObserver 暴露 workerCapacity、workerIndex、SupportsConcurrentBlocks，以及同一 worker 不重入、并发块借用槽位、driver 完成收束等规则。iGamePreparedSurfaceDecodeAdapter 据此建立 builder、块进度和同步状态。ModelGeometryDecodedSurfaceBuilder 按 worker 建立 FaceMemoryPool，worker 增长时动态扩展；Wasm 分支还创建块内局部 pool 和 FaceHashMap。

PreparedSurface 分支会把 decodedFrameCachePolicy.enabled 设为 false；该字段在当前普通会话路径未被消费，具体见 R11。此处确认的是接口和策略表达的耦合，当前没有据此确认缓存行为已经改变。

这里的并行计算运行于 DataCodec 工作线程，当前没有注入另一个线程池。抽壳工作区由表面业务拥有具有合理性；需要解除的是它与 codec worker 索引、生命周期及缓存状态的耦合。

处理要求：

1. 将 PreparedSurface 专用 adapter、模式选择、表面附着、失败处理、摘要和相应集成测试迁至 Example。
2. DataCodec 交付原始几何、拓扑、属性及其访问句柄；Example 自行执行抽壳并管理工作区、并发和展示结果。
3. 从这条接入链解除 workerIndex/workerCapacity 依赖，禁止用 codec worker 编号组织 Example 内存池。
4. 通用原始数据输出接口可以保留范围/块交付能力，其合同只描述数据、所有权和交付时机。抽壳算法、表面表示及其资源策略保持在 Example。
5. 原始结果缓存键、是否缓存以及成功判定均以原始解码结果为依据。Example 单独持有派生表面，避免把表面附着状态写入共享原始缓存对象。

本项需要同时切断调用、资源和结果语义的耦合。iGame 通用 ModelGeometryFilter 可继续由 Example 调用；为解码融合新增的专用桥接逻辑退出 DataCodec。

证据：DC/API/Adapter/IDecodeTopologyBlockObserver.h:13、24、35；DC/Filter/Adapter/iGamePreparedSurfaceDecodeAdapter.cpp:53、85、91；DC/Filter/Wasm/iGameWasmDataCodecBridge.h:20、50；同名 cpp:135、158、165、213；iGameCore/Filters/ModelSurface/iGameModelGeometryFilter.cpp:1159、1262、1276。

### R08 · 边界明确：表面缓存、跨页协调和持久化归 Example

**确认：页面、SharedWorker、OPFS 缓存和模型注册表承担表面业务的复用与生命周期管理。这些能力保留在 Example，完整原始帧缓存由 DataCodec 自行管理。**

| 所在层 | 实际行为 |
| --- | --- |
| iGameWasmDecodedModelRegistry | FindBySource 查询已加载模型，调用方据此绕过解码 |
| main_wasm.cpp | 同源模型命中后把任务直接标记 Completed |
| cache.js | SharedWorker 保存 artifact，按最近使用淘汰；最多 2 项、512 MiB |
| cache.js | 管理 owner、等待者队列、心跳和超时转交 |
| index.html | OPFS 表面缓存，独立维护最多 2 项、512 MiB 的存储策略 |
| index.html | 优先同页复用，再查持久化，再申请跨页所有权，最后执行读取 |

这些 artifact 是抽壳后的派生表面。SharedWorker 的内存容量与 OPFS 的持久化容量分别由 Example 约束，DataCodec 自有存储额度具有独立含义。基准测试模式跳过复用，正常模式在平台支持时启用这些路径。

处理要求：表面缓存的键、淘汰、失效、持久化、跨页在途合并和统计全部由 Example 实现。DataCodec 不接收表面缓存实现，也不感知 SharedWorker owner、OPFS 表面文件或当前场景模型。Example 可以按源身份激活已有模型并持有核心会话句柄。

表面缓存键可引用 DataCodec 提供的源身份，并加入 Example 自己的抽壳参数、算法/格式版本及表面表示。清理原始数据缓存、清理表面缓存和清理持久化表面是不同操作，分别报告状态。需要应用级“一并清理”时，由 Example 调用各自公开操作。

当前 iGameWasmDecodedModelRegistry 位于 DataCodec/Filter/Wasm，其 modelId、场景复用、输入文件登记与释放属于应用接入逻辑，应迁 Example。保留核心会话句柄属于正常调用方持有行为。

证据：DC/Filter/Wasm/iGameWasmDecodedModelRegistry.cpp:19；Examples/Wasm/main_wasm.cpp:1713；Examples/Wasm/cache.js:4、32、77、137、223；Examples/Wasm/index.html:421、567、694、747、2513。

### R09 · 需要内收：原生输出通过外部 ByteStore 参与核心存储

**确认：原生输出的零拷贝接入范围过宽，外部实现者需要了解内部存储协议。**

IDecodeAdapter::CreateGeometryDecodeStore、CreateConnectivityDecodeStores、CreateAttributeDecodeStore 允许返回 IRandomAccessByteStore。这个接口包含 ResizeBytes、WriteBytesAt，并通过父接口提供 AppendBytes、Seal、读取和容量描述。合同还要求并发写不重叠区间、期间地址稳定、Seal 后可读。

iGameDecodeAdapter::NativeArrayByteStore 为已经存在的数组重新实现了这些方法。它还需要携带原生输出身份，使内部解码缓存识别能否直接提交。

此处是原生业务输出适配，未发现通用 allocator 或预算器注入。按最新所有权合同，原始解码存储由 DataCodec 创建，完成后交付其具体结果；iGame 在核心外完成原生对象封装。

本项讨论解码完成后几何、拓扑、属性的原生数组交付。编码产生的压缩字节，以及进入解码器前的压缩字节，由 R15 的路径/内存入口承载。

处理要求：DataCodec 内部承担结果存储、并发写入和完成状态，返回可转移所有权的几何、拓扑及属性结果。移除公开入口中要求外部实现 ByteStore、payload 和所有权转接的协议。iGame 自行接管结果并组装原生对象，其智能指针桥接无需向 DataCodec 注册。数据层级与标量类型直接交付；保持支持移动接管的存储形态，避免因所有权迁移新增完整中间副本。具体原生容器的接管能力需在实施时验证。

本次迁移取消整个“核心直接填充调用方原生数组”的输出接入方式，覆盖外部解码 adapter 的可写目标、NativeArrayByteStore 和原生帧组装回调。几何、拓扑、属性及后续补充属性均由 DataCodec 生成拥有型结果，再由 iGame 接收和装入原生对象。输入侧继续通过只读视图借用原生数组并共享其所有权。文件输出由 DataCodec 自有 IO 写入目标路径，文件映射资源按第 10.7 节管理。

证据：DC/API/Adapter/IDecodeAdapter.h:34、37、41、44、150；DC/Storage/ByteStore/ByteStoreInterface.h:9、21；DC/Filter/Adapter/iGameDecodeAdapter.h:659；DC/Runtime/Cache/DecodeCache/DecodedGeometryCache.h:54。

### R10 · 可选设计：统一宿主的异步调用包装

**确认：DataCodec 管理自身内部工作线程；Qt 桌面应用与 Wasm 应用分别包装后台调用。现有证据支持“存在多套调用包装”，尚未证明这些包装造成架构耦合、错误取消或资源泄漏。本项撤销“必须内收”的原判定。**

两条调用路径彼此独立：Qt 桌面应用通过 QThread 调用 DataCodec，Wasm 应用通过 std::async 调用 DataCodec；随后均由 DataCodec 管理内部编解码工作线程。这里的 QThread 属于桌面 UI 层，Wasm 所列路径使用 std::async。DataCodec/iGameCore 应保持对 Qt 的独立性，统一异步入口的设计不得引入 Qt 依赖。

| 路径 | 宿主承担的任务行为 |
| --- | --- |
| Qt 文件读取 | QThread::create 内构造并执行 IGDCReader |
| Qt 压缩 | QThread::create 内执行 writer 及相关结果处理 |
| Qt 播放 | 额外 QThread 执行帧请求，内部 PlaybackSession 另有 driver |
| Qt 按需属性 | QThread 准备属性、stop_source、重复请求与当前选择代次检查 |
| Wasm | std::async 启动 driver，future 轮询，维护任务状态与结果 |

这些路径没有把线程池传给 Encode/Decode。宿主在后台线程中调用同步计算库，以避免阻塞界面，属于正常使用方式。外围代码分别安排启动、等待、取消请求转发和结果领取；存在多套包装本身不足以认定核心依赖宿主线程管理。

可选方案：基于现有运行与会话链提供 DataCodec 自有异步入口和具体操作句柄，统一状态查询、取消、等待与结果领取，减少宿主重复包装代码。同步入口与宿主后台调用方式继续作为有效用法。是否采用统一异步 API，根据复用价值和接口需求决定，既有 QThread/std::async 包装不构成必须删除的缺陷。

必要合同：无论选择哪种调用方式，核心负责自己的任务、内部线程、取消后的在途工作收束和资源清理，正式入口无需调用方提供线程池或 executor。最终完成、输入寿命与输出交接遵守第 6 节的同一规则；采用统一异步 API 属于独立设计选择。

GUI 当前选择代次、控件销毁检查和 UI 线程派发继续由宿主管理。Example 独立调度加载、抽壳、表面缓存与渲染组合业务；SharedWorker 协调跨页表面生成和复用。通用属性管理器对其他格式的工作线程继续由对应业务管理。

证据：Qt/src/IQCore/igQtFileLoader.cpp:305；Qt/src/IQWidgets/igQtDataCodecCompressionWidget.cpp:2521；Qt/src/IQWidgets/igQtAnimationWidget.cpp:219；Qt/src/IQCore/igQtAttributeDataSourceManager.cpp:118；DC/Filter/Wasm/iGameWasmDataCodecBridge.cpp:57；Examples/Wasm/main_wasm.cpp:133、1724、1851。

### R11 · 中：缓存配置、动作与统计的语义不一致

**确认：存在对外保留、实际不执行的缓存控制。**

普通 DataCodecDataObjectDecodeRequest 含 decodedFrameCachePolicy，DataCodecDataObjectDecodeResult 含 decodedFrameCacheHit。Open 只向 PackageDecodeSession 传递 encodedInputCachePolicy；完整帧策略没有被消费，hit 未被赋值。PackageDecodeSession 的 DecodedCacheStatistics 读取其资源对象内的默认帧缓存，该普通解码路径没有使用它保存完整结果。

播放侧 ConfigureCacheCapacity 为 no-op，ClearCachedFrames 只重置当前 ordinal。StreamingData::EnableCache、DisableCache、ClearCache 仍调用它们；Qt 动画缓存数量控件也进入这些函数。界面操作与 DataCodec 实际 LRU 行为因此不一致。

处理要求：仅公开真实支持的控制和统计。DataCodec 路径的关闭、清理和查询直接对应内部会话动作。当前完整帧容量由内部策略决定，数量控件应按实际支持能力调整。删除失效字段、占位返回值和无效桥接方法。

证据：DC/Filter/Adapter/iGameDataCodecDataObjectBridge.h:34、55；同名 cpp:68；DC/Workflow/Session/PackageDecodeSession.cpp:218；DC/Filter/Playback/iGameDataCodecStreamingFrameProvider.cpp:57、64；iGameCore/Core/Common/iGameStreamingData.cpp:164、191、202；Qt/src/IQWidgets/igQtAnimationWidget.cpp:551。

### R12 · 中：公开 API 穿透到内部会话和包存储结构

**确认：核心已经与 iGame/Qt 分离，公开合同与内部实现之间还存在直接依赖。**

IDecodedFrameAttributeAccess 包含 Workflow/Session/DecodeSession.h；IFramePackageDecodeAssembly 使用 FramePackage、FramePackageLeafRecord、LeafPackage 等存储模型；帧序列公开入口位于 Workflow 下。DataCodecCore 的 PUBLIC include 目录覆盖整个 Filters 父目录，调用方可以直接包含 Runtime、Cache、ByteStore 等实现头文件。

内部 final 类可被包含与“支持替换一个模块”是两个审查项。当前正式入口缺少外部缓存/线程池工厂；当前头文件边界也没有把实现细节完整隐藏。

处理要求：把业务描述类型、拥有型结果和显式会话操作放在稳定 API 中；把实际 package、cache、resource、worker、byte store 实现置于内部。核心结果直接携带必要的树、叶子、几何和属性数据，iGame 在接收结果后组装原生对象。构建检查应覆盖 API 到实现层的依赖，并验证安装后的消费者只能依赖公开合同。

IDecodedFrameAssemblyFactory::CacheIdentity 当前表达外部组装结果的表示身份。原生对象组装迁至调用方后，删除对应外部工厂要求；核心缓存身份仅描述核心自身的原始数据表示，iGame 的原生对象复用身份由 iGame 管理。

证据：DC/API/Adapter/IDecodedFrameAttributeAccess.h:6；DC/API/Adapter/IFramePackageDecodeAssembly.h:5；DC/API/Adapter/IDecodedFrameAssembly.h:25；DC/Workflow/FrameSequence/FrameSequenceEncodeExecutor.h:36；DC/CMakeLists.txt:18；DC/Cmake/CheckDataCodecDependencies.cmake:35。

### R13 · 高：播放进度回调存在异常隔离缺口

**确认：自定义记录 sink 在播放结束记录中抛异常时，存在异常穿出析构函数并终止进程的路径。**

IRunRecordSink 已提供 TrySubmit/TryExport；RunRecordDispatcher 也隔离下游异常。PlaybackProgressScope 直接调用用户 sink 的 Submit。其析构函数调用 Finish(false)，Finish 内仍直接 Submit；成功路径的 Finish(true) 在回调返回后才设置 m_finished。

触发条件是调用方提供会在对应 Progress 记录中抛异常的 sink，并进入该路径。普通 iGame 输出分发器能保护多种下游失败；公开 Playback 请求也允许直接传入自定义 sink，核心需要自行保证异常边界。

处理要求：所有可选遥测输出统一经过核心异常隔离入口，析构收尾必须 noexcept 安全；导出失败只改变诊断完整性，不破坏业务结果与资源回收。

证据：DC/API/Adapter/IRunRecordSink.h:15、28；DC/Runtime/Record/RunRecordDispatcher.h:57；DC/Workflow/Session/PlaybackSession.cpp:93、112、116、123、683、795。

### R14 · 中：展示身份、属性索引和消息分类仍有间接映射

| 项目 | 判定与影响 | 整理要求 |
| --- | --- | --- |
| 帧编号写入 int 元数据 | 确认 uint32_t 经 int 窄化；读取拒绝负数，超过 INT_MAX 的帧身份无法正确恢复 | 采用无损编号或绑定核心帧句柄 |
| 属性提交后用数组数量推算 nativeIndex | 确认当前实现采用 currentCount-1，并维护以 frameIndex/path/sourceIndex 为键的共享映射 | 从实际输出 adapter 获取索引，将映射绑定到具体输出实例 |
| 帧淘汰后重新创建、属性加载顺序变化 | 共享映射缺少输出实例代次，存在旧索引关联风险；本轮未做运行复现 | 加入专门回归用例，消除依赖追加顺序的推算 |
| 初始加载属性与后续补充属性 | 两条索引记录路径需要统一验收，禁止把 loaded 状态与有效 nativeIndex 分开交付 | 初始和补充提交共同产生明确映射 |
| Qt 判断错误种类 | 确认通过 error.find("版本不符合") 判断错误类型，行为依赖消息文本 | 按精确错误码判断，底层错误码完整传递到调用方；显示文本由 DataCodec 消息模块生成 |
| adapter 判断编码阶段 | 确认通过阶段名字中的 Remap、Topo、Geometry 等子串归类 | 使用阶段枚举，名称只用于显示与诊断 |
| 阶段资源释放 | 编码流水线通过阶段名字识别重排数据的消费者 | 使用明确的资源依赖及最后消费者完成状态，删除对显示名称的依赖 |

证据：DC/Filter/Adapter/iGameFramePresentationBridge.cpp:35、110；DC/Filter/Adapter/iGameDecodedFrameAttributeDataSource.cpp:171、215；DC/Filter/Adapter/iGameDataCodecDataObjectBridge.cpp:115、134；Qt/src/IQCore/igQtFileLoader.cpp:362；DC/Filter/Adapter/iGameEncodeAdapter.h:572。

现有 CodecErrorCode 与 CodecFailureRecord 已提供结构化错误基础，需要补齐精确原因并贯通传递。版本不支持、输入数据不完整和格式错误分别使用明确错误码。当前包头过短的校验路径也返回“版本不符合”，Qt 拼接消息后再匹配该文字会混淆原因。

DataCodec 已提供 DataCodecMessageId、中英文模板与 FormatDataCodecMessage；Qt 的状态接收端调用 DataCodec 格式化函数，再转为 QString 显示。最终分工是错误码用于程序判断，阶段枚举用于阶段识别，明确依赖用于资源释放，消息模板用于文本展示。详细合同见第 10.6 节。

补充证据：DC/Common/DataCodecError.h:19；DC/Validation/Storage/StorageValidator.h:19；DC/Storage/Package/PackageBinaryHeader.h:99；DC/Localization/DataCodecMessageCatalog.cpp:70；Qt/src/IQWidgets/igQtDataCodecUiSink.cpp:16；DC/Workflow/Encode/EncodePipeline.h:972、1027、1042。

### R15 · 需要简化：压缩字节的标准路径/内存入口未统一

**确认：DataCodec 已有标准文件/内存 IO 实现，部分公开请求仍要求调用方构造 reader/sink。编码的内存结果已作为正式能力提供。**

| 场景 | 当前实现 | 最终公开合同 |
| --- | --- | --- |
| 编码到内存 | EncodeOutput::Memory 返回 EncodeResult::encodedBytes，EncodedBuffer 提供 data/size/span 并持有存储 owner | 保留，调用方直接获取压缩字节并自行封装 |
| 编码到文件 | 调用方创建 FileByteRangeOutput，再传给 EncodeOutput::ByteRange | 调用方只传文件路径，DataCodec 创建并管理文件输出 |
| 从内存解码 | DecodePackageRequest 接收 inputReader；MemoryByteRangeReader 已支持 EncodedBuffer、共享 vector 和 owner + span | 调用方直接交付压缩字节与必要所有权描述，DataCodec 内部包装 |
| 从文件解码 | 调用方创建 FileByteRangeReader，再交付 inputReader | 调用方只传文件路径，DataCodec 创建并管理文件输入 |

FileByteRangeReader、FileByteRangeOutput 和 MemoryByteRangeReader 均由 DataCodec 提供。当前普通接入可以直接使用这些实现；当前接口负担在于调用方需要感知并构造底层 IO 对象。IGDCReader::SetMemoryInput 已在 iGame 外层包装 owner + span，IGDCWriter 内部仍显式构造 FileByteRangeOutput。

补充核验：iGame 通用 FileReader 包含 Windows 文件映射和 POSIX mmap；通用 FileWriter 也保留映射写出函数，其正常 SaveBufferDataToFile 路径通过 fwrite 完成。IGDCReader/IGDCWriter 重写了执行入口，当前正常 IGDC 读写使用 DataCodec 的范围 IO，多帧同样使用 FileByteRangeReader/FileByteRangeOutput。当前这些 DataCodec 文件 IO 使用文件流；DataCodec 自有 mmap 支持属于本次确定的目标实现。

处理要求：在现有 Filter 与公开编解码入口内统一文件路径和内存对象描述，将 reader/sink 留在内部及平台适配层，移除允许第三方替换其实现的公开接入点。DataCodec 自有文件 IO 提供 mmap/文件映射支持，负责映射窗口、文件扩展、刷新、解除映射和句柄关闭，并复用现有范围读写、存储和资源管理链。内存输出复用 EncodedBuffer。普通使用者无需继承、构造或管理 codec reader/writer，也无需创建 MemoryByteRangeOutput 或执行资源根。网络传输、数据库写入和容器封装由调用方围绕压缩字节完成。

普通与多帧文件 IO 使用相同的内部实现。多帧路径、帧身份和输出命名通过纯数据描述传入，DataCodec 负责读取、参考依赖、各帧写出、提交及本次失败输出清理；移除要求 iGame 实现 IFrameSequenceOutputSink 的接入方式。编码原始帧的业务加载及只读数据适配继续由 iGame 提供。四种调用方式及路径含义见第 10.5、第 10.7 节。

内存输入采用共享所有权，DataCodec 持有读取期间需要的 owner，输入在持有期间保持不可变。按需属性、播放或异步操作只由对应活动会话/任务延长输入寿命；单次完成解除核心输入持有。内存输出转移所有权，结果不得反向持有执行根、缓存或会话。浏览器的大文件按范围读取复用既有平台实现，避免因简化入口而强制整文件复制。

证据：DC/API/Entry/DataCodecEncodeEntry.h:58、65、69、88；DC/API/Entry/DataCodecDecodeEntry.h:26、27；DC/API/Output/EncodedBuffer.h:15、24；DC/Storage/ByteIO/FileByteRangeIO.h:22、58；DC/Storage/ByteIO/ByteRange.h:100、105、112；IO/IGDC/iGameIGDCReader.cpp:372；IO/IGDC/iGameIGDCWriter.cpp:406、421。

映射与多帧补充证据：IO/iGameFileReader.cpp:79、170、209；IO/iGameFileWriter.cpp:58、120、183；IO/IGDC/iGameIGDCReader.cpp:380、543；IO/IGDC/iGameIGDCWriter.cpp:204、219、280；IO/IGDC/iGameIGDCFrameSequence.h:65。

### R16 · 需要统一：输入共享、输出转移与执行资源释放

**确认：当前输入入口、原生输出组装和结果持有分别采用多套合同，尚未统一为共享输入与转移输出。**

EncodeInput 当前保存 IEncodeAdapter*/IBlockTreeAdapter*，请求类型本身未表达输入共享所有权。DecodePackageRequest 通过 leafAdapter/frameAssembly 向外部对象写入。IDecodedFramePayload 与 DecodedFrameLease 均为公开抽象类型；iGameDecodedFramePayload 持有 DataObject::Pointer，播放 DecodedFrame 同时持有 payload、assembly 和 DecodeSession。

EncodedBuffer 已是不可复制、可移动的具体结果，TakeBytes 移走内部 store。它当前持有 MemoryStore，MemoryStore 继续持有 ResidentByteBudget 和容量租约。该路径证明结果仍携带预算状态；本轮证据未证明 EncodedBuffer 连带持有完整缓存或线程资源。最终实现需要将结果所需存储/释放状态与执行期资源关系拆开。

处理要求：输入共享持有数据及访问状态；完成结果向调用方移动交付；单次操作在所有在途任务结束后解除输入、缓存、参考数据、工作区和内部输出引用。结果只拥有自身数据及最小释放状态，不反向持有执行根、预算管理器、缓存或会话。移交后的数据仍占实际内存，其容量归调用方持有的结果；codec 活动预算中的归属在交接时解除或转出，统计明确区分。

资源释放按第 6.4–6.10 节系统设计执行，覆盖块/字段最后使用、结果交接、文件完成、显式会话关闭和失败取消，并统一两侧的共享内存输入。最后一次使用已经结束的中间数据在对应阶段立即释放，单次结束负责清理全部剩余内部状态。

iGame 的 SmartPointer 使用 Register/UnRegister 管理原生对象引用。输入侧由 iGame 自己将原生智能指针保存在标准共享所有权对象中，DataCodec 仅接收通用输入合同。输出侧由 iGame 自己接管核心结果并绑定到原生对象。DataCodec 不定义 IInputOwner、IOutputOwner、智能指针适配器、payload 工厂或租约实现接口供调用方继承。标准共享所有权值与具体结果类型足以表达交接。

多帧播放和按需属性属于显式持续会话，会话可以保留继续执行所需的输入、参考数据与缓存。已交付结果与会话分别持有；会话关闭时等待任务退出并解除内部保留，结果继续有效。一次性读取与显式会话采用同一数据表示，生命周期分别按操作结束与会话关闭收束。

证据：DC/API/Entry/DataCodecEncodeEntry.h:32；DC/API/Entry/DataCodecDecodeEntry.h:27、29；DC/API/Adapter/DecodedFrameTypes.h:21、29；DC/API/Adapter/IDecodedFrameAssembly.h:14、24；DC/Filter/Adapter/iGameFramePackageDecodeAssembly.h:16、29；DC/Workflow/Session/PlaybackSession.cpp:135、177；DC/API/Output/EncodedBuffer.h:15、31；DC/API/Output/EncodedBuffer.cpp:88；DC/Storage/ByteStore/ByteStore.h:315；iGameCore/Core/Common/iGameSmartPointer.h:21、58、119。

## 6. 内存归属与生命周期

### 6.1 当前真实归属

| 内存类别 | 分配/所有权 | 当前约束 | 最终处理 |
| --- | --- | --- | --- |
| 压缩业务输入 | 调用方对象、reader 或共享 owner | 输入合同保持生命周期和内容稳定 | 统一共享输入合同，单次结束/会话关闭时解除核心持有 |
| 原生输入派生表示 | iGame 多面体面表、VTK 转换数组等 | adapter 自行创建和销毁，部分容量上报 | 允许必要业务适配，减少重复全量表示 |
| codec 计算 scratch | DataCodecExecutionResources 与内部工作区 | 核心准入、复用和释放 | 保持内部实现 |
| 内部自有 ByteStore | ByteStoreSession、MemoryStore、ResidentByteBudget | 内部容量预留、存储选择与释放 | 保持内部实现 |
| 编码输入缓存新分配 | EncodedInputCacheLoader | 申请容量后读取，内部分段填充 | 保持内部实现 |
| 编码输入缓存借用 | 持有原 reader/owner | 既有输入保持原所有权 | 保留借用，统计区分自有与外部 |
| 原生解码输出 | iGameDecodeAdapter 分配业务数组 | 部分容量上报；原生 store 路径直接返回数组包装 | 改为核心结果转移所有权，iGame 自行接管与封装 |
| 编码内存结果 | EncodedBuffer 持有 MemoryStore 及其预算状态 | TakeBytes 已移动 store；结果仍保留容量租约 | 转移结果存储并解除执行期预算关系，结果只保留自身释放所需状态 |
| 完整帧 LRU | 核心缓存持有帧、assembly、session、payload | 条目数与 OptionalRetentionAllowed 控制；底层容量随实际 owner | 保留范围限定为活动任务/显式会话，结束时清空；交付结果独立存活 |
| PreparedSurface scratch | 宿主按 codec worker 分配 pool/map | 目前与 codec 执行布局耦合 | 归 Example 抽壳业务，解除 codec worker 依赖 |
| Wasm 表面复用数据 | 页面、SharedWorker、OPFS、导入数组 | 表面内存与持久化分别约束 | 全部归 Example，单独统计、淘汰与失效 |
| 遥测与报告数据 | 核心记录及宿主输出收集器 | 可选收集/导出，独立失败统计 | 保留输出边界，明确收集成本 |
| 当前展示对象与 GPU | 场景、渲染器 | 宿主业务生命周期 | 保留 |

### 6.2 预算的实际含义

CodecResourceParams 当前使用 ownedStorageLimitBytes，注释明确限定“清单内自有存储”。默认资源模式是 Adaptive；没有显式 maxComputeThreads 时，默认线程模式是 Unlimited，工作线程按需增长；Adaptive 可用内存保留比例省略时采用 0.20。

这些参数的含义应直接由核心合同说明。它们不能被 UI 描述为整个进程内存、全部原生输出、全部浏览器缓存或 GPU 内存的硬上限。

原生几何存储路径直接调用 destination->CreateGeometryDecodeStore，内部普通路径调用 ByteStoreSession::CreateSizedStore。iGame 数组分配和 CapacitySamples 上报没有自动变成 ResidentByteBudget 的容量预留。DecodedFrameLruCache::Store 依据条目索引和 OptionalRetentionAllowed 决定保留，未在该入口按帧总字节建立新的容量租约。

因此当前可以确认“缓存策略在核心”和“内部自有存储受约束”；完整原生输出的全部容量不能据此宣称已纳入同一硬预算。

证据：DC/API/Params/CodecResourceParams.h:30；DC/Runtime/Cache/DecodeCache/DecodedGeometryCache.h:54；DC/Filter/Adapter/iGameDecodeAdapter.h:533、644；DC/Workflow/Decode/DecodePipeline.h:279；DC/Runtime/Cache/DecodedFrameLruCache.h:94。

### 6.3 生命周期统一为输入共享与输出转移

| 阶段 | 调用方 | DataCodec |
| --- | --- | --- |
| 交付输入 | 保持自身所有权，提供共享 owner 与只读数据视图 | 共享持有数据及访问状态 |
| 执行期间 | 按约定保持共享输入稳定 | 拥有工作区、内部缓存、参考数据及待交付结果 |
| 单次操作成功结束 | 接收并拥有完成结果 | 等待在途任务退出，转移结果，解除输入和内部结果引用，释放缓存与工作区 |
| 单次操作失败或取消结束 | 继续拥有原输入 | 等待任务退出，释放半成品、共享输入引用及执行资源 |
| 显式多帧/按需会话运行 | 分别持有会话与已交付结果 | 会话保留继续执行所需输入及缓存，已交付结果不反向持有会话 |
| 显式会话关闭 | 已交付结果继续有效 | 等待任务退出，解除全部内部保留并释放会话资源 |
| 调用方最终释放结果 | 释放结果存储或其原生对象封装 | 单次操作/已关闭会话已完成资源收束 |

结果的所有权转移覆盖编码字节以及解码后的几何、拓扑和属性。具体结果类型由 DataCodec 提供，结果只保留自身数据与最小释放状态。移动接管不要求复制整份数据，也不通过结果反向维持执行根、预算管理器或缓存。

iGame 输入交接代码自行持有其原生 SmartPointer，并用标准共享所有权承载该持有关系。输出交接代码自行接收核心结果并绑定到 iGame 对象。两段代码完全归 iGame，不通过 DataCodec 定义的外部所有权接口实现或注册。

Example 在抽壳期间持有已移交的原始结果，完成后按业务需求释放。派生表面的数组、缓存引用和持久化文件由 Example 独立持有。复用原始数组时由 Example 保持对应结果存活；派生表面状态不进入 DataCodec 原始帧缓存。

EncodedBuffer 的具体类型与移动语义可以复用；其 MemoryStore/预算持有链按 R16 解耦。单次调用的执行资源释放无需等待调用方销毁结果。

Wasm 输入注册表的 Erase/Clear 当前先 ReleaseInput，再移除包含会话的 entry。此顺序属于需要运行验证的生命周期边界：验证在途属性请求完成/取消后再释放浏览器 File 或临时输入。当前审查未证明这里已经发生并发访问已释放输入。

证据：DC/API/Output/EncodedBuffer.h；DC/Storage/ByteIO/ByteRange.h:117；DC/Filter/Wasm/iGameWasmDecodedModelRegistry.cpp:50、58、79。

### 6.4 资源释放的共同规则与粒度

以下为最终设计要求，尚未实施。编码与解码共用现有资源根、容量预算、任务协调及缓存实现，按实际依赖决定释放时机。

每项内部资源需要明确拥有者、当前使用者、后续必需消费者及保留原因。允许的保留原因限定为：在途任务/IO 正在使用、已知后继阶段依赖、显式活动会话的参考依赖、会话内受限的可选复用、等待结果交接。最后一个使用者退出且保留原因消失时，立即解除持有并释放对应容量。

释放按块、字段、叶子、帧、操作、会话逐级进行。同一分配被多个字段共享时，以底层分配的最后一个使用者为准。输入 owner 只覆盖整个 iGame 对象时，DataCodec 在全部依赖结束后解除该对象的共享引用；细粒度独立 owner 可以更早解除各自引用。调用方仍保留原始对象时，其数据继续存活。

阶段完成需要确认该阶段的任务、IO 和同步回调已经结束，并计入重排、校验、封装、后续预测及仍待执行的提交操作。只启动了下游任务不能作为上游缓冲区的释放条件。异步闭包和任务结果队列中的强引用也计入使用者，退出时同步清理。

释放包括解除引用、销毁实际缓冲、归还容量及清理空闲池保留。vector.clear 等仅改变逻辑长度的操作需要结合实际 capacity 检查。池中暂存可复用空间计入内部保留容量；单次操作结束与会话关闭时清空。结果所有权转出单独计量，仍在调用方存活的结果数据继续占实际内存。

### 6.5 编码与解码的具体释放节点

| 路径与资源 | 最早安全释放时机 | 释放动作与保留边界 |
| --- | --- | --- |
| 编码输入共享引用 | 所有读取该数据的阶段及在途 getter 完成，后续预测所需参考已就绪 | 解除 DataCodec 的输入引用；调用方原对象按其自身引用存活 |
| 编码重排表、转换数组、几何源视图 | 重排、拓扑、属性等最后消费者完成 | 逐项销毁中间表示；保留输出所需最小元数据 |
| 编码块 scratch 与压缩器缓冲 | 块结束且任务不再引用 | 归还当前操作内受限复用池，阶段/操作结束清理闲置容量 |
| 已压缩字段与叶子包 | 字节已被目标接收，封装/校验/回填不再读取该缓冲 | 释放字段存储；回填只保留确有需要的偏移、大小和索引 |
| 编码内存结果 | 最终包完成且交接提交成功 | 移出结果存储，解除执行预算关系及内部副本/引用；结果由调用方持有 |
| 编码文件结果 | 全部数据及头部/索引回填结束，Finalize、刷新、解除映射、关闭和最终长度处理成功 | 释放输出暂存、封装索引、映射对象、文件句柄及当前操作状态；不保留完整内存包 |
| 解码压缩输入窗口 | 所有读取该窗口的任务结束，当前后继计算无需原窗口 | 释放读取缓冲或解除映射窗口；显式按需会话保留其继续读取所需源 |
| 文件映射视图与句柄 | 视图的全部在途读写结束，可写窗口已完成必要刷新；句柄的全部使用者结束 | DataCodec 内部解除映射和关闭句柄；单次完成或显式会话关闭后无操作资源残留 |
| 解码块 scratch、重排与中间拓扑 | 对应转换、依赖重建及写入任务的最后消费者完成 | 逐块/逐字段归还，不等待整个模型完成 |
| 解码结果与中间缓存 | 结果完整、所有写入结束并转交 | 移交结果存储，清除不再承担参考依赖的中间表示和核心输出引用 |
| 当前帧参考数据 | 已知后续依赖全部消费完成 | 删除对应参考条目与存储持有；序列结束释放剩余参考 |
| 单次操作的全部剩余状态 | 操作进入最终完成边界 | 释放输入引用、缓存、工作区、任务闭包及执行资源，再交付完成状态 |

内存输出和文件输出均可以在最终交付前释放已无消费者的中间资源。解码字段在准备结果内持有，整体交付前不会被清理逻辑误回收。文件路径保持直接范围写出，避免为了统一交接而先生成整份内存包。

文件写入缓冲的复用需以 IO 已消费该缓冲为准。当前 FileByteRangeOutput 为同步写入，最终完成还包含刷新、关闭和最终文件长度处理。系统设计中的“文件完成”以公开文件输出合同为准，持久化到设备的保证需要在文件合同中单独定义。

目标文件映射实现采用同一完成合同。映射窗口换出、文件扩展和重新映射前，先结束所有使用旧视图的任务；写出窗口按文件合同刷新后解除映射。失败与取消同样等待在途访问退出，再释放视图、映射对象和文件句柄，并处理本次未完成输出。mmap 的虚拟映射长度、活动窗口范围及实际驻留观察值分别统计；完整文件大小和映射长度不直接记作同等大小的新增堆分配。

### 6.6 输出交接、清理与完成状态的顺序

本节规定核心操作的生命周期合同，适用于直接同步调用、宿主后台调用及可选的核心异步入口。核心收束自身工作线程与资源；宿主负责其外围调用线程和事件循环。清理合同不要求采用 R10 的统一异步 API。

单次操作由现有 driver/协调器统一执行以下顺序：

1. 完成所有结果生产、必要校验和文件回填，等待相关在途任务/IO 结束。
2. 准备可独立存活的拥有型结果，或完成文件输出；保留最小结果元数据及值形式诊断快照。
3. 提交交接：内存结果移入返回对象/完成任务的结果槽，解除其执行预算关系；文件结果记录已完成的路径、大小与状态。
4. 清理中间表示、缓存、输入引用、待执行/已完成闭包及空闲工作区，结束当前操作的资源根；显式持续会话只保留第 6.7 节规定的状态。
5. 发布最终成功状态，使同步返回、等待完成和最终完成通知共同具有“内部操作清理已结束”的含义。

内存结果存入异步结果槽后，其数据只由该槽拥有；取走结果后所有权归接收者。调用方尚未取走结果时，该槽也不得反向保留执行根、缓存、输入或工作线程。销毁未领取结果的完成句柄会释放结果自身。进度达到最后计算阶段与最终完成状态分别表达，UI 使用最终完成状态判断结果可交接。

交接成功、失败和取消的最终状态由单一协调路径决定。交接前失败或取消时停止新增任务、丢弃队列中尚未执行的闭包、等待在途工作结束，再销毁半成品及内部状态并发布终态。交接完成后的已发布结果保持有效。清理遵循 RAII 与幂等操作，析构兜底覆盖未完成或异常离开路径。

任务等待和线程 join 由协调线程完成。清理过程中先结束所有使用者，再解除其依赖的存储，最后关闭执行设施。失败文件按标准文件输出合同处理，禁止把未完成包报告为成功。日志、回调和统计保留值快照，避免为生成报告继续持有大数组或执行根。

### 6.7 多帧和按需属性会话的最小保留集

| 状态 | 会话可以保留 | 当次请求结束应释放 |
| --- | --- | --- |
| 顺序多帧编解码 | 已知后续帧需要的参考表示、源身份与最小索引 | 无后继消费者的旧参考、当前帧转换数据、封装缓冲与任务闭包 |
| 可跳转播放 | 数据源/索引、受限原始帧与编码输入缓存、必需参考 | 当前请求 scratch、无依赖的中间表示；可选缓存遵循内部容量与压力策略 |
| 按需属性 | 必要输入 owner/文件源、属性目录、定位与依赖元数据 | 已交付属性的组装状态、临时解码空间及已无消费者的中间副本 |
| 会话空闲 | 继续响应请求所需的最小状态与明确受限缓存 | 当前操作结束后无即时使用者的大块空闲工作区及压缩器缓冲 |
| 会话关闭或已声明序列执行完毕 | 无内部数据保留 | 等待任务退出，清空缓存/参考、解除输入持有、关闭执行设施 |

内部必需参考与可选复用缓存分别记录保留原因。必需参考只保留到最后一个确定消费者结束；可选缓存仅在显式会话中按内部策略受限保留。一次性路径不保留“供未来可能调用使用”的空闲缓存。

可跳转播放到达最后一个时间点仍可能继续接受跳转，资源收束以显式关闭为准。预先声明完整序列的一次性批处理在最后一个任务完成后直接收束。公开调用者无需操作内部 cache、pool 或 scratch；调用方仅控制业务会话的开启、请求与关闭。

活动会话与调用方结果可能涉及相同底层数据，容量统计按实际分配去重。解除核心持有时记录引用解除；底层仍由调用方结果持有时记录所有权归属变化。已交付结果的寿命独立于会话，会话关闭后全部内部引用被解除。

### 6.8 在现有实现上的落实位置

| 既有位置 | 当前可确认能力 | 本轮设计要求的调整 |
| --- | --- | --- |
| EncodePipeline::ApplyAfterStageLifecycleRelease | 跟踪点/单元重排消费者，释放重排及几何源 | 扩展为明确的阶段依赖与最后使用记录，覆盖转换、字段封装及输入持有；删除控制逻辑对阶段名字的依赖 |
| EncodeSessionWorkspace::CompleteLeafReferences/ResetSession | 按 nextFrameReferences 裁剪参考，清理会话存储登记 | 将最后使用与序列完成接入统一释放顺序，保留真实后继依赖 |
| EncodeLeafWorkspace/DecodeLeafWorkspace、ByteStoreSession | 已有工作区与存储生命周期管理 | 在现有对象内落地块/字段/叶子释放点，最终结果存储可以独立交出 |
| EncodedBuffer、MemoryByteRangeOutput::TakeBytes、MemoryStore | 具体可移动结果，TakeBytes 已移出 store；store 保留预算 | 分离独立结果存储与运行期预算/登记，不把整个执行状态交给结果 |
| FileByteRangeOutput | 范围写入、Finalize 刷新/关闭及最终长度处理 | 标准文件入口统一管理创建、完成和失败收尾，按写出进度释放已消费字段 |
| DecodeSession、PackageDecodeSession、PlaybackSession | 已有叶状态、参考、会话 Reset 和内部任务协调 | 明确一次性/持续会话保留集，逐阶段清理，关闭时解除全部内部引用 |
| DecodeCacheRuntime、ResidentByteBudget、ScratchByteBufferPool | 已有缓存裁剪、容量归还和闲置工作区管理 | 将释放策略接入阶段、交接、空闲与关闭节点，并区分真实释放和结果转出 |
| DataCodecExecutionResources、DecodeTaskCoordinator | 已有停止提交、取消、等待及线程收束 | 统一对外终态与清理完成边界；已完成任务句柄只保存结果和最小状态 |

当前 EndRun 检查在途工作已退出，正常路径保留 workers 和压缩器并清理固定 scratch；ShutdownAndJoin 负责最终线程收束与 scratch 关闭。普通单次入口的局部资源根会进入析构。显式会话中的同一资源根可以跨请求存活。验收需要分别验证“请求结束”和“操作/会话最终关闭”，当前静态证据不等于所有路径已满足最终释放合同。

上述调整原位复用现有执行、存储和会话链。所有权转出、最后使用计数和保留原因属于内部状态，不增加外部资源管理器、回收插件或第二套调度器。

证据：DC/Workflow/Encode/EncodePipeline.h:972、1027、1042；DC/Workflow/Session/EncodeSessionWorkspace.h:60、216；DC/Workflow/Session/DecodeSession.h:493；DC/Workflow/Session/PackageDecodeSession.cpp:24、35；DC/API/Output/EncodedBuffer.cpp:68、88；DC/Storage/ByteIO/FileByteRangeIO.h:86；DC/Runtime/Cache/DecodeCacheRuntime.h:23、29；DC/Runtime/Execution/DataCodecExecutionResources.cpp:486、565、1314、1364。

### 6.9 内存节省的验收方法

验证同时记录各检查点的内部活跃分配、闲置池保留、缓存保留、输入共享引用、待交付结果和已转交结果。底层共享分配只计一次，并分别记录解除核心引用、归还预算与实际销毁分配。操作系统 RSS/工作集作为补充指标；分配器保留页和系统文件缓存会影响其下降时机。

必须覆盖以下检查点：块结束、字段最后使用、叶子写出、帧交付、内存结果转出、文件完成、单次返回/等待完成、会话关闭、调用方释放结果。对具有明确阶段边界的数据集验证峰值内存与内部保留容量，确认中间数据在对应后继阶段开始前已经释放。

单次最终完成时，内部输入引用、缓存、工作区和待执行闭包应为零，所属执行设施已关闭；结果仍由调用方或异步结果槽持有。显式会话的每次请求结束只允许保留声明的最小状态和受限缓存；关闭后采用单次最终完成的同等标准。无外部持有时，结果释放后对应分配销毁。

故障注入覆盖读取、块计算、分配、输出写入、Finalize、结果交接前和取消竞争。验证成功结果不被迟到清理回收、失败半成品完整释放、输入存活覆盖全部在途读取，以及异步完成句柄长期保留时执行资源仍已退出。上述验收均为待实施验证。

### 6.10 编码与解码的共享内存输入合同

调用方直接提供内存时，编码与解码采用同一套共享持有原则。内存视图描述可读取的数据，共享 owner 保证该数据及访问状态存活，容量描述支持去重后的内存观察。所有权共享本身不复制完整数据。

| 输入场景 | 读取内容 | 所有权与内存管理 |
| --- | --- | --- |
| 编码内存输入 | 几何、拓扑、属性及必要的带类型视图/getter | 调用方共享持有源数据与访问状态；DataCodec 在最后一次使用后解除自身引用 |
| 解码内存输入 | 已编码的二进制包及其有效字节范围 | 调用方共享持有压缩字节；DataCodec 内部包装读取，任务或显式会话结束后解除引用 |
| 解码路径输入 | 压缩文件及多帧压缩文件集合 | DataCodec 打开文件并管理映射/读取窗口、缓存及文件句柄，调用方只提供路径与业务选项 |

内存输入的基础描述包含只读地址/范围、有效长度或元素形状/标量类型/布局，以及覆盖所有访问依赖的共享 owner。裸指针与长度只描述访问范围，完整输入还需要寿命保证。getter 的上下文、元数据及分段数组共同受到 owner 保护；只保留一个指向已失效对象的 adapter 指针不能满足合同。内存的底层内容与地址在对应使用期间保持稳定。

调用方可以在提交后释放自己的引用，DataCodec 已取得的共享引用持续保护在途读取。正常结束、失败、取消及会话关闭统一执行最后使用后的解除规则。DataCodec 只解除自己的引用；其他调用方引用继续决定原内存的实际寿命。iGame 用自有桥接代码将原生 SmartPointer 存入标准共享持有对象，不新增可供外部实现的内存提供器或生命周期接口。

容量统计同时区分有效数据量与已知底层分配容量。按实际底层分配去重，重叠视图不重复累计；同一对象 owner 可能保护多个独立数组，统计需保留这些分配的区别。无法确定完整分配容量时标记未知，并保留可确定的有效数据量。容量信息是描述值，调用方无需实现预算器或内存统计模块。

外部既有输入单列为“共享输入内存”，用于观察输入规模、保留成本与资源规划；核心自有存储限额仅约束其声明范围内的新分配。共享一个已有输入不重复申请同等大小的自有存储额度。读取、重排、类型转换或内部缓存确实创建的新缓冲按实际分配纳入核心预算；缓存只持有同一输入时记录其保留关系。系统可用内存采样已反映现存外部内存，规划时避免再次全额扣减。

同一内存输入被多个字段、参考帧或活动会话复用时，共享寿命与使用身份分别管理。内容版本和源身份用于正确性与缓存定位，owner 用于存活保障。共享字节在使用期间不被修改，DataCodec 无需通过复制整包建立该保证。

以上属于最终接口与统计设计。当前源码事实继续以 R15–R16 为准，相关实现和运行验收尚未完成。

## 7. 线程关系与额度范围

| 工作 | 当前执行归属 | 目标 |
| --- | --- | --- |
| 块级编解码计算 | DataCodec 自有 workers | 保留 |
| 资源采样/自适应控制 | DataCodec controller | 保留 |
| 播放和属性命令协调 | DataCodec DecodeTaskCoordinator | 复用并统一对外交付 |
| Qt 普通读写后台驱动 | 宿主 QThread | 正常后台调用方式可保留；可选采用统一异步入口 |
| Qt 帧请求等待 | 宿主 QThread + 核心播放 driver | 核心管理内部任务；宿主等待包装可按需简化 |
| Qt 按需属性准备 | 宿主线程 + 核心会话/任务 | 保持核心计算与 UI 派发边界；外围包装可选整合 |
| Wasm 纯解码 driver | 桥接 std::async | 与 Qt 桌面路径独立；现有包装可保留或采用统一异步入口 |
| Wasm 抽壳、跨页表面任务 | 当前混用 codec 回调与 Example/SharedWorker | Example 独立调度，资源不依赖 codec worker |
| 通用原始拓扑输出回调 | DataCodec 交付宿主数据 | 合同描述数据与生命周期，解除 worker 索引依赖 |
| 浏览器 File 读取交付 | 平台线程/浏览器异步 IO | 保留平台 IO 必需机制 |
| GUI 事件处理与渲染 | Qt/浏览器事件循环 | 保留 |
| 与 DataCodec 无关的插值、特征分析等业务 | 对应 iGame/Qt 业务模块 | 不纳入 codec 线程模块替换 |

当前 DataCodecExecutionResources 为具体 final 实现。资源根内部创建 workers 和 controller，播放/会话内部创建任务协调器。没有发现当前正式 Encode/Decode/Playback 请求接收第三方线程池。

宿主外围线程负责调用同步入口，核心内部线程负责其计算任务。报告未据此确认 Wasm 使用 Qt 线程或 DataCodec/iGameCore 依赖 Qt。外层包装的存在不单独构成架构缺陷；R10 仅评估统一异步 API 的复用价值。

**当前额度以运行/会话为单位。** Encode、Decode、PackageDecodeSession、PlaybackSession 各自构造资源根；FrameSequenceEncodeExecutor 在一次序列执行中使用自己的资源根。多个并发入口的额度不能自动解读为进程统一总额。

收束应明确这一范围。产品若要求并发会话共享进程总额度，应在 DataCodec 内部扩展现有资源链完成统一准入，避免要求宿主传入共享预算器或执行器。

证据：DC/API/Entry/DataCodecEncodeEntry.cpp:334；DC/API/Entry/DataCodecDecodeEntry.cpp:61；DC/Workflow/Session/PackageDecodeSession.cpp:70；DC/Workflow/Session/PlaybackSession.cpp:1023；DC/Workflow/FrameSequence/FrameSequenceEncodeExecutor.h:91；DC/Runtime/Execution/DataCodecExecutionResources.cpp:80、410、483；DC/Workflow/Task/DecodeTaskCoordinator.h:84。

## 8. 缓存关系

| 缓存或持有关系 | 当前实现 | 是否要求外部提供缓存模块 | 待处理内容 |
| --- | --- | --- | --- |
| 编码输入 LRU | EncodedInputLruCache，内部默认容量 1 项 | 否 | 维持内部实现，统一实际统计 |
| 输入加载合并 | EncodedInputCacheLoader | 否 | 维持内部实现 |
| 解码参考数据 | DecodeReferenceCache 与内部 transfer/reference 存储 | 否 | 保持内部依赖解析与保留策略 |
| 完整帧 LRU | DecodedFrameLruCache，内部默认容量 2 项 | 否 | 活跃帧能力与 LRU 解耦 |
| 已准备属性 | PackageDecodeSession/DecodedFrame 持有 prepared adapters | 否 | 对外表述为准备/提交结果，隐藏缓存阶段细节 |
| iGame 通用 StreamingData 缓存 | iGame 自身实现 | DataCodec 路径已关闭其普通帧缓存 | 清理 DataCodec 分支的无效控制 |
| Wasm 同页模型复用 | 模型注册表按 source 查找 | Example 应用行为 | 注册表归 Example，区分原始结果与表面结果能力 |
| Wasm SharedWorker 表面缓存 | cache.js | Example 自有缓存 | 全部留在 Example，独立键、容量、调度及统计 |
| Wasm OPFS 表面缓存 | index.html | Example 自有持久化 | 全部留在 Example，独立持久化协议和失效 |
| 渲染数据复用 | FramePresentation/绘制对象 | 业务渲染持有 | 保留展示职责，明确与完整帧语义的关系 |

IStreamingFrameProvider 定义于 iGame 的 StreamingData，当前提供帧请求、展示通知、缓存容量、清理和计数。它属于 iGame 播放集成层。DataCodec 输入读取接口是 IByteRangeReader，帧源由 FrameDecodeSource 描述。

调用方提供路径或共享持有的压缩字节，以及必要的稳定身份/内容版本。核心缓存针对自身原始数据表示工作，iGame 原生输出类型的封装留在 iGame。codec 原始数据缓存的查找、填充、LRU、预取、淘汰、容量与失效均留在 DataCodec，其保留范围限定为活动任务或显式会话。单次结束/会话关闭时释放内部缓存，已交付结果独立存活。抽壳派生结果的对应策略属于 Example。

公开 codec 缓存控制值和查询统计可以保留。GetCache/SetCacheFactory 一类让宿主获取或替换 codec 缓存实现的能力不应作为正式 SDK 接口。Example 自己实现表面缓存不会替换这些模块。当前没有发现 IDecodedFrameCache、IEncodedInputCache、IParallelTaskRunner、IDataCodecMemoryProvider 等旧式注入合同。

证据：DC/Runtime/Cache/DecodeCacheRuntime.h:14；DC/Runtime/Cache/EncodedInputCacheLoader.h:19、113；DC/Workflow/Session/PackageDecodeSession.cpp:30；DC/Workflow/Session/PlaybackSession.cpp:135；iGameCore/Core/Common/iGameStreamingData.h:26。

## 9. 转换与拷贝的逐类判定

| 路径 | 判定 | 原因或动作 |
| --- | --- | --- |
| iGame 连续属性 → NumericArrayView | 合理 | 借用原数组，无需整份类型转换 |
| iGame 点坐标 → float 视图 | 合理 | 当前 Points 原生存储即 FloatArray |
| Points::ConvertToArray | 合理 | 返回已有数组指针，不产生数组副本 |
| iGame 单元类型 → codec 单元语义 | 合理 | 显式语义映射 |
| iGame 多面体输入 → 面表/offset | 有业务依据 | 补齐目标表示需要的数据，控制构建次数及生命周期 |
| 普通文件编码 → IByteRangeOutput | 合理 | 直接范围写出，避免完整成包后再交给 FileWriter 缓冲区 |
| 已有连续输入 → 编码输入缓存 | 合理 | loader 直接持有原 reader/owner |
| 非连续输入 → 核心输入缓存 | 有业务依据 | 缓存主动读取并保留，容量与加载合并由核心管理 |
| Float64 几何 → Float32 输出 | 需修正 | 默认精度窄化，目标类型缺少显式合同 |
| 原类型属性 → double → 原类型 | 需修正 | 默认 getter 有精度风险和重复转换 |
| VTK vtkIdType → IndexType | 有格式依据 | 当前拓扑使用 32 位索引，源码检查点数、单元数、连接总量和点编号范围 |
| VTK 全量拓扑中间 vector → vtk 数组 | 可优化 | 两套完整数组共存；可采用范围读取/写入或原生视图减少副本 |
| 浏览器 File → JS Uint8Array → Wasm 字节 | 有平台依据 | 跨运行时内存域交付，当前读取有固定窗口 |
| Wasm artifact → string → 两份点坐标数组 | 需整理 | 展示导入含全量临时字节及同值长期副本 |
| UTF-8 消息 → QString | 合理 | UI 边界的字符串表示转换 |

VTK 适配器属于可选示例，其属性支持范围也有限。示例路径的全量转换成本需要单独衡量，当前普通 iGame 连续数组路径没有同样的整份转换步骤。

证据：DC/Filter/Adapter/iGameEncodeAdapter.h:155、703、811；Examples/Filter/Codec/VTK/VtkDataCodecAdapters.cpp:299、315、337、743；DC/Runtime/Cache/EncodedInputCacheLoader.h:113；DC/Platform/Wasm/WasmBrowserFileByteRangeReader.cpp；Qt/src/IQWidgets/igQtDataCodecUiSink.cpp:16。

## 10. 建议的最终公开边界

### 10.1 允许调用方实现的业务适配

- 原始业务数据的逐帧加载，以及压缩字节在网络、数据库、自定义容器中的上层封装。
- 带类型的数据视图、必要的业务 getter、块树枚举、单元语义映射。
- iGame 等框架在接收核心结果后的原生对象封装与智能指针交接，完全属于框架自有代码。
- UI、日志、进度、遥测和报告文件接收端。

业务回调应具有明确的调用时机、线程语义、共享输入寿命及异常处理。DataCodec 保证自身调用业务数据回调时的执行合同和错误隔离。压缩字节的标准文件/内存输入输出采用现成入口，调用方无需提供 reader/sink 实现。所有权通过标准共享输入和具体拥有型结果表达，公开边界不要求外部 payload、租约或智能指针转接实现。Example 接收原始结果后的抽壳任务由应用自行调度与处理失败。

### 10.2 只允许调用方使用 DataCodec 提供的实现

- 编解码资源根、预算与容量状态。
- 标准文件/内存字节输入输出，以及浏览器等平台的字节 IO 适配。
- 内部存储包装、工作区、缓冲区复用。
- 编解码工作线程、任务协调器、操作句柄、取消与结束等待。
- 编码输入、参考数据、完整原始帧、按需属性的缓存状态。
- 可移动交付的编码/解码结果，以及独立的播放/属性会话操作句柄。

这些 codec 能力可以暴露值参数、不可客制化的句柄和只读统计。SDK 不接受其抽象实现、工厂或通用函数式替换入口。Example 的表面工作区、任务调度和缓存独立存在于应用层。

### 10.3 宿主保留的职责

宿主选择输入与业务选项，接收输出，把完成通知派发到自身事件循环，管理当前场景和渲染对象。Wasm Example 还负责抽壳、表面属性映射、表面缓存、跨页协调和持久化。DataCodec 请求不要求宿主为 codec 建立 pool、budget、LRU 或 worker 状态。

IStreamingFrameProvider 的 DataCodec 分支只做播放操作与结果绑定。通用 iGame 播放器对其他格式已有的缓存行为单独保留。

### 10.4 Wasm Example 的具体切分范围

| 当前内容 | 目标归属与处理 |
| --- | --- |
| iGamePreparedSurfaceDecodeAdapter 及其抽壳状态 | 迁入 Example 表面处理，移除对 codec worker 身份的依赖 |
| iGameWasmTopologyOutputMode::PreparedSurface | 从 DataCodec 桥接请求中删除，成为 Example 自己的展示流程选择 |
| DecodeiGameWasmDataCodec 内的 surfaceObserver、AttachPreparedSurface | 从解码桥接移出，由 Example 在获得原始结果后执行 |
| 因 PreparedSurface 设置 decodedFrameCachePolicy 的分支 | 删除表面生成状态对原始数据缓存策略的干预 |
| surfaceSummary 与表面耗时/失败 | 由 Example 的应用结果与进度承载；codec 结果保留解码信息 |
| iGameWasmDecodedModelRegistry | 迁入 Example，管理 modelId、应用源登记、会话句柄和输入生命周期 |
| SharedWorker、OPFS 表面 artifact 与策略 | 保持在 Example，所有表面业务协议和策略均不进入 DataCodec/Platform/Wasm |
| PreparedSurface 属性映射和集成测试 | 跟随 Example 表面管线迁移；核心仅测试原始数据 IO 合同 |
| iGame 通用抽壳算法 | 继续作为 iGame 算法供 Example 调用；DataCodec 不调用或编排该算法 |
| WasmBrowserFileByteRangeReader 等通用浏览器字节 IO | 保留平台适配，只承担字节读取及必要输入生命周期 |
| MeshType::SurfaceMesh 的普通编解码支持 | 保留，代表输入数据本身的网格类型 |

最终依赖方向：Wasm Example 调用 DataCodec 获得原始结果，并调用 iGame 算法生成表面。DataCodec 及其通用 iGame 解码桥接不认识 Example 的表面对象、表面缓存和持久化协议。

### 10.5 压缩字节的四种标准交付方式

Filter 与公开编解码 API 共同支持编码到文件、编码到内存、从文件解码、从内存解码。编码文件路径表示压缩结果的输出位置，解码文件路径表示压缩字节的输入位置；文件入口使用路径和业务选项。编码原始内存输入提供带类型的数据视图及共享 owner，解码内存输入提供压缩字节描述及共享 owner。编码内存结果的所有权转移给调用方并独立于执行资源。路径在运行平台可访问的文件系统中解释。

本机文件路径模式由 DataCodec 自有 IO 提供 mmap/文件映射支持，文件输入保持按需访问；文件输出直接写入目标，避免先生成完整内存结果。内存结果能够直接读取或移动持有，无需为了提取字节额外复制整包。多帧、参考帧和按需属性入口共同复用这些源描述及生命周期规则。具体职责及迁移范围见第 10.7 节。

编码前的原生数据视图采用业务 adapter 与共享输入所有权。解码后由调用方自行消费具体结果并封装原生对象，智能指针交接不构成 DataCodec 的可实现接口。压缩字节读取/写出、内部 ByteStore 和原生对象封装分别保持独立职责。

### 10.6 错误判断、阶段状态、资源依赖与文本展示

**错误码用于判断，文本用于展示。** 在现有错误与消息体系上补齐精确原因和传递链，保持 DataCodec 自带消息模块负责生成编解码文本。

| 内容 | 判断依据或交付内容 | 负责方 |
| --- | --- | --- |
| 检测输入、格式、版本及编解码错误 | 精确错误码与相关结构化参数 | DataCodec 校验与编解码逻辑 |
| 根据错误决定处理行为 | 错误码；外层可以附加业务上下文 | DataCodec 或调用方各自的业务逻辑 |
| 判断当前处理阶段 | 阶段枚举与结构化阶段状态 | DataCodec 产生标识，宿主按标识处理 |
| 判断资源能否释放 | 实际使用依赖及最后消费者完成状态 | DataCodec 内部生命周期管理 |
| 生成编解码提示文本 | 消息模板、错误/阶段参数及语言 | DataCodec 自带消息与本地化模块 |
| 弹窗、状态栏、日志窗口等展示 | 接收生成后的 UTF-8 文本，转换为界面所需字符串 | Qt、Wasm 页面等调用方 |
| “请先选择模型”等应用操作提示 | 应用自身的业务状态与本地化资源 | iGame 或 Example |

错误码继续使用现有 CodecErrorCode/CodecFailureRecord 体系，按实际处理需要补齐版本不支持、输入数据不完整、格式错误等精确原因。错误在最先能够准确识别问题的位置产生，相关参数可以包含实际版本、支持版本及出错位置。外层保留原始错误码并附加上下文，避免把具体原因统一覆盖成笼统的“解码失败”。

传递链覆盖底层校验、编解码入口、iGame Reader/桥接及 Qt/Wasm 使用点。调用方直接访问结构化错误记录。拼接后的消息、messageId、reason 文案和本地化文本均不作为错误类型的判断依据；messageId 只标识展示模板，错误码与模板建立明确映射。

DataCodec 消息模块根据模板填入版本号、字段名等参数，生成指定语言的完整句子。调用方选择语言并决定展示方式。现有中英文模板、FormatDataCodecMessage 和状态格式化入口继续承担这一职责。错误记录、展示文本和技术详情保持一致来源，技术详情只用于诊断。

例如，版本校验失败时返回“版本不支持”的错误码及实际版本号；DataCodec 消息模块生成“当前文件版本 8 不受支持”等提示；Qt 根据错误码决定弹窗并显示文本。输入头部不足时返回“输入数据不完整”的错误码，进入对应处理分支。

正常阶段状态使用阶段枚举。资源释放由内部依赖与任务完成状态决定。阶段名称、翻译文本和诊断标签可以调整，错误处理、阶段分类及释放时机保持不变。实现时移除依赖文本内容的分支与阶段名字匹配，不保留字符串判断兼容路径。

验收覆盖：同一错误码在修改文案和切换语言后进入相同分支；不同错误码使用相近文案时正确区分；底层精确错误码经过全部桥接后保留；输入不完整与版本不支持正确分类；阶段名称调整后进度分类及资源释放顺序保持一致。当前这些变更属于待实施设计。

### 10.7 Filter 双模式入口与 DataCodec 自有文件映射 IO

本节为确定的迁移方案，源码实现尚未完成。DataCodec 的 Filter 提供路径和内存对象两种使用模式，核心公开入口采用一致的数据与所有权合同。iGame 的 IGDCReader/IGDCWriter 负责组织业务输入、选项和结果交付；IO 对象的构造及文件映射操作统一放在 DataCodec 自有模块中。

| 场景 | iGame 交付内容 | DataCodec 承担的工作 |
| --- | --- | --- |
| 普通编码到文件 | 原始数据 adapter、共享 owner、压缩文件输出路径及编码选项 | 读取共享原始数据，创建内部文件 IO，编码、映射写出、回填、刷新、解除映射、关闭及清理 |
| 普通文件解码 | 压缩文件输入路径及解码选项 | 创建内部文件 IO，按需映射/读取，解码并交付拥有型结果，释放本次操作资源 |
| 内存模式编解码 | 编码侧提供共享原始数据；解码侧提供共享压缩字节 | 借用只读输入，内部完成编解码，将完成结果的所有权转交调用方 |
| 多帧编码 | 逐帧原始数据来源、共享输入、输出路径及命名描述 | 管理编码参考、各帧文件 IO、提交和本次失败输出清理；内存输出逐帧交付编码结果 |
| 多帧解码与播放 | 压缩帧文件路径描述或共享字节、目标帧请求 | 管理输入 IO、参考依赖、预取及缓存，交付帧结果，关闭会话时清理内部资源 |

路径描述明确区分压缩文件输入和压缩文件输出。VTK 等原始业务文件的格式解析由 iGame 完成，再通过原生数据 adapter 与共享 owner 提供给 DataCodec。多帧路径、帧身份和输出命名使用纯数据描述，具体字段在公开入口实施时统一；这些描述不包含创建 reader/sink 的回调或工厂。

文件映射由 DataCodec 的 Storage/Platform 内部实现：Windows 使用文件映射能力，支持 POSIX 的平台使用 mmap。内部负责只读/可写映射、范围与对齐、活动窗口、文件扩展、重新映射、必要刷新、解除映射及句柄关闭。复用现有范围 IO、ByteStoreSession、资源预算和任务协调链，不建立外部可替换的映射提供器或另一套资源管理器。浏览器 File 输入继续使用 DataCodec 平台层现有的按范围读取能力，保持大文件分段访问；浏览器 IO 与 Example 的表面 OPFS 持久化分别归属原有边界。

映射地址仅供内部活动任务使用，换出窗口或调整文件长度前确认所有相关访问已结束。一次性操作在返回最终完成状态前释放其输入映射、输出映射及句柄；显式播放/属性会话按最小必要状态保留，并在关闭及任务退出后释放。已经交付的结果独立拥有自身存储，不借用即将释放的任务映射或反向持有会话。文件完成包含数据和索引回填、刷新、解除映射、最终长度处理与关闭，错误处理覆盖映射、扩展、写入和收尾失败。

解码输出统一由 DataCodec 分配并完成，随后向调用方转交所有权。删除核心向外部原生数组直写的接入方式，覆盖公开可写解码 adapter、原生 ByteStore、payload/租约实现和原生帧组装工厂。输入侧保留带共享 owner 的原生数组只读借用。iGame 在结果交接后自行接管通用存储、组装原生对象和处理智能指针；交接代码只属于 iGame。

实施范围包括在现有 IO 模块内补齐文件映射能力、接通普通和多帧的路径/内存入口、迁回多帧文件管理，以及统一拥有型输出和释放规则。被替换的公开 IO 接口与外部原生直写实现随对应阶段删除，整改顺序按第 11 节执行。所有步骤保持现有输入借用和分段处理效果；iGame 容器接管结果的能力通过实际交接验证，记录完整数组副本数量及峰值内存，避免新增完整结果中间副本。

## 11. 整改顺序

| 阶段 | 具体内容 | 完成标志 |
| --- | --- | --- |
| 第一阶段：正确性 | R01–R05、R13，补齐帧/属性身份映射 | 保真类型往返、按需属性、缓存开关、Writer 组合调用及异常 sink 用例通过 |
| 第二阶段：业务分离与资源边界 | R07、R09、R16 及第 6.4–6.10 节 | 表面流程迁 Example；落实两侧内存输入共享、分项记账、逐阶段释放与输出转移；宿主无需实现所有权或资源管理协议 |
| 第三阶段：结果与缓存合同 | R06、R08、R11 | codec 原始结果缓存内聚；Example 表面缓存独立，结果能力和清理语义明确 |
| 第四阶段：公开合同与自有文件 IO | R12、R14、R15 及第 10.6–10.7 节，统一错误、阶段、Filter 双模式和 mmap 文件 IO | 普通/多帧采用路径或内存描述；映射实现归核心；外部 reader/sink 接入移除；错误码贯通桥接；阶段与释放判断独立于显示文本 |
| 第五阶段：生命周期与成本 | 按第 6.9、第 10.7 节运行释放检查点、映射故障注入和复制量验收 | 最后使用后及时释放；单次完成/会话关闭时内部状态及映射句柄已清空；调用方结果独立存活 |

R10 的统一异步 API 作为可选接口整合项单独评估，不计入上述必需整改。现有宿主后台调用可以沿用，核心的取消、完成与资源清理合同仍按上述阶段验收。

codec 内部调整复用现有 DataCodecExecutionResources、ResidentByteBudget、ByteStoreSession、DecodeCacheRuntime、DecodeTaskCoordinator、PackageDecodeSession、PlaybackSession，避免建立重复的 codec 资源与执行体系。Example 保留并整理自身表面业务的资源、调度和缓存。被替换的公开入口、表面专用解码桥接和无效配置随对应阶段删除。

本报告中的整改是后续工作建议，本轮没有实施这些源码变更。

## 12. 验收清单

以下为后续实施时应执行的验证，当前状态均为未执行。

| 验收项 | 最小可检查结果 |
| --- | --- |
| 第三方最小接入 | 提供共享原始输入及必要数据 adapter，使用现成路径/内存入口并接收具体结果；无需实现 reader/sink、payload、租约、所有权转接或资源模块 |
| 四种标准字节 IO | 编码到文件/内存、从文件/内存解码均可仅通过公开 API 完成；文件路径不强制完整内存成包 |
| Filter 双模式与路径含义 | Filter 与核心使用同一合同；编码输出路径、解码输入路径明确；原始业务文件由 iGame 解析后共享输入 |
| 自有 mmap 文件 IO | 支持文件映射的平台由 DataCodec 自有实现完成读取与写出；覆盖非对齐范围、窗口边界、文件扩展、回填及最终长度 |
| 映射生命周期与失败清理 | 映射/扩展/刷新失败及取消时无在途访问失效视图；最终完成或会话关闭后映射与文件句柄释放，半成品按输出合同处理 |
| 普通与多帧 IO 一致 | 路径、帧身份及命名以纯数据传入；DataCodec 管理各帧文件及参考依赖；公开请求无外部 reader/sink 工厂或替换入口 |
| 原生直写移除与结果接管 | 外部可写解码 adapter、NativeArrayByteStore 和组装工厂接入已移除；iGame 自行接管结果，验证完整副本数量与峰值内存 |
| 编码结果提取 | EncodedBuffer 在执行结束后仍有效，调用方可直接读取字节或移动持有，无新增完整副本 |
| 输入共享寿命 | 调用方释放自身引用后在途操作仍可读取；任务结束解除核心引用；显式按需/播放会话共享持有其必要输入 |
| 编码与解码内存输入 | 原始数组与压缩字节都通过视图和共享 owner 接入；覆盖 getter 状态、分段输入及 iGame 原生指针桥接 |
| 外部输入容量统计 | 有效长度、底层容量、未知容量分别表达；重叠视图去重、独立数组不漏计，外部输入不重复扣自有预算 |
| 内存输入复制量 | 共享输入不新增完整副本；确有必要的转换/缓存分配单独记账，最后使用后及时释放 |
| 公开 API 边界 | 外部工程仅用公开头构建；无 allocator/pool/cache 工厂或 worker 索引要求 |
| Float64 几何 | 选择 Float32 无法精确表示的 double 值，无损往返保持原类型与原值 |
| 64 位 getter 属性 | getter-only 的 Int64/UInt64 覆盖 2^53+1 和边界值，无 double 精度损失 |
| 连续与非连续输入 | 连续视图走借用路径；getter/分段输入不强制构建整份 double 数组 |
| 普通按需属性 | 单叶包和单个帧包开启按需模式后，显式会话保持可用；关闭会话后已交付数据独立存活 |
| 播放缓存关闭 | 换帧后读取属性成功；当前帧不依赖 LRU 查找 |
| 淘汰与重新解码 | 同一帧多次创建、不同属性顺序，sourceIndex 与 nativeIndex 始终对应当前对象 |
| 缓存控制一致 | 关闭、清理、计数与实际内部状态一致；普通会话无虚假命中统计 |
| Wasm 展示结果合同 | 冷生成、同页/跨页/持久化表面命中均满足声明的展示能力 |
| Wasm 原始数据能力 | 原拓扑、按需属性等请求通过核心会话满足，Example 持有必要源与映射 |
| Wasm 缓存独立性 | 原始结果与表面缓存分别具有清理、统计和容量语义，互不伪报命中或控制状态 |
| Wasm 输入寿命 | 模型关闭、属性在途、任务取消及异常退出后，输入释放顺序正确 |
| Writer 合同 | 流式写出后不存在会截断输出的缓冲区保存调用组合 |
| PreparedSurface 边界 | codec 桥接无表面模式、附着和缓存干预；抽壳工作区及调度由 Example 管理 |
| 表面依赖边界 | DataCodec 不依赖 Example/抽壳算法；Example 通过公开原始数据合同消费结果 |
| 原始 SurfaceMesh | 原始输入为表面网格时继续正常编码、解码及缓存 |
| 任务结束与取消 | 普通读写、播放、属性请求具有一致句柄行为，结束后无悬挂任务访问业务对象 |
| 自定义 sink 抛异常 | Begin/Update/Finish、成功/失败/析构路径均只记录诊断导出失败 |
| 错误码精度与传递 | 版本不支持、输入不完整、格式错误正确区分；底层精确错误码经过 iGame/Qt/Wasm 桥接后保留 |
| 错误判断与文案独立 | 切换中英文或修改提示文本不改变同一错误码的处理分支；相近文案对应的不同错误码正确区分 |
| 阶段与释放判断 | 阶段名称变更不影响枚举分类；最后消费者与在途任务状态决定释放时机 |
| 编解码文本归属 | 调用方使用 DataCodec 自带消息模块生成的文本，选择语言与显示位置；应用操作提示由宿主维护 |
| 固定资源额度 | 清晰验证自有存储准入；外部借用与原生业务输出单独标明范围 |
| 多会话额度 | 验证文档声明的运行/会话范围，进程汇总指标不误称为单次预算 |
| 活跃输出越过任务寿命 | 调用方长期持有结果时，单次操作的输入引用、缓存、工作区和执行资源已释放；结果只保留自身存储/释放状态 |
| 输出所有权交接 | 结果交付后核心解除非必要持有，活动预算中的输出归属明确转出；调用方销毁结果时只释放结果自身数据 |
| 编解码阶段释放 | 重排、转换、块工作区和封装数据在最后消费者完成后释放；记录实际容量与底层分配销毁 |
| 文件编码完成 | 全部数据/索引回填、Finalize、刷新、解除映射、最终长度处理与关闭成功后无内部完整编码包、输出缓冲或句柄残留 |
| 最终完成状态 | 同步返回、异步等待和最终完成通知均发生在操作清理结束后；进度事件不冒充最终完成 |
| 异步结果长期未领取 | 完成句柄仅保留拥有型结果和最小状态；输入、缓存、工作区和执行根已释放 |
| 最后使用与在途任务 | 延迟读取/写入/回调期间资源保持有效；最后消费者退出后及时解除持有 |
| 取消与交接竞争 | 交接前取消清理半成品；已发布结果不被清理；队列闭包与在途访问安全收束 |
| 释放统计真实性 | 分别记录预算归还、核心引用解除、结果转出和分配销毁；共享存储去重，池 capacity 如实上报 |
| 显式会话关闭 | 多帧/属性会话关闭并等待任务退出后，内部输入、参考数据和缓存释放；已交付结果继续有效 |
| iGame 智能指针桥接 | 输入共享及输出接管代码只存在于 iGame；无外部 owner/payload/lease 实现要求，无双重释放和悬挂引用 |
| VTK 与 Wasm 复制量 | 记录全量数组数量及峰值持有，确认修改减少了目标中间表示 |
| Qt 线程交付 | 完成回调只经事件循环修改 UI/场景，codec 计算线程由内部提供 |

## 13. 当前可确认与待验证事项

### 已由源码确认

- 核心已有预算、工作线程、任务协调和三类主要缓存的具体实现。
- 当前正式请求中没有上述旧式外部资源模块注入接口。
- PreparedSurface 的表面专用解码桥接及 worker 绑定 scratch 确实存在，需迁出并解耦。
- Wasm 的表面缓存、跨页协调和 OPFS 持久化确实存在，属于 Example 业务范围。
- 类型窄化、double getter 中转、普通 Reader 会话丢失、播放属性缓存依赖、Writer 合同冲突具有明确代码路径。
- Wasm 表面缓存当前仅恢复展示能力，导入点坐标存在两份长期数组；两项均由 Example 整理。
- API 实现层依赖、无效缓存控制、进度回调异常缺口有直接源码依据。
- 编码已有自带所有权的内存结果；文件 IO 与内存 reader 已内置，路径/内存描述尚未统一进入核心解码请求及文件编码请求。
- iGame 通用文件层存在 mmap/Windows 文件映射；当前 IGDC 普通与多帧正常入口使用 DataCodec 文件流范围 IO。DataCodec 自有文件映射支持尚待实现。
- iGame 编码 adapter 提供原生连续数组借用；解码 adapter 的 NativeArrayByteStore 支持直接写入原生数组。最终方案保留共享只读输入，取消外部原生直写接入。
- 当前编码请求借用 adapter 指针，播放 payload/lease 为抽象接口；帧结果持有组装器和解码会话，EncodedBuffer 持有 MemoryStore 及容量预算状态。
- 编码已有按消费者释放重排/几何源的逻辑与后续帧参考裁剪；EndRun、ShutdownAndJoin、会话 Reset 和缓存裁剪提供了统一释放设计的现有落点。

### 需要运行验证

- 原生输出与内部缓存共同持有时的完整内存峰值、最终释放时点及预算统计。
- iGame 原生容器能否直接接管通用结果存储，以及所有权转移后的实际复制量。
- DataCodec 文件映射 IO 的窗口边界、扩展、回填、刷新、取消和异常释放行为，以及普通/多帧路径的活动映射与峰值内存。
- 单次结果长期存活时输入、缓存、工作区和执行资源的析构时点；显式会话关闭后的相同释放边界。
- 编解码各阶段最后使用、文件完成与异步结果交接时的实际容量下降，以及释放顺序对峰值内存的改善。
- 播放属性旧索引复用风险的具体复现组合。
- 浏览器输入释放与在途属性请求之间的并发行为。
- 各项转换和重复数组对真实数据规模的时间与峰值内存影响。
- Qt/Wasm 各平台的取消、退出、回调异常与资源不足行为。

本轮仅调整本报告，保留既有 ThirdParty/std_compat 未跟踪内容。没有修改源码、执行编译、运行测试或提交版本控制。
