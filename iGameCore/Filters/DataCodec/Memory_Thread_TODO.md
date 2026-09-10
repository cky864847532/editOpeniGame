# DataCodec 资源改造 TODO

实施依据：Memory_Thread_Implementation_Plan.md，2026-09-08 版本

目标：完整实现同一个最终架构，维护逐项状态并及时提交；最后迁移 Qt 编解码窗口

当前范围（2026-09-10 用户最新 GOAL）：重新聚焦 T14，逐项对照 TODO 核对实现及调用接入是否存在、测试案例是否匹配，完成必要的 Windows 核心编译与运行测试，在 doc 子目录新增实测文档。T16 文档整理暂停。只新增实测文档和更新本 TODO，不修改其他既有文档。跨平台、Linux、WASM、浏览器及跨设备验收范围外；Qt 沿用既有适用证据。

当前 T14 已按本轮范围完成，T16 暂停。极小 points +13.16% 继续作为未达到原 10% 目标的已知结果，不恢复围绕比例门槛的优化或重复测速。T00—T13、T15 已完成结果按适用范围复用；本轮通过实际实现入口、测试注册及运行记录重新确认覆盖关系，源码阅读不评价实现质量。

## 状态规则

- `[ ]` 尚未完成，`[-]` 正在实施，`[x]` 已有对应实现和验证证据
- 按下面的依赖顺序推进；共用机制确定后统一迁移调用方，不添加兼容包装、第二套管理器和失败重放
- 一个单元可以分多个提交；单元完成要求其旧调用、专属测试、配置和报告一并迁移
- 测试通过只证明实际覆盖的范围；源码完成、编译通过、测试通过、真实设备性能验证分别记录
- 每个实现提交同步更新此清单；阶段性提交不代表整个 goal 已完成
- 采用最终集中验证：按用户最新要求，先推进全部实现单元的代码改造，再统一编译、测试和修复缺陷。实现期间只做当前改造必需的源码阅读与接口衔接，不做事后静态核查，不运行阶段性构建、回归或性能测试，不反复展开缺陷排查。提交标明“已实现、未验证”，验证任务保持未完成，最终交付前集中完成全部验收

## 实施顺序

### 2026-09-10：T14 按 TODO 重新核对与补测

- 实现入口及调用接入、测试注册和独立门槛已核对，范围止于是否实现及测试是否匹配，不开展实现质量审查
- 初批增量构建通过；核心 CTest 9/9 通过，15.16 秒；coverage 910 次检查、635 名称、零失败和零遗漏。ResourceModes 避免触发极小样本计时而沿用既有证据，Qt 同样沿用
- 多帧独立性能及完成率数据存在缺口，新增 SequenceBenchmark；独立编译的测试头包含缺失和测试 sourceIdentity 缺失已补齐
- 多帧运行暴露无单元 PointSet 首帧误绑定不存在的 topology reference，跳过绑定后又暴露发布阶段缺少完整状态；现通过 InitializeEmpty 形成无数组的完整空拓扑，接入原提交/发布，并补充点集完整发布及非空拓扑缺失引用仍失败的回归
- 修复后集中构建成功，最终核心 CTest 9/9 通过，16.53 秒；coverage 913 次检查、637 名称、零失败和零遗漏，连续运行命令退出码 0。日志为 logs/datacodec-t14-20260910-empty-topology-build.log、datacodec-t14-20260910-final-ctest.log 和 datacodec-t14-20260910-final-coverage.log
- 新多帧实测 Fixed 1/2/4、Adaptive 4 每配置预热一次和正式三次全部成功；每轮六帧编码/解码及逐值对比通过、reference 完成记录 4 条、结果缓存命中 0、输出 598248 B。六帧编码加解码总计时的正式中位数依次为 569.9813/485.8357/449.1720/432.9283 ms，日志为 logs/datacodec-t14-20260910-sequence-final.log
- 新报告为 doc/T14_Windows_Test_Report_20260910.md，记录具体方法、参数、范围、数据和日志；不更新其他既有文档，T16 暂停

### 2026-09-10：大型 CGNS 测速及测试入口修正

- 默认 Adaptive、无损、全部 131 属性，10.57 GB CGNS 完整编码 27.596 秒，独立进程全量解码 8.611 秒，输出 2.20 GB
- 首次同进程解码超时已定位为新增测速入口的 CGNS reader 裸指针未销毁；外部读取改用项目智能指针，DataCodec 内部仍约定使用 C++ 标准库智能指针
- 修正后同进程复测通过，编码 26.972 秒、解码 7.594 秒、输入输出规模一致；释放原始输入后私有内存降至约 9.36 MiB。没有修改生产编解码逻辑，没有重跑全套回归
- 测速入口问题已关闭，T14/T16 既有极小请求性能验收仍未关闭。详细结果和适用边界见 Memory_Thread_Validation.md 的同日记录

### 2026-09-10：T13 旧执行接口删除，通过

- 针对方案列明的删除清单，当前跟踪源码中十九个旧资源符号无匹配；九类旧头文件不再跟踪，PipelineContracts 的不可包含/无注入字段编译契约已通过上一批构建
- 补查旧任务组接口发现 IParallelTaskRunner/IParallelTaskGroup、CreateGroup、ParallelForChunks、Inline runner 仍在源码中，业务调用方为零；现删除这条重复任务记录队列、空 runner 分支、无用 TLS worker index 和 context 前向声明，DataCodecExecutionResources 保留直接的有界终端执行接口
- PipelineContracts 补充 CreateGroup 不可用的编译契约；三个未跟踪实验头保持原状且未接入构建/注册
- 原有 bridge 自己持有的 executionResources/cacheRuntime 属于内部根生命周期，继续保留；保留用途限定为容量查询或诊断的 ResidentSizeHint，不将同名数据方法一律删除
- 顶层异步 bridge 的宿主 ThreadPool 提交改为显式 async driver，删除最后一处 DataCodec 对 iGameThreadPool 的引用；仅随 Windows 核心构建验证这段通用 C++ 桥接代码，不恢复 WASM/浏览器调试测试
- RelWithDebInfo 集中构建与核心 CTest 10/10 通过，17.07 秒；新增 CreateGroup 不可用编译契约通过。相关日志为 logs/datacodec-final-interface-build.log、logs/datacodec-final-interface-ctest.log
- 极小请求同条件各一百次全部回放成功，Fixed 1/2/4 分别为 9.8407/9.755/9.97825 ms，Adaptive 为 11.0386 ms，+13.16% 未达标。日志为 logs/datacodec-final-interface-small.log；不以重复运行寻找过线结果，不修改既定门槛
- T13 关闭；T14/T16 保持未完成。Qt、Windows Job 与其他性能样本沿用既有适用证据，控制器状态机和目标参数未修改

### 2026-09-09：五项对象证据补齐，通过

- grammar 十数组在六种组合中验证容量取样及独立身份；package probe 验证一次 1 MiB 读取、计算额度、可压缩/高熵/读取失败和全部临时资源退休
- 4097 帧、69632 leaf 的规划验证共享引用、零重复读取、依赖顺序和最终释放；已知对象/leaf 数组 6619256 B，结果数组 49152 B，耗时 5.2892 ms，未测部分明确留空
- float32/64 finite 固定/尾窗口、NaN 与读取失败，跨 1 MiB remap 签名与三个摘要容量通过；原生 assembly/payload/prepared source/face map/surface 生命周期由 All 验证
- 优化构建通过，--resource-coverage 910 次检查、635 名称、失败零、遗漏零；核心 CTest 10/10 通过，13.86 秒。首次错误测试名前缀匹配零项不计为成功，修正为 DataCodec 前缀并启用 --no-tests=error 后完成实际回归
- T07/T09/T12 关闭；T13/T14/T16 保持进行中。生产源码未修改，Qt/Windows Job/代表性性能沿用适用的既有结果

### 2026-09-09：业务 owner、前驱会话与原生边界，通过

- reference full/probe 同根且使用独立容量身份；Morton 五个桶表、三个叶工作数组、touched/叶描述逐项实测；connectivity 六组合补非空阶次容量与重排回放
- 三叶封装验证唯一共享输入、完整输出并存、少一字节时读入前拒绝、逐叶释放和解析字段范围最终 owner；真实 DecodeSession 验证前驱 state 退休、命令 required lease 释放、外部读取与最后 workspace alias 释放
- 原生 polyhedron 的 65537 cell 跨编码/emit 批次，visitedFaces/localIndexTable 逐项拒绝、单 cell 五数组实际容量、固定 1 MiB face-count 扫描、宿主输出豁免与最终释放均通过
- 原生 M=0 旁验证宿主借用视图、polyhedron 面表转换 key/offset 清理与高阶暂存退休；Windows 映射预取接受、范围引用寿命和越界拒绝通过，未把预取接受解释为页面驻留保证
- 首轮 Morton 测试误期望两个叶描述，按相同 key 的一个描述/分窗口输出修正；独立包含生产 polyhedron 编码头暴露缺失直接依赖，已补齐必要包含并复测
- RelWithDebInfo 核心构建、--resource-coverage（888 次、621 名称、失败零、遗漏零）和 --remap-contract 全部通过。沿用其他已通过结果，未重跑全部 CTest 或性能矩阵
- T06/T08 关闭；剩余六项及五个对象级 G 项继续推进，极小 points 的 +11.74% 保持未达标

### 2026-09-09：业务容量与窗口补测，通过

- 将原 DataCodecFeatureBudget 中的窗口 reader、范围复制、Raw/Zstd 字段流、完整属性 payload 和五条索引用例显式接入当前 SelfTest；原入口此前没有调用方，其源码不构成当前版本运行证据
- 属性 payload 用例迁到当前 PrepareLeafPackageFieldPayload 和请求作用域，检查 Memory/File、完整长度、固定窗口、Raw 借用及容量拒绝发生在读取前
- 区域精度用例补 float32/64、三分量、两层不同精度的背景和残差容量取样、逐元素误差及最后残差截断拒绝
- polyhedron 解码在第零到第四个 owner 的容量边界分别拒绝，核对历史申请与并存量、零输入读取、未发布结果和阶段/容量全部归还
- 区域容量首次检查误将最新取样与最大层比较，改为取样峰值后通过；优化构建及覆盖入口 856 次检查、598 个不同名称、失败零、遗漏零。没有更改生产审计实现
- 验证台账 N04/N05/T17/I05/I06 已补直接证据；T06—T09 的其他对象缺项继续保持未完成

### 2026-09-09：控制器与执行设施失败补测，通过

- 复用 ExecutionMechanism 增加低水位振荡期限、中间水位、冷却退避上限、仅槽位增长、重复样本、P=1/2/4/8/16/64 连续观察批、M/C 增长互斥、重型阶段类型交接、新请求 M 边界和完成时压力测试
- 使用真实容量 lease 验证 600 MiB reference 加 64 MiB 新需求可共存，512 MiB 硬上限下调后保留真实 U 并拒绝下一笔增长；此用例不实际分配 600 MiB 数据
- 通过现有分配拒绝入口覆盖 worker 创建的真实失败，保持拒绝至队列锁外析构、请求收束和 join；验证仅一次分配被拒绝、任务未运行、原根关闭且不重放
- 优化构建通过，ExecutionMechanism 1/1 通过，2.82 秒；复用上一批核心回归和 Windows Job 结果，T04/T11 共用机制验收关闭，业务数组与性能门槛按各自单元继续收尾

### 2026-09-09：Windows 核心收尾批次

- 四类新性能样本全部回放成功：many-fields、correlated-fields、variable-topology、morton 的 Adaptive 相对最佳 Fixed 分别为 +5.27%、-1.86%、+1.65%、+1.92%。变长拓扑测试 adapter 的类型拒绝已修正并复测；原极小 points 的 +11.74% 继续保留未达标
- --resource-coverage 接入现有输入预读、完整帧 LRU 和必要 reference 测试。输入缓存原用例错误地在必要容量拒绝后继续同一请求，现按首错终止契约收束，检查外部消费者存活与独立后续请求复用；并接入普通 All 回归
- reference 分组评分五种确定容量拒绝全部通过：索引、三个字段样本以及首条实际候选边；分配前读取次数、未发布 schedule、全部根 owner 释放均符合预期
- 优化构建通过；最新覆盖采集 770 次检查、547 个不同名称、失败零、遗漏零；核心 CTest 10/10 通过，14.50 秒，Qt 沿用既有结果
- T05/T10 完成证据汇总见验证文档：公开配置/入口/同根绑定，以及 scratch/typed LRU/输入预读/必要 reference/有限命令窗口已覆盖。T04 的失败注入完整性、T11 的其余确定性轨迹和业务对象清单继续收尾

### 2026-09-09：用户暂停 WASM 调试

- 按用户最新要求，暂停一切 WASM/浏览器工具链查找、调试、构建和验收；先前的 Emscripten 位置询问无需继续答复
- 保留已实现的 WASM 接口迁移代码，相关运行证据标记为暂停、未验证，不计作完成，也不列为暂停期间的执行任务
- 当前后台任务是 WSL Ubuntu 的 Linux 原生核心构建，继续归原生平台验收；Windows 新增四类性能 profile 的测试目标已链接，运行测量待编译负载结束

### 2026-09-09：代表性性能样本扩展，待集中执行

- 既有 --resource-performance 增加可选 profile：variable-topology 使用交替三/四连接的单元；many-fields 使用 128 个标量字段且明确关闭 reference；correlated-fields 使用八个线性相关标量字段及既有 Auto reference；morton 增加唯一原始点号并启用 point Morton
- 所有配置仍包含完整内存编码与解码；新增拓扑连接/offset 比对，Morton 按随同重排的唯一点号核对置换、几何与全部字段。数据构造和回放检查位于计时区间外，原 points 参数与测量基准保留
- 同一批完成上述测试入口后再增量构建，不因单个 profile 反复编译；Linux 核心构建继续进行，性能计时等待编译负载结束

### 2026-09-09：实际断言覆盖与 Linux 配置

- 增加显式 --resource-coverage，复用核心 SelfTest，采集实际执行的 Require 名称和成败；固定 16384 条记录，采集期不分配、不格式化，结束后按名称汇总，遗漏明确计数并使该入口失败
- 普通测试与生产运行不安装观察器；本入口用于归并实际验证证据，不以重新静态扫描生产源码代替运行验证，待集中执行
- 使用已存在的 WSL Ubuntu、CMake 3.28.3、GCC 13.3、Ninja 1.11.1 配置 Linux 核心测试，Qt 与 CGNS 等可选目标关闭，等待依赖配置结果
- Windows 增量构建及 --resource-coverage 已通过：746 次实际断言、523 个不同名称、失败零、记录遗漏零。61 行对象已在验证文档逐行关联直接/共用/缺失运行证据；没有把共用测试通过等同于每个业务申请点均已验证
- Linux 配置已通过，核心测试构建进行中；T02/T03 的共用失败和容量事务完成证据已齐备，状态更新为完成。其余业务模块仍按逐对象及平台缺项保持验收状态

### 2026-09-09：优化版本及 Windows Job 压力实测

- RelWithDebInfo 核心构建通过，核心 CTest 10/10 通过（18.69 秒），覆盖最新原生回调同步与公共资源接口编译契约；本目录 Qt 未重建，窗口证据沿用上一批
- 优化版中等/长请求 Adaptive 相对最佳 Fixed 为 +6.92%/+2.51%；17 tuple 正式一百次仍为 +11.74%（额外 1.1622 ms），保持未达标，未调整门槛
- --resource-environment 已通过：真实 Windows Job 进程剩余额度降到约 95 MiB，准入关闭、HOLD 排空，C/S 从 4/5 收缩到 2/3；释放后恢复，冷却结束升到 3/4，顺序提交和收束成功。实验使用测试宿主内存及固定延迟工作，不代表全机压力通知或算法吞吐验收
- 实际结果和实验边界已补入验证文档；继续保留小请求性能、61 项证据归并、代表性算法/多设备及浏览器等验收缺项

### 2026-09-09：固定成本测量与优化构建验收

- 新增显式 --resource-overhead 入口，分别测量设备探测、原生压力 monitor 与空请求根生命周期，每项预热一次、正式一百次，计时内不格式化输出；设备探测中位数 0.0091 ms，压力 monitor 三项均低于 0.004 ms，Adaptive 根创建/销毁分别为 0.42335/0.16465 ms
- 同一执行状态与互斥量内分开 worker/driver 和控制线程条件变量：NORMAL 的普通块事件只累计观察，定时采样读取；压力、DRAIN/HOLD 进展与请求生命周期即时唤醒。无变化决策不额外唤醒 worker/driver，原生压力信号与等待条件在同一锁下同步
- 增加 Adaptive 取消后复用根的真实终端工作回归；机制与模式用例通过（3.34 秒）。最新原生通知锁同步随优化构建一并验收
- 复用现有 RelWithDebInfo 目录，Clang、-O2 -g -DNDEBUG、Ninja、LLD，正在构建核心测试；性能计时避开编译负载
- PipelineContracts 新增旧资源模块头文件不可见、公开请求无外部执行/cache 字段及启动参数类型的编译契约；覆盖旧职责删除的明确接口边界，三个保留实验头不参与这一删除契约
- 增加独立 --resource-environment 实验：Windows Job 只限制当前测试进程，测试宿主最多占用 512 MiB 后释放，直接观察真实平台采样、压力准入暂停、HOLD 排空及恢复；采用固定延迟终端工作，不将本项描述为算法吞吐或全机低内存通知验证，待优化构建后执行

### 2026-09-09：当前实施批次结束，进入最终集中验证

- 当前已记录的 T02—T13、T15 改造代码，以及 T14 模式/性能/Qt 验收入口已写入；自 cbaedd074 之后未做阶段性编译、测试或事后静态核查
- 从本记录开始执行最终集中构建、测试和缺陷修复；不再以单个小项为单位重复长程测试，编译错误按同类接口批量修复后继续构建
- 逐对象覆盖、真实平台性能、Qt 实际窗口与最终清理仍需验证证据；原有状态不提前标为完成。后续发现的缺口归入本次集中验收修复

### 2026-09-09：集中构建与首次回归

- 健康启动上界八的最终构建通过，完整 CTest 11/11 通过（26.72 秒）；十六线程长样本 Adaptive 705.232 ms、最佳 Fixed 650.160 ms，差异约 8.5%。M=32 MiB 同一大样本成功，M=1 MiB 明确容量拒绝，均已记录在验证文档
- 本批仍未完成：极小请求自动管理成本、优化构建及真实外部压力、多设备与实际浏览器证据、61 项清单完整证据归并。上述缺项保持未完成，不把当前回归全绿等同于整个 goal 完成

- 十六线程上限实测显示四计算启动不足，健康启动上界进一步校准为八，方案与共用常量同步修改；Fixed 8/16 的 638.037/624.842 ms 支持这一有限上界，具体数据见验证记录。小请求二十次仍有约 2.64 ms 自动管理成本，保留未完成项

- 健康启动按本机实测校准为 C=min(P,4)、S=C+1，方案同步约定，压力后收缩及冷却保持原机制；新增 P=1/2/4/8/16 启动/类型边界与压力历史回归，ExecutionMechanism、ResourceModes、QtResourceControls 三项通过（8.54 秒）
- 四线程上限的中、大、长性能样本全部成功且进入 10% 目标，长样本 932.098/889.443 ms；17 tuple 仍有约 2.2 ms 成本，继续短样本重复及十六线程上限测量。Qt 真实窗口已验证字段载入启用、保存新参数和清空模型禁用

- 集中构建/回归修复已提交 179a6efef。8388608 tuple 长请求继续暴露 Adaptive 增长受阻，已定位 OrderedCommit 将尚无结果的首块计算等待误计为 Wo；改用既有完成/提交水位确认真实未消费结果，补写队列与提交等待分类回归，待集中复测

- Playback 用例迁移后，最终 All 通过（15.09 秒），包含 prepared surface 等后续宿主集成；十一项注册 CTest 已在本次集中回归及失败范围复测中分别通过。Qt 控件/真实无模型窗口已通过，实际主程序已链接；设备及自动性能验收继续进行

- 最新共用循环修复通过 ExecutionMechanism（0.20 秒），All 已通过 polyhedron/surface remap、progress、完整帧缓存、reference 缓存及任务协调器；继续处理 Playback 旧宿主缓存断言，用例改为检查内部 LRU 与宿主无重复留存
- 本机 float32 小/中/大样本、报告开关性能矩阵全部回放成功；1048576 tuple 的 Adaptive 387.745 ms、最佳 Fixed 179.572 ms，未达到 10% 目标。具体条件与数据见 Memory_Thread_Validation.md，自动策略性能继续验收，不标为完成

- 后续回归已达到 10/11；核心 self test、报告文件及 surface remap 已通过，All 的 polyhedron 最终提交失败已定位：RunOrderedBlocks 在 commit 推进游标后继续使用旧 hasMore，额外接纳空块。共用循环改为提交后重新采样供给及工作类型，补写线程/无线程的提交驱动游标回归
- Polyhedron 完成阶段增加确切 cell 数校验及根首错，保留算法完成失败的原始原因；NumericDecodeExecution 补齐属性 payload 顺序，强制 reference 失败用例绑定真实根并断言首错
- iGameVis 主程序已完成一次链接；Qt 用例继续增加真实编码窗口的无模型禁用检查。此处新增修复等待本批增量构建与回归
- 性能点集明确使用 Original 点序以逐值核对，宿主重排正确性由 remap 契约验证；性能测量参数固定为本机、float32 点集、Fixed 1/2/4 线程与 Adaptive 上限 4、M=256 MiB，预热一次与正式三次，初始允许差异沿用方案 10%，结果单独记录

- 加载已核实的环境脚本后，沿用 debug Ninja/UCRT64 Clang/LLD；集中修复迁移留下的 compressorState 参数声明、旧报告字段、prefetch 开关、错误枚举、局部变量、测试调用及 Qt 重排快照前置声明
- 核心库、iGameDataCodecTests、Qt_module 和 iGameDataCodecQtResourceTests 已完成构建链接；Windows Job MIN_MAX_RATE 按既有 ABI 读取高十六位，兼容当前 MinGW 结构声明
- 首次完整 CTest 为 8/11 通过，耗时 20.04 秒；FailureContract、StorageOwnership、MortonResources、TopologyExecution、NumericExecution、TaskCoordinator、ResourceModes、QtResourceControls 通过
- NumericDecodeExecution 的无线程测试配置误用 S=4 导致异常，All 同处中断；已改为无线程 C=S=1、线程平台 C=2/S=4，并同步运行时恢复和最终断言
- ExecutionMechanism 检出相同目标的 MechanismCheck 仍追加 Wake 事件；已删除这一无变化操作的事件，自动状态转换的独立原因记录保留。两项修复正等待增量构建及失败范围复测，未记录为通过

### 2026-09-09：T14/T15 Qt 资源控件与配置验收源码，已实现、未验证

- Qt 已启用且 DataCodec tests 打开时注册 QtResourceControls，使用既有 Qt Widgets 的 offscreen 平台，不引入 QtTest 依赖
- 验证两个不可编辑枚举项、optional 设备默认、显式零容量、线程与 MiB 设置、模式切换保留上限、程序加载不触发编辑通知、父控件禁用传播与大容量换算
- 通过临时 INI 文件验证重新读取、默认值删除显式 key；测试 Apply 将新资源参数发布给下一次解码请求，不修改用户持久化配置
- 控件用例的编写不代表真实编码窗口或主窗口交互已经验证；本次未事后静态核查、编译、运行测试或启动 GUI

### 2026-09-09：T14 模式回放与真实计时入口，已实现、未验证

- 复用现有 iGameDataCodecTests 增加 --resource-performance，显式传 tuple 数、重复次数、M 的 MiB、线程上限与审计开关；不增加生产调度接口或独立测试程序
- 同一确定 float32 标量/向量数据依次执行 Fixed 小/中/高并发和 Adaptive 完整内存编码及解码；每次完整比较几何与属性，首轮预热单列，后续轮换配置顺序，成功样本计算中位数，失败及首错原因单列
- 报告启动设备信息、请求上限、每次编解码耗时、压缩字节、两个阶段各自最大根数组事件峰值及三个进程 RSS 取样；未知项留空，不把 RSS 取样称为真实峰值，不将多个根或 encode/decode 峰值相加
- 增加短 ResourceModes CTest 覆盖两模式公开接口回放；性能矩阵仅经显式命令启动，不随普通 CTest 自动运行，避免每次回归重复长程实验
- 本入口当前覆盖原生 float32 点集、完整内存输出与公开模式配置；float64、拓扑、reference、旧大块、动态额度和取消继续由已写的专项用例覆盖，真实设备压力与性能结论仍待最终集中验收。未事后静态核查、编译或执行测试

### 2026-09-09：T03/T07/T08/T09 申请点必要 owner 诊断，已实现、未验证

- 定长 topology 四数组、polyhedron 五个索引在逐项创建时附带当前已成功取得的必要 owner；完整事务成功后发布，后续拒绝保留历史诊断并销毁未发布目标
- Morton slab 申请附带已知 key cache，high run 附带 key/slab，order/inverse 附带当前 run 与已取得的 order；工厂只传递非持有的固定容量描述，不扫描 session 登记表
- 完整帧输出与 SegmentedBinaryObject 连续化在申请前记录仍存活的直接输入 owner；MemoryStore 增长将自身旧数组与申请点描述合并，继续采用一次容量预约
- KnownStorageOwners 只识别实际 MemoryStore、按地址去重，最多保留十七项以触发现有十六项拒绝记录的截断标志；文件、借用和未知包装不填估算，描述不持共享引用、不进入审计合计或调度
- CapacityDiagnostics 补写无分配去重、完整输出拒绝不消费输入、拓扑事务回滚；MortonResources 补写 inverse 拒绝发生于 key 计算前及 order 历史容量。仅写源码，未事后静态核查、编译或执行测试

### 2026-09-09：T12 资源报告口径收尾，已实现、未验证

- 删除编解码 pipeline 的模块 resident/reserved/peak_reserved、scratch 累计申请及计数冒充字节的重复报告出口；沿用同根实际数组事件与空闲 scratch capacity，控制状态继续由固定根诊断提供
- 删除结束阶段为生成报告而遍历 session 弱登记对象的调用，以及宿主 NativeResidentBytesHint 的物理占用报告；adapter 明确自有数组仍按已有容量样本单独导出
- RSS 的 peakWorkingSetBytes 全链改为 sampledPeakWorkingSetBytes，JSON 标明 process 与 stage-boundary-snapshots；当前 workingSetBytes 使用实际结束样本，逻辑字节单列且不进入物理内存或容量合计
- 属性 reference remap 复用固定容量 callback，删除剩余标量资源 callback 与 EncodeStageCallbacks 文件；报告筛选不再凭 peak_/resident_limit 名字猜测数据口径
- OutputSinks 同步迁移采样字段，StorageAudit 补写逻辑字节与物理占用分离、RSS 取样范围及最大值语义；本次仅实施，未事后静态核查，未编译或运行测试

### 2026-09-09：T13/T15 Qt 资源窗口与启动参数，已实现、未验证

- 编码窗口与解码设置共用一个资源控件，枚举用 Fixed/Adaptive 下拉框，直接编辑计算线程与自有存储上限；设备默认保持 optional 未指定，零自有容量保持独立语义，不在 UI 推算设备额度或拆分模块比例
- 删除编码/解码 tier、切换档位重置 Zstd、缓存帧数配置；Zstd 等级、压缩增强、关键帧间隔、按需属性、缓存启用和日志作为各自独立选项保留
- 配置持久化只记录模式和显式启动上限；IGDC reader、独立按需属性 source 从现有 IOSettings 取得相同资源参数，运行中的 session 保持创建时配置，UI 不调用动态调节机制
- 编码 writer 在启动前接收资源参数；编码面板无数据时资源控件继续禁用，解码设置新增共用三行控件并调整窗口最小尺寸；三个 Codec 示例删除旧 tier 初始化
- Qt 报告配置、目录/时间戳初始化、报告格式化与文件导出接入可选异常边界，生成的编码结果保留并提示诊断缺失；明确请求的 remap 精度分析初始化失败在编码启动前结束
- telemetry capture 增加移动交接，Qt worker 取得唯一 capture 状态，底层 sink/provider 继续使用已有共享 owner；本次仅实施，未事后静态核查，未编译、测试或打开 GUI，完整交互与性能验收保持未完成

### 2026-09-09：T12/T13 可选输出初始化边界，已实现、未验证

- 输出绑定分别隔离默认 console/progress、dispatcher 与 router 创建异常；未创建成功的可选设施不发布，已有下游 sink 保持有效，局部固定标志报告诊断缺失
- telemetry capture 延后创建 dispatcher，可选 session capture 的初始化失败不影响编解码；明确请求的 remap capture 按必要分析契约传播失败，不返回伪造的恒等顺序
- IGDC 的报告目录、时间戳与导出采用独立可选边界，报告不依赖非空 sink；Reader/Writer 增加只读 DiagnosticsIncomplete，Wasm 汇入 capture 的初始化诊断，错误文本的留存改用已有有界容器
- Progress feature 补写持续分配拒绝时可选设施关闭、已有下游可用、明确分析失败和后续消息交付源码；本次仅实施，未事后静态核查，未编译或运行测试

### 2026-09-09：T09/T10/T13 完整输入与封装 owner，已实现、未验证

- 删除 MemoryByteRangeReader 的裸 span/vector 复制构造器，内存输入显式接收宿主共享 owner，DataCodec 自有编码结果使用 EncodedBuffer；非空视图缺少 owner 直接拒绝
- IGDC 的内存入口与 FileIO::ReadIGCFromMemory 要求真实共享 owner，保留到按需读取 session；Wasm 宿主接收的 string 移动到共享 owner，桥接层不再隐式复制整份压缩输入，其他格式的 FileReader 行为保持原有契约
- 删除 LeafPackageIO::ReadFromMemory 的逐字段完整复制入口，统一使用 shared reader 与字段范围；FramePackageIO 完整叶 writer 接收携带容量凭证的 EncodedBuffer，每次最多写出 1 MiB
- PackageIdentity、DecodeReferenceCache 和畸形输入用例迁移所有权接口；ByteRange 补写无复制地址、范围延寿、缺少 owner 拒绝及禁止隐式构造断言。两个 VTK 示例入口已使用共享 EncodedBuffer，沿用已有传递
- 本次仅实施，未做事后静态核查，未编译或运行测试；生命周期回放和容量释放验证统一留到最终验收

### 2026-09-09：T12/T13 prepared surface 寿命与可选摘要，已实现、未验证

- prepared surface 构建器改成独占 owner，Begin 替换旧构建状态、块失败/异常、End 不完整时移出并在锁外销毁，成功 Attach 只交接一次；宿主构造数据继续豁免根容量
- 同步 observer 明确校验 blockIndex、cellOffset、固定单元规格和累计 cell 数，失败后不继续接纳，重新 Begin 初始化新的请求状态
- 附着成功后的摘要格式化异常只标记 diagnosticsIncomplete，Wasm 的阶段摘要、surface 摘要及 cache 文本也在独立可选导出边界内；诊断失败不清除已经生成的宿主结果
- PreparedSurfaceAttributes 增加乱序拒绝、失败后重新 Begin 和重复 Attach 拒绝测试源码；本次仅实施，未做事后静态核查，未编译或运行测试

### 2026-09-09：T07/T12 Morton 公共实现与工作区取样，已实现、未验证

- 实施所需阅读发现 MortonRemapBuilder 当前缺失公共定义和辅助函数前半部，并留存两组旧新入口；从 cbaedd074 取回辅助算法文本，与当前重型准入、终端执行和可选容量拒绝记录组合成一个生产入口，删除重复实现，不回退资源架构
- high run 在 driver 创建确定 slab/run 后按固定范围交给终端工作；低桶计数、读取、叶排序和 order/inverse 写回复用 HeavyPhaseLease，分段之间检查停止，终端工作不提交同池子任务
- 内排 keyed/order、高低桶表、slice 表、scratch/ordered/lowBuckets、touched、leaves 和实际 run 窗口逐对象取 capacity；根已覆盖的 slab/key/run/provider 不再通过逻辑长度重复报告
- 删除 Morton resourceCallback 与合计/名义预算字节输出；point/cell/polyhedron remap 参数链传递同一个容量回调，driver 导出固定样本，进度与样本消费者异常只记录诊断缺失
- MortonResources 迁移旧回调测试，补写 driver/阶段凭证、内排数组、slice 表和固定读取窗口取样；slab/key 使用根实际数组事件观察，必要 provider 失败在计算前注入
- 本次仅实施，未做事后静态核查，未编译或运行测试；本次文本重组的完整编译、排序回放、故障与取消证据统一留到最终验证

### 2026-09-09：T06/T12 数值 reader 重排窗口取样，已实现、未验证

- NumericArrayReader 的完整 tuple 与单分量读取接收可选且不留存的 order 样本，局部 orderBlock 在实际 ReadRange 后取 capacity；直接视图记录该自有数组为零，宿主源和既有 order 视图不另计容量
- 普通与 reference 块在 driver 读入前建立固定样本，worker 继承同一身份并补充计算数据；current/reference 的读取索引分别记录，全部样本仍由原槽位中的 driver 顺序导出
- GetterOnly 的单 tuple D 维暂存遵守小对象豁免，源 provider 的长期容量仍由原 owner 持有；本次不增加 reader 字节预约或共享可变采集状态
- StorageAudit 补写重排 tuple 回放、局部索引容量和直接借用零容量断言，NumericExecution 补写 GetterOnly 无 orderBlock 断言；本次仅实施，未做事后静态核查，未编译或运行测试

### 2026-09-09：T07/T08/T12 polyhedron 工作数组与窗口取样，已实现、未验证

- polyhedron 固定样本清单覆盖编码 cell 的 unique/face/local 数组及 overflow 查找数组，五个真实 1 MiB 流窗口在初始化后单独交付；删除工作容量合计和 ResidentSizeHint 报告入口
- 五流解码为四个 varint 步骤、计数校验与 local-id 步骤分别保留样本身份；读取窗口、输出窗口、unique/cellFace/face 计数数组按真实 capacity 记录，删除 peakRead/peakCounts/peakBatch 标量传递
- emit 批次分别取样三个 offsets、两组 ids、两组 cell counts 和 face 扫描窗口；driver 同步交付宿主后导出，容量回调贯穿 CommitTopologyCache 的保留/释放两条调用路径
- 五个完整解码索引 owner、全域 visited/table 沿用根容量；本次没有增加槽位内逐笔预约，样本不进入调度，格式及五流先后顺序保持原有协议
- TopologyExecution 补写终端解码后的 driver/阶段凭证、六组独立样本、emit 数组容量和编码固定流窗口断言；本次仅实施，未做事后静态核查，未编译或运行测试

### 2026-09-09：T07/T08/T12 connectivity 全块工作数组取样，已实现、未验证

- 既有 ConnectivityTopologyTypes 集中定义固定 25 项容量样本；编码读入阶段记录 connectivity、cellSizes、types、orders 和逐 cell scratch，worker 把同一身份交给 artifact 并补充 grammar 与四段编码流
- grammar 分别记录 offsets、mainEvents/symbols、seed 字节、residual 和各编码 lane 的真实 vector capacity，固定字母表和库内部继续豁免；删除输入、输出合计形式的 memoryCheckpoint 接口
- 解码从四段输入 owner 取样，备用读取副本单列，借用 span 不重复收费；原始输出、cellSizes、offsets 及提交时的 1 MiB adjusted offsets 分别保存真实采集时刻
- 编解码阶段统一接入容量回调，由 driver 在原槽位内交付，异常只记录诊断缺失；TopologyExecution 迁移容量回调、运行降额/恢复和提交取消，并补写 grammar/借用输入/解码窗口取样断言
- 本次仅实施，未做事后静态核查，未编译或运行测试；polyhedron、Morton 与数值 reader 内部数组继续推进

### 2026-09-09：T06/T08/T12 整数 Wavelet 与参考候选取样，已实现、未验证

- 整数 Wavelet 编码分别记录 current/reference 的 low/high uint64 数组、复用 residual 和编码 blob；解码记录 reference、残差、重建数组及原始结果的实际 capacity，奇数尾项小对象按清单豁免
- Affine/Predictor 的 prepared delta 与编码候选单列固定样本，ReferenceBlockIO 将块样本传入普通数值分量编解码；D 维系数和第三方内部继续豁免
- 完整 ordinary/reference 候选分别记录真实自有结果；BoundedProbe 使用独立样本身份记录输入、准备残差和候选，允许它们与完整候选并存，收益估计仅用于原有算法选择
- 块 driver 在原槽位中分别交付完整与探测样本，导出失败仅记诊断缺失；整数无损用例和浮点精度矩阵补写相应容量断言
- 本次仅实施，未做事后静态核查，未编译或运行测试；拓扑、Morton 和数值 reader 内部工作区继续推进

### 2026-09-09：T06/T08/T12 浮点 Wavelet 与 reference 块取样，已实现、未验证

- ReferenceTransferCacheBuilder 的每块输出接入同一固定样本，geometry/attribute reference 调用传递可选回调；读入的 current/reference 与 predictor staging 分别按实际自有 scratch capacity 采集，driver 在原槽位内交付
- ReferenceCodec encode/decode 输入只借用当前块样本指针，不留存跨请求状态；geometry/attribute 解码将样本传到具体 Wavelet codec
- 浮点 Wavelet 编码分别记录带对齐空间的 low/high delta scratch 和自有结果；解码逐个记录 low/high blob、reference component、reference low/high、low/high delta、重建 low/high 和 reconstructed 的 vector capacity
- 删除旧 waveletPeakTemporaryDoubleBytes 字段及报告传递，使用逐对象的取样时刻与取样峰值；旧字段源于部分 double 数组同点容量合计，未覆盖全部临时量，不能作为整个 Wavelet 内存峰值
- 新样本保持固定清单与无分配采集，库拥有的压缩 blob 仍未知且豁免；整数 Wavelet、Affine/Predictor 内部残差数组及候选分支的精细取样继续推进
- ReferenceCodecTestHarness 可接收编解码样本，既有 float32/float64 精度矩阵补充 lowDelta 对齐容量、reference component 与 reconstructed 独立容量断言；本次仅实施，未做事后静态核查，未编译或运行测试，格式兼容与失败注入统一留到最终验证

### 2026-09-09：T08/T12 数值解码块的输入、转换与参考取样，已实现、未验证

- 固定块样本清单增加 encoded input、decoded component、geometry float 转换和三个 reference staging 类别，原有编码样本按同一枚举取值；关闭审计时块不创建样本
- geometry 每块以本地 params 副本传入样本指针，避免并行 worker 修改共享参数；取样当前输入 scratch、普通分量解码数组、reference staging、最终 raw 与实际存在的转换数组
- attribute 普通/区域残差解码传入同一块样本，reference 分支分别取样 intra、resampled、shifted 的自有 scratch；借用 reference 指针不另计容量
- 两种解码的 driver 在有序提交期间导出固定样本，回调失败只增加根诊断缺失；阶段接入共用 MakeCapacityRecordCallback，删除仅供编码使用的重复回调工厂
- 采集时刻随样本保存，离开 worker 后的导出不把历史取样伪装为当前实际存活合计；事件峰值继续留空，审计不参与根容量和槽位控制
- StorageAudit 补写三分量整数完整编码回放、分量解码 capacity 以及未使用 reference 样本保持未知的源码；本次仅实施，未做事后静态核查，未编译或运行测试；Wavelet/残差算法内部的详细数组与格式兼容测试集中留到后续实施和最终验收

### 2026-09-09：T06/T12 普通数值与区域残差块取样，已实现、未验证

- 普通数值块删除所有分量 raw scratch 的并存数组，分量循环复用一个确定大小的 scratch；区域残差同样复用一个 component raw，编码结果保留到当前块封装完成
- NumericArrayEncodedBytes 对自有 vector 暴露真实容量，对 Pressio owner 返回未知；分量编码结果只累计当前确实存活的自有数组，库内部占用保持豁免
- 每个需要审计的 BlockOutput 内嵌固定八项样本，覆盖 raw、component raw、自有分量 payload、base decoded、residual raw/decoded、bundle 暂存和最终 payload；采样保存实际时刻与取样峰值，不推算重分配事件峰值，不作为同时峰值合计
- worker 只填写固定样本，driver 在原槽位的有序提交内交付现有报告链；geometry/attribute 普通路径均传递同一可选容量回调，关闭 ResourceUsage 不创建每块样本
- 新容量回调抛异常只记录根诊断缺失，不改变块计算结果；NumericExecution 补充 driver/槽位交付断言，StorageAudit 补充库结果豁免、自有容量与有效长度区分、三分量只申请一个 raw scratch 的测试源码
- 本次仅实施，未做事后静态核查，未编译或运行测试；reference/Wavelet 及解码内部数组、连接与重排工作区取样继续推进

### 2026-09-09：T09/T12 参数字节与布局元数据取样，已实现、未验证

- 编码单次序列化成功后、解码参数流读入后，分别对当前临时字节 vector 的实际 capacity 取样；样本只说明采集时刻，未观察的重新分配瞬时峰值留空
- 参数元数据准备完成时分别记录 geometry/topology 的 blockLayouts 外层数组、attrParams 外层数组，以及同一时刻所有属性 blockLayouts 外层数组的容量；内部 string、componentLayouts、regionLayers 的额外分配不推算
- 所有取样复用 BufferCapacitySample 与既有可选报告导出链，关闭 ResourceUsage 时不读取容量，16 MiB 格式检查与根容量准入保持各自职责
- StorageAudit 补写实际外层 capacity、多个属性同时存活数组、嵌套成员不估计、事件峰值留空测试源码；本次仅实施，未做事后静态核查，未编译或运行测试；块内算法数组与 reference builder 的单独元数据解析继续在 T12 推进

### 2026-09-09：T12/T13 可选报告写出与消费者异常边界，已实现、未验证

- IDataCodecReportFileSink 增加 TryWriteReportFile，将调用点的报告准备和序列化纳入同一异常边界，分别记录 Preparation/Write 固定分类和失败计数；false 返回与异常都说明诊断不完整，异常分支不构造错误字符串，不重放写出
- DataCodecOutputRouter 分别隔离 UI、控制台及进度消费者异常，继续交付后续消费者；报告返回失败也增加既有 sink 诊断计数，保留可查询的固定失败类别，文件实现显式 close 后判断结果
- IRunRecordSink 增加调用点 TryExport；IGDCReader 的报告配置构造、运行中报告和完成后的报告/控制台处理接入该边界，文件输出接入 TryWriteReportFile，报告错误消息采用原有固定条目留存
- 叶级内存跟踪结束时的 Summary 复制、阶段进入/退出的字符串构造、拓扑进度格式化及资源回调放入既有 RunRecordEmitter::TryExport，不把可选格式化失败变为编解码失败
- DiagnosticExport 补写准备时分配失败不调用 writer、writer 抛异常不重放、false 返回计数、后续导出、UI 失败后控制台继续和路由错误可查询测试源码
- 本次仅实施，未做事后静态核查，未编译或运行测试；Qt 报告入口随最后窗口迁移，日志设施创建和其余格式化调用点继续按原 T12 推进

### 2026-09-09：T12/T13 宿主适配明确数组取样与退休，已实现、未验证

- 共用 BufferCapacitySample 只保存明确数组的当前 capacity、取样时刻和取样最大值，未取样字段保持未知；每个样本具备独立 scope，不持有数组，不影响根容量和任务准入
- iGame 编码 adapter 分别取样两个自有 offsets、去重临时 key 和 map 中 key vector 的真实 capacity；插入时只累计新 key 的已知容量，map 节点与原生面表容量继续未知，不扫描全表计算估计占用
- 去重结束及失败展开时销毁全部 key 数组；ReleaseConvertedInputs 释放派生面表并交换释放两个 offsets 的留存容量，保留独立取样峰值供叶报告读取
- 解码 adapter 单列 polynomial orders 的 vector capacity，在原生高阶拓扑构建消费完成及 Abort/Reset 时释放；删除将此临时量加到宿主原生结果逻辑量的记录
- 两种 adapter 的只读样本由现有编解码报告链通过 TryExport 导出，使用 SampledBuffer 口径，未知项省略、零值保留，多个样本峰值不合计
- StorageAudit 补充 capacity 与 size 区分、clear 后留存、交换后归零、未知省略、独立 scope、无分配取样及报告取样峰值测试源码；本次仅实施，未做事后静态核查，未编译或运行测试；其他块局部数组审计、宿主 observer 清理和最终综合验证继续推进

### 2026-09-09：T10/T13 解码会话共享元数据，已实现、未验证

- Playback 在 OpenSequence 一次构造只读 frame identity map，所有帧 DecodeSession 共享同一元数据 owner；删除为每个解码帧按值复制整张身份表的路径，Reset 只解除当前引用
- DecodeSession 的 ConfigureReferences 从所属内部根取得 reference cache，删除接收 cache 实例的配置参数；会话留存身份元数据，不引入外部缓存注入
- 活跃帧只保留 frameIndex、temporal role 和 key-frame index 五个必要标量，删除整个 FramePackage 的长期副本；分支按借用指针排序，向宿主提交原始描述，删除分支字符串和记录的排序副本
- 后续按需属性读取保留自己的 LeafDecodeState 与输入 owner，身份表在最后会话引用释放时销毁；本次未做事后静态核查，未编译或运行测试

### 2026-09-09：T08/T11 混合块工作类型边界，已实现、未验证

- RunOrderedBlocks 增加读取前的类型描述回调，只保存当前类型与下一块 key；类型改变时暂停新读入，已有记录完成顺序提交及退休后调用同一 SetWorkType，再取得新槽位
- 类型排空期间停止把后继类型视为当前观察批的独立供给，保留同一 flow 的连续提交序号；Fixed 保持额度，Adaptive 复用现有类型切换与冷却规则
- NumericDecodeCursor 按实际 layout 的 bytesCodec、普通/Affine/Wavelet/Predictor/LayeredResidual、referenceKind、分量 codec 组合、标量和分量数构造 key；普通尾块沿用固定规格，旧大块取实际粒度
- 几何与属性解码删除字段级粗略类型设置，接入 cursor 的块级 key；普通编码区分启用区域残差的算法路径，reference 编码继续按事前确定的候选算法路径准入，不依据编码完成后的收益选择重放块
- 新增线程/顺序平台的 A/A/B/B/A 类型交接、旧输入退休、提交连续性、描述次数、Fixed 额度保持与 metadata key 测试源码；未做事后静态核查，未编译或运行测试

### 2026-09-09：T03/T12 MemoryStore 实际数组审计，已实现、未验证

- ResidentByteBudget 既有共享 state 增加独立的实际数组事件区；MemoryStore 的确定数组通过 move-only AllocatedArray 持有，成功分配才登记，真实析构才归还，结果离开根后继续追踪至最后引用释放
- 分配/删除及对应审计更新在独立短锁内完成，复制旧数组内容在锁外；并行 owner 的实际事件按同一顺序记录，扩容保留 old+new 峰值，分配失败只增加失败计数并归还预约
- 容量许可 U/M 与 StorageAllocationSnapshot 分开；控制器、TryReserve 和缓存准入均不读取实际容量审计，审计没有额外 owner 注册表、线程或资源预测
- 根固定快照采用执行锁、容量锁、数组审计锁的 try_lock 顺序，新增 scopeId、实际 live/peak/count，输出消费阶段能够保留当前状态和独立峰值
- TelemetryResourceUsage 新增可选 tracked/eventPeak/sampledPeak 字段及明确 coverage；编解码报告接入根数组与空闲 scratch 两种独立观测，保留零值，未知峰值留空，重复快照不加总
- 新增预约尚未分配、实际数组登记、8+12 扩容峰值、真实分配失败无虚假事件、共享引用、根销毁后结果和报告重复快照测试源码；未做事后静态核查，未编译或运行测试
- T12 的块局部可变 vector/宿主指定数组精确取样及剩余失真统计迁移继续实施，未将根数组审计视为全部覆盖完成

### 2026-09-09：T08/T09/T13 读取窗口取消传递，已实现、未验证

- 包头检查、帧 metadata 的标量/字符串读取、叶 metadata 读取显式接收本次 token，PackageDecodeWorkflow 传入根停止令牌；所有元数据范围读取使用共用 1 MiB 窗口
- FieldDecodeStreamReader 在 Open/OpenRaw 取得本请求 token，raw 和 Zstd 输入窗口在 I/O 返回后检查取消，解压推进前检查停止；移动携带 token，Release 清理，长期 source 不保存旧请求的取消状态
- 数值块 encoded payload 的每个窗口在前后检查根停止状态，旧大块不在取消后继续读完整段；IByteSource 新 ReadCancellable 复用既有源，不分配额外完整输入
- 新增包头读中取消、停止后元数据不触发源读取、字段窗口读中取消、同一 source 在下一独立请求恢复读取及尾窗口测试源码；未做事后静态核查，未编译或运行测试

### 2026-09-09：T09/T13 浏览器范围读取，已实现、未验证

- 删除 WasmBrowserFileByteRangeReader 的 JS 预取函数、按 fileId 保留的 Uint8Array map、512 MiB 局部上限、统计和桥接报告分支；PrefetchRange 固定声明 Unavailable，完整自有输入预读继续由根的 EncodedInputCacheLoader 决定
- 浏览器 ReadAt 对大 span 顺序读取至多 1 MiB 的子范围；JS 入口验证窗口长度、精确源范围和复制时的线性内存边界，复制完成后清除本次 Uint8Array 引用；浏览器 GC 时机不纳入根容量承诺
- IByteRangeReader 增加不留存 token 的 ReadAtCancellable 共用窗口读取，窗口之间及最后 I/O 返回后检查取消，EncodedInputCacheLoader 已接入；浏览器读统计 observer 异常独立计数，真实 I/O 异常直接返回失败
- ByteRange 测试源码补充两个完整窗口及尾窗口、目标偏移、取消前不调用源、子窗口间取消和首个 I/O 失败不重放；未做事后静态核查，未编译或执行测试
- 其余解码范围调用点的逐窗口 token 传递、Wasm 实际浏览器验证及 Examples 性能接口迁移继续实施，T09/T13 未标记完成

### 2026-09-09：T12 可选记录交付异常隔离，已实现、未验证

- IRunRecordSink 增加无抛出的 TrySubmit 和固定原子失败计数；Dispatcher 锁外逐个交付已有引用，删除每条消息创建消费者 vector 的临时分配，单个 sink 失败后继续其余交付
- RunRecordEmitter 隔离记录构造、本地化、内部消息留存和 sink 导出的异常；Context 在可选导出范围内初始化记录信息，根通过独立 diagnosticsIncomplete/diagnosticExportFailures 保留缺失事实，不发布控制事件，不改变首错与目标
- ProgressRange 转发与直接消息提交接入同一交付入口；TelemetrySessionSink 的记录留存/快照复制失败不抛出，已有 session 保持，Take/Snapshot 与报告 capture.exportFailures 说明不完整覆盖
- 新增单消费者抛异常后继续交付、所有分配失败时格式化缺失、资源状态保持、后续完成通知、留存与快照失败计数的测试源码；未做事后静态核查，未编译或执行测试
- T12 其余阶段调用点外部格式化、报告文件导出、精确容量审计与必要并存 owner 上下文继续实施，未标记整项完成

### 2026-09-09：T03/T04/T12 容量拒绝与等待上下文，已实现、未验证

- ResidentByteBudget 在同一锁内决定最小所需容量与允许的扩容量；删除 MemoryStore 先读取剩余额度再独立拒绝的路径，拒绝记录保留当次 A/U/M 与单调时刻
- 容量 state 分配 ownerId，lease 与 MemoryStore 沿用同一 ID；固定用途、标签和静态文件/行号随 owner 保存，共享引用不增加身份，诊断不持强引用
- 扩容拒绝附带已有旧数组的确定容量；定长工厂接收调用点明确给出的并存 owner 描述，最多保留 16 项并标明截断，无全局 owner 注册表或 workspace 扫描
- 根保留固定拒绝上下文、请求/块流身份和终止性标记；必要容量拒绝先形成无分配的首错记录，范围后端选择与可选预取/key cache 拒绝仅留诊断，不停止运行或发布控制通知
- 只读快照分别标记当前取样与历史拒绝时刻；等待上下文携带对应 admission 的块身份/重型阶段、实时完成状态和预期唤醒事件，块读入/提交、终端工作、任务组与请求收束接入
- 新增拒绝 A/U/M、旧新增长、16 项截断、无分配失败交付、共享引用不增加、请求边界与等待身份测试源码；未做事后静态核查，未编译或执行测试
- T12 保持进行中：其余必要并存申请点的 owner 上下文、实际容量独立审计和可选导出异常隔离继续实施

### 2026-09-09：T12 有界遥测留存与文本复制，已实现、未验证

- TelemetrySessionSink 固定保留最多 256 个 session、合计 4096 条消息/阶段/资源/工件明细；session 满时淘汰最早已完成项，全部活跃时省略新 session，结束记录不重建已省略项
- TakeSession/TakeUnscopedMessages 归还明细条目空间，Snapshot 不在 sink 内保存副本；完成状态独立更新，不受明细满额影响
- 共用 TelemetryTextCopy 在复制前将所有字符串合计限制为 2048 UTF-8 字节并保持字符边界，消息参数最多 32 项，未知大小文本不先完整复制再缩短
- 增加 omittedSessions/omittedRecords/truncatedText 统计、message.textTruncated 和报告 capture 覆盖字段；节点明确标注 sink 累计快照口径，不将多节点计数相加
- RunRecordEmitter 内置消息留存、帧/序列/Playback/Filter 的消息汇总改为同一 4096 条限制；Playback 后台继续保留 64 条并使用同一文本规则，固定首错仍走独立失败字段
- Telemetry 测试源码覆盖 257 个活跃 session、已省略结束通知、完成项淘汰、4096 条满额、中文字符边界、两次 Snapshot、Take 回收、emitter 与 JSON 覆盖标记；本次未做事后静态核查，未编译或运行测试
- T12 的精确审计、可选导出异常隔离与固定拒绝/等待诊断继续实施，未标记整项完成

### 2026-09-09：T12 remap 分析共享 owner，已实现、未验证

- RunRemapOrderRecord 从发布处接收 RemapOrderSource::Handle，RemapOrderCapture 直接保留共享的 const provider；删除全量 ReadRange 快照和读取失败后清空 order 的路径
- AdapterSignatureOrderSet 复用 RemapOrderSnapshot，签名与精度分析使用同一个 SignatureOrderWindow，固定数组最多 1 MiB，尾窗口按剩余索引数读取；provider 尺寸不符、非法索引与读取错误明确成为分析失败
- 分析快照延长已有 store/lease 的寿命，根线程和会话登记结束后仍可读取，最后分析引用释放时归还原容量；不新建容量账本或可选缓存
- Filter round-trip oracle 与 remap 测试调用迁移到 provider owner；新增内存/文件 owner、捕获零读取、根销毁后回读、跨窗口与尾部、第二窗口失败、最后引用释放测试源码
- 本次未做事后静态核查，未编译或运行测试；T12 的独立审计、有界日志及固定拒绝/等待诊断继续实施

### 2026-09-09：T09 跨叶参数 owner 与单次序列化，已实现、未验证

- ParamsEncodeStage 在重型阶段序列化一次，按实际序列化长度创建 Ranged owner，按最多 1 MiB 填充并 Seal 后发布；短期序列化 vector 随本阶段结束释放
- SerializeCodecStorageParams 在复制结果到 vector 前执行同一 16 MiB 格式边界，超限直接失败；编码不会主动生成超过现有解码参数边界的字段
- BuildEncodedLeafFieldBundle 移交已发布 params owner，删除再次序列化；LeafPackage 接收共享范围 source，删除从参数 span 创建全量 VectorByteSource 副本
- 删除 workspace/transfer set 的 PublishTransferCacheBytes 包装；完整参数结果通过已有 PublishTransferCache 传递，长期存储保持唯一容量许可
- Frame package 开始压缩前解除 bundle 的重复字段引用，逐字段替换 source 后可释放已消费原字段；Raw 模式保留 LeafPackage 中同一 owner
- OutputResources 测试源码新增内存/文件参数 owner 共享、session 弱登记释放后回读与最后引用归还容量；本次未做事后静态核查，未编译或运行测试，T09 其他输入/封装清单余项继续实施

### 2026-09-09：T05/T10 帧参数借用与编码 reference 退休，已实现、未验证

- 序列入口只在没有业务配置时创建一次默认配置，帧执行器删除冗余完整副本，FrameEncodeState 保存只读参数指针；直接 workspace 无参数入口独占默认配置，移动时指针地址保持有效
- 调用方 regionRuns 随只读配置借用，字段级排序工作副本继续由已接入根容量的 NumericEncodeRegionState 唯一持有，帧/叶不重复复制整个参数树
- 固定业务配置的序列在调用帧执行器前明确下一帧 attribute/geometry reference 需求；每叶编码完成后解除当前消费者，并在封装前删除没有后续依赖的该叶 reference
- 未提前给出下一帧业务配置的逐帧调用保留当前必要 reference，下一次 PrepareFrame 按真实计划回收；最终帧明确没有后续依赖，禁止凭当前配置猜测未来变化
- reference ByteStoreSession 在叶结束 Reset 清理弱登记并解除运行绑定，下一个叶准备时绑定同根；必要 owner 的共享寿命不受登记清理影响
- PipelineContracts 测试源码增加业务配置同地址借用、真实 reference 数组跨帧回读、最后消费者结束后容量归零、最终帧无后续依赖；本次未做事后静态核查，未编译或运行测试

### 2026-09-09：T04/T10 worker 数值压缩器寿命，已实现、未验证

- 删除按配置无限增长的 ThreadLocalCompressorCache 与 Windows Clang 进程寿命裸指针；NumericArrayCompressorState 由 WorkerContext 惰性独占，每个 worker 只保留最近一个配置实例，配置变化先释放旧实例
- 工作线程在退出前通过局部 WorkerContext 析构释放句柄；无线程根在关闭收束后显式释放唯一 inline worker 句柄，库内部内存继续豁免
- 普通数值、区域残差、Affine/Predictor/Wavelet reference、reference 试编解码与 temporal predictor 评分显式传递同一 compressor state；独立底层调用使用本次调用的局部 state，不创建资源 TLS
- ConfigureCompressor 跳过 double 表中的线程选项，最后按 uint32/bool 固定 pressio:nthreads=1、sz3:openmp=false；每次复用前重设当前精度与线程选项
- 新增线程/无线程根重复创建销毁、交替配置、编码解码复用与线程选项不可被 double 表覆盖的测试源码；本次未做事后静态核查，未编译或运行测试，Windows Clang 析构与既有文件兼容性保留到最终验证

| 状态 | 编号 | 修改单元及完成条件 | 依据 |
| --- | --- | --- | --- |
| [x] | T00 | 核查 fork 远端，fetch 后对 libpressio/SZ3 执行 reset --hard 并核对 HEAD；保留其他工作区状态 | 用户前置要求 |
| [x] | T01 | 将最终方案纳入版本控制，建立顺序 TODO、逐对象覆盖与验收台账 | 1、17 |
| [x] | T02 | 共用固定失败记录与入口失败交付；FailureContract 及实际 failure.* 断言已通过，包含 14 次 UTF-8 边界与持续分配失败 | 16.5 |
| [x] | T03 | ResidentByteBudget lease、MemoryStore 事务、固定后端、唯一 owner 与 EncodedBuffer；StorageOwnership 及 capacity/sized/reserved/owner/output 命名断言已通过，业务逐申请点归 T06—T13 | 7、8 |
| [x] | T04 | 自有线程池、唯一根状态、Slot/HeavyPhase/compute lease、运行期额度、有限任务窗口、取消/收束、WorkerContext 和固定诊断；ExecutionMechanism 及真实 worker 创建分配失败用例已通过 | 4—6、13、16.1/16.4 |
| [x] | T05 | Fixed/Adaptive 启动配置与内部入口；外部执行/cache 字段删除编译契约、ResourceModes、同根 owner/scratch/workspace 绑定与帧 session 契约已通过 | 3、4、17.2 |
| [x] | T06 | 数值/几何/属性/reference 编码三步块流、分量串行、固定粒度；字段 run 索引、分组评分、窗口重采样及候选 full/probe 同根容量均有直接证据；删除属性 quota/lane | 6、7.8.2、9 |
| [x] | T07 | connectivity 有界 artifacts/真实连接计数/十个 grammar 数组、Morton slab/provider 与固定工作数组、polyhedron 必要 owner/五流/跨批次已具备直接证据 | 7.8.3、10 |
| [x] | T08 | 数值/拓扑解码统一块流、同步 observer、旧大块单记录、整属性 payload 范围 owner、真实前驱 workspace 退休及原生 polyhedron 跨批次 emit/索引释放已验证 | 7.8.2—4、11 |
| [x] | T09 | 输入与封装生命周期、1 MiB 读写/probe、整叶/帧追加与完整连续结果；共享引用、容量拒绝、固定后端和最后释放直接验证通过 | 7.8.4、8.3、10.3 |
| [x] | T10 | 根 scratch 有限留存与失败代际、输入/完整帧/reference 内部缓存、必要消费者保护、有限命令窗口及取消后复用；本批命名缓存用例、TaskCoordinator 和 All 已通过 | 7.8.5、8.2、12 |
| [x] | T11 | ResourceProbe、Adaptive 控制器与共用模式机制；完整确定性补充矩阵、观察批/冷却/硬能力、无线程能力配置及 Windows Job 压力恢复已通过，性能独立归 T14 | 14、15、17.4 |
| [x] | T12 | 低内存收束、固定首错/拒绝诊断、独立精确审计、有界日志、remap owner、宿主及库内部豁免；prepared assembly/finite/签名窗口补测通过，未测 allocator/宿主峰值保持未知 | 7.8.6、16 |
| [x] | T13 | Filter/IGDC/Examples/桥接接口迁移及旧资源模块删除；无外部注入/CreateGroup 编译契约通过，旧任务组/inline/TLS 与宿主池提交残留已删除，Windows 构建及核心回归通过；浏览器执行验收移出范围 | 17.2 |
| [x] | T14 | TODO 实现接入与测试覆盖已核对；多帧补测及空拓扑点集修复通过，最终核心 CTest 9/9、913 次断言零失败；新增 doc 实测报告已记录方法、边界和数据；极小 points 原比例目标未达标，按决定停止优化及复测 | 用户最新 goal |
| [x] | T15 | Qt 编解码窗口已采用 Fixed/Adaptive、启动线程/自有容量上限；主程序构建通过，真实编码窗口加载/清空字段、设置持久化及解码配置交付的 Qt 用例通过 | 用户追加要求 |
| [-] | T16 | 暂停既有文档整理，不属于本轮完成门槛；本轮只交付 T14 核对及实测报告 | 用户最新 goal |

T02 先实现固定记录和根创建前的入口边界，根内唯一首错在 T04 接入，Context 和业务传播在 T05—T12 随调用链迁移。每个阶段直接使用最终类型，不创建临时错误体系。T03 的私有上限发布权限随 T04 接通，预算不会保留公开 Reset/SetLimit。

### 2026-09-09：T08 属性解码依赖准备与有界块流，已实现、未验证

- 删除属性 cost/aging ready queue、独立 lane 和字段任务组；driver 按拓扑依赖顺序完成前驱准备，字段内复用 NumericDecodeCursor 与 RunOrderedBlocks，计算任务只执行终端块工作
- 完整属性目标在 HeavyPhase 内 Begin，最后块写入与 End 完成后归还槽位；未完成字段由局部 guard 释放，旧大块沿用根单记录准入
- 属性外层输入共用 PrepareLeafPackageFieldPayload：可随机读取的原始源共享借用，Zstd 在重型阶段取得定长范围 owner 后用计算额度解压，读写窗口最多 1 MiB；owner 随本次属性请求释放
- 按需 temporal reference 使用同一准备入口与范围解码，前驱准备发生在目标 Begin 和块准入前；块本地计时由 driver 合并
- 删除 attributeDecodeLanes、setter、preset、报告及旧调度计时字段；新增属性普通块和旧大块回放、截断输入拒绝与 owner 清理测试源码
- 本次未做事后静态核查，未编译或运行测试；T08 保持进行中，拓扑块执行与 adapter 提交继续实施

### 2026-09-09：T08/T13 属性结果串行提交，已实现、未验证

- CommitAttributeCacheFields 在重型阶段逐字段执行 Begin、固定窗口写入和 End，删除全字段提交记录列表、并发写入任务、独立错误锁与提交 runner 参数
- 删除 attributeCommitLanes 的配置、setter、preset、报告与旧比较断言；删除 SupportsConcurrentAttributeRangeWrites 的公共协议以及 iGame、VTK 实现，调用方与开发者说明同步迁移
- 共用 ReplayTypedDecodedCache 在每个窗口检查根停止状态，取消后停止继续读写；adapter-backed 字段沿用宿主存储并由 driver 完成 End
- 本次未做事后静态核查，未编译或运行测试；T08 的拓扑路径继续实施，统一验证阶段再验证提交顺序、取消及宿主错误交付

### 2026-09-09：T08 connectivity 解码块流与同步 observer，已实现、未验证

- 删除 TopologyDecode 的 blockState、条件变量、专属任务组和输出写锁；读入、终端解码、顺序提交统一接入 RunOrderedBlocks，外层顺序解压通过原槽位的 RunTerminalWork 取得计算额度
- ConnectivityTopologyDecodeInputReader 直接持有本块四段输入数组，删除完整块临时文件与二次连续化、input Memory/Managed 模式及局部字节上限；已知块范围、总字节和输出数组地址空间在读入前校验
- worker 返回本块 connectivity、offsets、types、orders，driver 以最多 1 MiB 范围写入完整目标，offsets 用有限窗口加全局基址；observer 由 driver 顺序同步接收移交的块数组
- 完整拓扑目标在 HeavyPhase 创建，最后槽位内完成目标 Seal 与 observer End；局部 guard 在失败时释放本次未交付目标，observer End 按一次结束处理
- 旧大 cell 块沿用单记录准入；删除 topologyBlockLanes、输入模式与输入额度的配置、preset、报告和旧断言，Wasm 旧 lane 调用同步删除
- TopologyExecution 新增实际编码数据的解码流、读入计算额度、顺序同步 observer、observer 拒绝与记录收束测试源码；本次未做事后静态核查，未编译或运行测试

### 2026-09-09：T08 polyhedron 五流解码与固定 cell 提交，已实现、未验证

- DecodePolyhedronTopologyStreamsToCache 在 HeavyPhase 内由 driver 先创建五个完整索引目标，终端工作只填充已有范围、校验计数并解码 local IDs，最后 Seal 后发布完整 cache
- PolyhedronTopologyStreamByteReader 使用最大 1 MiB 的流内读取窗口，varint 与 bitpack 状态跨窗口延续，取消检查放在读窗口及输出批次边界
- EmitPolyhedronCacheToAdapter 按最多 65536 cells 的单记录流执行；worker 按当前批次真实计数建立 offsets/ids，面计数使用固定窗口扫描，删除下一 cell 全量预读及字节阈值批次逻辑
- adapter Begin 在重型阶段执行，最后批次槽位内完成 End；driver 同步提交宿主批次，失败清理 End 只调用一次，完整五流 owner 在 emit 与必要 reference 消费结束后释放
- TopologyExecution 增加五流目标准备、计算额度、正常/截断输入清理及单批次索引还原测试源码；本次未做事后静态核查，未编译或运行测试
- polyhedron 编码的 visitedFaces/localIndexTable 与固定 cell 批次继续在 T07 实施

### 2026-09-09：T07 polyhedron 编码必要数组与固定窗口，已实现、未验证

- visitedFaces 按实际 faceCount 取得唯一 Contiguous owner，重型阶段终端校验后释放；每个 cell 累加实际面顶点引用数量，共享面继续计入对应 cell，返回是否需要全局局部编号表
- localIndexTable 按确定点域长度×sizeof(IndexType) 创建必要连续 owner，driver 先取得容量再提交初始化；批次工作只借用此表，overflow 和单 cell 工作数组随当前批次销毁
- cell 编码复用 RunOrderedBlocks 的固定 65536 cell 单记录流，保持五流连续状态；最后批次内完成 Finish、CompleteStream、Seal、布局与字段结果发布
- PolyhedronTopologyStreamEncoder 的五个缓冲各精确持有 1 MiB 数组，varint 用最多 10 字节栈临时量分段追加，local-ID 尾位只在 Finish 补齐；新增跨窗口 varint 字节与固定容量测试源码
- 删除 TopologyWorkBudget 模块及 polyhedron work-budget 估算记录，删除 PolyhedronTopologySchedule 与旧资源参数传递；TopoStage 直接传同根执行资源
- 本次未做事后静态核查，未编译或运行测试；T07 的 Morton driver 分段与计算额度继续实施

### 2026-09-09：T07 Morton 重型阶段与终端计算，已实现、未验证

- Morton 主流程取得一个 HeavyPhase，driver 创建必要 slab/run/order/inverse owner；小规模排序先准入输出 provider，再提交固定内排范围的终端计算
- key 统计与 high run 写入按固定 65536 元素提交终端工作；低位计数及 run 扫描每个固定 I/O 窗口取得计算额度，leaf 分组和排序独立提交，worker 不启动同池子任务
- order 按最多 1 MiB 顺序写入，inverse 随固定元素批次通过终端工作随机填充；EndWrite/EndRandomWrite 与结果发布保持在重型阶段内
- 点坐标范围扫描、cell topology 校验与 polyhedron remap source 准备接入同根重型/终端入口；point key 读取失败立即记录根首错并停止后续处理
- MortonResources 测试源码迁移请求 scope 与重型凭证，新增小规模 key 计算时的阶段和计算额度断言；终端异常按根失败结果验证
- 本次未做事后静态核查，未编译或运行测试；全路径取消频率、全部调用方衔接及设备性能在最终验证阶段集中检查

### 2026-09-09：T06 编码完整 reference 写回，已实现、未验证

- 几何与属性的当前 reference 写回先取得 HeavyPhase，再按确定完整形状 Begin；每个固定 I/O 窗口的读取、类型转换与 remap getter 通过同根终端工作取得计算额度
- driver 将窗口写入已准入目标，完成 End 后保留 reference owner；失败 guard 释放未完成字段，后续依赖只能消费完整 reference
- 属性 reference 完成后才发布字段记录与 metadata；删除已经串行的当前 reference 写回 mutex、context 字段及传递
- 本次未做事后静态核查，未编译或运行测试；共享 reference 的最后依赖释放继续在 T10 会话与缓存迁移中收尾

### 2026-09-09：T10 LRU 条目与逐项释放，已实现、未验证

- LruCacheIndex 只维护条目上限与访问顺序，删除缓存独立字节额度、逻辑大小推算和峰值字节报告；零条目明确拒绝可选留存
- encoded input、decoded frame、reference 三类缓存提供 TrimOne，每次只解除一个缓存引用，在锁外销毁 owner；活跃消费者继续持有数据，不以共享引用数阻止缓存淘汰
- Playback 的 Configure 调用、Wasm 统计报告与缓存测试源码同步迁移；增加大 size hint 不参与接纳、零条目拒绝、trim 后活跃消费者可读和最后引用释放用例
- 根缓存归属、统一压力留存政策、完整输入 owner、外接接口删除与有限 Playback 请求继续实施；本次未做事后静态核查，未编译或运行测试

### 2026-09-09：T05/T10 根缓存与完整输入准入，已实现、未验证

- DataCodecExecutionResources 唯一持有 DecodeCacheRuntime；删除进程静态 runtime、Playback 及 Filter 请求中的外接缓存/runtime 字段、默认/外接分支与 StreamingFrameCacheAdapter
- 公共 frame payload、lease、查询协议迁入 DecodedFrameTypes；完整输入改为共享 reader owner，管理类型只保留内部具体类，删除 IDecodedFrameCache 与 IEncodedInputCache 管理接口
- 完整输入 loader 使用真实 ByteSize 取得唯一许可，获准后分配 MemoryStore 并按最多 1 MiB 填充；已有连续输入共享 owner，准入未获准使用原 reader，已启动的分配/读取/Seal 失败记录根首错
- 同源加载按来源去重，删除外部 cache 地址 key；合并等待接入根 stop token，成功与异常均发布一次完成通知，结果引用由消费者和可选缓存共同持有
- driver 在阶段与块循环边界消费 trim epoch，逐项锁外释放 encoded input、decoded frame、reference 可选引用；必要容量入口按真实 A/U/M 回收，缓存不再次收费，压力政策统一向根查询
- Playback 使用根内固定两帧/一输入的缓存；宿主 StreamingData 不再建立重复完整帧缓存。单结果 DataObject bridge 保留当前结果，删除进程缓存中持有完整 session 的路径，Wasm 改读本会话统计
- 测试源码迁移内部缓存、真实容量与窗口、准入拒绝、开始后读失败、根隔离、共享 owner 最后释放及宿主结果生命周期；本次未做事后静态核查，未编译或运行测试
- 旧性能 policy 字段及 preset 删除、必要 owner 工厂接入准入前回收、reference 最后依赖、Playback 有限业务窗口继续实施，T05/T10 保持进行中

### 2026-09-09：T03/T10 必要定长目标准入前回收，已实现、未验证

- ByteStoreSession 增加本次运行绑定，CreateSizedStore 在正式预约前调用根 driver 的 ReclaimOptionalStorage，按照确定的下一笔容量与实际预约余量逐项回收
- 几何、属性、拓扑、reference、remap 等既有定长用途统一经过该工厂；叶 workspace 结束时解除运行绑定，按需属性 reference 采用局部绑定 guard，存活数据 owner 仍只持有容量 state
- 可选完整输入 loader 继续只尝试一次准入，不为自身驱逐其他 cache；必要申请在可选引用全部解除后仍缺少容量时按既定用途返回准入结果
- 测试源码新增缓存引用移除后仍由消费者持有的容量不可回收、最后消费者释放后真实余量恢复与必要目标重新准入；本次未做事后静态核查，未编译或运行测试
- 累积输出扩容的 driver 准入前回收与最终封装边界继续在 T09 实施；本记录不表示全部分配点已完成

### 2026-09-09：T10 必要 reference 的命令生命周期，已实现、未验证

- DecodeReferenceCache 增加 RequiredFrameLease；driver 根据依赖规划在准备前驱前登记必要引用，同一 key 的消费者共同持有，最后许可归还时在锁外释放必要 owner
- 必要 reference 发布与可选 LRU 留存分别执行；M=0 或压力暂停可选留存时，当前命令的依赖仍可发布和查找，trim 只解除可选 LRU 引用
- 普通帧解码的许可覆盖前驱准备至目标提交，按需属性补解的许可保留在该请求局部结果中，直至全部目标属性消费或失败清理结束
- DecodeSession 直接接收 Publish 返回的共享发布结果，不要求一次有效发布必须留在可选 LRU；原有叶级借用 owner 在叶执行结束后释放
- Playback 预取启动同时检查根可选留存政策；既有内部 coordinator 已有当前命令、一个前台 pending、一个可选 prefetch 的有限窗口，本步沿用该机制
- 测试源码新增关闭留存时两个必要 reference 的发布、重复依赖许可、trim 豁免及最后必要引用释放；本次未做事后静态核查，未编译或运行测试
- 跨帧编码历史与实际末次依赖释放、元数据复制、旧 policy/preset 删除和完整故障诊断继续实施；T10 保持进行中

### 2026-09-09：T05/T13 启动资源与业务配置分离，已实现、未验证

- 删除 Encode/Decode tier、旧 CodecPerformanceParams/PresetParams、RuntimeValidator 及业务配置中的模块资源预算，业务默认值迁入 CodecParamDefaults；压缩增强、显式 Zstd 等级、关键帧间隔和校验选项独立保留
- 删除 pipeline execution profile、阶段并行开关及叶 workspace 的旧预算传播；叶执行直接借用请求业务配置，资源统一沿已有根传递
- 缓存策略只保留业务启用开关，内部留存沿用根的固定条目规则；Playback 预取固定为一个方向候选，删除全文件预取及 Wasm/示例入口的对应开关
- IGDC reader/writer 改传 CodecResourceParams，删除外接缓存 setter；报告记录 Fixed/Adaptive 及明确指定的启动上限，未指定的值不填充推测结果
- 配置、报告、Playback 与预取测试源码同步迁移，删除未注册的旧三档增强重复测试；本次只实施，未做事后静态核查、编译或运行测试
- Qt 窗口保留最后迁移；平台能力与自动控制器、第三方计算额度及后续调用链按原 TODO 继续实施

### 2026-09-09：T04/T11 自动控制状态机、观察批与平台采样，已实现、未验证

- DataCodecResourceController 增加纯 Advance、统一常量、NORMAL/DRAIN/HOLD 状态、低水位确认、单次收缩及严重升级、30 秒 HOLD 期限、有效采样恢复、增长冷却和重复压力退避
- 系统压力保持当前请求 M，独立请求边界重新确定 M；可靠硬能力下降立即限制后续容量，M 增长遵守全局天花板、五秒间隔及高水位余量规则
- 根执行层增加 Adaptive 控制线程与同锁 PublishLimitsLocked，Fixed 不启动监控；采样在锁外，容量/准入/可选留存政策同锁发布，空闲 scratch 锁外回收，宿主缓存由 driver 回收
- 观察批以目标发布后首批 S 个槽位的实际退休完成，根累计计算、槽位、输入和提交等待时间；完整观察与新鲜样本后按需求择一增长 M/C/S，串行旧大块不增长
- 数值、reference、几何、属性、connectivity 和 polyhedron 块流接入工作类型描述，包含路径、标量、分量及基础规格，类型切换保持既有压力与冷却状态
- ResourceProbe 增加 Linux 所属 cgroup 与祖先的 memory/cpu 限制、地址空间限制，Windows job CPU 配额与一次性原生压力通知，Wasm 线性内存硬能力和模式相关线程许可扣减；未知当前可用量保持明确缺失
- Windows 原生通知只唤醒控制线程，重新采样在执行锁外完成；订阅析构等待回调退出，根关闭先停止监控，再销毁通知 owner；无 pthread 平台使用同一机制的 C=S=1 配置
- 状态机轨迹测试源码接入 ExecutionMechanism，覆盖压力、恢复、未知信号、观察归还、提交阻塞、M 增长与硬能力下降、无线程平台和 Advance 无分配
- 本次未做事后静态核查、编译或运行测试；混合块 codec/reference 类型切换的精细边界、平台实测、完整诊断导出与故障轨迹集中验收继续保留，T11 不标完成

### 2026-09-09：T09 最终输出阶段、追加增长与 package 计算额度，已实现、未验证

- MemoryStore 增加同根 PrepareCapacity，driver 在计算唯一扩容容量前逐项解除可选引用，worker 保持即时单次预约；owner 不保存根对象，写入适配器只在本次执行中借用根
- 数值、reference、connectivity、polyhedron 五流和帧字段追加写入统一复用 AppendableByteStoreWriter；MemoryByteRangeOutput 改为接收当前根，完整预分配、写入增长和 Finalize 增长共用同一容量事务
- 叶 bundle 导出、叶包写出、帧字段压缩及最终帧布局写出持有 HeavyPhase，覆盖输入消费、End/Seal、header 回填与最终结果发布
- package Zstd 经 RunTerminalWork 的 ExclusivePackage 执行，按实际取得的 w 设置内部 worker 为 w-1，w 小于 3 时显式设为零；块内和探测压缩保持同步模式，删除 RecommendWorkerCount、零值自动推荐与 package workerCount 参数、校验和报告
- 分段 Materialize 删除可变 vector 出口，先创建完整 MemoryStore 目标，获准后按固定窗口消费分段并交付 EncodedBuffer；容量不足时一次性来源保持未消费
- 新增输出资源测试源码，覆盖 driver 扩容前回收、old/new 并存容量、完整连续化准入与消费、package 排他额度及 Zstd 回放、零额度拒绝；已有输出与字段测试同步接口
- 本次未做事后静态核查、编译或运行测试；输入 owner/range 收尾、长期输入输出引用清理、第三方 compressor owner、完整审计与最终故障验收继续按原清单实施

## 逐对象覆盖台账

实现时逐行核对方案 7.8 的 61 项对象。每个根容量对象记录准入、分配、唯一 owner、最后释放和注册测试；槽位对象核对读入前取得、实际消费后归还。审计对象与豁免对象核查无预算拒绝分支。

| 状态 | 对象组 | 数量 | 实现归属及重点 |
| --- | --- | --- | --- |
| [x] | 7.8.2 数值、属性、几何、reference | 13 | N01—N13 均具备直接运行证据，候选 full/probe 与真实前驱 workspace 退休已补齐 |
| [x] | 7.8.3 重排、拓扑 | 18 | T01—T18 均有直接证据；grammar 十数组独立取样、Morton 固定表、polyhedron 全域 owner 与跨批次 emit 已补齐 |
| [x] | 7.8.4 输入、封装、输出 | 13 | I01—I13 均有直接证据；package probe、多叶并存、最后共享引用及 Windows OS 预取接受已补齐 |
| [x] | 7.8.5 会话、缓存、元数据 | 8 | S01—S08 均有直接证据；4097 帧 metadata 规模、共享引用及退休已补齐 |
| [x] | 7.8.6 宿主、校验、日志、平台 | 9 | H01—H07/H09 有直接证据；prepared assembly、finite/签名窗口已补齐；H08 浏览器按用户要求移出范围 |

## 必须独立验证的门槛

- [x] M/C/S 同时更新、零容量、非法目标、下调到实际用量以下、预约后下调、无反向锁序；沿用执行/容量/诊断锁交互用例，真实 Job 的 M 下调未实测
- [x] 共享 owner 最后引用释放、旧新数组并存、缩短不重分配、结果离开根状态继续存活
- [x] S=1、S>C、慢首块/reader/writer、顺序提交、最终 Seal 与空字段阶段；删除旧同池嵌套任务组入口
- [x] 固定失败结果、持续分配失败、取消唤醒、队列锁外销毁、首错保留、一次完成、线程 join
- [x] Fixed 无周期调节、Adaptive 第 17.4 节确定性轨迹、关闭审计/丰富诊断不改变执行；共用机制及独立 audit/export 用例通过，真实 Windows Job 压力与恢复单独记录
- [x] 61 行对象为当前 Windows 60 行直接证据、浏览器 1 行范围外；float32/64、多分量、SOA/getter/remap、reference/region、变长拓扑通过
- [x] 旧大块、新固定粒度、精度/格式 round-trip、外层 Zstd 与跨窗口 varint/bitpack
- [x] Windows 原生及无外存/无 pthread 内部能力配置、第三方显式线程与上下文释放、宿主借用不重复审计；实际其他设备不在验收范围
- [x] 旧接口/模块/报告字段按 T13 的删除及编译契约完成验证，三个未接入实验头排除并保持原状
- [x] 大字段/小字段/小结果/低内存/外部压力既有实测与完成率保留；本轮补齐独立六帧序列性能，四配置各三次正式运行全部成功；极小 points 原比例目标未达标明确列为已知限制，范围外平台不再验收
- [x] Qt 最终窗口及配置传递、失败信息、无数据时控件行为；沿用 T15 的真实窗口与配置通过结果

### 2026-09-09：T06 区域规划字段准备与局部规划，已实现、未验证

- NumericEncodeRegionState 通过现有 Contiguous MemoryStore 持有 `runCount*sizeof(RegionRun)` 的唯一自有排序数组；源配置数组只借用，字段准备取得 HeavyPhase 后提交终端工作完成复制、排序、范围与不相交校验
- precision levels、labelToLevel 和归并策略在字段准备时生成一次；NumericEncodeCursor 用二分查找取得当前块相交 runs，块内不再复制、排序或 reserve 全字段数组
- 空交集块保留默认精度区域；局部 runs 在分量残差计算完成后移动到最终 layout，字段 owner 在块流收束后释放
- RegionPrecision 测试源码迁移到准备/规划接口，增加精确 owner 容量、共享释放、跨块查找、空交集及非法 runs 用例，尚未执行
- 普通参数在帧/叶 runtime configuration 中的全配置复制，将随 T05/T13 删除旧资源配置路径统一清理；本记录不代表该边界已完成
- 本次未做事后静态核查，未编译或运行测试，T06 保持进行中

### 2026-09-09：T06 reference 块流与窗口重采样，已实现、未验证

- ReferenceTransferCacheBuilder 改为 driver 读入、worker 候选计算、driver 有序写出；current/reference、Predictor 搜索、Exact/Probe 样本和候选、待提交结果共用块槽位，最后 Seal 在归还最后槽位前完成
- reference 编码固定使用 65536 tuple 游标；删除可调 spatialBlockElementCount、全字段 SpatialBlockRange 列表与依据 reference 文件布局约束当前编码批次的分支，参考数据通过已准备的 source 读取
- 数值块结果移动到有界记录；算法拒绝与失败保留不同语义，淘汰的候选随计算结束释放，计算失败记录根首错，禁止失败重放为普通编码
- 删除 AttributeEncodeScheduler、AttributeLaneGate、reference lane 配置与报告；保留的普通数值耗时统计移入现有 DataCodecCallback 的 DurationAccumulator，由 driver 更新，不参与资源控制
- NumericArrayReferenceBytes 的重采样固定采用最多 1 MiB 源窗口与两个 tuple 暂存；稀疏跨距只读相邻 tuple，保留原坐标映射与插值运算顺序；AttributeDecode 已调用同一范围函数
- GeometryEncode、AttributeEncode 与现有 reference 测试调用迁移到根执行资源；测试源码增加多块/尾块、源布局独立、共享槽位/输出释放、稀疏源与跨窗口插值用例
- 本次未做事后静态核查，未编译或运行测试；reference 完整字段准备、分组评分容量、参数复制清理与精确审计仍需后续实施，T06/T08 保持进行中

### 2026-09-09：T06 reference 分组评分容量，已实现、未验证

- AttributeReferenceScheduleBuilder 的分组描述只保存字段索引；每个有效组先按 `s0*sizeof(size_t)` 创建完整连续索引 owner，终端函数保留既有舍入/clamp/去重顺序，只缩短 span 逻辑长度
- driver 按实际去重样本数为本组所有字段取得样本容量，终端函数读取/评分；连续样本段拆成最多 1 MiB 的源读取，单 tuple 超过窗口时只读该 tuple
- 真实通过评分的 IntraFieldEdge 直接追加到一个 Contiguous MemoryStore，扩容复用现有根容量事务；不提前保留所有潜在字段对，不重复评分计数
- 每组结束释放本组样本/索引，全部组结束在真实边数组上排序、防环选择并生成父关系和记录顺序，随后释放边数组；整个准备阶段共用一个 HeavyPhase 与根计算额度
- 属性编码调用与 PipelineContracts 评分测试迁移到根资源/session，补充持有峰值和评分后容量释放断言；本次未静态复核、编译或运行测试，T06 保持进行中

### 2026-09-09：T04/T08 几何解码与旧大块准入，已实现、未验证

- 根 BeginFlow 增加 singleRecord 标志；旧大块字段在同一槽位机制中限制实际并存记录数为 1，全局 M/C/S 保持原值，快照提供该流属性供后续 Adaptive 停止增长使用
- RunOrderedBlocks 的读入函数可接收已有 SlotLease；几何的唯一顺序字段流通过 RunTerminalWork 使用该槽位取得计算额度，外层解压不在 driver 无额度执行
- NumericDecodeCursor 在读取前检查完整布局、连续范围和确定 raw 大小，按文件已有块读取；负载 ReadBytes 拆为最多 1 MiB
- 合并普通和 reference 几何解码循环为 DecodeGeometryBlocks；完整 float 输出和原精度 reference 在 HeavyPhase 中分别创建，worker 解码/转换，driver 窗口写入，最后槽位内完成全部 Seal 与 complete，失败收束后放下未完成目标
- 删除原同步 DecodeNumericArrayBlocks 循环、两套几何字段循环和 driver 类型转换实现；新增 NumericDecodeExecution CTest 注册及测试源码，覆盖两后端、线程/无线程、新规格/旧大块、动态额度、转换与原精度 reference
- 本次未做事后静态核查、编译或测试；属性数值块流、前驱准备和旧属性调度删除继续实施，T08 保持进行中

## 已核实基线

- 分支 datacodec/dev，起点 df6f459dd
- libpressio fork：https://github.com/hKJGkUs76A7m/libpressio.git，datacodec-dev = 753f05cdc53b702f9fc4d784b34c001ea09d0cf5
- SZ3 fork：https://github.com/hKJGkUs76A7m/SZ3.git，datacodec-dev = f0f465af529096c28566e5445f70bd8eed4bbbd7
- 2026-09-08 已显式 fetch 上述分支并 reset --hard，HEAD 与远端一致；重置前后均无子模块未提交修改
- std_compat 的未跟踪构建内容、三个既有实验头保留，未纳入提交
- 根 CMake、iGameCore CMake 和现有 cache 确定 CMake/Ninja、UCRT64 Clang、C++20，现有 iGameDataCodecTests 注册于 CTest
- 用户要求编译前加载环境脚本；已核实 Env 下 clion_env.ps1 与根 CMake、现有 UCRT64 Clang cache 一致，按路径笔误处理并在每次构建/CTest 前加载
- 本机 debug cache 使用 CMAKE_LINKER_TYPE=LLD，显式选择 cache 中既有的 ld.lld；首次 GNU ld 链接曾出现 COFF 重定位溢出，LLD 已完成链接。该设置仅修改本机构建配置

## 实施与验证记录

### 2026-09-08：T00/T01

完成依赖重置、源码/构建基线核对及 TODO 建立。实现方案作为版本化基准保留，当前整体功能尚未实施，未运行构建或测试。Qt 已固定为最后的实现单元。

### 2026-09-08：T02 第一部分，待编译验证

- T00/T01 提交：b5c48e6d9
- Common/DataCodecError 增加固定 CodecFailureRecord、完整 UTF-8 字符截断和大小/值语义静态约束
- EncodeResult/DecodePackageResult 内联携带 failure；入口校验失败直接返回记录，删除该返回路径的日志路由/时间字符串/messages 申请
- 两个公共入口捕获 bad_alloc、标准异常及未知异常，直接交付固定错误；编码的输入校验在日志设施创建前执行
- 注册 DataCodec.FailureContract 和完整 suite 用例：一至四字节字符截断、空文本、数值复制、无日志失败、入口零申请、属性枚举首次真实分配失败后持续拒绝申请
- 分配故障注入只在现有测试可执行目标链接，Runner 下 cpp 明确从生产 iGameCore 源集合排除
- 本阶段提交时完成 diff 空白检查及源码核对，构建/运行结果见下方后续验证记录
- T02 保持进行中：根内唯一首错、Context/pipeline/Playback 传播及全请求无分配收束仍待 T04—T12 接入，不以入口测试替代全链条保证
- 验证使用现有 iGameDataCodecTests，执行 FailureContract、StorageOwnership 和已有回归，结果持续记录于下方

### 2026-09-08：T03 容量事务与共享释放，待编译验证

- T02 入口边界提交：ebee358e5
- ResidentByteBudget 改为独立共享 state 和 move-only Lease；删除公开 Reset/Release 数量操作，零容量有效，私有 SetLimitLocked 留给同一根对象统一发布
- MemoryStore 不再接受空预算旁路；扩容只申请一次完整新容量，新旧数组并存，失败保留原数组，逻辑缩短不分配；追加自身范围在扩容后重定位输入
- ByteStoreSession 使用弱登记，Reset/ReleaseAll 不清空存活容量、不强制清空共享数据；移动不创建新容量状态
- 删除 IByteSource/IRemapProvider 的破坏性 Release 接口及派生实现，源数据消费者改为放下自身引用；store 数组/文件由最终 owner 析构清理
- 删除内部 FileBackedMMapStore 类及生产选择，文件 store 采用现有范围流式实现；先构造 owner 再创建文件，使后续登记申请失败能析构清理文件
- 预约计数与实际 resident 取样分别报告，预约峰值字段改名 peak_reserved_bytes
- 新注册 StorageOwnership：额度加总/移动/零值/超出预算对象寿命、扩容并存/回滚/缩短、自追加、session Reset 与最后引用、分段消费及 remap 共享
- 本阶段提交时通过严格 UTF-8 解码、diff 空白检查及删除符号检索，构建/运行结果见下方后续验证记录
- T03 未完成项：根状态绑定并删除 session/各模块额度配置、完整用途工厂与平台外存能力、EncodedBuffer 和最终输出接口、弱登记去重/阶段清理、全链条验证
- 推进说明：T02 的未运行验证继续保留；期间推进已确定接口且不依赖该验证结果的 T03 基础修改，未将测试标为通过

### 2026-09-08：T02 固定首错传递与 T03 故障注入验证

- T03 容量事务与共享释放提交：9833a86ff
- EncodeContext/DecodeContext 删除 EncodeFailureState/DecodeFailureState 的三个字符串字段，使用固定 CodecFailureRecord；RecordFailure 和 FirstFailure 不申请文本，已有首错保持不变
- FailureScope 为 bad_alloc、标准异常、未知异常直接构造固定记录；普通 bool/status 的原有详情传播继续保留
- 编码 pipeline、叶、帧、帧序列结果和解码叶、session、package 结果携带 failure；公共编码转换与 Playback 已有帧结果传递该值
- 叶/帧入口捕获异常时保留已经保存的首错；早期失败返回移除日志构造，清理链移除 ByteStore 丰富日志导出；已保存失败的可选报告在锁外执行，导出异常不影响固定结果
- TakeMessages 在锁内移出既有 vector，在锁外按唯一 order 原地排序，删除 stable_sort 临时申请
- 首次运行 FailureContract 通过；StorageOwnership 暴露 AssignError 按值字符串参数在 error=nullptr 时仍分配的问题，改为 string_view，并补充无错误输出和消息移动的拒绝分配用例
- 修正后 LLD 构建通过，FailureContract 与 StorageOwnership 均通过；完整回归继续核查，结果见下方
- T02 仍在进行：当前 Context 中的固定记录与互斥量在 T04/T05 迁入唯一根首错；Playback 属性/提交错误出口、帧序列完整故障收束、工作线程取消/等待与全请求持续 OOM 仍待迁移，现有专项只保证实际覆盖范围
- T03 仍在进行：最终输出需复用同一根容量状态，先接通根状态，再处理 EncodedBuffer，禁止新增输出专属预算

### 2026-09-08：存储后端迁移回归与 T06 reference 块 owner

- 完整回归暴露 reference staging 依赖已删除的 FileBackedMMapStore 连续视图，按方案将 current/reference staging 原地改为 ScratchByteBuffer，覆盖单块读取、候选计算和写出
- 删除 NumericArrayReferenceStagingStoreFactory、StagedNumericArrayReferenceBytes、整块写文件再取映射视图的三个辅助函数，以及属性/几何工厂、对应 useMemoryStaging 与无用 access-window 参数；未恢复 mmap 和失败换后端路径
- 此项是完成 T03 后端删除所必需的调用方迁移，T06 记录为进行中；统一槽位、固定粒度、分量串行、quota/lane 删除与窗口重采样仍按依赖顺序实施
- 迁移后核心 self-test、报告文件契约和宿主重排语义用例已执行通过；完整集成在宿主压缩率文案的旧断言处停止
- 已核对该文案及断言均来自既有 cbb907f7a，生产中文已描述小数与除法，测试仍期待另一句文本；仅同步测试期望为现有中文，生产文案和 Qt 窗口未修改
- 修正文案后的完整 CTest 曾出现间歇性 Resource deadlock avoided；两个完整 GDB 运行通过，随后通过下方确定性用例取得真实崩溃栈并修复生命周期

### 2026-09-08：任务协调器真实收束与阶段提交验证

- 增加独立注册的 DataCodec.TaskCoordinator：任务仍运行时释放全部调用方 Handle，再调用 WaitIdle；修改生产代码前，GDB 捕获 Entry 在 worker 闭包析构时释放 taskGroup、TestParallelTaskGroup 析构等待自身线程并进入 terminate 的调用栈
- 将任务组所有权移到协调器的 dispatch 集合，driver 在后续提交和 WaitIdle 中等待已完成任务组并释放任务捕获；Entry 和结果共享引用不拥有可 join 的线程
- WaitIdle 使用真实活跃任务计数并完成任务组等待；已经取消且同 key 被新任务替换的旧任务仍计入收束范围；CancelAll 逐个在锁外请求停止，不分配临时 vector
- 这是现有协调器接入最终自有执行前必须修正的生命周期；有界 Playback 请求和内部执行替换继续归 T04/T10，不将现有 dispatch 集合声明为最终有界队列
- 加载已核实环境脚本后，LLD debug 构建成功；TaskCoordinator 连续 50 次通过；随后完整 CTest 的 All、FailureContract、StorageOwnership、TaskCoordinator 共 4 项通过
- 所有本阶段跟踪修改通过严格 UTF-8 解码与 git diff --check；Qt、三个未接入实验头和 std_compat 既有内容保持原状

### 2026-09-08：T04 根执行机制与 T05 调用链迁移中

- 上一阶段提交：1e9fad333
- 原地替换 DataCodecExecutionResources：同一执行同步边界维护 C/S、准入记录、有限终端队列、首错及固定事件环；M/U 继续唯一保存在 ResidentByteBudget，UpdateLimits 按执行锁到容量锁的顺序发布
- SlotLease/HeavyPhaseLease 使用独立计数状态，队列按部署天花板预先准备有限记录；普通与排他计算出队时取得额度，计算下调保留既有额度，提高 C 时可为已排队任务按需创建 worker
- ParallelExecution 增加 RunOrderedBlocks/RunTerminalWork，读入前准入、依次提交、最终消费后释放；取消逐个锁外销毁任务，首错与线程池关闭区分请求寿命
- 公共 Encode/Decode 请求开始改用 CodecResourceParams，内部 EncodeInRun/DecodePackageInRun 复用根状态；PackageDecodeWorkflow 的业务 session 改为内部形参，旧 Filter/Playback/Examples 注入调用迁移尚未结束
- 编解码 stage 驱动开始改为稳定拓扑串行顺序；块级调用及旧执行参数和专属测试的删除继续实施
- 新机制用例已使用本机已核实的 UCRT64 Clang 单独编译并运行通过：零容量、持有容量降额、M/C/S 无效更新、运行中缩小/增长额度、关闭门禁后完成已准入任务、慢首块有界顺序提交、失败后独立请求、无 worker 配置
- 上述独立检查使用现有生产源文件与新 feature 用例，临时入口仅位于忽略的构建目录；正式 suite/CTest 注册和完整工程编译仍待本轮调用链迁移完成，当前工作区不声明可完整编译
- ResourceController 目前只实现启动解析与水位公式，ResourceProbe 目前只具备初步 Windows 和 Linux 采样；完整 Adaptive 状态机、CPU/job/cgroup/运行时能力细节、监控循环仍归 T11，不能将当前启动解析称为已实现 Adaptive 模式
- T04 未完成：worker compressor owner、scratch weak owner 与完整压力留存策略、诊断 owner/等待上下文补充、更多故障与取消用例、全部机制接入；T05 未完成：Playback/补充属性/帧序列同根、Context 首错与 RunBinding、缓存注入和旧接口清零

### 2026-09-08：T04/T05/T10 有限执行与 Playback 集成验证

- DecodeTaskCoordinator 已原地改成单独 driver、一个当前命令、一个前台待运行和一个可选预取；帧与补充属性使用同一 variant 结果队列，reference 在当前 driver 依赖流程中执行
- Playback、DataObject session 和公共入口已接入内部根；运行中的命令异常、首错传递、取消、下一条独立命令复用及有限窗口拒绝已接入。帧序列编码、Wasm 旧 runner 与外部缓存注入仍需继续删除
- 完整集成用例定位到随机方向预取返回多个候选，窗口拒绝句柄被旧循环保存，后续同帧前台请求复用了失败句柄；现按最终窗口只登记一个可选预取，原有前台取消预取用例通过
- 根增加准入 driver 身份和序列溢出检查、worker 资源等待拒绝、取消锁外通知；排队闭包先锁外销毁，再发布取消完成。读入异常先发布固定首错，再收束工作并销毁记录
- 正式注册 DataCodec.ExecutionMechanism，新增首错先于停止回调、排他计算降额保留完整额度、worker 自等待拒绝用例。目标快照使用 try_lock，测试按接口契约等待一次可读取快照
- 加载已核实环境后，现有 LLD debug 目标构建成功；完整 CTest 的 All、FailureContract、StorageOwnership、TaskCoordinator、ExecutionMechanism 共 5 项通过
- 本阶段只证明以上执行路径。统一自有存储 owner、Context 唯一根首错、scratch/库上下文寿命、Adaptive 控制循环、平台能力及 Qt 均保持后续项，T04/T05/T10 未标完成

### 2026-09-08：T05 同根 Context、workspace 与帧序列；T08 同步消费

- 上一阶段提交：e1495e469
- 删除宿主线程执行器适配文件、并行拓扑 observer 文件及调用；Wasm 请求使用 CodecResourceParams，帧序列内部创建唯一根并传到帧、叶和 EncodeContext
- EncodeContext/DecodeContext 的取消与首错由根统一管理；RunBinding 绑定 workspace 的非拥有执行指针，作用域结束解除绑定，保留 workspace 不保留根执行寿命
- PreparedSurface 同步消费拓扑块，删除 observer 独立队列和任务组，完成时验证实际消费块数；取消和失败清理先收束根内任务
- 删除已失效的 pipeline runner 与并行 stage 参数；公共旧资源参数、外部缓存注入、各 session 容量和 scratch 仍待接续迁移
- 新用例覆盖跨 Context 唯一首错、绑定取消及解绑后根销毁、S=1 消费寿命、PreparedSurface 单四面体和非法块传播
- 加载已核实环境后，LLD debug 构建成功；完整 CTest 的 All、FailureContract、StorageOwnership、TaskCoordinator、ExecutionMechanism 共 5 项通过
- T05/T08 保持进行中；本次尚未修改 Qt

### 2026-09-08：T03/T05 ByteStoreSession 同根容量

- 上一阶段提交：1f3375f82
- ByteStoreSession 默认不创建容量状态；删除 ConfigureResidentLimit，内部 BindCapacity 连接根状态，未绑定时拒绝创建 MemoryStore，存活登记不能改绑另一根
- Encode/Decode workspace 的 RunBinding 绑定同根容量；帧序列 reference 在 PrepareFrame 绑定同根，删除叶和 reference 的独立 resident 上限发布
- 按需属性 reference 复用父 CacheResources，临时存储绑定父根；删除 DecodedAttributeCacheSet/DecodedGeometryReferenceCache 无根 InitializeOwned 和其独立 session 成员
- 弱登记去重并清除过期项；同一 source 重复登记不重复审计。Reset 与移动继续保留已存活 owner 的容量寿命
- StorageOwnership 新用例覆盖未绑定拒绝、跨 session 共享额度、降额后已有数据可读、重复登记、结果超出根执行寿命及最后释放不分配
- 加载已核实环境后，LLD debug 构建成功；完整 CTest 五项通过。用途工厂、各 codec 旧模块阈值、scratch quota、EncodedBuffer 与完整控制清单仍待接续，T03/T05 保持进行中

### 2026-09-08：T04/T06/T10 根 scratch 与数量留存

- 上一阶段提交：3a7f0abd4
- ScratchByteBuffer 使用纯池状态的 weak_ptr，删除 pool 裸指针和 scratch quota lease；所有 workspace、按需 reference 和帧封装访问根中的唯一复用池
- 池只按当前 2×max(S,C) 数量留存，每个空闲 buffer 固定最多 8 MiB；M=0、请求失败和关闭禁止归还留存，删除池的独立总字节份额及每叶 Configure/Clear
- 数量发布在根更新边界内仅修改标量，裁剪与数组析构在执行锁、容量锁和池锁外推进；池元数据预留至部署数量天花板，正常归还与裁剪不申请内存
- 删除数值读取、reference、wavelet 和 transfer builder 的 scratch quota 参数传递；删除失效 AttributeStagingHandle/Manager 及属性 scratch/staging quota 状态。属性计算 lane 和旧公共配置仍待业务块流迁移时删除
- 删除 SegmentedBinaryObject 无调用的独立窗口 CopyTo 重载；保留实际业务使用的 IByteWriter 消费协议。DecodeSession 不再将根 scratch 重复计入各叶占用
- 审计保留确定的 retained capacity 和借用次数/请求量，删除可变 vector 无法精确提供的 scratch 峰值字段；失败缓冲使用代次隔离，不能重新填入后续请求的池
- 新用例覆盖 best-fit、池关闭及销毁后归还、同根视图、运行降额、零容量延迟归还、失败代次和下一请求恢复。完整 CTest 五项通过；完成发布前清理的最后调整已重新构建并再次通过完整五项
- Adaptive 压力政策、业务块槽位全覆盖、旧预算配置清零和 Qt 保持后续项；T04/T06/T10 未标完成

### 2026-09-08：T03/T09 完整内存编码输出 owner

- 上一阶段提交：2e89132a8
- 增加只读、move-only EncodedBuffer，API、pipeline、叶和帧编码结果使用同一类型；其私有 owner 为 MemoryStore，最终交付无需另复制 vector
- MemoryByteRangeOutput 必须绑定根容量，增长使用已有 MemoryStore 事务，Finalize 完成后移动 owner；写入失败禁止后续交付，随机写空隙与最终扩展区域初始化为零
- 完整帧布局已知时，消费一次性叶数据前 PrepareExactSize 申请完整结果。公开已有 FramePackageIO 确定尺寸函数供内部帧执行器使用；未增加新的尺寸估算器
- 流式叶输出依实际增长申请；最终大小确认后 Seal，不改写为文件输出、不挂起 writer 调用栈、不通过完整结果的二次复制返还数据
- MemoryByteRangeReader 接收 EncodedBuffer 或其共享数据 owner，保持原数组地址和容量寿命。需要构造损坏数据的测试显式复制自己的 vector
- StorageOwnership 新用例覆盖准确布局预约、预约后降额、无分配交付、根销毁后读入原 owner、最后 reader 释放、写入增长失败及首次分配失败
- 加载已核实环境后，LLD debug 构建成功，完整 CTest 五项通过；VTK example 已迁移读取类型，当前 IGAME_DATACODEC_VTK_EXAMPLE=OFF，未构建该可选目标
- 固定用途后端工厂、完整输入与外部 cache 接口、1 MiB 窗口、window budget 删除、业务全部槽位与 Qt 保持后续项，T03/T09 未标完成

### 2026-09-08：T09 固定 I/O 窗口与删除独立窗口额度

- 上一阶段提交：00fea88f1
- 删除 WindowBudget 及专属断言文件，删除 CacheResources、workspace、帧封装、reference 和数值回放中的窗口预约、窗口总量、窗口配置接口及报告字段
- WindowRuntimeParams 统一 kIoWindowBytes=1 MiB；字段 raw/Zstd 读写、连续/范围复制、ByteStore CopyTo、叶与帧范围搬运使用同一规格，尾窗口按剩余长度缩短
- 删除数值 transfer builder 未使用的访问窗口参数；reference 块工作缓冲不再误作 I/O 窗口申请额度，后续 T06/T08 继续完成它们的块槽位覆盖
- 替换旧窗口预算测试，覆盖两个完整窗口和尾窗口、连续与范围源、范围越界及溢出、第二次读写失败停止、raw/Zstd 流跨窗口与 reader 移动、Zstd 截断失败
- 编译发现属性 reference 写回仍有窗口形参，已同步删除；保留通用 MiB 单位常量供尚待迁移的拓扑执行参数使用
- 加载已核实环境后，LLD debug 构建成功，完整 CTest 五项通过；严格 UTF-8 解码和 diff 空白检查通过。固定用途后端工厂、输入隐式全量复制、polyhedron 固定 cell 批次及业务槽位全覆盖继续保留为后续项

### 2026-09-08：T03/T06/T09 追加暂存固定后端

- 上一阶段提交：f1fe63d6f
- 根执行配置保存 ResourceProbe 的 externalSpillAvailable；workspace、reference session 和按需属性 reference 绑定相同容量及外存能力
- ByteStoreSession 的追加工厂删除调用方后端参数；具备真实外存能力时从创建起固定 FileBackedStreamStore，无外存时固定受控 MemoryStore。未知长度输出不依据大小、压缩率、旧档位或申请失败重新选择后端
- 未绑定或已释放 session 拒绝创建追加 owner；无外存能力拒绝文件工厂，删除 Wasm /tmp 伪外存分支；移动与 Reset 保留平台绑定，已有共享结果继续按最终引用释放
- 数值、reference、connectivity 四条流、polyhedron 五条流及帧字段封装共用此工厂，删除 useMemoryTransferCache、对应 scheduler 状态和 AttributeReferenceTransferSchedule 空壳
- 删除失效 geometry/attribute staging 模式、transfer/package 存储配置、属性 scratch/staging 配额及它们的 preset、校验和报告残留；remap/reference/解码配置继续在各自用途迁移时删除
- StorageOwnership 增加两种平台能力、M 降至零、固定后端、Seal、Reset 后共享读取、移动绑定及实际分配失败不切换后端用例；加载已核实环境后 LLD debug 构建成功，完整 CTest 五项通过
- 已知长度范围存储的分配前准入与连续数组用途尚待实施，T03/T06/T09 保持进行中；Qt 保持最后迁移项

### 2026-09-08：T03 定长用途工厂与 T08 属性 payload

- ByteStoreSession 新增唯一 CreateSizedStore 用途入口：按确定完整长度只预约一次；Ranged 未获容量许可且平台具备外存时在分配前选 File，Contiguous 固定要求内存；实际申请和 I/O 失败不更换后端
- MemoryStore 消费已获准 lease 精确分配完整数组，分配前后上限下调不撤销已有许可；owner 创建、实际数组申请及登记失败均按 RAII 归还容量
- AttrDecodeStage 删除 AttributeDecodePayloadMode、OneShotZstd 全量 vector、模块 payload 额度和配置/报告，统一固定窗口解压到定长范围 owner；原有效借用与范围读取保留
- StorageOwnership 覆盖精确容量、分配前选文件、连续用途拒绝、零长度、获准后上限下调、文件后端固定、真实 OOM 回滚和最后引用释放；FeatureBudget 新增跨两个 I/O 窗口的属性 payload 两后端回放与读入前拒绝
- 完整回归首次发现报告测试仍要求已删除的 OneShotZstd 字段，已同步删除旧断言；加载已核实环境后重新构建成功，完整 CTest 五项通过
- T03/T08 保持进行中：其他完整字段/reference 的用途迁移、driver 准入前回收及 HeavyPhaseLease、prepared payload 本次属性请求结束释放继续实施；单块 connectivity 输入属于槽位，不接入长期容量工厂

### 2026-09-09：T03/T06/T08 几何完整结果与 reference

- DecodedGeometryCache 按 E×D×sizeof(float) 完整长度创建 Ranged owner；DecodedGeometryReferenceCache 按原始标量类型的 E×D×V 在 BeginGeometry 一次取得容量，删除 Initialize 预分配、重复 Resize 及悬挂 session 成员
- 编码关键帧与解码 reference 调用同一个 BeginGeometry，分别持有自身确定容量；叶 session 的弱登记结束后，跨帧 reference owner 保持存活
- 删除 geometry 编码 reference、解码结果和解码 reference 的存储模式、局部额度、setter、preset 赋值、runtime 传递、报告与旧额度测试；属性与拓扑配置继续随各用途迁移
- 新增 float32 输出与 float64 reference 的精确分别计量、Memory/File 混合 owner、session 结束后读回、最后引用释放、零长度、零容量拒绝和尺寸乘法溢出用例
- 加载已核实环境后构建成功，完整 CTest 五项通过；T06/T08 的块循环、阶段准入与 reference 最后消费时点继续保持未完成

### 2026-09-09：T03/T06/T08 完整属性与属性 reference

- DecodedAttributeCacheSet 初始化仅记录字段元数据；每个实际请求字段在 BeginAttribute 以经过溢出校验的 E×D×V 创建定长 Ranged owner，字段之间可以独立选择固定后端
- 删除属性 Memory/Managed 模式、整集合大小预估及 m_allocatedBytes 局部拒绝账本；现存自有 owner 不重复 Resize，保留中改变逻辑尺寸直接失败；宿主 adapter-backed 输出继续调用宿主 Resize 协议并豁免根容量
- 编码、解码、按需 reference 与 Playback 属性初始化统一签名；删除属性模式、局部缓存限额、已无业务消费者的编码 reference 总额度，以及对应 preset/runtime 校验加总、报告和测试残留
- 删除统一 AttrCacheModeStage，避免混合后端的字段集合被报告成单一存储模式；本步未改变独立审计或 LRU 调度口径
- StorageOwnership 新增按需字段、float32/64 分别计量、独立 Memory/File、获准后上限下调、移动/会话结束/最后引用、宿主零容量豁免与必要字段拒绝及长度溢出用例
- 加载已核实环境后构建成功，完整 CTest 五项通过；T06/T08 保持进行中，字段 Begin 的 driver 阶段准入和有界块执行继续随调用链迁移

### 2026-09-09：T03/T08 完整拓扑与 polyhedron 索引

- DecodedTopologyCache 的 connectivity、offsets、cellTypes、polynomialOrders 按真实计数和元素类型分别创建定长 Ranged owner；计数加一、乘法及本地地址空间在创建第一个目标前全部检查
- connectivity 目标在局部 prepared 对象中逐个取得，全部准备成功才发布；后续目标容量拒绝或真实申请异常自动销毁本次已取得的 owner，不发布半成品
- DecodedIndexCache 同样按 count×sizeof(IndexType) 完整准入；polyhedron 五条流共用根容量，删除五流局部预算合计校验与逐流 mode/limit 传参；五流解码失败时局部 prepared 自动清理，成功后发布
- 删除拓扑解码结果与 reference 的独立模式/额度、preset、校验、报告和旧额度测试；删除将混合后端结果标记为单一模式的字段/日志，单块 encoded input 模式留待槽位迁移时删除
- StorageOwnership 覆盖四类数组精确尺寸、混合后端、缺省字段零容量、共享/移动/session 释放、后续目标拒绝的事务回滚及四类计数溢出；FeatureBudget 用五个真实索引 owner 的分配/回放/移动/释放替代旧配额数学测试
- 加载已核实环境后构建成功，完整 CTest 五项通过；T08 的拓扑块执行、同步 observer、polyhedron HeavyPhaseLease 与固定 cell emit 批次继续实施，未将完整存储迁移视为整阶段完成

### 2026-09-09：T03/T07 remap provider 与 Morton run 定长存储

- order/inverse 在工厂中按 count×sizeof(IndexType) 创建完整 Ranged owner；顺序写入按偏移填充，随机写入限于既有范围，完成协议 Seal 后才允许读取；provider 仅持有数据 owner，删除捕获 session 的存储重建包装和 BeginWrite/BeginRandomWrite 接口
- Morton run 按 recordCount×(sizeof(uint16_t)+sizeof(IndexType)) 一次创建，删除 ResizeRecords；读写检查固定长度与偏移溢出，消费和移动保持唯一数据所有权
- 删除 EncodeStorageMode、remap 后端选择参数及 preset/报告/测试残留；每个定长对象由共用工厂在分配前选定后端，真实分配与 I/O 失败不换后端
- 新增顺序/随机写入、完成前读拒绝、Seal 后写拒绝、两后端、获准后 M 下调、session 结束后回放、最后共享引用释放、零长度与溢出、run 越界不扩容和移动释放测试
- 首次回归发现新增用例在空 owner 存活时重绑 session，已修正用例的释放时点；随后构建成功，完整 CTest 五项通过，13.85 秒。本次提交复用该验证结果，未重复运行
- T07 保持进行中：Morton 固定粒度、高桶 slab、key cache、旧整阶段 scratch quota 删除、重型阶段准入及 topology 块迁移继续实施；Qt 保持最后迁移项

### 后续集中验证阶段

- 下方 Morton/connectivity 的验证记录属于已经执行的历史结果，不重复运行
- 后续按最新规则先推进全部代码实现，事后静态核查、构建、测试及缺陷修复统一安排在最终验收；代码完成和验证完成分别记录

### 2026-09-09：T07 connectivity 有界块执行，集中验证通过

- Morton 固定工作区阶段提交：dcea85c65
- connectivity 在 driver 重型准入后准备完整连续 remap owner，RemapProvider 的 span 重载直接填充，删除全量中间 vector；源 provider 和连续副本分别持有确定容量
- 块在读入前取得槽位，按本块真实 offset 统计连接数；worker 只生成当前块四段结果，driver 顺序消费到一个固定后端追加 owner，每次写入最多 1 MiB，最后 Seal 完成后归还槽位
- 删除全部块 artifacts、全量范围列表、connectivity 四流 spooler 与对应 sink 接口、旧 parallelTaskRunner/workerCount 参数和平均连接数估计；polyhedron 路径随后单独迁移
- 新增 TopologyExecution：变长且偏斜连接计数、重排回放、Memory/File、单槽位/多槽位、线程/顺序平台、运行时 C/S 下调恢复、必要数组拒绝、取消及容量不足收束、重型准入压力等待与取消
- 重型准入检查 driver 尚持有的块/阶段凭证，违规直接记录 phase-not-drained 并停止，禁止等待自身提交或归还；新增两种持证情况的失败测试
- 审计在 driver 记录读入与提交时的真实 vector capacity，槽位覆盖输入及结果；控制逻辑不使用审计数据
- 首轮集中构建通过；回归发现新测试矩阵包含无线程 S>1 与 S<C 的非法配置，已修正为无线程 C=S=1、线程平台 C<=S
- 专项通过后的完整回归发现帧 bundle 仍强制转换旧 segmented topology；connectivity 改为按 layout 生成同一追加 owner 的 SubrangeByteSource，校验总长度、四段加总溢出与 ordinal，不复制块 payload。polyhedron 五流仍遵守自己的格式表示
- 范围 owner 在既有 ByteSource.h 中集中实现，共享数据只由原 owner 持有容量；新增两后端固定窗口复制、范围越界、writer 失败、session 释放后读取和最后共享引用释放测试
- 加载已核实环境后的最终构建通过，完整 CTest 七项通过，15.73 秒，其中 TopologyExecution 0.20 秒；完整 suite 覆盖此前失败的帧序列封装与回放。严格 UTF-8、diff 空白及旧 connectivity spooler/sink 清零检查通过；本阶段未执行真实设备性能验收
- 固定 cell 粒度仍待数值及格式 metadata 的共同迁移，当前沿用 storage params 实际粒度；T07 保持进行中

### 2026-09-09：T03/T07 Morton 固定工作区与统一容量，集中验证通过

- remap 定长存储提交：c61b2ccad
- Morton leaf 固定 8 MiB，run 窗口固定 1 MiB，删除请求/workspace/preset 中对应粒度和 remap quota 配置、报告、旧配额数学断言
- highBuffers 的 65536 个 vector 改为一个必要连续 MemoryStore slab；按实际 highCounts 与每片最多 ceil(1024/r) 条记录计算精确容量，前缀表和已写长度是固定规模元数据；刷出后释放 slab，run 独立持有固定后端
- key cache 在读取 key 前检查根可选留存政策并只 TryReserve 一次；获准许可交给 ByteStoreSession::CreateReservedMemoryStore，复用原 MemoryStore 分配事务和弱登记，失败直接交付 failure；high run 写完释放 cache
- 小规模 Morton 输出同样必须由定长 provider 发布，删除无 factory 时转 VectorRemapProvider 的生产分支；point/cell/reference/polyhedron 调用统一传递已有根对象
- 删除 ActiveByteBudget/ActiveByteBudgetStats 及最后生产调用、CacheResources 的独立预算/Clear、workspace 配置和 remap_scratch 报告；三个既有实验头保持原状且不接入构建
- 新增并注册 MortonResources：小规模稳定排序与 inverse、集中桶/全部桶分散、两后端 slab 的精确容量与失败前置、跨固定叶的重复 key、可选 key cache 接纳/拒绝、keyGetter 异常及真实分配失败不重放
- 第一轮集中回归中 MortonResources 通过；StorageOwnership 的跨根许可用例发现参数析构可能延迟至完整调用表达式结束，CreateReservedMemoryStore 已改为局部持有许可，确保失败容量在函数返回前归还
- 加载已核实环境后的最终构建通过，完整 CTest 六项通过，15.70 秒，其中 MortonResources 1.07 秒；严格 UTF-8、diff 空白及旧生产符号清零检查通过；本轮未进行真实设备性能验收
- 未完成边界：Morton 的 HeavyPhaseLease/计算额度和 driver 分段、必要 owner 的计算前准入、全路径取消、connectivity 连续 remap 数组与有界 artifacts、polyhedron 全局数组仍待后续阶段；本记录不表示 T03/T07 整项完成

### 2026-09-09：T06 固定基础粒度与普通数值块流，已实现、未验证

- 上一阶段 connectivity 提交：cbaedd074
- SpatialBlockLayout.h 集中定义 65536 tuple/cell 基础规格；删除公共 SpatialBlockPolicyParams、SetSpatialBlockElementCounts、preset 赋值与请求校验；编码工厂不再接收控制参数，几何/属性/reference/connectivity 缺省值统一引用该常量
- SpatialBlockStorageParams 保留格式内的实际 point/cell 值，解码不覆盖旧文件粒度；新增新旧粒度 metadata 序列化回放和生产工厂规格测试，旧大块解码单记录准入仍属 T08 待迁移项
- 普通数值编码改为 NumericEncodeCursor 和共用 RunOrderedBlocks：读入前取得槽位，计算使用 WorkerContext 的根 scratch，driver 顺序消费 payload 和布局，最后 Seal 完成才归还槽位；删除预生成全部范围及零粒度表示整字段的分支
- 几何和普通属性调用接入根；删除 pressio lane 回调、独立 gate、配置 setter、preset 和报告残留，计时随 BlockOutput 返回 driver 记录；reference lane 随其块流迁移时删除
- WriteNumericArrayBlock 每次最多写出 1 MiB，不更改格式内容；删除未使用的 maxComponentBufferBytes 逻辑推算统计
- 新增 NumericExecution：float32/64 三分量、跨两个完整块及尾块、GetterOnly 输入、Memory/File、线程/无线程、C/S 下调恢复、误差回放、读失败、取消、必要输出容量拒绝、提交异常、空字段及资源收束
- 本阶段尚未编译与运行测试；区域 run 的字段级根 owner/局部规划、reference 块流与有界重采样、完整 reference 准备、数值解码、独立精确审计继续实施，T06/T08 保持进行中
