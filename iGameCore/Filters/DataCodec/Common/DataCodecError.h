#ifndef DATACODEC_COMMON_DATACODECERROR_H
#define DATACODEC_COMMON_DATACODECERROR_H

#include "DataCodec/API/Adapter/RunRecordTypes.h"
#include "DataCodec/Common/DataCodecTypes.h"

#include <algorithm>
#include <cassert>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
namespace datacodec {

enum class CodecErrorCode {
    InvalidInput,
    MissingInput,
    InvalidTopology,
    InvalidRemap,
    PipelineFailure,
    EncodeFailure,
    DecodeFailure,
    UnsupportedFormat,
    UnsupportedPlatform,
};

struct CodecFailureRecord {
    CodecErrorCode code{CodecErrorCode::PipelineFailure};
    bool cancelled{false};
    std::array<char, 32> reason{};
    std::array<char, 64> origin{};
    std::array<char, 256> message{};
    bool textTruncated{false};
    std::optional<std::uint64_t> requestedBytes;
    std::optional<std::uint64_t> reservedBytes;
    std::optional<std::uint64_t> limitBytes;
};

static_assert(sizeof(CodecFailureRecord) <= 512u);
static_assert(std::is_trivially_copyable_v<CodecFailureRecord>);
static_assert(std::is_nothrow_move_constructible_v<CodecFailureRecord>);

namespace failuredetail {

// 错误路径只写固定数组，完整保留 UTF-8 字符并预留结尾零字节
template<std::size_t N>
inline bool CopyText(std::array<char, N>& output, const std::string_view text) noexcept {
    static_assert(N > 0u);
    output.fill('\0');
    auto count = std::min(text.size(), N - 1u);
    if (count < text.size()) {
        while (count != 0u &&
               (static_cast<unsigned char>(text[count]) & 0xc0u) == 0x80u) {
            --count;
        }
    }
    for (std::size_t index = 0u; index < count; ++index) {
        output[index] = text[index];
    }
    return count != text.size();
}

} // 失败文本工具命名空间

[[nodiscard]] inline CodecFailureRecord MakeCodecFailureRecord(
    const CodecErrorCode code,
    const std::string_view reason,
    const std::string_view origin,
    const std::string_view message,
    const bool cancelled = false) noexcept {
    CodecFailureRecord record;
    record.code = code;
    record.cancelled = cancelled;
    record.textTruncated = failuredetail::CopyText(record.reason, reason);
    record.textTruncated |= failuredetail::CopyText(record.origin, origin);
    record.textTruncated |= failuredetail::CopyText(record.message, message);
    return record;
}

struct CodecStatus {
    bool success{true};
    CodecErrorCode code{CodecErrorCode::PipelineFailure};
    std::string message;

    explicit operator bool() const noexcept { return success; }

    [[nodiscard]] static CodecStatus Ok() { return {}; }

    [[nodiscard]] static CodecStatus Failure(
        const CodecErrorCode code,
        std::string message) {
        return CodecStatus{
            .success = false,
            .code = code,
            .message = std::move(message),
        };
    }
};

inline const char* CodecErrorCodeName(const CodecErrorCode code) noexcept {
    switch (code) {
        case CodecErrorCode::InvalidInput:
            return "invalid-input";
        case CodecErrorCode::MissingInput:
            return "missing-input";
        case CodecErrorCode::InvalidTopology:
            return "invalid-topology";
        case CodecErrorCode::InvalidRemap:
            return "invalid-remap";
        case CodecErrorCode::PipelineFailure:
            return "pipeline-failure";
        case CodecErrorCode::EncodeFailure:
            return "encode-failure";
        case CodecErrorCode::DecodeFailure:
            return "decode-failure";
        case CodecErrorCode::UnsupportedFormat:
            return "unsupported-format";
        case CodecErrorCode::UnsupportedPlatform:
            return "unsupported-platform";
    }
    return "unknown";
}

inline std::string FormatCodecError(
    const CodecErrorCode code,
    const std::string_view message) {
    std::string output;
    output.reserve(message.size() + 32u);
    output.push_back('[');
    output.append(CodecErrorCodeName(code));
    output.append("] ");
    output.append(message);
    return output;
}

inline TelemetryMessageRecord MakeCodecTelemetryMessage(
    std::string origin,
    const CodecErrorCode code,
    std::string text) {
    return TelemetryMessageRecord{
        .severity = TelemetryMessageSeverity::Error,
        .origin = std::move(origin),
        .code = CodecErrorCodeName(code),
        .text = std::move(text),
    };
}

inline bool SetCodecError(
    std::string* error,
    const CodecErrorCode code,
    const std::string_view message) {
    if (error != nullptr) {
        *error = FormatCodecError(code, message);
    }
    return false;
}

inline bool SetCodecError(
    std::string* error,
    const CodecErrorCode code,
    const char* message) {
    assert(message != nullptr);
    return SetCodecError(error, code, std::string_view(message));
}

inline bool SetCodecError(
    std::string* error,
    const CodecErrorCode code,
    const std::string& message) {
    return SetCodecError(error, code, std::string_view(message));
}

inline std::string FormatStageCodecError(
    const std::string_view stageName,
    const CodecErrorCode code,
    const std::string_view message) {
    std::string output;
    output.reserve(stageName.size() + message.size() + 36u);
    output.append(stageName);
    output.append(": ");
    output.append(FormatCodecError(code, message));
    return output;
}

// 丰富文本只在结果展示和可选报告阶段构造
inline std::string FormatCodecFailure(const CodecFailureRecord& failure) {
    return FormatStageCodecError(
        std::string_view(failure.origin.data()),
        failure.code,
        std::string_view(failure.message.data()));
}

inline std::string FormatStageCodecError(
    const std::string_view stageName,
    const CodecErrorCode code,
    const std::string& message) {
    return FormatStageCodecError(stageName, code, std::string_view(message));
}

inline std::string FormatStageCodecError(
    const std::string_view stageName,
    const CodecErrorCode code,
    const char* message) {
    assert(message != nullptr);
    return FormatStageCodecError(stageName, code, std::string_view(message));
}

} // namespace datacodec

#endif
