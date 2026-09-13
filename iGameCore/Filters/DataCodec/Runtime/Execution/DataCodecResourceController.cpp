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
    case ResourceDecisionReason::BeginRequest: return "BeginRequest";
    case ResourceDecisionReason::RequestEnded: return "RequestEnded";
    case ResourceDecisionReason::RequestCancelled: return "RequestCancelled";
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

void NoteMemoryReservation(ResourceControllerState& state, std::uint64_t,
    std::uint64_t demandId, ResourceClock::time_point) noexcept {
    if (state.grant.demandId != 0u && state.grant.demandId == demandId && !state.grant.used) {
        state.grant.used = true;
    }
}

ControlDecision Advance(ResourceControllerState& state, ResourceClock::time_point now,
    const ResourceSample& sample, const FlowSnapshot& flow) noexcept {
    using namespace resource_control;
    ControlDecision d;
    d.limits = flow.limits;
    d.gateOpen = flow.gateOpen;
    d.optionalRetentionPausedByPressure = state.optionalRetentionPausedByPressure;
    if (flow.stopped || !flow.runActive || !flow.limits.ownedStorageLimitBytes) { return d; }
    if (state.requestId != flow.requestId) {
        const auto ratio = state.memory.targetRatio;
        const auto slots = state.normalSlotLimit;
        const auto bound = state.memory.absoluteCapacityBytes;
        state = {};
        state.memory.targetRatio = ratio;
        state.memory.absoluteCapacityBytes = bound;
        state.normalSlotLimit = slots;
        state.requestId = flow.requestId;
        d.reason = ResourceDecisionReason::BeginRequest;
    }
    auto& m = state.memory;
    const bool valid = ValidPhysicalMemorySample(sample, now);
    const bool fresh = valid && (!state.lastSampleAt || sample.sampledAt > *state.lastSampleAt);
    const auto previousBound = m.absoluteCapacityBytes;
    state.signalValid = valid;
    if (valid) {
        m.physicalTotalBytes = sample.physicalTotalBytes;
        m.availableBytes = sample.availableBytes;
        m.environmentRemainingBytes = Remaining(sample);
        m.absoluteCapacityBytes = Absolute(sample);
        const auto marks = MakeReserveWatermarks(*sample.physicalTotalBytes, m.targetRatio);
        m.reserveBytes = marks.low;
        m.recoveryBytes = marks.high;
        // 必要增长扣除保留目标，恢复余量只约束提前开放的容量和并发
        const auto capacity = std::min(m.environmentRemainingBytes.value_or(std::numeric_limits<std::uint64_t>::max()),
            Sub(m.absoluteCapacityBytes, flow.reservedBytes));
        m.growthHeadroomBytes = std::min(Sub(*sample.availableBytes, m.reserveBytes), capacity);
        m.previewHeadroomBytes = std::min(Sub(*sample.availableBytes, m.recoveryBytes), capacity);
        if (fresh) {
            if (state.lastSampleAt && sample.sampledAt - *state.lastSampleAt > maximumSampleAge) {
                state.highSince.reset(); state.lowSince.reset();
            }
            state.lastSampleAt = sample.sampledAt;
        }
    } else {
        m.availableBytes.reset(); m.environmentRemainingBytes.reset();
        m.growthHeadroomBytes = m.previewHeadroomBytes = 0u;
        state.highSince.reset(); state.lowSince.reset();
    }
    if (m.absoluteCapacityBytes != previousBound) { d.reason = ResourceDecisionReason::HardCapabilityChanged; }
    const bool native = valid && (sample.pressure == PressureLevel::Low || sample.pressure == PressureLevel::Critical);
    const bool low = valid && *sample.availableBytes < m.reserveBytes;
    const bool capacityLow = valid && (m.environmentRemainingBytes == 0u || flow.reservedBytes > m.absoluteCapacityBytes);
    const bool pressure = !valid || native || low || capacityLow;
    const bool high = valid && *sample.availableBytes >= m.recoveryBytes && !native && !capacityLow;
    if (fresh) {
        if (high) { if (!state.highSince) { state.highSince = sample.sampledAt; } }
        else { state.highSince.reset(); }
        if (low) { if (!state.lowSince) { state.lowSince = sample.sampledAt; } }
        else { state.lowSince.reset(); }
    }
    const bool terminalsDrained = flow.activeComputeUnits == 0u && flow.queuedTasks == 0u;
    const bool drained = terminalsDrained && flow.admittedBlocks == 0u;
    const bool continuation = flow.byteWaiting && flow.demandKind == MemoryDemandKind::RequiredContinuation;
    const bool waitBoundary = terminalsDrained && (flow.admittedBlocks == 0u || continuation);
    const auto beginWait = [&] {
        if (waitBoundary && (flow.byteWaiting || flow.pendingNecessaryWork) && !m.waitSince) { m.waitSince = now; }
    };
    // 只有真实退休、提交或大额准备完成才能重置无进展期限
    if (m.waitSince && state.progressAt && *state.progressAt > *m.waitSince) { m.waitSince.reset(); }
    if (!state.initialized) {
        state.initialized = true;
        d.limits.ownedStorageLimitBytes = flow.reservedBytes + std::min(
            Scale(m.previewHeadroomBytes, m.previewRatio), std::max(MiB, m.physicalTotalBytes.value_or(0u) / 16u));
        d.limits.slotLimit = state.normalSlotLimit;
    }
    if (valid) {
        d.limits.ownedStorageLimitBytes = std::min(*d.limits.ownedStorageLimitBytes,
            std::min(flow.reservedBytes, m.absoluteCapacityBytes) + m.growthHeadroomBytes);
    }
    if (pressure) {
        state.phase = drained ? ResourceControlPhase::Hold : ResourceControlPhase::Drain;
        d.gateOpen = false;
        d.limits.ownedStorageLimitBytes = std::min(flow.reservedBytes, m.absoluteCapacityBytes);
        d.reason = !valid ? ResourceDecisionReason::SampleUnavailable : native ? ResourceDecisionReason::NativeMemoryPressure :
            low ? ResourceDecisionReason::ReserveTargetLow : ResourceDecisionReason::RuntimeHeadroomLow;
        const bool severe = valid && (sample.pressure == PressureLevel::Critical || *sample.availableBytes < m.reserveBytes / 2u);
        const bool confirmed = native || severe || (fresh && low && Elapsed(state.lowSince, now, pendingConfirmation));
        if (confirmed && (!state.pressureConfirmed || (severe && !state.severePressure))) {
            d.trimOptionalRetention = true;
            state.optionalRetentionPausedByPressure = true;
            state.pressureConfirmed = true;
        }
        state.severePressure |= severe;
        state.pressurePending = low && !state.pressureConfirmed;
        beginWait();
    } else {
        if (state.phase == ResourceControlPhase::Drain || state.phase == ResourceControlPhase::Hold ||
            (state.phase == ResourceControlPhase::Normal && !high)) {
            state.phase = ResourceControlPhase::Recover;
            state.recoveryStartedAt = now;
            state.cooldownUntil = now + recoveryCooldown;
        }
        if (state.phase == ResourceControlPhase::Recover && fresh && high &&
            Elapsed(state.highSince, sample.sampledAt, recoveryConfirmation) && now >= state.cooldownUntil &&
            (!state.progressAt || sample.sampledAt > *state.progressAt)) {
            state.phase = ResourceControlPhase::Normal;
            state.pressureConfirmed = state.pressurePending = state.severePressure = false;
            state.optionalRetentionPausedByPressure = false;
            d.reason = ResourceDecisionReason::RetentionRestored;
        }
        const bool single = state.phase == ResourceControlPhase::Recover;
        const bool preparedSample = !m.lastPreparationAt || sample.sampledAt > *m.lastPreparationAt;
        const bool progressSample = !state.progressAt || sample.sampledAt > *state.progressAt;
        d.limits.slotLimit = single ? 1u : state.normalSlotLimit;
        d.gateOpen = !single || ((drained || (continuation && terminalsDrained)) && preparedSample && progressSample);
        if (single) {
            d.limits.ownedStorageLimitBytes = flow.reservedBytes;
            // 尚未消费的同一申请保持原授权，不因重复唤醒反复创建额度
            if (d.gateOpen && state.grant.demandId == flow.demandId && !state.grant.used &&
                flow.byteWaiting && flow.nextWorkBytes <= m.growthHeadroomBytes) {
                d.limits.ownedStorageLimitBytes = flow.reservedBytes + flow.nextWorkBytes;
            }
        }
        const auto unused = Sub(*d.limits.ownedStorageLimitBytes, flow.reservedBytes);
        if (flow.byteWaiting && flow.nextWorkBytes > unused) {
            beginWait();
            // 收回未消费授权后，允许按当前需求重新授权，不制造虚假进展
            if (state.grant.demandId && !state.grant.used) { state.grant = {}; }
            bool ready = !state.grant.demandId ||
                (state.grant.used && state.grant.completedAt && sample.sampledAt > *state.grant.completedAt);
            if (continuation) {
                ready = terminalsDrained && preparedSample && (!state.grant.demandId || state.grant.used);
            }
            if (d.gateOpen && ready && flow.nextWorkBytes <= m.growthHeadroomBytes) {
                const auto preview = std::min(Scale(m.previewHeadroomBytes, m.previewRatio),
                    std::max(MiB, *m.physicalTotalBytes / 16u));
                const auto bytes = single || continuation ? flow.nextWorkBytes : std::max(flow.nextWorkBytes, preview);
                d.limits.ownedStorageLimitBytes = flow.reservedBytes + bytes;
                state.grant = {};
                state.grant.demandId = flow.demandId;
                state.grant.baselineReservedBytes = flow.reservedBytes;
                state.grant.sampledAt = sample.sampledAt;
                state.grant.grantedAt = now;
                ++m.grantEpoch;
                d.reason = continuation ? ResourceDecisionReason::RequiredContinuation : ResourceDecisionReason::RequiredGrowth;
            } else if (flow.nextWorkBytes > m.growthHeadroomBytes) {
                d.reason = waitBoundary ? ResourceDecisionReason::RetainedStoragePreventingProgress :
                    ResourceDecisionReason::RuntimeHeadroomLow;
            }
        }
    }
    d.optionalRetentionPausedByPressure = state.optionalRetentionPausedByPressure;
    if (valid && waitBoundary && (flow.reservedBytes > m.absoluteCapacityBytes ||
        (flow.byteWaiting && flow.nextWorkBytes > Sub(m.absoluteCapacityBytes, flow.reservedBytes)))) {
        d.failForCapacityBound = true;
        d.gateOpen = false;
        d.reason = ResourceDecisionReason::CapacityBoundExceeded;
    }
    if (waitBoundary && Elapsed(m.waitSince, now, holdTimeout)) {
        d.failForSustainedPressure = true;
        d.gateOpen = false;
        d.reason = ResourceDecisionReason::WaitDeadlineExceeded;
    }
    if (d.reason != ResourceDecisionReason::NoChange) { m.reason = d.reason; }
    return d;
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
