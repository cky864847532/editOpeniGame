#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREDIAGNOSTICEXPORT_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREDIAGNOSTICEXPORT_H

#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Log/Telemetry/Sinks/TelemetrySessionSink.h"
#include "DataCodec/Log/Report/DataCodecProcessReportJson.h"
#include "DataCodec/Runtime/Output/DataCodecOutputRouter.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

namespace datacodec::test {

inline TestResult RunDataCodecFeatureDiagnosticExport() {
    TestResult result;
    class ThrowingSink final : public IRunRecordSink {
    public:
        RunRecordMask Interests() const noexcept override {
            return kRunLifecycleRecordMask | RunRecordKind::Message | RunRecordKind::StageTiming;
        }
        void Submit(const RunRecord&) override { throw std::bad_alloc(); }
    } failing;
    class CountingSink final : public IRunRecordSink {
    public:
        RunRecordMask Interests() const noexcept override { return kRunLifecycleRecordMask; }
        void Submit(const RunRecord&) override { ++count; }
        std::size_t count{0u};
    } surviving;
    RunRecordDispatcher dispatcher;
    dispatcher.AddSink(&failing);
    dispatcher.AddSink(&surviving);
    const RunRecord begin = RunBeginRecord{RunRecordInfo{.runId = 1u}};
    bool complete = true;
    {
        RejectAllocationsScope reject;
        complete = dispatcher.TrySubmit(begin);
    }
    Require(result, !complete && rejectedAllocationCount == 0u && surviving.count == 1u &&
        dispatcher.DiagnosticsIncomplete() && failing.ExportFailureCount() == 1u,
        "export.independent-sinks", "a throwing diagnostic consumer must preserve delivery to later consumers without allocation");

    DataCodecExecutionResources run(ResolvedResourceConfiguration{{8u, 1u, 1u}, 8u, 1u, false, true});
    run.BeginRun();
    RunRecordEmitter records(&run);
    records.Reset(RunRecordInfo{.objectName = std::string(2048u, 'x')}, &dispatcher);
    const auto epoch = run.EventEpoch();
    {
        RejectAllocationsScope reject;
        records.BeginRun();
    }
    ResourceDebugSnapshot snapshot;
    Require(result, records.ExportFailureCount() != 0u && run.TryCopyResourceDebugSnapshot(snapshot) &&
        snapshot.diagnosticsIncomplete && snapshot.diagnosticExportFailures != 0u && !snapshot.failure &&
        !run.Stopped() && run.EventEpoch() == epoch && snapshot.limits.ownedStorageLimitBytes == 8u,
        "export.format-allocation", "failed optional record construction must only update fixed diagnostic counters");
    records.BeginRun();
    records.EndRun(RunEndRecord{.success = true});
    Require(result, surviving.count == 3u && !run.Stopped() && !run.FirstFailure(),
        "export.business-success", "later lifecycle records must remain deliverable without changing the business result");
    run.EndRun();

    TelemetrySessionSink session;
    RunBeginRecord captured{RunRecordInfo{.runId = 12u, .objectName = std::string(128u, 'a')}};
    const RunRecord captureRecord = captured;
    {
        RejectAllocationsScope reject;
        session.Submit(captureRecord);
    }
    Require(result, session.DiagnosticsIncomplete(), "export.capture-missing", "failed optional capture must report missing diagnostics");
    session.TrySubmit(RunRecord{std::move(captured)});
    session.TrySubmit(RunRecord{RunEndRecord{.run = RunRecordInfo{.runId = 12u}, .success = true}});
    bool empty = false;
    {
        RejectAllocationsScope reject;
        empty = session.SnapshotCompletedSessions().empty();
    }
    const auto recovered = session.SnapshotCompletedSessions();
    Require(result, empty && session.RetentionStats().exportFailures >= 2u && recovered.size() == 1u &&
        recovered.front().success && recovered.front().captureRetention.exportFailures >= 2u,
        "export.snapshot-missing", "failed snapshot copies must preserve retained sessions and export the incomplete coverage counter");
    class FailingReportSink final : public IDataCodecReportFileSink {
    public:
        DataCodecReportWriteResult WriteReportFile(const DataCodecReportFile&) override {
            ++calls;
            if (throwOnWrite) { throw std::bad_alloc(); }
            return {.success = succeed};
        }
        std::size_t calls{0u};
        bool throwOnWrite{true};
        bool succeed{false};
    };
    auto reportSink = std::make_shared<FailingReportSink>();
    const DataCodecProcessReport report{.objectName = std::string(512u, 'r')};
    DataCodecReportWriteResult preparation;
    {
        RejectAllocationsScope reject;
        preparation = reportSink->TryWriteReportFile([&] {
            return DataCodecReportFile{.name = report.objectName,
                .content = SerializeDataCodecProcessReportJson(report)};
        });
    }
    Require(result, !preparation.success && preparation.failure == DataCodecReportExportFailure::Preparation &&
        reportSink->calls == 0u && reportSink->ExportFailureCount() == 1u,
        "export.report-preparation", "report allocation failure must be recorded before the writer is called");
    DataCodecReportWriteResult writing;
    {
        RejectAllocationsScope reject;
        writing = reportSink->TryWriteReportFile([] { return DataCodecReportFile{}; });
    }
    Require(result, !writing.success && writing.failure == DataCodecReportExportFailure::Write &&
        reportSink->calls == 1u && reportSink->ExportFailureCount() == 2u && rejectedAllocationCount == 0u,
        "export.report-write", "a throwing report writer must be called once and return a fixed failure without secondary allocation");
    reportSink->throwOnWrite = false;
    const auto refused = reportSink->TryWriteReportFile([] { return DataCodecReportFile{}; });
    Require(result, !refused.success && reportSink->calls == 2u && reportSink->ExportFailureCount() == 3u,
        "export.report-refused", "a report writer returning false must also mark incomplete diagnostics");
    reportSink->succeed = true;
    const auto written = reportSink->TryWriteReportFile([] { return DataCodecReportFile{}; });
    Require(result, written.success && reportSink->calls == 3u && reportSink->ExportFailureCount() == 3u,
        "export.report-next-request", "later explicit exports must proceed while retaining the earlier diagnostic failure count");

    class FailingUi final : public IDataCodecUiSink {
    public:
        void SubmitUiStatus(const DataCodecStatusRecord&) override { throw std::bad_alloc(); }
    };
    class CountConsole final : public IDataCodecConsoleSink {
    public:
        void SubmitConsoleStatus(const DataCodecStatusRecord&) override { ++count; }
        std::size_t count{0u};
    };
    auto console = std::make_shared<CountConsole>();
    DataCodecOutputRouter router({.ui = std::make_shared<FailingUi>(), .console = console, .reportFile = reportSink});
    router.Submit(RunMessageRecord{.message = TelemetryMessageRecord{
        .severity = TelemetryMessageSeverity::Warning, .text = "warning"}});
    Require(result, console->count == 1u && router.DiagnosticsIncomplete(),
        "export.router-consumers", "UI failure must preserve delivery to the independent console consumer");
    reportSink->succeed = false;
    const RunRecord artifact = RunArtifactRecord{.artifact = TelemetryArtifactRecord{
        .name = "report", .text = "{}"}};
    const auto beforeRouterFailure = router.ExportFailureCount();
    router.Submit(artifact);
    Require(result, router.ExportFailureCount() > beforeRouterFailure && !router.LastReportError().empty(),
        "export.router-file-false", "an optional file failure must reach the router diagnostic state without throwing");
    return result;
}

}

#endif
