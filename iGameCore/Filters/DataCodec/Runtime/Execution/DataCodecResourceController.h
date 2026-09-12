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
inline constexpr auto recoveryConfirmation = std::chrono::seconds(2);
inline constexpr auto recoveryCooldown = std::chrono::seconds(2);
inline constexpr auto holdTimeout = std::chrono::seconds(30);
inline constexpr auto initialGrowthInterval = std::chrono::milliseconds(500);
inline constexpr double defaultReserveRatio = 0.20;
inline constexpr double nominalGain = 0.5;
inline constexpr double responseAlpha = 0.5;
inline constexpr double minimumGain = 0.025;
inline constexpr double maximumGain = 1.0;
inline constexpr double probeScale = 0.75;
inline constexpr auto gainWindow = std::chrono::milliseconds(250);
inline constexpr auto calibrationDeadline = std::chrono::seconds(1);
}

struct FlowWaitDurations {
    ResourceClock::duration compute{};
    ResourceClock::duration slotCapacity{};
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
    std::uint64_t nextWorkBytes{0u};
    bool byteWaiting{false};
    ResourceClock::time_point byteWaitStarted{};
    ResourceClock::duration byteWaitDuration{};
    std::uint64_t reservedBytes{0u};
    std::size_t queuedTasks{0u};
    std::uint64_t demandId{0u};
    MemoryDemandKind demandKind{MemoryDemandKind::Block};
};

struct MemoryGrant {
    std::uint64_t demandId{0u};
    std::uint64_t baselineReservedBytes{0u};
    ResourceClock::time_point sampledAt{};
    ResourceClock::time_point grantedAt{};
    std::optional<ResourceClock::time_point> completedAt;
    std::optional<std::uint64_t> sequence;
    std::uint64_t flowId{0u};
    bool used{false};
    bool probe{false};
};

struct ResourceControllerState {
    ResourceControlPhase phase{ResourceControlPhase::Normal};
    MemoryControlSnapshot memory;
    MemoryGrant grant;
    std::size_t normalSlotLimit{1u};
    bool initialized{false};
    bool signalValid{false};
    bool pressurePending{false};
    bool pressureConfirmed{false};
    bool severePressure{false};
    bool optionalRetentionPausedByPressure{false};
    std::optional<ResourceClock::time_point> lowSince;
    std::optional<ResourceClock::time_point> highSince;
    std::optional<ResourceClock::time_point> lastSampleAt;
    std::optional<std::uint64_t> previousAvailableBytes;
    std::uint64_t growthAvailableBytes{0u};
    double recentAvailableAverage{0.0};
    ResourceClock::time_point cooldownUntil{};
    std::optional<ResourceClock::time_point> recoveryStartedAt;
    std::optional<ResourceClock::time_point> progressAt;
    std::uint64_t requestId{0u};
    unsigned gainPhase{0u};
    bool initialMeasurement{false};
    ResourceClock::time_point gainWindowStarted{};
    double availableSum{0.0};
    std::size_t availableSamples{0u};
    std::uint64_t measurementBytes{0u};
};

struct ControlDecision {
    ResourceDecisionReason reason{ResourceDecisionReason::NoChange};
    RuntimeResourceLimits limits;
    bool gateOpen{true};
    bool optionalRetentionPausedByPressure{false};
    bool trimOptionalRetention{false};
    bool failForSustainedPressure{false};
    bool resetObservation{false};
    bool failForCapacityBound{false};
};

void InitializeResourceController(ResourceControllerState&,
                                  const ResolvedResourceConfiguration&,
                                  ResourceClock::time_point) noexcept;
ControlDecision Advance(ResourceControllerState&, ResourceClock::time_point,
                        const ResourceSample&, const FlowSnapshot&) noexcept;

ResourceWatermarks MakeResourceWatermarks(std::uint64_t totalBytes) noexcept;
ResourceWatermarks MakeReserveWatermarks(std::uint64_t, double) noexcept;
bool ValidPhysicalMemorySample(const ResourceSample&, ResourceClock::time_point) noexcept;
void NoteMemoryReservation(ResourceControllerState&, std::uint64_t bytes, std::uint64_t demandId,
    ResourceClock::time_point) noexcept;
void FinishMemoryMeasurement(ResourceControllerState&, ResourceClock::time_point,
    ResourceDecisionReason) noexcept;
std::size_t ResourceComputeCapacity(const ResourceSample&, CodecResourceMode,
    CodecThreadMode = CodecThreadMode::Fixed) noexcept;
ResolvedResourceConfiguration ResolveResourceConfiguration(const CodecResourceParams&, const ResourceSample&);

}

#endif
