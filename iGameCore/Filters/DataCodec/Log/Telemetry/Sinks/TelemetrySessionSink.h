#ifndef DATACODEC_LOG_TELEMETRY_SINKS_TELEMETRYSESSIONSINK_H
#define DATACODEC_LOG_TELEMETRY_SINKS_TELEMETRYSESSIONSINK_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Log/Telemetry/TelemetrySession.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace datacodec {

inline constexpr RunRecordMask kTelemetrySessionRecordMask =
    kRunLifecycleRecordMask | RunRecordKind::Message | RunRecordKind::StageTiming |
    RunRecordKind::ResourceUsage | RunRecordKind::Artifact;

enum class TelemetrySessionDetail : std::uint8_t { Full, ProcessSummary };

class TelemetrySessionSink final : public IRunRecordSink {
public:
    static constexpr std::size_t kSessionLimit = 256u;
    static constexpr std::size_t kRecordLimit = kTelemetryRetainedRecordLimit;
    static constexpr std::size_t kTextLimit = kTelemetryRetainedTextBytes;

    explicit TelemetrySessionSink(
        RunRecordMask interests = kRunLifecycleRecordMask | RunRecordKind::Message,
        RunCollectionMask collectionRequests = 0u,
        TelemetrySessionDetail detail = TelemetrySessionDetail::Full)
        : m_interests((interests & kTelemetrySessionRecordMask) | kRunLifecycleRecordMask),
          m_collectionRequests(collectionRequests), m_detail(detail) {}

    RunRecordMask Interests() const noexcept override { return m_interests; }
    RunCollectionMask CollectionRequests() const noexcept override { return m_collectionRequests; }

    void Submit(const RunRecord& record) override try {
        std::lock_guard lock(m_mutex);
        std::visit([this](const auto& value) { Consume(value); }, record);
    } catch (...) { RecordExportFailure(); }

    TelemetryRetentionStats RetentionStats() const {
        std::lock_guard lock(m_mutex);
        return CaptureStats();
    }
    std::size_t RetainedSessionCount() const {
        std::lock_guard lock(m_mutex);
        return m_sessions.size();
    }
    std::size_t RetainedRecordCount() const {
        std::lock_guard lock(m_mutex);
        return m_recordCount;
    }

    std::optional<TelemetrySession> TakeSession(std::uint64_t runId) {
        std::lock_guard lock(m_mutex);
        const auto found = m_sessions.find(runId);
        if (found == m_sessions.end()) { return std::nullopt; }
        m_recordCount -= DetailCount(found->second);
        auto session = std::move(found->second);
        session.captureRetention = CaptureStats();
        m_sessions.erase(found);
        std::erase(m_completedRunIds, runId);
        return session;
    }

    std::vector<TelemetrySession> SnapshotCompletedSessions() const try {
        std::lock_guard lock(m_mutex);
        std::vector<TelemetrySession> sessions;
        sessions.reserve(m_completedRunIds.size());
        for (auto runId : m_completedRunIds) {
            const auto found = m_sessions.find(runId);
            if (found != m_sessions.end()) {
                sessions.push_back(found->second);
                sessions.back().captureRetention = CaptureStats();
            }
        }
        std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) { return a.runId < b.runId; });
        return sessions;
    } catch (...) { RecordExportFailure(); return {}; }

    std::vector<TelemetryMessageRecord> SnapshotMessages() const try {
        std::lock_guard lock(m_mutex);
        auto messages = m_unscopedMessages;
        auto runIds = m_completedRunIds;
        std::sort(runIds.begin(), runIds.end());
        for (auto runId : runIds) {
            const auto found = m_sessions.find(runId);
            if (found != m_sessions.end()) {
                messages.insert(messages.end(), found->second.messages.begin(), found->second.messages.end());
            }
        }
        return messages;
    } catch (...) { RecordExportFailure(); return {}; }

    std::vector<TelemetryMessageRecord> TakeUnscopedMessages() {
        std::lock_guard lock(m_mutex);
        std::vector<TelemetryMessageRecord> messages;
        messages.swap(m_unscopedMessages);
        m_recordCount -= messages.size();
        return messages;
    }

private:
    TelemetryRetentionStats CaptureStats() const noexcept {
        auto stats = m_retention;
        stats.exportFailures = ExportFailureCount();
        return stats;
    }

    static std::size_t DetailCount(const TelemetrySession& session) noexcept {
        return session.messages.size() + session.stages.size() + session.artifacts.size();
    }
    bool HasRecordSpace() noexcept {
        if (m_recordCount < kRecordLimit) { return true; }
        ++m_retention.omittedRecords;
        return false;
    }
    void CopyRunInfo(TelemetrySession& session, const RunRecordInfo& info) {
        TelemetryTextCopy text;
        session.runId = info.runId;
        session.parentRunId = info.parentRunId;
        session.runKind = info.runKind;
        session.generatedAtUtc = text.Copy(info.generatedAtUtc);
        session.objectName = text.Copy(info.objectName);
        session.leafPath = text.Copy(info.leafPath);
        session.meshType = text.Copy(info.meshType);
        if (text.Truncated()) { ++m_retention.truncatedText; }
    }
    bool KeepStageTiming(const TelemetryStageRecord& stage) const noexcept {
        return m_detail == TelemetrySessionDetail::Full ||
            (stage.name.find('[') == std::string::npos &&
             stage.name.find(".connectivity.") == std::string::npos &&
             stage.name.find(".block_") == std::string::npos &&
             stage.name.find(".component") == std::string::npos);
    }
    bool KeepResourceUsage(const TelemetryStageRecord& stage) const noexcept {
        return stage.resource.capacityCoverage != TelemetryCapacityCoverage::None ||
            m_detail == TelemetrySessionDetail::Full || stage.name.starts_with("memory.");
    }
    TelemetryStageRecord CopyStage(const TelemetryStageRecord& stage) {
        TelemetryTextCopy text;
        TelemetryStageRecord output;
        output.name = text.Copy(stage.name);
        output.scope = text.Copy(stage.scope);
        output.order = stage.order;
        output.sampleCount = stage.sampleCount;
        output.elapsedMs = stage.elapsedMs;
        output.category = stage.category;
        output.resource = stage.resource;
        if (text.Truncated()) { ++m_retention.truncatedText; }
        return output;
    }
    void AddFoldedStageTiming(TelemetrySession& session, const TelemetryStageRecord& stage) {
        auto category = stage.category;
        if (category == TelemetryStageCategory::General) { category = ResolveTelemetryStageCategory(stage.name); }
        const auto name = std::string("folded.") + TelemetryStageCategoryName(category);
        const auto found = std::find_if(session.stages.begin(), session.stages.end(),
            [&name](const auto& item) { return item.name == name; });
        if (found != session.stages.end()) {
            found->elapsedMs = std::max(found->elapsedMs, stage.elapsedMs);
            found->sampleCount += std::max<std::uint64_t>(stage.sampleCount, 1u);
            return;
        }
        if (!HasRecordSpace()) { return; }
        TelemetryStageRecord folded;
        folded.name = name;
        folded.category = category;
        folded.order = stage.order;
        folded.elapsedMs = stage.elapsedMs;
        folded.sampleCount = std::max<std::uint64_t>(stage.sampleCount, 1u);
        session.AddStageRecord(std::move(folded));
        ++m_recordCount;
    }

    void Consume(const RunBeginRecord& record) {
        const auto previous = m_sessions.find(record.run.runId);
        if (previous != m_sessions.end()) {
            m_recordCount -= DetailCount(previous->second);
            m_sessions.erase(previous);
            std::erase(m_completedRunIds, record.run.runId);
        }
        if (m_sessions.size() == kSessionLimit) {
            if (m_completedRunIds.empty()) {
                ++m_retention.omittedSessions;
                return;
            }
            const auto oldest = m_completedRunIds.front();
            const auto found = m_sessions.find(oldest);
            if (found != m_sessions.end()) {
                const auto count = DetailCount(found->second);
                m_recordCount -= count;
                m_retention.omittedRecords += count;
                m_sessions.erase(found);
            }
            m_completedRunIds.erase(m_completedRunIds.begin());
            ++m_retention.omittedSessions;
        }
        TelemetrySession session;
        CopyRunInfo(session, record.run);
        m_sessions.emplace(record.run.runId, std::move(session));
    }

    void Consume(const RunEndRecord& record) {
        const auto found = m_sessions.find(record.run.runId);
        if (found == m_sessions.end()) { return; }
        auto& session = found->second;
        session.success = record.success;
        session.elapsedMs = record.elapsedMs;
        session.inputBytes = record.inputBytes;
        session.outputBytes = record.outputBytes;
        session.sourceBytes = record.sourceBytes;
        session.topologyBytes = record.topologyBytes;
        session.compressedPayloadBytes = record.compressedPayloadBytes;
        // order 已由发布端赋值，原地排序不保留第二份明细
        const auto byOrder = [](const auto& a, const auto& b) { return a.order < b.order; };
        std::sort(session.messages.begin(), session.messages.end(), byOrder);
        std::sort(session.stages.begin(), session.stages.end(), byOrder);
        std::sort(session.artifacts.begin(), session.artifacts.end(), byOrder);
        if (std::find(m_completedRunIds.begin(), m_completedRunIds.end(), record.run.runId) == m_completedRunIds.end()) {
            m_completedRunIds.push_back(record.run.runId);
        }
    }

    void Consume(const RunMessageRecord& record) {
        const auto found = m_sessions.find(record.runId);
        if (found == m_sessions.end() && record.runId != 0u) { ++m_retention.omittedRecords; return; }
        if (!HasRecordSpace()) { return; }
        auto message = CopyRetainedTelemetryMessage(record.message);
        const bool truncated = message.textTruncated;
        if (found != m_sessions.end()) { found->second.AddMessage(std::move(message)); }
        else { m_unscopedMessages.push_back(std::move(message)); }
        ++m_recordCount;
        if (truncated) { ++m_retention.truncatedText; }
    }

    void Consume(const RunStageTimingRecord& record) {
        const auto found = m_sessions.find(record.runId);
        if (found == m_sessions.end()) { ++m_retention.omittedRecords; return; }
        if (!KeepStageTiming(record.stage)) { AddFoldedStageTiming(found->second, record.stage); return; }
        if (!HasRecordSpace()) { return; }
        found->second.AddStageRecord(CopyStage(record.stage));
        ++m_recordCount;
    }

    void Consume(const RunResourceUsageRecord& record) {
        const auto found = m_sessions.find(record.runId);
        if (found == m_sessions.end() || !KeepResourceUsage(record.stage)) { ++m_retention.omittedRecords; return; }
        if (!HasRecordSpace()) { return; }
        found->second.AddStageRecord(CopyStage(record.stage));
        ++m_recordCount;
    }

    void Consume(const RunArtifactRecord& record) {
        const auto found = m_sessions.find(record.runId);
        if (m_detail == TelemetrySessionDetail::ProcessSummary || found == m_sessions.end()) {
            ++m_retention.omittedRecords;
            return;
        }
        if (!HasRecordSpace()) { return; }
        TelemetryTextCopy text;
        TelemetryArtifactRecord artifact;
        artifact.order = record.artifact.order;
        artifact.name = text.Copy(record.artifact.name);
        artifact.mediaType = text.Copy(record.artifact.mediaType);
        artifact.preferredExtension = text.Copy(record.artifact.preferredExtension);
        artifact.text = text.Copy(record.artifact.text);
        found->second.AddArtifact(std::move(artifact));
        ++m_recordCount;
        if (text.Truncated()) { ++m_retention.truncatedText; }
    }
    void Consume(const RunProgressRecord&) {}
    void Consume(const RunRemapOrderRecord&) {}

    RunRecordMask m_interests;
    RunCollectionMask m_collectionRequests;
    TelemetrySessionDetail m_detail;
    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, TelemetrySession> m_sessions;
    std::vector<std::uint64_t> m_completedRunIds;
    std::vector<TelemetryMessageRecord> m_unscopedMessages;
    std::size_t m_recordCount{0u};
    TelemetryRetentionStats m_retention;
};

} // 遥测命名空间

#endif
