#ifndef DATACODEC_API_ENTRY_ENCODESTORAGEANALYSIS_H
#define DATACODEC_API_ENTRY_ENCODESTORAGEANALYSIS_H

#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include <optional>
#include <stop_token>
#include <vector>

namespace datacodec {

struct EncodeRequest;

enum class EncodeStorageUnknown {
    EncodedPayloadAndOutput,
    StorageBackendAndGrowth,
    ReferenceSelectionAndRetention,
    TopologyRemapAndLocalIndexTable,
    MortonBucketDistribution,
    ReferenceSampleEnumeration,
    UnloadedTemporalFrames,
};

struct EncodeStorageAllocation {
    std::string owner;
    std::uint64_t bytes{0u};
};

struct EncodeStorageAnalysisResult {
    bool success{false};
    bool cancelled{false};
    // 成功时给出已证明必要的受控容量下界，零值表示当前证据未排除任何额度
    // 达到下界仅允许继续执行，完整峰值仍由数据内容和运行路径决定
    std::optional<std::uint64_t> provenLowerBoundBytes;
    std::string peakStage;
    std::uint32_t peakFrameIndex{0u};
    BlockPath peakLeafPath;
    // 同一见证中的对象必然并存，各阶段与各叶之间只取最大值
    std::vector<EncodeStorageAllocation> peakAllocations;
    std::vector<EncodeStorageUnknown> unresolved;
    std::size_t inspectedLeafCount{0u};
    std::optional<CodecFailureRecord> failure;
};

// 只读取输入元数据与参数，不读取数值正文，不启动执行根或申请被审计的大缓冲
// 只计入当前实现必需的连续受控存储，借用数据、第三方内部和可选缓存豁免
[[nodiscard]] EncodeStorageAnalysisResult AnalyzeEncodeStorage(
    const EncodeRequest& request, std::stop_token stop = {});

// limit 必须是 Fixed 的已解析上限，Adaptive 和 Unlimited 传空值
[[nodiscard]] std::optional<CodecFailureRecord> CheckEncodeStorageLowerBound(
    const EncodeStorageAnalysisResult& analysis, std::optional<std::uint64_t> limit);

}
#endif
