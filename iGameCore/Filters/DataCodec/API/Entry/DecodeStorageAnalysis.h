#ifndef DATACODEC_API_ENTRY_DECODESTORAGEANALYSIS_H
#define DATACODEC_API_ENTRY_DECODESTORAGEANALYSIS_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/API/Params/DataCodecControlParams.h"
#include "DataCodec/Common/DataCodecError.h"
#include <array>
#include <optional>
#include <stop_token>

namespace datacodec {

enum class DecodeStorageKind : std::size_t { Geometry, GeometryReference, Topology, Attributes, FieldPayload, Existing, StageWork, BlockWork, Count };
inline constexpr std::size_t kDecodeStorageKindCount = static_cast<std::size_t>(DecodeStorageKind::Count);

struct DecodeStorageAnalysisRequest {
    std::shared_ptr<IByteRangeReader> inputReader;
    // 时序依赖帧的可范围读取来源，不复制压缩正文
    std::vector<std::shared_ptr<IByteRangeReader>> referenceReaders;
    std::optional<std::uint32_t> frameIndex;
    AttributeSelectionMode attributeSelection{AttributeSelectionMode::AllAvailable};
    std::vector<AttributeTarget> attributeTargets;
    // 必须与实际 adapter 的 SupportsAttributeDecodeStore 能力一致
    bool adapterBackedAttributes{false};
    // 与实际宿主直写能力一致，多项式阶数拓扑仍使用受控缓存
    bool adapterBackedGeometry{false};
    bool adapterBackedConnectivity{false};
    TopologyDecodeOutputMode topologyOutputMode{TopologyDecodeOutputMode::CommitToAdapter};
    std::stop_token stopToken;
};

struct DecodeStorageAnalysisResult {
    bool success{false};
    bool cancelled{false};
    // 仅 success 时给出全内存、无可选缓存路径的受控容量，不包含宿主和第三方内存
    std::optional<std::uint64_t> minimumExecutionLimitBytes;
    std::uint64_t retainedOwnedStorageBytes{0u};
    std::array<std::uint64_t, kDecodeStorageKindCount> peakBytesByKind{};
    std::string peakStage;
    std::uint32_t peakFrameIndex{0u};
    BlockPath peakLeafPath;
    std::optional<std::uint64_t> peakBlock;
    double polyhedronCountScanMilliseconds{0.0};
    std::size_t inspectedLeafCount{0u};
    std::optional<CodecFailureRecord> failure;
};

// 全内存、可选缓存可回收、既定依赖顺序下的单块推进门槛，不乘计算线程数
// 多面体提交需要扫描三条计数流，数值属性正文不解码，调用方和第三方内部内存豁免
[[nodiscard]] DecodeStorageAnalysisResult AnalyzeDecodeStorage(const DecodeStorageAnalysisRequest& request);

}
#endif
