#ifndef DATACODEC_WORKFLOW_ENCODE_ENCODESTORAGEPLAN_H
#define DATACODEC_WORKFLOW_ENCODE_ENCODESTORAGEPLAN_H

#include "DataCodec/API/Entry/EncodeStorageAnalysis.h"
#include "DataCodec/API/Adapter/IEncodeAdapter.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/EncodePipelineParams.h"

namespace datacodec::encodestorage {

// 供普通入口、块树和时序叶执行复用，时序角色与拓扑复用必须来自实际执行上下文
[[nodiscard]] EncodeStorageAnalysisResult AnalyzeLeaf(
    const IEncodeAdapter& adapter, const CodecControlParams& controls,
    const EncodePipelineControlParams& pipeline, std::span<const AttributeTarget> targets,
    std::uint32_t frameIndex, const BlockPath& path, bool includeTopology = true,
    TemporalFieldRole attributeRole = TemporalFieldRole::SingleFrame,
    std::stop_token stop = {});

void Merge(EncodeStorageAnalysisResult& result, EncodeStorageAnalysisResult leaf);

}
#endif
