# DataCodec 内存与线程管理代码实现方案

日期：2026-09-08

源码快照：`datacodec/dev`，`df6f459dd`，包含本次读取时的工作区状态

状态：实施中，逐项实现、提交与验证证据由 Memory_Thread_TODO.md 维护；本文中的历史源码核查记录保留其原始时点

## 1. 文档契约

本文是 DataCodec 内存与线程管理改造的完整实现依据，集中规定设计边界、目标类型、接口、所有权、执行流程、自动调节、旧模块删除范围和验收要求。实现目标是简单、高效，适应不同设备与当前系统负载；对象是以生产者、块级计算和顺序消费者为主的单请求数据编解码模块。本文的 C++ 声明是目标接口轮廓，不表示当前源码已经具备这些接口。

现状依据为实际源码及其调用点；旧文档、CodeGraph 的历史符号关系、未接入生产的实验文件不作为实现完成的证据。本文中的现有源码路径相对于 DataCodec 目录，标注“仓库”的路径相对于仓库根目录；新增文件明确标注。

最终代码满足以下约束：

- 公共 API 不接受线程执行器、内存池、分配器、外部缓存实现或 cache runtime
- DataCodec 内部创建执行与容量状态，同一操作的帧、叶、字段、块共享该状态
- 块数据以槽位控制并存份数，清单内跨块存储及全局工作数组以确定容量控制；不存在通用任务内存预测字段
- 基础编码单元和执行窗口使用固定规格，设备适配只调整运行额度；编码侧不配置解码侧的内存、线程和在途块上限
- 不按比例给几何、拓扑、属性、reference、重排、输出及缓存分配内存份额；清单内长期自有存储共享一个根容量状态
- 资源操控模式明确为 Fixed 和 Adaptive，两者共用一个支持运行中更新上限的机制；模式决定目标从哪里来、何时更新，机制负责目标生效
- 自动控制不读取审计合计；审计关闭时控制逻辑保持一致
- 同一请求内，系统压力不自动降低长期容量许可 M；压力响应收缩 C/S、暂停新工作并回收可选留存，真实平台硬能力下降继续即时约束 M
- 保留数据 adapter、输入输出、结果对象、日志、取消及业务 observer
- 原地调整已有执行、ByteStore、workspace 和缓存模块，删除被替代接口及其兼容分支
- 不引入 oneTBB、外部资源 provider、通用 DAG 执行框架或全局 allocator 拦截

全部有效约定在本文内闭合：第 14 节完整规定自动控制策略，第 17 节规定逐项迁移与验收，第 18 节汇总已选定参数及验证边界。实现以本文已经明确的接口、存储用途、失败出口和执行模型为准，不保留交给实现者临时决定的候选分支。

需要删除的旧资源设施限定为线程执行、内存池、缓存容量及资源调度。IEncodeAdapter、IDecodeAdapter、字节输入输出、拓扑 observer、取消和日志回调继续作为数据与业务协议保留。压缩精度、误差要求及编码方式具有独立语义，压力响应不得改变这些要求；误差或精度相关 budget、格式尺寸计算、溢出检查和合法收益探测按算法契约保留。

控制清单与审计清单分别写在具体 owner 的接入点，允许两者覆盖范围不同。清单外对象明确豁免，大小阈值只在本文指定的清单内对象上使用，不建立全局小对象探测体系。宿主借用和第三方内部占用只通过系统环境信号间接影响 Adaptive，不进入自有容量账本。

条件分支必须同时写明输入条件、唯一动作和失败出口。平台能力配置、分配前准入和算法候选选择分别具有明确契约。已经开始的必要操作失败后，调用现有 failure 链结束当前请求；不增加换后端、改算法、减线程后重放的补救路径。第 16.2 节规定允许保留的算法选择边界。

本文使用明确的代码修改动词：“删除文件”要求从仓库及构建清单删除该文件；“删除类型、接口、字段或分支”要求删除对应声明、定义、调用、配置传递和专属测试，不保留空实现、停用开关、别名或兼容包装。含有保留业务代码的文件先迁移业务内容，再删除指定旧代码。运行中对象的处理使用“释放、销毁、解除绑定”，任务使用“停止、完成”，线程结束使用“退出”，这些生命周期动作不表示删除源码。

## 2. 从源码得出的实现约束

| 已读取的现有位置 | 对目标实现的直接约束 |
| --- | --- |
| `Runtime/Execution/DataCodecExecutionResources.h` | 当前只有外部 runner 指针和空指针 inline 分支；需要把该文件改成内部运行状态入口 |
| `Workflow/Encode/EncodePipeline.h`、`Workflow/Decode/DecodePipeline.h` | 当前同步 stage 会组织内部并行；stage 驱动不能作为计算 worker 的普通任务再等待同池子任务 |
| `Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h` | 现有循环包含 ReadElements、单块编码、WriteNumericArrayBlock，适合提取读入、计算、提交三个函数 |
| `Runtime/Cache/TransferCache/ReferenceTransferCacheBuilder.h` | current/reference staging、Exact 或 Probe 选择均发生在块循环中；它们归属同一个块任务，不能拆成各自申请槽位的嵌套任务 |
| `Codec/Topology/Connectivity/ConnectivityTopologyBlockEncode.h` | 当前为全部块保存 artifacts；目标只能保存窗口内尚未消费的 artifacts |
| `Codec/Topology/TopologyDecode.h` | 已有读入前增加在途数的结构；目标延长到真实消费完成才释放槽位 |
| `Storage/ByteStore/ByteStore.h` | MemoryStore 扩容会同时持有旧数组和新数组；ByteStoreSession 会主动 Release 所有登记 source，不能直接照搬为共享 owner 生命周期 |
| `Workflow/Session/DecodeSession.h` | 属性 reference 通过 aliasing shared_ptr 引用整个 leaf workspace；长期存储与执行线程需要分离寿命 |
| `Workflow/Leaf/EncodeOutputWriter.h`、`Workflow/Frame/FrameEncodeExecutor.h` | store session 被移入 bundle，叶结果累积到帧输出；块提交完成不等于输出存储已释放 |
| `Codec/NumericArray/NumericArrayCodec.h` | compressor 缓存使用 TLS，Windows Clang 分支为进程寿命指针；改用会话线程池需要显式决定 compressor owner 的寿命 |
| `Storage/ByteIO/ByteRange.h` | 内置 MemoryByteRangeOutput 用可变 vector 累积完整输出；这条路径需要纳入自有输出规则 |
| `Runtime/Failure/FailureScope.h`、`Runtime/Context/EncodeContext.h`、`DecodeContext.h` | 已有异常捕获与清理；首错保存和复制仍涉及字符串，目标先保存固定大小的首错，再停止与收束，具体见 16.5 节 |
| `API/Entry/DataCodecEncodeEntry.h`、`DataCodecDecodeEntry.h`、`DataCodecEncodeEntry.cpp` | 当前结果主要通过消息 vector 携带详细错误，入口失败会构造日志设施；目标增加内联失败记录，使根对象创建失败时也能返回确定结果 |
| 仓库 `Examples/Wasm/CMakeLists.txt` | 当前存在 Emscripten pthread、ASYNCIFY 和 WASMFS 配置；临时路径名称不能证明后端是外存 |

上述源码快照中的外部内存扩展主要是 decoded-frame cache、encoded-input cache 和 DecodeCacheRuntime 的替换入口，已核查的生产请求与会话入口没有通用外置内存池接口。删除外部资源方案是本次目标约束，不能据此描述当前已经完整实现内外两套内存池。ActiveByteBudget、ResidentByteBudget、LRU 逻辑大小和进程 RSS 的口径分别保留说明，不合并成一份可调度额度。

## 3. 文件和类型落点

### 3.1 原地改造的主体

| 位置 | 目标职责 |
| --- | --- |
| `Runtime/Execution/DataCodecExecutionResources.h`，新增对应 `.cpp` | 内部根对象、运行目标、槽位、控制循环、任务收束和存储状态访问 |
| `Runtime/Execution/ParallelExecution.h`，新增对应 `.cpp` | 自有计算线程池、终端计算任务、有限块循环；保留必要内部 task group，不接受调用方实现 |
| `Storage/ByteIO/ByteBudget.h` | 保留并改造 ResidentByteBudget，加入确定容量的可移动 lease；删除 ActiveByteBudget |
| `Storage/ByteIO/ScratchByteBuffer.h` | 块级缓冲所有者和有限复用，不再申请 scratch quota |
| `Storage/ByteStore/ByteStore.h` | 确定容量的 MemoryStore、现有文件后端、创建前固定后端的内部 store 工厂 |
| `Storage/ByteStore/SegmentedBinaryObject.h` | 组合 source 与一次性消费，删除独立 window budget 以及对共享底层源强制 Release 的调用 |
| `Runtime/Cache/CacheResources.h` | 内部资源访问视图，不持有独立政策和预算 |
| `Runtime/Cache/DecodeCacheRuntime.h` 及现有 LRU 文件 | 内部缓存集合、必要 reference 保留、可选对象淘汰 |
| `Runtime/Workspace/EncodeLeafWorkspace.h`、`DecodeLeafWorkspace.h` | 叶级业务数据和清理，每次运行通过 RunBinding 绑定同一根状态 |
| `API/Params/DataCodecControlParams.h`、`CodecParamDefaults.h` | 接收原 preset 文件中的业务配置类型与默认构造；启动配置只解析一次，运行额度由内部更新入口维护 |
| `Common/DataCodecError.h`、`Runtime/Failure/FailureScope.h` 及现有 Context | 共用固定大小失败记录、首错发布和无新增堆分配的停止收束；数据结果携带同一记录 |
| `Runtime/Execution/DataCodecExecutionResources`、`Log/Report/DataCodecProcessReportJson.h` | 根对象内的有界诊断记录与内部快照；现有报告层在锁外格式化，不新建诊断服务或管理线程 |

### 3.2 确有必要的新增文件

| 新增位置 | 新增原因 |
| --- | --- |
| `Runtime/Execution/DataCodecResourceController.h`，对应 `.cpp` | 将第 14 节实现为可独立测试的状态更新函数；不拥有线程、队列、allocator 或 cache |
| `Platform/ResourceProbe.h`，对应 `.cpp` | 提供平台资源采样和能力检测，平台头文件只出现在实现文件中 |
| `API/Params/CodecResourceParams.h` | 明确的 Fixed/Adaptive 模式选择及少量启动期数值配置，替代逐模块额度与性能档位 |
| `API/Output/EncodedBuffer.h`，对应 `.cpp` | 完整编码结果的数据 owner，隐藏内部容量 lease，不提供分配或资源调节接口 |
| `API/Adapter/DecodedFrameTypes.h` | 从外部缓存接口文件迁出仍被 assembly、属性访问和结果交付使用的数据协议 |
| `Workflow/Session/CodecRunEntry.h` | 仅声明复用已有根状态的内部入口；函数定义留在现有 Encode/Decode entry `.cpp` |

有限块循环放在已有 ParallelExecution 中；本次不新增独立 BlockScheduler、MemoryManager、CacheManager。新增 `.cpp` 继续加入现有 iGameCore 目标，不创建第二套 DataCodec 构建目标。

### 3.3 共用模块的职责与唯一约定位置

编码、解码、reference、重排、封装和 Playback 共用下表中的实现。每个共用模块只维护一份职责和状态，业务章节仅说明数据接入与调用顺序，不另建局部版本。参数或生命周期需要调整时先修改其约定章节，再同步调用点与验收要求。

| 共用模块 | 唯一持有的状态与职责 | 统一约定 |
| --- | --- | --- |
| DataCodecExecutionResources | 一次性操作或持续会话的根对象，持有线程池、执行锁、目标、计数、事件及当前请求状态；M/U 的真实存储位于同一 ResidentByteBudget，执行层取得一致快照 | 4.2 节所有权、4.3 节唯一更新入口、4.4 节请求与根对象寿命 |
| ParallelExecution、RunOrderedBlocks、RunTerminalWork | 同一计算队列与计算额度；块窗口负责有序消费，终端函数负责不可嵌套的计算 | 第 5 节与 6.3 节 |
| SlotLease、计算 lease、HeavyPhaseLease | 分别记录一个在途块、当前执行的计算额度、一个非块阶段；凭证不包含数组，也不互相代替 | 5.4 节准入范围、第 6 节槽位寿命、14.3 节排空 |
| ResidentByteBudget 与分配 owner | 确定容量的原子预约和归还；MemoryStore 持有每笔数组与 lease，扩容事务保持旧新数组真实并存 | 第 7 节；预算上限只经 4.3 节更新 |
| ByteStoreSession、SegmentedBinaryObject、EncodedBuffer | 登记、分段消费和结果交付；底层 owner 的存储与容量寿命保持一致，包装对象不重复记账 | 7.4 节后端规则及 8.1、8.3 节共享存储契约 |
| ScratchByteBufferPool | 根对象中的唯一 best-fit 复用池，按当前数量和单对象阈值保留空闲 buffer | 8.2 节数量规则、12.1 节共用留存策略 |
| DecodeCacheRuntime 与 typed LRU | 查找、必要引用保护、可选条目保留和回收；不维护模块字节份额 | 第 12 节，留存与回收请求统一由 12.1 节定义 |
| ResourceController 与 ResourceProbe | 前者根据采样生成目标与政策，后者提供平台值和有效性；均不直接分配业务数据 | 第 14、15 节；发布仍走 4.3 节 |
| Runtime/Failure 与 CodecFailureRecord | 当前请求唯一首错、无新增堆分配的关键错误记录与收束；入口结果使用同一记录 | 16.1、16.5 节 |
| 根对象内的资源诊断 | 既有运行状态快照与固定容量事件环，说明准入、等待和目标变更原因；不参与控制决策 | 16.4 节 |
| 容量审计 | 独立观察实际 owner，控制逻辑不读取审计合计，关闭审计不关闭关键资源诊断 | 16.3 节 |
| 业务对象接入清单 | 逐项规定实际数组、长度来源、管理方式与释放点；对象包装和所在目录不决定预算归属 | 7.8 节；涉及算法结构的细节由第 9—11 节补充 |

同步规则集中在 4.3、5.3、12.1 和 14.2 节：执行状态在执行锁内修改，容量状态在容量锁内修改，缓存或 scratch 中被取出的对象在锁外析构。不得从 codec、workspace、cache、controller 直接修改另一模块的计数或绕过统一接口。

## 4. 公共 API 和内部调用链

### 4.1 两种资源操控模式与启动配置

公共资源配置只包含一个明确的模式选择和少量数值。模式在 Encode/Decode 开始前确定，Playback 在 Open 前确定，会话运行中不切换模式。数值是否为空不隐式决定模式；空值按对应模式的启动规则解析，字节数零表示禁止该类新增内存容量，不表示无限制。

```cpp
enum class CodecResourceMode {
    Fixed,
    Adaptive,
};

struct CodecResourceParams {
    CodecResourceMode mode{CodecResourceMode::Adaptive};
    std::optional<std::size_t> maxComputeThreads;
    std::optional<std::uint64_t> ownedStorageLimitBytes;
};
```

| 操控模式 | 上限来源与生效时机 | 运行中的行为 |
| --- | --- | --- |
| Fixed，固定上限 | 编解码前解析固定的 M/C/S；显式内存和线程参数直接作为固定上限，未填项按启动环境解析一次；通过 UpdateLimits 发布 | 全程使用这组上限，不根据可用内存变化自动收缩、恢复和预热增长；实际使用量允许低于上限 |
| Adaptive，自适应环境 | 编解码前解析部署天花板 Mmax/P 和初始 M/C/S；通过同一个 UpdateLimits 发布 | ResourceController 根据当前允许环境及压力生成新上限，持续通过 UpdateLimits 生效；数值不超过部署天花板和有效平台硬能力 |

默认选择 Adaptive，延续本文原有的缺省自适应行为；Fixed 是完整、正式的生产模式。两种模式均使用同一个内部线程池实现、容量 state、槽位及失败收束链，不恢复旧管理模块。

`ownedStorageLimitBytes` 的范围始终是清单内长期自有存储，不表示整进程上限。Fixed 中显式值是实际固定 M，Adaptive 中显式值是 Mmax；`maxComputeThreads` 在 Fixed 中是固定活跃计算上限 C，在 Adaptive 中是允许恢复到的计算天花板 P。两者都先校验平台能力。槽位由运行配置确定，不新增对外 slot setter。缓存开关和结果交付方式属于业务选项，缓存不持有独立模块字节份额。

EncodeRequest、DecodePackageRequest 和 Playback 打开配置携带此模式与数值配置；删除 `executionResources`、runner、外部 cache 和 cache runtime 字段。公共接口不提供持续会话的资源更新方法。DataCodec 内部必须提供 4.3 节的运行时更新入口，一次性长请求和 Playback 会话都接入该入口；调用方可见性不限制内部动态调节能力。

从公共 DecodePackageRequest 结构中删除 session 字段及对应解析代码。持续解码通过 DataCodec 自有会话方法进入，同一会话的状态由其内部持有，调用方不向一次性解码拼装内部 workspace 或 reference cache。

公共接口保留两种使用方式，其资源实现相同：

- 一次性 Encode / DecodePackage：创建根状态，执行，等待任务收束，返回结果
- PlaybackSession：Open 时创建根状态，RequestFrame 和补充属性操作复用根状态，Reset 时停止并收束

内部增加 `EncodeInRun`、`DecodePackageInRun` 工作函数，分别定义在现有 `API/Entry/DataCodecEncodeEntry.cpp`、`DataCodecDecodeEntry.cpp`。声明统一放在内部 `Workflow/Session/CodecRunEntry.h`，公共 API 头文件不导出这些函数。它们接收 `DataCodecExecutionResources&` 和已有业务 session；内部递归及跨帧调用只走这些函数。

### 4.2 根状态与所有权

```text
一次性入口或 PlaybackSession
  +-- DataCodecExecutionResources
       +-- 计算线程池
       +-- 共用运行状态，以及仅 Adaptive 启用的压力控制循环
       +-- 一个共享 scratch 复用池
       +-- DecodeCacheRuntime
       +-- shared_ptr<ResidentByteBudget>
  +-- Frame / Leaf / Context / Workspace
       +-- 非拥有的内部执行状态访问
       +-- 自有 store、块任务、业务 reference

已交付且继续存活的数据 owner
  +-- 底层分配
  +-- 容量 lease -> ResidentByteBudget
```

容量状态不能反向持有线程池、workspace 或 PlaybackSession。返回结果仍存活时保留容量状态；一次性入口的线程可按 4.4 节销毁，Playback 线程按会话寿命保留。workspace 被 reference 间接持有时，不得继续解引用已失效的非拥有执行指针；每次运行通过显式 RunBinding 绑定，离开运行后清空该绑定。

根状态范围固定为一次操作或一个持续会话，不建立跨独立 DataCodec 实例的进程级总池。多个实例各自遵守所选模式和部署上限；Adaptive 实例分别观察系统压力，本方案不承诺多实例额度相加仍符合单个实例的上限。

删除 DecodeLeafWorkspace::InheritCacheResourceConfigFrom 和每叶资源 Set/Configure 的声明、定义及调用。workspace 构造时不绑定执行状态；每次 BeginRun 创建 RunBinding，作用域退出时解除绑定，Reset 只清理自身业务状态。ByteStoreSession 的创建不增加另一份根额度。

### 4.3 两种模式共用的上限更新机制

DataCodecExecutionResources 增加唯一内部更新入口，目标接口如下。UpdateLimits 接受具体目标，不读取 CodecResourceMode，不采样环境，不决定是否应该调节。Fixed 的启动配置与 Adaptive 的初始配置、运行中决策都调用它。类型与方法只出现在内部执行头文件，不增加对外动态 setter、资源注入和权限设施。

```cpp
struct RuntimeResourceLimits {
    std::uint64_t ownedStorageLimitBytes;
    std::size_t computeLimit;
    std::size_t slotLimit;
};

class DataCodecExecutionResources {
public:
    explicit DataCodecExecutionResources(const CodecResourceParams& params);
    ~DataCodecExecutionResources();
    std::optional<SlotLease> TryAcquireSlot();
    bool UpdateLimits(const RuntimeResourceLimits& limits,
                      bool gateOpen, ResourceDecisionReason reason,
                      std::string* error);
    void RequestStop() noexcept;
    void CancelAndWaitRun() noexcept;
    void ShutdownAndJoin() noexcept;
    void WaitForChange(std::uint64_t observedEpoch, std::stop_token stop);
    bool TryCopyResourceDebugSnapshot(ResourceDebugSnapshot& output) const noexcept;
    resource::ResidentByteBudget& StorageCapacity() noexcept;
    ScratchByteBufferPool& Scratch() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
```

三个数值分别对应 M、C、S。根状态保存部署计算天花板 P、容量天花板 Mmax、实际在途数 N 和实际活跃计算量 R；M 与实际预约量 U 唯一存储于 ResidentByteBudget，根状态按规定锁序读取一致快照，不另建副本账本。两种模式共用以下范围与正常准入规则，数量运算采用 checked arithmetic：

```text
P >= 1
Qmax = min(P, 8)
Smax = P + Qmax
0 <= M <= Mmax
1 <= C <= P
C <= S <= min(Smax, C + Qmax)

允许读入新块 = gateOpen && N < S && !runStopped && !closing
允许普通计算 = R < C && !exclusiveComputeActive && 存在依赖已满足的已接纳任务 && !runStopped && !closing
```

Qmax 是额外流水线窗口的数量上限，不代表字节数。C=4/S=8 与 C=4/S=5 都是合法目标；增加供给槽位不会自动增加计算额度。第 13.2 节需要多个计算额度的排他任务一次取得其完整额度；第 15 节无 pthread 配置固定 C=S=1。

更新支持升高和降低，M=0 有效；暂停新工作由 gate 表达。更新先校验上述范围和已知平台硬能力，非法更新进入现有 failure 通道，旧目标保持完整。一次有效更新同时发布 M/C/S/gate，增加事件序号并通知所有等待点；热路径继续按真实预约量和真实在途数判断，不预估单块字节。

ResourceDecisionReason 定义在同一内部执行头文件，调用方附带初始化、新请求边界、内部机制验证或控制器决策的固定原因；它仅用于诊断，UpdateLimits 不根据原因改变准入规则。资源目标的有效值、门禁或控制状态发生变化时记录 16.4 节的结构化原因；完全相同的重复目标不作废观察批，也不生成重复变更记录。诊断采样、序列化及导出失败均不修改资源目标。快照方法仅为内部只读诊断入口，不提供动态 setter 或资源注入。

额度下调时，根状态负责请求空闲 scratch 裁剪和可选缓存回收，复用已有 driver/控制事件处理这些动作；内部直接更新测试同样执行这一流程。基础能力不依赖自动控制器额外发出回收命令才能生效，回收的锁外析构与必要引用保护继续遵守第 12、14.2 节。

同步次序固定为先取得执行锁，再取得 ResidentByteBudget 的容量锁；通过预算的内部 SetLimitLocked 写入 M，同时发布 C/S/gate，释放锁后通知。SetLimitLocked 不再次加锁，只允许 UpdateLimits 的已持锁实现调用；删除可被其他模块单独调用的 SetLimit/Reset 上限变更路径。预算申请和 lease 归还只取得容量锁，不在锁内回调执行对象，禁止反向锁序。发布前已成功取得的槽位、计算凭证和容量 lease 全部有效，实际分配尚未完成的已批准 lease 也不撤销。

| 更新情况 | 必须提供的行为 |
| --- | --- |
| M 下调到当前 U 以下 | 保留 U 和所有存活 owner，停止新的正容量预约；可选留存按回收请求释放，必要数据按生命周期释放，不为“达标”清空计数 |
| M 下调后已有任务需要新的必要数组 | 新预约按当前 M 检查；无法满足且没有 7.6 节允许的独立释放路径时进入 failure，不承诺所有任务都能在任意小上限下完成 |
| M 上调 | 下一次预约立即使用新 M，并唤醒待准入 driver；不会自动分配增加的额度，不重放已失败请求 |
| C/S 下调 | 已运行的 R 和已准入的 N 允许暂时高于目标，原任务正常结束；新计算和新块分别等待计数回到允许范围 |
| C/S 上调 | 后续终端工作立即具备新增准入机会，线程按需创建且不超过 P，不改变基础块大小 |

这里的“线程上限”指当前获准的活跃计算额度 C，包含已声明的第三方并行额度。操作系统中已创建的空闲 worker 可以继续休眠，线程池容量天花板 P 与 driver/control 固定线程单列；不将 C 下调描述为立即减少同等数量的 OS 线程。内存上限 M 的口径仍限于确定容量清单，不承诺整进程物理内存硬上限。

“支持运行中修改”是共用机制的能力，“运行中保持不变”是 Fixed 的调用规则，“按环境持续修改”是 Adaptive 的调用规则。Fixed 运行中不再发出资源目标更新，底层 UpdateLimits 仍完整具备升降、唤醒和保留已有凭证的能力。内部测试可以直接调用该机制验证动态语义，不增加第三种对外“手动动态模式”。

模式选择发生在入口配置层；Fixed 的固定目标解析和 Adaptive 的控制器负责目标来源；执行层只接收具体目标。实现时先验证共同机制，再分别验收 Fixed 与 Adaptive 两个生产模式。“先实现机制”只描述代码依赖顺序，不表示 Fixed 是临时阶段。运行中的自动更新仅由 Adaptive 的内部控制器发起，公共调用方只在运行前选择模式和参数。

### 4.4 单次请求与根执行对象的生命周期

一次性入口只有一个顶层请求。Playback 的根对象从 Open 存活到 Reset，其中每条完整帧或属性加载命令是一个顶层请求；当前命令所需的 reference、帧、叶及内部递归复用这个请求，不再次初始化取消状态或创建根对象。

在现有 Impl 中区分根级 closing 标志和当前请求的 stop source、首错、任务完成状态及待准入位置。runStopped 表示当前请求已经停止；gateOpen 只表达资源准入政策。RequestStop 仅停止当前请求并唤醒相关等待，不改写 M/C/S、资源 gate 或长期 owner 的 U。取消效果由 4.3 节的组合谓词立即生效，自动控制器不能通过重新打开 gate 恢复已取消请求。

CancelAndWaitRun 由 driver 调用，停止当前请求、撤销其排队任务并等待正在运行的终端函数结束；被撤销的任务记录取消完成状态，通知其等待者，任务对象在锁外销毁。该方法只等待终端任务收束，不等待 driver 持有的 BlockRecord、workspace 清理或请求完成回调，也不销毁会话的计算池和控制线程。方法返回后，由 driver 按 16.1 节释放未提交记录、结束未完成操作、清理未交付对象并发布一次请求完成通知。这些步骤全部结束才构成请求收束，终端函数不得调用 CancelAndWaitRun 等待自身。

只有当前请求已收束、N/R 为零且没有活跃 HeavyPhaseLease，driver 才能为下一条独立命令建立新的 stop source 和首错状态；不得靠清零计数开始下一条命令。存活输出和缓存占用的 U 继续保持真实。

请求级取消、输入错误及 codec 操作失败结束当前请求，下一条已经接受的独立命令可以在原根对象中执行。失败请求不重放。线程创建、执行队列提交失败或运行状态不变量破坏将根对象标记为 closing，停止接收新命令，并使尚未开始的命令通过现有失败通道完成；禁止自动重建线程池或切换资源方案继续运行。

根拥有者在一次性入口收束或 Playback Reset/销毁时调用 ShutdownAndJoin：设置 closing、停止当前请求、结束待运行命令、等待任务和回调、销毁 worker 上的 compressor、join 自有线程，最后释放根对象。所属 worker、control 和后台 driver 只能发出关闭请求，不能 join 自己；实际 join 由根拥有者线程完成。结果 owner 保留独立容量 state，无需保留根线程。

Playback 的模式、必要 reference 和压力事件/冷却状态跨命令保留，C/S 继续遵守工作类型与压力规则。Adaptive 在下一条独立命令开始前按 14.4 节重新确定 M，Fixed 保持原目标；U 始终反映存活 owner。空闲期间控制线程等待事件，下一条命令开始前取得新鲜采样并作废旧观察批；空闲时间不能当作连续良好样本。HOLD 的 30 s 等待期限属于当前请求，新的独立命令重新建立自身等待起点，同一请求的重复通知不重置期限。

## 5. 线程分工和执行模型

### 5.1 固定角色

| 角色 | 执行内容 | 限制 |
| --- | --- | --- |
| workflow driver | 业务依赖遍历、块读取、顺序提交、完成消费和失败收束 | 每个运行范围同时只有一个 driver，不占计算池 worker |
| 计算 worker | 已满足依赖的终端计算，包含块编解码和明确的单阶段计算 | 数量按需创建，上限 P；任务不提交并等待同池子任务 |
| 资源控制循环 | Adaptive 的平台采样、状态更新、额度发布和回收请求 | Adaptive 使用一个内部控制线程；Fixed 不创建该线程；不执行编解码、不运行用户回调、不持锁做 I/O |

同步入口由当前调用线程执行 driver。Playback 的后台执行和 Wasm 的后台任务由 DataCodec 拥有的一个 driver 线程执行。两者调用同一 RunWorkflow，未引入外部执行器或外部资源方案。同步 API 的调用线程在调用期间属于执行角色，不通过参数注入。

Adaptive 采用独立资源控制线程。当前 IByteRangeReader、writer、字段解压与部分 stage 是阻塞式调用，driver 可能长时间无法处理定时事件。该选择使慢读取、慢提交及全 worker 忙碌期间仍可关闭新准入。Fixed 只在启动解析缺省参数和能力时读取环境，不启动周期采样、压力水位与冷却循环。

DataCodec 自建线程的数量上限：同步 Fixed 为 P 个计算 worker，Adaptive 为 P 个计算 worker 加一个控制线程；带后台 driver 的运行范围各再增加一个线程。第三方内部线程在此数量之外单列，其活跃并行额度按第 13 节计入 C。已创建的空闲 worker 不占 R，OS 线程总数不能用当前 C 代替。Adaptive 的控制线程只在存在运行工作时采样，空闲会话等待事件；跨请求寿命统一执行 4.4 节。

### 5.2 stage 组织的明确取舍

保留 BuildStageSchedule 生成的业务依赖和生命周期释放关系。driver 按稳定拓扑顺序推进，一个时刻执行一个重型 stage；真正的计算并行集中在当前 stage 的基础块中。删除 stage 并行开关、属性 stage lane、属性任务成本排序及内外两层 worker 数配置。

删除独立字段之间的重型 stage 并行实现，计算额度集中用于当前 stage 的块级并行。Fixed 的额度不随字段和工作类型改变；Adaptive 按第 14 节延续同类型目标并处理类型切换。大量微小字段的性能必须单列验证，不能仅用大字段测试说明无退化。

stage 的 Execute 由 driver 调用。可分块 stage 调用 RunOrderedBlocks；Morton、封装及字段外层解压等计算按 5.4 节调用 RunTerminalWork。两个辅助函数都只能由 driver 组织，终端函数不得再次提交并等待同池任务。数据复制和 I/O 可以在 driver 上与 worker 重叠；DataCodec 调用的第三方压缩、解压与 probe 计算均取得计算额度。

### 5.3 自有线程池

ParallelExecution 中的具体线程池只提供内部提交。worker 在同一个执行互斥边界内检查队列与额度：

```text
等待：根对象 closing，或当前请求未停止且存在可以取得计算额度的就绪任务
取出：检查任务的普通/排他执行条件，取得本次计算 lease，增加 R，移出任务
执行：释放执行锁，运行任务
结束：发布成功或失败，归还计算额度，增加事件序号，唤醒 driver/worker
```

有线程平台的计算额度只由 worker 在出队并开始执行时取得，driver 不预占 R。无 pthread 平台按第 15 节由 driver 在实际执行终端函数时取得唯一计算额度，执行后立即归还。排队任务不占活跃计算量。任务队列只接收已持槽位的块及其有界准备工作，或一个已准入非块阶段的有限终端工作；不存在先创建全文件任务闭包再等待的路径。请求停止只撤销该请求的排队任务，worker 等待下一个请求；根对象 closing 才使线程结束。

任务描述、执行队列、R、N、目标 C/S、gate 和通知事件序号使用同一个执行同步边界。缩小 C 后，已经运行的 R 可以暂时大于 C；后续领取等待 R 降到可启动范围。

取消时在锁内移出待执行任务，锁外销毁它们。销毁任务可能归还槽位，不能在仍持执行锁时触发同一锁的再次取得。禁止 detached thread，所有工作线程都可 join。

内部任务类型接收 `WorkerContext&`。WorkerContext 只提供当前 stop token、共享 scratch 访问和该 worker 的 NumericArrayCompressorState，不提供继续排队并等待的入口。底层数值函数显式传递 compressor state，不通过新的 TLS 指针隐藏资源归属。该状态的类型定义留在 NumericArrayCodec 内，执行头文件使用前置声明，具体拥有关系在执行 `.cpp` 中实现。

### 5.4 共用任务准入与阶段边界

RunOrderedBlocks 统一管理块窗口，RunTerminalWork 统一提交并等待一个终端函数。RunTerminalWork 不自动获取 SlotLease 或 HeavyPhaseLease；driver 必须明确该计算属于哪个已经准入的块或非块阶段。普通终端计算取得一个计算额度，package Zstd 的排他额度按 13.2 节取得。

| 工作范围 | 准入与凭证 | gate 关闭后的处理 |
| --- | --- | --- |
| 新的基础块 | 读取和输入分配前取得 SlotLease；准备、计算和提交共同持有它 | 停止接纳新块；已准入块及其有界准备、计算、提交继续完成 |
| 同一块的字段外层解压准备 | ReadNextBlock 的 driver 持有当前槽位，通过 RunTerminalWork 顺序推进一个字段流到当前块就绪；只有计算 lease，不取得第二个槽位或新阶段凭证 | 属于已准入块，继续推进，释放槽位前完成本块消费 |
| 首次建立完整字段目标、重排等非块预处理、首次进入最终输出阶段 | driver 在 gate 开放时取得一个 HeavyPhaseLease；已知必要容量在提交计算前检查；准入未获准时释放临时凭证并按 7.6 节等待或失败 | 尚未准入时等待；已准入阶段继续执行到提交和清理完成 |
| 已准入块或非块阶段内的 append、复制、Seal、header 回填及同一输出的后续增长 | 沿用原凭证；新的长期正容量仍按当前 M 检查 | 不再等待新阶段 gate；实际容量不足或操作失败按第 16 节结束请求 |
| 已有对象的析构、引用释放、取消清理 | 不需要新的准入或计算凭证 | 始终允许执行，不因门禁关闭而停止 |

非块阶段的范围在进入时确定。非空字段目标的建立阶段在目标初始化完成后释放 HeavyPhaseLease，目标 owner 继续按数据寿命保留；后续块流逐块取得 SlotLease，字段 End/Seal 的收尾范围统一见 6.3 节。空字段的创建、End/Seal 和提交在同一个非块阶段内完成。首次最终输出阶段从目标创建和首次必要容量申请开始，覆盖正式写出、最终提交和该阶段清理；只有成功完成或失败收束才释放 HeavyPhaseLease。不得用“整个 Encode 请求已开始”宣称所有未来阶段都已准入，也不得在同一个已准入输出内部重复取得阶段凭证。一个完整块流不持有覆盖全字段的 HeavyPhaseLease，阶段与块窗口的排空规则见 14.3 节。

driver 等待终端函数时不占计算额度，也不持执行锁；取消与失败使用同一事件通知。字段流的解压上下文由 Cursor 独占，每次仅有一个终端函数访问，返回后才推进后续读取；同一块的准备完成后才提交 ComputeBlock，不把准备任务发布为块编码完成。压缩、解压及收益 probe 失败统一结束请求，不在 driver 中执行一个未计入额度的重试。

## 6. 块接口与槽位生命周期

### 6.0 基础粒度清单与编码、解码的边界

下表集中规定本轮实现的粒度。固定规格在一次运行中保持不变，代码版本调整常量需重新验证格式、精度与性能。设备内存变化、UpdateLimits 和自动压力响应均不修改这些规格。

普通数值与 connectivity 的执行批次表示有界窗口中的若干独立基础块，不扩大编码依赖边界，也不将它们重新合并成一个必须整体解码的大单元。polyhedron 保留五条连续状态流，其 cell 执行批次只界定工作缓冲和槽位，不宣称产生独立格式块。固定块读取需要核查具体 provider 没有隐含整数组连续化；确需完整数组的路径按第 7 节列明其必要工作集。

| 对象或步骤 | 基础粒度 | 容量与执行含义 |
| --- | --- | --- |
| 点域几何、点属性的数值编码 | 每个字段每块最多 65536 个 point tuple | tuple 包含该字段的全部分量；尾块取剩余数量，原始字节为实际 tuple 数 × 分量数 × 类型字节数 |
| 单元属性和 connectivity 的单元范围 | 每块最多 65536 个 cell，尾块取剩余数量 | cell 数固定上界，连接索引数量按实际拓扑确定，不能将 65536 个 cell 换算成固定字节量 |
| 普通、reference、区域残差的对应数值块 | 使用所属 point/cell 域的同一块边界 | current、reference staging、候选表示共用该块的一个槽位；predictor 的读取范围受既定算法参数约束 |
| 块内分量计算 | 每次处理一个分量，依次完成当前块的各分量 | 不展开分量级线程任务，不为每个分量分配一份模块额度 |
| polyhedron 的变长连接与辅助流 | 五条流保留格式状态；编码/emit 每个执行批次最多 65536 cells，逐批次串行推进 | 固定 cell 批次不写成新格式块；流窗口、全域索引表和五个完整解码 store 的边界见 10.3 节 |
| 流式 Read/Write、复制及 package 封装 | 每次最多 1 MiB，尾窗口取剩余长度 | 只控制一次搬运量，不改变压缩单元及字段外层 Zstd 流的依赖 |
| Morton 内排叶 | 8 MiB 的固定执行规模，元素数按 10.2 节公式换算 | 这是算法分段规格，不能解释为预留给重排模块的内存份额；整个阶段占用另按真实对象处理 |
| Morton run 读写窗口 | 每次最多 1 MiB | 保持外排格式与排序结果，窗口不随 M/C/S 改变 |
| reference 收益探测 | 4096 个 tuple 样本，适用条件见 16.2 节 | 算法候选比较粒度，不作为内存需求估计 |
| 帧内字段 reference 匹配评分 | `s0=min(E,max(1,intraField.sampleCount))`，现有 sampleCount 缺省为 256 | 保留算法采样设置，在字段依赖准备时锁定，不随 M/C/S 调整；索引数组容量为 s0，去重后的实际索引数决定样本字节，具体见 9.4 节，与上一行块候选探测分开 |
| package Zstd 收益探测 | 最多 1 MiB 样本，适用字段与阈值见 16.2 节 | 与正式封装共用固定窗口规格，不受压力策略修改 |
| 调度与在途计数 | 一个独立基础块对应一个 SlotLease；polyhedron 的一个 cell 执行批次使用一个记录与槽位 | 本轮不增加独立批任务层；按 5.4 节顺序完成准备和块计算，同一块不同时排队两项终端计算；处理 k 个独立块计 k 个槽位。整 Attribute payload 准备按 11.4 节单独取得阶段凭证 |
| 重型预处理、完整字段、reference 和最终连续结果 | 生命周期与实际格式长度确定，不定义虚假的固定字节块 | 重型阶段一次一个，必要连续数组按确定容量准入，超出块槽位寿命的数据继续保留 owner |

编码器不写入供解码器继承的 M/C/S、线程数、模块份额和设备性能档位，也不提供通过改变编码块规格设置解码内存占用的接口。解码器为当前设备独立创建根状态，使用自己的 M/C/S；同一文件可以在不同设备上接纳不同数量的块，并在解码过程中更新这些上限。

文件中的实际块边界、字段外层压缩依赖和必要 reference 仍属于正确解码必须遵守的数据协议。解码资源政策独立于编码设备，单个既有压缩单元的最低解码需求继续由文件和算法决定。读取较大旧块时按第 11 节每次只推进一个记录，遵守所选操控模式的上限规则，不能在失败后动态切碎压缩数据。基础块的固定小规格用于建立通用处理单元，运行内存由接纳数量、活跃计算和长期自有容量共同控制。

### 6.1 目标接口轮廓

```cpp
class SlotLease {
public:
    SlotLease(SlotLease&&) noexcept;
    SlotLease& operator=(SlotLease&&) noexcept;
    SlotLease(const SlotLease&) = delete;
    void Reset() noexcept;
    ~SlotLease();
};

template<class Input, class Output>
struct BlockRecord {
    SlotLease slot;
    std::uint64_t sequence;
    std::optional<Input> input;
    std::optional<Output> output;
    BlockCompletion completion;

    void Retire() noexcept {
        output.reset();
        input.reset();
        slot.Reset();
    }
};
```

根执行对象的完整接口统一见 4.3 节，本节仅定义槽位和块记录。SlotLease 持有执行计数状态的安全引用，不绑定 worker，也不包含估计字节数。BlockRecord 的 slot 首先声明，异常析构时其他成员先析构，slot 最后归还。正常提交由 driver 调用 Retire，先释放输入和结果，再归还槽位；临时仍被 worker 持有的轻量记录引用不会延长业务数据寿命。

每条块流在现有 codec 文件中提供下面三个函数，Input/Output 为其具体类型，不建立通用数据对象继承树：

```cpp
bool ReadNextBlock(Cursor& cursor, BlockInput& input, std::string* error);
bool ComputeBlock(const BlockInput& input, BlockOutput& output,
                  WorkerContext& worker, std::string* error);
bool CommitBlock(Cursor& cursor, BlockOutput& output, std::string* error);
```

ComputeBlock 不持有 writer、下一个块的游标或共享 blockLayouts 的写权限。编码布局先写在本块 Output 中，由 CommitBlock 按序加入最终 metadata。

### 6.2 有限窗口的具体容器

RunOrderedBlocks 保存按准入序号排列的 `deque<shared_ptr<BlockRecord>>`。窗口中的记录数等于本流未归还的槽位数，最多为当前运行曾允许且尚未排空的规模。下调目标不截断容器，不修改活跃记录。

worker 只访问自身记录，在所有块数据访问和局部 scratch 使用结束后发布完成状态，此后不再读写该记录的 input/output；driver 观察到完成发布后才能提交和 Retire。driver 只从队首提交。无需维护全文件 artifacts 数组，也无需为已完成块另建不受限结果队列。

保留格式必需的最终 blockLayouts；它属于随输入规模增长的元数据，不能错误地把块窗口有界解释为整个 metadata 向量常量大小。删除仅为调度而完整展开 SpatialBlockRange 数组的代码，游标按偏移按需生成下一个范围。

`blockLayouts` 外层向量和序列化参数缓冲进入确定容量审计清单，按实际 capacity 报告已覆盖部分；其内部变长成员未覆盖的容量保持未知。解析和创建继续受格式长度、数量及溢出检查约束。首版不为全部元数据成员建立分配管控，超大块数量场景必须单列验证，不能把大型聚合元数据统称为可忽略小对象。

### 6.3 driver 循环

```text
每轮先在执行同步边界内读取事件序号 observedEpoch
处理停止、失败和必要的回收请求

队首已完成：
    CommitBlock，完成实际消费或受控所有权交接
        最后一块在本次提交内完成字段级 End/Seal/Finalize，发布完整字段就绪状态
    Retire 队首记录，释放其剩余输入/结果并归还槽位，再移出记录

仍有新块且 TryAcquireSlot 成功：
    建立记录，ReadNextBlock
    提交终端 ComputeBlock，记录留在窗口中

没有可推进动作：
    在条件变量上等待事件序号不等于 observedEpoch，或发生取消

所有块已准入后：
    消费剩余窗口，最后一个槽位归还前完成收尾，不等待自动增长
```

driver 不能在等待新槽位的阻塞调用中停止消费已完成结果，因此主循环使用 TryAcquireSlot。ReadNextBlock 失败、任务入队失败及记录分配失败均由局部 RAII 归还尚未发布的凭证。

非空块流的收尾属于最后一次 CommitBlock 的同步职责，沿用 6.1 节的三个块接口。Cursor 根据已验证的元素数或布局标识本流最后一个块。driver 在归还该块槽位前完成字段级 End、Seal、Finalize 和就绪发布，收尾计算沿用该块凭证通过 RunTerminalWork 执行，不重新等待 gate。收尾失败由 16.1 节统一清理；不能先使 N 归零，再执行仍需资源的字段收尾。空字段没有块记录，按 5.4 节在一个 HeavyPhaseLease 内完成其创建和收尾。最终 package 输出仍有独立的首次阶段准入，不由字段最后一个槽位提前接纳。

同一次通知可能代表多个完成，driver 每次唤醒重新检查状态谓词。检查可推进动作之前取得 observedEpoch，进入等待时在锁内比较事件序号；检查期间到达的完成不会丢失。完成状态统一使用第 5.3 节的执行互斥锁发布和读取：worker 写完 input/output 后加锁设置 completion，driver 加同一把锁确认完成后在锁外提交。这里不另建原子 release/acquire 协议，notify 只负责唤醒。

计算完成只归还计算额度。输出写入长期内存 store 时，目标存储先取得自己的确定容量；源块与目标同时存在直到复制完成。块数据被移动进长期 owner 时先完成该 owner 的准入，再归还槽位。

## 7. 确定容量控制

### 7.1 只控制清单内跨块存储与全局数组

同一运行范围只有一个根长期容量上限 M 和实际预约量 U。几何、拓扑、属性、reference、remap、累计输出及长期缓存不再持有 `M * 比例` 形式的子额度，也不预留静态份额、相互借额度、按权重重新切分预算。清单内 owner 在实际需要时向同一 ResidentByteBudget 申请确定容量；模块名只用于用途、生命周期、诊断与审计分类。

“长期容量”沿用本方案中 M 的名称，接入依据以 7.8 节为准。其中包含只活过一个全局阶段的大数组，例如全面数校验位图、全点数局部索引表和字段 reference 候选边。它们不受固定块范围约束，不能仅因函数返回时释放就划为块内 scratch。固定大小的阶段工作窗口继续按执行粒度限制，不再增加一层阶段字节预算。

固定块大小、I/O 窗口、有限条目数与清单内的大 buffer 留存阈值属于有界执行和对象保留规则。它们不从 M 中圈出一块内存，也不保证某模块总能得到一定容量。根据物理总量和可用内存推导根天花板、全局上限和压力水位可以使用比例，比例不再向各模块传递。

确定容量控制按存储用途与所有权接入。清单内跨块保留的自有存储使用 ResidentByteBudget；块内临时存储按槽位限制并存份数；宿主与第三方内部的豁免分配不进入该账本。MemoryStore 是存储实现类型，仅凭使用这个类型不能决定需要容量预约。块内 staging 通过块缓冲入口管理，不能因复用了连续存储实现就额外增加逐笔预算等待。

保留 ResidentByteBudget，内部共享 state 存储 `limitBytes`、`reservedBytes` 和必要统计。TryReserve 检查确定的下一笔分配量，成功后返回 move-only lease；预约未获准时立即返回空结果，交回上层处理。这里的“返回空结果”只表示本次没有取得容量许可，不直接宣告整个编解码请求失败。停止新增可选留存的政策由根执行状态按用途执行，必要提交和释放遵守第 14.3 节，不在预算内设置会阻挡已有工作收束的通用等待门禁。

```cpp
class ResidentByteBudget {
public:
    class Lease;
    std::optional<Lease> TryReserve(std::uint64_t allocationBytes);
    CapacitySnapshot Snapshot() const noexcept;
private:
    void SetLimitLocked(std::uint64_t limitBytes) noexcept;
};
```

`reservedBytes` 表示存活分配及已获准尚未完成的分配容量，不是审计值。分配成功之后才向独立审计报告 allocated；失败销毁 lease，不能留下实际占用事件。SetLimitLocked 仅作为 4.3 节统一发布的内部步骤，保留所有已有 lease 的计数；零上限有效，运行期不调用 Reset 清账。

同一受控 MemoryStore 被多处引用时，每笔存活数组分配只有一个容量 lease。LRU entry、workspace、bundle 和 reference 引用不另申请同一份容量。容量 state 使用 shared_ptr 独立存活，lease 销毁不访问根执行对象。

这套机制分三层，各层职责固定：

| 层次 | 执行什么 | 未满足条件时由谁处理 |
| --- | --- | --- |
| ResidentByteBudget::TryReserve | 在同步边界内检查上限并登记下一笔确定容量；不调用 new，不读系统可用内存，不查询审计 | 立即返回空结果，由直接调用者按用途继续处理 |
| 清单内 MemoryStore 等分配 owner | 持有已获准 lease，执行实际分配和复制事务，分配成功后持有数组 | 预约被拒绝与实际分配失败分别向上传递；失败事务回滚，保留原有数据 |
| workflow driver | 了解阶段顺序、必要输入和能够独立完成的释放步骤，决定准入时机 | 按 7.4 节在操作开始前执行固定的用途规则；必要内存尚未准入时只按 7.6 节延后，无可推进的释放路径时调用 failure |

move-only lease 是一次确定容量的自动归还凭证：移动只转移归还责任，不能复制出多份额度；销毁时归还本次预约。它不包含实际字节数组，也没有从操作系统预先锁定物理内存。申请数组期间计数已经增加，数组分配失败时 lease 随回滚归还，数组成功创建后 lease 随 owner 保留到真实释放。

例如上限为 1 GiB，已有预约 700 MiB，再申请完整新数组 600 MiB：TryReserve 立即返回空。在调用点确实存在独立消费步骤的前提下，driver 完成该步骤，实际归还 400 MiB，再次申请即可通过。两次检查之间只保留待准入描述，线程池和提交端能够继续推进。若原有 700 MiB 必须等待这笔 600 MiB 分配成功才可释放，按 7.5 节返回必要并存容量不足。这个数值例子用于解释许可语义；现有完整输出链是否存在这样的释放者，以 7.7 节源码核查为准。

等待放在 driver 的目的，是让掌握依赖和提交顺序的层继续推进释放，并统一处理取消。分配器本身无法判断当前容量占用是否依赖正在申请的操作完成。终端 worker 内必要存储的预约拒绝直接作为当前操作错误上报，不能切换后端、调用隐藏阻塞的 Acquire，也不能请求 driver 回收后在原调用栈中等待。已知容量的必要数组由 driver 在提交终端任务前完成准入。

同一未开始的申请只在 7.6 节允许的待准入位置重新检查；唤醒事件包括明确的容量释放和有效目标更新。缺少独立释放路径且已确定容量不足的请求直接失败，不为等待未来 M 上调延长寿命。第 7.3 节在预约前计算一个目标容量，同一次准入只检查这一笔容量。实际 new/malloc/vector 分配失败执行第 16 节收束；失败的编码、解码和外部提交不重新入队。

### 7.2 根容量天花板、初值与运行期上限

模式层先解析部署天花板 Mmax/P，随后按 Fixed/Adaptive 确定实际初值。以下环境公式用于启动解析，以及 Adaptive 在完整请求边界重新确定 M；不在当前请求的每个字段或阶段重新执行，不据此隐式启用 Fixed 的运行中自动调节。

```text
存在显式 ownedStorageLimitBytes：
    Mmax = 用户指定值，并校验已知地址空间/运行时硬限制

未指定且有可靠 Te：
    Mmax = Te / 4

未指定且缺少可靠 Te：
    Mmax = 64 MiB，并截断到已知较小的运行时硬上限

有可靠 Te/Ae：
    环境内存初值 = min(Mmax, max(Ae - H, 0) / 2)

缺少可靠 Te/Ae：
    环境内存初值 = min(Mmax, 64 MiB)

Fixed：
    M0 = 显式 ownedStorageLimitBytes；未指定时取环境内存初值
    C0 = P
    S0 = min(Smax, C0 + 1)
    运行前通过 UpdateLimits 发布 M0/C0/S0、gateOpen=true 和初始化原因，此后模式层不更新这组上限

Adaptive：
    M0 = 环境内存初值
    C0/S0/gate 按 14.6、14.10 节的启动与信号规则确定
    运行前发布初值，运行中按第 14 节调用同一个 UpdateLimits
```

P 取显式线程参数并校验启动平台能力；未指定时取平台允许的并发度。无 pthread 平台在运行前明确选择 C0=S0=1 的执行能力配置。Fixed 的固定 C0 表示允许的最高活跃量，线程按工作需求创建，算法串行步骤不需要用满 C0。Adaptive 缺少可靠内存信号时使用 C0=S0=1 并停止自动增长；该保守状态只属于 Adaptive。

这是全局启动策略，不拆分模块份额，不估计单块内存，也不保证 M 以内的分配一定成功。M=0 时禁止新增长期自有正容量，借用数据与文件路径按既定规则工作。Fixed 的 M0=0 在本次运行中保持为零；Adaptive 的 M0=0 且 Mmax>0 表示控制器可在条件满足后提高许可。固定模式不因当前可用内存低于显式上限而悄悄改用 Adaptive 的初值。

机制必须允许运行中升降 M，Fixed 的运行规则保持 M 不变。Adaptive 通过第 14.4 节调用同一个 UpdateLimits：同一请求内保持必要存储的已有许可，条件满足时允许提高 M；真实平台硬能力下降即时限制 M，新的独立请求开始前重新按环境确定 M。两种模式都保留既有容量 lease、必要 store 和已选后端的生命周期语义。内存上限改变不触发数据迁移，文件 I/O 的物理页变化不作为堆容量归还事件。

### 7.3 MemoryStore 分配事务

用于长期自有存储的 MemoryStore 继续采用自身拥有的连续数组，本节容量事务适用于清单内受控 owner。对于已知总长的整字段 store，在第一次分配前确定完整目标容量；它不采用每块 append 反复扩大整字段容量。槽位内临时缓冲的扩容沿用其容器行为，不据此新增逐笔容量预约。

追加 store 扩容的次序固定为：

1. 校验 required 的长度与地址空间边界；在分配前读取确定的剩余额度，按下述规则计算唯一目标 capacity
2. 旧分配的 lease 保持有效，为完整新 capacity 取得新 lease
3. 分配新数组，复制旧内容；任何失败保留旧数组及旧 lease
4. 发布新数组，销毁旧数组并归还旧 lease

required 不超过当前 capacity 时直接复用，不预约。确需扩容时，令 K 为旧 capacity，Q 为容量快照中的 `max(M - U, 0)`，减法使用防下溢比较。required 超过 Q 时不发起实际分配，返回准入未获准。其余情况取 `capacity = min(max(required, K + max(K / 2, 1)), Q)`，增长项限制在可表示的数组容量以内；已知总长的首次分配直接取完整长度。随后只调用一次 TryReserve(capacity)。没有“几何增长拒绝后再按 required 申请”的分支；发生并发状态变化导致预约拒绝时，返回原准入结果。`old + new` 的瞬时并存量真实进入受控容量。

逻辑缩短只修改 size，普通 Resize 不为了缩容再次分配，不提供 Shrink 重分配入口。独占 owner 的 Reset 和最终析构释放数组；共享消费者只放下自身引用。压力回收释放整个空闲 owner。

### 7.4 创建前固定存储后端

在现有 ByteStore 工厂中使用内部用途枚举，调用方传用途与必要逻辑大小，不传 memory budget、allocator 或工厂回调。下面的分支只在创建 owner 前执行，选择结果保存到该 owner，直至销毁保持不变。File 指现有 FileBackedStreamStore；范围访问统一经过 Read/WriteBytesAt，不依赖整文件 mmap。此用途工厂不保留 mmap 失败后切流式文件的路径。

| 用途 | 固定处理规则 |
| --- | --- |
| 块 raw、component、reference staging、候选结果 | 属于槽位，使用块缓冲；不变成独立长期 store 预算 |
| 已知长度、全程支持范围访问的 reference、整字段目标及 remap provider | driver 完成到期释放和第 12 节准入前回收，然后检查完整长度 A。TryReserve(A) 获准即创建定长 MemoryStore；未获准且平台具备真实外存能力时，直接创建 File。平台无外存时按 7.6 节判断必要内存准入 |
| 算法明确要求完整连续视图的必要数组 | 固定 MemoryStore，按完整容量准入；不足按 7.5、7.6 节处理，不改用文件模拟成功 |
| 最终长度未知的字段、叶、帧编码追加暂存 | 真实外存平台从第一笔写入起固定 File；无外存平台从创建起固定受控 MemoryStore，每次增长检查容量。增长不足直接失败，不在写入中迁移 |
| 可选完整输入预读 | 读取前检查开关、压力状态和完整容量许可；未获准即不启动预读，使用原 reader 的范围读取。已启动的分配及读取失败进入 failure |
| 可选缓存留存 | 按第 12 节固定条目与压力规则保留已有 owner；准入前回收只释放可选引用，不执行失败后的重放 |
| 明确请求完整内存编码输出 | 成功交付必须拥有完整、连续的内存结果；分配前允许按 7.6 节延后。容量规则确定无法满足、没有可推进的释放路径或实际分配失败时终止请求；不静默改成文件结果 |

对已知长度的范围存储，TryReserve 返回空是工厂的分配前许可状态，尚未发生数组分配、文件创建和数据写入。这一处 Memory/File 规则直接写入用途工厂；必要连续数组没有该分支。后端一旦选定，new、文件创建、读取、写入、Seal 任一步失败均结束当前请求，不重新选择后端。

内部存储实现不包含可切换 store 包装、追加中复制转外存及两次扩容申请。已发布和未发布 store 使用相同的固定后端规则。未知最终长度的追加结果不使用压缩率预测选内存，也不先试写内存再决定存储类型。原生平台的小结果同样经过文件暂存，这是本次选择的 I/O 成本，纳入第 17 节性能验收。

终端 worker 只向已经选定的 store 写入。已知定长目标在入队前完成容量准入；无外存平台上的未知长度 append 在增长时即时检查，拒绝即上报失败。worker 不反向等待 driver 回收，不尝试其他存储实现。

首版 I/O 窗口集中使用 1 MiB 校准默认值，尾窗口按剩余长度缩短。每条活跃流只有有限输入/输出窗口，不再各自申请 ActiveByteBudget。这个值是执行参数，和文件基础块规格分开。

### 7.5 必须满足的内存需求与失败条件

“必须满足”描述成功执行与交付的前提，不要求在发现任务时立刻分配。任务可以在分配前停留于有界待准入位置。开始使用数据之前必须取得所需存储，返回成功之前必须完成约定的结果交付。临时不足、配置容量不足、平台能力不足和已经发生的分配失败分别处理。

下表只列入需要确定容量控制的必要自有存储。这些对象跨过单块生命周期或随完整数据规模增长，即使 C=S=1 也可能持续累积，不能依靠槽位数量限制总量。容量判断与等待规则适用于它们实际拥有的内存分配；组合对象、共享引用及文件逻辑长度不另计一份容量。

| 必要需求 | 确定大小的时点与口径 | 必须满足的条件及失败边界 |
| --- | --- | --- |
| 累积的字段、叶、帧编码暂存 | 无外存平台随实际块结果确定 required 和 capacity，自有数组逐笔预约；真实外存平台采用文件追加，文件长度不记入 M。bundle/segment 引用不重复收费 | 内容保留到封装消费或有效交接；后端在首次写入前固定。受控 MemoryStore 增长不足失败；等待未完成字段被后继封装释放没有推进路径 |
| 完整内存编码结果 | 已知最终布局时按完整 package 长度 F 申请；长度随压缩产生时，只能准确知道当前 required 和下一次 capacity，F 在封装完成后确定 | 成功时持有完整连续结果；预约涵盖分配容量及与必要输入的并存量。不得用预计压缩率承诺成功，F 能放下也不自动证明生成它的过程能放下 |
| DataCodec 自有完整解码字段与必要 reference | 按经过检查的元素数 × 分量数 × 标量字节数确定逻辑长度，实际数组容量单独记录 | 范围 store 按 7.4 节在创建前固定后端；完整连续视图固定使用 MemoryStore 并满足容量。仍被本次解码使用的 reference 不能作为提前释放候选 |
| 自有必要重排、逆重排及清单内不可分块全局数组 | 按实际数组元素数 × sizeof(元素) 确定每笔容量；变长拓扑按已经验证的计数确定 | 已有外排/provider 能承担的部分采用其后端；不可替代的连续数组和同一步必须并存的数组需要满足容量。等待当前步骤完成才可释放的数组不能用于抵消当前申请 |
| 上述受控数组的扩容和复制交接 | 下一笔完整新 capacity，加仍存活的旧数组及其他必要 owner；移动同一 owner 不产生第二笔分配 | 扩容是同一 owner 的分配事务，不建立额外管理机制。禁止先释放旧内容再等待新容量，禁止仅按增长差额判断能否完成 |

其他必要内存按下面的边界处理，不用“必须能分配成功”推导它们应进入上述容量账本：

| 对象 | 管控方式 | 与预算等待的关系 |
| --- | --- | --- |
| 单块 raw、分量转换、reference staging、残差、候选结果和待提交 payload | 共用块槽位，限制并存块数；具体缓冲容量按算法需要分配，精确审计仍按覆盖清单进行 | 块内不增加逐笔字节预约。取得槽位前可以等待；已准入块的真实分配失败按错误路径收束 |
| 宿主 adapter 创建的完整几何、拓扑和属性结果，外部 sink 自行分配的存储 | 分配管控豁免；可确定的宿主逻辑输出量按既定边界单列 | 创建失败需要传播，不能作为新增 ResidentByteBudget 等待或拒绝机制的依据 |
| 宿主输入及由宿主提供的既有 reference 的借用视图 | 不审计其底层存储，不接入容量分配管控 | DataCodec 只维护借用寿命；复制形成的自有数据按其用途重新归类 |
| Zstd、Pressio 等第三方库内部的分配 | 不审计，不接入分配管控，使用公开性能参数 | 不向库内部安装预算等待逻辑；库报告内存耗尽时按错误结果收束 |
| 有限 I/O 窗口、参数、锁、任务状态和其他清单外对象 | 窗口按固定执行规模限制，轻量状态按有界队列限制；其余明确豁免，元数据审计范围仍按 6.2 节 | 正常处理创建失败，不为统一错误处理扩大容量清单 |

单块 raw 的长度仍可按元素数、分量数和类型确定，C=S=1 仍需满足一个不可拆编码单元的实际需要。宿主 BeginPoints/BeginAttribute 仍需成功创建约定结果。这些条件用于格式、平台能力及错误传播，不等于增加逐笔预算控制。

DataCodec 自己创建的受控 reference 在 workspace 之间共享时，原 owner 的容量 lease 继续有效；新视图不重复收费，共享也不将这份自有存储转为宿主豁免数据。

可选完整预读和 Morton key cache 在创建前确定是否启用，未获得容量许可时不启动；可选完整帧留存和空闲 scratch 按既定规则保留已有 owner。可选优化一旦开始执行，其分配、读入和计算失败仍进入 failure，不清空错误后继续主路径。第三方库内部继续豁免；库报告失败时终止当前请求，不估算其内部申请量。

清单内长期存储的判断只使用确定容量状态：令 U 为当前 reservedBytes，A 为下一笔受控数组的完整分配容量，M 为有效上限，准入条件是 `U <= M` 且 `A <= M - U`，实现使用防下溢比较。扩容时 U 已经包含旧数组，A 取完整新数组容量。A=0 且复用既有分配的操作不需要新增预约。宿主、借用、库内部和槽位内临时分配不进入该公式。

以下条件确定后，无需等待系统内存恢复：

- 必要分配的长度溢出或超出格式、地址空间、运行时单对象硬能力，直接返回相应长度或能力错误
- 对已选定的必要内存 owner，A 本身超过 M 时返回受控容量不足；M=0 时任何必要正容量自有内存 store 都属于此类
- 当前明确必须跨过申请点保留的受控 owner 容量，加 A 已超过 M，返回必要并存容量不足；这只核对调用点已知的 owner，不建立全任务工作集估算
- 准入前回收及第 7.6 节允许的既有工作推进结束后，TryReserve 仍未获准且不存在独立释放步骤，返回受控容量不足
- 预约成功后的 new/malloc/vector 分配报告失败，返回实际分配失败；地址碎片、外部突发占用等因素可能造成这种结果，预约不能替操作系统保证分配
- 必需文件后端不可用、写入失败或磁盘不足，返回后端能力或 I/O 错误；不能把失败数据重新无界放入内存

这些失败结论针对当前有效 M 与当前申请，不把后续自动上调当作确定可用的容量。降低 C/S 不会减少必要完整结果与 reference 本身的大小；单纯排队也不承诺 M 会提高。上限更新本身不因 U>M 宣告失败，已有工作继续消费和释放；新的必要正容量申请仍按 7.5、7.6 节处理。尚在合法待准入位置的工作在事件唤醒后读取最新 M/C/S，已经失败的请求不因上限提高而复活。

### 7.6 延后准入的范围和最小实现

支持在分配前延后必要任务。沿用 driver 的有界工作顺序：块由 Cursor 保存下一个范围，重型阶段由 workflow 保存至多一个 `PendingAdmission`。不增加通用内存等待队列，不把分配失败的 worker 放回计算队列。

PendingAdmission 只记录阶段标识、尚未开始的操作位置、等待原因，以及清单内长期存储申请点已确定的 A（适用时）。不为块内 scratch、宿主或库内部申请生成 A。它不捕获新建的大缓冲，不持有 SlotLease、计算额度、HeavyPhaseLease 或尚未使用的容量预约；已有必要输入由 workspace 继续持有并计入其真实生命周期。延后本身不会释放这些输入。

| 等待原因 | 允许延后的条件 | driver 在等待期间的职责 | 结束条件 |
| --- | --- | --- | --- |
| 槽位或计算额度暂时用尽 | 等待槽位的新块尚未读入；等待计算的块已经持有槽位，其输入留在有界窗口内 | 优先提交队首、消费结果、归还槽位，worker 继续执行已准入任务 | 额度可用时推进；取消和任何前序失败立即收束 |
| Adaptive 已观察到系统压力，新块或重型阶段尚未准入 | 未发现 7.5 节的确定不可满足条件 | 按 14.8 节 DRAIN/HOLD 处理；继续已准入工作的完成与清理 | 排空后连续 2 s 满足高水位才恢复；HOLD 最长 30 s，重复通知不重置期限；Fixed 不进入此行 |
| 同一容量 state 被既有工作临时占用 | 目标已按 7.4 节确定为必要内存；调用点能指出已经准入、无需当前申请即可完成的内部消费/释放步骤 | 停止扩大工作窗口，继续该段既有工作及其顺序提交；实际释放后重新检查 TryReserve | 申请获准则推进；这些步骤结束仍不足即进入 failure，不更换目标后端 |

第三种等待必须满足一个直接的依赖条件：释放者不需要等待当前 PendingAdmission 成功，不需要新建一批尚未准入的块，也不会被当前 driver 停止提交所阻挡。仅使用原有块完成、顺序消费和阶段结束位置；调用点须能指出实际释放的受控 owner，不能因存在一个“后台任务”就认定它会归还当前容量 state。当前单 driver、单重型 stage 结构中，前一 stage 通常已经收束；这种情况下直接清理并检查容量，无需创建额外等待状态。完整字段暂存和最终输出的具体依赖见 7.7 节，本次不为它们增加独立内存等待队列。

不从缓存条目数、use_count、审计合计或预计可回收字节推导“以后会够”。移出缓存引用之后，只有 owner 实际销毁、归还 lease 才形成可用容量。调用方仍持有的结果、依赖当前步骤的 reference，以及待扩容数组的旧分配均不属于可等待的内部释放步骤。首版不等待调用方未来释放结果，也不协调多个独立 DataCodec 请求争抢设备内存。

driver 按以下顺序处理待准入工作：

```text
记录 observedEpoch，检查取消和已有失败
校验已知长度和硬能力；必须驻留内存的用途检查确定不可满足的容量条件
执行到期生命周期释放；已知必要容量不足时执行一次准入前可选回收
新工作检查 gate，关闭时保留待准入位置并继续原控制器流程
无长期容量申请：按已有槽位或阶段准入继续正常工作
当前操作允许开始时，严格按 7.4 节用途规则确定后端并检查必要内存准入
    成功：直接开始该操作，不持有预约排队等待另一个准入条件
    尚有独立的既有释放步骤：保留 PendingAdmission，继续推进这些步骤
    没有可使条件改变的路径：返回具体失败
没有可推进动作时，按 observedEpoch 等待完成、回收、控制或取消事件
```

gate 与 HeavyPhaseLease 统一遵守 5.4、14.3 节；本节不重新定义哪些工作属于新阶段。新阶段只能在 gate 开放时正式准入，当次容量检查未获准时释放临时阶段凭证。等待时不持有分配 lease 或执行锁。容量检查成功与分配属于同一次准入过程，不能先占住容量再排队等待门禁。

工作完成/释放通知复用现有事件序号，不新增容量轮询线程。容量 owner 可以比执行状态活得更久，完成路径应在归还相关 owner 后通知 driver；纯 lease 析构不反向访问已销毁的线程池。所有唤醒均重新检查实际谓词，不把一次通知等同于足够容量。

目标实现禁止在执行中的调用栈里挂起等待清单内长期存储的容量。追加和封装只使用创建前选定的后端；必要内存增长被拒绝时立即调用 failure。块内 scratch 与第三方内部继续正常分配，不新增预算入口。普通互斥锁等待、同步 I/O 和库内部等待按原调用协议执行。受控输出扩容的队首结果不能把“提交自己之后才能释放的内存”当作等待依据；失败时结束请求并清理后续记录。为完成当前提交所需的增长不再次等待新阶段 gate，避免 DRAIN 等待自己。

HOLD 的 30 s 仅限制 Adaptive 排空后的系统压力等待，不作为运行中计算或阻塞 I/O 的强制超时。两种模式等待已准入释放步骤时都沿用正常任务完成、错误和取消语义；这些步骤结束后立即重新判定容量。Adaptive 的未知系统信号按控制器保守状态处理；Fixed 不等待环境采样恢复。两者都不反复尝试已经失败的实际分配。

例如 M=1 GiB，下一笔完整结果分配 A=600 MiB，U=700 MiB：

- 若其中 400 MiB 可由已经准入且独立的消费步骤释放，driver 先完成消费；实际 U 降为 300 MiB 后即可预约，共计 900 MiB
- 若 700 MiB 均须作为生成该完整内存结果的输入保留，必要并存量为 1300 MiB；直接报告必要并存容量不足，纯排队无法完成
- 若最终结果本身为 1200 MiB，已经超过 M；清空其他内存仍无法通过该配置的完整内存输出契约

这些数字只解释确定分配与生命周期关系，不是根据输入大小估计编码工作集。排空块窗口释放的 scratch 不计入 U；它能缓解系统压力，也不会凭空增加长期容量 M 的剩余额度。

### 7.7 从现有代码确认等待的实际边界

本轮核查了存储扩容、普通/reference 数值编码、单叶和整帧封装、解码缓存提交、拓扑在途计数及线程执行器。已读链条没有要求某类编解码任务出于实时性而永远不能阻塞。源码已经包含条件变量等待、同步 I/O 和 task group 等待。“不可阻塞任务”不作为任务分类；需要约束的是等待对象与释放路径之间的依赖。

用于论证预算等待边界的对象限定为累计编码暂存、DataCodec 自有完整内存输出及这些存储的扩容。它们随整个数据规模增长、超出单块寿命，需要进入确定容量控制。下表标明具体受控对象，不能将同一函数中出现的全部缓冲都归入预算。表中的依赖环是对增加阻塞式容量申请的静态判断；当前 MemoryStore 预约失败直接返回错误，本轮没有运行程序复现死锁。

| 现有位置 | 目标受控对象 | 已确认的顺序及容量等待结论 |
| --- | --- | --- |
| `Storage/ByteStore/ByteStore.h:259`，MemoryStore::ReserveCapacity | 用于长期自有存储的旧数组与新数组 | 先预约、new，在 288—292 行复制并替换旧数组；旧数组只能在替换时释放。不能等待本数组的旧分配先释放。当前按扩容差额预约，目标按完整新容量预约；块内临时缓冲不因这条规则加入预算 |
| `Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h:144`、`:233`、`:264` | 跨块累积的 bodyTransferCache 自有内存 | 每块同步写入同一个 store，全部块完成才 Seal。append 等待后继封装消费该字段会形成依赖环。rawBlockBytes、componentBundleBytes 属于槽位，它们的释放不能视为归还该 store 的受控容量 |
| `Workflow/Encode/EncodePipeline.h:757`、`:767`、`:796` | 尚待封装的完整字段缓存自有内存 | ExecuteStageDag 返回、所有 transfer cache ready 后才 WriteLeafPackage。未完成字段不能靠排队等待未来封装回收自身；其他 stage 的独立临时释放不能消除本字段必需的累计容量 |
| `Workflow/Leaf/EncodeOutputWriter.h:352`、`:372`；`Storage/ByteStore/SegmentedBinaryObject.h:158`、`:198` | DataCodec 自有内存输出增长，以及作为来源的受控字段/segment 存储 | 字段写出并 EndStream 后才释放，当前 segment 也在 CopyTo/WriteAt 完成后释放。受控 sink 扩容等待当前来源释放，会挡住同一调用栈。外部 sink 自行分配和文件逻辑长度不接入 M |
| `Storage/ByteIO/ByteRange.h:243`、`:262`；`Workflow/Frame/FrameEncodeExecutor.h:288`、`:325`、`:364` | DataCodec 自有完整内存结果，以及 frame bundles 引用的受控自有存储 | 当前内存 sink 直接 vector::resize，尚无 ResidentByteBudget 预约接口；目标由 EncodedBuffer 承载确定容量。整帧先累积 bundles 再 WriteToSink，暂停写出不能使尚待消费的存储或旧输出数组自行释放 |

普通数值字段的关键顺序可以直接写成：

```text
读入块 -> 编码块 -> 追加字段 store -> 字段 Seal
所有 stage 完成 -> 封装读取字段 store -> 写入最终 sink -> 释放字段 store
```

在“追加字段 store”处等待最后一步释放同一个 store，会使这条顺序停止。把当前函数挂在条件变量上、把同一个未完成字段重新排队、延迟启动后继封装，都不会改变这组依赖。保持该算法路径时，需要足够的受控自有累计内存或已有文件暂存能力。采用文件后端时，仅实际存在的清单内自有内存进入 M，不能把整个文件长度当作常驻数组预约。

以下两处用于说明槽位与计算准入的正常等待，独立于逐笔容量预约：

| 现有位置 | 已确认的顺序 | 目标规则 |
| --- | --- | --- |
| `Codec/Topology/TopologyDecode.h:451`、`:498`、`:541` | 读入前等待 inFlight 空位，已提交 worker 完成当前解码/消费后减少计数并 notify | 释放路径可独立执行时允许等待；目标以槽位覆盖读入至实际消费完成，不为每块附加字节预算 |
| `Codec/Attributes/AttributeEncodeScheduler.h:100`、`:149`；普通数值 builder 的 `:170` | 计算前可等待 lane，持有者完成后归还；当前取得 lane 前已经读入 raw 块 | 目标等待计算额度，输入保留在有限槽位中；删除现有局部 quota/lane 的实现和调用，不因已有等待实现而保留另一套预算 |

宿主输出分配不列为预算等待的依据。`Runtime/Cache/DecodedCacheCommit.h:66`、`:84`、`:377` 所示 BeginPoints/BeginAttribute 发生于源缓存释放之前，这只能说明物理内存可能并存。宿主目标没有进入 M，不能据此新增容量预约、预算拒绝或等待队列。必要 reference 的借用与自有存储按 7.5 节分别处理。宿主或第三方报告分配失败时执行统一错误收束，豁免范围保持不变。

线程池还存在单独的推进条件。`Runtime/Execution/DataCodecExecutionResources.h:12` 的默认 runner 使用 InlineParallelTaskRunner，Submit 在当前线程立即执行；等待稍后才能由同一线程提交的释放任务没有推进者。`Runtime/Execution/ParallelExecution.h:194` 的 AllowNested 路径会提交后继子工作再 Wait，`Filter/Execution/iGameDataCodecThreadPoolTaskRunner.h:93` 的 Wait 等待 pending 归零。目标有限线程池若让所有 worker 停在等待同池未执行子工作的状态，子工作将无法取得线程。

`Runtime/Execution/ParallelDecodeTopologyBlockObserver.h:103` 也有具体的嵌套消费关系：ObserveConnectivityBlock 等待 pending 窗口，真正减少 pending 的 handler 通过其 taskRunner 提交；拓扑 worker 在 `TopologyDecode.h:284` 同步调用这个 observer。同池且全部可用 worker 停在该等待处时，handler 无法执行。这个结论带有明确的同池与线程耗尽条件，不代表所有配置都发生死锁，也不构成所有同步等待都应删除的理由。

同池等待属于线程推进约束，不能用于扩大内存控制清单。首版实现保留的等待落点为块读取前、计算开始前、阶段边界和已有工作的完成确认；driver 始终推进已有结果消费。TryReserve 仅在清单内长期存储的申请点使用，其非等待语义把决定权返回上层。受控输出扩容、累计字段 append 和相关 source 消费不增加“等自己释放”的队列；块内临时缓冲、宿主和库内部申请不增设预算等待入口。

### 7.8 按实际存储逐模块接入

本节是内存接入的逐项清单。源码位置均以当前工作区实际存在的文件为准。核查范围包括 Codec、Storage、Runtime、Workflow、API、Common、Validation、Log、Filter 和 Platform 的生产源码；测试目录、文档以及 17.1 节的实验文件不作为生产实现依据。CodeGraph 返回的历史 AdjacencyContext/PointCellIncidence 路径在本快照中不存在，当前 point/cell 重排使用 Morton，不为历史邻接图增加目标模块。

#### 7.8.1 清单的读法与接入规则

下表中的 `n` 是当前块的 tuple 数，`E` 是完整字段 tuple 数，`D` 是字段分量数，`V` 是标量字节数，`I=sizeof(IndexType)`。`b/f/j` 分别表示当前拓扑批次的单元数、面数、连接值数。它们都来自经过检查的真实布局或计数，与计算额度 C、槽位数 S、当前在途数 N 分开。

- **根容量**：清单内自有数组接入同一 M，记录实际数组及 lease；固定连续数组使用 MemoryStore，支持范围访问的存储按 7.4 节工厂选择后端
- **块槽位**：输入、工作缓冲、候选和结果随 BlockRecord 存活；申请长度照算法计算，不向 M 逐笔申请，不在块内等待新槽位
- **固定阶段工作区**：由当前唯一 driver 阶段或块流 Cursor 独占，窗口数量和长度在本节写明；非块计算使用 HeavyPhaseLease，跨批次流窗口按 10.3 节随唯一 Cursor 保留，不为整个块流持有 HeavyPhaseLease。门禁关闭不创建新窗口，流或阶段结束释放；实际已覆盖容量独立审计
- **引用留存**：对象只持有底层 owner 的引用，按 8.1、12.1 节释放；不能再次预约底层容量
- **明确豁免/仅审计**：不进入 M；表中注明的确定数组可以独立审计，未知容量留空。不能将这类对象用于解释 U 或作为容量等待的释放者

根容量清单中的 POD 工作数组沿用 MemoryStore 的数组与 lease 事务，在具体业务 workspace 中提供类型明确的 span 和元素计数。校验 `count * sizeof(T)`、对齐与类型要求；需要的对齐属于该数组的确定分配长度。禁止先构造完整 vector，再为相同内容预约第二个 owner。此接入只覆盖本节列明的数组，不新增通用受控 STL 容器或 allocator 框架。动态追加的候选边以实际写入数计算 required，沿用 7.3 节唯一扩容事务；算法不能提前知道最终条数时，不预留“最多可能有多少条”的预测容量。

新进入根容量的已知数组在终端计算前由 driver 完成准入。依赖计算结果才能确定的追加量在现有 owner 上即时申请，拒绝进入 failure；不挂起 worker 等 driver 回收。不确定的压缩结果长度、采样收益和物理 RSS 均不成为下一笔数组容量的替代值。

#### 7.8.2 数值、属性、几何与 reference

| 源码模块与实际存储 | 长度来源、承载步骤 | 目标管理与释放点 |
| --- | --- | --- |
| `Runtime/Workspace/EncodeLeafWorkspace.h` 的 geometry source，`NumericArraySource.h` 的 values/owner/orderProvider | 保存几何、属性和重排来源，视图本身没有整字段数组 | 宿主既有输入借用豁免；自有 provider 的容量由原 owner 保留。清理 source 只解除自己的引用 |
| `NumericArrayReader.h` 的 raw/output、orderBlock、GetterOnly elementValues | raw 为 `n*D*V`，单分量为 `n*V`，重排范围为 `n*I`，getter 暂存为 `D*V`；用于按序读取、AOS/SOA 连续化和按 remap 收集 | 块槽位；取得槽位后才读入，最后实际消费后释放。当前 ReadElements 会复制数据；目标可借用路径必须携带有效 owner，不能仅凭指针非空称为零复制 |
| `NumericArrayBlockEncode.h` 的 componentRawBuffers、componentEncodedBytes、encodedBytes | 分量分离和压缩，raw 分量为 `n*V`；已编码长度由 codec 结果给出，bundle 按已产生片段长度相加 | 块槽位；分量串行计算，已压缩分量保留到组包消费，raw 分量完成使用即释放；完整 payload 留到顺序提交 |
| 同文件的 baseDecodedComponent、residualRawBytes、decodedResidualComponent、background/region owners | 区域背景解码、生成残差、解码残差验证并累加；背景为 `n*V`，每层为 `refinedElementCount*V`，层结果长度取已编码实际值 | 块槽位；逐分量、逐层处理。当前层工作数组在累加结束后释放，格式需要的已编码片段保存到当前块组包，不能跨块积存在 worker |
| `NumericArrayBlockDecode.h`、`GeometryDecode.h`、`AttributeDecode.h` 的 encoded/decoded/component/residual 缓冲 | 编码输入来自块 header 的各段长度；完整块输出为 `n*D*V`，分量及区域残差按各自实际元素数计算 | 块槽位；层残差用完释放；结果写入确定字段范围或同步 observer 消费后释放。旧大块按 11.1 节单记录规则处理 |
| `NumericArrayRegionPlan.h` 的 sortedRuns、clippedRegionRuns、target/merged/output runs、gaps、selectedGaps、isCore | 当前存在每块复制整份 regionRuns，以及按整份 runs 数量 reserve 的代码；局部选择完成前的 runs 数不受最终 maxRunsPerRegion=8 限制 | 字段级有序 run 数组按 `runCount*sizeof(RegionRun)` 接入根容量，一次构建；块内只裁剪本块相交 runs，归槽位。9.3 节规定具体拆分，禁止保留每块全量复制 |
| `ReferenceTransferCacheBuilder.h` 的 current/reference staging、nonReferenceCandidate、referenceCandidate、probe sample | current/predictor 为 `n*D*V`；probe 为实际采样 tuple 数 × D × V；候选长度为实际编码返回值 | 同一块槽位覆盖样本、候选编码、候选验证和最终结果；被淘汰候选立即释放。删除块 staging store 的独立配额和工厂回调 |
| `NumericArrayReferenceBytes.h` 与 `AttributeDecode.h` 的重采样/偏移读取 | 等长 reference 为 `n*D*V`；当前重采样读取首末映射点之间整个区间，可能远大于 n；输出仍为 `n*D*V` | 9.2 节改成固定范围窗口读取插值所需相邻 tuple，结果与读取窗口归本块；不能以“reference 块等于 raw 块”省略这一层 |
| `WaveletReferenceCodec.h` 的 low/high、lowDelta/highDelta、referenceComponent、重建数组和 blob | 浮点工作值使用 double；长度 n、ceil(n/2)、floor(n/2) 各乘 8；编码对齐 scratch 额外申请 `alignof(double)-1`。整数路径的 low/high/residual 使用 uint64_t，奇数尾项单独处理 | 块槽位；分量循环内复用并及时释放，各个同时存在的真实数组分别审计，不能合并成一个 `n*D*V` 估算值。库拥有的压缩 blob 按 13.3 节豁免容量 |
| `AffineReferenceCodec.h`、`PredictorReferenceCodec.h`、`ReferenceBlockIO.h` | delta/预测及候选回解按 `n*D*V`；alpha/beta、误差范围统计为 D 个 double；offset 候选为算法参数数组 | 大数组归块槽位；D 维系数和小候选描述豁免。候选按既有算法顺序计算，不为每个候选展开新任务；分配和验证失败按第 16 节处理 |
| `AttributeReferenceScheduleBuilder.h` 的 sampleIndices、fieldSamples、edges | 字段分组抽样；索引数组的确定容量为 `s0*sizeof(size_t)`，s0 见 6.0 节；每字段样本为 `去重后的实际sampleIndices数*D*V`；每条通过评分条件的 IntraFieldEdge 是真实新增记录，最坏随字段对数量增长 | 索引、样本字节和实际候选边数组接入根容量，单个前置阶段运行；每次只建立当前组的采样索引与样本，组评分后释放，建完依赖顺序释放 edges。9.4 节规定次序，不与块内 4096 tuple 的收益探测混用 |
| `DecodedAttributeCacheSet.h`、`DecodedGeometryCache.h` 与 `DecodedReferenceBuilder.h` | DataCodec 自有整字段结果、编码关键帧的回解 reference：`E*D*V`；几何通常 D=3，以实际输出类型为准 | 根容量/固定后端；每个实际需要的字段在 Begin 时取得完整长度。作为帧内或跨帧 reference 的 owner 留到最后依赖消费；当前 geometry 与 geometryReference 是两个数组时必须分别计数 |
| `AttributeReferenceDecode.h`、`DecodeSession.h` 的辅助 workspace/session | 必要前驱字段解码可能新建 CacheResources、ByteStoreSession，并通过 aliasing shared_ptr 延长叶 workspace 寿命 | 删除内部自建额度/池；driver 先准备前驱，共享根容量及 scratch。目标保留具体数据 owner，不用一个字段的引用无意保留整套执行状态或全部已提交字段 |

`NumericArrayEncodedBytes` 同时支持自有 vector 和 PressioDataHandle。前者按实际 capacity 审计；后者的分配属于库内部，data size 只作为有效负载长度。把 Pressio 结果复制到 DataCodec 自有 payload 后，新 payload 才按自有容量口径登记；二者并存时仅对已经覆盖的 owner 报告，不把库内部未知容量填为逻辑长度。

#### 7.8.3 重排与拓扑

| 源码模块与实际存储 | 长度来源、承载步骤 | 目标管理与释放点 |
| --- | --- | --- |
| `PointRemapBuilder.h`、`CellRemapBuilder.h`、`PolyhedronTopologyRemap.h` | 读取坐标或单元点号计算 Morton key；point/cell order 及 point inverse 各为对应全域元素数 × I | 输出 provider 接入根容量/固定后端；引用保留到最后依赖几何、拓扑、属性完成。坐标包围盒和单 tuple 栈数组豁免；恒等 provider 无全域数组 |
| `MortonRemapBuilder.h` 的 keyed/order/inverse 内排工作副本 | 当前元素数满足内排阈值时创建；keyed 按 `count*sizeof(MortonKeyedIndex)`，order/inverse 按 `count*I` | 固定阶段工作区，长度受 8 MiB 所导出的元素阈值限制；8 MiB 不是所有副本之和。发布后的 provider 另按根容量保存，临时源用完释放，排序库内部临时分配豁免 |
| 同文件 keyCache | 可选全域 `elementCount*sizeof(uint32_t)` | 根容量；计算前只选定一次是否启用，high run 写完立即释放，遵守 10.2 节准入 |
| 同文件 highCounts/highOffsets/highWriteOffsets、lowCounts/lowOffsets、touchedLowBuckets、leaves | 桶数固定为 65536；表元素为 size_t，touched 为 uint32_t，leaf 描述最多为低桶数 | 固定阶段工作区，按实际 capacity 审计；highWriteOffsets 在 high run 完成后释放，其余表在 Morton 阶段结束释放，不为每叶建立一组表 |
| 同文件 highBuffers | 65536 个桶各自保留小 vector，聚合容量可达数十 MiB；单桶“1 KiB”不能代表整体用量 | 改为 10.2 节规定的单个确定容量 slab，接入根容量；每桶只保存 slab 切片及已写长度，high run 完成即释放 |
| 同文件 scratch、orderedElements、lowBuckets 和 run 读取 buffer | 前三者按 `min(maxHighBucketSize,leafBudgetElements)` 各自分配；读窗口按 1 MiB 容纳完整 run record | 固定阶段工作区；逐叶复用，溢出同 key 的桶按现有分段外排推进，不把大桶整体装入内存 |
| `RemapScratchRun/Spooler` 与 `RemapProvider.h` | high run 为 `elementCount*(sizeof(uint16_t)+I)`；order/inverse 为全域 count×I，长度由计数确定 | 根容量/固定后端；run 与输出在排序期间可能并存，分别持有 lease；完成消费即放下 run，provider 继续由业务 owner 持有 |
| `ConnectivityTopologyBlockEncode.h` 的 inversePointRemapValues/cellOrderValues | 当前为整个点域/单元域的连续数组 | 10.1 节的必要连续 MemoryStore，根容量；在该拓扑阶段内跨块只读共享，阶段结束放下。源 provider 仍在被使用时继续独立持有原容量 |
| 同文件 connectivity、cellSizes、cellTypes、cellPolynomialOrders、cell | 固定单元为 `b*fixedCellSize` 个连接；变长单元连接数为本块所选 cell 的真实 offset 差之和；类型 b×I、阶次 b×2、单 cell 暂存取该 cell 实际连接数 | 块槽位；删除按全网格平均 cellSize 预留连接量，先按本块真实计数分配。不增加任意连接数性能上限，极端单元执行第 18 节检查 |
| `ConnectivityTopologyCodec.h` 的 offsets、mainEvents/symbols、seed、residual、编码流 | offsets 为 `b+1` 个 size_t；grammar 事件随本块 j 生成，符号流的 bitCount/实际字节长度决定分配；定长 lookup/Huffman 字母表规模来自 codec 常量 | 大 vector 归块槽位并审计；固定小表及 STL 排序内部豁免。四条已编码片段组装到 artifact 后，工作数组释放；artifact 留到提交 |
| `ConnectivityTopologyDecodeInputReader.h` 的四个 stream store | 当前每次 LoadFrom 装载一个 connectivity block 的四段编码字节，长度来自该块 layout；这里仍是编码数据 | 改成 BlockInput 的四段自有缓冲/借用视图，不走跨块 MemoryStore 预算；顺序提交完成归还槽位。禁止将整个拓扑字段读入这四个块容器 |
| `ConnectivityTopologyDecode.h`、`TopologyDecode.h` 的 connectivity、offsetScratch、observedBlock | 本块 j×I、offsets `(b+1)*I`、类型 b×I、阶次 b×2；观察结果当前可能再次复制本块 | BlockOutput 持有结果，driver 同步提交；offset 修正和 observer 必要副本仍受同一槽位覆盖，按真实分配分别审计，不增加后台消费队列 |
| `DecodedTopologyCache.h` 的完整 connectivity、offsets、cellTypes、cellPolynomialOrders | 完整连接数×I；需要 offsets 时为 `(cellCount+1)*I`；类型 cellCount×I；阶次 cellCount×2；结构化拓扑只有描述 | 自有数组接入根容量/固定后端；缺省分支不创建无用数组，最后提交或 reference 消费后释放；宿主目标仍按宿主输出边界处理 |
| `PolyhedronTopologyEncode.h` 的 visitedFaces、localIndexTable | 校验遍历的 visitedFaces 为全面数个 uint8_t；局部编号表为 `tableElementCount*I`，存在 point inverse provider 时取其 Size，否则取 pointCount，并校验与点域契约一致 | 必要连续数组接入根容量；校验阶段确定是否存在超过线性查找阈值的 cell，结束立即释放 visitedFaces。driver 据此在 cell 编码前建立所需查表，整段串行复用，到编码阶段结束释放 |
| 同文件 PolyhedronCellWorkBuffer 和 overflowPointIds/overflowLocalIds | 每 cell 的面数、面顶点引用总数、去重后的顶点数；容量从遍历后的真实计数得到。overflow reserve 当前随该 cell 引用数增长 | 单 cell 工作数组归当前 cell 批次槽位；直接编号表按上一行管理。只保留当前工作单元，不把每个 cell 的数组累积到队列 |
| `PolyhedronTopologyStreamEncoder.h` 的五个 m_buffers，`TopologyTransferCache.h` 的五条 spool | m_buffers 当前各按 8 MiB 刷出且 clear 保留 capacity；spool 跨 cell 保存全部已编码内容 | 五个流缓冲改为各 1 MiB 固定阶段窗口，允许跨 cell 保留尾部；五条累计 spool 按 7.4 节追加后端管理。最后块归还前 Finish/Seal，具体边界见 10.3 节 |
| `PolyhedronTopologyStreamDecode.h`、`DecodedPolyhedronCache` 的五条解码索引存储 | uniqueVertexCounts 与 cellFaceCounts 各 cellCount×I；faceVertexCounts 为 faceCount×I；cellUniqueVertexIds 为 uniqueVertexIdCount×I；localFaceVertexIds 为 localFaceVertexIdCount×I | 五个独立根容量/范围 owner；先准备前四条以解开第五条 bitpack，再供 emit 消费。它们跨批次存在，不能归入 1 MiB 读写窗口 |
| `PolyhedronTopologyEmit.h` 的批次 offsets/counts/ids | cellVertexOffsets、cellFaceOffsets 各 b+1 项，faceVertexOffsets 为 f+1 项，unique/local ids 按本批次真实总数；uniqueVertexCounts、cellFaceCounts 各 b，faceVertexCounts 为 f | 批次槽位；最多 65536 cells，先扫描计数再分配，写入宿主完成后释放。计数扫描窗口、尚未接纳的下一 cell 计数也必须有界，见 10.3 节 |

#### 7.8.4 字节输入、封装与完整输出

| 源码模块与实际存储 | 长度来源、承载步骤 | 目标管理与释放点 |
| --- | --- | --- |
| `MemoryByteRangeReader`、`SubrangeByteRangeReader`、`CallbackByteRangeReader`、`LeafPackageFields` | 既有 reader、范围视图和来源 owner；当前 span 构造 MemoryByteRangeReader 会复制完整 bytes | 从调用方接收的既有 input owner 豁免；DataCodec 内部新建的完整副本必须按完整长度接入根容量。内部 reader 统一保存已有 owner+范围，删除内部 span 构造引起的隐式全量复制 |
| `EncodedInputCacheLoader.h` 的完整预读 bytes | 实际 ByteSize，不使用压缩率 | 读取前按根容量申请，未准入不启动完整预读；获准后按 1 MiB 范围填充。留到 source/reference/可选输入缓存最后引用结束，预读缓存最多一份 |
| `PackageDecodeWorkflow` 的 enableFullInputPrefetch、`iGameFileByteRangeReader` 的 mmap/PrefetchVirtualMemory | 当前可以在解码前 PrefetchRange 整个输入；原生 reader 映射整个文件，映射长度与实际驻留页数量不同 | 删除整文件 OS 预取调用和独立开关。原生只读映射保留输入访问职责，范围预取仅在已准入读取中向当前最多 1 MiB 窗口发提示，读取共用可选政策；不循环提前预取尚未消费的全文件。映射与 OS 页单列，最后 reader 引用结束后 unmap |
| `LeafPackageIO::ReadFromMemory` 的 fieldBytes，`FramePackageIO::MakeFrameLeafPackageBytesWriter` 的整叶 vector | 当前 span 解析按每字段复制原压缩字节，整叶 writer 可以捕获完整 vector | 解析统一经 owner+范围 reader，删除无 owner 的逐字段全量复制入口。自有已编码整叶只传受控 owner，不恢复裸 vector 累计出口；必要完整复制按根容量准入 |
| `LeafPackageFieldDecodeStream.h` 的输入/输出/Skip 缓冲 | 原始输入与外层 Zstd 解压按当前位置读入；rawSize 是外层解压后的编码字段长度 | 每个活跃 reader 至多一个输入窗口和一个输出窗口，各最多 1 MiB；Skip 复用固定窗口。块准备在已有槽位内运行，整字段准备在 HeavyPhaseLease 内运行 |
| `AttrDecodeStage.h`、`DecodeLeafWorkspace::m_attributePayloadOwner` | 外层 Zstd 解开整个 Attribute 字段的 payload，长度是该 field.rawSize；其中包含所有属性的数值编码块 | 跨块根容量/范围 owner，保留现有按属性范围访问需要，固定流式准备方式见 11.4 节；不能按 `E*D*V` 给这份编码 payload 计算长度 |
| `NumericArrayTransferCacheBuilder`、`ReferenceTransferCacheBuilder`、`GeometryTransferCache` | body store 随 WriteNumericArrayBlock 累积全部块；geometry 包装保存 metadata 与 payload 引用 | 字段追加 store 按 7.4 节；每块提交释放源 payload，字段结果保存到封装完成或必要消费者结束。GeometryTransferCache 包装不重复记账 |
| `AttributeSpooler`、`EncodeTransferCacheSet`、`EncodedLeafFieldBundle`、`SegmentedBinaryObject` | sources/recordOrder/ready、segment 和 backing owner；保存各字段/叶的数据引用 | 引用留存；排序/就绪元数据按字段数保留。长期 payload 只在底层计一次，SnapshotRecord/OrderedRecordSources 不复制内容；消费后及时放下对应引用 |
| `FrameEncodeExecutor` 的 fieldBundles/leafPackages/leafPackageWriters | 当前整帧先保存各叶，再封装帧；不能把 leaf 数当作在途块数 | 所有被引用的自有字节 owner 继续接入同一根 M/文件后端。叶工作区的计算资源结束即解除绑定，尚待写出的字段 store 留到实际消费；帧 header 回填和 Finalize 完成后交付 |
| `LeafPackageFieldEncode.h`、`ZstdStreamingEncoder` 与 frame package 压缩暂存 | probe 输入至多 1 MiB，probe output 按 `ZSTD_compressBound(实际probe输入长度)` 分配，可能稍大于 1 MiB；正式输出随压缩器产生 | probe 的确定输入/输出数组按固定阶段管理；正式流的输入/输出窗口各最多 1 MiB。跨窗口累计的 compressedStore 按追加后端管理，Zstd 内部上下文豁免 |
| `MemoryByteRangeOutput`、`EncodedBuffer`、`SegmentedBinaryObject::Materialize` | 最终完整布局长度或当前真实 required/capacity | 根容量；7.3、8.3 节规定旧新并存、交付与失败。移动结果继续携带 lease，不能 TakeBytes 脱离 owner |
| `ByteStoreSession` 的登记集合和 `VectorByteSource` | 登记对象个数与实际字节 owner 分开；VectorByteSource 可以隐藏完整自有 payload | Session 用 weak 登记并在阶段结束清理过期项；不因弱引用也允许登记无限增长。VectorByteSource 仅保留空载/固定小头用途，清单内长期字节改用受控 owner，块 payload 留在 BlockRecord |
| `FileBackedStreamStore`、文件 reader/writer、WindowedCopy | 文件逻辑长度、操作系统缓存页和 1 MiB 用户态复制窗口 | 文件长度及 OS 页不记入 M；窗口按实际 capacity 审计，文件在最后 owner 销毁时关闭清理。删除整文件 mmap store 生产选择和独立 window budget，所有调用路径统一窗口常量 |

#### 7.8.5 会话、缓存、规划与元数据

| 源码模块与实际存储 | 长度来源、承载步骤 | 目标管理与释放点 |
| --- | --- | --- |
| `EncodeSessionWorkspace` 的 m_referenceCaches/referenceByteStoreSession | 保存各关键帧、各叶的 decoded attribute/geometry reference | 自有底层按 E×D×V 接入根容量；按实际时序依赖及时删除到期引用，不为每关键帧新建根预算。当前需要的 reference 属于必要数据，不能因可选 LRU 满而丢失 |
| `DecodeSession` 的 m_leafStates，`DecodeReferenceCache`、`DecodedTopologyReferenceCacheStore` | 保存部分解码状态、完整字段、自有拓扑及输入 owner；仅计 cache entry 会漏掉 session 强引用 | 自有存储按本节对应行管理；数据状态按当前消费/必要依赖/可选帧保留规则清理。到期时连同 aliasing workspace、prepared payload 引用一起解除，禁止另留全历史叶状态 |
| `DecodedFrameLruCache`、`EncodedInputLruCache`、`LruCacheIndex` | 条目、LRU 节点、payload/source 引用 | 12.1 节引用回收；可选完整帧最多两个、完整预读最多一个；宿主 payload 容量未知单列。插入相同自有 owner 不再次申请 M |
| `DecodeTaskCoordinator`、`PlaybackPrefetchPlanner`、Playback prefetchTasks/queuedFrames | 业务请求描述、去重 Interest、完成结果和预取句柄 | 12.3 节一个运行、一个前台待运行、一个可选预取；待运行请求不预分配块数据。完成条目从内部去重表清理，外部 handle 只延长其结果必要寿命 |
| `FrameSequenceDependencyPlanner`、`TemporalBuilder`、frame reader/package map | 帧/叶描述、依赖集合与访问顺序；结构规模随输入增长，非数值 payload | 元数据分配豁免；保留计数、长度、依赖环和范围检查，按已登记输入限定合法索引。reader 引用仍遵守上一节输入所有权，不能在规划 metadata 时隐式预读各整帧 |
| `CodecStorageParams`、blockLayouts/regionLayers/componentLayouts，参数序列化与解析 | 格式必需元数据；外层 vector capacity 可确定，内部 string/map/vector 分配分别存在 | 仅审计本方案列出的确定容量，不接入逐成员 M；禁止复制整套 layouts 给每个块。保留现有 params 16 MiB 解码边界，编码完成序列化时检查相同边界，超限 failure，不能生成本模块无法读取的包 |
| `ParamsEncodeStage`、`ParamsDecodeStage`、`DecodedReferenceBuilder` 的参数字节 | 实际序列化长度或验证后的 paramsField.rawSize，上限 16 MiB | 单次序列化/解析 scratch 仅审计并受单阶段限制；跨叶保存的参数字节必须交给根容量/范围 owner，临时 vector 消费后释放。第三方序列化器内部分配豁免 |
| `RunOrderedBlocks`、stage 描述、Context/workspace 中锁、通知与状态 | 块记录数受 S 限制，调度状态受阶段和请求窗口限制 | 轻量状态豁免；运行窗口仅保存已准入记录，worker 不保存全文件 artifact 数组；thread/diagnostic 固定成本不通过每个任务估算申请 |

元数据豁免是明确的首版边界，16 MiB 序列化上限不等于反序列化对象 capacity 的上限。随字段/叶/帧数增长的元数据必须在第 17 节单独测量，报告不能称其为常量小开销。该边界不用于豁免 reference 采样索引、fieldSamples、edges、全量 region run 工作副本或累计 payload；这些已在根容量清单中。

#### 7.8.6 宿主适配、校验、日志与平台

| 源码模块与实际存储 | 目标管理方式、确定口径与生命周期 |
| --- | --- |
| `iGameEncodeAdapter` 的宿主原生几何/拓扑/属性视图 | 既有宿主数组借用豁免，DataCodec 不统计其底层容量。GetTuple/GetCell 内部宿主实现分配继续属于宿主边界 |
| 同 adapter 的 `BuildPolyhedronFacesForUnstructured`、synthesizedOffsets | 此路径实际构造整叶 faces/cellFaces、faceIdsByKey 和两份 offsets，不能称为借用。作为宿主拓扑表示构造的明确大对象豁免，不向宿主 CellArray/map 安装分配管控；仅一个叶预处理阶段执行，建表结束释放去重 map，最后拓扑/remap 消费后调用 ReleaseConvertedInputs。两个自有 offsets 和去重键 vector 按实际 capacity 取样审计，宿主容器和 map 节点未知单列 |
| `iGameDecodeAdapter` 的 native arrays、NativeAttributeByteStore、m_cellPolynomialOrders 与 polyhedron face/cell 构造 | 宿主输出及其构造存储豁免 M，成功提交后由宿主结果持有；失败清理未提交结果。m_cellPolynomialOrders 是 cellCount 个 uint16_t 的实际 vector，单列 capacity 审计，到完成宿主高阶单元构造后释放；不得把 NativeAttributeByteStore 包装当成 DataCodec MemoryStore 再收费 |
| `iGamePreparedSurfaceDecodeAdapter`、frame assembly/presentation、Wasm model registry | 宿主表面构建器、faceToCellMap、原生模型和显示结果继续由宿主拥有；不纳入 DataCodec 字节额度。内部 observer 同步消费，DataCodec 不为宿主自行保留多个模型建立 LRU；registry 由模型释放接口结束寿命 |
| `FiniteNumericValidation`、Topology/Geometry/Attribute validator、`AdapterTreeSignature`、`AdapterPrecisionMetrics` | 现有有限值校验按固定 chunk 读取，签名/误差计算主要使用单 tuple 和 D 维统计；窗口归当前块或阶段，固定小统计豁免。多面体 visitedFaces 已单独接入根容量；不能以 Validation 目录名豁免全域数组 |
| `RemapOrderCapture`、`AdapterSignatureOrderSet` | 当前会 ReadRange 全量复制 point/cell order。目标快照保存不可变 provider 的共享数据 owner，分析按 1 MiB 范围读取，删除全域 vector 副本与读取失败后伪装恒等序的分支。该功能用于明确请求的分析，保留的数据仍带原 lease，不作为可选性能缓存回收；分析结果交付/销毁时结束引用 |
| `TelemetrySessionSink`、`DataCodecProcessReportJson`、消息和时间记录 | 自有日志对象不进入 M，按 16.3 节固定留存规则限制；报告按有界快照生成。Playback 已有 backgroundMessages 64 条规则保留。资源诊断继续使用 16.4 节固定环，不能把可选报告留存作为首错交付前提 |
| `WasmBrowserFileByteRangeReader` 的 JS ArrayBuffer/预取 map | 删除独立 JS 长期预取缓存和对应 maximumPrefetchBytes，完整输入缓存只走根 loader；浏览器 ReadAt 以最多 1 MiB 子请求顺序复制到目标 span，每次只有一个 JS 范围缓冲，返回后不放入 registry。GC/浏览器内部容量不进入 M，ArrayBuffer byteLength 只报平台暂存逻辑量，具体见第 15 节 |
| `NumericArrayCodec`、`ZstdCodec`、Huffman/Varint/Bitpack | 库内部内存、allocator 元数据与标准库算法隐藏临时内存豁免；调用处自有大输入/输出按对应块/阶段/store 行处理。固定字母表和小状态豁免，compressor owner 和库线程数执行第 13 节 |

宿主拓扑表示构造是本轮明确保留的大对象边界，可能随整个网格增长。Fixed 的 M 不覆盖它；Adaptive 可以根据设备压力暂停后续工作，也不能在宿主调用内部逐笔拒绝分配。文档和报告须同时列出该边界及已追踪容量，不能把“自带 adapter”自动解释成宿主借用或受控内存。彻底约束此部分将涉及宿主拓扑容器与接口的独立改造，本轮不加入该工程。


## 8. 所有权与复用的实际改造

### 8.1 共享存储释放契约的变更

当前存储采用显式 Release，由业务流程决定内容的释放时机。`ByteStoreSession::ReleaseAll` 对登记对象逐个调用 Release，`DecodedCacheCommit::ReleaseDecodedByteStore` 也先调用底层 Release，再 reset 自己的引用。shared_ptr 管理 store 对象的寿命，Release 可以提前清空对象内部的数组并归还容量；其他引用仍可指向已经清空的对象。

现有代码包含针对这种契约的业务判断：`Workflow/Decode/Stages/DecodeCommitStage.h:113` 对借用拓扑和需要保留为 reference 的拓扑选择保留内容的提交路径，其余路径执行提交后释放。本节记录设计变更；仅凭显式 Release 的存在，不能确认旧代码存在提前释放错误，也不能推断原设计遗漏了最后引用者释放的实现。

既有资源设计要求使用期间保留、最后消费者完成后释放。本实现方案选择让共享存储随拥有者引用释放，以减少跨 workspace、reference 和输出交接时的人工生命周期协调。这是落实该要求的具体实现选择，不能将其视为此前设计已规定的唯一方式。

目标实现中，session 的登记集合保存 weak_ptr，用于诊断和本会话资源发现，不成为额外保留者。创建后的强引用由具体 workspace、bundle、reference 或输出 owner 持有。ReleaseAll 清理登记和会话自己的引用，不对共享底层执行强制 Release。

业务消费者完成后及时放下自己的拥有者引用；可选缓存继续持有的强引用受其保留与淘汰规则约束。最后一个拥有者引用释放时析构底层存储。空闲缓存仍持有 owner 时，数组继续存活并占用相应容量；“最后业务消费者完成”不直接等于 shared_ptr 强引用数归零。

共享底层源以析构为真正的存储释放点。对 workspace、cache、segment 和 reference 的普通释放操作改为 reset 自己的引用；相应 store、文件和映射类必须拥有完整析构清理。删除 IByteSource/IRemapProvider 上破坏性 Release 方法的声明、派生类实现及共享调用点。独占消费游标统一用 Reset 清理自身状态，写入协议原有 Finish 保留其完成写入语义，均不得强制清空其他消费者共享的底层源。实现时逐调用点完成迁移，不用 use_count==1 作为正确性的判断条件。

SegmentedBinaryObject 的一次性消费仍在每段写完后丢弃该段引用，从而及早释放不再使用的 store。被其他 reference 持有的 store 继续有效。ByteStoreSession move 只移动登记和同一容量 state，不创建新的无限预算。

### 8.2 scratch 的边界

所有 workspace 访问同一个根 scratch 复用池，保留现有 best-fit 查找和可移动缓冲 RAII。活动 buffer 不按字节向另一个 quota 等待；它们的份数由块窗口或一个已准入的预处理阶段限制。

复用池最多保留 `2 * max(S, C)` 个空闲 buffer，每个 buffer 的实际 capacity 不超过固定 8 MiB；超出单对象阈值的 buffer 归还时直接释放。删除按 `M / 8` 划出 scratch 份额的规则，不为复用池新增一份模块预算。这个阈值仅作用于已经列入清单的空闲复用对象，活动缓冲仍按本块实际需求分配。是否允许留存统一读取 12.1 节的根级政策，M=0 和压力暂停期间不保留空闲 buffer；S/C 下调时裁剪空闲条目。

归还时根据当前目标决定保留，压力发生前借出的 buffer 归还后也不能重新填满池。Trim 在锁内移出空闲 buffer，锁外释放；正在使用的 buffer 保持有效。

ScratchByteBuffer 的归还目标使用池状态的 weak_ptr，不保存可悬空的 pool 裸指针。池已关闭时直接释放自身容量，不为归还一块缓冲保留整套执行线程。正常运行仍要求任务先收束再关闭池。

ScratchByteBuffer 的底层容量审计与复用账本分开。能够拦截实际 capacity 变化的接入点报告真实容量；仍向算法暴露可变 vector 的点只报告读取时刻的确定容量，不声称完整分配峰值。未知峰值留空，不为实现资源控制全面包装 STL。

### 8.3 连续编码输出

EncodeOutput::Memory 保留其业务含义。目标 `EncodeResult::encodedBytes` 改成只读、可移动的 EncodedBuffer 数据对象，提供 data、size、empty、span，不提供 resize/reserve 或容量调整方法。

EncodedBuffer 的私有实现唯一持有 MemoryStore owner，连同其容量 lease 移动，不另建等价数组类型。`MemoryByteRangeOutput` 的 DataCodec 自有实现直接写该 owner，Finalize 后交付，无需复制出第二份 vector。内部 EncodePipelineResult 的对应结果同样改用该对象。

完整内存输出的容量检查有两个确定时点。布局已知时，在消费一次性输入或开始不可重放写入前申请完整目标；布局仍依赖压缩结果时，写入过程中逐次检查真实 required 与扩容容量，最终大小在 Finalize 确认。后一种路径不承诺“开始编码前即可知道是否一定够用”，也不为了获得 F 再执行一次编码。

延后完整结果的分配只等待与该分配无依赖环的既有释放，规则见 7.6 节。压缩输入、尚待封装的字段 store 和旧输出数组需要保留到被消费或复制结束，不能计为“任务排队后即可释放”。直接内存写出路径在流中扩容失败时按容量/分配错误收束，不引入挂起任意 writer 调用栈的恢复机制。内部暂存继续遵守 7.4 节的固定后端规则；最终返回的 EncodedBuffer 必须完整驻留内存。

现有仓库调用方按只读字节视图读取新对象，删除依赖可变 vector 的调用。用户自行复制结果形成的新 vector 属于调用方数据。明确要求可变 STL 返回值将另增一次复制和新的边界，本文不保留这种兼容接口。

这项公开变化是数据结果类型变化，不是对外开放内存管理能力。EncodedBuffer 不暴露预算、lease、释放其他对象或申请任意存储的方法。

SegmentedBinaryObject::Materialize 的完整连续化能力按相同完整内存输出契约改造：消费分段前确定总长度，先为 MemoryStore owner 取得完整容量，结果通过 EncodedBuffer 交付；输入分段仍存活的容量纳入并存判断。删除无准入返回可变 vector 的通道。既有源码核查未发现该成员的生产调用，这项事实不构成保留未受控完整复制入口的理由。

## 9. 数值编码的具体拆分

### 9.1 普通与区域残差编码

在 `NumericArrayTransferCacheBuilder.h` 中保留原入口名称，将内部循环改为具体 NumericEncodeCursor 配合 RunOrderedBlocks。

Cursor 持有 reader、元数据、固定块范围游标、输出 store/writer、已提交布局和进度。BlockInput 持有当前范围及 raw owner；BlockOutput 持有 header、layout、压缩 payload。共享 CompressorConfig/region runs 以只读方式访问。

读取步骤在取得槽位后执行 `NumericArrayReader::ReadElements`。删除 acquireScratchQuota 参数。已有连续输入可借用时保持来源 owner，重排读取或连续化新分配归属 BlockInput。

ComputeBlock 复用 `ResolveEncodedNumericArrayBlockBytes` 和 `ResolveEncodedLayeredResidualNumericArrayBlockBytes`。分量转换、raw/component scratch、层间残差、候选 payload 都是块内局部变量。同一个块内依次处理分量，避免再展开一层 component 线程任务。

CommitBlock 复用 WriteNumericArrayBlock，再追加本块 layout、更新统计和进度。块范围游标只在读入成功后前移，最终偏移与布局只在提交成功后发布。结果输出 Seal 后才将 transfer cache 标为 ready。

删除 `NumericArrayTransferCacheRuntime` 中的 quota/lane 回调及 `useMemoryTransferCache`，保留必要计时信息并接入具体 store 用途。

### 9.2 reference 编码

在 `ReferenceTransferCacheBuilder.h` 中提取 ReferenceEncodeCursor，与普通编码使用同一个 RunOrderedBlocks。相同块的 current、reference、预测候选、普通编码候选、reference 候选共用一份槽位。

依赖字段或关键帧必须在开启此块流前准备完成。每块读取相应的 reference 范围；Predictor 的搜索范围由已有算法参数确定，在本块内完成必要分段读取，不能递归取得新块槽位或等待同池依赖。

重采样读取不能沿用“先把首末映射点之间的所有 reference tuple 装入 scratch”的实现。当前 NumericArrayReferenceBytes 中的 referenceRangeCount 由 floor(firstPosition) 与 ceil(lastPosition) 决定，当 reference 的全域元素数远大于目标时，这个范围可以接近完整 reference。目标保留现有坐标映射、边界 clamp、插值公式与运算顺序，依次为每个目标 tuple 取得所需的相邻 reference tuple；读取窗口最多 1 MiB，跨窗口的左/右 tuple 由至多两份 tuple 暂存保存。单 tuple 本身大于窗口时，按原有类型与地址空间检查处理其真实长度，不分配中间所有未使用的 reference 值。该读取方式从函数入口固定执行，不能在大数组分配失败后再改用窗口。

Predictor 每个 offset 的范围与候选串行处理，读入和结果都附属于原块槽位。相同数据源的借用视图沿用源 owner；自有采样、插值结果和移位结果按各自真实容量审计。编码与解码共用这一范围读取实现。

Exact/Probe、Affine、Wavelet、Predictor 和残差路径继续使用已有算法选择及误差约束。`EstimateNumericArrayBlockStoredBytes` 当前用于候选表示大小比较，这类编码决策不纳入容量合计，不按名字中存在 Estimate 就一律删除。

current/reference 的暂存统一使用该 BlockRecord 拥有的 ScratchByteBuffer，直接提供连续字节 span，不再创建块内 ByteStore。删除 createStagingStore 和 acquireScratchQuota 回调字段、参数及调用，块任务不接收可替换内存设施。候选比较完成后立即释放未选择的候选；选择结果由 CommitBlock 顺序写出。

### 9.3 区域规划的字段级与块级拆分

NumericArrayRegionPlan 当前在 ValidateRegionRunsForEncode 中复制全部 runs 排序，BuildNormalizedRegionPlansFromRegionRuns 又从每块调用该校验；clippedRegionRuns.reserve(regionRuns.size()) 会为每块预留全字段 run 数。maxRunsPerRegion 只限制归并后的输出，不能限制这些前置数组。

目标在 NumericEncodeCursor 建立时，按真实 runCount 为一份 RegionRun 连续数组取得根容量，复制并排序，完成非空、范围和不相交校验。内部请求与字段参数只读共享这一份有序数组，删除随普通参数复制再次深拷贝大 run 数组的代码。源配置的既有数组属于借用；DataCodec 创建的有序副本保持自身 lease，直到该字段最后块完成规划和编码后释放。

按块偏移查找相交 runs，把当前范围传入规划函数。clippedRegionRuns 按实际相交数分配；targetRuns、mergedRuns、gaps、selectedGaps、isCore 仅按局部实际条数增长。合法、不相交且每 run 至少一个 tuple 的约束，使相交片段数受该块元素数约束；需要补默认区间时据实际间隙生成。最终 layer.runs 移入本块 layout，提交后作为格式 metadata 保存，不能继续捕获字段级有序数组。

precision levels、labelToLevel 和 compressor 配置在字段准备时建立一次，以只读状态交给块计算。它们按配置条目数增长，执行 7.8.5 节元数据边界；各层残差数与 refinedElementRatio 保留原算法校验。任何非法区域配置与算法质量条件失败调用 failure，不通过删除区域精度要求继续编码。

### 9.4 字段 reference 选取阶段

AttributeReferenceScheduleBuilder 的 sampleIndices 随算法采样设置增长，fieldSamples 并存量取决于同组字段数、维度和类型；edges 在 Forced 模式下可能包含组内几乎所有有向字段对。三类数组均列入根容量，不能仅用一个采样数或“单个重型 stage”说明已限制总量。这里的 intraField.sampleCount 现有缺省为 256，与块内 reference 候选的 4096 tuple 探测具有不同职责；保留其算法语义，运行资源调节不改变该设置。

按现有匹配布局分组顺序逐组处理，sampleGroups 只保存字段分组描述，不预建各组 sampleIndices。字段数少于二的组不建立样本；当前有效组先计算 `s0=min(E,max(1,intraField.sampleCount))`，由 driver 按 `s0*sizeof(size_t)` 为索引数组取得容量，然后在终端函数中沿用原等距索引、舍入、clamp 和相邻去重顺序。s0 是这笔确定分配的元素容量，去重只缩短逻辑长度，不把两者混为审计值。

索引构建返回后，driver 按实际索引数 × D × V 为本组各字段的样本一次申请容量，再提交取样和评分。读样本的每次源请求限制为最多 1 MiB；无法容纳一个 tuple 时只取该 tuple，按第 18 节处理真实长度，不整段读取连续的全部样本。每组评分按原有 child/parent 次序生成真实通过条件的 IntraFieldEdge，追加到一个带容量 lease 的连续数组；不为全部潜在字段对提前申请空间，不重新执行评分来计算条数。组内评分完成即释放本组索引与样本，保留已产生的边。

全部组处理结束后，在同一个边数组上执行现有 score、child、parent 排序及防环选择，生成 parentOf 和 topologyOrder，随后释放边数组。parentOf/拓扑顺序属于字段级轻量元数据；计划中 parentSource 只持有来源引用。所有组与边共享同一 M，没有 reference-schedule 专用额度。样本准入或实际边数组增长不能满足时执行第 7 节失败规则；不因资源不足自动关闭被请求的 reference 算法。

## 10. 拓扑、重排和封装

### 10.1 connectivity

保留 OrderedTopologyCellSource、EncodeTopologyBlock、已有整数编码及 layout 计算。范围按需生成；每个窗口内 BlockRecord 持有一个 TopologyBlockEncodeArtifact。

块顺序提交时计算 connectivityOffset，更新 layout 并将 payload 消费进受控暂存。删除 `vector<Artifact>(全部块数)`、全量计算后汇总和 ParallelForChunksAllowNested。

当前 inversePointRemapValues/cellOrderValues 的整数组访问统一使用确定容量的 MemoryStore owner，在 connectivity 阶段开始前一次取得完整数组容量。RemapProvider 增加写入调用方 span 的 ReadRange 重载，直接填充该 owner；删除原 vector 再复制的中间层，不新增探测 provider 连续视图的另一套路径。源 provider 与目标数组并存期间各自保持真实 lease。Identity provider 通过现有 IsIdentity 协议直接采用恒等索引，不创建整数组；非恒等 provider 读取失败不得转恒等索引。

### 10.2 Morton 与全局预处理

保留已有外排、run store 和 provider。删除 EstimateMortonWorkspaceBytes 对 ActiveByteBudget 的整阶段申请及可空 scratchBudget。输出 order/inverse 使用长期 store 的确定容量；小规模 keyed/排序缓冲受固定内排执行规模限制。

key cache 为可选的已知 `elementCount * sizeof(uint32_t)` 存储，elementCount 表示待排序元素数，槽位计数 N 保持其独立含义。driver 在 Morton 计算开始前检查可选留存开关并调用一次 TryReserve；未获准时预先选定现有按需 keyGetter 模式，获准时创建 cache 并固定本次模式。实际数组分配、填充和 keyGetter 计算失败均进入 failure，不删除 cache 后重新计算。不能在预算为空时默认开启无限 key cache。

Morton 的 leafBudgetBytes 固定为内部执行常量 8 MiB，runBufferBytes 固定为 1 MiB，集中定义在 MortonRemapBuilder 中，删除请求及 workspace 上对应 setter。继续按现有 `leafBudgetElements = max(1, leafBudgetBytes / (2 * sizeof(IndexType) + sizeof(uint16_t)))` 划分内排叶，跨叶由现有外排算法处理；这些数值限制执行规模，不写入基础块格式，也不代表整个 Morton 阶段的占用。run store 按 7.4 节确定后端。全局预处理只允许一个重型阶段活跃，driver 组织有限终端工作，worker 不嵌套等待。

WriteHighBucketRun 的 highBuffers 需要单独改造。当前 65536 个 vector 分别在追加完整记录后达到 1024 字节时刷出，clear 保留 capacity，聚合量不受 leafBudgetBytes 限制。目标在 highCounts 确定后计算 `r=sizeof(uint16_t)+sizeof(IndexType)`、`q_i=min(highCounts[i],ceil(1024/r))`，以 checked arithmetic 计算 `A=sum(q_i*r)`。这是本次明确创建的所有桶缓冲切片容量，每片仅为该桶实际计数与固定刷出记录数的较小值。

为 A 取得一笔必要连续 MemoryStore 容量，建立前缀偏移表，各桶按切片写入，到 q_i 条完整记录即刷出；删除每桶可扩容 vector。high run 完成后释放整片数组和 highWriteOffsets。r=6 时，每个满切片为 1026 字节，65536 个满桶的数组合计为 67,239,936 字节；这份容量单独进入 M，不能藏在“1 KiB 小缓冲”描述中。运行中不缩短已选切片或在申请失败后重写外排路径。

keyed、order、inverse 的内排源数组以及 scratch/orderedElements/lowBuckets 的低位排序数组保持固定元素规模，分别审计真实 capacity。std::stable_sort 自行申请的临时存储按标准库内部边界豁免。输出 provider 创建前确定 count×I，自有长期输出持续计入 M；临时源与目标并存期不能只统计其中一份。删除 VectorRemapProvider 被作为生产无预算出口的分支，恒等 provider 继续保留。

### 10.3 polyhedron 和字段封装

删除 PolyhedronTopologyEncode 的 work-budget 估计生成和记录代码；保留拓扑格式、拆分和编码算法。其五条状态流按单元顺序连续编码，状态可以跨 cell 延续；本次不把它改写为五条互相独立的并行块 codec。

编码前 driver 为校验用 visitedFaces 按 faceCount 个 uint8_t 申请根容量，再提交校验终端函数。校验在已有 cell-face 遍历中按 checked arithmetic 累加每 cell 的面顶点引用数，返回是否存在超过现有线性查找阈值 24 的 cell；共享面仍计入每个引用它的 cell，不因 visitedFaces 已置位而漏计。只返回一个需要查表的布尔值，不建立全域 cell 计数副本。

校验结束立即释放 visitedFaces，driver 根据该结果在开启 cell 编码流前完成 localIndexTable 的必要准入。表长沿用当前代码的来源：存在 point inverse provider 时取其 Size，否则取 pointCount，验证点域契约后按表长×I 分配并初始化。全部 cell 满足小单元条件时不创建该表；需要时只建立一份串行复用，cell 编码终端函数只接收已准备的表，编码阶段结束释放。overflow 数组和 PolyhedronCellWorkBuffer 的长度来自当前 cell 的真实面顶点计数，归当前 cell 批次槽位。

编码按最多 65536 cells 的执行批次推进，同一时刻只处理一个批次记录，五条流的游标属于阶段对象。PolyhedronTopologyStreamEncoder 的五个字节缓冲分别固定为 1 MiB；编码一个 varint 使用最多 10 字节的栈临时量，再按剩余窗口分段追加并同步 Flush，允许 varint 跨 I/O 写入边界，不改变连续流内容。局部编号 bitpack 的尾位只在 Finish 中补齐，不能在每个执行批次重新补位。五条 spool 跨批次保留完整已编码流，统一执行 7.4 节追加存储规则；它们不由槽位回收。最后一个批次归还槽位前完成 Finish、CompleteStream、Seal 和字段就绪发布。

解码先用 HeavyPhaseLease 和固定 1 MiB 范围窗口准备五个 DecodedIndexCache，元素数量与用途见 7.8.3 节。unique/cell-face/face-vertex 计数和 unique IDs 在后续流验证、local-ID 解码及 emit 中继续使用，按实际最后消费时点释放。完成五条流后，EmitPolyhedronCacheToAdapter 使用同一块循环按最多 65536 cells 生成宿主批次；删除当前由 accessWindowBytes 派生 polyhedronBatchBytes 的字节批次判定。

emit 先以固定窗口扫描当前批次计数，得到实际 b/f/unique/local 数量，再建立结果 offsets 与 ids。删除为“尚未接纳的下一 cell”先创建 nextFaceVertexCounts 全数组再检查批次的方式；扫描下一 cell 所需 face counts 逐窗口求和，单 cell 的极端长度按第 18 节处理。计数数组、offsets、ids 和同步宿主转换可能并存，审计逐个记录 capacity。五个全量索引 store 在整个 emit/必要 reference 消费结束后释放；批次写入成功只释放该批次工作数组。

EncodeOutputWriter、LeafPackageFieldEncode 和 FrameEncodeExecutor 保留字段顺序、package header 回填、bundle 及帧布局。package Zstd 使用有限输入输出窗口处理已生成的字段 store。字段、叶和帧累积数据一直由长期 owner 持有，不能在基础块结束时清掉其容量。

首次进入最终输出阶段按 5.4 节取得 HeavyPhaseLease。已经准入的块或输出阶段内，必要写出、Seal、header 回填和清理不再次等待 gate，避免已持凭证的结果等待自身排空。仅完成上游块计算不代表尚未开始的最终输出阶段已经准入。

## 11. 解码与 observer

### 11.1 数值解码

AttributeDecode 的字段依赖由 driver 按稳定顺序准备。删除独立 inFlightCount、workerCount、readyQueue 和任务成本/aging 调度；按文件已有 NumericArrayBlockHeader/Layout 建立 DecodeCursor。

遇到合法文件中超过新基础块规格的旧块时，不按新规格拆开其压缩字节。该块流每次只推进一个记录，前一记录消费后才读取下一块，继续使用同一块循环与槽位；在读取大负载前完成已知格式大小和地址空间检查。这是旧块访问规则，Fixed 的全局 M/C/S 保持不变，实际 N/R 至多为 1；Adaptive 对该流停止规模增长。单块必要分配失败进入 failure，不通过重试或压缩率估计继续推进。

ReadNextBlock 由 driver 串行协调：取得槽位后，顺序字段流的外层解压按 5.4 节通过 RunTerminalWork 完成有界准备，计算计入 R；当前块就绪后再提交 ComputeBlock 调用单块解码/参考重建，CommitBlock 写确定的字段范围。字段流只有一个 Cursor 和解压上下文，不允许并发推进，也不在 driver 上执行未取得额度的解压计算。Attribute 字段已经按 11.4 节准备为可随机读取的 payload 时，ReadNextBlock 直接读其块范围；不再为每块重新执行外层解压。

完整字段目标由 BeginAttribute 一次确定逻辑形状。DataCodec 自有目标 store 先做长期容量准入；adapter-backed 新目标属于宿主输出，维护其有效生命周期，宿主预先存在的输入才属于借用。数据转换缓冲继续归属槽位。已完成字段才发布 complete 标记和 reference 可用状态。

### 11.2 拓扑解码

替换 TopologyDecode 现有 blockState 门禁为 SlotLease。encodedBlock input reader 属于 BlockInput；worker 一律解码到 BlockOutput 自有的本块结果，不直接写全局拓扑目标。driver 按序提交目标、发布 metadata、调用 observer，完成真实消费后归还槽位。

删除 ParallelDecodeTopologyBlockObserver 的独立 runner、pending 队列及对应调度实现。DataCodec 自有的块内 CPU 后处理合入同一次 ComputeBlock，在其发布完成前结束；提交和 observer 回调统一在 driver 同步执行，不再增加计算后的异步消费队列。Begin/End 不提前于对应实际消费。

公共 IDecodeTopologyBlockObserver 继续是业务协议。契约明确为同步接收：回调返回表示本次调用结束；按值参数转移到调用方后属于调用方拥有的结果。DataCodec 不为调用方自行建立的后台队列承诺内存上限。内部提交不把块数据存入另一个后台队列，不借公共回调返回提前释放尚未消费的自有结果。

### 11.3 adapter 提交边界

DecodedCacheCommit 的 Begin/End 及范围提交统一由 driver 串行执行，完整字段复制以 1 MiB I/O 窗口顺序推进；删除 attributeCommitLanes 的配置、传递和并发提交实现。删除 SupportsConcurrentAttributeRangeWrites 的声明、全部仓库实现及调度查询调用。块的解码与 driver 提交通过槽位窗口重叠，不再为提交建立一层线程任务。

宿主预先存在的数据才称为借用。自带 iGame adapter 在 BeginPoints/BeginAttribute 等位置创建的新原生数组属于宿主输出分配，不能标记为零成本借用。其内部 allocator 按 7.5 节的宿主输出边界豁免，不接入确定容量账本；可确定的逻辑输出量单列，用于必要工作集检查，不称为精确 capacity。

DataCodec 自有中间 store、转换数组以及 EncodedBuffer 继续接入清单。这一边界意味着完整宿主结果本身可能超过 DataCodec 存储额度；完整结果交付的最低需求和不承诺整进程硬上限必须在接口说明中写明。

### 11.4 Attribute 外层 payload 的准备

当前 AttrDecodeStage 的 PreparedAttributePayload 缓存的是外层字段解压后的数值编码数据，完整属性浮点结果保存在 DecodedAttributeCacheSet 等目标中。前者使用 LeafPackage::Field::rawSize，后者按各属性 E×D×V；二者可以同时存在，不能共用一个逻辑字节数记账。

属性依赖与按需属性解码需要按 payloadOrder/range 访问不同属性，本次保留准备可范围读取 payload 的结构。该前置阶段由 driver 在开启属性块流前取得 HeavyPhaseLease，通过同一计算池的终端工作顺序推进外层流；输入/输出窗口分别至多 1 MiB。明确删除 AttributeDecodePayloadMode 的 OneShotZstd、Memory、Managed 三组性能选择、CanPrepareOneShotZstdAttributePayload 和全量 Zstd 解到 vector 的生产路径。

具体规则固定为：外层 None 时校验 source 存在且长度符合布局，直接共享其范围读取接口；外层 Zstd 时，依据经过检查的 rawSize 在创建前走 7.4 节已知长度范围存储规则，之后使用 FieldDecodeStreamReader 填满并验证结束状态。缺失 source、非法布局及实际范围读取失败均进入 failure。不因连续视图不可用而进行整份连续化，也不因解压或 I/O 失败切换路径。store 为 Memory 时，输入 owner、payload owner 与已经存在的必要 reference 分别保持真实容量。

workspace 为当前属性请求只保留一份 prepared payload；所有目标及其前驱属性完成后解除该引用，部分解码会话保留原始 reader 和必要 decoded 数据。后续独立补充属性请求按相同规则重新准备所需输入。它属于新业务请求的准备步骤，不能用于恢复前一个失败请求。几何顺序解码继续直接消费外层流，拓扑的四段块输入与多面体五个完整索引 store 按 7.8.3 节分别管理。

## 12. 缓存和 reference 的内部实现

删除 IDecodedFrameCache、IEncodedInputCache 和外部 cache/runtime setter。DecodedFrameTypes 只包含 frame payload、lease、查询结果等数据协议；frame cache 的管理类型留在 Runtime/Cache。

DecodeCacheRuntime 由根状态持有，删除静态 DefaultDecodeCacheRuntime 的声明、定义及取用代码。Runtime/Cache 下现有 DecodedFrameLruCache、EncodedInputLruCache、DecodeReferenceCache 保留查找结构，LruCacheIndex 保留顺序与条目数，删除各索引独立 resident 字节额度和使用 ResidentSizeHint 计算的预算。

### 12.1 共用的可选留存与回收政策

scratch 归还、可选缓存插入、完整输入预读和 Morton key cache 的启动都读取根对象中的同一份政策。业务开关只决定该优化是否被请求，根政策再决定当前是否允许；各模块不保存自己的压力状态机或恢复计时器。已准入任务仍在使用的 key cache、输入和 reference 继续受必要引用保护，禁止回收其活跃数据。

根对象保存 optionalRetentionPausedByPressure 和 trim epoch。正常启动时压力暂停标志为 false；M=0 或根对象 closing 时一律禁止新增可选留存。Fixed 不更新压力暂停标志，M>0 时按固定数量/容量规则工作。Adaptive 确认压力时立即置位，禁止新增可选留存并发出一次 trim；进入 NORMAL、无待确认压力、增长冷却结束且连续 2 s 取得有效高水位样本后，控制器清除暂停标志。恢复留存不要求同时提高 M/C/S。仅打开新块 gate、提高 M 或收到信号未知状态都不能提前清除压力暂停。

UpdateLimits 下调 M 或 C/S 时必须发出相应回收请求，M=0 同时使可选留存不可用；该基础动作不依赖自动控制器。Advance 产生的压力暂停与恢复结果在同一个执行锁内随目标发布，回收在锁外推进。一次压力事件只发一次全量 trim；额度下调和准入前必要容量检查可以各自提出具体回收请求，重复通知不引起重复全量扫描。

scratch 只释放空闲 buffer，数量规则见 8.2 节；cache 只移出可选引用，最后 owner 的真实析构才归还 U。控制线程可处理纯自有 scratch，cache 和可能触发宿主析构的对象由 driver 在锁外处理。对象归还时读取最新政策；压力暂停期间借出的 buffer 在恢复条件尚未满足时归还也直接释放。清理失败请求持有的临时量直接释放，不借回池延长失败数据寿命。

容量检查只在实际自有分配处进行。缓存保存一个已经带容量 lease 的 owner 不再次收费。必要存储创建前，driver 使用确定的 A/U/M 检查容量；不足时按 encoded-input、decoded-frame、不再必要的 reference 次序完成一次可选引用回收，再进行正式准入。每类按 LRU 顺序释放可选引用，实际剩余额度足够时结束本次回收；必要引用始终保留。回收动作不声称释放了仍被共享的数组，不预测未来回收量，不在实际分配失败后重复扫描重试。可选 cache 自身创建不为争取容量驱逐其他 cache。

### 12.2 reference 与缓存数据所有权

必要 reference 在消费期间由显式 owner 引用固定。它不因缓存淘汰而失效，不要求依赖任务在获得块槽位后再去执行另一整个解码请求。所有必要前驱先在 driver 的业务依赖顺序中准备。

对于宿主 payload 容量未知的完整帧，首版采用明确的条目数约束，缺省最多保留两个可选完整帧；完整输入预读缓存至多保留一个 source，额外 reference 只保留实际依赖要求的对象。压力期间释放可选引用。未知字节不填零参与容量合计，不声称该条目数构成字节上限。当前呈现结果及调用方持有的结果不受缓存驱逐强制销毁。

自有完整输入预读必须先取得文件长度对应的容量并读取到自有 owner。调用 RetainAllBytes 返回的既有宿主输入只保留生命周期，不重复创建容量 lease。不同来源身份仍用于缓存 key，外部 cache 指针身份从 loader 去重 key 中删除。

完整自有预读与操作系统范围预取具有独立的所有权口径。完整字节副本统一由 loader 管理；PackageDecodeWorkflow 中 enableFullInputPrefetch 对全文件发出 PrefetchRange 的调用及开关删除。原生 reader 可以保留只读 mmap，已准入读取当前窗口前至多发出一次同范围的预取提示，压力暂停时不发提示。Unavailable/RejectedByPolicy 表示未启动该可选动作，继续正常 ReadAt；已执行提示返回 Error 时按第 16 节处理。映射页不能作为根 M 的预约量或释放量。

EncodedInputCacheLoader 保留对同一来源重复加载的合并，合并等待接入取消和首错通知。删除先整文件分配、读取完成后才向 cache 申请保留的旧顺序；按 7.4 节在预读开始前确定容量许可，未启动预读时直接执行已有范围读取。

### 12.3 Playback 的有限请求窗口

Playback 的 DecodeTaskCoordinator 保留 key 去重、Interest 取消和结果通知，改为登记待运行的业务命令。一个后台 driver 顺序执行命令，目标帧所需 reference 直接在该命令的前置依赖流程中完成；删除向计算 worker 提交完整请求和 SubmitInline/Submit 两套资源路径。当前运行、一个前台待运行请求和一个可选预取请求构成有限请求窗口，相同 key 合并；前台窗口已满时拒绝接纳新的不同请求，通过现有请求失败通道报告“待运行请求窗口已满”，不静默替换已接受的请求。

该拒绝属于业务准入结果，沿用 PipelineFailure 与具体原因文字，不新增 Busy 错误码，不停止当前请求或关闭根对象。4.4 节的请求级 stop source 与根级 closing 负责不同范围；Interest 的取消和异步完成关联原请求身份，迟到事件不能停止下一条命令。已接受命令的执行队列提交发生实际失败时，按根执行设施错误处理，不归类为窗口已满。

回调不得重入同一会话的阻塞 RequestFrame、Wait 或 Reset，也不得在回调内同步关闭根执行对象。该约束通过内部 driver 身份检查返回明确错误，禁止形成 driver 等待自身完成的循环。关闭请求与真正的 ShutdownAndJoin 分工统一执行 4.4 节。

## 13. 第三方线程和上下文

### 13.1 数值块

当前固定使用的 SZ3 Pressio 插件在仓库 `ThirdParty/libpressio/src/plugins/compressors/sz3.cc` 接受 `pressio:nthreads` 和 `sz3:openmp`。DataCodec 的 ConfigureCompressor 需要在用户精度选项之外，以正确的 uint32/bool 类型显式设置 `pressio:nthreads=1`、`sz3:openmp=false`。

这些线程选项由内部执行规则设置，不能经原有 double 选项表绕过。精度选项保持不变。块并行阶段不启用每块内部 OpenMP 并行，第三方内部内存继续豁免。

### 13.2 package Zstd

删除 RecommendWorkerCount 及零值触发硬件自动推荐。块内 Zstd 使用同步模式，底层 `ZSTD_c_nbWorkers=0`。

package Zstd 固定作为排他终端计算任务，driver 在所属阶段准入后只提交任务，不预占计算额度。worker 出队时在执行锁内等待 R=0 且没有其他排他任务，按当时的 C 确定 w，增加 R 并设置 exclusiveComputeActive；随后在锁外执行。C 小于 3 时取 w=1，设置 ZSTD_c_nbWorkers=0；C 至少为 3 时取 w=C，设置 ZSTD_c_nbWorkers=w-1。一个额度对应当前执行调用的 worker，其余额度覆盖 Zstd 内部 worker；外层不会为同一任务再加一个额度。

排他任务活跃期间禁止其他终端计算开始，即使 w<C 也保持排他。结束时在同一执行锁内归还全部 w 并清除 exclusiveComputeActive，通知等待者。设置参数和启动库线程失败均进入 failure，不改用同步方式重放；失败清理同样归还 lease。这个规则与 5.3 节“在执行时取得额度”共用同一实现。

这类任务的计算 lease 保存取得时的实际额度数，运行期 C 下调不修改已有 lease。库调用完成后一次归还，其他任务不会与它叠加超出当前已准入规模。此处只有确定线程数量，没有任务内存估计。

### 13.3 compressor owner

把 ThreadLocalCompressorCache 的拥有关系移入 worker 自有上下文，每个 worker 首版只缓存最近使用的一个配置实例；配置变化时在同一 worker 上销毁旧实例并创建新实例。删除当前按配置无限增长的 TLS map，以及 Windows Clang 的进程寿命裸指针实现。

销毁通过常规 compressor API 完成，发生在工作线程退出前，不能留给进程 TLS 析构顺序。关闭时先停止新任务、完成当前调用、销毁该 worker 的 compressor owner，再退出线程，最后销毁运行状态。主线程不跨线程销毁仍被调用的 compressor。

这项管理的是 DataCodec 选择保留的第三方句柄数量和寿命，不接管第三方内部 malloc，也不把其未知内部容量加入审计。Windows Clang 路径需要独立回归验证，源码中的旧析构规避说明不能直接作为新顺序已安全的证明。

## 14. Adaptive 模式的自动控制器

本节完整定义 Adaptive 的目标生成策略。Fixed 不进入压力状态机，不进行观察批增长、自动 M 调节与 HOLD 恢复等待；正常的任务/容量等待、取消及失败收束仍使用共用机制。NORMAL、DRAIN、HOLD 是 Adaptive 控制器的内部状态，顺序执行和无外存是平台能力配置，均不作为第三种资源操控模式。

控制器负责系统压力响应、有界供给和后续计算准入，不计算剩余内存可以容纳几个块，不读取审计合计，不根据压缩率预测工作集，也不在线寻找全局最优吞吐。执行前提是基础块工作集的边界已明确、长期 owner 已接入容量与生命周期规则、块结果能够实际消费或受控转移。自动控制不替代这些前提，也不增加吞吐预测器、逐算法内存学习模型或第二套自动缓存扩容循环。

### 14.1 纯状态更新与执行动作分开

```cpp
struct ResourceSample {
    std::optional<std::uint64_t> physicalTotalBytes;
    std::optional<std::uint64_t> availableBytes;
    std::optional<std::uint64_t> hardLimitBytes;
    std::optional<std::uint64_t> hardRemainingBytes;
    PressureLevel pressure;
    MonotonicTime sampledAt;
};

struct FlowSnapshot {
    RuntimeResourceLimits limits;
    std::size_t admittedBlocks;
    std::size_t activeComputeUnits;
    bool heavyPhaseAdmitted;
    bool moreIndependentBlocks;
    FlowWaitDurations waits;
    ObservationBatch observation;
};

struct ControlDecision {
    ResourceDecisionReason reason;
    bool gateOpen;
    RuntimeResourceLimits limits;
    bool optionalRetentionPausedByPressure;
    bool trimOptionalRetention;
    bool failForSustainedPressure;
};

ControlDecision Advance(ResourceControllerState& state,
                        MonotonicTime now,
                        const ResourceSample& sample,
                        const FlowSnapshot& flow);
```

Advance 不获取 OS 信息，不访问 cache，不发任务，不分配块缓冲。它从 FlowSnapshot 读取当前有效 M/C/S，输出完整 ControlDecision；执行层在同一执行锁内经 UpdateLimits 的内部实现发布目标，并更新 12.1 节的压力暂停标志，锁外处理回收与失败收束。控制器不直接修改 ByteBudget、worker 和 workspace。生产路径传真实时钟/平台样本，测试直接构造样本；公共请求中不存在 probe/provider 注入字段。

ResourceDecisionReason 使用 4.3 节的同一固定内部枚举，覆盖无变化、请求初始化、新请求边界、内部机制验证、压力待确认、压力确认及升级、排空完成、压力解除、信号失效及恢复、硬能力变化、类型切换、M/C/S 增长、留存恢复和压力超时。一个决定携带一个主原因，其他同时变化保存在同一记录的前后状态中；不在 Advance 内拼接原因字符串。NoChange 不写事件，平台采样值与有效时间继续更新到当前快照。

NORMAL/DRAIN/HOLD 控制器状态、待确认标记、首个低水位时间、HOLD 起点、恢复确认起点、冷却基值、上次压力/增长时间、当前工作类型、当前观察批和采样有效性均集中在 ResourceControllerState。另保存上次 M 增长时间，不设置待发布的 M 收缩目标、未来工作集估计或模块份额表。本节公式与阈值集中放入该模块一份默认常量，不在 codec 或 workspace 复制。

### 14.2 事件与同步

driver 和 worker 在状态变化时累计 Wc/Ws/Wi/Wo 的持续时间，使用单调时间的区间长度；控制线程读取稳定快照，不按采样命中次数计数。工作类型 key 来自当前 codec、类型、分量数、固定块规格、reference 路径和实际第三方线程参数，不含测得或估计的内存量。

单一顺序块流用“观察批最后序号 + 连续已归还序号”判断首批 S 个槽位是否全部归还。Retire 中实际归还槽位后才能推进已归还序号；计算完成、CommitBlock 返回和轻量记录销毁各自具有不同语义。改变目标或类型时生成新 observation epoch，旧任务归还不能误完成新观察批。仍需等待该批归还后的新鲜采样才允许增长。

控制线程每 250 ms 采样，平台压力通知只设置事件并唤醒。先在执行锁外采样，锁内取得流状态并计算，通过 UpdateLimits 的同一发布逻辑同时更新 gate/M/C/S，锁外通知并请求可选资源回收。锁内调用使用该入口的内部已持锁实现，不能再次取得同一执行锁。处理优先级固定为取消与失败、严重压力、普通压力及排空、信号失效、恢复、增长；先检查样本有效性，失效数值不参与水位判断。同一次更新不同时恢复和增长。

完成与槽位归还事件推进排空和观察批；控制线程的定时等待推进采样、压力确认和超时。全部 worker 忙碌或全部新准入暂停时，控制循环仍可触发。任务热路径只维护必要计数和状态事件，不查询操作系统、不扫描堆、不更新逐分配预测表；平台采样与控制均独立于日志开关。

压力确认、恢复留存和 trim 请求统一执行 12.1 节；本节只负责定时事件与同步发布，不定义另一份 cache/scratch 政策。driver 阻塞在 I/O 时，cache 实际回收可能延后，新准入已经关闭；不承诺同步 I/O 能被立即取消。重复通知不重复全量扫描，不重置同一请求的 HOLD 超时起点。

### 14.3 非块阶段和排空

非块阶段的具体范围和首次准入统一由 5.4 节规定。HeavyPhaseLease 从阶段正式准入保持到其实际提交与清理完成，最多同时一个；它不换算为槽位字节。最终输出首次准入和已准入输出的继续执行按同一规则区分。

RunOrderedBlocks 所在的整字段 stage 不持有覆盖整个块流的 HeavyPhaseLease。块流在 gate 关闭后让当前窗口排空，并等待下一次开放。DRAIN 的等待对象限定为已准入工作；覆盖整个字段会阻止剩余块取得准入，禁止建立这类等待。

DRAIN 完成条件为本轮 N=0、R=0 且没有已准入非块阶段尚待完成。reference 等前置依赖在块准入前准备；已持槽位的字段外层解压准备按 5.4 节继续推进，不在 DRAIN 中新接纳另一批依赖块。当前块提交与已准入输出阶段内的容量检查、写出、Seal 和 header 回填不再等待新阶段 gate；尚未首次准入的最终输出阶段保留待准入位置。确认压力所触发的 DRAIN 完成后按 HOLD 规则等待；仅由信号失效触发且没有已知压力的排空按 14.10 节恢复保守执行。

请求已经完成最终交付时直接结束。只有仍有未准入必要工作时进入 HOLD。控制器不对执行中第三方调用或阻塞 I/O 使用 30 s HOLD 超时，不能把它误作任务强制中断期限。

### 14.4 Adaptive 通过共用机制更新目标

本节是 Adaptive 模式的策略实现，依赖 4.3 节与 Fixed 共用的机制。M 控制清单内长期自有存储的许可，C/S 控制计算与在途数量。部署天花板 Mmax/P、平台硬能力、当前运行上限 M/C/S 分别保存；文件 metadata 不保存这些值。Fixed 的启动解析不调用本节控制器。

C/S 按 14.7—14.10 节的观察批、收缩、冷却、类型切换及信号规则调整。M 使用以下唯一规则：

1. 首个请求按 7.2 节发布 M0。Playback 下一条独立命令只在前一请求已经完整收束时重新计算 M，使用当时的新鲜环境值及原 Mmax；不能在字段、叶、reference 递归或 DRAIN 结束时重算。缺少可靠新样本时保留已有 M，并遵守最新已知硬能力；控制器不将未知值当作零。新请求重新检查自身必要容量，前一请求遗留结果与 reference 的 U 不清零。
2. 同一请求中，普通或严重系统压力均保持当前 M。控制器关闭新块和新重型阶段准入，按压力等级收缩 C/S、停止可选留存并请求回收。DRAIN 与 HOLD 不减少 M，压力解除后继续使用原许可。删除按压力将 M 减半及延后发布 Mnext 的规则，不新增“必要内存保底预算”、第二套额度或未来需求预测。
3. 处于 NORMAL、准入开放、无待确认压力，连续至少 2 s 取得有效高水位样本、增长冷却结束且仍有后续工作时，允许提高 M；启动后的 NORMAL 同样适用。令 Mbound 为 Mmax 与最新已知容量硬能力的较小值，缺少后者时取 Mmax。相邻 M 增长至少间隔 5 s，首次从本请求开始计时；块窗口中仍有观察批时先等该批归还。取 `dM = min(Mbound - M, max(1 MiB, floor(Mmax / 16)), floor(max(Ae - H, 0) / 2))`，减法先检查边界；dM 为零时保持，正值时发布 M+dM。一次控制更新只执行 M 增长、C 增长、S 增长中的一项，M 增长条件满足时先处理 M。
4. 已知进程、容器、地址空间或运行时容量硬上限降低时，立即将 M 限制在新的硬能力内，保留已有 lease，后续必要申请遵守第 7 节。物理可用量、提交剩余额度和硬上限的当前剩余额度用于压力判断，它们减少不等于容量硬上限本身下降。缺少可靠内存信号时保持 M 并禁止自动提高；恢复后的增长仍受最新硬能力与 Mmax 限制。

UpdateLimits 的能力继续包含任意合法 M 升降，自动策略的调用时机以本节为准。对同一请求，自动下降 M 的唯一原因是已确认的容量硬能力下降；阶段切换、普通压力、严重压力和信号失效都不构成该原因。请求边界重新设定 M 通过同一入口完成，允许上调和下调，不切换已存活 owner 的后端。

例如，本请求 M=1 GiB、必要 reference 占 U=600 MiB，下一阶段需要新增 64 MiB 的必要连续数组。普通压力经过排空与恢复后，M 仍为 1 GiB，664 MiB 的确定并存容量可以通过预约。若平台硬上限实际降至 512 MiB，必要申请按容量不足失败。若启动时 M 本身只有 512 MiB，该请求也按第 7 节失败；本规则不承诺超过配置或真实能力的工作能够完成。

保留 M 不预先分配内存，也不允许在压力门禁关闭时启动新的重型阶段。已准入工作的实际分配失败、持续压力超时和真正的容量不足继续进入 failure。块级排空与可选留存回收负责缓解当前压力，必要 reference 和输出按真实寿命保留。

M 的十六分之一步长和可用余量比例只作用于全局许可，不向模块分配份额，不从审计合计、块平均值和压缩率计算申请量。M 的有效变化作废旧增长观察；保持 M 的压力事件通过状态切换作废观察。自动策略的完成率、吞吐及占用变化按第 17 节验收，历史公式校核不作为当前策略已通过验证的证据。

### 14.5 输入信号与有效环境

| 输入 | 来源与口径 |
| --- | --- |
| `T`、`A` | 操作系统物理总量、可用内存；可回收内存采用平台正式口径 |
| 已知硬限制及剩余额度 | 进程、容器或运行时提供的容量约束；未知项不参与数值计算 |
| 原生压力等级 | 平台提供的普通或严重压力状态；通知到达时及时处理 |
| `P` | 部署计算天花板，考虑可用 CPU 集、运行时线程能力及已知 CPU 配额；当前计算上限 C 在其范围内更新 |
| `Mmax`、`M` | 根容量部署天花板和当前运行上限；由内部执行状态提供，实际分配许可继续由确定容量 owner 检查 |
| `N`、`R` | 实际持有槽位数、实际活跃计算数 |
| 流水线事件 | 生产者等待槽位、就绪任务等待计算额度、计算缺少就绪输入、提交受阻及槽位归还 |

有可靠硬限制时，使用 `Te = min(T, 已知容量硬限制)`、`Ae = min(A, 已知剩余额度)` 作为有效总量和有效可用量。各值必须来自同一次附近的有效采样，未知值不以零代替。`Ae` 采用保守的最小值，诊断同时保留各约束来源。它表达平台压力，不能记入 DataCodec 自有占用。

Windows 的物理可用量、提交剩余额度，Linux 的 `MemAvailable`、适用的 cgroup 额度，以及受限运行时的可分配容量具有不同含义。平台适配层需要区分来源及有效性，已知限制接近耗尽时及时报告压力。地址空间碎片和单次连续分配失败仍走分配失败路径。

目标范围与正常准入统一执行 4.3 节，C/S 下调后的实际 N/R 和已有凭证继续保持真实。Te/Ae 也供 Fixed 启动时解析缺省值使用；持续采样只属于 Adaptive。

### 14.6 水位、时间与启动值

以下是首版内部校准默认值。它们集中定义，具有独立测试入口，不对外形成性能档位或动态设置接口。

```text
L = min(Te / 4, clamp(Te / 16, 128 MiB, 4 GiB))
H = L + L / 2
K = L / 2

定时采样间隔 = 250 ms
采样最大有效年龄 = 1 s
普通低水位确认时间 = 500 ms
瞬时低水位解除确认时间 = 500 ms
排空后的恢复确认时间 = 连续 2 s 高水位
首次压力后的增长冷却期 = 10 s
重复压力的最大增长冷却期 = 60 s
```

`L` 是停止新增负载的低水位，`H` 是恢复和增长水位，`K` 是紧急水位。普通压力使用 `Ae < L`，紧急压力使用 `Ae < K`，恢复与增长使用 `Ae >= H`。阈值相等时的行为按这些比较符号执行。

例如，256 MiB 有效容量对应 `L = 64 MiB, H = 96 MiB`；16 GiB 对应 `L = 1 GiB, H = 1.5 GiB`；1 TiB 对应 `L = 4 GiB, H = 6 GiB`。这些值是运行余量策略，不能作为满足单块分配的证明。控制器没有将可用内存降到水位附近的目标，输入有限或流水线缺少供给需求时不增长。

启动取得新鲜采样后：高水位且无压力时使用 `C = 1, S = 2`；介于低水位与高水位之间时使用 `C = 1, S = 1`；低水位或已报告压力时使用 C=1/S=1，保持新块和新重型阶段准入关闭，按 14.8 节确认与处理压力。缺少可靠容量信号时按 14.10 节处理；无 pthread 配置遵守第 15 节的 C=S=1 约束。计算池按需创建 worker，不随 `P` 一次性启动全部计算线程。

计时使用单调时钟。判定依据是持续时间，采样延迟不能通过补算若干个“连续样本”满足条件。普通采样间隔超过最大有效年龄时，连续良好时间和观察窗口失效。严重压力通知直接触发处理，不等待下次定时采样。

### 14.7 观察批与 C/S 增长

每次目标变化都重新开始观察。允许下一次增长需要同时满足以下条件：

1. 控制器处于 NORMAL 状态，准入开启，没有普通、严重或待确认压力，采样有效且观察期间始终高于等于 `H`。
2. 目标保持不变至少 500 ms，至少取得三份覆盖该时段的新鲜采样。末次 `Ae` 相对首份的下降不超过 `(H - L) / 4`；较快的下降会冻结增长，不计算剩余可容纳块数。
3. 当前观察轮开始后接纳的首批 `S` 个块全部归还槽位，并取得此后的新鲜内存样本。完成计算或发布结果不视为归还。
4. 仍有可接纳的独立基础块；字段尾部、串行依赖等待、全局预处理和独立封装阶段不触发块规模增长。
5. 已满足增长冷却期，观察窗口内没有持续提交拥堵。

首批 `S` 个块构成观察批，正常生产和消费继续执行，不为观察插入流水线屏障。按 14.2 节唯一的顺序水位与 observation epoch 确认归还，只保存当前观察批，不保存各块内存画像。当前任务剩余块数不足 `S` 时直接完成剩余工作。

一轮观察覆盖该批从接纳到全部归还的时间，最短 500 ms。一轮完成后，无论是否调整目标，都清零本轮等待统计并开始下一轮；不无限累加早期的等待比例。低于高水位、信号失效或工作类型变化时作废当前轮，条件恢复后开始新轮。增长冷却期间仍可以完成观察，冷却结束后的新一轮才允许增长。

整批完成证明当前规模的工作已经走过完整生命周期。定时采样仍可能漏掉批内短暂内存峰值，算法不声称测得了该批的峰值。长时间运行的块会延迟下一次增长，期间仍及时处理压力事件。

流水线使用状态变化事件累计以下等待时间，在目标稳定的观察窗口内计算时间占比；不按轮询命中次数计数：

| 记号 | 时间统计条件 |
| --- | --- |
| `Wc` | 存在就绪任务，且活跃计算已经达到 `C` |
| `Ws` | 仍有待读块，生产者仅因槽位用尽而等待 |
| `Wi` | 仍有可独立处理的数据，存在闲置计算额度且没有就绪输入；排除串行依赖等待 |
| `Wo` | 存在未消费结果，且提交端被下游容量、输出 I/O 或前序结果阻挡 |

`Wo >= 25%` 定义为本观察窗口的持续提交拥堵。该窗口不增长。短时写入与计算重叠属于正常流水线活动。以下动作按表中顺序择一执行，动作后开始新的观察批：

| 条件 | 调整 |
| --- | --- |
| `Wc >= 25%` 且 `C < P` | 增加计算额度；仅在供给窗口不足时配套增加槽位 |
| `Ws >= 25%` 且 `Wi >= 10%`，且 `S < min(Smax, C + Qmax)` | 只令 `S = S + 1`，计算额度保持不变 |
| `S = C` 且 `Ws >= 25%`，且槽位上限允许 | 只增加一个槽位，为读入与计算建立重叠空间 |
| 其余情况 | 保持目标，不为了接近 `P` 或消耗可用内存而增长 |

计算增长的步长和配套槽位为：

```text
本次运行尚未发生已确认压力：
    d = min(P - C, 4, max(1, floor(C / 4)))
    相邻增长至少间隔 500 ms，并满足完整观察批条件

发生过已确认压力后的恢复：
    d = min(P - C, 1)
    相邻增长至少间隔 5 s，并满足完整观察批及冷却条件

Cnew = C + d
Snew = min(Smax, max(S, Cnew + 1))
```

单独增加槽位也遵守对应的增长间隔。`Snew` 保留已有有限窗口，必要时为新增计算提供一个额外供给位置；没有 `C = min(P, S)` 的反向联动。启动增长每步最多增加 4 个计算额度，压力后的增长每步最多增加 1 个。

首版不增加吞吐预测器、每算法内存学习模型或第二套自动缓存扩容循环。`Qmax`、等待占比门槛和增长速度需要通过实际吞吐验证。受限试探存在启动成本，例如只考虑 500 ms 时间下界，`C` 从 1 增至 64 需要 23 次增长、至少 11.5 s；完整观察批可能延长这一过程。短请求直接在已取得的规模下完成，不等待控制器升到上限。

本节只决定 C/S 的增长候选；与 M 增长同时满足条件时，按 14.4 节先处理 M，一次更新只执行一种增长。M 变化同样作废旧观察批。

### 14.8 压力事件、排空、等待与恢复

Adaptive 控制器的状态只有 `NORMAL`、`DRAIN`、`HOLD`。首次低水位的短暂准入关闭使用一个待确认标记，不新增调度队列。

| 事件 | 门禁、额度和状态动作 |
| --- | --- |
| `NORMAL` 中首次 `Ae < L` | 立即关闭新块和新重型阶段准入，停止增长，开始普通压力确认；已有任务继续执行 |
| 低水位在 500 ms 内解除 | 连续 500 ms 满足 `Ae >= L` 且无原生压力后重新开启准入；目标不变，增长观察重新开始 |
| `NORMAL` 中连续 500 ms 低水位，或平台明确报告普通压力 | 确认一个压力事件，关闭准入，执行一次普通收缩并进入 `DRAIN` |
| 待确认压力已保持 2 s，期间水位反复波动且始终未满足解除条件 | 确认普通压力并进入 `DRAIN`，结束待确认状态 |
| 任意时刻出现 `Ae < K` 或原生严重压力 | 立即令 `C = 1, S = 1` 并关闭准入；有在途工作时进入 `DRAIN`，已排空时保持或进入 `HOLD`，标记严重事件 |
| `DRAIN` 中普通压力持续 | 保持已经下调的目标；不按采样频率重复减半 |
| `DRAIN` 中所有已接纳任务完成释放或受控交接，仍有后续工作 | 保持 M，进入 `HOLD`；继续观察系统信号和处理取消 |
| `HOLD` 中压力解除且连续 2 s 满足 `Ae >= H` | 使用下调后的目标开启准入，进入 `NORMAL`；开始增长冷却与新的观察批 |

普通收缩收紧额外在途窗口，并减少后续计算并发：

```text
Cnew = max(1, floor(C / 2))
Snew = max(Cnew, min(S, Cnew + 1))
```

例如，`C = 8, S = 12` 收缩到 `C = 4, S = 5`；实际持有的 11 个槽位继续计数，准入保持关闭直至本次排空结束。后续排空计算至多使用 4 个额度，原先已经运行的任务先正常收束。严重事件使用单计算、单槽位目标，恢复时也从这一目标开始。

一次确认压力对应一次普通收缩；同一事件升级为严重压力时允许进一步收缩。只有恢复新块准入之后再次确认压力，才形成新的压力事件。这避免在释放尚未完成时因同一个压力信号反复下调。

待确认超时从首次关闭门禁计时，短暂跨过低水位不重置它；满足解除条件或确认压力时结束。进入 `HOLD` 后的等待起点只记录一次，同一压力事件中的重复通知、等级变化和采样失效都不重置等待期限。所有连续时间条件都需要新鲜的末次采样确认，单凭旧样本和经过的时间不能宣布压力已经解除。

排空顺序优先推进可提交结果、释放已结束计算的临时量，再按顺序执行已接纳块及其必需依赖。`gateOpen = false` 阻止新块和新重型阶段，不能阻止已有计算、写出和清理。第 14.3 节的已准入工作继续推进；必要 reference 在块准入前准备完成，不在 DRAIN 中另开一批依赖块。

请求已经完成全部必要计算与最终提交时正常结束，不为等候内存恢复进入 HOLD。5.4 节定义的已准入输出阶段和清理继续完成；尚未首次准入的最终输出属于必要新阶段，仅这类未准入工作和后续新块需要等待重新开放准入。

确认压力时按 12.1 节暂停可选留存并发出一次回收请求，恢复留存也只执行该节的 NORMAL 与冷却条件。必要 reference、活跃数据和长期输出遵守自身寿命；不在每次采样时重新扫描容器。

排空结束后没有可继续释放的块级数据。系统仍处于压力时进入等待，默认最长等待 30 s，起点为进入 `HOLD`；期满仍不满足恢复条件则报告资源不足并清理请求。正在计算、提交或处理依赖时不使用这项超时判定任务失败。必要长期工作集已确定无法满足容量规则时立即失败，不等待该超时。

首次确认压力后，恢复准入时启动 10 s 增长冷却。恢复后 60 s 内再次确认压力，将下次增长冷却加倍，上限 60 s；连续运行 60 s 没有新压力时，冷却基值回到 10 s。冷却期间正常处理已允许规模的任务，不增长。每次恢复还需满足连续高水位条件；高水位期间新的待确认压力会重置增长观察。

C/S 收缩立即发布，同一请求中的 M 保持与硬能力处理遵守 14.4 节；排空完成按 14.3 节检查 N、R 和 HeavyPhaseLease，不将 U 归零作为排空条件。必要容量已确定不满足时执行 7.5 节失败规则。

### 14.9 工作类型切换

一组相邻基础块的算法、标量类型、分量数、固定块规格、reference 处理路径及第三方线程参数一致时，视为同一工作类型。只保留当前类型标识及当前目标，首版不维护类型到工作集或历史配额的映射表。

同类型的相邻字段、叶可以延续目标；任务尾部不为等待增长增加延迟。工作类型改变时，旧类型的在途块先完成交接，停止增长；正常准入允许开始新类型时，按 14.6 节的单计算启动值重新观察，已有更低的收缩目标继续保留。压力状态、收缩目标及冷却不会被类型切换清除。这个规则限制轻量路径学到的高并发直接作用于新的重型路径。

块级类型边界由 RunOrderedBlocks 的事前描述回调给出，只保留当前 key 和下一块 key。解码直接读取已有 layout，区分实际 bytesCodec、reference mode/kind 和分量 codec 组合；不读入 payload 以推断需求。普通尾块不因元素数较少另起类型，旧大块使用其真实规格。类型改变时停止后继准入及当前类型供给计时，队列已有记录完成提交和退休后再调用 SetWorkType，同一字段 flow 的序号继续递增。编码的 Auto reference 收益选择属于既定候选算法路径的一部分，准入使用事前计划，不按已完成候选结果回头改型或重放。

DataCodec 自有预处理全局数组、必要 reference 扩容和完整解码 store 不由基础块数量覆盖。清单内长期 owner 继续使用确定容量检查，在同一压力门禁允许时开始新的重型阶段；已接纳工作为完成或清理所必需的动作保留有界进展路径。槽位控制器不得为这些对象伪造若干份块容量。

上述类型切换只属于 Adaptive 的目标生成规则；Fixed 保持启动时的 M/C/S。当前块流无法提供更多独立工作时，按既有依赖与收束流程完成，不等待预热增长。

### 14.10 信号失效、硬能力变化与保证边界

- 启动时无法取得可靠 `Te`、`Ae`：使用 `C = 1, S = 1`，关闭自动增长，继续执行已有确定容量规则。可用内存有效且仅缺少原生压力通知时，正常使用采样反馈。
- 运行中采样首次变为失效或过期：停止增长，关闭新块和新重型阶段准入，下调到 `C = 1, S = 1` 并排空；没有尚未解除的已知压力时，以此保守规模继续。持续缺少信号期间不重复排空。恢复有效高水位采样至少 2 s 后，开始 10 s 增长冷却，再重新完成增长观察。
- 已经观察到的严重或普通压力不能被“信号未知”清除。此时保持等待，遵循恢复确认或 `HOLD` 超时；原生通知的解除状态由平台适配层明确提供。
- 已知有效硬限制降低时，作废旧观察窗口，重新计算水位并重新检查长期准入条件；检查完成前禁止增长。CPU 可使用范围变小时同步限制后续计算。
- 分配失败按正常错误路径停止、清理并报告资源不足；不自动重试部分完成的第三方调用，不重放可能已经提交的输出。操作系统直接终止进程不在可恢复承诺内。

这个反馈控制器不具备物理内存硬上限证明。两个采样之间的大分配、外部进程突发占用、第三方内部工作区和分配器保留页都可能造成超调。可证明的内容是准入有界、增长步长有界、目标下调语义、排空进展和清理规则。若测试显示单个基础块即可耗尽目标设备的必要容量，需要重新检查基础块规格、算法参数和长期工作集前提，反馈参数不能补足这类容量条件。

未知信号以有效性表达，不伪造为零可用内存；M 的保持、增长及硬限制变化按 14.4 节执行。压力通知缺失、容量样本失效和平台无监控能力分别处理，无 pthread 平台从入口采用第 15 节配置。Fixed 运行中不进入本节状态变化。

## 15. 平台能力与边界

ResourceProbe.h 只定义普通数值、有效性和能力类型。实现文件选择 Windows、Linux、Wasm 或无可靠采样支持的实现；生产层不引用宿主 UI 或浏览器对象。

| 平台 | 必须实现的口径 |
| --- | --- |
| Windows | 物理总量和可用物理量；提交剩余量与适用进程/job 限制单独标记；CPU 可使用范围；原生压力信号可用时接入 |
| Linux | MemTotal/MemAvailable、实际所属 cgroup 及祖先有效 memory 限制/当前用量、CPU affinity 与配额；未知或无限值不作零处理 |
| Wasm | 线程可用性、编译/runtime worker 上限、线性内存硬能力与平台实际暴露的信号；Adaptive 缺少可靠当前可用内存时执行单计算单槽位规则 |
| 其他平台 | 明确缺失信号；Fixed 只解析一次缺省值，Adaptive 使用已定义保守状态，不从进程 RSS 反推系统可用量 |

硬限制与物理可用量不同源，先校验时间和有效性，再取保守有效值。Adaptive 周期观察允许 CPU 范围和容量硬限制，变化时更新后续准入；不在每次 tick 反复创建销毁 worker。Fixed 的数值在运行前确定，运行中环境变化不触发自动更新；操作因实际平台能力不足而失败时走 failure，不切换模式重试。

物理可用量使用操作系统定义的口径，包含平台认定可回收的容量，不只读取空闲页数量；它不表示本请求独占的额度。CPU 能力来自允许 CPU 集、配额和运行时线程能力，不使用某一时刻的空闲线程数。Log/Telemetry/TelemetryMemoryTrace 的阶段 RSS 采样只用于独立诊断，不承担 ResourceProbe 职责，也不作为可用内存的替代值。

Wasm 不把 `/tmp` 认定为释放堆压力的文件后端。本次已读配置未建立 OPFS/磁盘后端绑定。本轮 `externalSpillAvailable` 在 Windows/Linux 文件后端实现中为真，在 Wasm 和其他平台中为假；该值描述已提供的能力，不通过捕获一次文件创建失败来改写。无外存平台执行 7.4 节固定 MemoryStore 分支；必要增长不足进入 failure，不能继续向内存文件系统写入无界数据。

有 pthread 的 Wasm 复用同一线程结构，按所选模式从许可中扣除后台 driver、Adaptive 控制线程和必要宿主线程余量。没有 pthread 的平台使用同一块循环的顺序执行配置，C=S=1，不启动监控线程；Adaptive 执行其无监控能力的保守状态，Fixed 保持运行前设置。阻塞式浏览器 I/O 在主线程不受支持时返回能力错误。该能力分支不恢复外部 runner 或旧预算方案。

线程模式在创建执行状态前按平台能力确定。无 pthread 的顺序配置由 driver 持有唯一 WorkerContext 和 compressor owner，同一 RunTerminalWork 在当前线程取得一个计算额度并执行终端函数，RunOrderedBlocks 顺序调用相同块接口；根对象销毁前按相同顺序销毁上下文。该配置从入口确定，未新增失败后改用 inline 的路径。已选择线程配置后，driver/control/worker 创建失败按 4.4 节关闭根对象并 join 已创建线程；不通过减少 P 或借用宿主线程池继续运行。资源采样接口以有效性字段表达暂时未知，控制器执行已有保守状态；未声明的异常与内部状态不变量破坏进入 failure，不归类为信号缺失。

WasmBrowserFileByteRangeReader 当前用 Module.dataCodecBrowserPrefetchRanges 按 fileId 保存 JS Uint8Array，maximumPrefetchBytes 是另一套独立的留存限制。本次删除该 map、预取函数、参数与相关统计分支；浏览器 reader 的 PrefetchRange 固定声明 Unavailable，完整预读只由受控 EncodedInputCacheLoader 决定。调用方不能把可选预取能力缺失视为读取失败，也不能在已经发生实际浏览器读错误后继续主路径。

浏览器 ReadAt 对大 output span 顺序执行最多 1 MiB 的子请求，每次把当前 ArrayBuffer 复制到对应目标偏移；取消在子请求之间检查。目标可能是预先取得完整容量的 C++ owner，其容量持续计入 M；JS 范围缓冲由浏览器管理，完成后清除可达引用，GC 释放时机保持未知。Wasm 文件注册表的原始 File 对象、模型注册表中的宿主输出、JS 引擎内部及 WASMFS 页均不伪装成根预算可回收字节。多个已登记文件不能各自偷偷保留一份预取缓存。

## 16. 取消、错误和审计

### 16.1 清理顺序

本节定义当前请求的错误收束。请求停止与根对象关闭的范围统一见 4.4 节；同一套 failure 入口通过请求上下文记录首错，不能把每个 Playback 请求的结束都实现为会话线程池销毁。

```text
记录第一个错误或取消原因
  -> 设置当前请求 runStopped，通知所有相关等待点
  -> driver 调用 CancelAndWaitRun，撤销排队任务并等待运行中的终端函数结束
  -> driver 丢弃未提交记录，不再等待缺失的前序结果
  -> 中止尚未完成的 adapter/package 操作
  -> 释放本请求未交付的 workspace 数据、临时 store 和引用
  -> 结束本请求的绑定与回调，只发布一次请求结果

根对象关闭时另由拥有者执行 ShutdownAndJoin，销毁共用执行设施
```

FailureScope 的 workspace 清理必须排在任务收束之后。函数局部 RAII 的声明和析构顺序体现该要求；不能在 worker 仍读取 workspace 时触发现有 CleanupOnFailure。

沿用 `Runtime/Failure` 的具体入口：阶段错误分别调用 FailEncodeStage、FailDecodeStage；pipeline 驱动错误分别调用 FailEncodePipeline、FailDecodePipeline；它们经 RecordFailureAndStop 记录首个错误并请求停止。EncodeFailureGuard、DecodeFailureGuard 继续负责最终清理。底层存储和 codec 以现有 bool/status/error 传递结果，不为访问 failure 上下文增加跨层依赖。

worker 捕获 codec 异常并发布失败 completion，同时停止所属请求、唤醒 driver；driver 观察到任一失败立即进入上述 failure 入口，不等待失败块排到队首。执行设施故障或不变量破坏同时按 4.4 节标记 closing。控制线程的持续压力超时结束当前请求，保留真实资源目标和压力状态。guard 在任务收束对象之前构造，逆序析构时先完成取消、等待和记录释放，再运行 workspace cleanup；显式 CleanupIfFailed 调用也只能发生在收束之后。failure 路径与正常路径都只发布一次请求完成事件。

根状态、线程创建等发生在 context/workspace 建立前的错误，由原地改造的 MakeEncodeEntryFailure、MakeDecodeEntryFailure 返回 16.5 节固定大小的失败记录，不临时构造另一套 pipeline 或日志路由。错误码继续使用现有 CodecErrorCode：非法参数归 InvalidInput，格式与平台能力分别归 UnsupportedFormat、UnsupportedPlatform；执行设施错误归 PipelineFailure，编解码阶段操作错误分别归 EncodeFailure、DecodeFailure。受控容量、实际分配、I/O 等细分原因写入固定记录，并附可确定的阶段、申请量、预约量和上限；丰富文本按 16.4、16.5 节在关键收束之外生成。

所有槽位、重型阶段和计算 lease 都具有唯一归还路径。任务发布失败、ReadAt/WriteAt 失败、异常、取消、控制器持续压力失败均执行上述请求收束，再按 4.4 节判定是否需要关闭根对象。外部 sink 已写入的内容可能不完整，失败不能回滚未声明事务能力的 sink；只有成功 Finalize 的结果可作为有效输出。

分配前许可未获准时，严格执行 7.4 节的单一用途规则及 7.6 节的准入边界。选定操作的实际分配、I/O、codec 和提交失败均终止当前请求。错误信息分别标识受控容量不足、必要并存容量不足、平台能力不足、压力等待超时、实际分配失败和 I/O 失败；未知占用不补估计数。事务回滚只销毁未发布的新资源并保留旧资源供清理，不表示恢复业务执行。

受控容量不足只由清单内容量预约产生。槽位内缓冲、宿主、外部 sink 和第三方内部的实际分配失败按各自错误通道报告；统一收束不增加这些对象的审计或分配管控。

### 16.2 准入、算法选择与操作失败的边界

本次保留的条件分支如下，所有条件都必须通过专用状态表达。禁止用一个含义混合的 false、空指针和 catch-all 同时表示候选不适用与实际操作失败。

| 类别 | 条件与唯一处理 | 是否执行失败后的回退 |
| --- | --- | --- |
| 正常准入等待 | 槽位、计算额度、压力 gate 暂未开放；按既有事件推进、排空和等待 | 否，任务尚未取得执行条件 |
| 必要内存准入等待 | 分配尚未开始，存在 7.6 节规定的独立释放步骤；释放后重新检查，全部结束仍不足即 failure | 否，不重放已经执行的任务 |
| 固定用途的存储创建 | 已知范围存储按 7.4 节许可结果选择后端；未知长度追加按平台能力选定后端 | 否，数组分配和文件创建前只决定一次 |
| 可选优化未启动 | 可选留存关闭、压力状态不允许、确定容量未获准；预读和 key cache 不启动 | 否，已启动的优化失败同样进入 failure |
| 平台能力配置 | 无 pthread 时从入口采用顺序执行；Adaptive 无可靠采样时执行保守控制状态；无外存时采用受控内存用途规则 | 否，能力缺失不通过试运行失败来识别，也不增加资源操控模式 |
| reference 算法拒绝候选 | PrepareBlock 明确返回 Rejected，原因为 InsufficientModelFit、PrecisionBudgetUnavailable；Auto 使用普通编码，Forced 进入 failure | 是，保留算法规定的候选拒绝，精度预算与内存额度无关 |
| reference 大小选择 | Exact 比较成功候选的存储字节；BoundedProbe 仅在已指定该策略、IntraArray 且块超过 4096 元素时使用固定样本，其他情况按已有 Exact 规则 | 属于正常编码选择；采样和候选执行错误均进入 failure |
| package Zstd 收益选择 | 保留现有按字段类型、8 MiB 阈值、1 MiB 样本及至少 1/20 节省量决定是否压缩的规则 | 属于正常编码选择；读取、probe 压缩和正式压缩失败均进入 failure |
| Morton 内排与外排 | 开始排序前按确定元素数和固定 leafBudgetElements 选择现有路径；重复 key 的分桶与分段按既有排序算法执行 | 属于算法数据分解；排序、provider 和 run store 操作失败均进入 failure |

reference 的现有源码已给出明确分界：`ReferenceTransferCacheBuilder.h:659` 对 EncodePreparedBlock 的 Failed 传播错误，对准备完成后再次 Rejected 报告“unexpectedly rejected”；`:679` 的 PrepareBlock Rejected 才进入 Auto 的普通编码选择；Forced 不接受该拒绝。目标实现保留这些语义，不能将异常、分配失败、损坏的 reference、准备状态丢失包装为 Rejected。Exact 模式中任一候选执行失败就终止当前请求；另一个候选已有结果不改变此规则。

`Storage/LeafPackage/LeafPackageFieldEncode.h:118` 的 ShouldCompressLeafPackageField 使用函数返回值表达操作成功，以独立的 shouldCompress 表达收益结论。目标保持这一区分：只有成功探测得到“不压缩”才写原数据；probe 读取失败、Zstd 失败、Finish 失败均不得清除错误后写原数据。

以下恢复代码明确不进入目标实现：

- 内存实际分配失败后改文件、缩小申请量、回收 cache 后重放
- 追加到一半因容量拒绝迁移后端；文件创建、写入、Seal 失败后改内存
- 线程创建、任务提交、Zstd 线程参数设置失败后降线程数、改 inline 执行
- reference 数据损坏、布局不符、已准备候选再次拒绝后改普通编码；provider 读取失败后改恒等重排
- 可选预读、key cache 已开始执行后因错误撤销优化并继续当前请求
- worker 依赖环、缺失前序结果、槽位计数错误、重复完成等实现不变量破坏后强行继续

重试不能用于弥补上述不变量失效。部分分配回滚、取消唤醒、join、Abort 和临时文件清理仍是必须执行的收束步骤。清理中出现的次生错误沿用诊断通道保留首错；它们不能把失败结果改为成功。操作系统接口本身规定的短读短写推进属于一次完整 I/O 的实现，必须检查进度、EOF 和错误，不扩展为任意 I/O 错误后的无限重试。

### 16.3 审计实现

审计挂在具体 owner 的实际分配、容量变化、移动和析构处，关闭时不改变任何控制分支。MemoryStore 和 EncodedBuffer 的确定数组分配可以记录完整容量事件；scratch 可变 vector 未覆盖的短暂峰值留空。

MemoryStore 的实际数组采用 ResidentByteBudget 内部的 move-only AllocatedArray 承载，和容量 lease 各自记录实际存储与预约。既有共享 state 内的 allocationMutex 仅串行化自有完整数组的 new/delete 与对应确定容量事件，旧新数组之间的数据复制保持锁外；不持容量准入锁调用 allocator。控制器和 TryReserve 从不读取 allocated 字段，实际审计不参与准入、缓存留存或目标变更。该事件区只维护 scopeId、live/peak/count，不登记业务 owner 引用；零数组和失败分配不产生实际分配事件，根销毁后原数组凭借同一 state 继续完成析构登记。

TelemetryResourceUsage 的 trackedCapacityBytes、eventPeakCapacityBytes、sampledPeakCapacityBytes 分别使用 optional 表达覆盖情况。根数组的 eventPeak 为该 capacity scope 的生命周期事件峰值；空闲 scratch 仅报告取得快照时的真实留存容量，活跃可变数组未覆盖的峰值留空。记录带 scopeId、单调取样时刻与 coverage，报告同名重复快照只更新观测值，不跨叶、模块或根相加，也不把这些值填入进程 working set 字段。

每个物理分配 owner 具有唯一审计身份，shared_ptr 引用、span、LRU entry 不重复登记。lease 的预约量与实际分配量分别记录，分配失败只撤销预约，不产生假 allocated/free 事件。

通过 observer 等按值接口把原生 vector 所有权交给宿主时，记录所有权转出并结束该对象的自有追踪，不虚报已经释放物理存储。EncodedBuffer 保持自有 owner 时继续按其真实析构记录，不能混用这两种交付边界。

文件逻辑量、mmap 映射量、宿主结果逻辑量、库内部未知量和进程 RSS 单列。已追踪容量的采样最大值与完整事件峰值分别命名。未知项不通过 headroom、输入倍数或各模块峰值求和补齐。

首版审计接入清单如下，控制方式继续由第 6—12 节决定，不为扩大审计覆盖新增容量门禁：

| 已覆盖的具体存储 | 报告口径与限制 |
| --- | --- |
| ScratchByteBuffer 及其承载的 raw、component、converted、reference staging、候选 payload | 底层字节 vector 的实际 capacity；覆盖全部容量变化才报告事件峰值，其他接入点仅报告采样容量 |
| DataCodec 自有 Wavelet/residual 临时数组、连接和重排数组 | 清单内固定数组按实际分配元素数 × sizeof(T)，清单内 vector 按 capacity × sizeof(T)；无法观察的内部临时量留空 |
| MemoryStore、EncodedBuffer、完整解码及必要 reference 的底层 owner | 每笔实际数组容量，扩容同时记录存活的旧数组和新数组；缓存和 workspace 引用不重复登记 |
| blockLayouts 外层向量、序列化参数缓冲 | 按 6.2 节记录确定 capacity，未覆盖的变长成员不推算 |
| 7.8 节新增的有序 RegionRun、reference 采样索引/样本/候选边、Morton high-buffer slab、多面体 visitedFaces/localIndexTable | 根容量 owner 的实际分配容量及旧新并存，具体数量来源见接入清单；不能因对象只活过一个阶段省略 |
| 多面体 counts/offsets/ids、Morton 固定表与内排数组、拓扑 grammar 事件/流数组 | 覆盖的 vector 逐个按实际 capacity×sizeof(T)；可变 vector 的短暂重分配峰值未覆盖时留空 |
| 7.8.6 节明确列出的宿主适配自有 offsets、去重键 vector 和 polynomial orders | 仅取样其确定 capacity；map 节点、宿主原生容器容量未知，不从元素数推算总量。这些对象不接入 M |
| 借用视图、宿主 payload、第三方内部分配及清单外小对象 | 自有容量审计豁免；宿主逻辑输出、库内部未知量按独立口径展示 |

报告名称使用“已追踪容量”，同时给出覆盖范围和峰值口径，不命名为 DataCodec 总物理内存。allocator 元数据、堆内保留页及未覆盖分配留空；各模块独立峰值不能相加得到同时峰值。所有权移动保持唯一记录身份，存储真实释放才记容量归还。

删除旧资源职责对应的失真统计代码：ScratchByteBufferPool 按请求 bytes 记录的 active 值不能代替 capacity，零长度取得后再扩大 vector 也必须按真实变化处理；ResidentSizeHint、encoded-input 的 size、reference 逻辑元素字节与 TopologyWorkBudget 估计不进入自有容量合计。已删除 quota 的申请和峰值字段一并删除，不为旧报表重建账本。预约量、逻辑输出量和进程 RSS 可以保留明确命名的诊断意义，与实际分配容量分别展示。

精度分析需要 remap 顺序时，RunRemapOrderRecord 从发布处传入既有不可变 provider 的共享 owner，RemapOrderCapture 与 AdapterSignatureOrderSet 只保存同一 owner；禁止从裸指针另造 owning shared_ptr。分析循环使用最多 1 MiB 的索引读取窗口，读取失败明确进入分析失败结果，不能用空 order 代替。此处延长的 provider 寿命是明确请求的分析输入寿命，仍使用原有容量 lease，不归入默认可选性能缓存。

日志留存也必须有固定边界。原地在 TelemetrySessionSink 中集中规定首版常量：最多保存 256 个 session、合计 4096 条明细记录，每条保留文本合计最多 2048 个 UTF-8 字节，截断须在字符边界结束。增加记录前判断条目空间；session 满时先移出最早已完成的 session，全部仍活跃时跳过新增 session 的详细留存；明细达到上限时不再追加明细。分别记录 omittedSessions、omittedRecords 和 truncatedText 计数，报告明确呈现不完整覆盖。Take 后释放其留存，反复 Snapshot 不在内部保存快照副本。

消息参数列表最多保留 32 项，避免大量空参数绕过文本字节边界；省略参数同样设置 textTruncated。共用文本复制与内置消息列表条数常量放在 RunRecordTypes，TelemetrySessionSink 引用同一值。报告节点中的 capture 计数为所属 sink 的累计快照，counterScope 明确标识该口径，不将不同节点的同一累计计数相加。

这些条目规则属于可选观察的正常留存边界，不能丢弃 16.5 节固定首错和请求完成结果，也不能把诊断缺项解释成操作成功。Playback 的 64 条后台消息继续保留并使用相同文本长度规则；内置 entry/frame 消息收集不再额外积存无界副本。用户自行实现的外部日志 sink 继续作为业务回调，其自身存储豁免。关键资源诊断仍使用 16.4 节的独立固定环，日志取样与格式化不参与调度。

### 16.4 共用资源诊断与等待定位

诊断直接放在 DataCodecExecutionResources 的现有 Impl 中，不新增诊断服务、全局 owner 注册表或线程。它观察已经存在的控制状态，控制器与容量预约不读取诊断结果。Fixed 与 Adaptive 都保留关键记录；关闭审计和普通日志不关闭这些记录。

根对象内嵌一个 `std::array<ResourceEvent, 128>` 事件环。ResourceEvent 仅含枚举、整数、时间和定长数据，`static_assert(sizeof(ResourceEvent) <= 256)` 将事件存储限制在 32 KiB 以内；写满后覆盖最旧事件并记录覆盖次数，不扩容、不等待消费者。首错单独保存在 16.5 节的请求记录中，事件覆盖不能覆盖首错。该固定诊断存储明确列为分配管控和容量审计豁免对象。

ResourceDebugSnapshot 是调用者提供存储的固定大小结构，包含当前状态、事件环副本和至多 16 个申请点 owner 描述。整体大小通过 `static_assert` 限制在 64 KiB 以内，不包含 vector、string、shared_ptr 或拥有型业务数据引用。快照字段统一如下：

| 内容 | 明确字段与口径 |
| --- | --- |
| 身份与时序 | requestId、flowId、阶段标识、单调采样时刻、执行事件序号、目标变更序号；块序号只在所属 flowId 内比较 |
| 目标与真实状态 | M/U、C/R、S/N、Mmax/P、gate、runStopped、closing、HeavyPhaseLease 活跃状态、排他计算状态与取得的额度数 |
| 块流进度 | 最后准入序号、最大已完成序号、连续已提交序号、连续已归还序号、队首块状态和就绪任务数；最大完成序号不代表此前块均已完成 |
| 当前等待 | driver 当前操作、等待原因、等待开始时间、已有推进者的 request/flow/block 标识、预期唤醒事件；无对应工作时明确标记不适用 |
| 自动策略 | NORMAL/DRAIN/HOLD、待确认压力、样本值及来源/有效年龄、HOLD 起点、冷却期限、当前观察批和 Wc/Ws/Wi/Wo；Fixed 中自动策略字段标记不适用 |
| 拒绝与首错上下文 | 实际失败申请的 A/U/M、用途、申请点、owner 描述、截断标记、原始错误及次生清理错误计数；未知值保持未提供 |

TryCopyResourceDebugSnapshot 按执行锁、容量锁、数组审计锁的固定顺序执行 try_lock。任一锁未取得时立即返回 false，表示这次没有快照；调用者不能读取未成功复制的 output。成功时在同一同步边界复制当前控制状态及已覆盖的实际数组状态，锁外消费结果，不做格式化、I/O、宿主回调或堆分配。实际数组分配和析构只取得数组审计锁，释放该锁后才归还容量 lease，不形成反向嵌套。该诊断调用不产生控制通知，不作废观察批；debugger 也可直接检查根对象中的固定存储。

执行路径仅在既有状态变更位置记录事件：请求/块流开始和结束、目标及门禁变化、等待原因进入和结束、块准入/计算完成/提交/归还、可选回收请求与处理完成、首错/取消/关闭。目标事件保存前后 M/C/S 和第 14 节的固定原因；块事件保存本流序号与当时 N/R；首错事件保存原因及首错标识，完整文本留在独立失败记录中。相同状态的定时采样不反复写事件，不记录每次 scratch 分配，不为写事件增加容量锁或扫描全部 owner。

等待原因使用固定内部枚举：槽位不足、终端计算未完成、顺序提交等待、已准入 owner 消费释放、压力排空、压力恢复、输入 I/O、输出 I/O 和请求收束。每个等待位置写明能够使其结束的实际事件。例：队首尚未完成时记录队首块身份及其计算完成事件；输入 ReadAt 尚未返回时记录输入 I/O，预期推进者为当前 reader 返回。不得把实际 I/O 阻塞统一标成“内存等待”，也不建立用于自动推断依赖的通用等待图。

MemoryStore 在创建时从所属容量 state 取得唯一 ownerId，保存用途和静态申请点标识；这些小型标签不参与容量判断。共享引用沿用原 ownerId，audit 关闭时标识仍有效。必要申请被拒绝时，由调用点复制其已知、必须跨过该申请点保留的 owner 描述，最多 16 个；超出数量设置 ownerListTruncated，不推算其余容量。仅保存 ID、确定容量与采集时刻，不保留额外强引用。快照不在后台遍历 workspace 或并发读取可变 store，历史 owner 描述不冒充当前完整占用清单。

容量拒绝的 A/U/M 取自该次同步检查，在释放容量锁后作为固定数据传到 driver/根状态。后续实时快照中的 U 可以已经变化，两者分别标记时刻。lease 析构继续只访问独立容量 state，不反向调用事件环；driver 的既有消费完成通知足以记录推进过程。owner 描述不求和替代 U，也不进入审计合计。

确定容量与追加增长共用同一锁内预约入口，增长传入“完整新数组的最小所需容量”和“优先扩容量”，按当次剩余额度确定获准长度；最小需求不满足时直接记录其 A/U/M，不能在锁外快照处提前拒绝。owner 标签使用固定数组存放用途、文件名、行号和 UTF-8 标签，许可移交 MemoryStore 时保留 ID。根区分后端选择/可选缓存拒绝与终止性拒绝：前者只记录观察，后者在构造动态错误文本前写入固定首错及 A/U/M。当前请求首个终止性拒绝保留至交付，下一请求清理该上下文。

等待上下文按现有 admission 索引和 generation 定位，快照在执行锁内读取其完成状态；不保留 TerminalWork 或业务 owner 的引用。ReaderReturned、WriterReturned、TerminalCompleted、OwnerRetired、AdmissionAvailable、PressureSample 和 RunDrained 明确预期推进事件，取消仍按原停止通道唤醒。观察批的四类等待时长在快照副本中补上当前时间片，读取诊断不改变控制器累计值；Fixed 用 applicability 字段明确这些自动观察值不适用。

正常运行的诊断导出由 driver 在阶段边界通过已有 RunRecord/Log 报告链完成，JSON 格式化留在 Log/Report 层，禁止持执行锁调用 sink。诊断导出属于可选观察：记录缺失、sink 抛异常及格式化分配失败只设置 diagnosticsIncomplete 和相应计数，不改变资源目标、首错或业务结果，不重新编码。必要数据的 reader、writer、adapter 和 codec 继续遵守第 16.2 节的失败契约。分配失败收束期间不执行丰富诊断导出，调用方仍能取得固定失败记录。

每次请求开始记录 requestId，结束记录结果与 N/R/非块阶段是否已收束；U 可以因合法存活结果继续为正。Playback 的事件环跨请求保留最近记录，通过 requestId 区分，首错只在前一请求完成交付后初始化。运行停止、长时间 I/O 和锁竞争均可通过只读诊断定位，不新增生产超时监控或自动重放。

内部不变量检查落实到准入、归还、提交和请求结束位置：计数不得下溢，凭证不得重复归还，成功提交序号必须连续，新请求必须等待旧请求真实收束。违反不变量时记录当时的固定上下文并按 4.4 节关闭根对象。目标下调后 U>M、N>S、R>C 的合法暂时超额不触发错误；调试断言不能用静态上限比较取代真实生命周期规则。

### 16.5 低内存下的关键错误路径

现有 FailureScope::Fail/Run 与 RecordFailureAndStop 已捕获异常并请求停止，EncodeContext/DecodeContext 的首错记录包含动态字符串，FirstFailure 返回值会复制记录。MakeEncodeEntryFailure 还会创建消息、输出路由和日志元数据。上述代码需要原地修改，不能仅以接口标注 noexcept 说明内存耗尽时能够保存错误。

在现有 Common/DataCodecError.h 中定义共用 CodecFailureRecord，继续使用 CodecErrorCode，不新增公共错误码体系。目标记录如下，数值缺失通过 optional 表达，文本采用 UTF-8：

```cpp
struct CodecFailureRecord {
    CodecErrorCode code{CodecErrorCode::PipelineFailure};
    bool cancelled{false};
    std::array<char, 32> reason{};
    std::array<char, 64> origin{};
    std::array<char, 256> message{};
    bool textTruncated{false};
    std::optional<std::uint64_t> requestedBytes;
    std::optional<std::uint64_t> reservedBytes;
    std::optional<std::uint64_t> limitBytes;
};
```

记录使用 `static_assert(sizeof(CodecFailureRecord) <= 512)` 约束大小，复制和移动均不申请堆内存。reason 使用固定原因标识，区分受控容量不足、必要并存容量不足、实际分配失败、I/O 失败、压力超时、取消及执行设施错误；CodecErrorCode 的阶段分类保持第 16.1 节规则。origin/message 写入预留数组，保留结尾零字节，按完整 UTF-8 字符边界截断并设置 textTruncated；数值格式化使用定长缓冲和无分配转换函数。

根状态内的 `optional<CodecFailureRecord>` 是当前请求唯一可写首错。EncodeContext/DecodeContext 的 RecordFailure、HasFailure、FirstFailure 通过 RunBinding 访问它；删除独立的字符串首错存储与同步副本，FirstFailure 返回固定记录的值拷贝。进入失败入口时先在执行锁内保存首错、标记请求停止，锁外通知所有等待点。已持执行锁的调用使用同一内部已持锁实现；底层 store 必须先释放容量锁再上报，不增加反向锁序。次生错误仅增加固定计数和有限描述，不能覆盖首错。

首错记录与停止标志必须先于任何日志调用。std::bad_alloc、线程创建异常、codec 错误以及普通 bool/status 失败都进入同一收束流程；异常捕获点从静态原因和现有数据构造固定记录，不先构造 std::string。低层算法的正常 bool/status/error 接口保留，向上传播期间发生的分配异常由最近的执行或入口边界转换成固定失败记录。

关键路径保证在“后续所有 DataCodec 堆分配均失败”的故障注入下仍能完成首错保存、停止发布、取消唤醒、任务等待、资源归还和固定结果交付。具体约定如下：

- stop source、必要的停止回调和任务完成状态在任务接纳前建立；创建失败发生在该任务准入之前。停止回调只设置状态和通知，不提交新任务、不执行宿主业务回调。
- 撤销队列逐个把待执行任务移到已有栈上容器，锁外销毁，不先分配 vector 收集取消任务。清理与 trim 同样逐个移出引用，不为清理构造新的大集合。
- 槽位、计算和容量 lease 的析构与归还不抛异常，不拼接日志，不创建线程，不把缓冲重新插入需要分配节点的复用容器。失败请求的临时缓冲直接释放。
- adapter Abort、文件关闭/删除及宿主完成回调在锁外执行，捕获可传播的次生异常并继续其余清理。临时文件清理需要的路径与平台参数在创建时准备。DataCodec 自有的清理步骤不申请新堆内存，宿主和系统操作的错误不改变首错；不在错误路径调用 Shrink 重分配或重建对象。
- 既有请求完成状态先保存固定结果并唤醒等待者，driver 按原协议至多发布一次业务完成通知；不为通知另建线程任务或临时消息队列。运行中 I/O 的返回时间仍遵守第 7.6 节边界。

EncodeResult 和 DecodePackageResult 增加内联 `optional<CodecFailureRecord> failure`；内部 pipeline 结果及 Playback 的既有完成结果同步携带该记录，解绑和销毁根对象前完成值拷贝。success/cancelled 按已有业务字段表达，messages 保留普通丰富报告，失败结果不依赖 messages 非空。EncodedBuffer 的空对象和结果的默认构造不分配数据存储，结果交付使用移动。

根对象创建前的入口错误直接在栈上形成同一记录。MakeEncodeEntryFailure、MakeDecodeEntryFailure 接受既有错误码和字符串视图，返回 success=false 与固定 failure；删除这些关键失败路径中创建 RunRecordDispatcher、路由 shared_ptr、时间字符串和消息 vector 的前置要求。入口不调用依赖尚未建立的 context、线程池或控制器。结构化结果始终提供错误事实，丰富消息交给关键路径之外的既有报告层。

固定记录与有界诊断属于同一个 failure/执行体系，不新增应急 allocator、紧急内存池、备用线程池或错误后的业务重试。诊断文本截断、可选导出缺失和次生清理错误具有明确标记，均不把失败结果改为成功。实际系统终止进程及宿主违反析构协议不属于可捕获分配失败的处理保证。

## 17. 修改单元和完成证据

### 17.1 实施单元与顺序

按下表完成同一个最终架构。每个单元结束时其涉及的旧调用链和测试一起迁移，目标代码不保留旧新探测或兼容入口。

| 单元 | 修改位置与交付要求 |
| --- | --- |
| 公共与内部入口 | Encode/Decode entry、Playback、DecodeSession、Frame/Leaf 内部请求；公共头文件无执行资源和外部 cache 注入，内部运行可复用 |
| 自有执行 | DataCodecExecutionResources、ParallelExecution；有限入队、C/S/N/R、控制线程、driver 约束、取消/Join 测试 |
| 确定容量 owner | ByteBudget、MemoryStore、ByteStoreSession、SegmentedBinaryObject、EncodedBuffer；单次容量选择、固定后端、扩容双份并存、失败清理、共享引用寿命测试 |
| 数值块与字段前置数据 | 普通、区域残差和 reference transfer builder；同槽位覆盖 current/reference/candidates，按序写块；字段级 run 索引、分组样本/真实 edges 和窗口重采样按 7.8、9.3、9.4 节落地 |
| 拓扑和全局数据 | topology encode/decode、Morton/provider、polyhedron；有界 artifacts，high-buffer slab、visitedFaces/localIndexTable、五条完整索引 store 及固定批次均有明确 owner，无同池嵌套等待 |
| 解码提交与缓存 | AttributeDecode、DecodedCacheCommit、observer、typed caches、DecodeTaskCoordinator；引用固定和消费完成确认 |
| 自动控制与平台 | ResourceController、ResourceProbe、WasmRuntime；第 14 节全部状态规则及 17.4 节确定性轨迹和信号失效分支 |
| 调用方迁移与旧模块删除 | Filter/Execution、Filter/Adapter、Filter/Playback、Filter/Wasm、仓库 `iGameCore/IO/IGDC`、Qt 和 Examples 中实际资源引用 |
| 第三方上下文 | NumericArrayCodec、ZstdCodec；明确线程参数、worker 所有的 compressor、正常退出与 Windows Clang 验证 |
| 诊断与低内存收束 | 现有执行 Impl、Common/DataCodecError、FailureScope、Context、entry 结果和 Log/Report；16.4、16.5 节的固定记录、无分配收束及故障注入一起完成 |

实施先完成固定粒度、共享 owner、有限执行和 UpdateLimits；固定失败记录、关键状态快照及基础故障注入与这些共用机制同时接入，再分别接通 Fixed 的启动目标与 Adaptive 的初值、运行期决策。验证分为共用机制、Fixed 模式、Adaptive 模式三个范围；两个生产模式同时保留。它们共享一个执行实现，不以构造第二套 manager/runner 表达模式差异，不以完成全面审计作为实施前置条件。

必须删除的整类职责包括 AttributeEncodeScheduler、ActiveByteBudget、WindowBudget、局部 quota/lane、TopologyWorkBudget 估计记录、CodecPerformancePresetParams 资源档位、外部 cache 接口及宿主线程池适配。业务配置和数据协议按前述文件落点迁移，不能直接删除其仍有效的类型。

当前三个未跟踪实验头文件不接入此实现：Runtime/Execution/DataCodecResourceTaskRunner.h、Runtime/Execution/MortonRemapResourceReservation.h、Test/Feature/DataCodecFeatureResourceTaskRunner.h。既有核查只发现它们之间的引用，未发现生产调用或 suite/runner 注册；其 task memory claim、reservation provider、legacy scratch 路径不用于本方案。本次不改动这些文件。

验证使用现有 CMake/C++20 构建目标和 `Test/Runner/DataCodecTestMain.cpp`、`Test/Suite/DataCodecTestSuite.h` 注册，不以测试头文件存在替代实际执行。生产模块与平台边界继续通过现有 CheckDataCodecDependencies 规则。

### 17.2 旧职责删除、调用方迁移与测试修改

下表与第 3—16 节共同构成必做清单。含有数据协议或业务配置的文件先迁出保留内容，再删除资源设施；删除模块名不能代替清理实际调用链。

| 位置或符号 | 唯一处理及保留边界 |
| --- | --- |
| Runtime/Execution/DataCodecExecutionResources、ResolveDataCodecExecutionResources | 删除外部 IParallelTaskRunner 指针及空指针选择静态 InlineParallelTaskRunner 的生产分支；原地改为第 4—6 节的自有状态 |
| API/Entry 的 Encode/Decode 请求、Frame/Leaf 内部请求、Playback 打开请求 | 删除 executionResources、runner、外部 cache/runtime 字段和解析分支；公共入口按所选模式创建根状态，内部调用只传播已有状态 |
| Filter/Execution/iGameDataCodecThreadPoolTaskRunner.h、DataCodecTaskRunner、MakeDataCodecExecutionResources | 调用方迁移后删除该宿主线程池适配文件和工厂；宿主 iGameThreadPool 的非 DataCodec 用途保留 |
| API/Adapter/IDecodedFrameCache.h、IEncodedInputCache.h | 删除管理接口；frame payload、lease 和查询协议迁入 DecodedFrameTypes，EncodedInputBuffer 的数据持有职责随 loader 的内置输入类型保留，不导出缓存设施替换入口 |
| Filter/Adapter/iGameStreamingFrameCacheAdapter.h 及对应 cpp | 删除把宿主 StreamingData cache 注入 DataCodec 的资源适配器和构造链；宿主呈现结果保留自身所有权 |
| Runtime/Cache/DecodeCacheRuntime、DefaultDecodeCacheRuntime、PlaybackSession 中 default/external 标记 | 删除进程级静态缺省实例、外部替换与两套缓存构造分支；由单个根状态持有内部 runtime |
| Codec/Attributes/AttributeEncodeScheduler.h | 删除 AttributeByteQuota、scratch/staging quota、AttributeLaneGate 和 Pressio/reference lane；AttributeStagingHandle/Manager 的存储所有权与必要计时迁入现有 store/workspace，随后删除调度器文件 |
| Storage/ByteIO/ByteBudget.h 的 ActiveByteBudget、Window/WindowBudget.h 及其 lease | 删除估计或逻辑活跃字节等待体系与别名调用；ResidentByteBudget 按第 7 节保留改造 |
| Storage/ByteIO/Window/WindowRuntimeParams.h、WindowedCopy.h、SegmentedBinaryObject、Storage/LeafPackage/LeafPackageFieldEncode.h、LeafPackageFieldDecodeStream.h | 删除 active-window 参数、window lease 及复制函数临时创建的独立 budget；保留 1 MiB 范围读写、字段流式压缩和一次性消费，实际 store 后端按 7.4 节固定 |
| ScratchByteQuotaAcquire、CacheResources 的 window/remap 预算、Codec/Reference/Common/ReferenceTypes.h 的 NumericArrayReferenceCodecEncodeInput 和 remap options 中的 quota/scratchBudget | 删除注入回调、可空预算旁路、每个集合独立造额度与重置行为；reference 预测、误差约束、重排 provider 和 scratch 复用按原职责保留 |
| Workflow/Session/EncodeSessionWorkspace、DecodeSession、EncodeLeafWorkspace、DecodeLeafWorkspace | 删除独立 reference store 额度、外部 reference cache 配置、每叶额度复制及 InheritCacheResourceConfigFrom；按 RunBinding 和显式 owner 维护依赖与寿命 |
| DecodedAttributeCacheSet、DecodedGeometryCache、DecodedTopologyCache 及各 typed LRU | 删除独立容量上限、重复字节决策账本、ResidentSizeHint 预算；保留 key、LRU、必要引用保护和数据访问，容量在 owner 处登记 |
| Codec/Attributes/AttributeDecode、ParallelExecution、ParallelDecodeTopologyBlockObserver | 删除独立 cost/aging 调度、局部 inFlight/worker 配置、ParallelForChunksAllowNested 同池等待和 observer 的独立 pending 队列；执行第 5、6、11 节唯一块循环与同步提交 |
| API/Params/CodecPerformanceParams.h、CodecPerformancePresetParams.h | 删除资源 tier、逐模块份额、lane 与成组存储政策；业务 options、精度、增强、语言、日志、validation、observer 移入 DataCodecControlParams 和 CodecParamDefaults，公共资源配置归 CodecResourceParams |
| Codec/Topology/Common/TopologyWorkBudget.h、topology.work_budget.requested | 删除虚拟工作集估计和对应 memoryCheckpoint；既有核查显示该估计仅用于记录，未通过它取得预算，不将此项描述为实际门禁迁移 |
| Filter/Adapter/iGameDataCodecDataObjectBridge、Filter/Playback/iGameFrameSequenceDecodeBridge | 删除 bridge 的 executionResources 与 runner/cache 注入、资源适配器构造链；对象组装、属性访问、播放呈现和持续会话业务保留 |
| Filter/Wasm/iGameWasmDataCodecBridge、MakeiGameWasmDataCodecTaskRunner、SubmitiGameWasmDataCodecTask | 删除向宿主 ThreadPool 提交及资源注入的旁路；自有后台 driver 承接任务，浏览器 I/O 与线程能力按第 15 节处理 |
| 仓库 iGameCore/IO/IGDC/iGameIGDCWriter.cpp、iGameIGDCReader.h 及对应 cpp | 删除 DataCodecTaskRunner、MakeDataCodecExecutionResources、SetDecodedFrameCache、SetEncodedInputCache 与注入链；只经自有入口和启动资源配置调用 |
| Codec/SubCodec/ZstdCodec、NumericArrayCodec 的 ThreadLocalCompressorCache | 删除独立硬件线程推荐、无限 TLS 配置缓存和进程寿命裸指针；按第 13 节显式线程参数及 worker owner 改造 |
| API/Params、Validation、Log/Report、Filter/UI、Qt、Examples 的资源消费者 | 删除旧 tier/quota/lane/外部资源字段及其校验、展示和示例；删除已撤销额度的申请与峰值诊断字段，保留的预约、逻辑量及 RSS 按 16.3 节分别命名，编码格式和精度预算保留 |
| Runtime/Context 的 EncodeFailureState、DecodeFailureState 及 failureMutex，Runtime/Failure/FailureScope、PipelineFailureManagement | 删除独立字符串首错，迁移对 formattedMessage 的读取；清理判断与错误查询改接根状态的固定首错和停止状态。保留已有阶段、pipeline 入口和 guard，PublishByteStoreCleanupDiagnostics 改为收束之外的可选导出 |
| Common/DataCodecError、API/Entry 结果及 MakeEncodeEntryFailure/MakeDecodeEntryFailure、Playback 完成结果 | 接入同一 CodecFailureRecord；删除关键失败返回必须创建日志路由和消息 vector 的路径，结果直接携带固定失败数据，不新增错误后的执行方案 |
| NumericArrayRegionPlan、NumericArrayReferenceBytes、AttributeReferenceScheduleBuilder | 删除每块全量 run 复制/排序、跨完整 reference 区间的巨大重采样 staging、所有分组样本同时保留；按第 9 节原地实现字段级索引、固定读取窗口与根容量样本/边数组 |
| MortonRemapBuilder、PolyhedronTopologyEncode/Emit/StreamEncoder | 删除 highBuffers 的 65536 个可扩容字节 vector、未受控全点表/全面位图和按可调字节量确定 cell 批次的路径；替换为第 10 节确定数组与固定窗口，不恢复阶段工作集估计 |
| AttrDecodeStage 的 AttributeDecodePayloadMode/OneShotZstd、DecodeLeafWorkspace 的 prepared payload | 删除 payload 存储档位和 one-shot 全量 vector 解压；按 11.4 节保留一份确定长度范围 owner，本次属性请求完成后放下引用 |
| MemoryByteRangeReader 的内部复制调用、LeafPackageIO::ReadFromMemory、FramePackageIO 的 vector writer | 删除内部分配整输入/整字段却不携带容量 owner 的入口；公共既有输入 owner 继续作为数据输入，内部完整副本必须经根容量 |
| PackageDecodeWorkflow、enableFullInputPrefetch 及参数/报表调用点 | 删除解码前整文件 OS 预取与独立开关；原生只读映射按输入寿命持有，已准入的当前范围提示与完整自有 loader 预读按 12.2 节分别处理 |
| Platform/Wasm/WasmBrowserFileByteRangeReader | 删除 JS 预取 map、独立大小参数与持久字节副本；保留分段 ReadAt、文件注册和明确的能力结果 |
| RemapOrderCapture、AdapterSignatureOrderSet、TelemetrySessionSink 及内置消息收集 | 删除全量 remap vector 快照与无限明细留存；数据分析保留已有 provider owner，诊断按 16.3 节固定条目数与截断标记，修改对应结果消费者 |

算法互斥、非线程安全库的串行要求和文件顺序属于正确性约束，保留为明确同步与依赖；它们不继续以模块可配置 lane 表达。替换资源门禁时，同时接通原门禁覆盖的数据生命周期。通用执行与平台模块不依赖宿主/Wasm 类型，生产模块不依赖测试。

测试与构建迁移固定包括：

- Test/Feature/DataCodecFeatureBudget.h、Test/Assertions/WindowBudgetAssertions.h：删除旧 window/quota 断言及专属注册，ByteStore 清理、scratch 复用及 I/O 正确性用例改接新状态
- Test/Feature/DataCodecFeatureTopologyObserver.h、task coordinator 和 cache 用例：改用自有状态，保留消费、取消、引用和结果正确性覆盖
- Filter/Test/Feature/iGameDataCodecFeaturePlaybackSession.h、iGameDataCodecFeatureStreamingFrameCache.h：删除外部缓存替换和宿主资源适配用例，保留对应播放业务覆盖
- 仓库 iGameCore/CMakeLists.txt 的递归 GLOB 与现有 suite/runner：核对新实现被实际调用、用例被实际注册；头文件被收集不等于运行链已接通
- 仓库根 CMakeLists.txt 与 Examples/Wasm 构建配置：源码快照中的 pthread 预建池 16、DataCodec 最大 worker 6 作为现状核查项，按第 15 节核对计算、driver、Adaptive 控制、第三方和宿主用途的总需求；编译期平台能力与运行期 C 分开
- 仓库 iGameCore/Cmake/CheckDataCodecDependencies.cmake：现有分层检查通过，不增加平行构建目标或宿主依赖旁路

删除验收逐项检索生产声明与调用：外部资源注入、宿主线程池调用、default/external cache 分支、逐叶新额度、属性 quota/lane、window 活跃预算、可空预算旁路及同池嵌套等待的声明、实现和调用全部删除。三个既有未跟踪实验文件继续按 17.1 节保留于工作区，不接入生产或测试注册；其符号出现不作为新架构已实现的证据。

### 17.3 共用机制与业务路径的完成证据

- Fixed 和 Adaptive 均通过同一 UpdateLimits 初始化，公共 mode 显式选择；填写数值和缺省数值都不会隐式切换模式
- Fixed 在不同环境采样轨迹下保持 M/C/S 不变，不创建自适应控制线程、不进行预热增长和压力 HOLD；实际分配及 I/O 失败直接进入共用 failure 链
- Adaptive 在显式天花板内按采样升降目标，信号缺失只进入其保守状态，不改写为 Fixed
- 共用 UpdateLimits 的动态测试无需创建 ResourceController，验证机制本身支持运行期升降；Fixed 的生产调用只在运行前设置目标
- 给定完全相同的具体 M/C/S、gate 和 owner 状态，两种模式的底层准入判断与提交结果一致；模式差异限定为目标生成、更新时机及既定可选留存动作

- 同一运行范围的多个 workspace 指向同一执行状态和容量 state，公开对象无法替换这些设施
- S=1/C=1、S>C、额度下调、慢首块、慢 sink、取消和 worker 异常均能完成或清理
- 排队任务、输入和待提交结果受同一 N 约束，计算结束不提前减 N
- 共享 owner 只计一次；移出 cache 后仍被引用的存储可读，最后引用释放才减少实际容量
- 新旧数组扩容并存真实入账，Resize 缩短不触发未预约的大分配
- 一次性入口返回 EncodedBuffer 后容量 state 仍有效，自有线程已销毁；Playback 单请求结束保留会话线程，Reset 后结果 owner 仍可继续存活
- reference 依赖不在持槽位的 worker 中递归解码；固定块格式及误差约束通过 round-trip
- 对原有串行 stage 选择、TLS 更改、Wasm 无外存路径分别有回归测试
- 自动与固定并发性能比较包含大字段、众多小字段、重排、reference、外部压力及最终封装时间
- 审计开关不影响调度决定，不存在用估计审计数作为容量许可的路径
- 禁用平台采样的内部测试中，运行中依次升降 M/C/S，验证等待唤醒、U/M 与 N/S、R/C 暂时超额、先批准后下调的 lease 及取消；动态基础能力独立通过
- M=0、M 从零上调、M 下调到 U 以下、Mmax 边界及非法更新均有明确结果；无计数清零、旧 owner 强制释放和失败任务重放
- 改变 M 后所有模块继续共享同一实际剩余额度，源码不存在模块比例额度及切分 M 的映射表，scratch 不再保留 M/8 子份额
- 同一文件在不同 M/C/S 下解码，并在运行中更新额度，数据结果及格式边界保持一致；编码请求和 metadata 不携带解码资源政策
- 固定 point/cell 粒度、1 MiB I/O、Morton 分段及 probe 粒度不随设备和压力修改；块尾、变长拓扑、外层 Zstd 依赖及旧大块分别验证
- 同一请求的普通/严重压力与信号失效均保持 M，硬能力下降即时限制 M；M 增长与 C/S 增长互斥，审计开关不影响轨迹
- C=S=1 时，字段/叶/帧累计自有内存和完整内存结果仍受 M 约束；扩容检查完整新数组及旧数组并存，不能用槽位归还抵消长期容量
- 块内 raw、component、reference staging 和候选缓冲不产生逐笔长期容量预约；宿主、借用与第三方内部没有预算拒绝或预算等待入口
- 完整内存输出暂时被独立消费占用容量时可延后，消费期间 driver 继续提交，释放后按真实容量成功；超出 M 或必要并存量超限时无需等到 HOLD 超时
- PendingAdmission 不占槽位、计算额度、重型阶段凭证或未使用的容量；排空和取消能结束待准入状态
- reference 等待当前计算才可释放、扩容旧数组等待当前复制才可释放、队首提交受阻等情形均不会循环等待；没有可行路径时及时失败
- 库调用、vector/new、文件创建/写入及宿主输出分配失败均不自动重放；共享引用存活与审计关闭不改变上述失败条件
- 完整结果小于 M 且旧新数组并存超过 M、最终大小只能在编码后确定、已有大块只能单槽位解码等边界分别覆盖
- 使用故障注入核对数组分配、文件创建/Read/Write/Seal、线程创建、任务提交、Zstd 参数设置及 codec Failed：只报告一次失败，不调用替代后端、不改 inline、不重放任务，全部已创建资源完成收束
- Auto 的合法 Rejected、Forced 的拒绝、准备后再次 Rejected、Exact 的任一候选 Failed 分别覆盖；只有表中规定的算法拒绝能够继续普通编码
- 可选预读/key cache 的“准入未获准”与“已启动后失败”分开覆盖；前者按分配前选定的读取与 key 获取路径正常执行，后者进入 failure
- 创建前 Memory/File 分支、无 pthread/无外存能力配置、Adaptive 信号未知状态分别从既定条件直接进入，不以人为制造一次操作失败触发
- 对未知长度小结果固定文件暂存、串行 adapter 提交、完整 remap 数组并存及固定 Morton 执行规模分别测量时间与大对象容量，记录该单一实现的真实成本
- 根执行对象的目标修改仅由 UpdateLimits 发布，其他模块没有可单独改 M 的 SetLimit/Reset；并发预约和目标更新不存在重复加锁或反向锁序
- package Zstd 出队前 R 不因该任务增加，出队时一次取得全部 w；C=1、C=2 和运行期下调时均能开始、完成或正确失败，不出现 driver 预占额度后 worker 无法出队
- 字段外层解压与收益 probe 计入计算额度；同一块的准备与 ComputeBlock 顺序执行并共用一个槽位，无同池子任务等待
- gate 关闭时，未首次准入的最终输出阶段保持待准入；已准入阶段的写出和增长继续，N=0 且 HeavyPhaseLease 活跃时不提前宣布 DRAIN 完成
- 最后一个块的 End/Seal/Finalize 未完成时仍持槽位；压力排空不能提前完成，收尾失败只完成一次错误清理；空字段在单个非块阶段内收尾
- 压力暂停期间 scratch 归还不留存，恢复严格使用 12.1 节统一条件；单纯打开 gate、上调 M 和未知信号不提前恢复，Fixed 不引入控制器状态
- Playback 当前请求取消或 codec 失败后，独立后续命令复用同一根状态，旧通知不影响新 stop source，U 不清零；执行设施故障关闭根对象并完成待运行命令，禁止自动重建和重放
- 前台请求窗口已满只拒绝新请求，不替换已接受请求、不取消当前请求；使用现有失败码和明确原因，不新增 Busy 枚举
- CancelAndWaitRun 返回只表示终端任务已收束，driver 随后清理块记录和 workspace，再发布完成通知；方法不等待自身返回后才发生的动作，回调重入阻塞操作直接报告错误
- 无 pthread 配置复用相同准入与块接口，driver 仅在终端函数执行期间持有唯一计算额度；不创建 worker 或控制线程
- 容量不足、std::bad_alloc、线程创建失败和 codec 异常均留下同一格式的固定首错；根对象创建失败也能返回该记录，messages 为空时错误仍可识别
- 在目标分配点失败后持续拒绝后续 DataCodec 堆分配，首错保存、停止唤醒、取消队列、任务等待、owner 释放和结果移动均能结束；同时注入日志与次生清理错误，首错不变且没有重复完成
- 通过正常结果存活场景验证请求结束 N/R 为零且无活跃非块阶段，U 按仍存活的结果保持真实；通过异常场景验证不遗漏或重复归还凭证
- 128 条事件环覆盖时内存不增长，覆盖计数和请求身份可读，首错保持；快照在锁竞争时立即报告未取得，诊断调用不修改控制状态
- 慢首块、慢 reader、慢 writer、压力 HOLD 和真实容量拒绝分别输出明确等待原因与推进者，能够从快照区分这些状态；没有新增不动点超时或自动重放
- 容量拒绝记录当次 A/U/M；owner 描述超过 16 项时明确截断，共享 ownerId 一致，记录不增加强引用、不汇总替代 U；实时 U 与历史拒绝 U 的时刻分别标记
- 固定失败记录的 UTF-8 截断、空文本和超长文本通过检查；snapshot、事件、结果记录满足大小约束，复制与移动不分配堆内存
- 相同采样与流水线事件下，审计开关、丰富日志导出及诊断导出失败都不改变资源决策和业务结果；数据 sink 的真实失败仍结束请求

#### 17.3.1 逐模块内存接入的验收

7.8 节每个根容量对象都必须能定位到“准入、实际分配、唯一 owner、最后释放”四个源码落点，并提供容量不足和实际分配失败的注册用例。槽位对象证明其读入、计算、顺序消费和异常清理都在同一凭证寿命内；仅审计与豁免对象不能出现 ResidentByteBudget 拒绝分支。测试观察真实受控 owner/槽位状态，不读取审计合计来驱动执行。

| 场景 | 必须观察到的结果 |
| --- | --- |
| 同一块范围，分别使用 float32/float64、多分量、SOA/getter/remap 输入 | raw、component、orderBlock 与实际自有复制容量分别符合形状；宿主直接视图无自有分配事件，关闭审计仍得到相同准入行为 |
| regionRuns 总数很大，每块仅相交少量 runs | 全字段有序数组只有一份受控容量；块临时量随局部相交数增长，不能随全字段 runCount 乘上槽位数；编码误差要求与原算法一致 |
| 大量同布局属性、Forced reference、多个采样组及大 sampleCount | 索引和样本一次仅保留一个组，索引容量与去重后长度分别记录；edges 按实际新增记录预约，old+new 增长可见；容量不足正确失败，无静默关 reference/忽略边 |
| reference/目标元素数相差很大及 predictor 边界 clamp | 结果与原坐标映射/插值数学定义一致；中间读取不申请首末映射点之间整个巨大数组，只有固定窗口、相邻 tuple 与目标块 |
| Morton 桶高度集中、全部桶分散、奇数 run 窗口尾部 | slab 申请量等于实际 q_i×r 之和，清理后对应 U 归还；内排阈值、重复 key 外排和最终 order/inverse 正确；没有高桶 vector 隐藏扩容 |
| polyhedron 全面/全点规模大、单 cell 巨大、五条流尾位与跨窗口 varint | visitedFaces/localIndexTable 受控且不按 worker 复制；五条 spool 和五条解码索引 owner 分别可追踪；批次边界不改变连续编码字节与 bitpack 尾位 |
| Attribute 外层 None/Zstd、按需属性及后续补充请求 | 直接 source 无副本；Zstd payload 按 field.rawSize 预约，decoded 字段按 E×D×V 预约；请求结束解除 prepared payload，后续请求按原 reader 正常准备 |
| 多叶帧封装、完整内存输出和无外存追加 | 所有累计字节使用同一根状态；被 bundle/segment 引用的数据持续占容量，提交后依实际引用释放；完整结果离开请求仍保留自己的 lease |
| 同一 reference 被 workspace、LRU、结果共同持有，cache 驱逐或 session 清理 | 只删除当前引用，无强制清空；最后 owner 销毁归还一次容量，ByteStoreSession 弱登记随阶段清理，不积累全历史记录 |
| 浏览器多文件、完整预读、长会话和取消 | 没有按 fileId 保留的独立 JS 预取字节；范围请求不超过 1 MiB，前一请求结束后才开始下一段；C++ 目标与平台临时量按不同口径报告 |
| 宿主多面体表示构造、prepared surface 与原生结果 | 明确标记宿主大对象豁免；可得 vector capacity 仅取样，不伪装全部宿主容量已覆盖；ReleaseConvertedInputs/失败 Abort 后不残留无用派生引用 |
| remap 分析、超长日志与反复快照 | 分析不复制全域 order；读取错误明确报告；session/明细/文本达到固定边界出现省略标记，首错和结果交付完整，内部不保留快照副本 |

### 17.4 Adaptive 控制器确定性测试

使用可注入的单调时钟、平台采样和流水线事件测试同一个状态更新逻辑。测试注入属于内部验证接口，不形成对外运行时调节入口。首先验证以下轨迹，不要求真实制造内存耗尽：

| 场景 | 必须满足的结果 |
| --- | --- |
| 高水位启动，观察批尚未完成 | 保持当前目标，无连续盲目增长 |
| 一个快块已经计算完成，另一个块尚未提交 | 观察批未完成，不增长 |
| 同类型计算持续等待额度 | 按规定步长增长，始终满足目标范围 |
| 预读不足且有槽位等待 | 只增加供给槽位，计算额度不被槽位反向提高 |
| 慢输出、首块延迟和乱序完成 | 有界积压；提交拥堵期间不增长；前序工作可继续 |
| 低水位出现一个短脉冲 | 首次低水位立即关门，解除确认后原目标恢复，无普通减半 |
| 低水位附近反复波动 | 待确认状态最多保持 2 s，随后确认压力并排空 |
| 连续低水位和长时间计算 | 一个事件只普通收缩一次，实际在途数不被改写，排空计算可继续 |
| 普通压力升级为严重压力 | 立即关闭准入并降至单计算目标，已有调用不被强制终止 |
| 同一压力信号持续到排空之后 | `HOLD` 超时不被重复通知重置，期满明确失败 |
| 最后一批块在压力期间完成，请求已完成最终提交 | 正常结束，不增加高水位确认或压力等待耗时 |
| 内存在 `L` 与 `H` 之间波动 | 正常运行不增长；已确认压力后的等待不提前解除 |
| 恢复后很快再次出现压力 | 再次收缩，增长冷却按 10、20、40、60 s 退避，上限固定 |
| 采样过期、恢复、再次过期 | 作废旧窗口，单次失效只排空一次，未知信号不清除已知压力 |
| 在关门、排空和等待时取消 | 全部相关等待者被唤醒，槽位和存储按生命周期清理 |
| 轻量工作类型切换到重型工作类型 | 原在途块先交接，新类型从小规模开始，保留压力冷却 |
| 槽位全部释放，长期存储仍占用容量 | 长期控制状态保持真实，不将块级排空解释为操作内存已清空 |
| 新块准入与目标下调同时发生 | 按同步先后次序确定归属，无漏计和重复归还 |
| 分配失败或前序块失败 | 无自动重放，提交等待解除，后续结果得到清理 |
| M=1 GiB，必要 reference 为 600 MiB，压力恢复后新增必要数组 64 MiB | 普通及严重压力保持 M，恢复后确定并存容量 664 MiB 可获准；不因压力减半许可而失败 |
| 同一场景的真实平台容量硬上限降为 512 MiB | UpdateLimits 即时限制 M，保留已有 owner，后续必要申请明确失败并完成清理 |
| 当前剩余额度下降，平台容量硬上限未变化 | 触发相应压力门禁及 C/S 调整，不将剩余额度当作新的 M 硬上限 |
| Playback 前一请求完成，新的独立命令面对较低或较高可用量 | 在新请求边界重算 M，原结果及 reference 的 U 保持；字段切换和内部递归不触发重算 |
| 连续采样重复产生相同目标 | 不重复写目标变更事件，不作废已有效推进的观察批 |

额度范围、增长步长、阈值顺序和冷却上限应做参数遍历。事件模型验证不代表真实 allocator、第三方库或操作系统压力行为已经验证。

本表与 17.3 节动态 M 测试共同覆盖 M/C/S 联合轨迹：普通和严重压力不减少当前请求的 M，真实硬能力下降即时约束 M，M 增长与 C/S 增长互斥。Playback 下一条独立命令按新鲜环境重算 M，必要 owner 及 U 继续有效；字段和 reference 递归不触发重算。Fixed 单独验证整个请求不进入上述压力和增长状态。

### 17.5 性能、设备压力与参数验收

性能基线使用同一基础块格式、算法、长期容量及输出方式，比较 Fixed 的小、中、高并发配置与 Adaptive。计时包含预热、类型切换、排空和恢复，不只截取升到高并发后的稳态片段。

- 小请求：检查 500 ms 观察规则没有成为人为完成等待，统计自动控制额外开销。
- 长请求且无外部压力：检查到达有效并发的时间、端到端耗时，以及高核设备是否长期受预热或等待比例门槛限制。
- 内存带宽受限的数据：计算繁忙不能证明继续增线程有益，比较自动模式与较低固定并发的吞吐和进程占用。
- 输出受限的数据：检查没有随可用内存高而持续堆积槽位，记录结果队列的数量上界。
- 不同工作类型频繁交替：测量重新预热成本，核查类型划分是否过细；不通过新增逐模块内存预测来抵消成本。
- 外部持续压力和阶跃下降：记录从首个有效压力信号到关闭门禁的延迟、排空耗时、实际容量变化与失败次数。
- 重复压力：记录恢复与再次收缩间隔、目标轨迹及冷却是否生效，检查稳定负载下是否反复启停。

正确性与状态轨迹必须全部通过。首版性能校准目标为 Adaptive 的端到端耗时相对同一资源条件下最佳可行 Fixed 配置增加不超过 10%；测量前确定目标设备、样本、重复次数和允许偏差。该比例是初始验收目标，尚未得到实测验证。

请求完成率单列验收，不以更早失败缩短的时间计入吞吐收益。确定性验收样本离线已知各步真实容量与并存关系，全部必要受控分配可在启动 M 内完成；在压力于 30 s HOLD 期限内满足恢复条件、平台硬上限未下降且没有取消和实际分配/I/O/codec 错误的轨迹中，必须完成请求，压力过程不降低 M。这些已知容量只用于测试，不向生产控制器加入全任务估算。真实设备按成功、取消、持续压力超时、容量不足、实际分配失败、I/O/codec 错误分类报告次数和已完成工作量。成功样本比较耗时，失败样本保留固定首错及可得的有界资源轨迹，并标明事件覆盖和诊断导出缺失。

关键诊断的固定存储计入根对象初始化成本，事件写入与快照导出分别测量开销；正常运行不以全量 owner 遍历生成快照。采用相同控制输入分别关闭和开启丰富报告，确认报告层不改变准入轨迹；记录诊断导出对运行时间的影响，不把日志关闭当作正确性的前提。

资源回收验收同时观察清单内确定容量和独立进程指标：前者验证所有者按要求释放，后者识别 allocator 保留页、线程栈和第三方工作区。不能要求调用 `free` 后进程物理占用立即等量下降。测试不通过时先区分单块与长期必要工作集、控制器决策和存储释放问题，再校准对应规则。

样本覆盖代表性浮点字段、多分量数据、变长拓扑、reference、重排及不同输出方式；文件中浮点字段的高占比仅用于选择测试重点，不转化为运行期申请量系数。设备覆盖小内存、高核数、宿主已有大输入、其他进程制造压力及无可靠信号平台。测量吞吐、系统压力、进程实际占用、计算空闲、提交积压、收缩耗时和恢复振荡，并核对单块读取没有隐含全字段连续化。

对固定 65536 tuple、1 MiB I/O、Morton 执行规模及 probe 粒度测量格式、精度、压缩率和元数据成本；同一文件在不同 M/C/S 及运行中额度变化下完成正确解码。水位、采样周期、增长步长、额外窗口、压力退避和 M 增长默认值集中校准，修改参数经代码评审并重新验证，不通过失败后的备用策略弥补验收不通过。

资源控制与审计增强分别验收，完整审计不作为控制实现前置条件。完成标准是本文承诺的有界性、清理正确性、资源目标生效和运行效果，不承诺精确整进程物理内存硬上限。

## 18. 已确定的实现参数与验收边界

本次确定以下实现选择。数值属于可审阅的设计决定，未作为实测最优值；本表汇总固定选择，第 14 节完整定义自动调节公式与时间参数，两者共同作为实现和测试输入，不在运行失败后切换其他值。

| 项目 | 本文的具体处理 |
| --- | --- |
| 固定基础块规格 | 新数值/connectivity 编码取最多 65536 元素，尾块取剩余元素数，删除公共 setter；普通/reference 的相同域使用同一边界。polyhedron 的 65536 cells 仅作为执行批次，保持原有五流格式。当前源码的 262144 仅作为旧文件样本，不视为此前已认可的目标值。常量集中到现有 SpatialBlockLayout.h，具有块布局的格式 metadata 写入实际值 |
| 块规格选择依据 | 65536 个 float32 标量原始字节为 256 KiB，float64 为 512 KiB；相对于当前 262144，把同类型同分量的一份完整基础块降为四分之一。分量、副本和拓扑连接量另有实际需求，这个数值不等于单块总内存上限。压缩率、元数据增长和吞吐列入实测验收 |
| 单块不可再分的极端数据 | 本轮不新增任意“最多几个分量/几个连接”的性能阈值。保留当前格式与 codec 类型边界：维度为合法正值，计数和偏移可表示，元素数 × 分量数 × 类型大小、拓扑变长计数和连续分配长度均经 checked arithmetic 与平台硬能力检查。违反条件直接 failure；合法单块的真实分配失败直接结束请求，不动态缩块重编码 |
| 既有文件支持 | 解码继续读取已有 metadata 中的块边界，不重新切割已经压缩的单元；旧大块每次只推进一个记录，仍可能无法适配小设备。资源不足错误与格式不支持错误分开，具体兼容样本列入测试 |
| 性能取舍 | 只保留重型 stage 内块级并行，driver 串行提交；删除独立重型 stage 并行和额外提交队列的实现。小字段密集数据与原实现的性能比较列入验收；不保留根据失败和性能探测恢复旧架构的开关 |
| 公开结果类型变化 | 内存编码结果由可变 vector 改为只读 EncodedBuffer，底层固定 MemoryStore；全部仓库调用方同步按 span 消费，不保留逃离 owner 的隐式转换 |
| Morton 执行规模 | leafBudgetBytes=8 MiB，runBufferBytes=1 MiB；只作为单阶段执行常量，不按 M 猜测整个阶段用量，具体算法分解见 10.2 节 |
| Morton 高桶暂存 | 使用按真实 highCounts 和固定 1024 字节刷出门槛推导的一个 slab，容量公式见 10.2 节；完整分配进入根 M，不为 65536 个桶各留可扩容 vector |
| polyhedron 工作与长期数据 | 五个各 1 MiB 流窗口，visitedFaces/localIndexTable 及五条完整索引存储按 7.8、10.3 节；cell 批次与格式流状态分开 |
| 字段前置工作数组 | region run 索引、同组 reference 采样索引/样本、实际候选边进入根 M；块局部计划与重采样读取按第 9 节限制 |
| 基础粒度完整清单 | point/cell tuple、分量、reference、变长拓扑、I/O、Morton、probe、终端任务与全局 owner 的边界以 6.0 节为准；固定规格与运行期额度分开 |
| 模块容量 | 不划分比例份额，清单内长期 owner 共享一个当前 M；空闲 scratch 只按数量及固定单对象阈值保留，超限释放 |
| 两种操控模式 | Fixed 在运行前固定 M/C/S；Adaptive 根据环境持续更新 M/C/S；默认 Adaptive，模式在运行前明确选定 |
| 一个共用机制 | 内部 UpdateLimits 支持 M/C/S 同时更新，两种模式共用同一执行与容量状态；分别验证机制能力和模式的调用规则 |
| 同一请求的容量许可 | 普通/严重压力保持 M，通过门禁、C/S 及可选回收缓解压力；条件满足时可提高 M，真实硬能力下降立即限制 M，独立新请求按环境重算 |
| 关键诊断 | 根对象内 128 条固定事件、单条最多 256 字节，快照最多 64 KiB、至多 16 项已知 owner 描述；锁竞争立即返回，诊断不参与控制 |
| 低内存失败结果 | 共用 CodecFailureRecord 最多 512 字节，内联到既有结果对象；首错、停止、清理与固定结果交付不依赖新的堆分配，丰富消息独立导出 |
| 普通日志留存 | TelemetrySessionSink 最多 256 个 session、合计 4096 条明细、每条保留文本合计 2048 个 UTF-8 字节；省略和截断显式报告，16.4 节关键诊断另按固定环处理 |
| 大对象豁免 | 宿主输入/输出/拓扑表示构造、第三方内部、标准库隐藏临时量与未接入元数据按 7.8 节分别列明；不能声称 M 是 DataCodec 全量占用上限 |
| 自动控制完整规则 | 输入与有效环境见 14.5 节，水位与启动见 14.6 节，观察批及 C/S 增长见 14.7 节，压力/恢复见 14.8 节，类型切换见 14.9 节，信号失效见 14.10 节，M 与 C/S 联合更新见 14.4 节 |
| 参数与效果验收 | 共用机制和业务要求见 17.3 节，确定性状态轨迹见 17.4 节，设备与性能验收见 17.5 节；公式校核、事件模型和真实运行结果分别报告 |
| 平台与存储 | Windows/Linux 提供 FileBackedStreamStore 外存路径；Wasm 及其他平台本轮不提供外存能力。未知长度追加按平台在创建前固定后端，连续返回结果固定内存，具体用途规则见 7.4 节 |

固定块参数作为格式/算法常量进入一个定义位置，不随设备、线程数、压力或 M 改变。水位、增长和 M 默认集中定义，代码评审与实测校准通过修改该定义完成；运行中的操作失败不触发常量切换。参数验收未通过时修订设计并重新验证，不保留另一套运行时补救实现。

### 18.1 校核记录与验证限制

历史校核记录（2026-09-08，本次文档合并未重跑）：遍历 P=1..512 下的 1,181,868 个合法 (P,C,S) 组合，检查普通收缩、计算增长和单独槽位增长保持目标范围；检查 64 MiB 至 4 TiB 代表性有效容量的水位顺序及启动增长时间下界。一次性压力事件模型覆盖首次低水位关门、同事件只减半一次、严重升级保持等待期限、短脉冲恢复、波动确认和排空后冷却。

上述记录仅校核当时的 C/S 公式与部分事件轨迹，未实现完整控制器，未验证线程竞争、实际内存峰值和吞吐，也不构成 UpdateLimits、两种正式模式及自动 M 调节已经通过验证的证据。新增能力按第 17 节完成独立测试与真实设备验收。

本轮在相同源码快照上枚举生产目录的 275 个 h/cpp 文件，包含 17.1 节两个未接入生产的 Runtime 实验头；排除这两个文件后按 273 个候选生产文件筛查容器、原始分配、扩容、ByteStore、自定义数组、隐式复制和线程入口，再沿资源持有及消费链逐模块核对。重点补读数值/区域/reference、Morton、connectivity/polyhedron、整字段输入准备、叶/帧封装、会话与缓存，以及 Filter/Validation/Log/Wasm 的分配边界。当前不存在的历史邻接图路径不计入目标模块。

据此新增 7.8 节逐对象清单，具体补齐第 9—11 节算法接入、第 15 节浏览器范围读取、第 16 节审计/诊断留存及 17.3.1 节验收。目录归属与函数局部变量均未被用作单独的豁免依据；宿主派生拓扑和未覆盖元数据的现实边界已明确列出。本轮仅修改本文，未修改生产或测试代码，未编译，未运行 DataCodec。数组改造、格式等价性、真实峰值、吞吐、并发清理与压力完成率均需在实施后按第 17 节验证。
