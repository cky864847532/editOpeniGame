#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURETELEMETRY_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURETELEMETRY_H

#include <DataCodec/Log/Report/DataCodecProcessReportJson.h>
#include <DataCodec/Log/Telemetry/Sinks/TelemetrySessionSink.h>
#include <DataCodec/Runtime/Record/RunRecordEmitter.h>
#include <DataCodec/Test/Assertions/ProcessReportAssertions.h>
#include <DataCodec/Test/Common/DataCodecTestResult.h>
#include <DataCodec/Test/Feature/DataCodecFeatureDiagnosticExport.h>

#include <cereal/external/rapidjson/document.h>

#include <string>
#include <utility>
#include <vector>

namespace datacodec::test::feature_telemetry {

using datacodec::test::Require;
using datacodec::test::TestResult;

inline TestResult TestInterestDrivenCollection() {
    TestResult result;
    TelemetrySessionSink statusSink;
    TelemetrySessionSink reportSink(
        kRunLifecycleRecordMask |
        RunRecordKind::Message |
        RunRecordKind::StageTiming |
        RunRecordKind::ResourceUsage |
        RunRecordKind::Artifact,
        RunCollectionBit(RunCollectionKind::MemoryTrace));

    Require(result, statusSink.Wants(RunRecordKind::Message),
            "telemetry.interest.statusMessage", "status sink should request messages");
    Require(result, !statusSink.Wants(RunRecordKind::StageTiming),
            "telemetry.interest.statusTiming", "status sink should not request stage timing");
    Require(result, !statusSink.Wants(RunRecordKind::ResourceUsage),
            "telemetry.interest.statusResource", "status sink should not request resource usage");
    Require(result, reportSink.Wants(RunRecordKind::StageTiming),
            "telemetry.interest.reportTiming", "report sink should request stage timing");
    Require(result, reportSink.Wants(RunRecordKind::ResourceUsage),
            "telemetry.interest.reportResource", "report sink should request resource usage");
    Require(result, reportSink.Requests(RunCollectionKind::MemoryTrace),
            "telemetry.interest.reportMemory", "report sink should request memory tracing");

    TelemetrySessionSink boundedSink(
        kRunLifecycleRecordMask |
        RunRecordKind::Progress |
        RunRecordKind::RemapOrder);
    Require(result, !boundedSink.Wants(RunRecordKind::Progress),
            "telemetry.interest.progress", "session sink should reject progress records");
    Require(result, !boundedSink.Wants(RunRecordKind::RemapOrder),
            "telemetry.interest.remap", "session sink should reject remap records");

    return result;
}

inline TestResult TestSessionCaptureAndProcessReport() {
    TestResult result;
    TelemetrySessionSink sessionSink(
        kRunLifecycleRecordMask |
        RunRecordKind::Message |
        RunRecordKind::StageTiming |
        RunRecordKind::ResourceUsage |
        RunRecordKind::Artifact);
    RunRecordEmitter records;
    records.Reset(
        RunRecordInfo{
            .generatedAtUtc = "2026-01-01T00:00:00Z",
            .runKind = TelemetryRunKind::Encode,
            .objectName = "telemetry-test",
            .leafPath = "/0",
            .meshType = "SurfaceMesh",
        },
        &sessionSink);
    records.BeginRun();
    records.AddInfo("TelemetryTest", "message");
    records.RecordStageTiming("TopoStage", 3.5, TelemetryStageCategory::Topology);
    records.RecordResourceUsage("AttrStage", 4096u, TelemetryStageCategory::Attribute);
    records.AddArtifact(TelemetryArtifactRecord{
        .name = "diagnostic_note",
        .mediaType = "text/plain",
        .preferredExtension = ".txt",
        .text = "telemetry artifact",
    });
    records.EndRun(RunEndRecord{
        .success = true,
        .elapsedMs = 8.0,
        .inputBytes = 100u,
        .outputBytes = 40u,
        .sourceBytes = 100u,
    });

    const auto emittedMessages = records.TakeMessages();
    Require(result, emittedMessages.size() == 1u,
            "telemetry.messages.emitter", "run emitter message count mismatch");
    const auto capturedMessages = sessionSink.SnapshotMessages();
    Require(result, capturedMessages.size() == 1u,
            "telemetry.messages.snapshot", "session message snapshot count mismatch");
    const auto session = sessionSink.TakeSession(records.RunId());
    Require(result, session.has_value(),
            "telemetry.session.present", "completed run should produce a session");
    if (session.has_value()) {
        const std::vector<TelemetrySession> sessions{*session};
        Require(result, session->messages.size() == 1u,
                "telemetry.session.messages", "session message count mismatch");
        Require(result, session->stages.size() == 2u,
                "telemetry.session.stages", "session stage count mismatch");
        Require(result, session->artifacts.size() == 1u,
                "telemetry.session.artifacts", "session artifact count mismatch");
        Require(result, HasSerializableDataCodecProcessReport(sessions),
                "telemetry.processReport.sharedAssertion",
                "captured session should produce a valid process report");
        auto processNodes = BuildTelemetryProcessNodes(
            sessions,
            TelemetryRunKind::Encode);
        DataCodecProcessReport processReport{
            .operation = TelemetryRunKind::Encode,
            .generatedAtUtc = session->generatedAtUtc,
            .objectName = session->objectName,
            .success = session->success,
            .elapsedMs = session->elapsedMs,
            .inputBytes = session->inputBytes,
            .outputBytes = session->outputBytes,
            .processes = std::move(processNodes),
        };
        CompleteDataCodecProcessReportMemory(processReport);
        const auto json = SerializeDataCodecProcessReportJson(processReport);
        rapidjson::Document document;
        document.Parse(json.data(), json.size());
        Require(result, !document.HasParseError() && document.IsObject(),
                "telemetry.processReport.validJson", "process report should be valid json");
        Require(result, json.find("telemetry-test") != std::string::npos,
                "telemetry.processReport.object", "process report should contain the object name");
        Require(result, json.find("\"Topology\"") != std::string::npos,
                "telemetry.processReport.stage", "process report should contain grouped stage timing");
        Require(result, json.find("diagnostic_note") == std::string::npos &&
                json.find("telemetry artifact") == std::string::npos,
                "telemetry.processReport.artifact",
                "process report should not expose raw telemetry artifacts");
    }

    return result;
}

inline TestResult TestModuleTimingScopes() {
    TestResult result;
    TelemetrySessionSink sink(kRunLifecycleRecordMask | RunRecordKind::StageTiming,
        0u, TelemetrySessionDetail::ProcessSummary);
    RunRecordEmitter records;
    records.Reset(RunRecordInfo{.runKind = TelemetryRunKind::Encode, .objectName = "module-timing"}, &sink);
    records.BeginRun();
    const auto earlyReturn = [&] {
        ScopedRunStageTiming timing(records, "PackageWrite.Test");
        return false;
    };
    (void)earlyReturn();
    try {
        ScopedRunStageTiming timing(records, "EncodePrepare", TelemetryStageCategory::Params);
        throw 1;
    } catch (int) {}
    records.EndRun(RunEndRecord{.success = false, .elapsedMs = 42.0});
    const auto session = sink.TakeSession(records.RunId());
    Require(result, session && session->stages.size() == 2u &&
        std::all_of(session->stages.begin(), session->stages.end(), [](const auto& stage) {
            return stage.scope == "module-wall" && stage.elapsedMs >= 0.0;
        }), "telemetry.module-return-unwind", "early return and exception must retain one partial wall interval each");
    if (session) {
        auto nodes = BuildTelemetryProcessNodes({*session}, TelemetryRunKind::Encode);
        Require(result, nodes.size() == 1u && nodes[0].elapsedMs == 42.0 && nodes[0].children.size() == 2u &&
            std::all_of(nodes[0].children.begin(), nodes[0].children.end(), [](const auto& group) {
                return !group.elapsedMsValid && group.children.size() == 1u &&
                    group.children[0].elapsedMsValid && !group.children[0].success;
            }), "telemetry.module-report-boundary", "module timings must survive summary capture without inventing summed group or run time");
        const auto json = SerializeDataCodecProcessReportJson(DataCodecProcessReport{
            .operation = TelemetryRunKind::Encode, .processes = std::move(nodes)});
        Require(result, json.find("PackageWrite.Test") != std::string::npos &&
            json.find("inclusive-wall-including-waits") != std::string::npos,
            "telemetry.module-json", "existing JSON report must retain module identity and timing semantics");
    }

    TelemetrySessionSink statusOnly;
    records.Reset(RunRecordInfo{}, &statusOnly);
    {
        RejectAllocationsScope reject;
        ScopedRunStageTiming timing(records, "PackageWrite.Disabled");
    }
    Require(result, records.ExportFailureCount() == 0u && rejectedAllocationCount == 0u,
        "telemetry.module-disabled", "disabled module timing must not allocate or emit records");

    records.Reset(RunRecordInfo{}, &sink);
    {
        RejectAllocationsScope reject;
        ScopedRunStageTiming timing(records, "PackageWrite.OptionalDiagnosticAllocationFailure");
    }
    Require(result, records.ExportFailureCount() != 0u,
        "telemetry.module-export-failure", "optional timing formatting failure must remain inside diagnostic export handling");
    return result;
}

inline TestResult TestTelemetryRetention() {
    TestResult result;
    TelemetrySessionSink sessions(kTelemetrySessionRecordMask);
    for (std::uint64_t id = 1u; id <= TelemetrySessionSink::kSessionLimit + 1u; ++id) {
        sessions.Submit(RunBeginRecord{RunRecordInfo{.runId = id}});
    }
    Require(result, sessions.RetainedSessionCount() == 256u && sessions.RetentionStats().omittedSessions == 1u,
        "telemetry.active-session-cap", "full active sessions must omit the next capture without growing the table");
    sessions.Submit(RunEndRecord{.run = {.runId = 257u}, .success = true});
    Require(result, !sessions.TakeSession(257u).has_value(), "telemetry.omitted-session-end",
        "completion of an omitted session must not recreate its retained state");
    sessions.Submit(RunEndRecord{.run = {.runId = 1u}, .success = true});
    sessions.Submit(RunBeginRecord{RunRecordInfo{.runId = 258u}});
    Require(result, !sessions.TakeSession(1u).has_value() && sessions.RetainedSessionCount() == 256u &&
        sessions.RetentionStats().omittedSessions == 2u,
        "telemetry.completed-session-eviction", "new capture must evict the oldest completed session");

    TelemetrySessionSink sink(kTelemetrySessionRecordMask);
    RunRecordEmitter records;
    records.Reset(RunRecordInfo{.runKind = TelemetryRunKind::Encode, .objectName = "bounded"}, &sink);
    records.BeginRun();
    for (std::size_t i = 0u; i < TelemetrySessionSink::kRecordLimit; ++i) {
        records.AddMessage({.text = i == 0u ? std::string(2047u, 'x') + "中文" : "ok"});
    }
    records.RecordStageTiming("overflow", 1.0);
    records.AddMessage({.text = "omitted"});
    records.EndRun({.success = true});
    const auto before = sink.RetentionStats();
    const auto firstSnapshot = sink.SnapshotCompletedSessions();
    const auto secondSnapshot = sink.SnapshotCompletedSessions();
    const auto retained = sink.TakeSession(records.RunId());
    Require(result, retained && retained->success && retained->messages.size() == 4096u &&
        retained->messages.front().text.size() == 2047u && retained->messages.front().textTruncated &&
        before.omittedRecords == 2u && before.truncatedText == 1u,
        "telemetry.record-and-utf8-cap", "detail cap and UTF-8 text truncation must preserve run completion");
    Require(result, sink.RetainedRecordCount() == 0u && sink.RetainedSessionCount() == 0u &&
        firstSnapshot.size() == 1u && secondSnapshot.size() == 1u,
        "telemetry.take-reclaims-capacity", "snapshots must not accumulate inside the sink and take must free its retained slots");
    Require(result, records.MessageRetention().omittedRecords == 1u &&
        records.MessageRetention().truncatedText == 1u && records.TakeMessages().size() == 4096u,
        "telemetry.emitter-cap", "entry message collection must use the same fixed bounds");
    const auto nodes = BuildTelemetryProcessNodes(firstSnapshot, TelemetryRunKind::Encode);
    const auto json = SerializeDataCodecProcessReportJson(DataCodecProcessReport{
        .operation = TelemetryRunKind::Encode, .processes = nodes});
    Require(result, json.find("capture.omittedRecords") != std::string::npos &&
        json.find("capture.incomplete") != std::string::npos,
        "telemetry.retention-report", "report must expose omitted and truncated capture coverage");

    TelemetrySessionSink unscoped;
    unscoped.Submit(RunMessageRecord{.message = {.text = "outside a run"}});
    Require(result, unscoped.TakeUnscopedMessages().size() == 1u && unscoped.RetainedRecordCount() == 0u,
        "telemetry.unscoped-take", "unscoped messages must share and release the detail capacity");
    return result;
}

inline TestResult TestStageCategoryResolution() {
    TestResult result;
    Require(result, ResolveTelemetryStageCategory("ParamsEncodeStage") == TelemetryStageCategory::Params,
            "telemetry.category.params", "params stage category mismatch");
    Require(result, ResolveTelemetryStageCategory("GeometryStage") == TelemetryStageCategory::Geometry,
            "telemetry.category.geometry", "geometry stage category mismatch");
    Require(result, ResolveTelemetryStageCategory("TopoDecodeStage") == TelemetryStageCategory::Topology,
            "telemetry.category.topology", "topology stage category mismatch");
    Require(result, ResolveTelemetryStageCategory("PointAttributeStage") == TelemetryStageCategory::Attribute,
            "telemetry.category.attribute", "attribute stage category mismatch");
    Require(result, ResolveTelemetryStageCategory("CellSpatialPartition.Morton") == TelemetryStageCategory::Remap,
            "telemetry.category.remap", "remap stage category mismatch");
    Require(result, ResolveTelemetryStageCategory("DecodeCommitStage") == TelemetryStageCategory::Commit,
            "telemetry.category.commit", "commit stage category mismatch");

    return result;
}

} // namespace datacodec::test::feature_telemetry

namespace datacodec::test {

inline TestResult RunDataCodecFeatureTelemetry() {
    auto result = feature_telemetry::TestInterestDrivenCollection();
    const auto appendResult = [&result](const TestResult& addition) {
        if (!addition.passed) {
            result.passed = false;
            result.failures.insert(
                result.failures.end(),
                addition.failures.begin(),
                addition.failures.end());
        }
        result.AppendDiagnostics(addition.diagnostics);
    };
    appendResult(feature_telemetry::TestSessionCaptureAndProcessReport());
    appendResult(feature_telemetry::TestModuleTimingScopes());
    appendResult(feature_telemetry::TestStageCategoryResolution());
    appendResult(feature_telemetry::TestTelemetryRetention());
    appendResult(RunDataCodecFeatureDiagnosticExport());
    return result;
}

} // namespace datacodec::test

#endif
