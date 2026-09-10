#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace datacodec {

namespace {

std::optional<std::uint64_t> EffectiveTotal(const ResourceSample& sample) noexcept {
    if (!sample.physicalTotalBytes || *sample.physicalTotalBytes == 0u) { return std::nullopt; }
    return std::min(*sample.physicalTotalBytes,
        sample.hardLimitBytes.value_or(std::numeric_limits<std::uint64_t>::max()));
}

std::optional<std::uint64_t> EffectiveAvailable(const ResourceSample& sample) noexcept {
    if (!sample.availableBytes) { return std::nullopt; }
    return std::min(*sample.availableBytes,
        sample.hardRemainingBytes.value_or(std::numeric_limits<std::uint64_t>::max()));
}

bool Elapsed(const std::optional<ResourceClock::time_point>& since,
             ResourceClock::time_point now, ResourceClock::duration duration) noexcept {
    return since && now >= *since && now - *since >= duration;
}

void ResetObservation(ResourceControllerState& state, ControlDecision& decision) noexcept {
    state.firstObservationSampleAt.reset();
    state.observationSampleCount = 0u;
    decision.resetObservation = true;
}

void ClearContinuousSamples(ResourceControllerState& state, ControlDecision& decision) noexcept {
    state.highSince.reset();
    state.lowSince.reset();
    state.pendingClearSince.reset();
    ResetObservation(state, decision);
}

bool FractionAtLeast(ResourceClock::duration part, ResourceClock::duration whole,
                     unsigned percent) noexcept {
    return whole > ResourceClock::duration::zero() &&
        std::chrono::duration<double>(part).count() >=
            std::chrono::duration<double>(whole).count() * static_cast<double>(percent) / 100.0;
}

}

void InitializeResourceController(ResourceControllerState& state,
                                  const ResolvedResourceConfiguration& configuration,
                                  ResourceClock::time_point now) noexcept {
    state = {};
    state.storageCeilingBytes = configuration.storageCeilingBytes;
    state.computeCeiling = configuration.computeCeiling;
    state.currentComputeCeiling = configuration.computeCeiling;
    state.threaded = configuration.threaded;
    state.lastGrowthAt = now;
    state.lastStorageGrowthAt = now;
}

ControlDecision Advance(ResourceControllerState& state, ResourceClock::time_point now,
                        const ResourceSample& sample, const FlowSnapshot& flow) noexcept {
    using namespace resource_control;
    ControlDecision decision;
    decision.limits = flow.limits;
    decision.gateOpen = flow.gateOpen;
    decision.optionalRetentionPausedByPressure = state.optionalRetentionPausedByPressure;
    if (flow.stopped || !flow.runActive) { return decision; }

    const bool newRequest = state.requestId != flow.requestId;
    if (newRequest) {
        state.requestId = flow.requestId;
        state.holdSince.reset();
        state.lastGrowthAt = now;
        state.lastStorageGrowthAt = now;
        ClearContinuousSamples(state, decision);
        decision.reason = ResourceDecisionReason::BeginRequest;
    }
    const bool fresh = sample.sampledAt <= now && now - sample.sampledAt <= maximumSampleAge;
    const bool newSample = fresh && (!state.lastSampleAt || sample.sampledAt > *state.lastSampleAt);
    const bool gap = newSample && state.lastSampleAt &&
        sample.sampledAt - *state.lastSampleAt > maximumSampleAge;
    const auto total = EffectiveTotal(sample);
    const auto available = EffectiveAvailable(sample);
    const bool valid = state.threaded && fresh && total && *total != 0u && available;
    const auto marks = total ? MakeResourceWatermarks(*total) : ResourceWatermarks{};
    const bool wasValid = state.signalValid;
    if (gap) { ClearContinuousSamples(state, decision); }
    if (newSample) {
        state.lastSampleAt = sample.sampledAt;
        if (sample.pressure == PressureLevel::Normal) { state.nativePressureActive = false; }
        if (sample.pressure == PressureLevel::Low || sample.pressure == PressureLevel::Critical) {
            state.nativePressureActive = true;
        }
    }
    const bool severe = state.threaded && fresh && (sample.pressure == PressureLevel::Critical ||
        (valid && *available < marks.critical));
    const bool nativeLow = state.threaded && fresh && sample.pressure == PressureLevel::Low;
    const bool low = valid && *available < marks.low;
    const bool high = valid && *available >= marks.high && !state.nativePressureActive;
    state.signalValid = valid;

    // 当前硬能力单独保存，样本未知时保持最后一次已知值
    bool capabilityChanged = false;
    if (fresh && sample.hardLimitBytes &&
        state.currentHardLimitBytes != sample.hardLimitBytes) {
        state.currentHardLimitBytes = sample.hardLimitBytes;
        capabilityChanged = true;
    }
    if (fresh && sample.allowedComputeThreads) {
        const auto allowed = state.threaded
            ? std::max<std::size_t>(1u, std::min(state.computeCeiling,
                ResourceComputeCapacity(sample, CodecResourceMode::Adaptive))) : 1u;
        if (allowed != state.currentComputeCeiling) {
            state.currentComputeCeiling = allowed;
            capabilityChanged = true;
        }
    }
    const auto memoryBound = std::min(state.storageCeilingBytes,
        state.currentHardLimitBytes.value_or(std::numeric_limits<std::uint64_t>::max()));
    const auto p = state.currentComputeCeiling;
    const auto q = std::min<std::size_t>(p, 8u);
    const auto slotMaximum = state.threaded ? p + q : 1u;
    if (capabilityChanged) {
        decision.limits.ownedStorageLimitBytes = std::min(decision.limits.ownedStorageLimitBytes, memoryBound);
        decision.limits.computeLimit = std::min(decision.limits.computeLimit, p);
        decision.limits.slotLimit = std::min({decision.limits.slotLimit, slotMaximum,
            decision.limits.computeLimit + q});
        ResetObservation(state, decision);
        decision.reason = ResourceDecisionReason::HardCapabilityChanged;
    }
    if (newRequest && fresh && total && available) {
        decision.limits.ownedStorageLimitBytes = std::min(memoryBound,
            (*available - std::min(*available, marks.high)) / 2u);
    }
    if (!state.initialized) {
        state.initialized = true;
        decision.limits.computeLimit = high ? std::min(p, healthyStartupComputeLimit) : 1u;
        decision.limits.slotLimit = high ? decision.limits.computeLimit + 1u : 1u;
        decision.gateOpen = !severe && !nativeLow && !low;
    }

    if (newSample) {
        if (high) {
            if (!state.highSince) { state.highSince = sample.sampledAt; }
        } else { state.highSince.reset(); }
        if (low) {
            if (!state.lowSince) { state.lowSince = sample.sampledAt; }
        } else { state.lowSince.reset(); }
    }

    const auto confirmPressure = [&](bool critical) {
        const bool firstEvent = !state.pressureConfirmed;
        const bool escalation = critical && !state.severePressure;
        if (firstEvent) {
            if (state.lastRecoveryAt && now - *state.lastRecoveryAt <= repeatPressureWindow) {
                state.cooldown = std::min<ResourceClock::duration>(maximumCooldown, state.cooldown * 2);
            } else { state.cooldown = initialCooldown; }
            state.lastPressureAt = now;
            state.everConfirmedPressure = true;
            state.pressureConfirmed = true;
            state.optionalRetentionPausedByPressure = true;
            decision.optionalRetentionPausedByPressure = true;
            decision.trimOptionalRetention = true;
            decision.reason = ResourceDecisionReason::PressureConfirmed;
        }
        if (critical) {
            decision.limits.computeLimit = 1u;
            decision.limits.slotLimit = 1u;
            if (!firstEvent && !state.severePressure) {
                decision.reason = ResourceDecisionReason::PressureEscalated;
            }
            state.severePressure = true;
        } else if (firstEvent) {
            const auto compute = std::max<std::size_t>(1u, decision.limits.computeLimit / 2u);
            decision.limits.computeLimit = compute;
            decision.limits.slotLimit = std::max(compute, std::min(decision.limits.slotLimit, compute + 1u));
        }
        state.pressurePending = false;
        state.pendingSince.reset();
        state.pendingClearSince.reset();
        decision.gateOpen = false;
        if (state.phase == ResourceControlPhase::Normal) { state.phase = ResourceControlPhase::Drain; }
        if (firstEvent || escalation) { ResetObservation(state, decision); }
    };

    if (severe) { confirmPressure(true); }
    else if (nativeLow) { confirmPressure(false); }
    else if (state.phase == ResourceControlPhase::Normal && !state.pressureConfirmed) {
        if (low && !state.pressurePending) {
            state.pressurePending = true;
            state.pendingSince = now;
            state.pendingClearSince.reset();
            decision.gateOpen = false;
            decision.reason = ResourceDecisionReason::PressurePending;
            ResetObservation(state, decision);
        }
        if (state.pressurePending) {
            decision.gateOpen = false;
            if (newSample && valid && !low && !state.nativePressureActive) {
                if (!state.pendingClearSince) { state.pendingClearSince = sample.sampledAt; }
            } else if (newSample) { state.pendingClearSince.reset(); }
            if (newSample && Elapsed(state.pendingClearSince, sample.sampledAt, pendingConfirmation)) {
                state.pressurePending = false;
                state.pendingSince.reset();
                state.lowSince.reset();
                state.pendingClearSince.reset();
                decision.gateOpen = true;
                decision.reason = ResourceDecisionReason::PressureCleared;
                ResetObservation(state, decision);
            } else if ((newSample && low && Elapsed(state.lowSince, sample.sampledAt, pendingConfirmation)) ||
                       Elapsed(state.pendingSince, now, pendingMaximum)) {
                confirmPressure(false);
            }
        }
    }

    if (!valid) {
        if (wasValid || newRequest) { ClearContinuousSamples(state, decision); }
        if (wasValid) {
            state.signalRecoveryPending = true;
            decision.limits.computeLimit = 1u;
            decision.limits.slotLimit = 1u;
            decision.gateOpen = false;
            if (state.phase == ResourceControlPhase::Normal) { state.phase = ResourceControlPhase::Drain; }
            decision.reason = ResourceDecisionReason::SignalLost;
        }
    }
    const bool drained = flow.admittedBlocks == 0u && flow.activeComputeUnits == 0u &&
        !flow.heavyPhaseAdmitted;
    if (state.phase == ResourceControlPhase::Drain && drained) {
        if (state.pressureConfirmed || state.pressurePending) {
            if (flow.pendingNecessaryWork) {
                state.phase = ResourceControlPhase::Hold;
                if (!state.holdSince) { state.holdSince = now; }
                state.highSince.reset();
                decision.reason = ResourceDecisionReason::Drained;
            }
        } else {
            state.phase = ResourceControlPhase::Normal;
            decision.gateOpen = true;
            decision.reason = ResourceDecisionReason::Drained;
            ResetObservation(state, decision);
        }
    }
    if (state.phase == ResourceControlPhase::Hold) {
        decision.gateOpen = false;
        if (newRequest && !state.holdSince && flow.pendingNecessaryWork) { state.holdSince = now; }
        if (newSample && high && Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation) &&
            state.holdSince && state.highSince && *state.highSince >= *state.holdSince) {
            state.phase = ResourceControlPhase::Normal;
            state.pressureConfirmed = false;
            state.pressurePending = false;
            state.severePressure = false;
            state.holdSince.reset();
            state.pendingSince.reset();
            state.lastRecoveryAt = now;
            state.cooldownUntil = now + state.cooldown;
            state.lastGrowthAt = now;
            decision.gateOpen = true;
            decision.reason = ResourceDecisionReason::PressureCleared;
            ResetObservation(state, decision);
            return decision;
        }
        if (flow.pendingNecessaryWork && Elapsed(state.holdSince, now, holdTimeout)) {
            decision.failForSustainedPressure = true;
            decision.reason = ResourceDecisionReason::PressureTimeout;
        }
        return decision;
    }
    if (state.phase != ResourceControlPhase::Normal || state.pressurePending || state.pressureConfirmed) {
        return decision;
    }

    if (state.workType != flow.workType) {
        if (!drained) {
            ResetObservation(state, decision);
            return decision;
        }
        state.workType = flow.workType;
        // 健康环境采用有限初始并发，压力与信号恢复仍遵守收缩和冷却
        const bool healthyStartup = high && !state.everConfirmedPressure &&
            !state.signalRecoveryPending && now >= state.cooldownUntil;
        decision.limits.computeLimit = healthyStartup ? std::min(p, healthyStartupComputeLimit) : 1u;
        decision.limits.slotLimit = healthyStartup ? decision.limits.computeLimit + 1u :
            std::min<std::size_t>(decision.limits.slotLimit, state.threaded && high ? 2u : 1u);
        decision.reason = ResourceDecisionReason::WorkTypeChanged;
        ResetObservation(state, decision);
    }
    if (!valid || !high) {
        if (state.firstObservationSampleAt || state.observationSampleCount != 0u) {
            ResetObservation(state, decision);
        }
        return decision;
    }
    if (state.signalRecoveryPending) {
        if (newSample && Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation)) {
            state.signalRecoveryPending = false;
            state.cooldownUntil = now + initialCooldown;
            decision.reason = ResourceDecisionReason::SignalRestored;
            ResetObservation(state, decision);
        }
        return decision;
    }
    if (state.lastPressureAt && now - *state.lastPressureAt >= repeatPressureWindow) {
        state.cooldown = initialCooldown;
    }
    if (state.optionalRetentionPausedByPressure && now >= state.cooldownUntil &&
        newSample && Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation)) {
        state.optionalRetentionPausedByPressure = false;
        decision.optionalRetentionPausedByPressure = false;
        decision.reason = ResourceDecisionReason::RetentionRestored;
        ResetObservation(state, decision);
        return decision;
    }
    if (!decision.gateOpen || decision.resetObservation || capabilityChanged || !newSample) { return decision; }

    const auto& observation = flow.observation;
    if (state.observationEpoch != observation.epoch) {
        state.observationEpoch = observation.epoch;
        state.firstObservationSampleAt.reset();
        state.observationSampleCount = 0u;
    }
    if (sample.sampledAt >= observation.startedAt) {
        if (!state.firstObservationSampleAt) {
            state.firstObservationSampleAt = sample.sampledAt;
            state.firstObservationAvailableBytes = *available;
        }
        if (state.observationSampleCount != std::numeric_limits<std::size_t>::max()) {
            ++state.observationSampleCount;
        }
    }
    const bool batchComplete = observation.required != 0u &&
        observation.admitted >= observation.required && observation.retiredAt &&
        sample.sampledAt > *observation.retiredAt;
    const bool observingBlocks = observation.admitted != 0u || flow.moreIndependentBlocks;
    const bool cooldownComplete = now >= state.cooldownUntil;
    if (flow.pendingNecessaryWork && cooldownComplete &&
        Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation) &&
        now - state.lastStorageGrowthAt >= storageGrowthInterval &&
        (!observingBlocks || batchComplete) &&
        decision.limits.ownedStorageLimitBytes < memoryBound) {
        constexpr std::uint64_t mib = 1024u * 1024u;
        const auto delta = std::min({memoryBound - decision.limits.ownedStorageLimitBytes,
            std::max(mib, state.storageCeilingBytes / 16u),
            (*available - std::min(*available, marks.high)) / 2u});
        if (delta != 0u) {
            decision.limits.ownedStorageLimitBytes += delta;
            state.lastStorageGrowthAt = now;
            decision.reason = ResourceDecisionReason::StorageGrowth;
            ResetObservation(state, decision);
            return decision;
        }
    }
    if (!batchComplete || !state.firstObservationSampleAt ||
        sample.sampledAt - *state.firstObservationSampleAt < initialGrowthInterval ||
        now - observation.startedAt < initialGrowthInterval || state.observationSampleCount < 3u) {
        return decision;
    }
    const auto duration = now - observation.startedAt;
    const auto interval = state.everConfirmedPressure
        ? ResourceClock::duration(recoveryGrowthInterval) : ResourceClock::duration(initialGrowthInterval);
    const bool fallingQuickly = state.firstObservationAvailableBytes > *available &&
        state.firstObservationAvailableBytes - *available > (marks.high - marks.low) / 4u;
    const bool mayGrow = flow.moreIndependentBlocks && !flow.singleRecordFlow && !flow.heavyPhaseAdmitted &&
        state.threaded && cooldownComplete && observation.startedAt >= state.cooldownUntil &&
        now - state.lastGrowthAt >= interval && !fallingQuickly &&
        !FractionAtLeast(observation.waits.output, duration, 25u);
    if (mayGrow) {
        auto& c = decision.limits.computeLimit;
        auto& s = decision.limits.slotLimit;
        if (c < p && FractionAtLeast(observation.waits.compute, duration, 25u)) {
            const auto step = state.everConfirmedPressure ? 1u :
                std::min<std::size_t>(4u, std::max<std::size_t>(1u, c / 4u));
            c += std::min(p - c, static_cast<std::size_t>(step));
            s = std::min(slotMaximum, std::max(s, c + 1u));
            decision.reason = ResourceDecisionReason::ComputeGrowth;
        } else if (s < std::min(slotMaximum, c + q) &&
                   FractionAtLeast(observation.waits.slots, duration, 25u) &&
                   (FractionAtLeast(observation.waits.input, duration, 10u) || s == c)) {
            ++s;
            decision.reason = ResourceDecisionReason::SlotGrowth;
        }
        if (decision.reason == ResourceDecisionReason::ComputeGrowth ||
            decision.reason == ResourceDecisionReason::SlotGrowth) { state.lastGrowthAt = now; }
    }
    ResetObservation(state, decision);
    return decision;
}

ResourceWatermarks MakeResourceWatermarks(std::uint64_t totalBytes) noexcept {
    constexpr std::uint64_t mib = 1024u * 1024u;
    const auto low = std::min(totalBytes / 4u, std::clamp(totalBytes / 16u, 128u * mib, 4096u * mib));
    return {low, low + low / 2u, low / 2u};
}

ResolvedResourceConfiguration ResolveResourceConfiguration(const CodecResourceParams& params, const ResourceSample& sample) {
    constexpr std::uint64_t mib = 1024u * 1024u;
    if (params.mode != CodecResourceMode::Fixed && params.mode != CodecResourceMode::Adaptive) {
        throw std::invalid_argument("unknown DataCodec resource mode");
    }
    ResolvedResourceConfiguration result;
    result.mode = params.mode;
    result.initialSample = sample;
    result.threaded = sample.threaded;
    result.externalSpillAvailable = sample.externalSpillAvailable;
    const auto allowed = ResourceComputeCapacity(sample, params.mode);
    result.computeCeiling = params.maxComputeThreads.value_or(allowed);
    if (result.computeCeiling == 0u || result.computeCeiling > allowed) {
        throw std::invalid_argument("compute thread limit exceeds allowed CPU capacity");
    }
    if (!result.threaded && result.computeCeiling != 1u) {
        throw std::invalid_argument("this runtime only supports one compute thread");
    }
    const auto total = sample.physicalTotalBytes ? std::optional<std::uint64_t>(std::min(*sample.physicalTotalBytes,
        sample.hardLimitBytes.value_or(std::numeric_limits<std::uint64_t>::max()))) : std::nullopt;
    const auto available = sample.availableBytes ? std::optional<std::uint64_t>(std::min(*sample.availableBytes,
        sample.hardRemainingBytes.value_or(std::numeric_limits<std::uint64_t>::max()))) : std::nullopt;
    result.storageCeilingBytes = params.ownedStorageLimitBytes.value_or(total ? *total / 4u :
        std::min(64u * mib, sample.hardLimitBytes.value_or(64u * mib)));
    if (sample.hardLimitBytes && result.storageCeilingBytes > *sample.hardLimitBytes) {
        throw std::invalid_argument("owned storage limit exceeds runtime capacity");
    }
    const auto marks = total ? MakeResourceWatermarks(*total) : ResourceWatermarks{};
    const auto memoryInitial = total && available
        ? std::min(result.storageCeilingBytes, (*available - std::min(*available, marks.high)) / 2u)
        : std::min(result.storageCeilingBytes, 64u * mib);
    if (params.mode == CodecResourceMode::Fixed) {
        result.initialLimits = {params.ownedStorageLimitBytes.value_or(memoryInitial), result.computeCeiling,
            result.threaded ? result.computeCeiling + 1u : 1u};
    } else {
        const bool pressure = sample.pressure == PressureLevel::Low || sample.pressure == PressureLevel::Critical;
        result.gateOpen = !pressure && (!available || !total || *available >= marks.low);
        const bool healthy = result.threaded && total && *total != 0u && available &&
            *available >= marks.high && !pressure;
        const auto compute = healthy ? std::min(result.computeCeiling, resource_control::healthyStartupComputeLimit) : 1u;
        result.initialLimits = {memoryInitial, compute, healthy ? compute + 1u : 1u};
        if (!result.threaded) { result.gateOpen = true; }
    }
    return result;
}

std::size_t ResourceComputeCapacity(const ResourceSample& sample, CodecResourceMode mode) noexcept {
    if (!sample.threaded) { return 1u; }
    auto allowed = sample.allowedComputeThreads.value_or(1u);
    if (sample.runtimeThreadLimit) {
        // 运行时线程许可先扣除宿主余量、后台 driver 和当前模式的控制线程
        const auto overhead = sample.reservedHostThreads + 1u +
            (mode == CodecResourceMode::Adaptive ? 1u : 0u);
        const auto workers = *sample.runtimeThreadLimit > overhead ? *sample.runtimeThreadLimit - overhead : 0u;
        allowed = std::min(allowed, workers);
    }
    return allowed;
}

}
