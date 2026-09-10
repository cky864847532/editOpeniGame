#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCECONTROLLER_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCECONTROLLER_H

#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"

namespace datacodec::test {
namespace controller_test {

inline constexpr std::uint64_t gib = 1024u * 1024u * 1024u;

struct Trace {
    ResourceControllerState state;
    FlowSnapshot flow;
    ResourceSample sample;
    ResourceClock::time_point now{};

    Trace() {
        const ResolvedResourceConfiguration config{{gib, 8u, 12u}, gib, 16u, true, true};
        InitializeResourceController(state, config, now);
        state.initialized = true;
        state.requestId = 1u;
        state.signalValid = true;
        flow.limits = config.initialLimits;
        flow.gateOpen = true;
        flow.runActive = true;
        flow.requestId = 1u;
        flow.moreIndependentBlocks = true;
        flow.pendingNecessaryWork = true;
        flow.observation.required = 12u;
        flow.observation.epoch = 1u;
        sample.physicalTotalBytes = 16u * gib;
        sample.availableBytes = 8u * gib;
        sample.allowedComputeThreads = 16u;
        sample.pressure = PressureLevel::Normal;
    }

    ControlDecision Tick(std::chrono::milliseconds elapsed = std::chrono::milliseconds(250),
                         bool newSample = true) {
        now += elapsed;
        if (newSample) { sample.sampledAt = now; }
        const auto decision = Advance(state, now, sample, flow);
        const bool changed = decision.limits != flow.limits || decision.gateOpen != flow.gateOpen;
        flow.limits = decision.limits;
        flow.gateOpen = decision.gateOpen;
        if (changed || decision.resetObservation) {
            const auto epoch = flow.observation.epoch + 1u;
            flow.observation = {};
            flow.observation.epoch = epoch;
            flow.observation.startedAt = now;
            flow.observation.required = flow.limits.slotLimit;
        }
        return decision;
    }
};

}

inline TestResult RunDataCodecFeatureResourceController() {
    using namespace controller_test;
    using namespace std::chrono_literals;
    TestResult result;
    {
        const auto small = MakeResourceWatermarks(gib / 4u);
        const auto large = MakeResourceWatermarks(1024u * gib);
        Require(result, small.low == gib / 16u && small.high == 3u * gib / 32u &&
            large.low == 4u * gib && large.high == 6u * gib,
            "controller.watermarks", "small and large capacities must use the specified watermarks");
        ResourceSample sample;
        sample.physicalTotalBytes = 16u * gib;
        sample.availableBytes = 8u * gib;
        sample.allowedComputeThreads = 16u;
        sample.runtimeThreadLimit = 8u;
        sample.reservedHostThreads = 1u;
        sample.pressure = PressureLevel::Normal;
        const auto fixed = ResolveResourceConfiguration({.mode = CodecResourceMode::Fixed,
            .maxComputeThreads = 4u, .ownedStorageLimitBytes = gib}, sample);
        const auto adaptive = ResolveResourceConfiguration({.mode = CodecResourceMode::Adaptive,
            .maxComputeThreads = 4u, .ownedStorageLimitBytes = gib}, sample);
        Require(result, fixed.initialLimits == RuntimeResourceLimits{gib, 4u, 5u} &&
            adaptive.initialLimits == RuntimeResourceLimits{gib, 4u, 5u} &&
            ResourceComputeCapacity(sample, CodecResourceMode::Fixed) == 6u &&
            ResourceComputeCapacity(sample, CodecResourceMode::Adaptive) == 5u,
            "controller.modes", "both modes must share capability validation and bound healthy startup concurrency");
    }
    {
        for (const std::size_t ceiling : {1u, 2u, 4u, 8u, 16u}) {
            Trace trace;
            trace.state.initialized = false;
            trace.state.computeCeiling = ceiling;
            trace.state.currentComputeCeiling = ceiling;
            trace.flow.limits = {gib, 1u, 1u};
            const auto initial = trace.Tick();
            const auto compute = std::min<std::size_t>(ceiling, 8u);
            Require(result, initial.limits.computeLimit == compute && initial.limits.slotLimit == compute + 1u,
                "controller.healthy-startup-bound", "healthy startup must respect small CPU ceilings and the eight-worker bound");
            trace.flow.workType.components = 3u;
            const auto changed = trace.Tick();
            Require(result, changed.limits.computeLimit == compute && changed.limits.slotLimit == compute + 1u,
                "controller.healthy-type-bound", "a healthy type boundary must use the bounded startup target");
            trace.state.everConfirmedPressure = true;
            trace.state.cooldownUntil = trace.now + std::chrono::seconds(10);
            trace.flow.limits = {gib, 1u, 1u};
            ++trace.flow.workType.components;
            const auto recovering = trace.Tick();
            Require(result, recovering.limits.computeLimit == 1u && recovering.limits.slotLimit == 1u,
                "controller.type-keeps-pressure-reduction", "a type change must not restore healthy startup concurrency after pressure");
        }
        Trace trace;
        trace.state.initialized = false;
        trace.sample.availableBytes.reset();
        Require(result, trace.Tick().limits.computeLimit == 1u,
            "controller.unknown-startup", "unknown capacity must retain single-worker startup");
    }
    {
        Trace trace;
        trace.flow.admittedBlocks = 8u;
        trace.flow.activeComputeUnits = 8u;
        trace.sample.availableBytes = 3u * gib / 4u;
        const auto first = trace.Tick();
        trace.Tick();
        const auto confirmed = trace.Tick();
        const auto repeated = trace.Tick();
        Require(result, !first.gateOpen && first.limits.computeLimit == 8u &&
            confirmed.limits == RuntimeResourceLimits{gib, 4u, 5u} &&
            confirmed.trimOptionalRetention && !repeated.trimOptionalRetention &&
            repeated.limits == confirmed.limits && trace.state.phase == ResourceControlPhase::Drain,
            "controller.confirm-once", "ordinary pressure must close admission immediately and shrink once after confirmation");
        trace.sample.pressure = PressureLevel::Critical;
        const auto severe = trace.Tick();
        Require(result, severe.limits == RuntimeResourceLimits{gib, 1u, 1u} &&
            !severe.trimOptionalRetention && severe.reason == ResourceDecisionReason::PressureEscalated,
            "controller.escalation", "critical escalation must preserve M and avoid a repeated full trim event");
        trace.flow.admittedBlocks = 0u;
        trace.flow.activeComputeUnits = 0u;
        trace.Tick();
        const auto holdStart = trace.state.holdSince;
        trace.sample.pressure = PressureLevel::Unknown;
        trace.sample.availableBytes.reset();
        for (int i = 0; i < 119; ++i) { trace.Tick(); }
        const auto timeout = trace.Tick();
        Require(result, timeout.failForSustainedPressure && trace.state.holdSince == holdStart &&
            timeout.limits.ownedStorageLimitBytes == gib,
            "controller.hold-deadline", "unknown signals and repeated notifications must preserve the original pressure deadline");
    }
    {
        Trace trace;
        trace.sample.availableBytes = 3u * gib / 4u;
        trace.Tick();
        trace.sample.availableBytes = 2u * gib;
        trace.Tick();
        trace.Tick();
        const auto cleared = trace.Tick();
        Require(result, cleared.gateOpen && cleared.limits == RuntimeResourceLimits{gib, 8u, 12u} &&
            !trace.state.everConfirmedPressure,
            "controller.short-pulse", "a cleared low-water pulse must preserve the original targets");
    }
    {
        Trace trace;
        trace.flow.moreIndependentBlocks = false;
        trace.sample.pressure = PressureLevel::Low;
        trace.Tick();
        Require(result, trace.state.phase == ResourceControlPhase::Hold,
            "controller.empty-drain", "a drained request with pending work must enter hold");
        trace.sample.pressure = PressureLevel::Normal;
        for (int i = 0; i < 9; ++i) { trace.Tick(); }
        Require(result, trace.flow.gateOpen && trace.state.phase == ResourceControlPhase::Normal &&
            trace.state.optionalRetentionPausedByPressure && trace.flow.limits.ownedStorageLimitBytes == gib,
            "controller.recover", "recovery must retain the pressure-reduced compute targets and preserve M");
        const auto recoveryAt = trace.now;
        for (int i = 0; i < 39; ++i) { trace.Tick(); }
        Require(result, trace.state.optionalRetentionPausedByPressure,
            "controller.retention-cooldown", "optional retention must remain paused until the growth cooldown ends");
        trace.Tick();
        Require(result, !trace.state.optionalRetentionPausedByPressure && trace.now - recoveryAt == 10s,
            "controller.retention-restore", "valid high-water samples must restore retention after cooldown");
        trace.sample.pressure = PressureLevel::Low;
        trace.Tick();
        Require(result, trace.state.cooldown == 20s,
            "controller.repeat-backoff", "pressure within the recovery window must double the next cooldown");
    }
    {
        Trace trace;
        trace.flow.admittedBlocks = 2u;
        trace.flow.activeComputeUnits = 1u;
        trace.Tick();
        const auto lost = trace.Tick(1500ms, false);
        Require(result, !lost.gateOpen && lost.limits == RuntimeResourceLimits{gib, 1u, 1u},
            "controller.signal-loss", "a stale sample must close admission and drain at the conservative targets");
        trace.flow.admittedBlocks = 0u;
        trace.flow.activeComputeUnits = 0u;
        trace.Tick(250ms, false);
        const auto stillUnknown = trace.Tick(250ms, false);
        Require(result, stillUnknown.gateOpen && trace.state.phase == ResourceControlPhase::Normal,
            "controller.unknown-progress", "unknown signals without confirmed pressure must permit conservative progress");
        for (int i = 0; i < 9; ++i) { trace.Tick(); }
        Require(result, !trace.state.signalRecoveryPending && trace.state.cooldownUntil > trace.now,
            "controller.signal-recovery", "signal recovery requires fresh high-water samples and a new cooldown");
    }
    {
        Trace trace;
        trace.flow.limits = {gib, 1u, 2u};
        trace.flow.observation.required = 2u;
        trace.Tick();
        trace.Tick();
        trace.flow.observation.admitted = 2u;
        trace.flow.observation.lastSequence = 1u;
        trace.flow.observation.waits.compute = 500ms;
        const auto noRetirement = trace.Tick();
        Require(result, noRetirement.limits.computeLimit == 1u,
            "controller.retire-required", "completed computation must not complete a slot observation batch");
        trace.flow.observation.retiredAt = trace.now;
        const auto grown = trace.Tick();
        Require(result, grown.reason == ResourceDecisionReason::ComputeGrowth &&
            grown.limits == RuntimeResourceLimits{gib, 2u, 3u},
            "controller.compute-growth", "a fully retired batch and a fresh sample must allow demand-driven compute growth");
    }
    {
        Trace trace;
        trace.flow.limits = {gib, 1u, 2u};
        trace.flow.observation.required = 2u;
        trace.Tick();
        trace.Tick();
        trace.flow.observation.admitted = 2u;
        trace.flow.observation.retiredAt = trace.now;
        trace.flow.observation.waits.compute = 500ms;
        trace.flow.observation.waits.output = 500ms;
        const auto blocked = trace.Tick();
        Require(result, blocked.limits.computeLimit == 1u && blocked.resetObservation,
            "controller.commit-backpressure", "persistent commit congestion must end observation without growth");
    }
    {
        Trace trace;
        trace.flow.moreIndependentBlocks = false;
        trace.state.storageCeilingBytes = 2u * gib;
        for (int i = 0; i < 19; ++i) { trace.Tick(); }
        const auto growth = trace.Tick();
        Require(result, growth.reason == ResourceDecisionReason::StorageGrowth &&
            growth.limits.ownedStorageLimitBytes == gib + gib / 8u,
            "controller.storage-growth", "M growth must use the global ceiling step and a five-second interval");
        trace.sample.hardLimitBytes = gib / 2u;
        const auto lowered = trace.Tick();
        Require(result, lowered.limits.ownedStorageLimitBytes == gib / 2u,
            "controller.hard-limit", "a known hard capability reduction must immediately constrain future capacity");
    }
    {
        Trace trace;
        trace.state.threaded = false;
        trace.sample.threaded = false;
        trace.state.initialized = false;
        trace.sample.pressure = PressureLevel::Critical;
        const auto decision = trace.Tick();
        Require(result, decision.gateOpen && decision.limits.computeLimit == 1u &&
            decision.limits.slotLimit == 1u && !decision.failForSustainedPressure,
            "controller.no-monitor", "a runtime without threads must execute the conservative same-mechanism path");
    }
    {
        Trace trace;
        trace.flow.admittedBlocks = 1u;
        trace.sample.availableBytes = 3u * gib / 4u;
        trace.Tick();
        const auto pendingAt = trace.now;
        bool boundedPending = true;
        for (unsigned i = 1u; i < 8u; ++i) {
            trace.sample.availableBytes = i % 2u ? 5u * gib / 4u : 3u * gib / 4u;
            const auto decision = trace.Tick();
            boundedPending &= !decision.gateOpen && !trace.state.pressureConfirmed &&
                decision.limits.computeLimit == 8u;
        }
        const auto confirmed = trace.Tick();
        Require(result, boundedPending && trace.now - pendingAt == 2s &&
            trace.state.pressureConfirmed && trace.state.phase == ResourceControlPhase::Drain &&
            confirmed.limits == RuntimeResourceLimits{gib, 4u, 5u} && trace.flow.admittedBlocks == 1u,
            "controller.oscillation-deadline", "alternating watermarks must confirm pressure within two seconds without rewriting live work");
    }
    {
        Trace trace;
        trace.sample.availableBytes = 5u * gib / 4u;
        trace.flow.observation.admitted = trace.flow.observation.required;
        trace.flow.observation.retiredAt = trace.now;
        trace.flow.observation.waits.compute = 10s;
        bool unchanged = true;
        for (unsigned i = 0u; i < 12u; ++i) {
            const auto decision = trace.Tick();
            unchanged &= decision.gateOpen && decision.limits == RuntimeResourceLimits{gib, 8u, 12u};
        }
        trace.sample.pressure = PressureLevel::Low;
        trace.Tick();
        trace.sample.pressure = PressureLevel::Normal;
        bool held = true;
        for (unsigned i = 0u; i < 12u; ++i) {
            const auto decision = trace.Tick();
            held &= !decision.gateOpen && trace.state.phase == ResourceControlPhase::Hold;
        }
        Require(result, unchanged && held, "controller.middle-band",
            "the middle water band must neither grow normal work nor release a confirmed pressure hold");
    }
    {
        Trace trace;
        bool bounded = true;
        for (const auto expected : {10s, 20s, 40s, 60s, 60s}) {
            trace.sample.pressure = PressureLevel::Low;
            const auto pressure = trace.Tick();
            bounded &= !pressure.gateOpen && trace.state.cooldown == expected &&
                pressure.limits.ownedStorageLimitBytes == gib;
            trace.sample.pressure = PressureLevel::Normal;
            for (unsigned i = 0u; i < 9u; ++i) { trace.Tick(); }
            bounded &= trace.flow.gateOpen && trace.state.phase == ResourceControlPhase::Normal &&
                trace.state.cooldownUntil - trace.now == expected;
        }
        Require(result, bounded, "controller.backoff-ceiling",
            "repeated recovered pressure must use ten twenty forty sixty seconds and stay at the fixed ceiling");
    }
    {
        Trace trace;
        trace.sample.allowedComputeThreads = 18u;
        trace.flow.limits = {gib, 2u, 2u};
        trace.flow.observation.required = 2u;
        trace.Tick();
        const auto samples = trace.state.observationSampleCount;
        bool preserved = true;
        for (unsigned i = 0u; i < 8u; ++i) {
            const auto repeated = trace.Tick(0ms, false);
            preserved &= repeated.limits == trace.flow.limits && !repeated.resetObservation &&
                trace.state.observationSampleCount == samples;
        }
        trace.Tick();
        trace.flow.observation.admitted = 2u;
        trace.flow.observation.retiredAt = trace.now;
        trace.flow.observation.waits.slots = 500ms;
        trace.flow.observation.waits.input = 250ms;
        const auto grown = trace.Tick();
        Require(result, preserved && grown.reason == ResourceDecisionReason::SlotGrowth &&
            grown.limits == RuntimeResourceLimits{gib, 2u, 3u},
            "controller.fresh-slot-growth", "duplicate samples must preserve observation and input shortage may grow only slots");
    }
    {
        bool bounded = true;
        for (const std::size_t ceiling : {1u, 2u, 4u, 8u, 16u, 64u}) {
            Trace trace;
            trace.state.computeCeiling = ceiling;
            trace.state.currentComputeCeiling = ceiling;
            trace.sample.allowedComputeThreads = ceiling + 2u;
            trace.flow.limits = {gib, 1u, 1u};
            trace.flow.observation.required = 1u;
            const auto extra = std::min<std::size_t>(ceiling, 8u);
            for (unsigned batch = 0u; batch < 80u; ++batch) {
                trace.Tick();
                trace.Tick();
                trace.flow.observation.admitted = trace.flow.observation.required;
                trace.flow.observation.retiredAt = trace.now;
                trace.flow.observation.waits.compute = 500ms;
                const auto before = trace.flow.limits;
                const auto after = trace.Tick().limits;
                bounded &= after.computeLimit >= before.computeLimit &&
                    after.computeLimit - before.computeLimit <= 4u && after.computeLimit <= ceiling &&
                    after.slotLimit >= after.computeLimit && after.slotLimit <= ceiling + extra &&
                    after.slotLimit <= after.computeLimit + extra && after.ownedStorageLimitBytes == gib;
            }
            bounded &= trace.flow.limits.computeLimit == ceiling;
        }
        Require(result, bounded, "controller.growth-parameter-matrix",
            "completed batches must reach each CPU ceiling with bounded steps and a bounded slot surplus");
    }
    {
        Trace trace;
        trace.sample.allowedComputeThreads = 18u;
        trace.flow.limits = {gib / 2u, 2u, 3u};
        trace.flow.observation.required = 3u;
        for (unsigned i = 0u; i < 19u; ++i) { trace.Tick(); }
        trace.flow.observation.admitted = 3u;
        trace.flow.observation.retiredAt = trace.now;
        trace.flow.observation.waits.compute = 4s;
        const auto grown = trace.Tick();
        Require(result, grown.reason == ResourceDecisionReason::StorageGrowth &&
            grown.limits == RuntimeResourceLimits{9u * gib / 16u, 2u, 3u},
            "controller.storage-compute-exclusive", "storage and compute growth must not be applied in the same decision");
    }
    {
        Trace trace;
        trace.flow.heavyPhaseAdmitted = true;
        trace.flow.workType.components = 3u;
        const auto pendingType = trace.Tick();
        const auto priorType = trace.state.workType;
        trace.flow.heavyPhaseAdmitted = false;
        const auto changed = trace.Tick();
        Require(result, pendingType.reason != ResourceDecisionReason::WorkTypeChanged &&
            priorType != trace.flow.workType && changed.reason == ResourceDecisionReason::WorkTypeChanged &&
            trace.state.workType == trace.flow.workType,
            "controller.type-waits-heavy-phase", "type changes must wait until the preceding non-block phase retires");
    }
    {
        Trace trace;
        trace.state.storageCeilingBytes = 4u * gib;
        trace.sample.availableBytes = 3u * gib;
        trace.flow.requestId = 2u;
        const auto lower = trace.Tick();
        trace.sample.availableBytes = 8u * gib;
        const auto sameRequest = trace.Tick();
        ++trace.flow.workType.components;
        const auto field = trace.Tick();
        trace.flow.requestId = 3u;
        const auto nextRequest = trace.Tick();
        Require(result, lower.limits.ownedStorageLimitBytes == 3u * gib / 4u &&
            sameRequest.limits.ownedStorageLimitBytes == lower.limits.ownedStorageLimitBytes &&
            field.limits.ownedStorageLimitBytes == lower.limits.ownedStorageLimitBytes &&
            nextRequest.limits.ownedStorageLimitBytes == 13u * gib / 4u,
            "controller.request-memory-boundary", "only a new independent request may immediately recalculate M from available memory");
    }
    {
        // 使用真实容量 lease 验证控制决策，不分配六百 MiB 测试数据
        constexpr std::uint64_t mib = 1024u * 1024u;
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{gib, 8u, 12u}, gib, 16u, true, true});
        auto capacity = run.StorageCapacity();
        auto reference = capacity->TryReserve(600u * mib);
        Trace trace;
        trace.sample.pressure = PressureLevel::Critical;
        const auto pressure = trace.Tick();
        bool kept = run.UpdateLimits(pressure.limits, pressure.gateOpen, pressure.reason);
        trace.sample.pressure = PressureLevel::Normal;
        for (unsigned i = 0u; i < 9u; ++i) { trace.Tick(); }
        auto next = capacity->TryReserve(64u * mib);
        kept &= reference && next && trace.flow.gateOpen && trace.flow.limits.ownedStorageLimitBytes == gib &&
            capacity->Snapshot().reservedBytes == 664u * mib;
        next.reset();
        trace.sample.hardLimitBytes = 512u * mib;
        const auto hard = trace.Tick();
        const bool applied = run.UpdateLimits(hard.limits, hard.gateOpen, hard.reason);
        Require(result, kept && applied && capacity->Snapshot().limitBytes == 512u * mib &&
            capacity->Snapshot().reservedBytes == 600u * mib && !capacity->TryReserve(64u * mib),
            "controller.reference-hard-limit", "pressure must preserve necessary capacity and a real hard-limit reduction must preserve live leases while denying new growth");
    }
    {
        Trace trace;
        trace.sample.hardRemainingBytes = 96u * 1024u * 1024u;
        const auto low = trace.Tick();
        Require(result, !low.gateOpen && low.limits.ownedStorageLimitBytes == gib &&
            !trace.state.currentHardLimitBytes,
            "controller.remaining-is-not-ceiling", "remaining environment capacity must signal pressure without becoming M's hard ceiling");
    }
    {
        Trace trace;
        trace.flow.admittedBlocks = 1u;
        trace.sample.pressure = PressureLevel::Critical;
        trace.Tick();
        trace.flow.admittedBlocks = 0u;
        trace.flow.pendingNecessaryWork = false;
        const auto finished = trace.Tick();
        const auto later = trace.Tick(31s);
        Require(result, !finished.failForSustainedPressure && !later.failForSustainedPressure &&
            !trace.state.holdSince && trace.state.phase != ResourceControlPhase::Hold,
            "controller.finished-with-pressure", "fully committed requests must not acquire a pressure hold or await recovery");
    }
    {
        Trace trace;
        bool allocationFree = false;
        {
            RejectAllocationsScope reject;
            const auto decision = trace.Tick();
            allocationFree = decision.gateOpen && rejectedAllocationCount == 0u;
        }
        Require(result, allocationFree, "controller.no-allocation",
            "pure controller advancement must not allocate memory");
    }
    return result;
}

}

#endif
