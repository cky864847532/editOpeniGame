# 编码受控内存下界

## 契约

`AnalyzeEncodeStorage` 读取输入元数据、选中的属性和实际编码参数，返回 `provenLowerBoundBytes`。该数值是成功执行必需受控容量的已证明下界。Fixed 已解析上限小于此值时，在数值读取和编码输出前拒绝请求；达到此值时，继续正常运行。达到下界不保证编码成功，也不保证完整峰值等于下界。零值表示当前证据未排除任何额度。

分析对象是 `ResidentByteBudget` 控制的必需连续存储。借用输入、宿主输出、第三方库内部、普通豁免 scratch、可选缓存不计入。可使用文件的 ByteStore 不按完整内存存储计入。预检不根据原始数据量、压缩比、线程数或安全系数计算拒绝阈值。

各见证只包含已证明同时存活的分配，见证之间取最大值。叶之间、属性附件之间和先后阶段之间也取最大值。已持有缓存与在途块的总占用不追加到启动下界；运行期会按真实生命周期处理这些占用。

## 已接入的分配

| 条件与阶段 | 下界组成 | 复用依据 |
| --- | --- | --- |
| 普通非结构拓扑，自有拓扑，Morton 点顺序或单元顺序 | 点逆重排 `pointCount × sizeof(IndexType)` 与单元重排 `cellCount × sizeof(IndexType)`，仅加入实际启用的项，两项相加 | `CalculateTopologyRemapStorageBytes` 同时用于真实分配；Morton 对大于一的域生成非恒等存储 provider |
| 多面体自有拓扑验证 | `faceCount` 字节访问标记 | 编码器直接以面数创建连续 byte store；数据扫描后才知道是否需要局部索引表，局部表不加入预检 |
| 数值场启用区域精度 | `regionRuns.size() × sizeof(RegionRun)` | `CalculateNumericRegionStorageBytes` 同时用于真实分配；仅有列表、未启用区域精度时无此申请 |
| 单帧属性帧内参考采样 | 一份索引容量，加同组全部字段采样容量 | 沿用字段资格、相同类型/维度/元素数分组和 `BuildAttributeReferenceSampleIndices`；点属性与单元属性分别分组；`CalculateAttributeReferenceFieldSampleBytes` 与真实分配共用 |
| Morton 超过内存叶阈值进入高位分桶 | `min(elementCount, recordsPerSlice) × recordBytes` | 每桶实际申请 `min(bucketCount, recordsPerSlice) × recordBytes`，其总和必不小于此值；使用编码器相同阈值与常量 |

采样索引数量超过 65536 时，预检只计入精确的索引容量，并记录 `ReferenceSampleEnumeration` 未决项；避免预检本身枚举巨量索引。通常的采样请求沿用实际索引生成函数，字段采样按去重后的数量计算。

`peakStage`、帧号、叶路径和 `peakAllocations` 保存最大见证。`unresolved` 标明编码结果、存储后端与增长、参考选路与保留、分桶分布等未决部分。失败和取消不返回可用下界。尺寸乘加检查溢出。错误统一使用现有 Failure 记录，额度不足错误同时携带 `requestedBytes` 和 `limitBytes`。

## 执行和 Qt 接入

普通 `Encode` 入口在创建执行根前预检 Fixed 请求，块树先预检全部叶。叶执行器复用同一分析器，覆盖时序内部入口，使用实际拓扑归属和时序角色。Adaptive 的瞬时额度不参与启动拒绝，Unlimited 不执行字节限额拒绝。两种模式保留既有运行期行为。

Qt 通过 `IGDCWriter::CheckStorageBeforeEncode` 使用与 writer 一致的参数和属性选择，在创建输出目录、报告和工作线程前检查。失败写入压缩窗口现有运行状态区域，不启动压缩，不提高配置上限。

尚未加载的时序帧不在 GUI 线程预读。writer 对此返回零下界并标明 `UnloadedTemporalFrames`；各帧实际叶加载后、进入编码前执行下界检查。因此时序后续帧额度不足可以在运行过程中报告。

真实压缩结果长度、增长时新旧存储共存和参考保留继续走既有 `WaitForStorage` / `PrepareCapacity`。启动分析不替代这些准入点，也不把某次暂时无法准入当作全任务必然无法执行。

现有解码容量分析维持原有契约：全内存、既定依赖顺序、单块推进的受控容量。本文新增的编码证明型下界不更改解码分析的定义或计算。

## 验证

新增 `DataCodec.EncodeStorageAnalysis`，覆盖元数据读取边界、选择过滤、采样并存、跨组/跨叶最大值、区域开关、拓扑与复用、结构网格、分桶下界、未知项、取消、溢出、Fixed 提前拒绝及真实采样分配的上下边界。Qt 资源测试使用实际压缩按钮，确认零额度报错并且不创建输出目录。

2026-09-12：加载 `clion_env.ps1`，使用项目已配置的 MSYS2 UCRT64 Clang、CMake/Ninja，完成 Windows RelWithDebInfo 编译。`iGameDataCodecTests`、`iGameDataCodecQtResourceTests` 与 `iGameVis` 均构建成功。

14 项 `DataCodec.*` 测试均获得通过结果。首轮 13 项通过；Qt 已展示正确的额度错误，测试断言曾查找 `FormatCodecFailure` 不展示的 reason 字段。断言改为核对实际显示的固定上限和下界后，Qt 单项复测通过（3.79 秒）。核心测试覆盖现有编解码、解码容量分析、所有权、Morton、拓扑/数值执行、任务协调、执行机制、资源模式和 CPU 控制。

Qt 测试的双属性输入证明下界为 32 字节。配置 0 字节并点击压缩后，运行状态区域显示固定上限与已证明下界，压缩按钮保持可用，输出目录未创建。本次验证范围为 Windows 桌面构建和测试。
