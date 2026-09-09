#ifndef DATACODEC_RUNTIME_EXECUTION_DATACODECRESOURCECONTROLLER_H
#define DATACODEC_RUNTIME_EXECUTION_DATACODECRESOURCECONTROLLER_H

#include "DataCodec/Platform/ResourceProbe.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

namespace datacodec {

struct ResourceWatermarks {
    std::uint64_t low{0u};
    std::uint64_t high{0u};
    std::uint64_t critical{0u};
};

namespace resource_control {
inline constexpr auto sampleInterval = std::chrono::milliseconds(250);
inline constexpr auto maximumSampleAge = std::chrono::seconds(1);
inline constexpr auto pendingConfirmation = std::chrono::milliseconds(500);
inline constexpr auto pendingMaximum = std::chrono::seconds(2);
inline constexpr auto recoveryConfirmation = std::chrono::seconds(2);
inline constexpr auto initialCooldown = std::chrono::seconds(10);
inline constexpr auto maximumCooldown = std::chrono::seconds(60);
inline constexpr auto repeatPressureWindow = std::chrono::seconds(60);
inline constexpr auto holdTimeout = std::chrono::seconds(30);
inline constexpr auto initialGrowthInterval = std::chrono::milliseconds(500);
inline constexpr auto recoveryGrowthInterval = std::chrono::seconds(5);
inline constexpr auto storageGrowthInterval = std::chrono::seconds(5);
}

struct FlowWaitDurations {
    ResourceClock::duration compute{};
    ResourceClock::duration slots{};
    ResourceClock::duration input{};
    ResourceClock::duration output{};
};

// 观察对象是目标发布后接纳的首批槽位，退休时间来自真实的归还事件
struct ObservationBatch {
    std::uint64_t epoch{0u};
    ResourceClock::time_point startedAt{};
    std::size_t required{0u};
    std::size_t admitted{0u};
    std::optional<std::uint64_t> lastSequence;
    std::optional<ResourceClock::time_point> retiredAt;
    FlowWaitDurations waits;
};

struct FlowSnapshot {
    RuntimeResourceLimits limits;
    std::size_t admittedBlocks{0u};
    std::size_t activeComputeUnits{0u};
    bool heavyPhaseAdmitted{false};
    bool moreIndependentBlocks{false};
    bool pendingNecessaryWork{false};
    bool gateOpen{true};
    bool runActive{false};
    bool stopped{false};
    bool singleRecordFlow{false};
    std::uint64_t requestId{0u};
    ResourceWorkType workType;
    ObservationBatch observation;
};

struct ResourceControllerState {
    ResourceControlPhase phase{ResourceControlPhase::Normal};
    std::uint64_t storageCeilingBytes{0u};
    std::size_t computeCeiling{1u};
    std::size_t currentComputeCeiling{1u};
    std::optional<std::uint64_t> currentHardLimitBytes;
    bool threaded{true};
    bool initialized{false};
    bool signalValid{false};
    bool signalRecoveryPending{false};
    bool pressurePending{false};
    bool pressureConfirmed{false};
    bool severePressure{false};
    bool nativePressureActive{false};
    bool everConfirmedPressure{false};
    bool optionalRetentionPausedByPressure{false};
    std::optional<ResourceClock::time_point> pendingSince;
    std::optional<ResourceClock::time_point> lowSince;
    std::optional<ResourceClock::time_point> pendingClearSince;
    std::optional<ResourceClock::time_point> highSince;
    std::optional<ResourceClock::time_point> holdSince;
    std::optional<ResourceClock::time_point> lastPressureAt;
    std::optional<ResourceClock::time_point> lastRecoveryAt;
    std::optional<ResourceClock::time_point> lastSampleAt;
    ResourceClock::time_point cooldownUntil{};
    ResourceClock::time_point lastGrowthAt{};
    ResourceClock::time_point lastStorageGrowthAt{};
    ResourceClock::duration cooldown{resource_control::initialCooldown};
    std::uint64_t requestId{0u};
    ResourceWorkType workType;
    std::uint64_t observationEpoch{0u};
    std::optional<ResourceClock::time_point> firstObservationSampleAt;
    std::uint64_t firstObservationAvailableBytes{0u};
    std::size_t observationSampleCount{0u};
};

struct ControlDecision {
    ResourceDecisionReason reason{ResourceDecisionReason::NoChange};
    RuntimeResourceLimits limits;
    bool gateOpen{true};
    bool optionalRetentionPausedByPressure{false};
    bool trimOptionalRetention{false};
    bool failForSustainedPressure{false};
    bool resetObservation{false};
};

void InitializeResourceController(ResourceControllerState&,
                                  const ResolvedResourceConfiguration&,
                                  ResourceClock::time_point) noexcept;
ControlDecision Advance(ResourceControllerState&, ResourceClock::time_point,
                        const ResourceSample&, const FlowSnapshot&) noexcept;

ResourceWatermarks MakeResourceWatermarks(std::uint64_t totalBytes) noexcept;
std::size_t ResourceComputeCapacity(const ResourceSample&, CodecResourceMode) noexcept;
ResolvedResourceConfiguration ResolveResourceConfiguration(const CodecResourceParams&, const ResourceSample&);

}

#endif
