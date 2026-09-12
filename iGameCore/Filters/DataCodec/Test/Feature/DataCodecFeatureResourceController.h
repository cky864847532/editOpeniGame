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
        Require(result, initial == t.flow.limits && t.state.memory.grantEpoch == 0u &&
            t.state.memory.calibration == MemoryCalibrationResult::Pending,
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
        Require(result, t.state.phase == ResourceControlPhase::Hold && t.state.memory.waitSince == wait,
            "reserve.heavy-hold", "an idle heavy phase is a hold boundary and does not reset the deadline");
        t.sample.availableBytes = 8u * gib;
        t.sample.pressure = PressureLevel::Normal;
        for (int i = 0; i < 9; ++i) { decision = t.Tick(); }
        Require(result, decision.gateOpen && t.state.phase == ResourceControlPhase::Recover &&
            decision.limits.slotLimit == 1u && t.state.memory.waitSince == wait,
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
    // 首次测量完全使用合成时间，成功预约计数与 lease 归还量分别驱动
    const auto measure = [&](double responseFactor) {
        Trace t;
        t.Need(2u * gib);
        t.Tick(0ms);
        t.Reserve(2u * gib);
        t.Prepared();
        t.Tick();
        Require(result, t.state.gainPhase == 2u, "gain.baseline-window", "the baseline window must complete after 250 ms");
        t.Need(gib / 2u);
        t.Tick();
        Require(result, t.state.grant.probe && *t.flow.limits.ownedStorageLimitBytes == 2u * gib + 3u * gib / 4u,
            "gain.probe-preview", "one initial probe reduces only the optional preview by a quarter");
        t.Reserve(gib / 2u);
        t.flow.reservedBytes -= gib;
        t.sample.availableBytes = 8u * gib - static_cast<std::uint64_t>(responseFactor * static_cast<double>(gib / 2u));
        t.Tick();
        return t;
    };
    {
        auto t = measure(2.0);
        Require(result, t.state.memory.calibration == MemoryCalibrationResult::Estimated &&
            t.state.memory.gain == 0.25 && t.state.memory.measuredReservationBytes == gib / 2u &&
            t.state.memory.gainUpdates == 1u && t.state.memory.calibrationElapsed == 750ms,
            "gain.estimated", "positive response estimates Km once using gross new reservations within one second");
        t.Tick(0ms, false);
        Require(result, t.state.memory.gainUpdates == 1u, "gain.single-use", "a completed measurement pair must not be reused");
        ++t.flow.requestId;
        t.Tick();
        Require(result, t.state.memory.gain == 0.5 && t.state.memory.gainUpdates == 0u &&
            t.state.memory.calibration == MemoryCalibrationResult::Pending, "gain.new-request", "new requests reset nominal Km");
    }
    {
        const auto t = measure(0.0);
        Require(result, t.state.memory.calibration == MemoryCalibrationResult::Unusable && t.state.memory.gain == 0.5 &&
            !t.state.memory.response, "gain.no-response", "zero response retains nominal gain");
    }
    {
        Trace t;
        t.Need(7u * gib);
        t.Tick(0ms);
        t.Tick();
        t.Tick(750ms);
        Require(result, t.state.memory.calibration == MemoryCalibrationResult::TimedOut && t.state.memory.gain == 0.5,
            "gain.timeout", "waiting for a reservation cannot extend the one-second initial attempt");
        t.Need(2u * gib);
        t.Tick();
        t.Reserve(2u * gib);
        t.sample.availableBytes = 7u * gib;
        t.Tick();
        Require(result, t.state.memory.gainUpdates == 1u && t.state.memory.gain == 1.0,
            "gain.passive-update", "ordinary growth after a failed initial attempt can update the clamped gain");
    }
    {
        Trace t;
        t.state.gainPhase = 3u;
        t.state.gainWindowStarted = t.now;
        t.state.memory.baselineAvailable = 8.0 * gib;
        t.state.measurementBytes = 1u;
        t.sample.availableBytes = 4u * gib;
        t.Tick();
        Require(result, t.state.memory.gain == resource_control::minimumGain,
            "gain.minimum-clamp", "a large valid response must clamp to the minimum gain");
        t.state.gainPhase = 3u;
        t.state.gainWindowStarted = t.now;
        t.state.availableSamples = 0u;
        t.state.availableSum = 0.0;
        t.state.memory.baselineAvailable = 4.0 * gib;
        t.state.measurementBytes = 1u;
        t.sample.availableBytes = 5u * gib;
        t.Tick();
        Require(result, t.state.memory.gain == resource_control::minimumGain && t.state.memory.gainUpdates == 1u,
            "gain.negative-response", "a negative response must preserve the previous observed gain");
        t.state.gainPhase = 2u;
        t.state.initialMeasurement = true;
        t.sample.jobLimitBytes = 10u * gib;
        t.Tick();
        Require(result, t.state.gainPhase == 0u &&
            t.state.memory.calibrationEndReason == ResourceDecisionReason::HardCapabilityChanged,
            "gain.capability-change", "capacity changes terminate an active measurement");
    }
    {
        Trace t;
        t.Need(2u * gib);
        t.Tick(0ms);
        t.Tick();
        t.sample.availableBytes = 0u;
        t.Tick();
        Require(result, t.state.memory.calibration == MemoryCalibrationResult::Unusable &&
            t.state.memory.calibrationEndReason == ResourceDecisionReason::ReserveTargetLow &&
            !t.flow.gateOpen, "gain.pressure-first", "pressure must terminate measurement before accepting a response");
    }
    {
        Trace t;
        t.Need(2u * gib);
        t.Tick(0ms);
        t.Tick();
        FinishMemoryMeasurement(t.state, t.now, ResourceDecisionReason::RequestCancelled);
        Require(result, t.state.memory.calibration == MemoryCalibrationResult::Unusable &&
            t.state.memory.calibrationEndReason == ResourceDecisionReason::RequestCancelled,
            "gain.cancel", "cancellation records an explicit incomplete observation");
        t.state.initialMeasurement = true;
        t.state.gainPhase = 2u;
        t.state.measurementBytes = std::numeric_limits<std::uint64_t>::max();
        NoteMemoryReservation(t.state, 1u, 0u, t.now);
        Require(result, t.state.gainPhase == 0u && t.state.memory.calibrationEndReason == ResourceDecisionReason::CapacityBoundExceeded,
            "gain.counter-overflow", "reservation counter overflow ends the measurement without affecting capacity");
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
