# T14 Windows 实现覆盖核对与实测报告

日期：2026-09-10。核对基线：datacodec/dev，f871bb959。本轮新增本文、更新 Memory_Thread_TODO.md，补齐独立多帧性能测试及必要编译依赖，并修复实测暴露的无拓扑点集帧解码失败。其他既有文档不变。

## 1. 目的、判定方式和结论

本轮针对 TODO 核对三件事：对应实现是否存在并接入调用链，测试案例是否覆盖要求，实际执行结果是否支持该项结论。源码核对止于实现入口、调用接入和测试断言，不开展实现质量、架构或代码风格审查。

测试选择沿用既有有效证据，并重新运行当前 Windows 核心回归及对象断言采集。两种采集入口覆盖范围不同：CTest All 包含原生 remap、Playback 和 prepared surface 集成；`--resource-coverage` 额外执行完整帧缓存和 reference 缓存，并输出带名称的 Require 断言。二者共同用于本轮覆盖判定。

- 首批增量构建 iGameDataCodecTests、iGameDataCodecFileBenchmark 成功；补齐多帧测试并修复点集问题后，iGameDataCodecTests 和 iGameDataCodecSequenceBenchmark 构建成功。本轮没有强制全量重编译。
- 最终核心 CTest：9/9，通过，16.53 秒。修复前首批为 9/9、15.16 秒，分开记录。
- 最终对象断言采集：913 次检查、637 个唯一名称、失败名称 0、遗漏 0，进程退出码 0。修复前首批为 910 次、635 个名称。
- 新增六帧序列实验：四配置各一次预热及三次正式运行全部成功，每轮六帧逐值一致。Fixed 1/2/4、Adaptive 4 的编码加解码计时中位数依次为 569.9813/485.8357/449.1720/432.9283 ms，方法和原始数据见 6.4。
- TODO 所列当前 Windows 实现入口及既有功能测试注册均存在；多帧性能要求只有功能/生命周期测试、缺少独立计时和完成率数据，本轮补充序列实测。新样本暴露点集帧缺少拓扑载荷时误绑定 reference 的实现缺口，已修复并补充正反向回归。
- 61 行对象清单中，60 行具备所列 Windows 测试证据，1 行浏览器对象范围外。名称计数不是代码覆盖率，也不代表穷举全部输入或故障交错。
- 极小 points 的 Adaptive 额外 1.2836 ms、13.16% 仍未达到原 10% 目标；用户已停止围绕这一比例门槛的优化和重复测速。本轮完成指 TODO 覆盖核对与必要验证完成，不表示该比例达标。
- T16 既有文档整理暂停，不作为本轮完成门槛。

## 2. 环境、版本和证据复用依据

| 项目 | 记录 |
| --- | --- |
| 系统 | Windows 11 专业工作站版，10.0.26200 |
| CPU | AMD Ryzen 7 7840H，8 核、16 逻辑处理器 |
| Windows 可见物理内存 | 33,522,085,888 B，约 31.22 GiB |
| 构建 | CMake、Ninja、UCRT64 Clang、LLD、C++20 |
| 配置 | RelWithDebInfo，`-O2 -g -DNDEBUG`，链接参数 `-fuse-ld=lld` |
| 构建目录 | 仓库下 cmake-build-relwithdebinfo-dev |
| 配置开关 | IGAME_DATACODEC_BUILD_TESTS=ON，ENABLE_CGNS_MODULE=ON |
| 环境加载 | 编译和运行前均加载已核实的 CLion 环境脚本 clion_env.ps1 |
| 首批测试 EXE SHA256 | BE617F4EBAFC59D6D7EC87C8DD52F418FEB754D2FA61D98A4B3EA4087E3A2850；这是修复前首批 9/9 和 910 次采集的文件 |
| 最终测试 EXE SHA256 | 46AA6997EB068F0992ECD044C3CE76B01B73B1823A7BDEB93100C6FCD2DDBE07 |
| 最终序列测速 EXE SHA256 | 3455904E284A77432DE98DF9A49C7D6311866536634A1D8F0B081B99209722B4 |

本轮核对了根 CMakeLists.txt、iGameCore/CMakeLists.txt 和已有 CMakeCache，按其配置执行。各 EXE 哈希分别绑定首批与最终运行文件，不用于推断其他历史运行的二进制身份。

核对基线的最新生产实现提交为 ec67c1bf6，后续至核对基线的 DataCodec 跟踪差异仅涉及文档、新增文件测速入口及其 CMake 目标。本轮随后修改无拓扑点集的 TopoDecodeStage。四类单帧性能、Windows Job 和 Qt 的既有证据按下面的具体范围复用；不把沿用结果写成本轮重测。

相关提交：392c84812 校准启动规模；a65a839b2 控制唤醒及压力响应；3224ccc5d 代表性性能与缓存；98dd4a59d 控制轨迹与 worker 启动失败；f83c9968b 窗口与分层容量；104acc3de 业务 owner 与原生边界；efb2b87af 对象证据补齐；ec67c1bf6 删除旧执行接口；6d4969200 大型 CGNS 测速入口；6b89bbec2 修正测速 reader 所有权；f871bb959 记录停止极小请求优化的决定。

## 3. TODO 与实现、测试的对应关系

下表的源码位置相对于 DataCodec 目录。实现列说明已经存在的接入，不评价实现质量。用例名省略共同前缀 RunDataCodecFeature。

| TODO | 已确认的实现或交付入口 | 实际测试接入与判定范围 |
| --- | --- | --- |
| T00 | 第三方前置操作已有提交和历史记录 | 前置工作沿用，不执行 fetch、reset 或修改子模块 |
| T01 | 跟踪中的实现方案、TODO、验证台账 | 文档和跟踪状态可见；本轮新增本文，不更新旧设计 |
| T02 | API/Entry 编解码入口的固定失败记录与捕获出口 | Failure；固定记录、UTF-8、持续分配拒绝、阶段清理、可选报告失败 |
| T03 | Storage/ByteIO/ByteBudget.h 的 TryReserve/Growth，ByteStore.h 的数组预约事务和 owner，EncodedBuffer 交付 | StorageOwnership；准确容量、旧新并存、拒绝回滚、预约后降额、共享与最后释放、输出交付 |
| T04 | Runtime/Execution 的根、UpdateLimits、终端执行和 RunOrderedBlocks；编码入口直接创建根，PlaybackSession 创建内部共享根 | ExecutionMechanism、TaskCoordinator；M/C/S 动态额度、块凭证、有界窗口、顺序提交、取消、启动分配失败和 join |
| T05 | ResolveResourceConfiguration、CodecResourceParams、叶/帧资源传递及 Playback 内部根 | PipelineContracts 的公共字段编译契约、模式配置和根共享；ResourceModes 本轮沿用既有结果 |
| T06 | GeometryEncode、AttributeEncode 接入 NumericArrayTransferCacheBuilder；普通数值和 ReferenceTransferCacheBuilder 使用 RunOrderedBlocks | NumericExecution、ReferenceCodecs、RegionPrecision、PipelineContracts；固定/尾块、分量、getter、区域、分组和 reference |
| T07 | ConnectivityTopologyBlockEncode 和 PolyhedronTopologyEncode 接入块流；MortonRemapBuilder 通过根执行及 CreateSizedStore 创建工作区 | TopologyExecution、MortonResources、CellGraphTopology；All 内原生 Remap 补充 polyhedron 数据适配 |
| T08 | GeometryDecode、AttributeDecode、TopologyDecode、PolyhedronTopologyEmit 使用块流和同步消费 | NumericDecodeExecution、TopologyObserver、TopologyExecution、原生 Remap；旧大块、输出引用和前驱退休 |
| T09 | ByteRange、LeafPackageFieldEncode/DecodeStream、MemoryStore、封装 writer 和完整输出 owner | ByteRange、StorageOwnership、PackageIdentity、OutputSinks、窗口/payload 用例；范围寿命、两后端、容量拒绝和最后释放 |
| T10 | DecodeCacheRuntime 内部构造三类缓存；PlaybackSession 调用 RequireFrame；DecodeTaskCoordinator 使用内部根 | 三类缓存用例、PipelineContracts 会话用例、TaskCoordinator、All 的 Playback；必要引用、可选留存、LRU、命令窗口和取消 |
| T11 | DataCodecExecutionResources 调用 ProbeResources、Advance、应用额度；ResourceController/ResourceProbe 接入真实环境 | ExecutionMechanism 调用 ResourceController；确定性轨迹本轮重跑，Windows Job 历史实测复用 |
| T12 | TelemetryMemoryTrace 将确定容量样本交付记录；固定失败、日志留存、输出导出、共享 owner 接入 | Telemetry、Failure、Validation、OutputSinks、原生 Remap/PreparedSurface；审计与控制独立、导出失败和宿主豁免 |
| T13 | 公共请求、Filter/IGDC 与统一执行入口；删除接口具有编译契约 | PipelineContracts 的 __has_include、HasExternalResourceInjection、HasLegacyTaskGroup 断言；All 实际调用宿主桥接与文件流程 |
| T14 | 当前测试 runner、CTest 注册、ResourcePerformance/Environment、FileBenchmark、新增 SequenceBenchmark | 本文的运行记录、性能方法、实际数据和边界；多帧性能缺口补测，极小请求比例限制单独保留 |
| T15 | 已交付 Qt Fixed/Adaptive、启动线程及容量参数适配 | 沿用 QtResourceControls 和主程序既有证据，不重新编译或执行 Qt |
| T16 | 既有文档整理 | 按用户要求暂停，本文不关闭该任务 |

### 3.1 测试入口核对

`Test/Runner/DataCodecTestMain.cpp` 和 `Test/Suite/DataCodecTestSuite.h` 的注册链已核对：CTest 专项调用对应 Feature；SelfTest 调用数值、拓扑、Morton、容量、失败、执行机制、区域/reference、遥测和包契约等 Feature；ExecutionMechanism 进一步调用 ResourceController。窗口 reader/copy、Raw/Zstd field、完整 attribute payload 和 polyhedron 五索引用例已有显式注册。

`--resource-coverage` 执行 SelfTest、EncodedInputCache、DecodedFrameCache、DecodeReferenceCache。All 还执行报告文件契约、EncodedInputCache、原生 Remap、PlaybackSession 和 PreparedSurfaceAttributes。TaskCoordinator 是单独注册的 CTest。上述入口共同覆盖本文的命名断言及返回 bool/int 的集成检查。

核对了具体判定条件，例如：

- `execution.downsize-inflight` 在 4 个在途槽位、2 个活跃计算下将目标下调至 1，断言已有工作继续完成且无法额外取得槽位。
- `execution.ordered-bounded-flow` 延迟首块，检查读入有界及最终提交顺序 0—7，最后门禁关闭仍能完成收束。
- `numeric.failure-drain` 覆盖读取失败、取消、容量拒绝、提交异常和空字段，检查失败结果及槽位、计算、阶段、scratch、U 的退休。
- `numericDecode.geometryFlow` 检查真实解码值及几何转换、reference、旧大块单记录准入和消费退休；释放 owner 后检查容量归零。
- TaskCoordinator 的 `TestCommandWindowAndOrder` 检查第 3 个前台命令拒绝、一个可选预取及 1/2/4 提交顺序；`TestCancelAllDoesNotAllocate` 在持续拒绝分配时检查取消、WaitIdle 和结果交付。

## 4. 本轮构建与正确性运行

以下命令在仓库根目录、已加载环境后顺序执行。日志目录为仓库 logs，未注册需要用户大数据文件的 CTest。

```powershell
cmake --build cmake-build-relwithdebinfo-dev --target iGameDataCodecTests iGameDataCodecFileBenchmark -j 2
ctest --test-dir cmake-build-relwithdebinfo-dev -R '^DataCodec\.' -E 'DataCodec\.(ResourceModes|QtResourceControls)$' --output-on-failure --no-tests=error
& ./cmake-build-relwithdebinfo-dev/iGameCore/iGameDataCodecTests.exe --resource-coverage
```

| 修复前首批 CTest | 耗时 | 结果 |
| --- | ---: | --- |
| All | 12.37 s | 通过 |
| FailureContract | 0.18 s | 通过 |
| StorageOwnership | 0.22 s | 通过 |
| MortonResources | 0.78 s | 通过 |
| TopologyExecution | 0.24 s | 通过 |
| NumericExecution | 0.54 s | 通过 |
| NumericDecodeExecution | 0.40 s | 通过 |
| TaskCoordinator | 0.18 s | 通过 |
| ExecutionMechanism | 0.21 s | 通过 |

首批 CTest 总耗时 15.16 s；显示的单项秒数经过舍入。首批运行一次回归及一次 coverage，点集生产修复后再运行一次最终回归及 coverage。CTest 时间包含测试数据生成、断言、线程/文件准备和输出，不是编解码算法性能数据。

ResourceModes 内部会调用 17 tuple、一次正式迭代的 ResourcePerformance。本轮排除它，沿用 logs/datacodec-final-interface-ctest.log 的通过结果；执行机制和两模式对应编译契约仍在本轮 All/ExecutionMechanism 中运行。QtResourceControls 沿用 logs/datacodec-final-ctest.log 的既有通过结果。

原始日志：logs/datacodec-t14-20260910-build.log、logs/datacodec-t14-20260910-ctest.log、logs/datacodec-t14-20260910-coverage.log。命名采集末行是 `executed_check_summary,total=910,unique=635,omitted=0`，635 行各自的失败计数均为 0。

### 4.1 点集修复后的最终验证

最终增量构建目标为 iGameDataCodecTests 和 iGameDataCodecSequenceBenchmark，日志 logs/datacodec-t14-20260910-empty-topology-build.log，退出码 0。随后顺序执行序列测速、与上文相同筛选条件的 CTest、`--resource-coverage`，前项成功才执行下一项，整个命令退出码 0。该批运行结束后仅补充本文及 TODO，没有再次修改源码。

| 最终 CTest | 耗时 | 结果 |
| --- | ---: | --- |
| All | 13.68 s | 通过 |
| FailureContract | 0.18 s | 通过 |
| StorageOwnership | 0.23 s | 通过 |
| MortonResources | 0.80 s | 通过 |
| TopologyExecution | 0.24 s | 通过 |
| NumericExecution | 0.58 s | 通过 |
| NumericDecodeExecution | 0.38 s | 通过 |
| TaskCoordinator | 0.18 s | 通过 |
| ExecutionMechanism | 0.22 s | 通过 |

最终 CTest 为 9/9、16.53 s，日志 logs/datacodec-t14-20260910-final-ctest.log。最终采集日志 logs/datacodec-t14-20260910-final-coverage.log 的汇总为 `executed_check_summary,total=913,unique=637,omitted=0`，637 行失败计数均为 0。新增三次检查：`topology.pointset-no-topology-reference` 一次，`topology.nonempty-missing-reference` 两次；分别确认完整空拓扑发布成功及两类非空拓扑缺失引用仍失败。

## 5. 对象清单与运行检查索引

本节 N/T/I/S/H 是 61 行对象编号，其中 T01—T18 表示拓扑/重排对象，不是任务编号。下列短名称按所在行前缀理解，`*` 表示同前缀的一组断言。coverage 收集 Require 断言；原生与部分缓存用例由返回值及 All/coverage 的退出状态判定，不能要求每行都对应一个独立 CTest。

| 对象 | 存储或生命周期 | 本轮运行覆盖 |
| --- | --- | --- |
| N01 | 宿主 source/provider 借用 | audit.reader-borrowed、owner.remap-reference |
| N02 | raw/output/order/getter 窗口 | numeric.reader、audit.reader-order |
| N03 | 串行分量、编码块组包 | audit.serial-component-scratch、numeric-owned-payload、numeric.output-release |
| N04 | 区域背景和逐层残差 | regionPrecision.layered-capacity-*、truncated-layer |
| N05 | 解码分量和最终块 | numericDecode.geometryFlow/attributeFlow、audit.numeric-decode-capacity |
| N06 | 字段 run 索引及局部交集 | regionPrecision.fieldOwner/sharedCapacity/localIntersection/lastOwner/invalidRuns |
| N07 | reference full/probe/candidate | referenceCodec.candidate-capacities、boundedProbe.capacities、blockFlow、flowRelease |
| N08 | reference 重采样窗口 | referenceCodec.windowedResample |
| N09 | Wavelet 工作数组及 blob | referenceCodec.2.*.waveletCapacitySamples、wavelet.int32.capacity |
| N10 | Affine/Predictor 数组 | referenceCodec.1.*、referenceCodec.3.* 的 referenceCapacitySamples |
| N11 | 分组 sampleIndices/样本/edges | pipeline.sampledParentSelection、sample-capacity-denial-0 至 -4 |
| N12 | 完整属性、几何及 reference | attributes.individual-admission/necessary-denied/last-release、geometry.sized-owners/reference-release |
| N13 | 前驱 workspace 的共享寿命 | pipeline.decode-session-prunes-predecessor/command-retired/surviving-read/alias-retains-workspace/final-owner |
| T01 | order/inverse provider | remap.exact-owners/required-capacity/shared-release/last-release |
| T02 | Morton 内排工作副本 | morton.small-publish/small-release |
| T03 | Morton 可选 keyCache | morton.external-sort/key-failure/inverse-owner-refusal |
| T04 | 桶表、touched、叶描述 | morton.fixed-table-capacities |
| T05 | 高桶 slab | morton.slab-exact/slab-required/slab-replay/slab-and-run-release |
| T06 | Morton 叶工作数组及窗口 | morton.leaf-array-capacities/external-sort |
| T07 | high run 与输出并存 | morton.run-exact/run-overflow/run-range/run-release/provider-allocation-failure |
| T08 | connectivity 连续 remap | topology.remap-exact/remap-direct-read/remap-required/remap-independent/identity-exempt |
| T09 | connectivity 输入、类型、阶次 | topology.bounded-flow/polynomial-capacities/format-roundtrip/failure-drain |
| T10 | grammar 十数组 | topology.grammar-array-identities；CellGraphTopology 回放 |
| T11 | connectivity 四段 owner | topology.range-source/shared-range/range-last-reader/range-release |
| T12 | 解码块和同步 observer | topology.decode-flow/decode-drain、topologyObserver.consume-before-retire |
| T13 | 完整 topology 四数组 | topology.exact-owners/shared-owner/last-release/partial-rollback |
| T14 | polyhedron visitedFaces/localIndexTable | All 的原生 Remap：polyhedron.native-required-owner-8/-9、native-cell-work-capacities |
| T15 | 单 cell 工作数组及 overflow | All 的原生 Remap：polyhedron.native-cell-work-capacities |
| T16 | polyhedron 五流及 spool | polyhedron.fixed-stream-window、append.fixed-backend/last-release |
| T17 | 五条完整解码索引 | polyhedron.decode-phase、index-sized/read/release、index-capacity-denial-0 至 -4 |
| T18 | emit 批次数组 | polyhedron.decode-phase；All 的原生 Remap：native-emit-windows/counts/final-release |
| I01 | reader 及范围 owner | byteRange.retained-input/retained-subrange/missing-owner |
| I02 | 完整输入预读缓存 | encodedInputCache.exact-windows-last-owner/denied-before-read/necessary-live-owner/read-failure-no-replay/borrowed-root-isolation |
| I03 | 映射和 OS 预取窗口 | All 的原生 Remap：native.mapping-prefetch-admitted-window/mapping-range-lifetime/mapping-last-owner |
| I04 | 包字段范围及 writer owner | packageIdentity.parsed-ranges-share-output/parsed-ranges-final-release、multi-leaf-* |
| I05 | FieldDecodeStream 窗口 | window.field.*、byteRange.windows/cancel-between-windows、input.cancel-field |
| I06 | 外层 Zstd 完整属性 payload | payload.sized-owner/range-replay/raw-borrowed/release/admit-before-read |
| I07 | 累计数值/reference body | numeric.output-release、referenceCodec.flowRelease、append.accounting/last-release |
| I08 | spooler/bundle/segment 引用 | owner.segment-consumption/shared-reference/last-reference |
| I09 | 多叶临时包和 writer | packageIdentity.multi-leaf-shared-input/output-overlap/output-denied/retire-*/denied-cleanup |
| I10 | package probe/窗口/输出 | output.package-probe-window-0/-1/-2、package-probe-retirement、package-compute-units |
| I11 | 最终输出及连续化 | output.move-owner/allocation-failure/materialize-admission/materialize-owner/failed-growth |
| I12 | Session 弱登记及最后 owner | owner.unique-registration/session-reset/root-lifetime/root-last-release |
| I13 | 文件 store 及固定窗口 | append.fixed-backend/read-after-seal/no-failure-backend-switch、byteRange.windows |
| S01 | 编码跨帧 reference | pipeline.reference-last-consumer/reference-final-frame |
| S02 | 解码状态和 payload | pipeline.decode-session-* |
| S03 | 三类内部 LRU | frameCache.*、referenceCache.*、encodedInputCache.*；coverage 入口显式调用 |
| S04 | 有界前台及预取命令 | TaskCoordinator 的窗口、去重、最后句柄及取消无分配用例 |
| S05 | 帧依赖规划元数据 | pipeline.large-planning-shares-metadata/large-planning-release：4097 帧、69632 leaf |
| S06 | layout/region/component 元数据 | audit.metadata-outer-arrays、pipeline.sharedSpatialLayoutParams/persist-file-granularity |
| S07 | 参数字节共享 owner | output.params-shared-owner/params-session-release/params-final-release |
| S08 | BlockRecord/阶段轻量状态 | execution.ordered-bounded-flow/failure-and-next-request/reject-self-wait/commit-driven-cursor |
| H01 | 原生编码借用视图 | All 的原生 Remap：native.borrowed-input-exempt |
| H02 | 宿主派生拓扑和 offsets | All 的原生 Remap：native.converted-face-key-retirement/converted-input-release |
| H03 | 宿主解码结果及高阶暂存 | All 的原生 Remap：native.polynomial-storage-exempt/polynomial-storage-retired、polyhedron.native-emit-windows |
| H04 | prepared surface/assembly | All 的 PreparedSurfaceAttributes：payload/source/face map 最后释放及 surface 独立存活 |
| H05 | finite/签名/误差窗口 | validation.finite-window-4/8-*、analysis.signature-window-and-summary |
| H06 | remap 分析引用 | analysis.capture-owner/last-owner/read-failure/windowed-precision |
| H07 | 日志、报告和固定首错 | telemetry.active-session-cap/record-and-utf8-cap/take-reclaims-capacity、export.*、failure.persistent-allocation-failure |
| H08 | 浏览器缓冲及范围读取 | 范围外，未执行，不计作通过 |
| H09 | 第三方上下文及自有 payload 边界 | worker.compressor-serial-options/compressor-round-trip、audit.numeric-library-exempt |

这些用例验证其明确断言的容量、所有权、拒绝、回放和退休条件。第三方内部、allocator 保留页、宿主内部容器及未知峰值不通过名称计数补齐，也不形成整进程物理内存硬上限的声明。

## 6. 代表性性能实验：方法和既有数据

### 6.1 合成数据的控制变量

入口为 Test/Experiment/DataCodecResourcePerformance.h，由 runner 的 `--resource-performance tuples repetitions storage_MiB max_threads audit [profile]` 调用。四类既有数据来自优化构建，M=256 MiB、请求线程上限 P=4、审计开启；Fixed 分别用 1/2/4 线程上限，Adaptive 使用同一 M/P。每配置预热一次、正式三次，逐轮轮换配置次序，报告成功请求端到端耗时的中位数。

输入由测试程序确定生成，输出为完整内存编码包。默认数值配置无损；many-fields 关闭 reference，correlated-fields 使用默认 Auto reference，morton 启用点重排。所有配置保持相同样本、算法选项和误差配置。

编码计时围绕 Encode，解码计时围绕 DecodePackage，包含根生命周期和实际执行；端到端值为该次编码与解码耗时之和。数据生成、adapter/request 的准备、RSS 取样、逐值验证及汇总输出在相应计时区间外。不得相加独立编码/解码中位数替代端到端中位数。

无外部文件冷读过程，未执行系统缓存清理。成功条件包含输出提交、mesh/点数/字段规模、connectivity/offset 一致和无损值核对；Morton 使用唯一原始点号检查置换无重复、几何及所有属性对应。correlated-fields 的计时入口没有断言必须最终选中 reference，实际 reference codec 的正确性由专项用例承担。

| profile | tuples / 属性 / 单元 | Fixed 1 ms | Fixed 2 ms | Fixed 4 ms | Adaptive ms | 相对最佳 Fixed |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| many-fields | 17 / 128 标量 / 0 | 266.666 | 272.851 | 264.217 | 278.141 | +5.27% |
| correlated-fields | 262144 / 8 相关标量 / 0 | 134.726 | 88.4632 | 75.379 | 73.974 | -1.86% |
| variable-topology | 262144 / 2 字段 / 262144 个交替三四连接单元 | 101.242 | 86.1983 | 77.8017 | 79.0839 | +1.65% |
| morton | 1048576 / 3 字段含原点号 / 0 | 1633.23 | 1504.06 | 1439.42 | 1467.11 | +1.92% |

各配置 3 次正式请求及预热均成功，失败 0。运行记录的可用物理内存分别为 6,082,875,392 / 6,008,721,408 / 6,047,617,024 / 6,011,121,664 B。该数值是各实验开始时的环境记录，不是测试全程恒定的内存量。

原始日志位于 logs/datacodec-profile-many-fields.log、datacodec-profile-correlated-fields.log、datacodec-profile-variable-topology.log、datacodec-profile-morton.log。每个日志保留环境行、warmup 标记、逐轮编码/解码耗时、成功/失败及汇总。审计字段是已覆盖 scope 数组峰值，不能视作整个请求所有对象的同时峰值。三次中位数不支持跨设备或高置信度稳定收益的推断。

### 6.2 极小请求已知限制

沿用 logs/datacodec-final-interface-small.log：points、17 tuple、M=256 MiB、P=4、审计开启，每配置预热一次、正式 100 次，全部成功。Fixed 1/2/4 的端到端中位数分别为 9.8407/9.7550/9.97825 ms，Adaptive 为 11.0386 ms，相对最佳 Fixed 增加 1.2836 ms、13.16%。未达到原 10% 目标。本轮没有重跑该入口，也没有更改门槛来标记性能通过。

### 6.3 低容量与动态压力的证据边界

低容量沿用既有 Debug 样本，不与上表优化构建混合比较：8388608 tuples、P=4、预热一次及正式一次。M=32 MiB 下四配置成功，端到端分别为 2790.200/1846.230/1110.920/1114.620 ms；M=1 MiB 下四配置明确返回 capacity-admission-rejected，失败耗时不计吞吐。宿主输入/输出具有独立所有权，M 不等于进程 RSS。日志为 logs/datacodec-performance-lowcapacity.log、datacodec-performance-insufficient.log。

Windows Job 入口 `--resource-environment` 沿用 logs/datacodec-resource-environment.log：进程私有内存起点加 512 MiB 作为 Job 限制，测试宿主实际提交并触碰至多 512 MiB，使剩余额度约 96 MiB，保持约 3 秒后释放。同一 Adaptive 根执行约 22 秒的固定延迟终端块流，M=128 MiB。

| 观察时间 | Job 剩余 B | C/S | 事件 |
| --- | ---: | --- | --- |
| 57.76 ms | 536547328 | 4/5 | 正常准入 |
| 525.31 ms | 99811328 | 4/5 | 门禁关闭，已有工作排空 |
| 1147.72 ms | 99811328 | 2/3 | HOLD，可选留存暂停 |
| 3574.92 ms | 上次样本 99811328 | 2/3 | 宿主负载释放 |
| 5904.07 ms | 536539136 | 2/3 | 准入恢复 |
| 15963.70 ms | 536539136 | 2/3 | 可选留存恢复 |
| 16789.70 ms | 536539136 | 3/4 | 额度逐步增长 |

该实验验证真实 Job 探测、压力收缩、HOLD 排空及恢复顺序提交，终端任务是机制负载，不报告算法吞吐。M 在该轨迹保持 128 MiB，未实测真实 Job M 下调；运行中 M/C/S 发布及硬上限变化由本轮 execution/controller 确定性用例验证。不声称已经覆盖整机低内存通知、独立外部进程竞争或操作系统线程配额耗尽。

### 6.4 本轮新增的多帧性能与完成率实验

目标是补齐 TODO 独立门槛中的多帧性能要求。此前多帧编码、依赖规划、Playback 和 reference 生命周期有功能测试，四类性能 profile 均为单帧；功能测试时长不足以代替完整序列编解码计时。

新增 Test/Runner/DataCodecSequenceBenchmark.cpp 和 iGameDataCodecSequenceBenchmark 构建目标，不增加生产接口或独立资源管理设施。执行命令为 `./cmake-build-relwithdebinfo-dev/iGameCore/iGameDataCodecSequenceBenchmark.exe`，先加载相同环境并完成构建，运行期间没有并行编译。

样本固定为 6 帧、每帧 262144 个 float32 点（三分量）和 8 个 float32 标量属性，一个 leaf、无单元；坐标固定，第 f 帧各属性在原相关字段样本上增加 f×0.25。原始数组逻辑量共 69,206,016 B，约 66 MiB。点/单元顺序显式为 Original、默认无损精度、geometry/attribute temporal selection 为 Forced、关键帧间隔 3。检查至少一次 ReferenceEncode 完成记录，避免只测试六个互不相关的单帧包。

M=256 MiB；Fixed 1/2/4 与 Adaptive 4；每配置预热一次、正式三次、逐轮轮换顺序。审计、完整输入缓存、完整帧结果缓存和预取关闭，必要 reference 机制保持工作。输入六帧在计时外生成；外部测试宿主用 std::vector 保存输出及解码结果，这些是宿主缓冲，不向 DataCodec 注入线程池或内存管理设施。

编码计时覆盖整个 FrameSequenceEncodeExecutor::Execute，包含逐帧适配器构造、六个包写出、根生命周期和序列收束。解码计时是 session 构造/OpenSequence、六次 RequestFrame、逐帧交付后释放/NotifyFramePresented、Reset/析构区间之和；源 reader/request 构造及逐值检查在计时外。帧间检查期间会经过实际墙钟时间，所以该实验模拟每帧消费后继续请求的工况，不作为无人消费间隔下的饱和吞吐。

成功要求六帧编码提交、六帧解码成功、全部几何和属性逐值一致、字段结构一致、解码结果缓存命中数为零。逐轮记录编码/解码时间、编码字节数、reference 完成记录数、帧数和失败信息；失败耗时不进入成功中位数。

接入及故障记录：

- logs/datacodec-t14-20260910-sequence-build.log：首次独立编译发现 ResourcePerformance 测试头缺少 DataCodecExecutionResources 的直接包含，补齐后 logs/datacodec-t14-20260910-sequence-rebuild.log 构建成功。
- logs/datacodec-t14-20260910-sequence.log：首次运行编码六帧成功，解码因测试夹具未填稳定 sourceIdentity 被入口拒绝；各配置正式请求失败 3 次，无成功中位数。
- logs/datacodec-t14-20260910-sequence-identity.log：补齐身份后揭示生产点集缺陷，各配置均在首帧报 `failed to bind topology reference cache`；各配置正式请求失败 3 次，无成功中位数。
- logs/datacodec-t14-20260910-sequence-fixed.log：首次点集修复跳过绑定后，后续发布报 `decoded topology reference is incomplete`，各配置正式请求仍失败 3 次。最终采用现有 Kind::None 表示完整空拓扑，通过 InitializeEmpty 标记完成，由既有提交路径发布；不新增拓扑数组和容量预算。
- TopologyExecution 增加正向回归，检查空拓扑完成、提交及引用发布，并保留两个非空拓扑缺失 reference 的反向检查。最终构建与运行使用独立日志，上述失败记录保留。

最终结果来自 logs/datacodec-t14-20260910-sequence-final.log，进程退出码 0，开始时可用物理内存为 19,107,995,648 B。四配置预热均成功，编码加解码计时依次为 672.4544/517.4116/479.1966/465.9425 ms；预热不进入下列正式中位数。

| 配置 | 正式轮次 | 编码 ms | 解码 ms | 总计时 ms |
| --- | ---: | ---: | ---: | ---: |
| Fixed 1 | 1 | 364.2959 | 214.9912 | 579.2871 |
| Fixed 1 | 2 | 353.7946 | 203.3137 | 557.1083 |
| Fixed 1 | 3 | 361.0785 | 208.9028 | 569.9813 |
| Fixed 2 | 1 | 347.2354 | 160.5080 | 507.7434 |
| Fixed 2 | 2 | 309.9164 | 154.2159 | 464.1323 |
| Fixed 2 | 3 | 325.8786 | 159.9571 | 485.8357 |
| Fixed 4 | 1 | 315.4278 | 140.3664 | 455.7942 |
| Fixed 4 | 2 | 310.1130 | 139.0590 | 449.1720 |
| Fixed 4 | 3 | 292.0727 | 132.1829 | 424.2556 |
| Adaptive 4 | 1 | 306.0444 | 139.0887 | 445.1331 |
| Adaptive 4 | 2 | 297.0160 | 135.9123 | 432.9283 |
| Adaptive 4 | 3 | 291.7336 | 136.4309 | 428.1645 |

| 配置 | 正式完成率 | 编码中位数 ms | 解码中位数 ms | 总计时中位数 ms |
| --- | --- | ---: | ---: | ---: |
| Fixed 1 | 3/3，100% | 361.0785 | 208.9028 | 569.9813 |
| Fixed 2 | 3/3，100% | 325.8786 | 159.9571 | 485.8357 |
| Fixed 4 | 3/3，100% | 310.1130 | 139.0590 | 449.1720 |
| Adaptive 4 | 3/3，100% | 297.0160 | 136.4309 | 432.9283 |

三个中位数分别由各列计算，总计时中位数使用每轮编码加解码之和，不使用两个独立中位数相加。全部 16 轮（含 4 次预热）每轮均提交、解码六帧，reference 完成记录 4 条，结果缓存命中 0，总编码量 598,248 B，几何及全部属性逐值检查通过；正式运行总计 72 帧编码和 72 帧解码成功。

该样本覆盖具有重复坐标及高度相关属性的单叶点集序列，未包含磁盘 I/O、多叶或多帧拓扑变化，也未注入外部内存压力。Adaptive 4 表示配置的计算上限，未记录其全程活跃线程轨迹。三次正式测量用于记录本机该样本结果，不推断跨设备的稳定性能优势。

## 7. 大型 CGNS 编解码实测

输入标识为 car_3800W_Fluent_node_HDF5-0001.cgns，原始文件大小 10,571,784,552 B。读取后的数据为 1 个可编码叶、8,034,973 点、39,494,600 单元和 131 个 Float64 单分量属性，合计 1,052,581,463 个属性标量。输入路径由测速入口参数提供，不加入仓库测试夹具。

入口 Test/Runner/DataCodecFileBenchmark.cpp 使用外部 CGNS reader、公共 Encode 和全属性 DecodeDataCodecDataObject。资源为默认 Adaptive、自动线程/容量目标，默认无损数值压缩。日志没有记录完整动态额度轨迹，16 是硬件逻辑处理器数，不是全程活跃计算数。

### 7.1 计时及验证边界

- CGNS 载入单独计时，包含读取器后处理和表面提取，不计入 DataCodec 编码时间。
- 编码包含 adapter 构造、完整编码、文件写出及关闭，输出到新文件；没有保证物理磁盘强制落盘完成。
- 同进程解码在释放源对象、reader 和编码 adapter 后开始，包含映射输入、几何/拓扑/全部属性解码、宿主结果构造及便捷入口会话清理，不包含随后元数据比较。
- 单次运行，未清空系统文件缓存；解码读刚生成的文件，可能受缓存明显影响。DataCodec 解码结果缓存未命中。
- 已对比点数、单元数、叶数和全部属性的名称、类型、tuple 数及分量数；本次大文件未逐元素核对数值和连接，不能声明它完成了逐值无损验证。
- 进程 working set/private bytes 与 DataCodec 容量审计分开解释；生命周期工作集峰值含宿主、映射和读取器，不是受控存储 U。

### 7.2 实际数据与失败修正

| 运行 | CGNS 载入 s | 编码 s | 解码 s | 结果 |
| --- | ---: | ---: | ---: | --- |
| 首次同进程 | 33.692 | 27.596 | 33.463 | 编码成功，解码内存压力 HOLD 超时 |
| 独立解码进程 | 不适用 | 不适用 | 8.611 | 全属性解码成功 |
| reader 所有权修正后同进程 | 26.942 | 26.972 | 7.594 | 完整编码、解码成功，规模对比通过 |

两次编码结果大小均为 2,204,353,773 B，原文件/输出文件大小比为 4.796。按首次源文件大小折算编码速度为 383.09 MB/s，独立解码按编码文件大小计算约 256.00 MB/s。MB 使用十进制。不同文件格式有不同元数据和表示开销，文件大小比不等于同一原始字节流的压缩比，吞吐分母不能混用。

首次失败根因是新测速代码以 `auto reader = iGameCGNSReader::New()` 接收裸指针，随后置空没有销毁读取器，原始数据仍被持有。外部读取改为 `iGameCGNSReader::Pointer` 后，解码前私有内存从 10,915,344,384 B 降至 9,809,920 B，工作集降至 22,253,568 B，同进程解码成功。这是测速入口所有权错误，不能作为 DataCodec 自身泄漏证据。DataCodec 内部智能指针仍使用 C++ 标准库类型。

首次进程启动时可用物理内存约 5.13 GB；独立解码约 19.85 GB；修正复测约 19.29 GB。独立解码进程工作集峰值为 12,564,152,320 B，约 11.70 GiB。运行环境和缓存状态不同，26.972/7.594 s 的复测不代表算法优化收益。本轮 T14 核对沿用这些已完成的大文件实验，没有再次读取该大文件。

原始日志：logs/datacodec-car-3800w-20260910.log、datacodec-car-3800w-decode-20260910.log、datacodec-car-3800w-owned-reader-20260910.log。生成文件名为 car_3800W_datacodec_bench_20260910.igc 和 car_3800W_datacodec_bench_20260910_owned_reader.igc，保留于原始数据目录。

## 8. 交付范围及未验证内容

首批既有核心用例验证通过；新增多帧实测补齐 TODO 的性能覆盖缺口并暴露点集帧的拓扑 reference 绑定及发布错误。修复在 TopoDecodeStage 中对 cellCount 和 cellBufferSize 均为零的 PointSet 调用 DecodedTopologyCache::InitializeEmpty，生成完整、无数组的空拓扑状态，接入原提交与引用发布；非空拓扑保留 reference 校验。新增 topology.pointset-no-topology-reference 及 topology.nonempty-missing-reference 正反向回归，由原 TopologyExecution/All/coverage 注册链执行。

修复后最终核心回归 9/9、913 次命名检查零失败，四配置序列实测全部成功，结果与方法分别记录于 4.1 和 6.4。T14 按当前 Windows 核对与必要验证范围完成；T16 保持暂停，其他既有文档不作修改。

独立编译同时暴露 ResourcePerformance 测试头依赖此前的间接包含，已补充 DataCodecExecutionResources 的直接包含。多帧测试夹具首次漏填稳定 sourceIdentity，已补齐。这两个测试接入问题与生产点集缺陷分别记录，失败耗时不进入成功吞吐。

上述结果支持已列 Windows 输入、容量及生命周期边界，不扩展为全平台、全负载或整进程物理内存硬上限保证。

未验证及不再执行的部分保持明确：浏览器对象 H08、Linux/WASM/多设备；真实 Job M 下调及整机低内存通知；所有 allocator/宿主内部峰值；大 CGNS 的逐元素比较及冷缓存持续吞吐。极小 points 的原比例目标未达标，按用户决定不继续优化。Qt 采用既有适用证据，本轮没有重跑。

原始 logs 属于本地实验产物，受仓库忽略规则影响；本报告保存关键参数、判定方式、数据和日志索引，Git 中的报告不表示所有原始日志或用户输入大文件已经入库。三个未接入实验头和 std_compat 的原有工作区状态保持原样。
