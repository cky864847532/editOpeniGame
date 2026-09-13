#pragma once

#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include <iostream>
#include <cmath>
#include <thread>
#include <vector>
#include <future>
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"

namespace datacodec::test {

// 仅供显式调查入口读取现有快照，不修改资源目标，不制造系统压力
class MemoryInvestigationTrace {
    struct Row {
        double seconds{};
        MemoryControlSnapshot memory;
        std::uint64_t reserved{}, arrays{}, transferred{}, scratch{}, next{}, flow{}, retired{}, trim{};
        std::size_t admitted{}, computing{}, queued{};
        bool gate{}, waiting{};
        ResourceControlPhase phase{};
        ResourceWorkPath path{};
        ResourceWaitReason waitReason{};
        bool heavy{};
        std::optional<std::uint64_t> completed, committed;
        std::optional<BlockCompletion> nextCompletion;
        std::uint64_t epoch{};
    };
    DataCodecExecutionResources& m_root;
    const char* m_stage;
    ResourceClock::time_point m_started{ResourceClock::now()};
    std::vector<Row> m_rows;
    std::size_t m_missed{}, m_dropped{};
    std::jthread m_observer;

    void Capture() {
        ResourceDebugSnapshot s;
        if (!m_root.TryCopyResourceDebugSnapshot(s)) { ++m_missed; return; }
        if (m_rows.size() == m_rows.capacity()) { ++m_dropped; return; }
        m_rows.push_back({std::chrono::duration<double>(s.capturedAt - m_started).count(), s.memory,
            s.storage.reservedBytes, s.allocatedStorage.liveBytes, s.allocatedStorage.transferredBytes,
            m_root.Scratch().SnapshotStats().retainedBytes, s.nextWorkBytes, s.flowId,
            s.lastRetired.value_or(0u), s.trimEpoch, s.admittedBlocks, s.activeComputeUnits,
            s.queuedTasks, s.gateOpen, s.byteWaiting, s.controlPhase, s.workType.path,
            s.waiting, s.heavyPhaseAdmitted, s.maxCompleted, s.lastCommitted, s.nextQueuedCompletion, s.eventEpoch});
    }
public:
    MemoryInvestigationTrace(DataCodecExecutionResources& root, const char* stage) : m_root(root), m_stage(stage) {
        m_rows.reserve(6000u);
        m_observer = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                Capture();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    ~MemoryInvestigationTrace() {
        m_observer.request_stop();
        if (m_observer.joinable()) { m_observer.join(); }
    }
    void Finish() {
        m_observer.request_stop();
        if (m_observer.joinable()) { m_observer.join(); }
        Capture();
        const auto optional = [](const auto& value) { return value ? std::to_string(*value) : "null"; };
        for (const auto& r : m_rows) {
            const auto& m = r.memory;
            std::cout << "MEMTRACE stage=" << m_stage << " t=" << r.seconds
                << " available=" << optional(m.availableBytes) << " reserve=" << m.reserveBytes
                << " recovery=" << m.recoveryBytes << " headroom=" << m.growthHeadroomBytes
                << " reserved=" << r.reserved << " arrays=" << r.arrays << " transferred=" << r.transferred
                << " scratch=" << r.scratch << " next=" << r.next << " flow=" << r.flow << " retired=" << r.retired
                << " trim=" << r.trim << " admitted=" << r.admitted << " computing=" << r.computing
                << " queued=" << r.queued << " gate=" << r.gate << " waiting=" << r.waiting
                << " phase=" << static_cast<unsigned>(r.phase) << " path=" << static_cast<unsigned>(r.path)
                << " wait_reason=" << static_cast<unsigned>(r.waitReason) << " heavy=" << r.heavy
                << " completed=" << optional(r.completed) << " committed=" << optional(r.committed)
                << " next_completion=" << (r.nextCompletion ? static_cast<int>(*r.nextCompletion) : -1)
                << " epoch=" << r.epoch
                << " preview_ratio=" << m.previewRatio << " grant=" << m.grantEpoch
                << " reason=" << MemoryDecisionReasonName(m.reason)
                << " wait_age=" << (m.waitSince ? std::chrono::duration<double>(m_started +
                    std::chrono::duration_cast<ResourceClock::duration>(std::chrono::duration<double>(r.seconds)) - *m.waitSince).count() : 0.0)
                << '\n';
        }
        std::cout << "MEMTRACE_END stage=" << m_stage << " samples=" << m_rows.size()
            << " missed=" << m_missed << " dropped=" << m_dropped << std::endl;
    }
};

// 用真实内部线程池核验准入、排空、退休和唤醒，控制信号由测试确定发布
inline int InvestigateScheduler(IRunRecordSink& sink) {
    using namespace std::chrono_literals;
    constexpr std::uint64_t MiB = 1024u * 1024u;
    int failures = 0;
    const auto record = [&](const char* name, bool ok) {
        failures += !ok;
        RunMessageRecord message;
        message.message.code = "scheduler-investigation";
        message.message.text = std::string("case=") + name + ";passed=" + (ok ? "1" : "0");
        sink.TrySubmit(message);
    };
    {
        RuntimeResourceLimits limits{4u * MiB, 1u, 2u};
        DataCodecExecutionResources root(ResolvedResourceConfiguration{limits, 4u * MiB, 1u, true, true});
        CodecRunScope scope(root);
        if (!scope || !root.BeginFlow()) { return 6; }
        resource::ResidentByteBudget::Lease aBytes, bBytes;
        auto a = root.TryAcquireSlot(MiB, aBytes);
        auto b = root.TryAcquireSlot(MiB, bBytes);
        if (!a || !b) { return 6; }
        std::promise<void> release;
        const auto ready = release.get_future().share();
        auto first = std::make_shared<TerminalWork>([ready](WorkerContext&) { ready.wait(); return true; });
        auto second = std::make_shared<TerminalWork>([](WorkerContext&) { return true; });
        const bool submitted = root.SubmitTerminal(*a, first) && root.SubmitTerminal(*b, second);
        const bool closed = root.UpdateLimits(limits, false, ResourceDecisionReason::MechanismCheck);
        release.set_value();
        const auto deadline = ResourceClock::now() + 2s;
        while (root.Completion(*second) != BlockCompletion::Succeeded && !root.Stopped() && ResourceClock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        record("closed_gate_runs_already_queued_work", submitted && closed &&
            root.Completion(*first) == BlockCompletion::Succeeded && root.Completion(*second) == BlockCompletion::Succeeded);
        if (root.Completion(*second) != BlockCompletion::Succeeded) { root.RequestStop(); return 6; }
        const bool committed = root.CommitSlot(*a) && root.CommitSlot(*b);
        a.reset(); b.reset(); aBytes.Reset(); bBytes.Reset();
        ResourceDebugSnapshot snapshot;
        const bool read = root.TryCopyResourceDebugSnapshot(snapshot);
        record("closed_gate_commits_and_retires", committed && read && !snapshot.gateOpen &&
            snapshot.admittedBlocks == 0u && snapshot.queuedTasks == 0u && snapshot.activeComputeUnits == 0u &&
            snapshot.storage.reservedBytes == 0u);
        const auto heavyBlocked = !root.TryAcquireHeavyPhase();
        root.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck);
        auto heavy = root.TryAcquireHeavyPhase();
        record("zero_byte_heavy_phase_needs_open_gate", heavyBlocked && heavy.has_value());
        heavy.reset();
        auto lease = root.TryAcquireStorage(4u * MiB, MemoryDemandKind::RequiredContinuation);
        auto blocked = root.TryAcquireStorage(1u, MemoryDemandKind::RequiredContinuation);
        const auto epoch = root.EventEpoch();
        if (lease) { lease->Reset(); }
        const bool releaseNotified = root.EventEpoch() != epoch;
        auto granted = root.TryAcquireStorage(1u, MemoryDemandKind::RequiredContinuation);
        record("byte_release_publishes_wake_and_allows_request", lease.has_value() && !blocked && releaseNotified && granted.has_value());
        granted.reset();
        root.ClearByteWait();
        record("request_drains", scope.Finish(true));
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{MiB, 1u, 2u}, MiB, 1u, true, true});
        CodecRunScope scope(root);
        if (!scope) { return 6; }
        const auto epoch = root.EventEpoch();
        std::jthread cancel([&] { std::this_thread::sleep_for(20ms); root.RequestStop(); });
        root.WaitForChange(epoch);
        record("cancel_wakes_waiting_driver", root.Stopped());
    }
    return failures ? 6 : 0;
}

// 合成观测只驱动生产控制器，虚拟时间无需等待，也不申请压力内存
inline int InvestigateMemoryController() {
    using namespace std::chrono_literals;
    constexpr std::uint64_t MiB = 1024u * 1024u, GiB = 1024u * MiB;
    struct Scenario {
        ResourceControllerState state;
        FlowSnapshot flow;
        ResourceSample sample;
        ResourceClock::time_point now{ResourceClock::time_point{} + 10s};
        Scenario() {
            ResolvedResourceConfiguration config{{0u, 16u, 17u}, 32u * GiB, 16u, true, true};
            config.targetAvailableMemoryRatio = 0.25;
            InitializeResourceController(state, config, now);
            flow.limits = config.initialLimits; flow.runActive = true; flow.requestId = 1u;
            sample.physicalTotalBytes = 32u * GiB; sample.availableBytes = 12u * GiB;
            sample.pressure = PressureLevel::Normal;
            Tick(0ms);
        }
        ControlDecision Tick(std::chrono::milliseconds step = 250ms) {
            now += step; sample.sampledAt = now;
            const auto d = Advance(state, now, sample, flow);
            flow.limits = d.limits; flow.gateOpen = d.gateOpen;
            return d;
        }
    };
    int failed = 0;
    const auto check = [&](const char* name, bool observed) {
        std::cout << "CONTROL_CASE name=" << name << " reproduced=" << observed << '\n';
        failed += !observed;
    };
    {
        Scenario s;
        s.sample.availableBytes = s.state.memory.reserveBytes - 1u;
        const auto low = s.Tick();
        s.sample.availableBytes = s.state.memory.recoveryBytes - 1u;
        ControlDecision end;
        for (int i = 0; i < 120; ++i) { end = s.Tick(); }
        check("single_low_sample_then_deadband_allows_one_slot", !low.gateOpen &&
            end.gateOpen && !end.failForSustainedPressure && s.state.phase == ResourceControlPhase::Recover);
    }
    {
        Scenario s;
        s.sample.availableBytes = s.state.memory.reserveBytes - 1u; s.Tick();
        s.sample.availableBytes = s.state.memory.recoveryBytes + MiB;
        ControlDecision d;
        for (int i = 0; i < 8; ++i) { d = s.Tick(); }
        const bool singleBeforeTwoSeconds = d.gateOpen && d.limits.slotLimit == 1u;
        d = s.Tick();
        check("recovery_requires_two_seconds_for_parallel_slots", singleBeforeTwoSeconds && d.gateOpen && d.limits.slotLimit == 17u);
    }
    {
        Scenario s;
        s.sample.availableBytes = s.state.memory.reserveBytes - 1u; s.Tick();
        ControlDecision d;
        for (int i = 0; i < 120; ++i) {
            s.sample.availableBytes = i % 8 == 7 ? s.state.memory.recoveryBytes - 1u : s.state.memory.recoveryBytes + MiB;
            d = s.Tick();
        }
        check("brief_recovery_dips_keep_single_slot", d.gateOpen && d.limits.slotLimit == 1u && !d.failForSustainedPressure);
    }
    {
        Scenario s;
        s.flow.reservedBytes = GiB; s.flow.byteWaiting = true; s.flow.nextWorkBytes = MiB;
        s.sample.availableBytes = s.state.memory.reserveBytes - MiB; s.Tick();
        const auto d = s.Tick(30s);
        check("idle_retained_storage_waits_full_deadline", d.failForSustainedPressure &&
            !d.failForCapacityBound && s.state.phase == ResourceControlPhase::Hold);
    }
    {
        Scenario s;
        s.flow.admittedBlocks = 1u; s.flow.activeComputeUnits = 1u;
        s.flow.pendingNecessaryWork = true;
        s.sample.availableBytes = s.state.memory.reserveBytes - MiB; s.Tick();
        const auto d = s.Tick(30s);
        check("active_drain_does_not_expire_external_wait_deadline", !d.failForSustainedPressure &&
            s.state.phase == ResourceControlPhase::Drain);
    }
    {
        Scenario s;
        s.sample.availableBytes = s.state.memory.reserveBytes + 4u * MiB;
        s.flow.byteWaiting = true; s.flow.nextWorkBytes = 2u * MiB; s.flow.demandId = 1u;
        const auto d = s.Tick();
        check("required_block_uses_reserve_headroom", d.gateOpen && d.limits.slotLimit == 1u &&
            d.limits.ownedStorageLimitBytes == 2u * MiB && s.state.memory.previewHeadroomBytes == 0u);
    }
    {
        Scenario s;
        const auto ratio = s.state.memory.previewRatio;
        for (int i = 0; i < 10; ++i) { NoteMemoryReservation(s.state, GiB, 0u, s.now); }
        *s.sample.availableBytes -= GiB;
        s.Tick();
        check("reservation_churn_cannot_calibrate_preview", s.state.memory.previewRatio == ratio);
    }
    {
        Scenario s;
        s.sample.availableBytes = s.state.memory.reserveBytes + 4u * MiB;
        s.flow.byteWaiting = true; s.flow.nextWorkBytes = 8u * MiB; s.flow.demandId = 1u;
        s.Tick(); const auto begin = s.state.memory.waitSince;
        for (int i = 0; i < 120; ++i) { ++s.flow.demandId; s.Tick(); }
        check("changed_requests_do_not_reset_no_progress_deadline", begin == s.state.memory.waitSince &&
            s.Tick().failForSustainedPressure);
    }
    std::cout << "CONTROL_RESULT cases=8 failed=" << failed << std::endl;
    return failed ? 6 : 0;
}

}
