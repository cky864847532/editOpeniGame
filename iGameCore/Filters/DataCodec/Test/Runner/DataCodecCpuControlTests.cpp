#include "DataCodec/Runtime/Execution/DataCodecCpuController.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Test/Feature/DataCodecFeatureAdapterRoundTrip.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace datacodec::test {

TestResult RunDataCodecCpuControlTests() {
    using namespace std::chrono_literals;
    TestResult result;
    const auto check = [&](bool ok, const char* name) { Require(result, ok, name, name); };
    const auto origin = cpu_control::Clock::time_point{} + 10s;
    const auto sample = [&](int ms, double idle) { return CpuUsageSample{idle, 8u, origin + std::chrono::milliseconds(ms)}; };
    const CpuControlFlow work{8u, true, true, false};
    DataCodecCpuController controller;
    controller.Reset(CodecThreadMode::Fixed, 0.0, 3u, origin);
    controller.Advance(origin + 2s, {}, work);
    check(controller.Snapshot().quota == 3.0 && controller.Snapshot().initialized, "cpu.fixed.no-sampling");
    controller.Reset(CodecThreadMode::Adaptive, 0.2, 8u, origin);
    controller.Advance(origin + 50ms, sample(50, 0.7), work);
    check(std::abs(controller.Snapshot().quota - 4.0) < 1e-9, "cpu.startup-idle-budget");
    for (int ms = 100; ms <= 300; ms += 50) { controller.Advance(origin + std::chrono::milliseconds(ms), sample(ms, 0.2), work); }
    check(std::abs(controller.Snapshot().quota - 3.0) < 1e-9, "cpu.calibration-downward-step");
    for (int ms = 350; ms <= 500; ms += 50) { controller.Advance(origin + std::chrono::milliseconds(ms), sample(ms, 0.325), work); }
    const auto calibrated = controller.Snapshot();
    check(calibrated.calibration == CpuCalibrationResult::Estimated && calibrated.response &&
        std::abs(*calibrated.response - 1.0) < 1e-9 && calibrated.calibrationElapsed == 400ms &&
        std::abs(calibrated.shrinkGain - 0.5) < 1e-9 && std::abs(calibrated.growthGain - 0.125) < 1e-9, "cpu.two-window-estimate");
    check(!controller.Advance(origin + 600ms, {}, work).sampleFailure &&
        controller.Advance(origin + 1600ms, {}, work).sampleFailure, "cpu.invalid-sample-deadline");

    for (const double after : {0.2, 0.1}) {
        controller.Reset(CodecThreadMode::Adaptive, 0.2, 8u, origin);
        controller.Advance(origin + 50ms, sample(50, 0.7), work);
        for (int ms = 100; ms <= 500; ms += 50) {
            controller.Advance(origin + std::chrono::milliseconds(ms), sample(ms, ms <= 300 ? 0.2 : after), work);
        }
        check(controller.Snapshot().calibration == CpuCalibrationResult::Unusable &&
            !controller.Snapshot().response && controller.Snapshot().shrinkGain == 0.5,
            "cpu.invalid-response-keeps-nominal");
    }
    controller.Reset(CodecThreadMode::Adaptive, 0.2, 8u, origin);
    controller.Advance(origin + 50ms, sample(50, 0.7), work);
    controller.Advance(origin + 100ms, sample(100, 0.2), work);
    controller.Advance(origin + 1100ms, sample(1100, 0.2), work);
    check(controller.Snapshot().calibration == CpuCalibrationResult::TimedOut &&
        controller.Snapshot().calibrationElapsed == 1s, "cpu.calibration-one-second-deadline");

    controller.Reset(CodecThreadMode::Adaptive, 0.2, 8u, origin);
    controller.Advance(origin + 50ms, sample(50, 0.1), work);
    check(controller.Snapshot().quota == 0.0, "cpu.zero-pauses-new-compute");
    const CpuControlFlow pending{8u, false, true, false};
    controller.Advance(origin + 250ms, sample(250, 0.7), pending);
    check(controller.Snapshot().quota > 0.0, "cpu.pending-admission-recovers-from-zero");
    const auto previous = controller.Snapshot().quota;
    controller.Advance(origin + 450ms, sample(450, 0.9), {8u, false, true, true});
    check(controller.Snapshot().quota == previous, "cpu.memory-gate-blocks-growth");
    controller.Advance(origin + 650ms, sample(650, 0.0), pending);
    check(controller.Snapshot().quota < previous, "cpu.external-load-shrinks-quota");
    controller.Advance(origin + 850ms, sample(850, 1.0), {1u, false, true, false});
    check(controller.Snapshot().quota <= 1.0, "cpu.memory-compute-ceiling");
    const auto duplicate = controller.Advance(origin + 1900ms, sample(850, 1.0), work);
    check(duplicate.sampleFailure, "cpu.stale-sample-fails");

    CpuPermitBudget budget;
    budget.Reset(0.5, origin);
    check(budget.Available(), "cpu.fractional-initial-permit");
    budget.Accrue(origin + 100ms, 1u);
    check(!budget.Available() && budget.Credit() < 0.0, "cpu.long-task-creates-occupancy-debt");
    budget.Accrue(origin + 200ms, 0u);
    check(budget.Available(), "cpu.debt-repayment-restores-permit");
    budget.SetQuota(0.0, origin + 200ms, 0u);
    budget.Accrue(origin + 10s, 0u);
    check(!budget.Available(), "cpu.zero-quota-does-not-open-permit");
    budget.SetQuota(0.5, origin + 10s, 0u);
    budget.Accrue(origin + 20s, 0u);
    check(budget.Credit() <= 0.011, "cpu.idle-credit-remains-bounded");

    auto environment = ProbeResources();
    const auto rejects = [&](CodecResourceParams params) {
        try { ResolveResourceConfiguration(params, environment); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    CodecResourceParams params;
    params.threadMode = CodecThreadMode::Adaptive;
    check(rejects(params), "cpu.adaptive-requires-idle-target");
    params.targetCpuIdleRatio = std::numeric_limits<double>::quiet_NaN();
    check(rejects(params), "cpu.reject-nan");
    params.targetCpuIdleRatio = 1.0;
    check(rejects(params), "cpu.reject-full-idle");
    params.targetCpuIdleRatio = -0.1;
    check(rejects(params), "cpu.reject-negative-idle");
    params.targetCpuIdleRatio = 0.2;
    params.maxComputeThreads = 1u;
    check(rejects(params), "cpu.reject-conflicting-fixed-limit");
    params.threadMode = CodecThreadMode::Fixed;
    check(rejects(params), "cpu.fixed-reject-idle-target");

#if defined(_WIN32)
    CpuUsageProbe probe;
    check(probe.Reset(), "cpu.windows-probe-initialization");
    std::this_thread::sleep_for(60ms);
    const auto observed = probe.Sample();
    check(observed.idleRatio && *observed.idleRatio >= 0.0 && *observed.idleRatio <= 1.0 &&
        observed.logicalProcessors != 0u, "cpu.windows-system-idle-sample");
    const auto data = MakeAdapterRoundTripDataset();
    for (const auto memory : {CodecResourceMode::Fixed, CodecResourceMode::Adaptive, CodecResourceMode::Unlimited}) {
        for (const auto threads : {CodecThreadMode::Fixed, CodecThreadMode::Adaptive}) {
            CodecResourceParams resources;
            resources.mode = memory;
            resources.threadMode = threads;
            if (threads == CodecThreadMode::Fixed) { resources.maxComputeThreads = 1u; }
            else { resources.targetCpuIdleRatio = 0.0; }
            if (memory == CodecResourceMode::Fixed) { resources.ownedStorageLimitBytes = 128u * 1024u * 1024u; }
            TestEncodeAdapter encoder(data);
            EncodeRequest request;
            request.input = EncodeInput::LeafAdapter(&encoder, {}, data.name, "PointSet");
            request.output = EncodeOutput::Memory(EncodePackageKind::LeafPackage);
            request.attributeSelection = AttributeSelectionMode::AllAvailable;
            request.resources = resources;
            auto encoded = Encode(request);
            AppendCodecMessages(result, encoded.messages);
            if (!Require(result, encoded.success, "cpu.mode-matrix.encode", "thread/memory mode encode failed")) { continue; }
            TestDecodeAdapter decoder;
            DecodePackageRequest decode;
            decode.inputReader = std::make_shared<MemoryByteRangeReader>(
                std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes)));
            decode.leafAdapter = &decoder;
            decode.attributeSelection = AttributeSelectionMode::AllAvailable;
            decode.resources = resources;
            const auto decoded = DecodePackage(decode);
            AppendCodecMessages(result, decoded.messages);
            check(decoded.success && decoder.Committed() && decoder.Points() == data.points &&
                decoder.Attributes().size() == data.pointFields.size(), "cpu.mode-matrix.roundtrip");
            for (std::size_t i = 0; i < std::min(decoder.Attributes().size(), data.pointFields.size()); ++i) {
                const auto& actual = decoder.Attributes()[i];
                const auto& expected = data.pointFields[i].values;
                check(actual.complete && actual.bytes.size() == expected.size() * sizeof(float) &&
                    std::memcmp(actual.bytes.data(), expected.data(), actual.bytes.size()) == 0,
                    "cpu.mode-matrix.attribute-values");
            }
        }
    }
    {
        CodecResourceParams options;
        options.mode = CodecResourceMode::Fixed;
        options.ownedStorageLimitBytes = 1024u * 1024u;
        options.threadMode = CodecThreadMode::Adaptive;
        options.targetCpuIdleRatio = 0.0;
        auto config = ResolveResourceConfiguration(options, environment);
        config.computeCeiling = std::min<std::size_t>(4u, config.computeCeiling);
        config.initialLimits.computeLimit = config.computeCeiling;
        config.initialLimits.slotLimit = config.computeCeiling + 1u;
        DataCodecExecutionResources root(config);
        CodecRunScope scope(root);
        check(scope && root.BeginFlow(), "cpu.queue.begin");
        std::vector<SlotLease> slots;
        std::vector<std::shared_ptr<TerminalWork>> jobs;
        std::atomic_size_t active{0u};
        std::atomic_bool exclusiveValid{true};
        for (unsigned i = 0; i < 4; ++i) {
            std::optional<SlotLease> slot;
            while (!root.Stopped() && !slot) {
                const auto epoch = root.EventEpoch();
                slot = root.TryAcquireSlot();
                if (!slot) { root.WaitForChange(epoch); }
            }
            if (!slot) { break; }
            const bool exclusive = i == 1u;
            auto job = std::make_shared<TerminalWork>([&, exclusive](WorkerContext& worker) {
                const auto previousActive = active.fetch_add(1u);
                if (exclusive && previousActive != 0u) { exclusiveValid = false; }
                if (worker.ComputeUnits() > config.computeCeiling) { exclusiveValid = false; }
                std::this_thread::sleep_for(20ms);
                active.fetch_sub(1u);
                return true;
            }, exclusive ? TerminalWorkKind::ExclusivePackage : TerminalWorkKind::Ordinary);
            check(root.SubmitTerminal(*slot, job), "cpu.queue.submit");
            slots.push_back(std::move(*slot));
            jobs.push_back(std::move(job));
            // 最小设备配置及时提交以释放有限槽位
            if (slots.size() == config.initialLimits.slotLimit || i == 3u) {
                for (std::size_t j = 0; j < jobs.size(); ++j) {
                    while (!root.Stopped() && root.Completion(*jobs[j]) != BlockCompletion::Succeeded) {
                        const auto epoch = root.EventEpoch();
                        if (root.Completion(*jobs[j]) == BlockCompletion::Succeeded) { break; }
                        root.WaitForChange(epoch);
                    }
                    check(root.CommitSlot(slots[j]), "cpu.queue.ordered-commit");
                    slots[j].Reset();
                }
                jobs.clear(); slots.clear();
            }
        }
        root.SetFlowProgress(false);
        check(exclusiveValid && active == 0u && scope.Finish(!root.Stopped()), "cpu.exclusive-and-ordinary-drain");
    }
    // 高空闲目标下仍能取消等待，请求复用时重新初始化控制状态
    CodecResourceParams adaptive;
    adaptive.mode = CodecResourceMode::Unlimited;
    adaptive.threadMode = CodecThreadMode::Adaptive;
    adaptive.targetCpuIdleRatio = 0.999;
    DataCodecExecutionResources run(adaptive);
    for (int request = 0; request < 2; ++request) {
        check(run.BeginRun() && run.BeginFlow(), "cpu.reuse.begin");
        std::jthread cancel([&] { std::this_thread::sleep_for(150ms); run.RequestStop(); });
        while (!run.Stopped()) {
            const auto epoch = run.EventEpoch();
            auto slot = run.TryAcquireSlot();
            if (slot) { slot->Reset(); }
            run.WaitForChange(epoch);
        }
        run.CancelAndWaitRun();
        check(run.EndRun(), "cpu.cancel-drains-paused-request");
    }
#endif
    return result;
}

}
