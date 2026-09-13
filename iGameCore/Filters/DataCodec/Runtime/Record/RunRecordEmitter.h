#ifndef DATACODEC_RUNTIME_RECORD_RUNRECORDEMITTER_H
#define DATACODEC_RUNTIME_RECORD_RUNRECORDEMITTER_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

[[nodiscard]] inline std::uint64_t NextRunRecordId() noexcept {
    static std::atomic_uint64_t nextId{1u};
    return nextId.fetch_add(1u, std::memory_order_relaxed);
}

class RunRecordEmitter {
public:
    explicit RunRecordEmitter(DataCodecExecutionResources* resources = nullptr) noexcept : m_resources(resources) {}

    [[nodiscard]] std::uint64_t ExportFailureCount() const noexcept {
        return m_exportFailures.load(std::memory_order_relaxed);
    }

    template<class Function>
    void TryExport(Function&& function) const noexcept {
        try { std::forward<Function>(function)(); }
        catch (...) { ReportExportFailure(); }
    }

    void Reset(RunRecordInfo run, IRunRecordSink* sink) {
        if (run.runId == 0u) {
            run.runId = NextRunRecordId();
        }
        m_run = std::move(run);
        m_sink = sink;
        m_messageOrder.store(0u, std::memory_order_relaxed);
        m_stageOrder.store(0u, std::memory_order_relaxed);
        m_artifactOrder.store(0u, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_messageMutex);
        m_messages.clear();
        m_messageRetention = {};
        m_exportFailures.store(0u, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t RunId() const noexcept {
        return m_run.runId;
    }

    [[nodiscard]] const RunRecordInfo& RunInfo() const noexcept {
        return m_run;
    }

    [[nodiscard]] DataCodecLanguage Language() const noexcept {
        return m_run.language;
    }

    [[nodiscard]] bool Wants(const RunRecordKind kind) const noexcept {
        return m_sink != nullptr && m_sink->Wants(kind);
    }

    [[nodiscard]] bool Requests(const RunCollectionKind kind) const noexcept {
        return m_sink != nullptr && m_sink->Requests(kind);
    }

    void BeginRun() const try {
        Submit(RunRecordKind::RunBegin, RunRecord{RunBeginRecord{m_run}});
    } catch (...) { ReportExportFailure(); }

    void EndRun(const RunEndRecord& input) const try {
        auto record = input;
        record.run = m_run;
        Submit(RunRecordKind::RunEnd, RunRecord{std::move(record)});
    } catch (...) { ReportExportFailure(); }

    // 请求的固定失败结果和清理已完成，丰富报告为锁外可选导出
    void TryEndFailedRun(const CodecFailureRecord& failure, const RunEndRecord& record = {}) noexcept {
        try {
            AddMessage(MakeCodecTelemetryMessage(
                std::string(failure.origin.data()), failure.code, std::string(failure.message.data())));
        } catch (...) {
            ReportExportFailure();
        }
        try {
            auto completion = record;
            completion.success = false;
            EndRun(std::move(completion));
        } catch (...) {
            ReportExportFailure();
        }
    }

    void SubmitProgress(RunProgressRecord record) const try {
        record.runId = m_run.runId;
        Submit(RunRecordKind::Progress, RunRecord{std::move(record)});
    } catch (...) { ReportExportFailure(); }

    void SubmitProgress(
        const RunProgressPhase phase,
        const double normalized,
        const DataCodecMessageId messageId,
        std::initializer_list<DataCodecMessageArgument> arguments = {},
        const bool success = false,
        std::string technicalDetail = {}) const try {
        auto message = LocalizeDataCodecMessage(
            m_run.language,
            messageId,
            arguments,
            std::move(technicalDetail));
        SubmitProgress(RunProgressRecord{
            .phase = phase,
            .normalized = normalized,
            .language = message.language,
            .messageId = message.id,
            .messageArguments = std::move(message.arguments),
            .text = std::move(message.text),
            .technicalDetail = std::move(message.technicalDetail),
            .success = success,
        });
    } catch (...) { ReportExportFailure(); }

    void AddLocalizedMessage(
        const TelemetryMessageSeverity severity,
        std::string origin,
        const DataCodecMessageId messageId,
        std::initializer_list<DataCodecMessageArgument> arguments = {},
        std::string code = {},
        std::string technicalDetail = {}) try {
        auto message = LocalizeDataCodecMessage(
            m_run.language,
            messageId,
            arguments,
            std::move(technicalDetail));
        AddMessage(TelemetryMessageRecord{
            .severity = severity,
            .origin = std::move(origin),
            .code = std::move(code),
            .language = message.language,
            .messageId = message.id,
            .messageArguments = std::move(message.arguments),
            .text = std::move(message.text),
            .technicalDetail = std::move(message.technicalDetail),
        });
    } catch (...) { ReportExportFailure(); }

    void AddMessage(TelemetryMessageRecord message) try {
        // 原始诊断继承请求语言，已本地化的消息保留自身语言
        if (message.messageId == DataCodecMessageId::None) {
            message.language = m_run.language;
        }
        message.order = m_messageOrder.fetch_add(1u, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            try { AppendRetainedTelemetryMessage(m_messages, message, &m_messageRetention); }
            catch (...) { ReportExportFailure(); }
        }
        Submit(
            RunRecordKind::Message,
            RunRecord{RunMessageRecord{
                .runId = m_run.runId,
                .runKind = m_run.runKind,
                .message = std::move(message),
            }});
    } catch (...) { ReportExportFailure(); }

    [[nodiscard]] std::vector<TelemetryMessageRecord> TakeMessages() noexcept {
        std::vector<TelemetryMessageRecord> messages;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            messages.swap(m_messages);
        }
        // order 在接收时唯一赋值，原地排序无需 stable_sort 的临时数组
        std::sort(
            messages.begin(),
            messages.end(),
            [](const auto& left, const auto& right) { return left.order < right.order; });
        return messages;
    }

    [[nodiscard]] TelemetryRetentionStats MessageRetention() const {
        std::lock_guard lock(m_messageMutex);
        return m_messageRetention;
    }

    void AddInfo(std::string origin, std::string text) try {
        AddMessage(TelemetryMessageRecord{
            .severity = TelemetryMessageSeverity::Info,
            .origin = std::move(origin),
            .text = std::move(text),
        });
    } catch (...) { ReportExportFailure(); }

    void AddWarning(std::string origin, std::string text) try {
        AddMessage(TelemetryMessageRecord{
            .severity = TelemetryMessageSeverity::Warning,
            .origin = std::move(origin),
            .text = std::move(text),
        });
    } catch (...) { ReportExportFailure(); }

    void AddError(std::string origin, std::string text) try {
        AddMessage(TelemetryMessageRecord{
            .severity = TelemetryMessageSeverity::Error,
            .origin = std::move(origin),
            .text = std::move(text),
        });
    } catch (...) { ReportExportFailure(); }

    void RecordStageTiming(
        std::string stageName,
        const double elapsedMs,
        const TelemetryStageCategory category = TelemetryStageCategory::General,
        std::string scope = {}) try {
        TelemetryStageRecord stage{
            .name = std::move(stageName),
            .order = m_stageOrder.fetch_add(1u, std::memory_order_relaxed),
            .elapsedMs = elapsedMs,
            .category = category,
            .scope = std::move(scope),
        };
        Submit(
            RunRecordKind::StageTiming,
            RunRecord{RunStageTimingRecord{m_run.runId, std::move(stage)}});
    } catch (...) { ReportExportFailure(); }

    void RecordResourceUsage(
        std::string stageName,
        const std::uint64_t logicalBytes,
        const TelemetryStageCategory category = TelemetryStageCategory::General,
        std::string scope = {}) try {
        RecordResourceUsage(
            std::move(stageName),
            MakeLogicalTelemetryResourceUsage(logicalBytes),
            category,
            std::move(scope));
    } catch (...) { ReportExportFailure(); }

    void RecordResourceUsage(
        std::string stageName,
        TelemetryResourceUsage resource,
        const TelemetryStageCategory category = TelemetryStageCategory::General,
        std::string scope = {}) try {
        TelemetryStageRecord stage{
            .name = std::move(stageName),
            .order = m_stageOrder.fetch_add(1u, std::memory_order_relaxed),
            .category = category,
            .scope = std::move(scope),
            .resource = std::move(resource),
        };
        Submit(
            RunRecordKind::ResourceUsage,
            RunRecord{RunResourceUsageRecord{m_run.runId, std::move(stage)}});
    } catch (...) { ReportExportFailure(); }

    void AddArtifact(TelemetryArtifactRecord artifact) try {
        artifact.order = m_artifactOrder.fetch_add(1u, std::memory_order_relaxed);
        Submit(
            RunRecordKind::Artifact,
            RunRecord{RunArtifactRecord{m_run.runId, std::move(artifact)}});
    } catch (...) { ReportExportFailure(); }

    void RecordRemapOrder(
        BlockPath leafPath,
        const RunRemapDomain domain,
        std::shared_ptr<const IRemapProvider> provider) const try {
        Submit(
            RunRecordKind::RemapOrder,
            RunRecord{RunRemapOrderRecord{
                .runId = m_run.runId,
                .leafPath = std::move(leafPath),
                .domain = domain,
                .provider = std::move(provider),
            }});
    } catch (...) { ReportExportFailure(); }

private:
    void ReportExportFailure() const noexcept {
        m_exportFailures.fetch_add(1u, std::memory_order_relaxed);
        if (m_resources != nullptr) { m_resources->RecordDiagnosticExportFailure(); }
    }

    void Submit(const RunRecordKind kind, const RunRecord& record) const noexcept {
        if (m_sink != nullptr && m_sink->Wants(kind)) {
            if (!m_sink->TrySubmit(record)) { ReportExportFailure(); }
        }
    }

    RunRecordInfo m_run;
    IRunRecordSink* m_sink{nullptr};
    DataCodecExecutionResources* m_resources{nullptr};
    mutable std::atomic_uint64_t m_exportFailures{0u};
    std::atomic_uint64_t m_messageOrder{0u};
    std::atomic_uint64_t m_stageOrder{0u};
    std::atomic_uint64_t m_artifactOrder{0u};
    mutable std::mutex m_messageMutex;
    std::vector<TelemetryMessageRecord> m_messages;
    TelemetryRetentionStats m_messageRetention;
};

// 粗粒度墙钟区间，包含区间内的等待和子调用，失败退出也记录已消耗时间
// 名称使用静态字符串，关闭计时时不读取时钟、不分配字符串
class ScopedRunStageTiming final {
public:
    ScopedRunStageTiming(RunRecordEmitter& records, const std::string_view name,
        const TelemetryStageCategory category = TelemetryStageCategory::General) noexcept
        : m_records(records), m_name(name), m_category(category),
          m_enabled(records.Wants(RunRecordKind::StageTiming)),
          m_start(callback::StartTiming(m_enabled)) {}
    ScopedRunStageTiming(const ScopedRunStageTiming&) = delete;
    ScopedRunStageTiming& operator=(const ScopedRunStageTiming&) = delete;
    ~ScopedRunStageTiming() noexcept {
        if (!m_enabled) { return; }
        const auto elapsedMs = callback::ElapsedMilliseconds(m_start);
        m_records.TryExport([&] {
            m_records.RecordStageTiming(std::string(m_name), elapsedMs, m_category, "module-wall");
        });
    }
private:
    RunRecordEmitter& m_records;
    std::string_view m_name;
    TelemetryStageCategory m_category;
    bool m_enabled;
    callback::PhaseTimePoint m_start;
};

} // 命名空间 datacodec

#endif
