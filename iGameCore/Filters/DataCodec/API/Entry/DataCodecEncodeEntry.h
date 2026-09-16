#ifndef DATACODEC_API_ENTRY_DATACODECENCODEENTRY_H
#define DATACODEC_API_ENTRY_DATACODECENCODEENTRY_H

#include "DataCodec/API/Adapter/IBlockTreeAdapter.h"
#include "DataCodec/API/Adapter/IEncodeAdapter.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Output/DataCodecOutputSinks.h"
#include "DataCodec/API/Output/EncodedBuffer.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Params/CodecResourceParams.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace datacodec {

enum class EncodePackageKind : std::uint8_t {
    Auto,
    LeafPackage,
    FramePackage,
};

struct EncodeInput {
    // 适配器共同持有原始数组、getter 和访问元数据，读取期间由宿主保持内容稳定
    using Adapter = std::variant<std::monostate, std::shared_ptr<IEncodeAdapter>, std::shared_ptr<IBlockTreeAdapter>>;

    Adapter adapter;
    std::string objectName;
    std::string meshType;
    BlockPath leafPath;
    std::string rootName;
    std::uint32_t frameIndex{0u};
    std::uint32_t frameCount{1u};
    float timeValue{0.0f};

    [[nodiscard]] static EncodeInput LeafAdapter(
        std::shared_ptr<IEncodeAdapter> inputAdapter,
        BlockPath path = {},
        std::string name = {},
        std::string type = {},
        std::uint32_t inputFrameIndex = 0u);

    [[nodiscard]] static EncodeInput BlockTreeAdapter(
        std::shared_ptr<IBlockTreeAdapter> inputAdapter,
        std::string name = {},
        std::uint32_t inputFrameIndex = 0u,
        std::uint32_t inputFrameCount = 1u,
        float inputTimeValue = 0.0f);
};

struct EncodeOutput {
    using Target = std::variant<std::monostate, std::filesystem::path>;

    EncodePackageKind packageKind{EncodePackageKind::Auto};
    Target target;

    // 将完整编码包返回到 EncodeResult::encodedBytes
    [[nodiscard]] static EncodeOutput Memory(
        EncodePackageKind kind = EncodePackageKind::Auto);

    // 核心负责创建文件输出及完成后的关闭与发布
    [[nodiscard]] static EncodeOutput File(
        std::filesystem::path path,
        EncodePackageKind kind = EncodePackageKind::Auto);
};

struct EncodeRequest {
    EncodeInput input;
    EncodeOutput output;
    AttributeSelectionMode attributeSelection{AttributeSelectionMode::AllAvailable};
    std::vector<AttributeTarget> attributeTargets;
    DataCodecEncodeConfigurationParams configuration{MakeDefaultEncodeConfigurationParams()};
    DataCodecOutputSinks outputSinks;
    std::shared_ptr<IRunRecordSink> runRecordSink;
    CodecResourceParams resources;
    std::stop_token stopToken;
};

struct EncodeResult {
    bool success{false};
    bool hasEncodedOutput{false};
    EncodedBuffer encodedBytes;
    std::uint64_t encodedByteCount{0u};
    std::size_t leafCount{0u};
    EncodePackageKind packageKind{EncodePackageKind::Auto};
    std::vector<TelemetryMessageRecord> messages;
    std::optional<CodecFailureRecord> failure;
    InputMemoryObservation inputMemory;
};

[[nodiscard]] EncodeResult Encode(const EncodeRequest& request);

} // namespace datacodec

#endif
