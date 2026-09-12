#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Runtime/Cache/DecodeCacheRuntime.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace datacodec {
namespace {

CodecFailureRecord ExecutionFailure(std::string_view reason, std::string_view message) noexcept {
    return MakeCodecFailureRecord(CodecErrorCode::PipelineFailure, reason,
                                  "DataCodecExecutionResources", message);
}

bool ValidLimits(const RuntimeResourceLimits& limits, const ResolvedResourceConfiguration& config) noexcept {
    if (config.threadMode != CodecThreadMode::Fixed && config.threadMode != CodecThreadMode::Adaptive) { return false; }
    if (config.threadMode == CodecThreadMode::Adaptive && (!config.threaded || !CpuUsageProbe::Supported() ||
        !std::isfinite(config.targetCpuIdleRatio) || config.targetCpuIdleRatio < 0.0 || config.targetCpuIdleRatio >= 1.0)) { return false; }
    const auto p = config.computeCeiling;
    const auto q = std::min<std::size_t>(p, 8u);
    const bool unlimited = config.mode == CodecResourceMode::Unlimited;
    const bool validStorage = unlimited
        ? !config.storageCeilingBytes && !limits.ownedStorageLimitBytes
        : config.storageCeilingBytes && limits.ownedStorageLimitBytes &&
            (config.mode == CodecResourceMode::Adaptive || *limits.ownedStorageLimitBytes <= *config.storageCeilingBytes);
    return p != 0u && p <= std::numeric_limits<std::size_t>::max() / 2u - q - 1u &&
        validStorage &&
        limits.computeLimit >= 1u && limits.computeLimit <= p &&
        limits.slotLimit >= 1u && limits.slotLimit <= p + q &&
        (config.threaded || (limits.computeLimit == 1u && limits.slotLimit == 1u));
}

}

struct ExecutionState {
    struct Admission {
        std::uint64_t generation{0u};
        std::uint64_t sequence{0u};
        bool active{false};
        bool pendingWork{false};
        bool committed{false};
        bool releaseAfterWork{false};
        BlockCompletion completion{BlockCompletion::Pending};
    };

    explicit ExecutionState(const ResolvedResourceConfiguration& config)
        : configuration(config),
          capacity(std::make_shared<resource::ResidentByteBudget>(0u)),
          admissions(config.computeCeiling + std::min<std::size_t>(config.computeCeiling, 8u) + 1u),
          queue(admissions.size()), workerIds(config.computeCeiling) {
        debug.storageCeilingBytes = config.storageCeilingBytes;
        debug.computeCeiling = config.computeCeiling;
        debug.mode = config.mode;
        debug.resourceSample = config.initialSample;
        InitializeResourceController(controller, config, ResourceClock::now());
        ResetCpuLocked(ResourceClock::now());
    }

    bool AutomaticCpu() const noexcept { return configuration.threadMode == CodecThreadMode::Adaptive; }
    bool HasController() const noexcept { return AutomaticCpu() || configuration.mode == CodecResourceMode::Adaptive; }
    void DemandLocked(std::uint64_t bytes, MemoryDemandKind kind, resource::StorageOwnerTag owner = {}) noexcept {
        const bool changedDemand = !debug.byteWaiting || debug.nextWorkBytes != bytes || debug.memoryDemandKind != kind;
        if (!debug.byteWaiting) { debug.byteWaitStarted = ResourceClock::now(); }
        debug.byteWaiting = true;
        debug.nextWorkBytes = bytes;
        debug.memoryDemandKind = kind;
        debug.memoryRequester = owner;
        if (changedDemand) {
            if (debug.memoryDemandId == std::numeric_limits<std::uint64_t>::max()) {
                FailLocked(ExecutionFailure("memory-demand-overflow", "memory demand identity exhausted"));
                return;
            }
            ++debug.memoryDemandId;
            memoryControlRequested = true;
            controllerChanged.notify_all();
        }
    }
    void ClearDemandLocked() noexcept {
        if (debug.byteWaiting) { debug.byteWaitDuration += ResourceClock::now() - debug.byteWaitStarted; }
        debug.byteWaiting = false;
        debug.nextWorkBytes = 0u;
    }
    void ReservedLocked(std::uint64_t bytes, std::optional<std::uint64_t> sequence = {}) noexcept {
        const auto now = ResourceClock::now();
        if (controller.grant.demandId == debug.memoryDemandId && !controller.grant.used) {
            controller.grant.sequence = sequence;
            controller.grant.flowId = debug.flowId;
        }
        NoteMemoryReservation(controller, bytes, debug.memoryDemandId, now);
        if (controller.phase == ResourceControlPhase::Normal) { controller.memory.waitSince.reset(); }
        ClearDemandLocked();
    }
    void ResetCpuLocked(ResourceClock::time_point now) noexcept {
        cpuController.Reset(configuration.threadMode, configuration.targetCpuIdleRatio, configuration.computeCeiling, now);
        debug.cpu = cpuController.Snapshot();
        cpuBudget.Reset(debug.cpu.quota, now);
        debug.cpuThrottleDuration = {};
        cpuUpdatedAt = now;
    }
    std::size_t EffectiveComputeLocked() const noexcept {
        return AutomaticCpu() ? std::min(debug.limits.computeLimit,
            static_cast<std::size_t>(std::ceil(debug.cpu.quota))) : debug.limits.computeLimit;
    }
    bool CpuAdmissionOpen() const noexcept { return !AutomaticCpu() || debug.cpu.quota > 0.0; }
    void AccrueCpuLocked(ResourceClock::time_point now) noexcept {
        if (!AutomaticCpu()) { return; }
        if (debug.runActive && !debug.runStopped && debug.queuedTasks != 0u &&
            (!cpuBudget.Available() || EffectiveComputeLocked() == 0u)) {
            debug.cpuThrottleDuration += now - cpuUpdatedAt;
        }
        cpuBudget.Accrue(now, debug.activeComputeUnits);
        cpuUpdatedAt = now;
    }

    void AccumulateWaits(ResourceClock::time_point now) noexcept {
        const auto elapsed = now - waitsUpdatedAt;
        if (waitCompute) { observation.waits.compute += elapsed; }
        if (waitSlots) { observation.waits.slotCapacity += elapsed; }
        if (waitInput) { observation.waits.input += elapsed; }
        if (waitOutput) { observation.waits.output += elapsed; }
        waitsUpdatedAt = now;
    }

    void RefreshWaits() noexcept {
        waitCompute = moreIndependentBlocks && debug.queuedTasks != 0u &&
            debug.activeComputeUnits >= debug.limits.computeLimit;
        waitSlots = moreIndependentBlocks && debug.gateOpen &&
            debug.admittedBlocks >= debug.limits.slotLimit && !debug.singleRecordFlow;
        waitInput = moreIndependentBlocks && debug.gateOpen && debug.queuedTasks == 0u &&
            debug.activeComputeUnits < debug.limits.computeLimit && !debug.heavyPhaseAdmitted;
        // 首块仍在计算时没有结果积压，不能把计算等待归为输出拥堵
        const bool hasUnconsumedResult = debug.maxCompleted &&
            (!debug.lastCommitted || *debug.maxCompleted > *debug.lastCommitted);
        waitOutput = hasUnconsumedResult &&
            (debug.waiting == ResourceWaitReason::OrderedCommit ||
             debug.waiting == ResourceWaitReason::OutputIO ||
             debug.waiting == ResourceWaitReason::OwnerConsumption);
    }

    void ResetObservationLocked(ResourceClock::time_point now) noexcept {
        const auto epoch = observation.epoch + 1u;
        observation = {};
        observation.epoch = epoch;
        observation.startedAt = now;
        observation.required = debug.singleRecordFlow ? 1u : debug.limits.slotLimit;
        waitsUpdatedAt = now;
        RefreshWaits();
    }

    FlowSnapshot FlowLocked(ResourceClock::time_point now) noexcept {
        AccumulateWaits(now);
        return {debug.limits, debug.admittedBlocks, debug.activeComputeUnits,
            debug.heavyPhaseAdmitted, moreIndependentBlocks,
            moreIndependentBlocks || pendingHeavyPhase || debug.byteWaiting, debug.gateOpen,
            debug.runActive, debug.runStopped || debug.closing,
            debug.singleRecordFlow, debug.requestId, workType, observation,
            debug.nextWorkBytes, debug.byteWaiting, debug.byteWaitStarted,
            debug.byteWaitDuration + (debug.byteWaiting ? now - debug.byteWaitStarted : ResourceClock::duration{}),
            capacity->Snapshot().reservedBytes, debug.queuedTasks, debug.memoryDemandId, debug.memoryDemandKind};
    }

    void Event(ResourceEventKind kind, std::uint64_t sequence = 0u,
               ResourceDecisionReason reason = ResourceDecisionReason::NoChange,
               RuntimeResourceLimits before = {}, std::optional<std::uint64_t> reservedBytes = {}) noexcept {
        const auto now = ResourceClock::now();
        AccumulateWaits(now);
        RefreshWaits();
        auto& event = debug.events[debug.nextEvent];
        event = ResourceEvent{kind, reason, now, debug.requestId,
            debug.flowId, sequence, before, debug.limits, debug.admittedBlocks, debug.activeComputeUnits};
        if (kind == ResourceEventKind::MemoryControl || kind == ResourceEventKind::Limits ||
            kind == ResourceEventKind::EndRequest || kind == ResourceEventKind::Failure) {
            // 发布额度时调用方已持有容量锁，事件只接收同一事务取得的确定值
            event.reservedBytes = reservedBytes;
            event.grantEpoch = controller.memory.grantEpoch;
            event.memoryGain = controller.memory.gain;
            event.memoryCalibration = controller.memory.calibration;
            event.memoryPhase = controller.phase;
        }
        debug.nextEvent = (debug.nextEvent + 1u) % debug.events.size();
        if (debug.eventCount < debug.events.size()) { ++debug.eventCount; }
        else { ++debug.overwrittenEvents; }
        ++debug.eventEpoch;
        // 正常块流由定时采样观察，排空与请求生命周期即时唤醒控制线程
        if (HasController() &&
            (debug.controlPhase != ResourceControlPhase::Normal ||
             kind == ResourceEventKind::BeginRequest || kind == ResourceEventKind::EndRequest ||
             kind == ResourceEventKind::Failure || kind == ResourceEventKind::Stop || kind == ResourceEventKind::Close)) {
            controllerChanged.notify_all();
        }
    }

    bool FailLocked(const CodecFailureRecord& failure, bool close = false) noexcept {
        FinishMemoryMeasurement(controller, ResourceClock::now(),
            failure.cancelled ? ResourceDecisionReason::RequestCancelled : ResourceDecisionReason::RequestEnded);
        const bool first = !debug.failure.has_value();
        if (first) { debug.failure = failure; }
        else { ++debug.secondaryFailures; }
        debug.runStopped = true;
        debug.closing |= close;
        Event(ResourceEventKind::Failure);
        return first;
    }

    bool IsWorkerLocked() const noexcept {
        const auto id = std::this_thread::get_id();
        return (!configuration.threaded && runningTasks != 0u && id == driver) ||
            std::find(workerIds.begin(), workerIds.end(), id) != workerIds.end();
    }

    bool CanStartLocked() noexcept {
        AccrueCpuLocked(ResourceClock::now());
        if (debug.runStopped || debug.closing || debug.queuedTasks == 0u || debug.exclusiveUnits != 0u) {
            return false;
        }
        if (AutomaticCpu() && !cpuBudget.Available()) { return false; }
        const auto& work = queue[queueHead];
        return work->m_kind == TerminalWorkKind::ExclusivePackage
            ? debug.activeComputeUnits == 0u
            : debug.activeComputeUnits < EffectiveComputeLocked();
    }

    std::shared_ptr<TerminalWork> PopLocked() noexcept {
        auto work = std::move(queue[queueHead]);
        queueHead = (queueHead + 1u) % queue.size();
        --debug.queuedTasks;
        return work;
    }

    std::size_t StartLocked(TerminalWork& work) noexcept {
        AccrueCpuLocked(ResourceClock::now());
        const auto compute = EffectiveComputeLocked();
        const auto units = work.m_kind == TerminalWorkKind::ExclusivePackage && compute >= 3u ? compute : 1u;
        debug.activeComputeUnits += units;
        debug.peakActiveComputeUnits = std::max(debug.peakActiveComputeUnits, debug.activeComputeUnits);
        ++runningTasks;
        if (work.m_kind == TerminalWorkKind::ExclusivePackage) { debug.exclusiveUnits = units; }
        work.m_completion = BlockCompletion::Running;
        admissions[work.m_admission].completion = work.m_completion;
        AccumulateWaits(ResourceClock::now());
        RefreshWaits();
        return units;
    }

    void ReleaseLocked(std::size_t index, std::uint64_t generation) noexcept {
        auto& admission = admissions[index];
        if (!admission.active || admission.generation != generation) {
            FailLocked(ExecutionFailure("invalid-admission", "admission was already released"), true);
            return;
        }
        if (admission.pendingWork) {
            admission.releaseAfterWork = true;
            FailLocked(ExecutionFailure("live-admission-release", "admission released while terminal work is pending"), true);
            return;
        }
        admission.active = false;
        controller.progressAt = ResourceClock::now();
        if (controller.grant.used && controller.grant.flowId == debug.flowId &&
            controller.grant.sequence == admission.sequence && index + 1u < admissions.size()) {
            controller.grant.completedAt = controller.progressAt;
        }
        if (index == admissions.size() - 1u) {
            debug.heavyPhaseAdmitted = false;
        } else {
            --debug.admittedBlocks;
            debug.lastRetired = admission.sequence;
            if (observation.lastSequence && admission.sequence == *observation.lastSequence) {
                observation.retiredAt = ResourceClock::now();
            }
        }
        Event(ResourceEventKind::Retire, admission.sequence);
    }

    void CompleteLocked(TerminalWork& work, std::size_t units, bool success) noexcept {
        AccrueCpuLocked(ResourceClock::now());
        auto& admission = admissions[work.m_admission];
        work.m_completion = debug.runStopped ? BlockCompletion::Cancelled : BlockCompletion::Succeeded;
        if (!success) { work.m_completion = BlockCompletion::Failed; }
        admission.completion = work.m_completion;
        debug.activeComputeUnits -= units;
        --runningTasks;
        if (work.m_kind == TerminalWorkKind::ExclusivePackage) { debug.exclusiveUnits = 0u; }
        admission.pendingWork = false;
        if (work.m_admission != admissions.size() - 1u) {
            debug.maxCompleted = std::max(debug.maxCompleted.value_or(0u), admission.sequence);
        }
        Event(ResourceEventKind::Complete, admission.sequence);
        if (admission.releaseAfterWork) { ReleaseLocked(work.m_admission, work.m_generation); }
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable controllerChanged;
    const ResolvedResourceConfiguration configuration;
    std::shared_ptr<resource::ResidentByteBudget> capacity;
    std::vector<Admission> admissions;
    std::vector<std::shared_ptr<TerminalWork>> queue;
    std::vector<std::thread::id> workerIds;
    std::size_t queueHead{0u};
    std::size_t runningTasks{0u};
    std::thread::id driver;
    std::stop_source stopSource;
    ResourceDebugSnapshot debug;
    std::optional<std::size_t> waitingAdmission;
    std::uint64_t waitingGeneration{0u};
    ResourceControllerState controller;
    DataCodecCpuController cpuController;
    CpuPermitBudget cpuBudget;
    ResourceClock::time_point cpuUpdatedAt{};
    ObservationBatch observation;
    ResourceClock::time_point waitsUpdatedAt{ResourceClock::now()};
    bool moreIndependentBlocks{false};
    bool pendingHeavyPhase{false};
    bool memoryControlRequested{false};
    ResourceWorkType workType;
    bool waitCompute{false};
    bool waitSlots{false};
    bool waitInput{false};
    bool waitOutput{false};
};

struct DataCodecExecutionResources::Impl {
    explicit Impl(const ResolvedResourceConfiguration& config)
        : state(std::make_shared<ExecutionState>(config)),
          scratch(2u * (config.computeCeiling + std::min<std::size_t>(config.computeCeiling, 8u))),
          inlineWorker(scratch, 0u) {
        workers.reserve(config.computeCeiling);
    }
    ~Impl() { pressureMonitor.reset(); }
    std::shared_ptr<ExecutionState> state;
    ScratchByteBufferPool scratch;
    WorkerContext inlineWorker;
    std::vector<std::thread> workers;
    std::thread controllerThread;
    std::unique_ptr<ResourcePressureMonitor> pressureMonitor;
    std::unique_ptr<CpuUsageProbe> cpuProbe;
    std::atomic_bool pressureSignal{false};
    std::unique_ptr<DecodeCacheRuntime> caches;
    std::uint64_t servicedTrimEpoch{0u};
};

SlotLease::SlotLease(std::shared_ptr<ExecutionState> state, std::size_t index, std::uint64_t generation) noexcept
    : m_state(std::move(state)), m_index(index), m_generation(generation) {}
SlotLease::SlotLease(SlotLease&& other) noexcept
    : m_state(std::move(other.m_state)), m_index(other.m_index), m_generation(other.m_generation) {}
SlotLease& SlotLease::operator=(SlotLease&& other) noexcept {
    if (this != &other) {
        Reset();
        m_state = std::move(other.m_state);
        m_index = other.m_index;
        m_generation = other.m_generation;
    }
    return *this;
}
SlotLease::~SlotLease() { Reset(); }
void SlotLease::Reset() noexcept {
    auto state = std::move(m_state);
    if (!state) { return; }
    bool stop = false;
    std::stop_source runStop(std::nostopstate);
    {
        std::lock_guard lock(state->mutex);
        state->ReleaseLocked(m_index, m_generation);
        stop = state->debug.runStopped;
        runStop = state->stopSource;
    }
    if (stop) { runStop.request_stop(); }
    state->changed.notify_all();
}

HeavyPhaseLease::HeavyPhaseLease(HeavyPhaseLease&&) noexcept = default;
HeavyPhaseLease& HeavyPhaseLease::operator=(HeavyPhaseLease&&) noexcept = default;
HeavyPhaseLease::~HeavyPhaseLease() = default;
void HeavyPhaseLease::Reset() noexcept { m_admission.Reset(); }

DataCodecExecutionResources::DataCodecExecutionResources(const ResolvedResourceConfiguration& config) {
    if (!ValidLimits(config.initialLimits, config)) { throw std::invalid_argument("invalid resource configuration"); }
    m_impl = std::make_unique<Impl>(config);
    m_impl->state->capacity->SetReleaseNotification([weak = std::weak_ptr(m_impl->state)]() noexcept {
        if (auto state = weak.lock()) {
            {
                std::lock_guard lock(state->mutex);
                ++state->debug.eventEpoch;
            }
            state->changed.notify_all();
        }
    });
    m_impl->caches = std::make_unique<DecodeCacheRuntime>(*this);
    if (!UpdateLimits(config.initialLimits, config.gateOpen, ResourceDecisionReason::Initialize)) {
        throw std::invalid_argument("resource initialization failed");
    }
    if (config.mode == CodecResourceMode::Adaptive) {
        m_impl->pressureMonitor = std::make_unique<ResourcePressureMonitor>([](void* context) noexcept {
            auto& impl = *static_cast<Impl*>(context);
            {
                // 与控制线程的条件检查同步，避免通知发生在进入等待之前
                std::lock_guard lock(impl.state->mutex);
                impl.pressureSignal.store(true, std::memory_order_release);
            }
            impl.state->controllerChanged.notify_all();
        }, m_impl.get());
        m_impl->pressureMonitor->Observe(config.initialSample);
    }
    if (config.threadMode == CodecThreadMode::Adaptive) { m_impl->cpuProbe = std::make_unique<CpuUsageProbe>(); }
    if (m_impl->state->HasController()) {
        m_impl->controllerThread = std::thread([this] { ControlMain(); });
    }
}
DataCodecExecutionResources::~DataCodecExecutionResources() { ShutdownAndJoin(); }
DataCodecExecutionResources::DataCodecExecutionResources(const CodecResourceParams& params)
    : DataCodecExecutionResources(ResolveResourceConfiguration(params, ProbeResources())) {}

std::optional<std::uint64_t> DataCodecExecutionResources::FixedStorageLimitBytes() const noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    return state.configuration.mode == CodecResourceMode::Fixed
        ? state.capacity->Snapshot().limitBytes : std::nullopt;
}

bool DataCodecExecutionResources::BeginRun() noexcept {
    try {
        auto& state = *m_impl->state;
        const auto sample = state.configuration.mode == CodecResourceMode::Adaptive
            ? ProbeResources() : state.configuration.initialSample;
        std::unique_lock lock(state.mutex);
        if (state.debug.closing) { return false; }
        if (state.debug.runActive || state.debug.admittedBlocks != 0u || state.runningTasks != 0u ||
            state.debug.queuedTasks != 0u || state.debug.heavyPhaseAdmitted) {
            lock.unlock();
            RecordFailure(ExecutionFailure("run-not-drained", "previous request has not drained"), true);
            return false;
        }
        if (state.debug.requestId == std::numeric_limits<std::uint64_t>::max()) {
            lock.unlock();
            RecordFailure(ExecutionFailure("request-overflow", "request identity exhausted"), true);
            return false;
        }
        state.debug.failure.reset();
        std::stop_source nextStop;
        state.stopSource = std::move(nextStop);
        state.driver = std::this_thread::get_id();
        state.debug.runActive = true;
        state.debug.singleRecordFlow = false;
        state.debug.runStopped = false;
        state.debug.failure.reset();
        state.debug.secondaryFailures = 0u;
        state.debug.diagnosticsIncomplete = false;
        state.debug.diagnosticExportFailures = 0u;
        state.debug.capacityRejection.reset();
        state.debug.capacityRejectionRequestId = 0u;
        state.debug.capacityRejectionFlowId = 0u;
        state.debug.capacityRejectionFatal = false;
        state.debug.waiting = ResourceWaitReason::None;
        state.debug.waitContext = {};
        state.waitingAdmission.reset();
        ++state.debug.requestId;
        state.ResetCpuLocked(ResourceClock::now());
        state.ClearDemandLocked();
        state.debug.memoryDemandId = 0u;
        state.memoryControlRequested = false;
        state.moreIndependentBlocks = false;
        state.pendingHeavyPhase = false;
        state.ResetObservationLocked(ResourceClock::now());
        std::optional<CodecFailureRecord> controlFailure;
        if (state.configuration.mode == CodecResourceMode::Adaptive) {
            controlFailure = ValidPhysicalMemorySample(sample, ResourceClock::now())
                ? ApplyControlLocked(ResourceClock::now(), sample)
                : std::optional(ExecutionFailure("memory-observation-unavailable", "physical memory observation unavailable at request start"));
        }
        m_impl->scratch.SetRetainedCount(state.debug.limits.ownedStorageLimitBytes != 0u &&
            !state.debug.optionalRetentionPausedByPressure
            ? 2u * std::max(state.debug.limits.slotLimit, state.debug.limits.computeLimit) : 0u);
        state.Event(ResourceEventKind::BeginRequest);
        lock.unlock();
        m_impl->scratch.TrimRetained();
        if (controlFailure) { RecordFailure(*controlFailure); }
        state.changed.notify_all();
        return !controlFailure;
    } catch (const std::bad_alloc&) {
        RecordFailure(ExecutionFailure("allocation-failed", "request stop state allocation failed"), true);
    } catch (...) {
        RecordFailure(ExecutionFailure("execution-failed", "request initialization failed"), true);
    }
    return false;
}

bool DataCodecExecutionResources::EndRun() noexcept {
    auto& state = *m_impl->state;
    bool failed = false;
    {
        std::lock_guard lock(state.mutex);
        failed = state.driver != std::this_thread::get_id() || !state.debug.runActive ||
            state.debug.admittedBlocks != 0u || state.runningTasks != 0u ||
            state.debug.queuedTasks != 0u || state.debug.heavyPhaseAdmitted;
        if (!failed) {
            FinishMemoryMeasurement(state.controller, ResourceClock::now(), ResourceDecisionReason::RequestEnded);
            state.debug.runActive = false;
            state.debug.waiting = ResourceWaitReason::None;
            state.moreIndependentBlocks = false;
            state.pendingHeavyPhase = false;
            state.Event(ResourceEventKind::EndRequest);
        }
    }
    if (failed) { RecordFailure(ExecutionFailure("run-not-drained", "request completion requires all work to drain"), true); }
    state.changed.notify_all();
    return !failed;
}

bool DataCodecExecutionResources::BeginFlow(const bool singleRecord) noexcept {
    auto& state = *m_impl->state;
    bool invalid = false;
    {
        std::lock_guard lock(state.mutex);
        if (state.debug.runStopped || state.debug.closing) { return false; }
        invalid = !state.debug.runActive || state.driver != std::this_thread::get_id() ||
            state.IsWorkerLocked() || state.debug.admittedBlocks != 0u || state.debug.heavyPhaseAdmitted ||
            state.debug.flowId == std::numeric_limits<std::uint64_t>::max();
        if (!invalid) {
            ++state.debug.flowId;
            state.debug.singleRecordFlow = singleRecord;
            state.debug.lastAdmitted.reset();
            state.debug.maxCompleted.reset();
            state.debug.lastCommitted.reset();
            state.debug.lastRetired.reset();
            state.moreIndependentBlocks = true;
            state.ResetObservationLocked(ResourceClock::now());
            state.Event(ResourceEventKind::Wake);
        }
    }
    if (invalid) { RecordFailure(ExecutionFailure("flow-not-drained", "block flow requires an idle driver scope"), true); }
    state.changed.notify_all();
    return !invalid;
}

void DataCodecExecutionResources::SetFlowProgress(bool more) noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    if (state.moreIndependentBlocks == more) { return; }
    state.moreIndependentBlocks = more;
    state.Event(ResourceEventKind::Wake);
    state.changed.notify_all();
}

void DataCodecExecutionResources::SetWorkType(const ResourceWorkType& key) noexcept {
    auto& state = *m_impl->state;
    std::optional<CodecFailureRecord> failure;
    {
        std::lock_guard lock(state.mutex);
        if (state.workType == key) { return; }
        if (state.debug.admittedBlocks != 0u || state.debug.heavyPhaseAdmitted) {
            failure = ExecutionFailure("work-type-not-drained", "work type changes require previous admissions to retire");
        } else {
            state.workType = key;
            state.ResetObservationLocked(ResourceClock::now());
            state.Event(ResourceEventKind::Wake);
        }
    }
    m_impl->scratch.TrimRetained();
    if (failure) { RecordFailure(*failure); }
    state.changed.notify_all();
}

std::optional<SlotLease> DataCodecExecutionResources::TryAcquireSlot() {
    resource::ResidentByteBudget::Lease reservation;
    return TryAcquireSlot(0u, reservation);
}

std::optional<SlotLease> DataCodecExecutionResources::TryAcquireSlot(
    std::uint64_t bytes, resource::ResidentByteBudget::Lease& reservation) {
    if (reservation) {
        RecordFailure(ExecutionFailure("live-admission-reservation", "admission output lease must be empty"));
        return std::nullopt;
    }
    auto state = m_impl->state;
    std::unique_lock lock(state->mutex);
    auto& debug = state->debug;
    if (debug.runStopped || debug.closing) { return std::nullopt; }
    if (!debug.runActive || state->driver != std::this_thread::get_id() || state->IsWorkerLocked()) {
        lock.unlock();
        RecordFailure(ExecutionFailure("invalid-admission-driver", "block admission requires the active driver"), true);
        return std::nullopt;
    }
    if (!debug.gateOpen) { state->DemandLocked(bytes, MemoryDemandKind::Block); return std::nullopt; }
    if (!state->CpuAdmissionOpen() ||
        debug.admittedBlocks >= debug.limits.slotLimit || debug.heavyPhaseAdmitted ||
        (debug.singleRecordFlow && debug.admittedBlocks != 0u)) { return std::nullopt; }
    for (std::size_t i = 0u; i + 1u < state->admissions.size(); ++i) {
        auto& admission = state->admissions[i];
        if (admission.active) { continue; }
        if (admission.generation == std::numeric_limits<std::uint64_t>::max() ||
            (debug.lastAdmitted && *debug.lastAdmitted == std::numeric_limits<std::uint64_t>::max())) {
            lock.unlock();
            RecordFailure(ExecutionFailure("admission-overflow", "block admission sequence exhausted"), true);
            return std::nullopt;
        }
        auto lease = state->capacity->TryReserve(bytes);
        if (!lease) {
            state->DemandLocked(bytes, MemoryDemandKind::Block);
            return std::nullopt;
        }
        state->ReservedLocked(bytes, debug.lastAdmitted ? *debug.lastAdmitted + 1u : 0u);
        reservation = std::move(*lease);
        admission = ExecutionState::Admission{admission.generation + 1u,
            debug.lastAdmitted ? *debug.lastAdmitted + 1u : 0u, true};
        ++debug.admittedBlocks;
        debug.peakAdmittedBlocks = std::max(debug.peakAdmittedBlocks, debug.admittedBlocks);
        debug.lastAdmitted = admission.sequence;
        if (state->observation.admitted < state->observation.required) {
            ++state->observation.admitted;
            if (state->observation.admitted == state->observation.required) {
                state->observation.lastSequence = admission.sequence;
            }
        }
        state->Event(ResourceEventKind::Admit, admission.sequence);
        return SlotLease(state, i, admission.generation);
    }
    lock.unlock();
    RecordFailure(ExecutionFailure("slot-state-invalid", "slot state has no available record"), true);
    return std::nullopt;
}

bool DataCodecExecutionResources::CheckNecessaryCapacity(std::uint64_t bytes,
    const resource::CapacityRejection* details, MemoryDemandKind kind) {
    resource::CapacityRejection rejection = details ? *details : resource::CapacityRejection{};
    bool fatal = false;
    {
        auto& state = *m_impl->state;
        std::lock_guard lock(state.mutex);
        if (state.debug.runStopped || state.debug.closing) { return false; }
        const auto capacity = state.capacity->Snapshot();
        if (state.debug.gateOpen && capacity.CanReserve(bytes)) { return true; }
        state.DemandLocked(bytes, kind, rejection.requester);
        if (!state.debug.storageCeilingBytes) { return true; }
        const auto ceiling = *state.debug.storageCeilingBytes;
        // 提交中的共存申请仍持有自己的槽位，只等待真实排队或运行的终端任务
        const bool canProgress = state.runningTasks != 0u || state.debug.queuedTasks != 0u ||
            (kind == MemoryDemandKind::Block && state.debug.admittedBlocks != 0u);
        if (bytes <= ceiling && canProgress) { return true; }
        fatal = bytes > ceiling || (state.configuration.mode == CodecResourceMode::Fixed && !capacity.CanReserve(bytes)) ||
            capacity.reservedBytes > ceiling || bytes > ceiling - capacity.reservedBytes ||
            (state.controller.memory.waitSince && ResourceClock::now() - *state.controller.memory.waitSince >= resource_control::holdTimeout);
        rejection.requestedBytes = bytes;
        rejection.reservedBytes = capacity.reservedBytes;
        rejection.limitBytes = capacity.limitBytes;
        rejection.checkedAt = ResourceClock::now();
    }
    if (fatal) {
        RecordCapacityRejection(rejection, true, "DecodeMemoryAdmission");
        return false;
    }
    return true;
}

void DataCodecExecutionResources::ClearByteWait() noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    if (state.controller.grant.demandId == state.debug.memoryDemandId && !state.controller.grant.used) {
        state.controller.grant.used = true;
        state.controller.grant.completedAt = ResourceClock::now();
    }
    state.ClearDemandLocked();
    if (state.controller.phase == ResourceControlPhase::Normal) { state.controller.memory.waitSince.reset(); }
}

std::optional<resource::ResidentByteBudget::Lease> DataCodecExecutionResources::TryAcquireStorage(
    std::uint64_t bytes, MemoryDemandKind kind, resource::CapacityRejection* rejection,
    resource::StorageOwnerTag owner, std::span<const resource::StorageOwnerDescription> coexist,
    std::uint64_t preferredBytes) {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    if (state.debug.runStopped || state.debug.closing) { return std::nullopt; }
    if (!state.debug.runActive || state.driver != std::this_thread::get_id() || state.IsWorkerLocked()) {
        throw std::logic_error("necessary storage admission requires the driver");
    }
    const auto capacity = state.capacity->Snapshot();
    if (!state.debug.gateOpen || !capacity.CanReserve(bytes)) {
        if (rejection) {
            *rejection = {};
            rejection->requestedBytes = bytes;
            rejection->reservedBytes = capacity.reservedBytes;
            rejection->limitBytes = capacity.limitBytes;
            rejection->requester = owner;
            rejection->checkedAt = ResourceClock::now();
            rejection->ownerCount = std::min(coexist.size(), rejection->owners.size());
            rejection->ownerListTruncated = coexist.size() > rejection->owners.size();
            std::copy_n(coexist.begin(), rejection->ownerCount, rejection->owners.begin());
        }
        state.DemandLocked(bytes, kind, owner);
        return std::nullopt;
    }
    auto lease = state.capacity->TryReserveGrowth(bytes, std::max(bytes, preferredBytes), rejection, owner, coexist);
    if (lease) { state.ReservedLocked(lease->Bytes()); }
    else { state.DemandLocked(bytes, kind, owner); }
    return lease;
}

std::optional<resource::ResidentByteBudget::Lease> DataCodecExecutionResources::WaitForStorage(
    std::uint64_t bytes, MemoryDemandKind kind, resource::StorageOwnerTag owner,
    std::span<const resource::StorageOwnerDescription> coexist, std::uint64_t preferredBytes) {
    if (!IsDriverThread()) {
        RecordFailure(ExecutionFailure("worker-resource-wait", "storage waiting requires the driver"));
        return std::nullopt;
    }
    for (;;) {
        ServiceDriverEvents();
        ReclaimOptionalStorage(bytes);
        const auto epoch = EventEpoch();
        if (Stopped()) { return std::nullopt; }
        resource::CapacityRejection rejection;
        if (auto lease = TryAcquireStorage(bytes, kind, &rejection, owner, coexist, preferredBytes)) {
            SetWaitReason(ResourceWaitReason::None);
            return lease;
        }
        if (!CheckNecessaryCapacity(bytes, &rejection, kind)) { return std::nullopt; }
        SetWaitReason(ResourceWaitReason::ByteCapacity);
        WaitForChange(epoch);
    }
}

void DataCodecExecutionResources::CompleteMemoryPreparation() noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    const auto now = ResourceClock::now();
    state.controller.memory.lastPreparationAt = now;
    state.controller.progressAt = now;
    if (state.controller.grant.used && !state.controller.grant.sequence) {
        state.controller.grant.completedAt = now;
    }
    state.Event(ResourceEventKind::Complete);
    state.changed.notify_all();
}

std::optional<HeavyPhaseLease> DataCodecExecutionResources::TryAcquireHeavyPhase() {
    auto state = m_impl->state;
    std::unique_lock lock(state->mutex);
    auto& debug = state->debug;
    if (debug.runStopped || debug.closing) { return std::nullopt; }
    if (!debug.runActive || state->driver != std::this_thread::get_id() || state->IsWorkerLocked()) {
        lock.unlock();
        RecordFailure(ExecutionFailure("invalid-admission-driver", "phase admission requires the active driver"), true);
        return std::nullopt;
    }
    // driver 必须先消费并释放当前凭证，不能等待自身推进的阶段结束
    if (debug.admittedBlocks != 0u || debug.heavyPhaseAdmitted) {
        lock.unlock();
        RecordFailure(ExecutionFailure("phase-not-drained", "phase admission requires all previous admissions to retire"), true);
        return std::nullopt;
    }
    if (!debug.gateOpen || !state->CpuAdmissionOpen()) {
        if (!state->pendingHeavyPhase) {
            state->pendingHeavyPhase = true;
            state->Event(ResourceEventKind::Wait);
            state->changed.notify_all();
        }
        return std::nullopt;
    }
    const auto i = state->admissions.size() - 1u;
    auto& admission = state->admissions[i];
    if (admission.generation == std::numeric_limits<std::uint64_t>::max()) {
        lock.unlock();
        RecordFailure(ExecutionFailure("admission-overflow", "phase admission generation exhausted"), true);
        return std::nullopt;
    }
    admission = ExecutionState::Admission{admission.generation + 1u, 0u, true};
    debug.heavyPhaseAdmitted = true;
    state->pendingHeavyPhase = false;
    state->Event(ResourceEventKind::Admit);
    return HeavyPhaseLease(SlotLease(state, i, admission.generation));
}

bool DataCodecExecutionResources::SubmitTerminal(const SlotLease& slot, const std::shared_ptr<TerminalWork>& work) noexcept {
    return Submit(slot, work);
}
bool DataCodecExecutionResources::SubmitTerminal(const HeavyPhaseLease& phase, const std::shared_ptr<TerminalWork>& work) noexcept {
    return Submit(phase.m_admission, work);
}

bool DataCodecExecutionResources::Submit(const SlotLease& slot, const std::shared_ptr<TerminalWork>& work) noexcept {
    auto& state = *m_impl->state;
    std::optional<CodecFailureRecord> failure;
    bool inlineWork = false;
    std::size_t inlineUnits = 0u;
    {
        std::lock_guard lock(state.mutex);
        if (state.debug.runStopped || state.debug.closing) { return false; }
        if (!state.debug.runActive || state.driver != std::this_thread::get_id() || state.IsWorkerLocked() ||
            slot.m_state.get() != &state ||
            !work || !work->m_function || work->m_completion != BlockCompletion::Pending) {
            failure = ExecutionFailure("invalid-terminal-submit", "terminal work requires driver and a live admission");
        } else {
            auto& admission = state.admissions[slot.m_index];
            if (!admission.active || admission.generation != slot.m_generation || admission.pendingWork || admission.committed ||
                state.debug.queuedTasks == state.queue.size()) {
                failure = ExecutionFailure("invalid-terminal-submit", "admission already owns work or is no longer live");
            } else {
                work->m_admission = slot.m_index;
                work->m_generation = slot.m_generation;
                admission.pendingWork = true;
                if (!state.configuration.threaded) {
                    inlineWork = true;
                    inlineUnits = state.StartLocked(*work);
                    m_impl->inlineWorker.m_stop = state.stopSource.get_token();
                    m_impl->inlineWorker.m_units = inlineUnits;
                } else {
                    state.queue[(state.queueHead + state.debug.queuedTasks) % state.queue.size()] = work;
                    ++state.debug.queuedTasks;
                    work->m_completion = BlockCompletion::Queued;
                    admission.completion = work->m_completion;
                    failure = GrowWorkersLocked();
                }
                state.Event(ResourceEventKind::Wake);
            }
        }
    }
    if (failure) { RecordFailure(*failure, true); }
    state.changed.notify_all();
    if (failure) { return false; }
    if (inlineWork) {
        bool success = false;
        try { success = work->m_function(m_impl->inlineWorker); }
        catch (const std::bad_alloc&) { failure = ExecutionFailure("allocation-failed", "terminal allocation failed"); }
        catch (const std::exception& error) { failure = ExecutionFailure("terminal-exception", error.what()); }
        catch (...) { failure = ExecutionFailure("terminal-exception", "unknown terminal exception"); }
        if (failure || !success) {
            RecordFailure(failure.value_or(ExecutionFailure("terminal-failed", "terminal operation failed")));
        }
        work->m_function = {};
        std::stop_source runStop(std::nostopstate);
        bool stopped = false;
        {
            std::lock_guard lock(state.mutex);
            state.CompleteLocked(*work, inlineUnits, success);
            runStop = state.stopSource;
            stopped = state.debug.runStopped;
        }
        if (stopped) { runStop.request_stop(); }
        state.changed.notify_all();
    }
    return true;
}

void DataCodecExecutionResources::WorkerMain(std::size_t index) noexcept {
    auto state = m_impl->state;
    WorkerContext worker(m_impl->scratch, index);
    {
        std::lock_guard lock(state->mutex);
        state->workerIds[index] = std::this_thread::get_id();
    }
    for (;;) {
        std::shared_ptr<TerminalWork> work;
        std::size_t units = 0u;
        {
            std::unique_lock lock(state->mutex);
            while (!state->debug.closing && !state->CanStartLocked()) {
                if (state->AutomaticCpu() && state->debug.queuedTasks != 0u && !state->debug.runStopped) {
                    state->changed.wait_until(lock, state->cpuBudget.NextWake(ResourceClock::now(), state->debug.activeComputeUnits));
                } else { state->changed.wait(lock); }
            }
            if (state->debug.closing) { break; }
            work = state->PopLocked();
            units = state->StartLocked(*work);
            worker.m_units = units;
            worker.m_stop = state->stopSource.get_token();
        }
        bool success = false;
        std::optional<CodecFailureRecord> failure;
        try { success = work->m_function(worker); }
        catch (const std::bad_alloc&) { failure = ExecutionFailure("allocation-failed", "terminal allocation failed"); }
        catch (const std::exception& error) { failure = ExecutionFailure("terminal-exception", error.what()); }
        catch (...) { failure = ExecutionFailure("terminal-exception", "unknown terminal exception"); }
        if (failure || !success) {
            RecordFailure(failure.value_or(ExecutionFailure("terminal-failed", "terminal operation failed")));
        }
        work->m_function = {};
        bool stop = false;
        std::stop_source runStop(std::nostopstate);
        {
            std::lock_guard lock(state->mutex);
            state->CompleteLocked(*work, units, success);
            stop = state->debug.runStopped;
            runStop = state->stopSource;
        }
        if (stop) { runStop.request_stop(); }
        state->changed.notify_all();
    }
    {
        std::lock_guard lock(state->mutex);
        state->workerIds[index] = {};
    }
    state->changed.notify_all();
}

BlockCompletion DataCodecExecutionResources::Completion(const TerminalWork& work) const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return work.m_completion;
}

bool DataCodecExecutionResources::CommitSlot(const SlotLease& slot) noexcept {
    auto& state = *m_impl->state;
    bool valid = false;
    {
        std::lock_guard lock(state.mutex);
        if (state.debug.runStopped || state.debug.closing) { return false; }
        if (state.debug.runActive && state.driver == std::this_thread::get_id() && !state.IsWorkerLocked() &&
            slot.m_state.get() == &state && slot.m_index + 1u < state.admissions.size()) {
            auto& admission = state.admissions[slot.m_index];
            const auto next = state.debug.lastCommitted ? *state.debug.lastCommitted + 1u : 0u;
            valid = admission.active && admission.generation == slot.m_generation && !admission.pendingWork &&
                !admission.committed && admission.sequence == next;
            if (valid) {
                admission.committed = true;
                state.debug.lastCommitted = next;
                state.Event(ResourceEventKind::Commit, next);
            }
        }
    }
    if (!valid) { RecordFailure(ExecutionFailure("invalid-commit", "block commits must be consecutive and unique"), true); }
    state.changed.notify_all();
    return valid;
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::PublishLimitsLocked(
    const RuntimeResourceLimits& limits, bool gateOpen, ResourceDecisionReason reason,
    std::optional<bool> retentionPaused, bool trim, bool resetObservation) noexcept {
    auto& state = *m_impl->state;
    if (!ValidLimits(limits, state.configuration)) {
        return ExecutionFailure("invalid-limits", "resource targets exceed configured capabilities");
    }
    if (state.configuration.mode == CodecResourceMode::Adaptive &&
        *limits.ownedStorageLimitBytes > state.controller.memory.absoluteCapacityBytes) {
        return ExecutionFailure("invalid-limits", "memory limit exceeds current absolute capacity");
    }
    std::lock_guard capacityLock(state.capacity->m_state->mutex);
    const auto before = RuntimeResourceLimits{state.capacity->m_state->limitBytes,
        state.debug.limits.computeLimit, state.debug.limits.slotLimit};
    const bool policyChanged = retentionPaused &&
        *retentionPaused != state.debug.optionalRetentionPausedByPressure;
    const bool changed = before != limits || state.debug.gateOpen != gateOpen ||
        policyChanged || reason == ResourceDecisionReason::Initialize;
    if (retentionPaused) { state.debug.optionalRetentionPausedByPressure = *retentionPaused; }
    if (changed) {
        state.capacity->SetLimitLocked(limits.ownedStorageLimitBytes);
        // M 的副本只用于固定诊断，预约始终读取容量 state
        state.debug.limits = limits;
        state.debug.gateOpen = gateOpen;
        ++state.debug.targetEpoch;
        state.Event(ResourceEventKind::Limits, 0u, reason, before, state.capacity->m_state->reservedBytes);
    } else if (reason != ResourceDecisionReason::NoChange && reason != ResourceDecisionReason::MechanismCheck) {
        state.Event(ResourceEventKind::Wake, 0u, reason, before);
    }
    if (trim || limits.ownedStorageLimitBytes < before.ownedStorageLimitBytes ||
        limits.computeLimit < before.computeLimit || limits.slotLimit < before.slotLimit) {
        ++state.debug.trimEpoch;
        state.Event(ResourceEventKind::TrimRequested);
    }
    if (changed || resetObservation) { state.ResetObservationLocked(ResourceClock::now()); }
    const bool retain = limits.ownedStorageLimitBytes != 0u && !state.debug.optionalRetentionPausedByPressure &&
        !state.debug.closing && !state.debug.runStopped;
    m_impl->scratch.SetRetainedCount(retain ? 2u * std::max(limits.slotLimit, limits.computeLimit) : 0u);
    if (!state.debug.closing && !state.debug.runStopped && state.configuration.threaded) {
        return GrowWorkersLocked();
    }
    return std::nullopt;
}

bool DataCodecExecutionResources::UpdateLimits(const RuntimeResourceLimits& limits, bool gateOpen,
    ResourceDecisionReason reason, std::string* error) noexcept {
    auto& state = *m_impl->state;
    std::optional<CodecFailureRecord> failure;
    {
        std::lock_guard lock(state.mutex);
        failure = PublishLimitsLocked(limits, gateOpen, reason);
    }
    m_impl->scratch.TrimRetained();
    if (failure) {
        RecordFailure(*failure, true);
        if (error) { try { error->assign("resource target publication failed"); } catch (...) {} }
    }
    state.changed.notify_all();
    return !failure;
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::ApplyControlLocked(
    ResourceClock::time_point now, const ResourceSample& sample) noexcept {
    auto& state = *m_impl->state;
    state.debug.resourceSample = sample;
    const auto previousCalibration = state.controller.memory.calibration;
    const auto previousGainUpdates = state.controller.memory.gainUpdates;
    const auto previousPhase = state.controller.phase;
    const auto flow = state.FlowLocked(now);
    const auto decision = Advance(state.controller, now, sample, flow);
    state.debug.controlPhase = state.controller.phase;
    state.debug.pressurePending = state.controller.pressurePending;
    state.debug.signalValid = state.controller.signalValid;
    state.debug.cooldownUntil = state.controller.cooldownUntil;
    state.debug.holdSince = state.controller.memory.waitSince;
    state.debug.storageCeilingBytes = state.controller.memory.absoluteCapacityBytes;
    state.debug.memory = state.controller.memory;
    auto failure = PublishLimitsLocked(decision.limits, decision.gateOpen, decision.reason,
        decision.optionalRetentionPausedByPressure, decision.trimOptionalRetention, decision.resetObservation);
    if (failure) { return failure; }
    if (previousCalibration != state.controller.memory.calibration ||
        previousGainUpdates != state.controller.memory.gainUpdates || previousPhase != state.controller.phase) {
        state.Event(ResourceEventKind::MemoryControl, 0u, decision.reason, {}, flow.reservedBytes);
    }
    if (decision.failForCapacityBound) {
        return ExecutionFailure("memory-capacity-bound", "necessary coexisting storage exceeds current absolute capacity");
    }
    if (decision.failForSustainedPressure) {
        return ExecutionFailure("sustained-memory-pressure", "memory pressure did not recover within the request hold deadline");
    }
    return std::nullopt;
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::ApplyCpuControlLocked(
    ResourceClock::time_point now, const CpuUsageSample& sample) noexcept {
    auto& state = *m_impl->state;
    state.AccrueCpuLocked(now);
    const CpuControlFlow flow{
        state.debug.limits.computeLimit,
        state.runningTasks != 0u || state.debug.queuedTasks != 0u,
        state.debug.queuedTasks != 0u || state.moreIndependentBlocks || state.pendingHeavyPhase,
        !state.debug.gateOpen || state.debug.byteWaiting ||
            state.debug.waiting == ResourceWaitReason::InputIO || state.debug.waiting == ResourceWaitReason::OutputIO};
    const auto decision = state.cpuController.Advance(now, sample, flow);
    state.debug.cpu = state.cpuController.Snapshot();
    state.debug.cpuDecision = decision.reason;
    state.cpuBudget.SetQuota(state.debug.cpu.quota, now, state.debug.activeComputeUnits);
    // CPU 采样只唤醒执行器，不重置内存控制器的观察批次
    ++state.debug.eventEpoch;
    if (decision.sampleFailure) {
        return ExecutionFailure("cpu-sample-unavailable", "system CPU idle sampling was unavailable for one second");
    }
    return GrowWorkersLocked();
}

void DataCodecExecutionResources::ControlMain() noexcept {
    auto& state = *m_impl->state;
    auto nextSampleAt = ResourceClock::now();
    auto nextCpuAt = nextSampleAt;
    std::uint64_t cpuRequest = 0u;
    const bool memoryAdaptive = state.configuration.mode == CodecResourceMode::Adaptive;
    for (;;) {
        std::unique_lock lock(state.mutex);
        state.controllerChanged.wait(lock, [&] {
            return state.debug.closing || (state.debug.runActive && !state.debug.runStopped);
        });
        if (state.debug.closing) { return; }
        const auto requestId = state.debug.requestId;
        auto now = ResourceClock::now();
        ResourceSample sample = state.debug.resourceSample;
        CpuUsageSample cpuSample;
        const bool resetCpu = state.AutomaticCpu() && cpuRequest != requestId;
        const bool sampleCpu = state.AutomaticCpu() && (resetCpu || now >= nextCpuAt);
        const bool pressureNotified = m_impl->pressureSignal.exchange(false, std::memory_order_acq_rel);
        const bool demandNotified = std::exchange(state.memoryControlRequested, false);
        const bool memoryDue = memoryAdaptive && (now >= nextSampleAt || pressureNotified);
        if ((memoryAdaptive && (now >= nextSampleAt || pressureNotified)) || sampleCpu) {
            const bool sampleMemory = memoryAdaptive && (now >= nextSampleAt || pressureNotified);
            lock.unlock();
            if (sampleMemory) {
                try { sample = ProbeResources(); }
                catch (...) { sample = {}; sample.sampledAt = ResourceClock::now(); }
                m_impl->pressureMonitor->Observe(sample);
                nextSampleAt = ResourceClock::now() + resource_control::sampleInterval;
            }
            if (sampleCpu) {
                if (resetCpu) {
                    m_impl->cpuProbe->Reset();
                    cpuRequest = requestId;
                } else { cpuSample = m_impl->cpuProbe->Sample(); }
                nextCpuAt = ResourceClock::now() + cpu_control::sampleInterval;
            }
            now = ResourceClock::now();
            lock.lock();
            if (state.debug.closing) { return; }
            if (!state.debug.runActive || state.debug.runStopped || state.debug.requestId != requestId) { continue; }
        }
        const auto before = state.debug.eventEpoch;
        auto failure = memoryAdaptive && (memoryDue || demandNotified || state.debug.controlPhase != ResourceControlPhase::Normal)
            ? ApplyControlLocked(now, sample) : std::optional<CodecFailureRecord>{};
        if (!failure && sampleCpu && !resetCpu) { failure = ApplyCpuControlLocked(now, cpuSample); }
        const auto observed = state.debug.eventEpoch;
        lock.unlock();
        // 控制线程只回收纯自有空闲 scratch，宿主缓存留给 driver
        m_impl->scratch.TrimRetained();
        if (failure) { RecordFailure(*failure); }
        if (observed != before || failure || memoryDue) { state.changed.notify_all(); }
        lock.lock();
        const auto wakeAt = std::min(memoryAdaptive ? nextSampleAt : ResourceClock::time_point::max(),
            state.AutomaticCpu() ? nextCpuAt : ResourceClock::time_point::max());
        state.controllerChanged.wait_until(lock, wakeAt, [&] {
            return state.debug.closing || !state.debug.runActive || state.debug.runStopped ||
                state.debug.requestId != requestId ||
                state.memoryControlRequested ||
                (state.debug.controlPhase != ResourceControlPhase::Normal && state.debug.eventEpoch != observed) ||
                m_impl->pressureSignal.load(std::memory_order_acquire);
        });
    }
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::GrowWorkersLocked() noexcept {
    auto& state = *m_impl->state;
    const auto needed = std::min(state.EffectiveComputeLocked(), state.runningTasks + state.debug.queuedTasks);
    try {
        while (m_impl->workers.size() < needed) {
            const auto index = m_impl->workers.size();
            m_impl->workers.emplace_back([this, index] { WorkerMain(index); });
            state.debug.createdWorkers = m_impl->workers.size();
        }
    } catch (const std::bad_alloc&) {
        return ExecutionFailure("allocation-failed", "worker creation allocation failed");
    } catch (const std::exception& error) {
        return ExecutionFailure("thread-create-failed", error.what());
    } catch (...) {
        return ExecutionFailure("thread-create-failed", "worker creation failed");
    }
    return std::nullopt;
}

bool DataCodecExecutionResources::RecordFailure(const CodecFailureRecord& failure, bool closeRoot) noexcept {
    auto& state = *m_impl->state;
    bool first = false;
    std::stop_source stop(std::nostopstate);
    {
        std::lock_guard lock(state.mutex);
        first = state.FailLocked(failure, closeRoot);
        m_impl->scratch.SetRetainedCount(0u);
        stop = state.stopSource;
    }
    m_impl->scratch.Clear();
    stop.request_stop();
    state.changed.notify_all();
    return first;
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::FirstFailure() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->debug.failure;
}
bool DataCodecExecutionResources::Stopped() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->debug.runStopped || m_impl->state->debug.closing;
}
std::stop_token DataCodecExecutionResources::StopToken() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->stopSource.get_token();
}
void DataCodecExecutionResources::RequestStop() noexcept {
    RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure, "cancelled",
        "DataCodecExecutionResources", "request cancelled", true));
}

void DataCodecExecutionResources::CancelAndWaitRun() noexcept {
    auto& state = *m_impl->state;
    std::stop_source stop(std::nostopstate);
    bool invalidDriver = false;
    {
        std::lock_guard lock(state.mutex);
        invalidDriver = state.IsWorkerLocked() ||
            (state.debug.runActive && state.driver != std::this_thread::get_id());
        if (invalidDriver) {
            state.FailLocked(ExecutionFailure("invalid-drain-driver", "only the active driver may drain the request"), true);
        }
        state.debug.runStopped = true;
        FinishMemoryMeasurement(state.controller, ResourceClock::now(), ResourceDecisionReason::RequestCancelled);
        m_impl->scratch.SetRetainedCount(0u);
        state.Event(ResourceEventKind::Stop);
        stop = state.stopSource;
    }
    stop.request_stop();
    state.changed.notify_all();
    if (invalidDriver) { return; }
    m_impl->scratch.Clear();
    for (;;) {
        std::shared_ptr<TerminalWork> work;
        {
            std::lock_guard lock(state.mutex);
            if (state.debug.queuedTasks == 0u) { break; }
            work = state.PopLocked();
        }
        // 先释放闭包持有的输入和回调，再发布取消完成和归还延迟凭证
        work->m_function = {};
        {
            std::lock_guard lock(state.mutex);
            work->m_completion = BlockCompletion::Cancelled;
            auto& admission = state.admissions[work->m_admission];
            admission.completion = work->m_completion;
            admission.pendingWork = false;
            if (admission.releaseAfterWork) { state.ReleaseLocked(work->m_admission, work->m_generation); }
            ++state.debug.eventEpoch;
        }
        work.reset();
        state.changed.notify_all();
    }
    SetWaitReason(ResourceWaitReason::RunDrain);
    std::unique_lock lock(state.mutex);
    state.changed.wait(lock, [&] { return state.runningTasks == 0u; });
    lock.unlock();
    SetWaitReason(ResourceWaitReason::None);
}

void DataCodecExecutionResources::ShutdownAndJoin() noexcept {
    if (!m_impl) { return; }
    auto& state = *m_impl->state;
    bool worker = false;
    {
        std::lock_guard lock(state.mutex);
        worker = state.IsWorkerLocked();
        state.debug.closing = true;
        state.Event(ResourceEventKind::Close);
    }
    state.changed.notify_all();
    if (worker) {
        RecordFailure(ExecutionFailure("worker-self-join", "worker cannot join its own root"), true);
        return;
    }
    CancelAndWaitRun();
    if (m_impl->controllerThread.joinable()) { m_impl->controllerThread.join(); }
    m_impl->pressureMonitor.reset();
    for (auto& thread : m_impl->workers) { if (thread.joinable()) { thread.join(); } }
    m_impl->workers.clear();
    m_impl->inlineWorker.ReleaseCompressor();
    m_impl->scratch.Close();
    {
        std::lock_guard lock(state.mutex);
        state.debug.createdWorkers = 0u;
    }
}

std::uint64_t DataCodecExecutionResources::EventEpoch() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->debug.eventEpoch;
}
void DataCodecExecutionResources::WaitForChange(std::uint64_t observedEpoch, std::stop_token stop) {
    auto& state = *m_impl->state;
    {
        std::unique_lock lock(state.mutex);
        if (state.IsWorkerLocked()) {
            lock.unlock();
            RecordFailure(ExecutionFailure("worker-resource-wait", "terminal work cannot wait on the resource driver"), true);
            return;
        }
    }
    std::stop_callback wake(stop, [&] {
        {
            std::lock_guard lock(state.mutex);
            ++state.debug.eventEpoch;
        }
        state.changed.notify_all();
    });
    std::unique_lock lock(state.mutex);
    const auto changed = [&] {
        return state.debug.eventEpoch != observedEpoch || state.debug.runStopped || state.debug.closing || stop.stop_requested();
    };
    if (state.debug.byteWaiting) { state.changed.wait_for(lock, resource_control::sampleInterval, changed); }
    else { state.changed.wait(lock, changed); }
}
void DataCodecExecutionResources::SetWaitReason(ResourceWaitReason reason,
    const SlotLease* slot, const TerminalWork* work) noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    std::optional<std::size_t> index;
    if (state.AutomaticCpu() &&
        (reason == ResourceWaitReason::SlotCapacity || reason == ResourceWaitReason::PressureRecovery ||
         reason == ResourceWaitReason::TerminalWork) &&
        (!state.CpuAdmissionOpen() || (state.debug.queuedTasks != 0u && !state.cpuBudget.Available()))) {
        reason = ResourceWaitReason::CpuThrottle;
    }
    std::uint64_t generation = 0u;
    if (reason != ResourceWaitReason::None) {
        if (slot != nullptr && slot->m_state.get() == &state) {
            index = slot->m_index;
            generation = slot->m_generation;
        } else if (work != nullptr && work->m_completion != BlockCompletion::Pending) {
            index = work->m_admission;
            generation = work->m_generation;
        }
    }
    if (index && (*index >= state.admissions.size() || !state.admissions[*index].active ||
        state.admissions[*index].generation != generation)) { index.reset(); }
    if (state.debug.waiting == reason && state.waitingAdmission == index &&
        (!index || state.waitingGeneration == generation)) { return; }
    state.debug.waiting = reason;
    state.waitingAdmission = index;
    state.waitingGeneration = generation;
    state.debug.waitStarted = ResourceClock::now();
    auto& context = state.debug.waitContext;
    context = {};
    if (reason != ResourceWaitReason::None) {
        context.requestId = state.debug.requestId;
        context.flowId = state.debug.flowId;
    }
    switch (reason) {
    case ResourceWaitReason::None: break;
    case ResourceWaitReason::SlotCapacity: context.expected = ResourceWakeEvent::AdmissionAvailable; break;
    case ResourceWaitReason::ByteCapacity: context.expected = ResourceWakeEvent::AdmissionAvailable; break;
    case ResourceWaitReason::TerminalWork:
    case ResourceWaitReason::OrderedCommit: context.expected = ResourceWakeEvent::TerminalCompleted; break;
    case ResourceWaitReason::OwnerConsumption:
    case ResourceWaitReason::PressureDrain: context.expected = ResourceWakeEvent::OwnerRetired; break;
    case ResourceWaitReason::PressureRecovery: context.expected = ResourceWakeEvent::PressureSample; break;
    case ResourceWaitReason::InputIO: context.expected = ResourceWakeEvent::ReaderReturned; break;
    case ResourceWaitReason::OutputIO: context.expected = ResourceWakeEvent::WriterReturned; break;
    case ResourceWaitReason::RunDrain: context.expected = ResourceWakeEvent::RunDrained; break;
    case ResourceWaitReason::CpuThrottle: context.expected = ResourceWakeEvent::CpuSample; break;
    }
    if (index) {
        context.heavyPhase = *index == state.admissions.size() - 1u;
        if (!context.heavyPhase) { context.block = state.admissions[*index].sequence; }
        context.completion = state.admissions[*index].completion;
    }
    // 诊断事件不制造供 driver 重新检查的控制事件
    const auto epoch = state.debug.eventEpoch;
    state.Event(reason == ResourceWaitReason::None ? ResourceEventKind::Wake : ResourceEventKind::Wait);
    state.debug.eventEpoch = epoch;
}
bool DataCodecExecutionResources::TryCopyResourceDebugSnapshot(ResourceDebugSnapshot& output) const noexcept {
    auto& state = *m_impl->state;
    std::unique_lock lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { return false; }
    std::unique_lock capacityLock(state.capacity->m_state->mutex, std::try_to_lock);
    if (!capacityLock.owns_lock()) { return false; }
    std::unique_lock allocationLock(state.capacity->m_state->allocationMutex, std::try_to_lock);
    if (!allocationLock.owns_lock()) { return false; }
    output = state.debug;
    output.cpu = state.cpuController.Snapshot();
    output.effectiveComputeLimit = state.EffectiveComputeLocked();
    output.cpuPermitCredit = state.cpuBudget.Credit();
    output.memory = state.controller.memory;
    output.capturedAt = ResourceClock::now();
    if (output.memory.calibration == MemoryCalibrationResult::Measuring) {
        output.memory.calibrationElapsed = output.capturedAt - output.memory.calibrationStarted;
    }
    output.memory.measuredReservationBytes = state.controller.gainPhase >= 2u
        ? state.controller.measurementBytes : output.memory.measuredReservationBytes;
    output.allocatedStorage = state.capacity->m_state->allocated;
    output.allocatedStorage.capturedAt = output.capturedAt;
    output.workType = state.workType;
    if (state.waitingAdmission) {
        const auto& admission = state.admissions[*state.waitingAdmission];
        output.waitContext.completion = admission.active && admission.generation == state.waitingGeneration
            ? std::optional(admission.completion) : std::nullopt;
    }
    if (state.debug.queuedTasks != 0u) {
        const auto& work = *state.queue[state.queueHead];
        output.nextQueuedCompletion = work.m_completion;
        if (work.m_admission != state.admissions.size() - 1u) {
            output.nextQueuedBlock = state.admissions[work.m_admission].sequence;
        }
    }
    output.automaticObservationApplicable = state.configuration.mode == CodecResourceMode::Adaptive;
    if (output.automaticObservationApplicable) {
        const auto elapsed = output.capturedAt - state.waitsUpdatedAt;
        const auto& waits = state.observation.waits;
        output.observationWaits = {waits.compute + (state.waitCompute ? elapsed : ResourceClock::duration{}),
            waits.slotCapacity + (state.waitSlots ? elapsed : ResourceClock::duration{}),
            waits.input + (state.waitInput ? elapsed : ResourceClock::duration{}),
            waits.output + (state.waitOutput ? elapsed : ResourceClock::duration{})};
    }
    output.observationEpoch = state.observation.epoch;
    output.observationAdmitted = state.observation.admitted;
    output.observationRetired = state.observation.retiredAt.has_value();
    output.storage = {state.capacity->m_state->limitBytes, state.capacity->m_state->reservedBytes,
        state.capacity->m_state->peakReservedBytes};
    output.limits.ownedStorageLimitBytes = output.storage.limitBytes;
    return true;
}
void DataCodecExecutionResources::RecordCapacityRejection(
    const resource::CapacityRejection& rejection, const bool fatal, std::string_view origin) noexcept {
    std::optional<std::uint64_t> ceiling;
    {
        auto& state = *m_impl->state;
        std::lock_guard lock(state.mutex);
        ceiling = state.debug.storageCeilingBytes;
        // 终止性拒绝保留至请求交付，可选后端选择不得覆盖它
        if (!state.debug.capacityRejectionFatal) {
            state.debug.capacityRejection = rejection;
            state.debug.capacityRejectionRequestId = state.debug.requestId;
            state.debug.capacityRejectionFlowId = state.debug.flowId;
            state.debug.capacityRejectionFatal = fatal;
        }
        const auto epoch = state.debug.eventEpoch;
        state.Event(ResourceEventKind::CapacityRejected, rejection.requester.id);
        state.debug.eventEpoch = epoch;
    }
    if (fatal) {
        std::array<char, 160u> message{};
        if (ceiling) {
            std::snprintf(message.data(), message.size(),
                "required controlled execution capacity is unavailable; allowedCeilingBytes=%llu",
                static_cast<unsigned long long>(*ceiling));
        } else {
            std::snprintf(message.data(), message.size(), "unexpected capacity rejection in Unlimited mode");
        }
        auto failure = MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
            "capacity-admission-rejected", origin, message.data());
        failure.requestedBytes = rejection.requestedBytes;
        failure.reservedBytes = rejection.reservedBytes;
        failure.limitBytes = rejection.limitBytes;
        RecordFailure(failure);
    }
}
void DataCodecExecutionResources::RecordDiagnosticExportFailure() noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    m_impl->state->debug.diagnosticsIncomplete = true;
    ++m_impl->state->debug.diagnosticExportFailures;
}
std::shared_ptr<resource::ResidentByteBudget> DataCodecExecutionResources::StorageCapacity() const noexcept {
    return m_impl->state->capacity;
}
ScratchByteBufferPool& DataCodecExecutionResources::Scratch() noexcept { return m_impl->scratch; }
DecodeCacheRuntime& DataCodecExecutionResources::Caches() noexcept { return *m_impl->caches; }

void DataCodecExecutionResources::ServiceDriverEvents() {
    const auto epoch = TrimEpoch();
    if (epoch == m_impl->servicedTrimEpoch) { return; }
    m_impl->caches->TrimAll();
    m_impl->servicedTrimEpoch = epoch;
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    state.Event(ResourceEventKind::TrimCompleted);
}

void DataCodecExecutionResources::ReclaimOptionalStorage(const std::uint64_t requiredBytes) {
    ServiceDriverEvents();
    const auto enough = [&] {
        const auto current = StorageCapacity()->Snapshot();
        return current.CanReserve(requiredBytes);
    };
    if (!enough()) { m_impl->scratch.ClearFixed(); }
    while (!enough() && m_impl->caches->TrimOne()) {}
}
std::size_t DataCodecExecutionResources::Concurrency() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return std::max<std::size_t>(1u, m_impl->state->EffectiveComputeLocked());
}
bool DataCodecExecutionResources::Threaded() const noexcept { return m_impl->state->configuration.threaded; }
bool DataCodecExecutionResources::IsDriverThread() const noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    return state.debug.runActive && state.driver == std::this_thread::get_id() && !state.IsWorkerLocked();
}
bool DataCodecExecutionResources::ExternalSpillAvailable() const noexcept {
    return m_impl->state->configuration.externalSpillAvailable;
}
bool DataCodecExecutionResources::OptionalRetentionAllowed() const noexcept {
    auto& state = *m_impl->state;
    std::lock_guard lock(state.mutex);
    return !state.debug.optionalRetentionPausedByPressure && !state.debug.closing && !state.debug.runStopped &&
        state.capacity->Snapshot().limitBytes != 0u;
}
std::uint64_t DataCodecExecutionResources::TrimEpoch() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->debug.trimEpoch;
}

}
