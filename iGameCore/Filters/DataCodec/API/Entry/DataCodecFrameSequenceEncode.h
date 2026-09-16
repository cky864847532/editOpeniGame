#ifndef DATACODEC_API_ENTRY_FRAMESEQUENCEENCODE_H
#define DATACODEC_API_ENTRY_FRAMESEQUENCEENCODE_H

#include "DataCodec/API/Adapter/IBlockTreeAdapter.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Output/EncodedBuffer.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Params/CodecResourceParams.h"
#include "DataCodec/Common/DataCodecError.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace datacodec {

struct FrameSequenceEncodeFrame {
    std::shared_ptr<IBlockTreeAdapter> blockTreeAdapter;
    std::string rootName;
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    std::vector<AttributeTarget> attributeTargets;
};

class IFrameSequenceEncodeSource {
public:
    virtual ~IFrameSequenceEncodeSource() = default;
    [[nodiscard]] virtual std::size_t FrameCount() const noexcept = 0;
    virtual bool LoadFrame(std::size_t ordinal, FrameSequenceEncodeFrame& frame,
        std::string* error = nullptr) = 0;
};

struct FrameSequenceFileTarget {
    // 核心按实际帧编号生成同目录的连续文件名，提交成功后清理该序列的旧帧
    std::filesystem::path path;
    int indexWidth{4};
};

struct FrameSequenceEncodeRequest {
    std::shared_ptr<IFrameSequenceEncodeSource> source;
    // 空值表示逐帧转交内存编码结果
    std::optional<FrameSequenceFileTarget> files;
    DataCodecEncodeConfigurationParams configuration{MakeDefaultEncodeConfigurationParams()};
    std::shared_ptr<IRunRecordSink> runRecordSink;
    CodecResourceParams resources;
    std::stop_token stopToken;
};

struct EncodedSequenceFrame {
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    EncodedBuffer bytes;
    std::filesystem::path path;
    InputMemoryObservation inputMemory;
};

struct FrameSequenceEncodeResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    std::size_t encodedFrameCount{0u};
    std::uint64_t encodedByteCount{0u};
    std::vector<EncodedSequenceFrame> frames;
    std::vector<TelemetryMessageRecord> messages;
};

[[nodiscard]] FrameSequenceEncodeResult EncodeFrameSequence(const FrameSequenceEncodeRequest& request) noexcept;

} // 命名空间 datacodec
#endif
