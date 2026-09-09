#ifndef DATACODEC_API_ADAPTER_RUNRECORDTYPES_H
#define DATACODEC_API_ADAPTER_RUNRECORDTYPES_H

#include "DataCodec/Localization/DataCodecMessageCatalog.h"

#include <cstdint>
#include <algorithm>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

namespace datacodec {

inline constexpr std::size_t kTelemetryRetainedRecordLimit = 4096u;
inline constexpr std::size_t kTelemetryRetainedTextBytes = 2048u;
inline constexpr std::size_t kTelemetryRetainedArgumentLimit = 32u;

struct TelemetryRetentionStats {
    std::uint64_t omittedSessions{0u};
    std::uint64_t omittedRecords{0u};
    std::uint64_t truncatedText{0u};
    std::uint64_t exportFailures{0u};
};

class TelemetryTextCopy final {
public:
    std::string Copy(std::string_view source) {
        auto bytes = std::min(source.size(), m_remaining);
        if (bytes < source.size()) {
            while (bytes != 0u && (static_cast<unsigned char>(source[bytes]) & 0xc0u) == 0x80u) { --bytes; }
            m_truncated = true;
        }
        m_remaining -= bytes;
        return std::string(source.substr(0u, bytes));
    }
    bool Truncated() const noexcept { return m_truncated; }
    void MarkTruncated() noexcept { m_truncated = true; }
private:
    std::size_t m_remaining{kTelemetryRetainedTextBytes};
    bool m_truncated{false};
};

enum class TelemetryRunKind : std::uint8_t {
    Unknown = 0,
    Encode = 1,
    Decode = 2,
};

inline const char* TelemetryRunKindName(const TelemetryRunKind kind) {
    switch (kind) {
        case TelemetryRunKind::Encode:
            return "encode";
        case TelemetryRunKind::Decode:
            return "decode";
        case TelemetryRunKind::Unknown:
        default:
            return "unknown";
    }
}

enum class TelemetryStageCategory : std::uint8_t {
    General = 0,
    Topology = 1,
    Params = 2,
    Geometry = 3,
    Attribute = 4,
    Remap = 5,
    Commit = 6,
};

inline const char* TelemetryStageCategoryName(const TelemetryStageCategory category) {
    switch (category) {
        case TelemetryStageCategory::Topology:
            return "topology";
        case TelemetryStageCategory::Params:
            return "params";
        case TelemetryStageCategory::Geometry:
            return "geometry";
        case TelemetryStageCategory::Attribute:
            return "attribute";
        case TelemetryStageCategory::Remap:
            return "remap";
        case TelemetryStageCategory::Commit:
            return "commit";
        case TelemetryStageCategory::General:
        default:
            return "general";
    }
}

inline TelemetryStageCategory ResolveTelemetryStageCategory(const std::string_view stageName) {
    if (stageName.find("Params") != std::string_view::npos) {
        return TelemetryStageCategory::Params;
    }
    if (stageName.find("Geometry") != std::string_view::npos) {
        return TelemetryStageCategory::Geometry;
    }
    if (stageName.find("Topo") != std::string_view::npos) {
        return TelemetryStageCategory::Topology;
    }
    if (stageName.find("Attr") != std::string_view::npos ||
        stageName.find("Attribute") != std::string_view::npos) {
        return TelemetryStageCategory::Attribute;
    }
    if (stageName.find("Remap") != std::string_view::npos ||
        stageName.find("SpatialPartition") != std::string_view::npos) {
        return TelemetryStageCategory::Remap;
    }
    if (stageName.find("Commit") != std::string_view::npos) {
        return TelemetryStageCategory::Commit;
    }
    return TelemetryStageCategory::General;
}

enum class TelemetryMessageSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2,
    Critical = 3,
};

inline const char* TelemetryMessageSeverityName(const TelemetryMessageSeverity severity) noexcept {
    switch (severity) {
        case TelemetryMessageSeverity::Warning:
            return "warning";
        case TelemetryMessageSeverity::Error:
            return "error";
        case TelemetryMessageSeverity::Critical:
            return "critical";
        case TelemetryMessageSeverity::Info:
        default:
            return "info";
    }
}

struct TelemetryMessageRecord {
    std::uint64_t order{0};
    TelemetryMessageSeverity severity{TelemetryMessageSeverity::Info};
    std::string origin;
    std::string code;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    DataCodecMessageId messageId{DataCodecMessageId::None};
    std::vector<DataCodecMessageArgument> messageArguments;
    std::string text;
    std::string technicalDetail;
    bool textTruncated{false};
};

inline TelemetryMessageRecord CopyRetainedTelemetryMessage(const TelemetryMessageRecord& source) {
    TelemetryTextCopy text;
    TelemetryMessageRecord output;
    output.order = source.order;
    output.severity = source.severity;
    output.language = source.language;
    output.messageId = source.messageId;
    output.origin = text.Copy(source.origin);
    output.code = text.Copy(source.code);
    output.text = text.Copy(source.text);
    output.technicalDetail = text.Copy(source.technicalDetail);
    const auto arguments = std::min(source.messageArguments.size(), kTelemetryRetainedArgumentLimit);
    output.messageArguments.reserve(arguments);
    for (std::size_t i = 0u; i < arguments; ++i) {
        output.messageArguments.push_back({text.Copy(source.messageArguments[i].name), text.Copy(source.messageArguments[i].value)});
    }
    if (arguments != source.messageArguments.size()) { text.MarkTruncated(); }
    output.textTruncated = source.textTruncated || text.Truncated();
    return output;
}

inline void AppendRetainedTelemetryMessage(std::vector<TelemetryMessageRecord>& messages,
    const TelemetryMessageRecord& message, TelemetryRetentionStats* stats = nullptr) {
    if (messages.size() >= kTelemetryRetainedRecordLimit) {
        if (stats) { ++stats->omittedRecords; }
        return;
    }
    auto retained = CopyRetainedTelemetryMessage(message);
    const bool truncated = retained.textTruncated;
    messages.push_back(std::move(retained));
    if (stats && truncated) { ++stats->truncatedText; }
}

inline void AppendRetainedTelemetryMessages(std::vector<TelemetryMessageRecord>& messages,
    const std::vector<TelemetryMessageRecord>& source, TelemetryRetentionStats* stats = nullptr) {
    const auto count = std::min(source.size(), kTelemetryRetainedRecordLimit -
        std::min(messages.size(), kTelemetryRetainedRecordLimit));
    for (std::size_t i = 0u; i < count; ++i) { AppendRetainedTelemetryMessage(messages, source[i], stats); }
    if (stats) { stats->omittedRecords += source.size() - count; }
}

struct TelemetryArtifactRecord {
    std::uint64_t order{0};
    std::string name;
    std::string mediaType;
    std::string preferredExtension;
    std::string text;
};

enum class TelemetryCapacityCoverage : std::uint8_t { None, OwnedStorageArrays, RetainedScratch, SampledBuffer };

struct TelemetryResourceUsage {
    bool valid{false};
    std::uint64_t logicalBytes{0};
    std::uint64_t privateBytes{0};
    std::uint64_t workingSetBytes{0};
    std::uint64_t workingSetBeforeBytes{0};
    std::uint64_t workingSetAfterBytes{0};
    std::uint64_t sampledPeakWorkingSetBytes{0};
    TelemetryCapacityCoverage capacityCoverage{TelemetryCapacityCoverage::None};
    std::uint64_t capacityScopeId{0u};
    std::uint64_t capacitySampleNanoseconds{0u};
    std::optional<std::uint64_t> trackedCapacityBytes;
    std::optional<std::uint64_t> eventPeakCapacityBytes;
    std::optional<std::uint64_t> sampledPeakCapacityBytes;
};

inline TelemetryResourceUsage MakeLogicalTelemetryResourceUsage(const std::uint64_t logicalBytes) {
    TelemetryResourceUsage usage;
    usage.valid = true;
    usage.logicalBytes = logicalBytes;
    return usage;
}

struct TelemetryStageRecord {
    std::string name;
    std::uint64_t order{0};
    std::uint64_t sampleCount{1u};
    double elapsedMs{0.0};
    TelemetryStageCategory category{TelemetryStageCategory::General};
    std::string scope;
    TelemetryResourceUsage resource;
};

} // 命名空间 datacodec

#endif
