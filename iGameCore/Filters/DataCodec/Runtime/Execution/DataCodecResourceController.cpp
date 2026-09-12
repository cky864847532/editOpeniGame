#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace datacodec {
namespace {
constexpr std::uint64_t MiB = 1024u * 1024u;
std::uint64_t Sub(std::uint64_t a, std::uint64_t b) noexcept { return a - std::min(a, b); }
bool Elapsed(const std::optional<ResourceClock::time_point>& at, ResourceClock::time_point now,
    ResourceClock::duration interval) noexcept { return at && now >= *at && now - *at >= interval; }
std::optional<std::uint64_t> Remaining(const ResourceSample& s) noexcept {
    std::optional<std::uint64_t> result;
    for (auto value : {s.hardRemainingBytes, s.commitRemainingBytes, s.processRemainingBytes,
            s.jobRemainingBytes, s.addressSpaceRemainingBytes, s.cgroupRemainingBytes}) {
        if (value) { result = result ? std::min(*result, *value) : value; }
    }
    return result;
}
std::uint64_t Absolute(const ResourceSample& s) noexcept {
    auto value = std::min(s.physicalTotalBytes.value_or(0u),
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    for (auto bound : {s.hardLimitBytes, s.processLimitBytes, s.jobLimitBytes, s.addressSpaceLimitBytes, s.cgroupLimitBytes}) {
        if (bound) { value = std::min(value, *bound); }
    }
    return value;
}
std::uint64_t Scale(std::uint64_t bytes, double factor) noexcept {
    return static_cast<std::uint64_t>(std::min(static_cast<long double>(bytes),
        std::floor(static_cast<long double>(bytes) * factor)));
}
void ResetGainWindow(ResourceControllerState& state, ResourceClock::time_point now) noexcept {
    state.gainWindowStarted = now;
    state.availableSum = 0.0;
    state.availableSamples = 0u;
}
void ObserveGain(ResourceControllerState& state, ResourceClock::time_point now, bool newSample) noexcept {
    auto& m = state.memory;
    if (state.initialMeasurement && m.calibration == MemoryCalibrationResult::Measuring &&
        now - m.calibrationStarted >= resource_control::calibrationDeadline) {
        m.calibration = MemoryCalibrationResult::TimedOut;
        m.calibrationElapsed = now - m.calibrationStarted;
        m.calibrationEndReason = ResourceDecisionReason::WaitDeadlineExceeded;
        state.gainPhase = 0u;
        state.initialMeasurement = false;
    }
    if (!newSample || (state.gainPhase != 1u && state.gainPhase != 3u)) { return; }
    state.availableSum += static_cast<double>(*m.availableBytes);
    ++state.availableSamples;
    if (now - state.gainWindowStarted < resource_control::gainWindow) { return; }
    const double average = state.availableSum / static_cast<double>(state.availableSamples);
    if (state.gainPhase == 1u) {
        m.baselineAvailable = average;
        state.measurementBytes = 0u;
        state.gainPhase = 2u;
        return;
    }
    m.responseAvailable = average;
    m.measuredReservationBytes = state.measurementBytes;
    const double response = state.measurementBytes != 0u
        ? (*m.baselineAvailable - average) / static_cast<double>(state.measurementBytes) : 0.0;
    const bool usable = std::isfinite(response) && response >= 1e-6;
    if (usable) {
        m.response = response;
        m.gain = std::clamp(resource_control::responseAlpha / response,
            resource_control::minimumGain, resource_control::maximumGain);
        ++m.gainUpdates;
    }
    if (state.initialMeasurement) {
        m.calibration = usable ? MemoryCalibrationResult::Estimated : MemoryCalibrationResult::Unusable;
        m.calibrationElapsed = now - m.calibrationStarted;
        m.calibrationEndReason = usable ? ResourceDecisionReason::MemoryGainUpdated : ResourceDecisionReason::MeasurementUnusable;
    }
    state.gainPhase = 0u;
    state.initialMeasurement = false;
}
}

const char* MemoryCalibrationResultName(MemoryCalibrationResult result) noexcept {
    switch (result) {
    case MemoryCalibrationResult::Pending: return "Pending";
    case MemoryCalibrationResult::Measuring: return "Measuring";
    case MemoryCalibrationResult::Estimated: return "Estimated";
    case MemoryCalibrationResult::Unusable: return "Unusable";
    case MemoryCalibrationResult::TimedOut: return "TimedOut";
    }
    return "Unknown";
}
const char* MemoryDecisionReasonName(ResourceDecisionReason reason) noexcept {
    switch (reason) {
    case ResourceDecisionReason::ReserveTargetLow: return "ReserveTargetLow";
    case ResourceDecisionReason::NativeMemoryPressure: return "NativeMemoryPressure";
    case ResourceDecisionReason::RuntimeHeadroomLow: return "RuntimeHeadroomLow";
    case ResourceDecisionReason::RequiredGrowth: return "RequiredGrowth";
    case ResourceDecisionReason::RequiredContinuation: return "RequiredContinuation";
    case ResourceDecisionReason::SampleUnavailable: return "SampleUnavailable";
    case ResourceDecisionReason::RetainedStoragePreventingProgress: return "RetainedStoragePreventingProgress";
    case ResourceDecisionReason::WaitDeadlineExceeded: return "WaitDeadlineExceeded";
    case ResourceDecisionReason::CapacityBoundExceeded: return "CapacityBoundExceeded";
    case ResourceDecisionReason::HardCapabilityChanged: return "HardCapabilityChanged";
    case ResourceDecisionReason::PressureCleared: return "PressureCleared";
    case ResourceDecisionReason::RetentionRestored: return "RetentionRestored";
    case ResourceDecisionReason::MemoryGainUpdated: return "MemoryGainUpdated";
    case ResourceDecisionReason::BeginRequest: return "BeginRequest";
    case ResourceDecisionReason::RequestEnded: return "RequestEnded";
    case ResourceDecisionReason::RequestCancelled: return "RequestCancelled";
    case ResourceDecisionReason::MeasurementUnusable: return "MeasurementUnusable";
    default: return "NoChange";
    }
}

bool ValidPhysicalMemorySample(const ResourceSample& sample, ResourceClock::time_point now) noexcept {
    return sample.physicalTotalBytes && *sample.physicalTotalBytes != 0u && sample.availableBytes &&
        *sample.availableBytes <= *sample.physicalTotalBytes && sample.sampledAt <= now &&
        now - sample.sampledAt <= resource_control::maximumSampleAge;
}

ResourceWatermarks MakeReserveWatermarks(std::uint64_t total, double ratio) noexcept {
    const auto reserve = static_cast<std::uint64_t>(std::min(static_cast<long double>(total),
        std::ceil(static_cast<long double>(total) * ratio)));
    const auto hysteresis = std::min(Sub(total, reserve) / 2u,
        std::clamp(total / 200u, 16u * MiB, 256u * MiB));
    return {reserve, reserve + hysteresis, reserve / 2u};
}

// Fixed 的旧默认值独立保留
ResourceWatermarks MakeResourceWatermarks(std::uint64_t total) noexcept {
    const auto low = std::min(total / 4u, std::clamp(total / 16u, 128u * MiB, 4096u * MiB));
    return {low, low + low / 2u, low / 2u};
}

void InitializeResourceController(ResourceControllerState& state,
    const ResolvedResourceConfiguration& config, ResourceClock::time_point) noexcept {
    state = {};
    state.memory.targetRatio = config.targetAvailableMemoryRatio;
    state.memory.absoluteCapacityBytes = config.storageCeilingBytes.value_or(0u);
    state.normalSlotLimit = config.threaded ? config.computeCeiling + 1u : 1u;
}

void FinishMemoryMeasurement(ResourceControllerState& state, ResourceClock::time_point now,
    ResourceDecisionReason reason) noexcept {
    if (state.gainPhase >= 2u) { state.memory.measuredReservationBytes = state.measurementBytes; }
    if (state.initialMeasurement) {
        state.memory.calibration = MemoryCalibrationResult::Unusable;
        state.memory.calibrationElapsed = now - state.memory.calibrationStarted;
    }
    if (state.gainPhase != 0u) { state.memory.calibrationEndReason = reason; }
    state.gainPhase = 0u;
    state.initialMeasurement = false;
}

void NoteMemoryReservation(ResourceControllerState& state, std::uint64_t bytes,
    std::uint64_t demandId, ResourceClock::time_point now) noexcept {
    if (state.gainPhase >= 2u) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - state.measurementBytes) {
            FinishMemoryMeasurement(state, now, ResourceDecisionReason::CapacityBoundExceeded);
        } else { state.measurementBytes += bytes; }
    }
    if (state.grant.demandId != 0u && state.grant.demandId == demandId && !state.grant.used) {
        state.grant.used = true;
        if (state.gainPhase == 2u && (state.grant.probe || !state.initialMeasurement)) {
            state.gainPhase = 3u;
            ResetGainWindow(state, now);
        }
    }
}

ControlDecision Advance(ResourceControllerState& state, ResourceClock::time_point now,
    const ResourceSample& sample, const FlowSnapshot& flow) noexcept {
    using namespace resource_control;
    ControlDecision decision;
    decision.limits = flow.limits;
    decision.gateOpen = flow.gateOpen;
    decision.optionalRetentionPausedByPressure = state.optionalRetentionPausedByPressure;
    if (flow.stopped || !flow.runActive || !flow.limits.ownedStorageLimitBytes) { return decision; }
    const bool newRequest = state.requestId != flow.requestId;
    if (newRequest) {
        const auto ratio = state.memory.targetRatio;
        const auto slots = state.normalSlotLimit;
        const auto bound = state.memory.absoluteCapacityBytes;
        state = {};
        state.memory.targetRatio = ratio;
        state.memory.absoluteCapacityBytes = bound;
        state.normalSlotLimit = slots;
        state.requestId = flow.requestId;
        decision.reason = ResourceDecisionReason::BeginRequest;
    }
    auto& m = state.memory;
    const bool valid = ValidPhysicalMemorySample(sample, now);
    const bool newSample = valid && (!state.lastSampleAt || sample.sampledAt > *state.lastSampleAt);
    const bool gap = newSample && state.lastSampleAt && sample.sampledAt - *state.lastSampleAt > maximumSampleAge;
    if (gap) { state.previousAvailableBytes.reset(); state.highSince.reset(); state.lowSince.reset(); }
    state.signalValid = valid;
    const auto previousTotal = m.physicalTotalBytes;
    const auto previousBound = m.absoluteCapacityBytes;
    if (valid) {
        m.physicalTotalBytes = sample.physicalTotalBytes;
        m.availableBytes = sample.availableBytes;
        m.environmentRemainingBytes = Remaining(sample);
        m.absoluteCapacityBytes = Absolute(sample);
        const auto marks = MakeReserveWatermarks(*sample.physicalTotalBytes, m.targetRatio);
        m.reserveBytes = marks.low;
        m.recoveryBytes = marks.high;
        if (newSample) {
            state.recentAvailableAverage = static_cast<double>(*sample.availableBytes);
            if (state.previousAvailableBytes && state.lastSampleAt && sample.sampledAt - *state.lastSampleAt <= gainWindow) {
                state.recentAvailableAverage = (state.recentAvailableAverage + static_cast<double>(*state.previousAvailableBytes)) / 2.0;
            }
            state.growthAvailableBytes = std::min(*sample.availableBytes,
                state.previousAvailableBytes.value_or(*sample.availableBytes));
            state.previousAvailableBytes = sample.availableBytes;
            state.lastSampleAt = sample.sampledAt;
        }
        m.growthHeadroomBytes = std::min({Sub(state.growthAvailableBytes, m.recoveryBytes),
            m.environmentRemainingBytes.value_or(std::numeric_limits<std::uint64_t>::max()),
            Sub(m.absoluteCapacityBytes, flow.reservedBytes)});
    } else {
        m.availableBytes.reset();
        m.environmentRemainingBytes.reset();
        m.growthHeadroomBytes = 0u;
        state.previousAvailableBytes.reset();
        state.highSince.reset();
    }
    const bool capacityChanged = previousTotal && (previousTotal != m.physicalTotalBytes || previousBound != m.absoluteCapacityBytes);
    if (capacityChanged) {
        FinishMemoryMeasurement(state, now, ResourceDecisionReason::HardCapabilityChanged);
        decision.reason = ResourceDecisionReason::HardCapabilityChanged;
    }
    const bool native = valid && (sample.pressure == PressureLevel::Low || sample.pressure == PressureLevel::Critical);
    const bool low = valid && *sample.availableBytes < m.reserveBytes;
    const bool runtimeLow = valid && (m.environmentRemainingBytes == 0u || flow.reservedBytes > m.absoluteCapacityBytes);
    const bool pressure = !valid || native || low || runtimeLow;
    const bool high = valid && *sample.availableBytes >= m.recoveryBytes && !native && !runtimeLow;
    if (newSample) {
        if (high) { if (!state.highSince) { state.highSince = sample.sampledAt; } }
        else { state.highSince.reset(); }
        if (low) { if (!state.lowSince) { state.lowSince = sample.sampledAt; } }
        else { state.lowSince.reset(); }
    }
    const bool safeBoundary = flow.activeComputeUnits == 0u && flow.queuedTasks == 0u && flow.admittedBlocks == 0u;
    const auto beginWait = [&] { if (!m.waitSince) { m.waitSince = now; } };
    const auto expire = [&] {
        if (Elapsed(m.waitSince, now, holdTimeout)) {
            decision.failForSustainedPressure = true;
            decision.reason = ResourceDecisionReason::WaitDeadlineExceeded;
        }
    };
    if (!state.initialized) {
        state.initialized = true;
        const auto window = std::min(Scale(m.growthHeadroomBytes, m.gain),
            std::max(MiB, m.physicalTotalBytes.value_or(0u) / 16u));
        decision.limits.ownedStorageLimitBytes = flow.reservedBytes > m.absoluteCapacityBytes
            ? m.absoluteCapacityBytes : flow.reservedBytes + (pressure ? 0u : window);
        decision.limits.slotLimit = state.normalSlotLimit;
        decision.gateOpen = !pressure;
    }
    if (valid) {
        const auto allowed = flow.reservedBytes > m.absoluteCapacityBytes ? m.absoluteCapacityBytes :
            flow.reservedBytes + m.growthHeadroomBytes;
        decision.limits.ownedStorageLimitBytes = std::min(*decision.limits.ownedStorageLimitBytes, allowed);
    }
    if (pressure) {
        beginWait();
        decision.reason = !valid ? ResourceDecisionReason::SampleUnavailable : native ? ResourceDecisionReason::NativeMemoryPressure :
            low ? ResourceDecisionReason::ReserveTargetLow : ResourceDecisionReason::RuntimeHeadroomLow;
        FinishMemoryMeasurement(state, now, decision.reason);
        state.phase = safeBoundary ? ResourceControlPhase::Hold : ResourceControlPhase::Drain;
        decision.gateOpen = false;
        decision.limits.ownedStorageLimitBytes = std::min(flow.reservedBytes, m.absoluteCapacityBytes);
        const bool severe = valid && (sample.pressure == PressureLevel::Critical || *sample.availableBytes < m.reserveBytes / 2u);
        const bool confirmed = native || severe || (newSample && low && Elapsed(state.lowSince, now, pendingConfirmation));
        if (confirmed && (!state.pressureConfirmed || (severe && !state.severePressure))) {
            decision.trimOptionalRetention = true;
            state.optionalRetentionPausedByPressure = true;
            state.pressureConfirmed = true;
        }
        state.severePressure |= severe;
        state.pressurePending = low && !state.pressureConfirmed;
    } else if (state.phase == ResourceControlPhase::Drain || state.phase == ResourceControlPhase::Hold) {
        decision.gateOpen = false;
        if (safeBoundary) { state.phase = ResourceControlPhase::Hold; }
        if (newSample && high && Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation)) {
            state.phase = ResourceControlPhase::Recover;
            state.recoveryStartedAt = now;
            state.cooldownUntil = now + recoveryCooldown;
            decision.gateOpen = true;
            decision.limits.slotLimit = 1u;
            decision.reason = ResourceDecisionReason::PressureCleared;
        }
    } else { decision.gateOpen = true; }
    if (state.phase == ResourceControlPhase::Recover) {
        decision.limits.slotLimit = 1u;
        if (state.progressAt && state.recoveryStartedAt && *state.progressAt >= *state.recoveryStartedAt) {
            m.waitSince.reset();
            if (newSample && high && sample.sampledAt > *state.progressAt && now >= state.cooldownUntil) {
                state.phase = ResourceControlPhase::Normal;
                state.pressureConfirmed = state.pressurePending = state.severePressure = false;
                state.optionalRetentionPausedByPressure = false;
                decision.limits.slotLimit = state.normalSlotLimit;
                decision.reason = ResourceDecisionReason::RetentionRestored;
            }
        }
    }
    decision.optionalRetentionPausedByPressure = state.optionalRetentionPausedByPressure;
    if (valid && safeBoundary && (flow.reservedBytes > m.absoluteCapacityBytes ||
        (flow.byteWaiting && flow.nextWorkBytes > Sub(m.absoluteCapacityBytes, flow.reservedBytes)))) {
        decision.failForCapacityBound = true;
        decision.reason = ResourceDecisionReason::CapacityBoundExceeded;
    }
    if (!decision.gateOpen || decision.failForCapacityBound) {
        expire();
        m.reason = decision.reason;
        return decision;
    }
    // 未消费的授权被收回时取消该授权，新的授权仍需新鲜样本
    if (state.grant.demandId && !state.grant.used && flow.byteWaiting &&
        *decision.limits.ownedStorageLimitBytes < flow.reservedBytes + std::min(flow.nextWorkBytes, Sub(m.absoluteCapacityBytes, flow.reservedBytes))) {
        state.grant.used = true;
        state.grant.completedAt = now;
    }
    const auto gainBefore = m.gainUpdates;
    ObserveGain(state, now, newSample);
    if (gainBefore != m.gainUpdates) { decision.reason = ResourceDecisionReason::MemoryGainUpdated; }
    const auto unused = Sub(*decision.limits.ownedStorageLimitBytes, flow.reservedBytes);
    if (flow.byteWaiting && flow.nextWorkBytes > unused) {
        beginWait();
        if (state.phase == ResourceControlPhase::Normal && m.calibration == MemoryCalibrationResult::Pending) {
            m.calibration = MemoryCalibrationResult::Measuring;
            m.calibrationStarted = now;
            state.initialMeasurement = true;
            state.gainPhase = 1u;
            ResetGainWindow(state, now);
        }
        const bool continuation = flow.demandKind == MemoryDemandKind::RequiredContinuation;
        bool grantReady = state.grant.demandId == 0u;
        if (continuation) {
            grantReady = flow.activeComputeUnits == 0u && flow.queuedTasks == 0u &&
                (!m.lastPreparationAt || sample.sampledAt > *m.lastPreparationAt) &&
                (state.grant.demandId == 0u || state.grant.used);
        } else if (state.grant.used && state.grant.completedAt) {
            grantReady = sample.sampledAt > *state.grant.completedAt &&
                now - state.grant.grantedAt >= initialGrowthInterval;
        }
        if (grantReady && flow.nextWorkBytes <= m.growthHeadroomBytes) {
            auto preview = std::min(Scale(m.growthHeadroomBytes, m.gain), std::max(MiB, *m.physicalTotalBytes / 16u));
            const bool probe = !continuation && state.initialMeasurement && state.gainPhase == 2u;
            if (probe) { preview = Scale(preview, probeScale); }
            const auto window = continuation || state.phase == ResourceControlPhase::Recover ? flow.nextWorkBytes :
                std::min(m.growthHeadroomBytes, std::max(flow.nextWorkBytes, preview));
            decision.limits.ownedStorageLimitBytes = flow.reservedBytes + window;
            state.grant = {};
            state.grant.demandId = flow.demandId;
            state.grant.baselineReservedBytes = flow.reservedBytes;
            state.grant.sampledAt = sample.sampledAt;
            state.grant.grantedAt = now;
            state.grant.probe = probe;
            ++m.grantEpoch;
            if (!continuation && !state.initialMeasurement && state.gainPhase == 0u &&
                m.calibration != MemoryCalibrationResult::Pending && now - sample.sampledAt <= gainWindow) {
                m.baselineAvailable = state.recentAvailableAverage;
                state.measurementBytes = 0u;
                state.gainPhase = 2u;
            }
            decision.reason = continuation ? ResourceDecisionReason::RequiredContinuation : ResourceDecisionReason::RequiredGrowth;
        } else if (flow.nextWorkBytes > m.growthHeadroomBytes) {
            decision.reason = state.optionalRetentionPausedByPressure && safeBoundary ?
                ResourceDecisionReason::RetainedStoragePreventingProgress : ResourceDecisionReason::RuntimeHeadroomLow;
        }
    }
    expire();
    if (decision.reason != ResourceDecisionReason::NoChange) { m.reason = decision.reason; }
    return decision;
}

ResolvedResourceConfiguration ResolveResourceConfiguration(const CodecResourceParams& params, const ResourceSample& sample) {
    constexpr std::uint64_t mib = 1024u * 1024u;
    if (params.mode != CodecResourceMode::Fixed && params.mode != CodecResourceMode::Adaptive &&
        params.mode != CodecResourceMode::Unlimited) {
        throw std::invalid_argument("unknown DataCodec resource mode");
    }
    ResolvedResourceConfiguration result;
    if (params.threadMode != CodecThreadMode::Fixed && params.threadMode != CodecThreadMode::Adaptive) {
        throw std::invalid_argument("unknown DataCodec thread mode");
    }
    if (params.threadMode == CodecThreadMode::Adaptive) {
        if (!sample.threaded || !CpuUsageProbe::Supported()) {
            throw std::invalid_argument("adaptive CPU control is unavailable on this runtime");
        }
        if (params.maxComputeThreads || !params.targetCpuIdleRatio ||
            !std::isfinite(*params.targetCpuIdleRatio) || *params.targetCpuIdleRatio < 0.0 ||
            *params.targetCpuIdleRatio >= 1.0) {
            throw std::invalid_argument("adaptive threads require an idle ratio in [0,1) and no fixed thread limit");
        }
    } else if (params.targetCpuIdleRatio) {
        throw std::invalid_argument("fixed threads require an unspecified CPU idle target");
    }
    result.threadMode = params.threadMode;
    result.targetCpuIdleRatio = params.targetCpuIdleRatio.value_or(0.0);
    result.mode = params.mode;
    result.initialSample = sample;
    result.threaded = sample.threaded;
    result.externalSpillAvailable = sample.externalSpillAvailable;
    const auto allowed = ResourceComputeCapacity(sample, params.mode, params.threadMode);
    result.computeCeiling = params.maxComputeThreads.value_or(allowed);
    if (result.computeCeiling == 0u || result.computeCeiling > allowed) {
        throw std::invalid_argument("compute thread limit exceeds allowed CPU capacity");
    }
    if (!result.threaded && result.computeCeiling != 1u) {
        throw std::invalid_argument("this runtime only supports one compute thread");
    }
    if (params.mode != CodecResourceMode::Adaptive && params.targetAvailableMemoryRatio) {
        throw std::invalid_argument("memory reserve ratio requires Adaptive mode");
    }
    if (params.mode == CodecResourceMode::Adaptive) {
        if (params.ownedStorageLimitBytes) { throw std::invalid_argument("Adaptive memory requires an unspecified byte limit"); }
        result.targetAvailableMemoryRatio = params.targetAvailableMemoryRatio.value_or(resource_control::defaultReserveRatio);
        if (!std::isfinite(result.targetAvailableMemoryRatio) || result.targetAvailableMemoryRatio < 0.0 || result.targetAvailableMemoryRatio >= 1.0) {
            throw std::invalid_argument("memory reserve ratio must be finite and in [0,1)");
        }
        if (!ValidPhysicalMemorySample(sample, ResourceClock::now())) {
            throw std::invalid_argument("memory-observation-unavailable");
        }
        result.storageCeilingBytes = Absolute(sample);
        ResourceControllerState control;
        InitializeResourceController(control, result, sample.sampledAt);
        FlowSnapshot flow;
        flow.limits = {0u, result.computeCeiling, result.threaded ? result.computeCeiling + 1u : 1u};
        flow.runActive = true;
        flow.requestId = 1u;
        const auto decision = Advance(control, sample.sampledAt, sample, flow);
        result.initialLimits = decision.limits;
        result.gateOpen = decision.gateOpen;
        return result;
    }
    if (params.mode == CodecResourceMode::Unlimited) {
        if (params.ownedStorageLimitBytes) {
            throw std::invalid_argument("Unlimited mode requires an unspecified owned storage limit");
        }
        result.storageCeilingBytes.reset();
        result.initialLimits = {std::nullopt, result.computeCeiling,
            result.threaded ? result.computeCeiling + 1u : 1u};
        return result;
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
        ? std::min(*result.storageCeilingBytes, (*available - std::min(*available, marks.high)) / 2u)
        : std::min(*result.storageCeilingBytes, 64u * mib);
    result.initialLimits = {params.ownedStorageLimitBytes.value_or(memoryInitial), result.computeCeiling,
        result.threaded ? result.computeCeiling + 1u : 1u};
    return result;
}

std::size_t ResourceComputeCapacity(const ResourceSample& sample, CodecResourceMode mode, CodecThreadMode threadMode) noexcept {
    if (!sample.threaded) { return 1u; }
    auto allowed = sample.allowedComputeThreads.value_or(1u);
    if (sample.runtimeThreadLimit) {
        // 运行时线程许可先扣除宿主余量、后台 driver 和当前模式的控制线程
        const auto overhead = sample.reservedHostThreads + 1u +
            (mode == CodecResourceMode::Adaptive || threadMode == CodecThreadMode::Adaptive ? 1u : 0u);
        const auto workers = *sample.runtimeThreadLimit > overhead ? *sample.runtimeThreadLimit - overhead : 0u;
        allowed = std::min(allowed, workers);
    }
    return allowed;
}

}
