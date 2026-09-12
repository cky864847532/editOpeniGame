#ifndef DATACODEC_WORKFLOW_DECODE_DECODESTORAGEPLAN_H
#define DATACODEC_WORKFLOW_DECODE_DECODESTORAGEPLAN_H

#include "DataCodec/API/Entry/DecodeStorageAnalysis.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Storage/LeafPackage/LeafPackage.h"
#include "DataCodec/Storage/FramePackage/FramePackageFormat.h"
#include <map>

namespace datacodec::storageplan {

// 会话补充解码使用内部确定状态，已保留对象的预约容量只计入 existingBytes 一次
struct ExistingLeaf {
    std::uint32_t frameIndex{};
    LeafPackage package;
    std::vector<bool> completeAttributes;
    std::vector<bool> adapterBackedAttributes;
    bool geometryReferenceReady{false};
    bool topologyReady{false};
};

struct ExistingState {
    std::uint64_t reservedBytes{};
    std::vector<ExistingLeaf> leaves;
    bool supplementAttributesOnly{false};
};

[[nodiscard]] DecodeStorageAnalysisResult Analyze(const DecodeStorageAnalysisRequest& request,
    const ExistingState& existing = {});

}
#endif
