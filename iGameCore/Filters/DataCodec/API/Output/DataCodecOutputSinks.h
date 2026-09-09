#ifndef DATACODEC_API_OUTPUT_DATACODECOUTPUTSINKS_H
#define DATACODEC_API_OUTPUT_DATACODECOUTPUTSINKS_H

#include "DataCodec/Localization/DataCodecMessageCatalog.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

enum class DataCodecStatusSeverity : std::uint8_t {
    Info = 0u,
    Warning = 1u,
    Error = 2u,
    Critical = 3u,
};

struct DataCodecStatusRecord {
    std::uint64_t runId{0u};
    DataCodecStatusSeverity severity{DataCodecStatusSeverity::Info};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    DataCodecMessageId messageId{DataCodecMessageId::None};
    std::vector<DataCodecMessageArgument> messageArguments;
    std::string text;
    std::string technicalDetail;
};

[[nodiscard]] inline std::string FormatDataCodecStatusText(
    const DataCodecStatusRecord& status) {
    const auto messageText = status.text.empty() &&
            status.messageId != DataCodecMessageId::None
        ? FormatDataCodecMessage(
              status.language,
              status.messageId,
              status.messageArguments)
        : status.text;
    if (status.technicalDetail.empty() ||
        status.technicalDetail == messageText) {
        return messageText;
    }
    if (messageText.empty()) {
        return status.technicalDetail;
    }
    return messageText +
        (status.language == DataCodecLanguage::English ? ": " : "：") +
        status.technicalDetail;
}

enum class DataCodecProgressPhase : std::uint8_t {
    Begin = 0u,
    Update = 1u,
    Finish = 2u,
};

struct DataCodecProgressUpdate {
    std::uint64_t runId{0u};
    DataCodecProgressPhase phase{DataCodecProgressPhase::Update};
    double normalized{0.0};
    bool success{false};
    std::uint32_t frameOrdinal{0u};
    std::uint32_t frameCount{0u};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    DataCodecMessageId messageId{DataCodecMessageId::None};
    std::vector<DataCodecMessageArgument> messageArguments;
    std::string text;
    std::string technicalDetail;
};

[[nodiscard]] inline std::string FormatDataCodecProgressText(
    const DataCodecProgressUpdate& progress) {
    auto messageText = progress.text.empty() &&
            progress.messageId != DataCodecMessageId::None
        ? FormatDataCodecMessage(
              progress.language,
              progress.messageId,
              progress.messageArguments)
        : progress.text;
    if (progress.frameCount <= 1u) {
        return messageText;
    }
    const auto displayOrdinal = progress.frameOrdinal < progress.frameCount
        ? progress.frameOrdinal + 1u
        : progress.frameCount;
    auto frameText = FormatDataCodecMessage(
        progress.language,
        DataCodecMessageId::FrameCounter,
        std::vector<DataCodecMessageArgument>{
            {"index", std::to_string(displayOrdinal)},
            {"count", std::to_string(progress.frameCount)},
        });
    if (!messageText.empty()) {
        frameText += progress.language == DataCodecLanguage::English ? ": " : "：";
        frameText += messageText;
    }
    return frameText;
}

struct DataCodecReportFile {
    std::string name;
    std::string mediaType;
    std::string preferredExtension;
    std::string content;
};

enum class DataCodecReportExportFailure : std::uint8_t {
    None,
    Preparation,
    Write,
};

struct DataCodecReportWriteResult {
    bool success{false};
    std::string path;
    std::string error;
    DataCodecReportExportFailure failure{DataCodecReportExportFailure::None};
};

class IDataCodecUiSink {
public:
    virtual ~IDataCodecUiSink() = default;
    virtual void SubmitUiStatus(const DataCodecStatusRecord& status) = 0;
};

class IDataCodecConsoleSink {
public:
    virtual ~IDataCodecConsoleSink() = default;
    virtual void SubmitConsoleStatus(const DataCodecStatusRecord& status) = 0;
};

class IDataCodecProgressSink {
public:
    virtual ~IDataCodecProgressSink() = default;
    virtual void SubmitProgress(const DataCodecProgressUpdate& progress) = 0;
};

class IDataCodecReportFileSink {
public:
    virtual ~IDataCodecReportFileSink() = default;
    [[nodiscard]] virtual DataCodecReportWriteResult WriteReportFile(
        const DataCodecReportFile& report) = 0;

    // 报告构造、序列化与文件交付共用可选导出边界，失败时只保留固定状态
    template<class Prepare>
    [[nodiscard]] DataCodecReportWriteResult TryWriteReportFile(Prepare&& prepare) noexcept {
        auto failure = DataCodecReportExportFailure::Preparation;
        try {
            auto report = std::forward<Prepare>(prepare)();
            failure = DataCodecReportExportFailure::Write;
            auto result = WriteReportFile(report);
            if (!result.success) {
                result.failure = failure;
                m_exportFailures.fetch_add(1u, std::memory_order_relaxed);
            }
            return result;
        } catch (...) {
            m_exportFailures.fetch_add(1u, std::memory_order_relaxed);
            return {.failure = failure};
        }
    }

    [[nodiscard]] std::uint64_t ExportFailureCount() const noexcept {
        return m_exportFailures.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool DiagnosticsIncomplete() const noexcept { return ExportFailureCount() != 0u; }

private:
    std::atomic_uint64_t m_exportFailures{0u};
};

struct DataCodecOutputSinks {
    std::shared_ptr<IDataCodecUiSink> ui;
    std::shared_ptr<IDataCodecConsoleSink> console;
    std::shared_ptr<IDataCodecProgressSink> progress;
    std::shared_ptr<IDataCodecReportFileSink> reportFile;

    [[nodiscard]] bool Empty() const noexcept {
        return ui == nullptr && console == nullptr &&
            progress == nullptr && reportFile == nullptr;
    }
};

} // namespace datacodec

#endif
