# DataCodec 资源改造集中验证

## 口径

本文记录实际执行证据。实现进度以 Memory_Thread_TODO.md 为准，测试通过仅证明被执行的输入与平台。未执行项保持未验证，不以事件模型替代操作系统压力实验，不以进程 RSS 取样替代峰值。

2026-09-09 用户更新验收范围：本次仅验收当前 Windows 核心编解码，移除跨平台、Linux、WASM、浏览器及跨设备验收门槛。Linux 构建已中止；历史未验证平台记录保留为边界说明，不计为本次待办。Qt 既有通过结果直接沿用。后续逐对象索引中的浏览器平台项按范围移除处理。

当前收尾状态（2026-09-10）：T00—T15 已关闭，T16 待最终交付归并。用户明确决定“T14到此为止”，因此结束该项继续优化与验收；最新极小 points 的 Adaptive 额外 1.2836 ms（+13.16%）仍未达到原 10% 目标，不记为性能达标。Windows 既有优化构建及核心 CTest 10/10 通过；61 行对象台账为 60 行直接证据、1 行浏览器范围外。后文保留各批历史测量和适用边界，历史待办以本状态及 TODO 顶部为准。

## 平台与构建

- 平台：Windows 11，原生 UCRT64 Clang，C++20，Ninja，LLD，Debug
- 沿用已核实的 CLion 环境脚本与现有构建目录
- 核心库、核心测试、Qt_module、Qt 资源控件测试及实际 iGameVis 主程序均已完成构建链接
- Linux、实际浏览器/Wasm、无外存设备尚无本次运行证据；无线程与无外存的测试能力配置单独记录
- 保留三个未接入实验头及 ThirdParty/std_compat 的既有工作区状态

## 已执行的回归

- 首轮完整 CTest：8/11 通过，20.04 秒
- 后续完整 CTest：10/11 通过，13.19 秒，All 尚有集成失败
- 真实编码窗口 offscreen 用例已通过：无模型时模式、线程和容量控件均禁用；配置控件通知、临时 INI 持久化、显式零容量及解码设置交付通过
- 最后 polyhedron 提交失败已定位为提交后使用旧供给状态而接纳空块；执行机制新增提交驱动游标回归，并保留必要的算法完成首错
- 修复期间工作类型边界用例发现下一块描述被重复计算，当前修正为独立读取游标保留描述、单记录提交游标在提交后计算描述；ExecutionMechanism 复测通过，0.20 秒
- Playback 用例按新方案验证内部留存、宿主无第二份 LRU、宿主清理后同一结果可回放；最终 All 通过，15.09 秒，包含后续宿主集成
- 十一个注册 CTest 已在集中回归与失败范围复测中分别通过；最新 All 与 ExecutionMechanism 为修复后的验证，Qt 真实编码窗口用例已通过。本记录不表示十一项在最新提交上重新完整运行过

## 性能测量约定

测量前固定以下条件，后续调整需另列实验：

- 同机、相同算法与文件格式，float32 点集含几何三分量、标量字段及向量三分量字段
- Original 点序、完整内存编码输出，每次解码后逐值比较；重排语义由独立宿主 remap 用例验证
- M=256 MiB，Fixed 的线程上限为 1、2、4，Adaptive 上限为 4
- 每项预热一次、正式三次，执行顺序轮换；输入生成不计时，线程初始化和最终输出交付计时
- 小样本 17 tuples，中等样本 262144 tuples，大样本 1048576 tuples
- 在首轮暴露自动增长窗口开销后，追加 8388608 tuples 长请求，保持同一算法、M、线程及重复次数，单独列出
- 记录四种配置的成功率与成功耗时中位数；失败单列，不以失败耗时计算性能收益
- 关闭与开启审计分别运行中等样本，报告耗时差异
- 初始验收目标：Adaptive 相对最佳可行 Fixed 的端到端中位耗时增加不超过 10%；Debug 结果用于定位调度问题，正式吞吐结论需要优化构建复测
- 自有数组审计按单根作用域事件峰值记录，编码与解码分别列出；RSS 为请求前、编码后和解码后的三个进程取样

## 本机首轮性能数据

设备探测返回物理内存 33522085888 字节、允许线程 16、可用内存约 6.37—6.41 GB；以下为端到端成功中位数，单位 ms。每格均为三次正式运行成功，预热全部成功，完整数值回放全部匹配。

| tuples / 审计 | Fixed 1 | Fixed 2 | Fixed 4 | Adaptive 4 |
| --- | ---: | ---: | ---: | ---: |
| 17 / 开 | 12.1379 | 13.0796 | 11.9389 | 15.7157 |
| 262144 / 开 | 112.180 | 77.3923 | 64.7763 | 110.917 |
| 1048576 / 开 | 375.145 | 231.442 | 179.572 | 387.745 |
| 262144 / 关 | 110.819 | 80.8816 | 66.1055 | 110.910 |
| 8388608 / 开 | 2564.870 | 1443.370 | 873.485 | 2543.320 |

本轮 Adaptive 未达到 10% 性能目标。1048576 tuple 样本约为最佳 Fixed 的 2.16 倍。当前自动模式从 C=1 开始，增长要求至少 500 ms 观察及三个新环境样本；短于这一窗口的工作类型可能在增长前结束。后续需针对增长轨迹与优化构建继续验证，不能将当前默认参数记为已校准。

本轮报告开关差异不足以给出可靠开销比例，样本中存在毫秒级噪声。所有配置完成率均为 100%，无取消、容量不足或操作失败。真实外部压力、长期多帧、代表性 reference/拓扑/重排吞吐、设备差异及优化构建性能尚未验证。

长请求仍未增长的排查发现实际等待分类错误：driver 等待前序计算时使用 OrderedCommit，原统计不检查是否已经存在结果，导致计算等待被记为 Wo 并阻止增长。修正为仅在 maxCompleted 超过 lastCommitted、确有未消费结果时累计 Wo；保留输出 I/O、下游消费和前序阻塞的分类规则，复用既有顺序水位，不新增分配审计或逐块扫描。新增真实终端队列回归区分尚无结果的计算等待和已有结果的提交等待，本批待验证。

## 健康启动规模校准

等待分类修正通过 ExecutionMechanism；单独修正后的长样本仍为 Adaptive 2458.110 ms、最佳 Fixed 834.018 ms。进一步按方案允许的参数校准，将高水位且无压力的启动改为 C=min(P,4)、S=C+1，类型边界采用同一有限启动；压力历史、信号恢复和冷却期间保持收缩目标。

新增控制器用例覆盖 P=1/2/4/8/16 的健康启动和类型边界、压力后不恢复高并发、信号未知单线程。ExecutionMechanism、ResourceModes、QtResourceControls 共同复测通过，8.54 秒；Qt 覆盖实际窗口加载标量字段后启用、设置写入临时配置、移除模型后禁用。

相同四线程上限下重新测量，所有配置各三次正式回放及预热成功：

| tuples | Fixed 1 | Fixed 2 | Fixed 4 | Adaptive 4 |
| --- | ---: | ---: | ---: | ---: |
| 17 | 12.8599 | 12.5837 | 12.8215 | 14.7926 |
| 262144 | 114.482 | 74.3872 | 66.8338 | 66.6023 |
| 1048576 | 384.582 | 239.418 | 185.457 | 180.340 |
| 8388608 | 2637.420 | 1529.780 | 889.443 | 932.098 |

中、大、长样本满足本轮 10% 目标，长样本差异约 4.8%；17 tuple 仍有约 2.2 ms 额外成本，未达到相对比例目标。该结果只覆盖请求上限为四线程的本机 Debug 配置。

后续追加两项测量：17 tuple 增至二十次正式运行以减少短请求噪声；8388608 tuple 将线程上限改为本机允许的十六线程，Fixed 取 1/8/16，Adaptive 上限 16，M=256 MiB、预热一次、正式三次。结果单列，不把四线程校准推断为高并发设备验收。

追加测量全部成功：小样本二十次的最佳 Fixed 为 13.4855 ms、Adaptive 为 16.1239 ms，额外成本约 2.64 ms，仍未满足相对 10% 目标。十六线程上限的 Fixed 1/8/16 分别为 2628.260/638.037/624.842 ms，四线程启动的 Adaptive 为 902.382 ms，差异约 44.4%。

根据十六线程实验，当前方案进一步将健康启动上界设为八，计算仍受 P 限制，后续增长继续使用原观察窗口；四线程及更小上限的启动行为保持该上限。该校准只修改共用策略常量，压力后恢复规则保持原约定。新的完整回归及十六线程性能复测待记录。小请求的固定自动管理成本保持待优化，不通过修改验收阈值把未达标项记为完成。

## 本批最终结果

健康启动上界为八的版本已完成核心、Qt 资源测试及 iGameVis 构建；完整 CTest **11/11 通过，26.72 秒**。这一结果包含等待分类、健康启动边界、压力后类型切换、实际编码窗口控件与配置交付。

十六线程上限的同条件长样本再次全部成功，正式中位数如下：Fixed 1 为 2750.810 ms，Fixed 8 为 679.625 ms，Fixed 16 为 650.160 ms，Adaptive 为 705.232 ms。Adaptive 相对最佳 Fixed 增加约 8.5%，满足本机这项 10% 目标；没有将同一次测量的最优单轮替代各配置中位数。

低容量追加实验使用 8388608 tuples、线程上限 4、预热一次与正式一次：

- M=32 MiB：四种配置全部成功，Fixed 1/2/4 与 Adaptive 耗时分别为 2790.200/1846.230/1110.920/1114.620 ms；解码根数组峰值为 32 MiB。约 234881024 字节的原始输入及宿主解码结果具有自身所有权，进程 RSS 约 490 MB，未将其声明为 32 MiB 的整进程限制
- M=1 MiB：四种配置的预热与正式请求均明确失败，首错均为 `capacity-admission-rejected`，信息为 `required owned storage capacity is unavailable`，无完整结果发布；后续独立请求继续执行并得到相同类别失败。失败样本不计入成功耗时比较

尚未完成的验收边界：17 tuple 的约 2.64 ms 固定自动管理成本及其相对 10% 目标、优化构建性能、真实外部压力及恢复轨迹、更多代表性算法/设备、Linux/实际浏览器平台，以及 61 项覆盖清单的完整证据归并。现有确定性控制器和功能测试不替代这些设备实测。

## 优化构建与控制唤醒验收

2026-09-09，Windows 原生 Clang、Ninja、LLD、RelWithDebInfo（`-O2 -g -DNDEBUG`）核心构建通过。核心 CTest 10/10 通过，18.69 秒，包含最后的原生压力回调锁同步、取消后根复用和公共接口删除编译契约。该目录的 Qt 目标未重建，本次排除 Qt CTest；Qt 证据沿用上一批实际窗口测试。

NORMAL 的普通块事件仅累计观察，控制线程按 250 ms 周期读取；DRAIN/HOLD、请求生命周期、停止及原生压力通知即时唤醒。控制线程与 worker/driver 使用同一锁下的独立条件变量，控制决策未改变状态时不额外通知 worker/driver。

新增显式 `--resource-overhead`，每项预热一次、正式一百次，计时区间不生成诊断文本。优化版本中位数：设备探测 0.0091 ms，monitor 创建/观察/销毁 0.0029/0.0018/0.0026 ms；Fixed 根创建/销毁 0.0114/0.001 ms，Adaptive 根创建/销毁 0.4476/0.1835 ms。平台探测与原生 monitor 的成本很小，Adaptive 独立控制线程生命周期仍有可测固定成本。

构建及回归结束后独立计时，所有配置、预热和正式运行均成功，完整数值回放匹配：

| tuples / 正式次数 / 线程上限 | Fixed 1 | Fixed 中档 | Fixed 上限 | Adaptive | 相对最佳 Fixed |
| --- | ---: | ---: | ---: | ---: | ---: |
| 17 / 100 / 4 | 9.93195 | 9.8957（2） | 9.8969（4） | 11.0579 | +11.74% |
| 262144 / 3 / 4 | 43.3513 | 36.3427（2） | 30.5061（4） | 32.6176 | +6.92% |
| 8388608 / 3 / 16 | 764.131 | 309.215（8） | 365.917（16） | 316.962 | +2.51% |

单位 ms，M=256 MiB，审计开启。17 tuple 的二十次初测为最佳 Fixed 10.1366 ms、Adaptive 11.191 ms，因靠近 10% 边界追加一百次；追加结果仍未达标，额外 1.1622 ms，不修改验收门槛。中等及长请求满足本机优化构建目标；十六线程配置的最佳 Fixed 为八线程，比较基准取实际最佳可行配置。

## Windows Job 真实受限环境

显式 `--resource-environment` 在独立测试进程建立 Job 的进程内存硬限制：启动私有内存加 512 MiB。测试宿主随后实际提交并触碰一段至多 512 MiB 的内存，使 Job 剩余额度约为 96 MiB，保持三秒后释放。初始宿主可用内存不足 1 GiB 时不开始实验。该内存属于实验宿主；不通过 DataCodec 预约，不伪造 ResourceSample，不调用 UpdateLimits 触发状态转换。

同一 Adaptive 根执行 22 秒的固定延迟终端块流，采样上限 600 条，实际结果如下：

| 观察时间 | 真实剩余额度 | C/S | 准入及运行状态 |
| --- | ---: | --- | --- |
| 58 ms | 536547328 B | 4/5 | 正常接纳，已有在途计算 |
| 525 ms | 99811328 B | 4/5 | 压力待确认，gate 关闭，在途已排空 |
| 1148 ms | 99811328 B | 2/3 | HOLD，N=R=0，可选留存暂停 |
| 3575 ms | 上次采样仍为 99811328 B | 2/3 | 宿主释放负载，等待新的恢复样本 |
| 5904 ms | 536539136 B | 2/3 | gate 开放，恢复块处理，仍在冷却期 |
| 15964 ms | 536539136 B | 2/3 | 冷却结束，可选留存恢复 |
| 16790 ms | 536539136 B | 3/4 | 后续计算及槽位额度逐步增长 |

实验退出码为零，检查了压力确认、HOLD 时真实排空、计算额度收缩、恢复接纳、可选留存恢复和完整顺序提交。M 的上限在这次轨迹中保持 128 MiB，不能把 C/S 收缩描述为 M 的动态下调证据。

本项证明 Windows Job 剩余内存的真实探测及同一执行机制的响应；终端工作使用固定延迟，本项不报告编解码算法吞吐，不声称覆盖整机低物理内存通知或独立外部进程竞争。Linux、实际浏览器、真实多设备与代表性 reference/拓扑吞吐继续保持未验证。环境发现：WSL Ubuntu 有 CMake、Ninja、Clang/GCC，Windows 与 Ubuntu 当前命令环境均未找到 emcc。

## 逐对象运行证据索引

### 共用控制机制补充矩阵

本批新增矩阵已写入既有 ExecutionMechanism 注册入口，优化构建通过，该 CTest **1/1 通过，2.82 秒**（用例本体 2.63 秒）。以下用例复用同一 Advance 纯状态逻辑和真实执行根，不增加生产调度入口。虚拟时钟循环不进行真实休眠。其他模块沿用上一批核心 10/10 结果，本批没有重复完整回归或性能矩阵。

| 方案约定 | 对应运行检查 |
| --- | --- |
| 高水位有限启动、观察尚未退休不增长 | controller.healthy-startup-bound、retire-required |
| 同类型计算增长及参数范围 | controller.compute-growth、growth-parameter-matrix；P=1/2/4/8/16/64，每项连续 80 个观察批，检查增长可达且不越界 |
| 仅因输入不足增加槽位、新采样要求 | controller.fresh-slot-growth；重复旧样本不清空观察、不增加样本数 |
| 慢首块、未消费结果和输出拥堵 | controller.commit-backpressure、execution.compute-is-not-output、completed-output-wait、ordered-bounded-flow |
| 单次低水位脉冲与反复波动 | controller.short-pulse、oscillation-deadline；反复越过 L 后在两秒内确认 |
| 持续压力只减半一次、严重压力升级 | controller.confirm-once、escalation；已有 N/R 保留真实值 |
| HOLD 截止不延期、完整提交不新增等待 | controller.hold-deadline、finished-with-pressure |
| L/H 中间区域的运行与 HOLD 区别 | controller.middle-band |
| 恢复后重复压力的退避上限 | controller.backoff-ceiling；10/20/40/60/60 秒，retention-cooldown/restore 检查可选留存 |
| 过期样本及恢复确认 | controller.signal-loss、unknown-progress、signal-recovery；未知信号与原 HOLD 截止由 hold-deadline 联合覆盖 |
| 重型阶段未退休时类型不能交接 | controller.type-waits-heavy-phase、type-keeps-pressure-reduction、work-type.drained-boundary |
| M 增长与计算增长互斥 | controller.storage-compute-exclusive |
| 普通压力保持 M、真实硬上限下降限制后续预约 | controller.reference-hard-limit；真实 lease 600 MiB 与后续 64 MiB，512 MiB 新硬上限保留存活 lease 并拒绝增长；仅预约，无对应大数组分配 |
| 环境剩余额度不当作 M 的硬上限 | controller.remaining-is-not-ceiling |
| 独立新请求重算 M、字段切换不立即重算 | controller.request-memory-boundary |
| 运行中 M/C/S 下调、等待工作继续完成 | execution.downsize-live-capacity、downsize-inflight、grow-queued-work、exclusive-downsize |
| 持续分配拒绝下 worker 创建失败及队列清理 | execution.worker-creation-allocation-failure；检查零任务执行、锁外捕获析构、一次失败申请、N/R/队列归零、关闭根及 join |

上述矩阵验证确定性决策和明确的执行故障边界。操作系统信号实测继续引用 Windows Job 实验；没有把虚拟样本称为新的 OS 压力实测，也没有把 worker 创建时 C++ 分配拒绝称为内核线程配额耗尽。

T04/T11 的共用执行与自动控制验收据此关闭：额度更新、凭证寿命、终端队列、首错收束、线程创建分配失败和确定性控制矩阵已有直接运行证据，原生资源探测与恢复轨迹已有 Windows Job 证据。业务模块的具体数组、独立审计和性能目标继续分别在 T06—T09、T12、T14 验收；此状态不表示整套改造已完成。

### Windows 收尾批次：代表性性能样本

2026-09-09，沿用上述 RelWithDebInfo 优化构建，M=256 MiB、P=4、审计开启，每种模式预热一次、正式三次。耗时包含完整内存编码、解码和结果交付；数据生成与逐值回放核对在计时区间外。四种 profile 均完成全部预热和正式请求，失败为零。

| profile / tuples | Fixed 1 / ms | Fixed 2 / ms | Fixed 4 / ms | Adaptive / ms | 相对最佳 Fixed |
| --- | ---: | ---: | ---: | ---: | ---: |
| many-fields / 17 | 266.666 | 272.851 | 264.217 | 278.141 | +5.27% |
| correlated-fields / 262144 | 134.726 | 88.4632 | 75.379 | 73.974 | -1.86% |
| variable-topology / 262144 | 101.242 | 86.1983 | 77.8017 | 79.0839 | +1.65% |
| morton / 1048576 | 1633.23 | 1504.06 | 1439.42 | 1467.11 | +1.92% |

- many-fields 使用 128 个标量字段，明确关闭 reference，覆盖大量短字段的阶段切换
- correlated-fields 使用八个线性相关标量及默认 Auto reference，证明相关字段和分组路径的整体回放；该计时入口没有断言最终必须选择 reference，实际 reference codec 正确性继续使用 referenceCodec 与 pipeline.temporalExecution 用例
- variable-topology 使用交替三/四连接的非结构网格，逐值核对连接、offset、几何和属性；首轮暴露测试 adapter 只接收 PointSet，补齐 UnstructuredMesh 支持后本轮通过
- morton 使用唯一原始点号核对排列无重复以及所有几何、属性的对应关系；重排校验不计入计时

以上四类满足同机 10% 比例目标。原 points 的 17 tuple、一百次实验仍为 +11.74%（额外 1.1622 ms），维持未达标记录。控制线程生命周期已测得固定成本，本批保留单一内部控制器，没有为短请求增加新的执行模式或资源管理设施。

### 索引与证据边界

最新业务容量批次优化构建与 `--resource-coverage` **通过：856 次检查、598 个不同名称、失败零、遗漏零**。该次运行包含上面的控制矩阵与 worker 分配拒绝测试。新增五组窗口/存储原用例现由 SelfTest 显式调用，普通 All 和覆盖入口都执行；旧 DataCodecFeatureBudget 的函数定义曾未接入当前 runner，历史构建结果不作为它们本版通过证据。

- `regionPrecision.layered-capacity-float32/float64`：三分量、两层精度的逐元素回放、背景/残差真实容量取样峰值与 scratch 归还；两种类型的末层截断均被拒绝。首次测试将最新取样当成最大层容量，已改用既有 sampledPeakBytes，生产审计口径没有变化
- `polyhedron.index-capacity-denial-0` 至 `-4`：逐个必要索引 owner 容量拒绝，历史 U 与前面已准入数组对应，实际 U 归零，无输入读取、未发布 cache、重型阶段已归还
- `payload.sized-owner/range-replay/raw-borrowed/release/admit-before-read`：跨两个完整窗口及尾部的外层 Zstd，Memory/File 正确回放、Raw 只共享原 source、最后 owner 释放、容量不足不启动读取
- `window.reader.* / window.copy.* / window.field.*`：固定窗口及尾部、范围越界、读写失败不继续搬运、Raw/Zstd 流状态移动、Zstd 截断拒绝；`polyhedron.index-*` 补完整五条索引两后端的精确尺寸、移动、回放和释放

前一缓存批次的优化构建和核心 CTest **10/10 通过，14.50 秒**；Qt 沿用既有验收。该批 `--resource-coverage` 为 **770 次检查、547 个不同名称、失败零、遗漏零**，在 SelfTest 之外显式执行输入缓存、完整帧缓存和 reference 缓存原用例。输入缓存现已同时接入普通 All 入口。

新增运行证据补齐以下边界：

- 完整输入恰好读取 `2×1 MiB+13 B`，分三次窗口完成，只占一份确定容量；M 下调为零驱逐可选缓存后，消费者仍可读，最后引用释放 U 归零
- 完整预读未获准时读取次数为零；已开始读取后失败保留首错、不重放、释放全部预读 owner
- 必要容量拒绝后的请求按失败收束；外部输入消费者在收束后仍持有真实数组，最后释放才归还 U；独立新请求复用根并重新成功申请
- 分组评分依次在索引、三个字段样本和首条实际候选 edge 的容量边界拒绝；前四项没有读取样本，最后一项每字段读取 32 个 tuple；所有失败均未发布 schedule 且 U 归零

共用模块完成证据：T05 由公开接口删除的编译契约、controller.modes、ResourceModes、owner.same-root、scratch.root-view、failure.workspace-run-binding 和 pipeline.encodeSessionContracts 共同覆盖；T10 由 scratch.*、三个内部缓存命名用例及独立 TaskCoordinator/All 覆盖。TaskCoordinator 的四个现有用例分别验证同目标去重、句柄释放后收束与新命令、一个运行/一个前台待运行/一个预取及拒绝额外前台、持续分配拒绝时取消全部待运行命令。业务前驱 workspace 退休的逐对象证据继续归 T08，完整对象清单和性能门槛继续归 T14/T16。

上一批优化构建的 `--resource-coverage` 为 746 次检查、523 个不同名称，失败零、遗漏零。采集期使用固定数组，不分配、不输出；完成测试后按名称汇总。独立 Qt、Playback、prepared surface 与平台实验继续使用各自运行记录；未接入命名采集的检查通过其独立用例记录引用。

下表按方案 7.8 的原顺序覆盖 61 行对象，编号只用于本台账。D 表示已有与该对象直接相关的命名运行证据；G 表示当前索引仅关联到共用机制或端到端运行证据；U 表示缺少当前平台的运行证据。D 不自动代表该行列出的每个数组和每个失败注入点均已完成验收，G/U 继续作为待补齐项。表中测试名均来自上述实际输出；带 `*` 的名称表示已执行名称的前缀。

### 2026-09-09：业务 owner 与原生边界补充验证

- RelWithDebInfo 核心库及测试构建通过，核心覆盖入口 888 次检查、621 个不同名称，失败零、遗漏零；日志为 logs/datacodec-native-owner-build.log、logs/datacodec-native-owner-coverage.log
- 原生 --remap-contract 通过，日志为 logs/datacodec-native-owner-remap.log。本入口执行新增原生资源边界用例及既有 surface/polyhedron 语义回放；原生新增检查未接入核心命名采集，不能算进上述 888 次
- reference 完整 staging 与 4096 tuple probe 使用同一个根，分别记录实际容量和独立取样身份；三个完整/尾块的候选在槽位退休前取样，输出消费后容量与活动 scratch 归零
- Morton 固定高低桶表、touched、scratch、orderedElements、lowBuckets 的实际容量已逐项断言。相同 key 仅有一个叶描述，超出叶阈值后分窗口输出；首轮测试误期望两个描述，已根据实际分块算法修正并复测
- 三个叶 payload owner 中一个被两个字段共享，封装时仅计三个源 owner；完整输出与源同时持有确定容量，少一个字节即在调用任何叶 writer 前拒绝。逐叶退休、外部输入消费者、解析后字段范围持有完整输出以及最后引用释放均通过
- 真实 DecodeSession 发布关键帧 attribute/geometry reference，下一独立帧结束后前驱 leaf state 数降到零；命令 required lease 和会话释放后，外部消费者仍可读取。byteStoreSession 的最后 alias 持有实际 workspace，释放后 U 归零
- connectivity 六组合新增非空阶次输入和编码容量，逐块核对重排后的阶次值及解码取样
- 原生 polyhedron 使用 65537 个 cell、每 cell 九个三角面引用：分别以 M=8/9 字节拒绝 visitedFaces 的 9 字节及 localIndexTable 的 24 字节；前一阶段 owner 已释放，未进入块处理。成功路径两个批次共享 24 字节表，单 cell 五个工作数组按真实 capacity 取样；overflow 数组在合法点域中逻辑为空，预留容量仍被准确取样
- polyhedron 完整五索引回解后原生 emit 跨两个 cell 批次，第一批 face-count 扫描窗口固定为 1 MiB，尾批为九个索引；offset/local id 数组按本批真实数量取样，原生输出不增加 U，最终完整索引与输出退休后 U 归零
- Windows 原生映射在已取得槽位时接受 1 MiB PrefetchVirtualMemory 提示；范围读取、越界拒绝和最后 reader 引用释放通过。本实验验证 OS 接受提示及映射寿命，不测量页面实际驻留或回收时点
- 原生几何和属性视图借用、Unstructured polyhedron 面表转换的 key 容量峰值与清理、派生 offset 释放、高阶暂存数组在 Abort/下一 topology 时清理，均在 M=0 的根旁验证豁免边界。宿主对象仍由宿主管理；没有将宿主内部所有容器纳入审计
- 新原生测试单独包含 polyhedron 编码头时暴露直接依赖缺失，生产头已补齐 PolyhedronTopologyRemap 包含；测试补齐 iGameDecodeAdapter 定义。未修改算法、调度和审计实现

T06/T08 据此关闭：普通/区域/reference 数值编码、完整 owner、数值与拓扑解码、同步消费和实际前驱 workspace 退休已具备直接证据。其余对象证据由下一批记录补齐，性能独立验收。

### 2026-09-09：剩余五项对象证据与模块收尾

- RelWithDebInfo 构建通过；`--resource-coverage` 共 910 次检查、635 个不同名称，失败零、遗漏零。日志为 logs/datacodec-final-object-build.log、logs/datacodec-final-object-coverage.log
- Windows 核心 CTest 10/10 通过，13.86 秒，包含 All 内的 prepared surface/assembly 新生命周期用例。实际筛选为 `-R '^DataCodec\.' -E 'Qt' --no-tests=error`，日志为 logs/datacodec-final-object-ctest.log。首次使用错误前缀未匹配任何测试，不计入通过证据；修正筛选后完成上述十项
- connectivity 六种组合逐项检查十个 grammar 数组已经记录容量、时间和互不相同的 scopeId；既有逐块容量、回放、取消和释放检查在同一次运行中通过
- package probe 读取逻辑长 8 MiB+13 B 的数据源时只读一次 offset=0、长度 1 MiB；可压缩、高熵和读取失败三个输入均验证实际 HeavyPhase、一个计算额度、一个活动 scratch，以及终端工作结束后的 scratch/U/阶段退休
- 规划实验包含 4097 帧、69632 个 leaf 记录与 4096 条前驱依赖。预加载 FramePackage 与 reader 共享引用，无重复读取；规划顺序完整，planner 退休后释放自身引用。确定可计算的 FramePackage 对象及 leaf 数组容量为 6619256 B，输出两个数组容量为 49152 B，规划耗时 5.2892 ms。map 节点、string、共享控制块和 allocator 开销未测量；这些已知子项不作为规划总内存或调度依据
- finite 校验以 float32/float64 各读取 65536、65536、7 个值，完整、末尾 NaN、第二窗口读取失败三个路径验证次数和偏移。签名分析跨 1 MiB 边界，通过非恒等 remap 读取两段，验证 sum/min/max 及三个容量为一的摘要数组，随后运行既有第二段读取失败用例。有限窗口读取证据不表述为完整 allocator 峰值测量
- 原生 frame assembly 完成后交付 payload，assembly/adapter 退休仍由 payload 保留 prepared source 和 face map；最后 payload 释放后 map 引用归还，独立 surface 继续可用。M=0 的根旁确认该宿主路径不增加受控容量；宿主未覆盖数组与派生图形缓存继续豁免
- 本批仅修改测试与验收文档；编译时修正新增测试 reader 的 ReadAt 多余 const，生产接口和算法保持原有实现

T07/T09/T12 的对象接入验收据此关闭。T13 的剩余删除证据由下一批补齐。极小 points 性能目标和 T16 最终交付继续保持未完成。

### 2026-09-10：旧执行接口删除与最终 Windows 回归

- 方案 17.2 列明的十九个旧资源符号在当前跟踪 C++ 源码中没有匹配，九类旧模块头不再跟踪；不可包含旧头、六类请求无外部执行/cache 字段及启动配置类型由 PipelineContracts 编译契约验证。检查范围限于指定删除项，排除文档、明确的负向编译用例和三个未跟踪实验头
- 清单检查发现旧 IParallelTaskRunner/IParallelTaskGroup、CreateGroup、ParallelTaskGroup、ParallelForChunks/Threshold 与 Inline runner 无业务调用方仍被保留。本批删除全部声明、实现、独立任务记录队列、空 runner 分支、旧 TLS worker index 和 Context 前向声明，根类直接提供统一有界执行接口；新增 CreateGroup 不可用的编译契约
- bridge 内自己持有的 executionResources/cacheRuntime 是当前内部根与缓存的生命周期引用，没有恢复外部注入。ResidentSizeHint 的数据查询接口继续保留，其查询结果不因名称相同被计入受控预算
- 最后一处 DataCodec 对宿主 ThreadPool 的提交改为 `std::async(std::launch::async, ...)` 顶层 driver，块计算仍由请求根执行；该桥接 cpp 已随本次 Windows 核心构建编译。没有恢复 WASM/浏览器调试或声称浏览器运行通过
- 旧 runner/group/TLS、宿主 ThreadPool 和三个实验头注册名在 DataCodec 当前跟踪生产/测试声明调用中无匹配，实验文件本身与 std_compat 工作区状态保持原样
- RelWithDebInfo 核心构建通过，核心 CTest **10/10 通过，17.07 秒**；日志为 logs/datacodec-final-interface-build.log、logs/datacodec-final-interface-ctest.log。最新 All 运行覆盖前一批新增对象用例，命名计数沿用前一批 910/635 的采集结果，没有伪称本批重新采集同一计数。Qt 沿用已有实际窗口通过结果

T13 据此关闭：旧管理接口和重复执行路径已删除，当前 Windows 核心调用方、公开接口契约与集成回放通过。旧资源档位由 6a8c49c01 删除，外部 cache/宿主适配由 42dc0a388 与 1f3375f82 迁移，属性调度、window budget、拓扑估计分别由 b4dfebbae、f1fe63d6f、c8023cb1b 删除；最新构建与负向接口契约验证这些删除后的调用状态，提交标题中的历史“未验证”不代替本节实际结果。

同一优化可执行文件在编译和回归结束后进行 17 tuple、M=256 MiB、P=4、审计开启的性能复测，各配置预热一次、正式一百次，顺序轮换，完整结果回放全部成功：

| 配置 | 成功/失败 | 端到端中位耗时 ms |
| --- | ---: | ---: |
| Fixed 1 | 100/0 | 9.8407 |
| Fixed 2 | 100/0 | 9.7550 |
| Fixed 4 | 100/0 | 9.97825 |
| Adaptive 4 | 100/0 | 11.0386 |

日志为 logs/datacodec-final-interface-small.log。Adaptive 相对最佳 Fixed 额外 **1.2836 ms、13.16%**，10% 目标未达标；此前的一百次为额外 1.1622 ms、11.74%，两批都保留。删除无调用的任务组未改变控制线程模型；本批不能据此声称改善了极小请求性能。已有根生命周期分项显示控制线程创建/销毁具有可测固定成本。达到当前 10% 门槛需要把本批额外成本压到 0.9755 ms 以内，尚差约 0.3081 ms。任何极小请求例外均需用户明确变更验收约定，当前 T14/T16 继续未完成。

### 数值、几何、属性与 reference：13 项

| 编号 | 实际存储 | 当前运行证据 | 证据级别 |
| --- | --- | --- | --- |
| N01 | source 的宿主视图及 provider 引用 | audit.reader-borrowed、owner.remap-reference | D |
| N02 | reader raw/output、orderBlock、getter 暂存 | numeric.reader、audit.reader-order、audit.reader-borrowed | D |
| N03 | 分量 raw、分量编码及块组包 | audit.serial-component-scratch、audit.numeric-owned-payload、numeric.output-release | D |
| N04 | 区域背景、逐层残差及回解缓冲 | regionPrecision.layered-capacity-float32/float64 检查两层三分量回放、背景和残差实际容量峰值及 scratch 归还；truncated-layer 拒绝末层截断 | D |
| N05 | 数值解码输入、分量、残差与最终块 | numericDecode.geometryFlow/attributeFlow、audit.numeric-decode-capacity；regionPrecision.layered-capacity-* 补分层残差回放与真实容量峰值 | D |
| N06 | 字段 region run 索引与块局部 runs | regionPrecision.fieldOwner、sharedCapacity、localIntersection、lastOwner、invalidRuns | D |
| N07 | reference staging、完整候选与 probe | referenceCodec.candidate-capacities、boundedProbe.capacities、blockFlow、flowRelease；同根、独立身份、实际 full/probe 容量、持槽位取样和消费释放 | D |
| N08 | reference 重采样及偏移读取窗口 | referenceCodec.windowedResample | D |
| N09 | 浮点/整数 Wavelet 工作数组及 blob | referenceCodec.2.*.waveletCapacitySamples、referenceCodec.wavelet.int32.capacity、audit.numeric-library-exempt | D |
| N10 | Affine/Predictor delta、候选与小系数 | referenceCodec.1.*.referenceCapacitySamples、referenceCodec.3.*.referenceCapacitySamples | D |
| N11 | 分组评分 sampleIndices、fieldSamples、edges | pipeline.sampledParentSelection 检查采样后 U=0 及索引/三字段样本峰值；sample-capacity-denial-0 至 -4 检查索引、三个样本、首条 edge 容量拒绝、读取次数和失败释放 | D |
| N12 | 完整属性、几何及 decoded reference | attributes.individual-admission、necessary-denied、last-release；geometry.sized-owners、reference-release、last-release | D |
| N13 | 前驱解码 workspace/session 的共享 owner | pipeline.decode-session-prunes-predecessor、command-retired、surviving-read、alias-retains-workspace、final-owner；真实会话清理与最后 alias 释放 | D |

### 重排与拓扑：18 项

| 编号 | 实际存储 | 当前运行证据 | 证据级别 |
| --- | --- | --- | --- |
| T01 | point/cell order 与 point inverse provider | remap.exact-owners、required-capacity、shared-release、last-release | D |
| T02 | Morton 内排 keyed/order/inverse 工作副本 | morton.small-publish 检查 HeavyPhase、driver 取样、keyed/order 实际 capacity 及发布 owner 的精确 U；small-release 验证归零 | D |
| T03 | Morton 可选 keyCache | morton.external-sort 分别检查有/无 key cache 时实际容量及 key 读取 1/2 次；key-failure、inverse-owner-refusal 检查失败清理 | D |
| T04 | Morton 高低桶表、touched、leaves | morton.fixed-table-capacities；五个固定桶表、touched 与单 key 叶描述实际容量，在 HeavyPhase 内取样 | D |
| T05 | Morton 单一高桶 slab | morton.slab-exact、slab-required、slab-replay、slab-and-run-release | D |
| T06 | Morton scratch/ordered/lowBuckets 与读取窗口 | morton.leaf-array-capacities、external-sort；三个数组固定叶容量、跨阈值稳定顺序、inverse 与不超过 1 MiB 的完整记录读取窗口 | D |
| T07 | Morton high run 与输出 provider 并存 | morton.run-exact、run-overflow、run-range、run-release、provider-allocation-failure | D |
| T08 | connectivity 必要连续 point inverse/cell order | topology.remap-exact、remap-direct-read、remap-required、remap-independent、identity-exempt | D |
| T09 | connectivity 输入、类型、阶次及单 cell 暂存 | topology.bounded-flow、polynomial-capacities、format-roundtrip、decode-flow、failure-drain；六组合含非空阶次容量及重排后的逐值回放 | D |
| T10 | topology grammar offsets/events/seed/residual/编码流 | topology.grammar-array-identities；六组合十数组容量取样、时间与独立 scopeId；golden/variable/byteStream 回放 | D |
| T11 | connectivity 块四段输入 owner/借用视图 | topology.range-source、shared-range、range-last-reader、range-release | D |
| T12 | connectivity 解码块与同步 observer 副本 | topology.decode-flow、decode-drain、topologyObserver.consume-before-retire、failure-drain | D |
| T13 | 完整 topology 四个数组 | topology.exact-owners、shared-owner、last-release、partial-rollback、diagnostics.topology-transaction | D |
| T14 | polyhedron visitedFaces 与 localIndexTable | --remap-contract 的 polyhedron.native-required-owner-8/-9、native-cell-work-capacities；逐项确定容量拒绝、阶段释放和两个批次共用 24 字节表 | D |
| T15 | polyhedron 单 cell 工作数组与 overflow | --remap-contract 的 polyhedron.native-cell-work-capacities；五个单 cell 数组实际容量与持槽位取样，包含预留且逻辑为空的 overflow | D |
| T16 | polyhedron 五个流窗口与累计 spool | polyhedron.fixed-stream-window、append.fixed-backend、append.last-release | D |
| T17 | polyhedron 五条完整解码索引 | polyhedron.decode-phase、index-sized/read/release 与 index-capacity-denial-0 至 -4；五条 owner 回放、两后端、逐项容量拒绝、零输入读取与 U/阶段归还 | D |
| T18 | polyhedron emit 批次 offsets/counts/ids | polyhedron.decode-phase 与 --remap-contract 的 native-emit-windows、native-emit-counts、native-final-release；65537 cell 跨批次、完整/尾扫描窗口、offset/local id 实际容量、原生输出与最终释放 | D |

### 字节输入、封装与输出：13 项

| 编号 | 实际存储 | 当前运行证据 | 证据级别 |
| --- | --- | --- | --- |
| I01 | 输入 reader 与 owner+范围 | byteRange.retained-input、retained-subrange、missing-owner | D |
| I02 | 根完整输入预读 | encodedInputCache.exact-windows-last-owner、denied-before-read、necessary-live-owner、read-failure-no-replay、borrowed-root-isolation | D |
| I03 | 原生映射与已准入窗口的 OS 预取提示 | --remap-contract 的 native.mapping-prefetch-admitted-window、mapping-range-lifetime、mapping-last-owner；Windows 接受固定窗口提示、范围寿命及越界，不推断 OS 驻留量 | D |
| I04 | 包解析字段范围及整叶 writer owner | packageIdentity.multi-leaf-*、parsed-ranges-share-output、parsed-ranges-final-release；真实三叶封装、字段范围共享完整输出、最后范围释放 | D |
| I05 | FieldDecodeStream 输入/输出/Skip 窗口 | window.field.* 检查 Raw/Zstd 两后端跨窗口、reader 移动与截断；byteRange.windows/cancel-between-windows、input.cancel-field 补取消边界 | D |
| I06 | 外层 Zstd 解开的完整 Attribute payload | payload.sized-owner/range-replay/raw-borrowed/release/admit-before-read；numericDecode.attributeFlow、input.reopen-field 补按需请求 | D |
| I07 | 数值/reference 字段累计 body store | numeric.output-release、referenceCodec.flowRelease、append.accounting、last-release | D |
| I08 | spooler、bundle、segment 引用 | owner.segment-consumption、shared-reference、last-reference | D |
| I09 | 多叶 fieldBundles/leafPackages/writers | packageIdentity.multi-leaf-shared-input、output-overlap、output-denied、retire-0/-1/-2、denied-cleanup；唯一源容量、完整输出并存与逐叶退休 | D |
| I10 | package Zstd probe、流窗口与累计结果 | output.package-probe-window-0/-1/-2、package-probe-retirement；一次 1 MiB 读取、计算额度、scratch/阶段退休；完整结果沿用 package-compute-units、package-retirement、append.fixed-backend | D |
| I11 | 最终 MemoryByteRangeOutput/EncodedBuffer/Materialize | output.move-owner、allocation-failure、materialize-admission、materialize-owner、failed-growth | D |
| I12 | Session 弱登记与底层唯一 owner | owner.unique-registration、session-reset、root-lifetime、root-last-release | D |
| I13 | 文件 store 与固定复制窗口 | append.fixed-backend、read-after-seal、no-failure-backend-switch、byteRange.windows | D |

### 会话、缓存及元数据：8 项

| 编号 | 实际存储 | 当前运行证据 | 证据级别 |
| --- | --- | --- | --- |
| S01 | 编码跨帧 reference | pipeline.reference-last-consumer、reference-final-frame | D |
| S02 | 解码 leafStates/reference 与 payload 留存 | pipeline.decode-session-*；真实关键帧发布、前驱 state 清理、required 命令退休、外部消费者和最终 workspace alias 释放 | D |
| S03 | 内部 typed LRU 与输入缓存 | frameCache.*、referenceCache.*、encodedInputCache.*；实际验证 LRU、版本、关闭留存、必要 reference、活跃消费者和根隔离 | D |
| S04 | 协调器一个运行/前台待运行/预取 | 独立 TaskCoordinator 的 TestCommandWindowAndOrder、TestDuplicateTargetUsesSingleTask、TestReleasedHandleStillJoinsTaskOnDriver、TestCancelAllDoesNotAllocate；本批 CTest 通过 | D |
| S05 | 帧依赖规划与 reader 引用元数据 | pipeline.large-planning-shares-metadata、large-planning-release；4097 帧、69632 leaf，共享 metadata/reader、零重复读取、顺序与最终释放；已知容量子项和未测部分分别记录 | D |
| S06 | params/layouts/regionLayers/componentLayouts | audit.metadata-outer-arrays、pipeline.sharedSpatialLayoutParams、persist-file-granularity | D |
| S07 | 单次参数字节与跨叶根 owner | output.params-shared-owner、params-session-release、params-final-release | D |
| S08 | BlockRecord、stage、Context 的轻量状态 | execution.ordered-bounded-flow、failure-and-next-request、reject-self-wait、commit-driven-cursor | D |

### 宿主、校验、日志与平台：9 项

| 编号 | 实际存储 | 当前运行证据 | 证据级别 |
| --- | --- | --- | --- |
| H01 | 编码宿主视图 | --remap-contract 的 native.borrowed-input-exempt；原生几何和属性保持 Borrowed 与原始指针，M=0 下无受控申请；拓扑语义沿用同入口回放 | D |
| H02 | 宿主 polyhedron 转换与 synthesizedOffsets | --remap-contract 的 native.converted-face-key-retirement、converted-input-release；实际面 key 峰值和零容量退休、派生 offset 清理后宿主输入保持完整 | D |
| H03 | 宿主解码结果与高阶拓扑暂存 | native.polynomial-storage-exempt、polynomial-storage-retired、polyhedron.native-emit-windows；高阶暂存实际容量、Abort/下一 topology 清理、原生大输出不增加 U | D |
| H04 | prepared surface、frame assembly、模型 registry | All 的 TestPreparedSurfacePointAndCellAttributes：真实 assembly 交付、payload 保留 source、最后 payload/map 释放和 surface 独立存活；Wasm registry 移出验收 | D |
| H05 | finite 校验与签名/误差窗口 | validation.finite-window-4/8-0/1/2、analysis.signature-window-and-summary；float32/64 固定/尾窗口、NaN/读取失败、跨窗口 remap 和三个摘要容量；allocator 峰值未测 | D |
| H06 | remap 分析 provider 共享引用 | analysis.capture-owner、last-owner、read-failure、windowed-precision | D |
| H07 | 有界 telemetry/log/报告与固定首错 | telemetry.active-session-cap、record-and-utf8-cap、take-reclaims-capacity、export.*、failure.persistent-allocation-failure | D |
| H08 | 浏览器 JS ArrayBuffer 与范围读取 | 用户已移除本次浏览器验收；保留现有实现，运行结论为未验证，不计本次待办 | 范围外 |
| H09 | 第三方上下文、显式线程与自有 payload 边界 | worker.compressor-serial-options、compressor-round-trip、audit.numeric-library-exempt、numeric-owned-payload | D |

共用容量事务的直接证据包括 capacity.allocation-rollback、growth-overlap、logical-shrink、state-lifetime，sized.exact-admission、creation-rollback、granted-downsize，reserved.before-downsize、consume-once、wrong-root，以及 diagnostics.exact-rejection、historical-u、known-owner。业务对象的直接传递与生命周期验证按上表单独列出。

当前台账的 61 行包含 60 行当前 Windows 范围内的直接证据及 1 行明确移出范围的浏览器对象，G/U 剩余零。D 表示所列边界已有直接验证，未覆盖的 allocator/宿主内部峰值仍保持未知。旧职责删除已在 T13 关闭，性能和最终交付按 T14/T16 继续收尾，不重新开展无边界的生产源码静态审计。

## 最终门槛证据归并

本表引用已执行的检查和平台实验，便于核对 TODO 末尾的独立门槛。最新生产提交为 ec67c1bf6，业务对象补证据提交为 efb2b87af。本节不扩展平台范围，不重新运行已适用的长程测试。

| 门槛 | 已有证据 | 结论及范围 |
| --- | --- | --- |
| M/C/S 动态发布、零额度、非法目标、先预约后下调 | execution.downsize-live-capacity/downsize-inflight/invalid-target/zero-capacity、reserved.before-downsize、sized.granted-downsize、controller.reference-hard-limit | 当前 Windows 执行根及确定性目标矩阵通过；真实 Job 实验未实测 M 下调 |
| 新旧容量并存、共享和最后释放 | capacity.growth-overlap/logical-shrink/state-lifetime、owner.*、packageIdentity.multi-leaf-*、pipeline.decode-session-final-owner | 共用容量事务与业务 owner 直接验证通过；结果存活时保留真实 U |
| 有界在途、计算后消费、顺序提交与收尾 | execution.ordered-bounded-flow/commit-driven-cursor/completed-output-wait、numeric.bounded-flow、topologyObserver.consume-before-retire、output.package-* | 同根块流、单记录、慢首块、提交和阶段退休通过；旧任务组及同池嵌套入口已删除 |
| 失败、取消、队列析构和 join | failure.persistent-allocation-failure/stage-allocation-cleanup、execution.worker-creation-allocation-failure/failure-and-next-request、TaskCoordinator | 持续分配失败与实际线程创建申请失败均收束；未将其描述为操作系统线程数耗尽测试 |
| 模式、控制轨迹、诊断独立 | controller.* 补充矩阵、ResourceModes、audit.*、export.*、Windows Job 实验 | 两模式共用机制、确定性压力/恢复与实际 Job 响应通过；审计只报告所覆盖的容量，未知项不补估计 |
| 61 行对象及算法边界 | 上述 60 行 D、1 行范围外；float32/64、分量、region/reference、SOA/getter/remap、拓扑与原生测试 | 每个当前平台对象具有所列直接证据；未声称每个 allocator 或宿主内部容器已被覆盖 |
| 格式与固定粒度 | pipeline.fixed-encode-granularity/persist-file-granularity、numeric.fixed-roundtrip、window.field.*、polyhedron.index-*、原生跨批次测试 | 新固定块/旧大块、尾块、外层 Zstd、固定窗口和原生跨批次语义通过 |
| 能力及豁免 | execution.no-threads、sized.no-spill、worker.compressor-*、audit.numeric-library-exempt、native.borrowed-input-exempt | 当前 Windows 原生与内部能力配置通过；实际无 pthread/无外存设备不在本次验收范围 |
| 旧职责删除和接口交付 | T13 的指定删除项、无注入/旧头不可见/CreateGroup 不可用编译契约、核心/IGDC/Filter 构建回归 | 旧模块和重复执行路径已删除；三个实验头保持未跟踪、未接入，std_compat 保持原状态 |
| 性能与完成率 | 四 profile、中长请求、低容量、Windows Job、17 tuple 两批一百次 | 极小 points 仍未达到 10%，保留未完成；其他已列样本按各自记录通过，不把失败耗时算作吞吐收益 |
| Qt 窗口与配置交付 | logs/datacodec-final-ctest.log 的 QtResourceControls、既有主程序构建与窗口加载/清空/持久化用例 | 沿用既有实际窗口和配置通过结果，本批没有重跑 Qt |

T16 已完成上述证据整理，最终完成状态继续等待 T14 的性能目标处理及最终交付记录。当前 goal 保持 active；没有通过缩减门槛或重新解释失败结果将其标为完成。

### 2026-09-10：极小请求分段计时与线程原语对照

直接复用两批已有一百次日志，分别计算编码和解码中位数，没有重新运行编解码计时：

| 批次 | 配置 | 编码中位 ms | 解码中位 ms | 端到端中位 ms |
| --- | --- | ---: | ---: | ---: |
| 首批优化版 | Fixed 2 | 7.8158 | 2.05175 | 9.8957 |
| 首批优化版 | Adaptive 4 | 8.27425 | 2.72945 | 11.0579 |
| 删除旧执行接口后 | Fixed 2 | 7.68915 | 1.9690 | 9.7550 |
| 删除旧执行接口后 | Adaptive 4 | 8.24605 | 2.6862 | 11.03855 |

Fixed 2 为各批实际最佳 Fixed。第二批分段额外耗时分别约 0.5569/0.7172 ms；分段中位数不相加替代端到端中位数。端到端原汇总保留四位小数 11.0386 ms。编码和解码均有固定差额，与此前两次独立资源根创建/销毁的分项实验一致；没有把全部差额精确归因给单个函数。

为判断是否存在简单的 Windows 启动优化，新增独立线程原语实验，源和日志分别为 logs/datacodec-thread-startup-probe.cpp、logs/datacodec-thread-startup-probe.log。同一 UCRT64 Clang、C++20、O2，std::thread 与 `_beginthreadex` 执行同一互斥量/条件变量等待函数，轮换顺序，各预热十次、正式一百次。分别在创建后立即停止、以及主线程暂停 2 ms 后停止两种条件下测量创建与 stop/join：

| 条件 | 原语 | 创建中位 ms | stop/join 中位 ms |
| --- | --- | ---: | ---: |
| 立即停止 | std::thread | 0.2516 | 0.1257 |
| 立即停止 | _beginthreadex | 0.26155 | 0.12585 |
| 请求间隔 2 ms | std::thread | 0.56585 | 0.2992 |
| 请求间隔 2 ms | _beginthreadex | 0.49245 | 0.23775 |

所有线程创建、等待与关闭成功。立即停止条件下没有收益；暂停条件下两个中位差额合计约 0.135 ms。该小型进程没有加载完整编解码模块，2 ms 是 sleep 请求值，不声称是严格的实际调度间隔；两组条件间绝对数值变化也不能解释成编码工作量变化。本实验未验证原生线程替换能满足核心请求仍差约 0.3081 ms 的性能目标，因此没有引入 Windows 专用控制线程后端，没有修改生产实现。

当前保留 10% 目标及 T14/T16 未完成状态。已向用户提出这一极小样本能否作为明确例外的选择；自动续行和等待时长不构成接受例外。进一步调整验收约定须取得明确决定。

### 2026-09-10：真实大型 CGNS 单次测速

本轮按用户要求暂停极小请求例外讨论，优先测试 car_3800W_Fluent_node_HDF5-0001.cgns。新增独立 iGameDataCodecFileBenchmark 目标，复用生产入口；未修改生产编解码实现。Windows UCRT64 Clang RelWithDebInfo 构建通过

设备为 Ryzen 7 7840H、8 核 16 逻辑处理器，Windows 可见物理内存 31.22 GiB。配置为默认 Adaptive、自动线程及存储额度、默认无损数值压缩、全部属性。16 仅表示硬件线程数，本轮未采集动态运行额度轨迹

| 项目 | 实测结果 |
| --- | ---: |
| CGNS 文件大小 | 10,571,784,552 字节 |
| CGNS 载入及读取器后处理 | 33.692 秒 |
| 完整编码并写入文件 | 27.596 秒 |
| 编码文件大小 | 2,204,353,773 字节 |
| 独立进程完整解码 | 8.611 秒 |

原文件大小与输出文件大小之比为 4.796；按 CGNS 文件大小折算编码速度为 383.09 MB/s；按编码文件大小计算解码读取速度为约 256.00 MB/s。不同格式包含不同元数据和表示开销，文件大小比不能视为同一原始字节流的压缩比

输入、解码输出均包含 1 个可编码叶、8,034,973 点、39,494,600 单元和 131 个 Float64 单分量属性，共 1,052,581,463 个属性标量。已比较全部属性名称、类型、tuple 数和分量数，结果一致；未逐元素比较数值及拓扑连接

本轮首次同进程解码实际失败，33.463 秒后返回 `memory pressure did not recover within the request hold deadline`。原因已定位为新增测速入口使用 `auto reader = iGameCGNSReader::New()` 获得裸指针，随后置空没有销毁 reader，原始输入持续被持有。进程私有内存因此仍为 10,917,019,648 字节。这项问题属于测速入口生命周期错误，不构成 DataCodec 自身泄漏的证据

随后新进程解码同一结果成功，退出码为 0、解码结果缓存未命中。新进程启动前可用物理内存为 19,849,084,928 字节，首次编码进程启动前为 5,131,739,136 字节，两次系统内存状态不同。独立解码进程工作集峰值为 12,564,152,320 字节，约 11.70 GiB；这是整进程指标，不是 DataCodec 受控容量

测速为单轮，未清空系统文件缓存，解码读取刚生成的文件。编码时间包括适配器构造和文件关闭，解码时间包括全部属性及宿主结果构造和便捷入口会话清理。两者均不包括额外的规模枚举对比。阶段和消息日志已开启，未开启完整对象容量审计。本轮没有执行完整回归

详细口径与原始日志保留于 logs/datacodec-car-3800w-20260910-results.md、logs/datacodec-car-3800w-20260910.log、logs/datacodec-car-3800w-decode-20260910.log。输出文件保留在原始数据同目录，命名为 car_3800W_datacodec_bench_20260910.igc

### 2026-09-10：测速 reader 所有权修正及同进程复测

外部读取代码改用 `iGameCGNSReader::Pointer`，未修改 DataCodec 内部实现。独立测速目标重新构建通过；同一 CGNS 文件完整编解码退出码为 0，编码 26.972 秒、解码 7.594 秒、CGNS 载入 26.942 秒。输出仍为 2,204,353,773 字节，解码结果缓存未命中，输入输出规模比较通过

释放输入及 reader 后，进程私有内存从 10,915,344,384 字节降至 9,809,920 字节，工作集降至 22,253,568 字节，随后同进程解码成功。该证据关闭本次测速入口保留输入的问题。运行前可用物理内存为 19,294,654,464 字节，未清空文件缓存；此次复测不用于宣称算法性能提升。记录为 logs/datacodec-car-3800w-owned-reader-20260910.log，输出为 car_3800W_datacodec_bench_20260910_owned_reader.igc

本轮未重跑核心回归。T14/T16 的既有极小请求性能门槛保持未完成；大型文件成功不能替代该项验收
