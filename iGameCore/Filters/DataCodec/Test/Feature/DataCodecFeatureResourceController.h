#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCECONTROLLER_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCECONTROLLER_H

#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include <cmath>
#include <limits>

namespace datacodec::test {
namespace controller_test {
using namespace std::chrono_literals;
inline constexpr std::uint64_t gib = 1024ull * 1024u * 1024u;
inline constexpr std::uint64_t mib = 1024u * 1024u;

struct Trace {
    ResourceControllerState state;
    FlowSnapshot flow;
    ResourceSample sample;
    ResourceClock::time_point now{ResourceClock::time_point{} + 10s};

    Trace() {
        ResolvedResourceConfiguration config{{0u, 4u, 5u}, 16u * gib, 4u, true, true};
        InitializeResourceController(state, config, now);
        flow.limits = config.initialLimits;
        flow.runActive = true;
        flow.requestId = 1u;
        sample.physicalTotalBytes = 16u * gib;
        sample.availableBytes = 8u * gib;
        sample.allowedComputeThreads = 4u;
        sample.pressure = PressureLevel::Normal;
        Tick(0ms);
    }
    ControlDecision Tick(std::chrono::milliseconds step = 250ms, bool fresh = true) {
        now += step;
        if (fresh) { sample.sampledAt = now; }
        const auto decision = Advance(state, now, sample, flow);
        flow.limits = decision.limits;
        flow.gateOpen = decision.gateOpen;
        return decision;
    }
    void Need(std::uint64_t bytes, MemoryDemandKind kind = MemoryDemandKind::Block) {
        flow.byteWaiting = true;
        flow.nextWorkBytes = bytes;
        flow.demandKind = kind;
        ++flow.demandId;
    }
    void Reserve(std::uint64_t bytes) {
        NoteMemoryReservation(state, bytes, flow.demandId, now);
        flow.reservedBytes += bytes;
        flow.byteWaiting = false;
        flow.nextWorkBytes = 0u;
    }
    void Prepared() {
        state.memory.lastPreparationAt = now;
        state.progressAt = now;
        state.grant.completedAt = now;
    }
};
}

inline TestResult RunDataCodecFeatureResourceController() {
    using namespace controller_test;
    TestResult result;
    {
        ResourceSample sample;
        sample.sampledAt = ResourceClock::now();
        sample.physicalTotalBytes = 16u * gib;
        sample.availableBytes = 8u * gib;
        sample.allowedComputeThreads = 4u;
        sample.jobLimitBytes = 2u * gib;
        sample.jobRemainingBytes = 128u * mib;
        const auto adaptive = ResolveResourceConfiguration({}, sample);
        Require(result, adaptive.targetAvailableMemoryRatio == 0.20 &&
            adaptive.storageCeilingBytes == 2u * gib &&
            adaptive.initialLimits.ownedStorageLimitBytes == 64u * mib &&
            adaptive.initialLimits.computeLimit == 4u,
            "reserve.physical-and-job", "physical denominator and independent environment remaining capacity must be preserved");
        const auto fixed = ResolveResourceConfiguration({.mode = CodecResourceMode::Fixed}, sample);
        Require(result, fixed.storageCeilingBytes == 4u * gib &&
            fixed.initialLimits.ownedStorageLimitBytes == 13u * gib / 4u,
            "reserve.fixed-default", "Fixed retains the old effective-total default");
        const auto zero = ResolveResourceConfiguration({.mode = CodecResourceMode::Fixed, .ownedStorageLimitBytes = 0u}, sample);
        sample.pressure = PressureLevel::Critical;
        const auto unlimited = ResolveResourceConfiguration({.mode = CodecResourceMode::Unlimited}, sample);
        Require(result, zero.initialLimits.ownedStorageLimitBytes == 0u && !unlimited.storageCeilingBytes &&
            unlimited.gateOpen, "reserve.zero-unlimited", "explicit zero and Unlimited must retain distinct semantics");
        sample.threaded = false;
        const auto single = ResolveResourceConfiguration({}, sample);
        Require(result, single.initialLimits.computeLimit == 1u && !single.gateOpen,
            "reserve.single-thread-pressure", "memory observation and native pressure remain active with a single compute thread");
        sample.pressure = PressureLevel::Normal;
        for (const double ratio : {0.0, 0.2, 0.999999}) {
            const auto marks = MakeReserveWatermarks(*sample.physicalTotalBytes, ratio);
            Require(result, marks.low <= marks.high && marks.high <= *sample.physicalTotalBytes,
                "reserve.watermark-bounds", "reserve watermarks must remain within physical capacity");
        }
        for (const auto total : {1ull, 1024ull, 512ull * mib, std::numeric_limits<unsigned long long>::max()}) {
            const auto marks = MakeReserveWatermarks(total, 0.999999);
            Require(result, marks.high <= total && marks.low <= marks.high,
                "reserve.watermark-size", "small and large physical totals must use safe byte arithmetic");
        }
        const auto invalid = [&](CodecResourceParams params, ResourceSample input) {
            try { (void)ResolveResourceConfiguration(params, input); return false; }
            catch (const std::invalid_argument&) { return true; }
        };
        for (const double ratio : {-0.1, 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            Require(result, invalid({.targetAvailableMemoryRatio = ratio}, sample),
                "reserve.invalid-ratio", "invalid numerical ratios must be rejected");
        }
        Require(result, invalid({.ownedStorageLimitBytes = 0u}, sample) &&
            invalid({.mode = CodecResourceMode::Fixed, .targetAvailableMemoryRatio = 0.2}, sample) &&
            invalid({.mode = CodecResourceMode::Unlimited, .targetAvailableMemoryRatio = 0.0}, sample) &&
            invalid({.mode = CodecResourceMode::Unlimited, .ownedStorageLimitBytes = 0u}, sample),
            "reserve.mode-conflicts", "all mutually exclusive memory parameters must be rejected");
        sample.availableBytes.reset();
        Require(result, invalid({}, sample), "reserve.no-observation", "Adaptive requires a current physical sample");
        sample.availableBytes = 17u * gib;
        Require(result, invalid({}, sample), "reserve.invalid-observation", "available memory cannot exceed total memory");
        sample.availableBytes = 8u * gib;
        sample.sampledAt -= 2s;
        Require(result, invalid({}, sample), "reserve.stale-start", "a stale startup sample must be rejected");
    }
    {
        Trace t;
        const auto initial = t.flow.limits;
        for (int i = 0; i < 30; ++i) { t.Tick(); }
        Require(result, initial == t.flow.limits && t.state.memory.grantEpoch == 0u,
            "reserve.no-demand", "healthy unused capacity must not trigger growth or a calibration request");
        t.Need(2u * gib);
        const auto growth = t.Tick();
        Require(result, growth.reason == ResourceDecisionReason::RequiredGrowth &&
            t.flow.limits.ownedStorageLimitBytes == 2u * gib && t.state.memory.grantEpoch == 1u,
            "reserve.atomic-large", "a whole request larger than the preview and step must be admitted when N fits F");
        for (int i = 0; i < 5; ++i) { t.Tick(0ms, false); }
        Require(result, t.state.memory.grantEpoch == 1u, "reserve.unused-grant", "duplicate wakes cannot reuse an unused grant");
        t.Reserve(2u * gib);
        t.flow.admittedBlocks = 1u;
        t.Need(gib);
        t.Tick(750ms);
        Require(result, t.state.memory.grantEpoch == 1u,
            "reserve.retirement-required", "ordinary growth waits for the prior block's retirement");
        t.flow.admittedBlocks = 0u;
        t.state.grant.completedAt = t.now;
        t.Tick(0ms, false);
        Require(result, t.state.memory.grantEpoch == 1u, "reserve.post-retirement-sample", "retirement needs a later sample");
        t.Tick();
        Require(result, t.state.memory.grantEpoch == 2u, "reserve.next-growth", "retired work and a later sample permit the next grant");
    }
    {
        Trace t;
        t.Need(2u * gib, MemoryDemandKind::RequiredContinuation);
        t.Tick();
        t.Reserve(2u * gib);
        t.Prepared();
        t.flow.heavyPhaseAdmitted = true;
        t.Need(2u * gib, MemoryDemandKind::RequiredContinuation);
        t.Tick(0ms, false);
        Require(result, t.state.memory.grantEpoch == 1u, "reserve.preparation-sample", "continuations require a post-preparation sample");
        const auto decision = t.Tick();
        Require(result, decision.reason == ResourceDecisionReason::RequiredContinuation &&
            *t.flow.limits.ownedStorageLimitBytes == 4u * gib && t.state.memory.grantEpoch == 2u &&
            t.flow.reservedBytes == 2u * gib, "reserve.coexistence", "necessary coexisting storage can grow with the heavy phase lease still live");
        t.Reserve(2u * gib);
        t.Prepared();
        t.Need(gib, MemoryDemandKind::RequiredContinuation);
        t.flow.queuedTasks = 1u;
        t.Tick();
        Require(result, t.state.memory.grantEpoch == 2u, "reserve.continuation-queued", "continuation growth waits for queued terminal work");
        t.flow.queuedTasks = 0u;
        t.Tick();
        Require(result, t.state.memory.grantEpoch == 3u, "reserve.continuation-progress", "drained terminal work permits continuation");
    }
    {
        Trace t;
        t.flow.reservedBytes = gib;
        t.flow.activeComputeUnits = 2u;
        t.flow.admittedBlocks = 2u;
        t.sample.availableBytes = 3u * gib;
        auto decision = t.Tick();
        t.flow.pendingNecessaryWork = true;
        const auto wait = t.state.memory.waitSince;
        Require(result, !decision.gateOpen && !decision.trimOptionalRetention &&
            t.state.phase == ResourceControlPhase::Drain && decision.limits.computeLimit == 4u &&
            decision.limits.ownedStorageLimitBytes == gib, "reserve.drain", "low physical reserve closes admission without changing CPU capacity or leases");
        t.Tick();
        decision = t.Tick();
        Require(result, decision.trimOptionalRetention && decision.optionalRetentionPausedByPressure,
            "reserve.confirm-trim", "500 ms of low reserve requests one optional trim");
        decision = t.Tick();
        Require(result, !decision.trimOptionalRetention, "reserve.trim-once", "steady pressure must not repeatedly request trim");
        t.sample.pressure = PressureLevel::Critical;
        decision = t.Tick();
        Require(result, decision.trimOptionalRetention, "reserve.escalation", "native critical pressure may escalate once");
        t.flow.admittedBlocks = t.flow.activeComputeUnits = 0u;
        t.flow.heavyPhaseAdmitted = true;
        t.Tick();
        Require(result, t.state.phase == ResourceControlPhase::Hold && t.state.memory.waitSince.has_value(),
            "reserve.heavy-hold", "an idle heavy phase is a hold boundary and does not reset the deadline");
        t.sample.availableBytes = 8u * gib;
        t.sample.pressure = PressureLevel::Normal;
        for (int i = 0; i < 8; ++i) { decision = t.Tick(); }
        Require(result, decision.gateOpen && t.state.phase == ResourceControlPhase::Recover &&
            decision.limits.slotLimit == 1u && t.state.memory.waitSince.has_value(),
            "reserve.recover-single", "two seconds of healthy samples restore one slot and preserve the unresolved wait");
        t.state.progressAt = t.now;
        for (int i = 0; i < 8; ++i) { decision = t.Tick(); }
        Require(result, t.state.phase == ResourceControlPhase::Normal && decision.limits.slotLimit == 5u &&
            !decision.optionalRetentionPausedByPressure && !t.state.memory.waitSince,
            "reserve.recover-normal", "actual progress plus cooldown and a later healthy sample restore normal retention");
    }
    {
        Trace t;
        t.flow.reservedBytes = gib;
        t.sample.availableBytes.reset();
        t.Need(1u);
        t.Tick();
        const auto started = t.state.memory.waitSince;
        t.Need(1u);
        const auto decision = t.Tick(30s);
        Require(result, decision.failForSustainedPressure && decision.reason == ResourceDecisionReason::WaitDeadlineExceeded &&
            started == t.state.memory.waitSince && !decision.gateOpen,
            "reserve.unavailable-deadline", "invalid samples retain one bounded wait until failure");
    }
    {
        Trace t;
        t.flow.reservedBytes = 2u * gib;
        t.flow.admittedBlocks = 1u;
        t.sample.jobLimitBytes = gib;
        auto decision = t.Tick();
        Require(result, !decision.failForCapacityBound && !decision.gateOpen && t.flow.reservedBytes == 2u * gib &&
            t.state.memory.reserveBytes == MakeReserveWatermarks(16u * gib, 0.2).low,
            "reserve.capability-drain", "a reduced environment limit must preserve physical R and existing leases while draining");
        t.flow.admittedBlocks = 0u;
        decision = t.Tick();
        Require(result, decision.failForCapacityBound && decision.reason == ResourceDecisionReason::CapacityBoundExceeded,
            "reserve.capability-fail", "retained necessary storage above current absolute capacity must fail after drain");
    }
    {
        Trace t;
        t.Need(7u * gib);
        t.Tick();
        auto decision = t.Tick(30s);
        Require(result, decision.failForSustainedPressure && t.flow.limits.ownedStorageLimitBytes == gib,
            "reserve.large-wait-bounded", "an unmet physical reserve constraint cannot wait forever or silently lower the target");
    }
    // 必要增长与预给分开验证，不使用占用周转推导物理响应
    {
        Trace t;
        t.sample.availableBytes = t.state.memory.reserveBytes + 4u * mib;
        t.Need(2u * mib);
        const auto d = t.Tick();
        Require(result, d.gateOpen && d.limits.slotLimit == 1u &&
            d.limits.ownedStorageLimitBytes == 2u * mib && t.state.memory.previewHeadroomBytes == 0u,
            "reserve.single-exact", "a required block fits below the throughput recovery watermark");
        const auto grant = t.state.memory.grantEpoch;
        for (int i = 0; i < 5; ++i) { t.Tick(0ms, false); }
        Require(result, t.state.memory.grantEpoch == grant && t.flow.limits.ownedStorageLimitBytes == 2u * mib,
            "reserve.single-unused-grant", "duplicate wakes preserve the exact outstanding grant");
        t.Reserve(2u * mib);
        t.flow.admittedBlocks = 1u;
        t.Need(mib);
        Require(result, !t.Tick().gateOpen, "reserve.single-live", "single-step admission drains the current block");
        t.flow.admittedBlocks = 0u;
        t.state.progressAt = t.now;
        t.state.grant.completedAt = t.now;
        Require(result, !t.Tick(0ms, false).gateOpen, "reserve.single-fresh", "next block needs a post-retirement sample");
        Require(result, t.Tick().gateOpen, "reserve.single-next", "fresh observed headroom permits another block");
    }
    {
        Trace t;
        t.sample.availableBytes = t.state.memory.reserveBytes + 4u * mib;
        t.Need(8u * mib);
        t.Tick();
        const auto started = t.state.memory.waitSince;
        const auto ratio = t.state.memory.previewRatio;
        for (int i = 0; i < 120; ++i) {
            NoteMemoryReservation(t.state, gib, 0u, t.now);
            t.Need(8u * mib);
            t.Tick();
        }
        Require(result, t.state.memory.waitSince == started && t.Tick().failForSustainedPressure,
            "reserve.wait-real-progress", "request retries and reservation churn do not reset a blocked wait");
        Require(result, t.state.memory.previewRatio == ratio, "reserve.preview-policy", "preview is a policy fraction");
    }
    {
        Trace t;
        t.flow.admittedBlocks = t.flow.activeComputeUnits = 1u;
        t.flow.pendingNecessaryWork = true;
        t.sample.availableBytes = 0u;
        t.Tick();
        Require(result, !t.Tick(31s).failForSustainedPressure, "reserve.active-drain",
            "running work is allowed to drain before external recovery waiting starts");
        t.flow.admittedBlocks = t.flow.activeComputeUnits = 0u;
        t.Tick();
        Require(result, t.Tick(30s).failForSustainedPressure, "reserve.drained-deadline",
            "drained work cannot wait indefinitely for unavailable resources");
    }
    {
        Trace t;
        ControlDecision decision;
        {
            RejectAllocationsScope reject;
            t.sample.availableBytes = gib;
            decision = t.Tick();
        }
        Require(result, !decision.gateOpen && rejectedAllocationCount == 0u,
            "reserve.allocation-free", "controller decisions must not allocate even under low memory");
    }
    return result;
}
}
#endif
