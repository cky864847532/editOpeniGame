# 解码最低受控存储额度：接口与验证

## 计算口径

`AnalyzeDecodeStorage` 根据包描述和 Params 计算当前全内存解码路径的必要受控存储并存量，输出 `minimumOwnedStorageLimitBytes`。计算只覆盖实际进入 `ResidentByteBudget` 的存储，不使用占用审计或压缩率估计。

该值适用于关闭可选完整输入缓存、后台预取及可选解码结果留存的请求。调用方将其传入 Fixed 模式的 `ownedStorageLimitBytes`。这一步不会改变操作系统、WASM 线性内存或第三方库的限制。运行时的额度变更需要重新满足本次请求的需求。

`adapterBackedAttributes` 必须与实际 adapter 的 `SupportsAttributeDecodeStore` 一致。iGame 直接输出属性使用 true；普通 DataCodec 自有属性缓存使用 false。现有块级 scratch、宿主输出、渲染数据和第三方内部内存均不进入结果。

桌面允许文件 spill 时，可以在低于该数值的额度下使用文件存储。验证程序明确关闭 spill，测试使用与 WASM 相同的全内存容量准入语义。

## 接口

```cpp
datacodec::DecodeStorageAnalysisRequest request;
request.inputReader = reader;
request.adapterBackedAttributes = true;
const auto result = datacodec::AnalyzeDecodeStorage(request);
if (result.success) {
    resources.mode = datacodec::CodecResourceMode::Fixed;
    resources.ownedStorageLimitBytes = *result.minimumOwnedStorageLimitBytes;
}
```

时序请求通过 `referenceReaders` 提供依赖帧。分析复用 `FrameSequenceDependencyPlanner`，并遵循当前 PlaybackSession 对前驱帧解码全部属性的行为。`attributeSelection` 和 `attributeTargets` 决定目标帧的属性及其依赖闭包。

内部 `DecodeSession::AnalyzeAttributeStorage` 接受已排空任务的资源根，读取确定的预约基线和已有属性完成状态，用于属性补充请求。共享 reference 及其 workspace 在预约基线中仅计一次。该接口不提供外部内存池或缓存注入。

失败时 `minimumOwnedStorageLimitBytes` 留空，`failure` 使用现有 `CodecFailureRecord`。取消不发布额度。分析成功仅说明受控容量计算完成，数值正文的正确性继续由实际解码验证。

## 实现依据

- 几何：Float32 输出容量和原始类型 reference 容量分别计算
- 普通拓扑：connectivity、offsets、cell types、polynomial orders 的真实数组容量
- 多面体：五个完整索引数组
- 属性：目标字段及依赖字段，根据宿主或 DataCodec 的实际存储归属计算
- 外层属性载荷：Zstd 字段完整展开的 rawSize；未压缩字段借用输入
- 生命周期：遵循 Geometry、Topology、Attribute、Commit 阶段顺序，保留跨叶、跨帧及属性补充所需 owner
- 尺寸函数和属性前驱顺序由预检与实际分配、解码共用

## 下界计算过程

此处的“下界”定义为当前指定解码路径能够通过全内存存储准入的最低配置额度。输入包、属性选择、输出存储归属及已有会话状态共同决定结果。它不表示所有可能解码算法的理论最低内存，也不表示浏览器需要提供的最低堆容量。

1. 通过范围读取解析叶包或帧包描述及 Params，取得元素数、分量数、类型、拓扑布局、字段 rawSize 和依赖关系。Params 使用现有解码长度限制；此阶段不读取几何、拓扑、属性的数值正文
2. 复用帧依赖计划和属性前驱顺序，确定实际需要展开的叶、参考帧及属性依赖闭包
3. 使用与真实存储分配共用的容量函数，计算每笔受控存储的确定字节数
4. 按实际几何、拓扑、属性和提交顺序模拟存储生存期。在申请时增加并存量，释放时扣除，跨叶及跨帧 owner 保留到对应消费阶段
5. 取全过程并存量的最大值作为 `minimumOwnedStorageLimitBytes`，同时返回峰值阶段及该时刻各类存储的组成

定义 `E` 为已有会话的确定预约量，`G` 为当前几何缓存，`R` 为几何 reference，`T` 为尚未释放的拓扑，`A` 为 DataCodec 自有属性及属性 reference，`P` 为展开中的外层属性载荷，则：

```text
L = max阶段 (E + G + R + T + A + P)
```

各项均按实际生命周期增减。`peakBytesByKind` 是同一个峰值时刻的组成，不能理解成各模块独立峰值的集合。`retainedOwnedStorageBytes` 表示请求完成后的受控留存。

| 存储 | 确定容量依据 |
| --- | --- |
| 几何输出缓存 | `elementCount × dimension × sizeof(float)` |
| 原始几何 reference、DataCodec 自有属性 | `elementCount × dimension × NumericArrayValueSize(meta)` |
| 普通拓扑 connectivity | `cellBufferSize × sizeof(IndexType)` |
| 变长单元 offsets | `(cellCount + 1) × sizeof(IndexType)`；固定大小单元不申请 |
| cell types | 存在时为 `cellCount × sizeof(IndexType)` |
| polynomial orders | 存在时为 `cellCount × sizeof(uint16_t)` |
| 多面体五组索引 | `(2 × cellCount + polyhedronFaceVertexCount + polyhedronVertexCount + cellBufferSize) × sizeof(IndexType)` |
| 属性字段外层 Zstd 展开存储 | 包描述中的 `rawSize`；未压缩字段借用来源，不新增此项 |

当前 `IndexType` 为 32 位。所有容量乘加检查溢出。结构化拓扑不创建上述完整索引缓存。宿主直接承接的目标属性不新增 `A`；额外需要解码的前驱属性按实际 DataCodec 存储归属计入。几何和拓扑的外层解压使用当前流式路径，其临时 scratch 属于既有豁免范围。

属性补充从已排空任务的会话取得精确预约基线，共享存储只进入基线一次。已完成属性不重复申请。目标属性载荷与临时展开的参考帧载荷可以同时存在，此时二者同时进入 `P`。调用方须在分析及应用结果期间保持该资源根的会话状态不变。

## 上限不足时的使用方式

调用方可以在解码前比较当前 Fixed 额度与 `L`，并向用户展示需要增加的字节数。用户预先允许提升额度时，可将本次运行额度提升至 `L`。预检本身不修改额度，也不自动重试失败的解码。

分析失败、缺失依赖、输入非法或取消时不发布部分额度。实际解码仍沿用现有容量申请和 failure 路径；系统分配失败、压缩正文损坏和豁免内存不足继续由实际执行处理。浏览器示例的自动提升、提示界面及无限制模式不在本次实现范围。

预检给出的是存储准入需要的额度，WASM 线性内存还需要容纳宿主输出、scratch、第三方库及运行时开销。提高 `ownedStorageLimitBytes` 仅解除 DataCodec 对所覆盖存储的拒绝条件。宿主可用内存和 WASM 最大堆容量仍构成独立约束。

## 验证方法

`iGameDataCodecTests --storage-analysis` 对真实编码包执行预检，再以计算值运行完整解码，检查实际 `peakReservedBytes`。小型用例额外以计算值减 1 字节验证容量拒绝，不将逐字节精度作为大型数据的验收要求。

`iGameDataCodecFileBenchmark --storage-bound input.igc [deltaMiB]` 先分析文件，再以计算值加指定 MiB 偏移运行完整属性解码。执行配置固定为单计算额度、单槽位、无文件 spill；输出预测值、实际预约峰值、受控留存、结果规模和耗时。

大型数据验收要求为：计算值能够完成解码，计算值与必要受控额度的偏差控制在 200–500 MiB 范围内。进程工作集单独观察，不与受控预约容量混合。

## 实测结果

### 大型 IGC：完整输出

2026-09-10，Windows UCRT64 Clang Release，输入 `car_3800W_analysis_adaptive_20260910-113653.igc`，文件大小 2,204,353,773 字节。使用 iGame 直接属性输出，关闭 spill，以一个计算额度和一个槽位完成全量解码。

| 项目 | 结果 |
| --- | --- |
| 预检耗时 | 0.0291758 秒 |
| 计算最低受控额度 | 1,073,554,092 字节，约 1,023.82 MiB |
| 实际受控预约峰值 | 1,073,554,092 字节 |
| 峰值阶段 | topology |
| 峰值几何缓存 | 96,419,676 字节 |
| 峰值拓扑缓存 | 977,134,416 字节 |
| 解码结果 | 成功；8,034,973 点、39,494,600 单元、131 个属性 |
| 输出属性值总数 | 1,052,581,463 |
| 解码耗时 | 33.8099 秒 |
| 解码结束受控预约量 | 0 字节 |

该配置用于验证容量边界，耗时属于单计算额度口径。此数据不能用于对比原有多线程性能成绩。

同一文件采用计算值减 256 MiB，即 805,118,636 字节，再次解码时在拓扑存储申请处由既有 failure 机制拒绝。失败耗时 2.44393 秒，已达到的预约峰值为 757,597,288 字节，清理后预约量为 0。日志分别为 `logs/storage-analysis-car.log` 和 `logs/storage-analysis-car-minus256.log`。

本例计算值与所执行路径的受控预约峰值逐字节一致，满足允许 200–500 MiB 偏差的验收要求。降低 256 MiB 的测试提供了一次明确的不可运行对照。此结果只代表已测试数据和路径，不承诺全部输入均具有同样精度。

同次运行解码后的进程工作集为 11,735,105,536 字节，进程历史工作集峰值为 12,554,211,328 字节。宿主承接属性输出等豁免对象解释了受控存储额度之外的大量内存。这两个进程指标独立报告，未参与下界计算。当前接口的成功返回不构成在约 1 GiB WASM 堆中完成该文件解码的保证。

### 合成用例及回归

Windows Release 最终 CTest 结果：`DataCodec.All` 通过，耗时 12.04 秒；`DataCodec.StorageAnalysis` 通过，耗时 0.38 秒。两个测试入口全部通过，总耗时 12.44 秒。

新增测试实际编码有效测试包，随后在计算额度下执行解码，核对真实预约峰值。覆盖：

- 点集、普通非结构化拓扑、结构化拓扑、多面体
- 未压缩和外层 Zstd 字段，包括压缩 Params
- 不解码属性、显式属性选择、完整属性选择
- DataCodec 自有属性和 iGame 直接属性输出
- 多叶帧、几何 reference 留存、真实时序属性引用、帧内属性依赖闭包
- 已有会话属性补充、重复补充、部分完成的参考帧按需补充
- 两个计算额度与三个槽位、ObserverOnly 拓扑输出
- 只允许读取元数据和 Params 的输入来源
- 小型用例额度减 1 字节时的拒绝、取消、缺失依赖、非法属性目标和容量乘法溢出

部分确定容量结果如下，单位均为字节：

| 用例 | 计算额度 | 实际预约峰值 |
| --- | ---: | ---: |
| 普通拓扑完整属性、DataCodec 自有输出、外层 Zstd | 458 | 458 |
| 同包 iGame 直接属性输出 | 386 | 386 |
| 真实时序属性引用、DataCodec 自有输出 | 224 | 224 |
| 真实时序属性引用、宿主输出 | 96 | 96 |
| 部分完成参考帧的按需属性补充 | 708 | 708 |
| 显式选取帧内依赖子属性、DataCodec 自有输出 | 204 | 204 |
| 同一子属性由宿主承接、前驱由 DataCodec 保留 | 172 | 172 |
| 结构化拓扑、宿主属性输出 | 194 | 194 |
| 多面体拓扑、宿主属性输出 | 354 | 354 |

708 字节用例的峰值组成为：已有预约 48，目标属性载荷 319，参考属性载荷 325，新增参考属性 16。此时参考帧尚有其他属性未完成，预检逐字段读取其完成状态并按真实调用时点补解。该测试验证了目标与参考载荷同时存在的情形。

最终入口结果记录在 `logs/storage-analysis-ctest-final.log`；各用例输出记录在 `cmake-build-release-dev/Testing/Temporary/LastTest.log`。排障期间的旧测试日志保留原始失败结果，最终验收以上述 CTest 记录为准。

### WASM64 编译验证

Emscripten 6.0.9、Release、`MEMORY64=1`、`memory-16g` 配置下，包含最终修正的 `iGameCore` 静态库构建通过。构建记录为 `logs/storage-analysis-wasm64-build-final.log`。

本次未重新链接浏览器示例，也未进行浏览器端大文件解码。运行时结果来自 Windows 的无 spill 配置，验证了与 WASM 相同的 DataCodec 全内存存储准入规则。浏览器分配行为和实际 WASM 堆容量可用性仍需后续浏览器运行验证。
