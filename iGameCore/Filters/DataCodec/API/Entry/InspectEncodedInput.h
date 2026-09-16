#ifndef DATACODEC_API_ENTRY_INSPECTENCODEDINPUT_H
#define DATACODEC_API_ENTRY_INSPECTENCODEDINPUT_H

#include "DataCodec/API/Input/EncodedInput.h"
#include "DataCodec/API/Adapter/DecodeCacheIdentity.h"
#include "DataCodec/Common/DataCodecError.h"
#include <stop_token>

namespace datacodec {

enum class EncodedPackageKind { Unknown, Leaf, Frame };

struct EncodedInputInspection {
    bool success{false};
    EncodedPackageKind kind{EncodedPackageKind::Unknown};
    std::uint64_t byteSize{0u};
    std::uint16_t version{0u};
    std::string contentIdentity;
    DecodeSourceIdentity sourceIdentity;
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    std::optional<CodecFailureRecord> failure;
};

// 检查输入与帧描述，返回值仅保存元数据，返回前关闭本次范围 IO
[[nodiscard]] EncodedInputInspection InspectEncodedInput(
    const EncodedInput& input, std::stop_token stop = {});

// 仅检查固定包头，同步借用前缀，不读取帧目录
[[nodiscard]] EncodedInputInspection InspectEncodedPrefix(
    std::span<const std::uint8_t> prefix, std::uint64_t totalBytes);

}
#endif
