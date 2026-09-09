#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Runtime/Cache/DecodeCacheRuntime.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
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
    const auto p = config.computeCeiling;
    const auto q = std::min<std::size_t>(p, 8u);
    return p != 0u && p <= std::numeric_limits<std::size_t>::max() / 2u - q - 1u &&
        limits.ownedStorageLimitBytes <= config.storageCeilingBytes &&
        limits.computeLimit >= 1u && limits.computeLimit <= p &&
        limits.slotLimit >= limits.computeLimit && limits.slotLimit <= p + q &&
        limits.slotLimit <= limits.computeLimit + q &&
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
    }

    void AccumulateWaits(ResourceClock::time_point now) noexcept {
        const auto elapsed = now - waitsUpdatedAt;
        if (waitCompute) { observation.waits.compute += elapsed; }
        if (waitSlots) { observation.waits.slots += elapsed; }
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
        waitOutput = debug.admittedBlocks != 0u &&
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
            moreIndependentBlocks || pendingHeavyPhase, debug.gateOpen,
            debug.runActive, debug.runStopped || debug.closing,
            debug.singleRecordFlow, debug.requestId, workType, observation};
    }

    void Event(ResourceEventKind kind, std::uint64_t sequence = 0u,
               ResourceDecisionReason reason = ResourceDecisionReason::NoChange,
               RuntimeResourceLimits before = {}) noexcept {
        const auto now = ResourceClock::now();
        AccumulateWaits(now);
        RefreshWaits();
        auto& event = debug.events[debug.nextEvent];
        event = ResourceEvent{kind, reason, now, debug.requestId,
            debug.flowId, sequence, before, debug.limits, debug.admittedBlocks, debug.activeComputeUnits};
        debug.nextEvent = (debug.nextEvent + 1u) % debug.events.size();
        if (debug.eventCount < debug.events.size()) { ++debug.eventCount; }
        else { ++debug.overwrittenEvents; }
        ++debug.eventEpoch;
    }

    bool FailLocked(const CodecFailureRecord& failure, bool close = false) noexcept {
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

    bool CanStartLocked() const noexcept {
        if (debug.runStopped || debug.closing || debug.queuedTasks == 0u || debug.exclusiveUnits != 0u) {
            return false;
        }
        const auto& work = queue[queueHead];
        return work->m_kind == TerminalWorkKind::ExclusivePackage
            ? debug.activeComputeUnits == 0u
            : debug.activeComputeUnits < debug.limits.computeLimit;
    }

    std::shared_ptr<TerminalWork> PopLocked() noexcept {
        auto work = std::move(queue[queueHead]);
        queueHead = (queueHead + 1u) % queue.size();
        --debug.queuedTasks;
        return work;
    }

    std::size_t StartLocked(TerminalWork& work) noexcept {
        const auto units = work.m_kind == TerminalWorkKind::ExclusivePackage && debug.limits.computeLimit >= 3u
            ? debug.limits.computeLimit : 1u;
        debug.activeComputeUnits += units;
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
    ObservationBatch observation;
    ResourceClock::time_point waitsUpdatedAt{ResourceClock::now()};
    bool moreIndependentBlocks{false};
    bool pendingHeavyPhase{false};
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
    m_impl->caches = std::make_unique<DecodeCacheRuntime>(*this);
    if (!UpdateLimits(config.initialLimits, config.gateOpen, ResourceDecisionReason::Initialize)) {
        throw std::invalid_argument("resource initialization failed");
    }
    if (config.mode == CodecResourceMode::Adaptive && config.threaded) {
        m_impl->pressureMonitor = std::make_unique<ResourcePressureMonitor>([](void* context) noexcept {
            auto& impl = *static_cast<Impl*>(context);
            impl.pressureSignal.store(true, std::memory_order_release);
            impl.state->changed.notify_all();
        }, m_impl.get());
        m_impl->pressureMonitor->Observe(config.initialSample);
        m_impl->controllerThread = std::thread([this] { ControlMain(); });
    }
}
DataCodecExecutionResources::~DataCodecExecutionResources() { ShutdownAndJoin(); }
DataCodecExecutionResources::DataCodecExecutionResources(const CodecResourceParams& params)
    : DataCodecExecutionResources(ResolveResourceConfiguration(params, ProbeResources())) {}

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
        state.moreIndependentBlocks = false;
        state.pendingHeavyPhase = false;
        state.ResetObservationLocked(ResourceClock::now());
        std::optional<CodecFailureRecord> controlFailure;
        if (state.configuration.mode == CodecResourceMode::Adaptive) {
            controlFailure = ApplyControlLocked(ResourceClock::now(), sample);
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
            if (state.configuration.mode == CodecResourceMode::Adaptive) {
                failure = ApplyControlLocked(ResourceClock::now(), state.debug.resourceSample);
            }
            state.Event(ResourceEventKind::Wake);
        }
    }
    m_impl->scratch.TrimRetained();
    if (failure) { RecordFailure(*failure); }
    state.changed.notify_all();
}

std::optional<SlotLease> DataCodecExecutionResources::TryAcquireSlot() {
    auto state = m_impl->state;
    std::unique_lock lock(state->mutex);
    auto& debug = state->debug;
    if (debug.runStopped || debug.closing) { return std::nullopt; }
    if (!debug.runActive || state->driver != std::this_thread::get_id() || state->IsWorkerLocked()) {
        lock.unlock();
        RecordFailure(ExecutionFailure("invalid-admission-driver", "block admission requires the active driver"), true);
        return std::nullopt;
    }
    if (!debug.gateOpen ||
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
        admission = ExecutionState::Admission{admission.generation + 1u,
            debug.lastAdmitted ? *debug.lastAdmitted + 1u : 0u, true};
        ++debug.admittedBlocks;
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
    if (!debug.gateOpen) {
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
    ParallelWorkerIndexScope workerIndex(index);
    {
        std::lock_guard lock(state->mutex);
        state->workerIds[index] = std::this_thread::get_id();
    }
    for (;;) {
        std::shared_ptr<TerminalWork> work;
        std::size_t units = 0u;
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&] { return state->debug.closing || state->CanStartLocked(); });
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
        state.Event(ResourceEventKind::Limits, 0u, reason, before);
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
    const auto decision = Advance(state.controller, now, sample, state.FlowLocked(now));
    state.debug.controlPhase = state.controller.phase;
    state.debug.pressurePending = state.controller.pressurePending;
    state.debug.signalValid = state.controller.signalValid;
    state.debug.cooldownUntil = state.controller.cooldownUntil;
    state.debug.holdSince = state.controller.holdSince;
    auto failure = PublishLimitsLocked(decision.limits, decision.gateOpen, decision.reason,
        decision.optionalRetentionPausedByPressure, decision.trimOptionalRetention, decision.resetObservation);
    if (failure) { return failure; }
    if (decision.failForSustainedPressure) {
        return ExecutionFailure("sustained-memory-pressure", "memory pressure did not recover within the request hold deadline");
    }
    return std::nullopt;
}

void DataCodecExecutionResources::ControlMain() noexcept {
    auto& state = *m_impl->state;
    auto nextSampleAt = ResourceClock::now();
    for (;;) {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] {
            return state.debug.closing || (state.debug.runActive && !state.debug.runStopped);
        });
        if (state.debug.closing) { return; }
        const auto requestId = state.debug.requestId;
        auto now = ResourceClock::now();
        ResourceSample sample = state.debug.resourceSample;
        if (now >= nextSampleAt || m_impl->pressureSignal.exchange(false, std::memory_order_acq_rel)) {
            lock.unlock();
            try { sample = ProbeResources(); }
            catch (...) {
                sample = {};
                sample.sampledAt = ResourceClock::now();
            }
            m_impl->pressureMonitor->Observe(sample);
            now = ResourceClock::now();
            nextSampleAt = now + resource_control::sampleInterval;
            lock.lock();
            if (state.debug.closing) { return; }
            if (!state.debug.runActive || state.debug.runStopped || state.debug.requestId != requestId) { continue; }
        }
        const auto failure = ApplyControlLocked(now, sample);
        const auto observed = state.debug.eventEpoch;
        lock.unlock();
        // 控制线程只回收纯自有空闲 scratch，宿主缓存留给 driver
        m_impl->scratch.TrimRetained();
        if (failure) { RecordFailure(*failure); }
        state.changed.notify_all();
        lock.lock();
        state.changed.wait_until(lock, nextSampleAt, [&] {
            return state.debug.closing || !state.debug.runActive || state.debug.runStopped ||
                state.debug.requestId != requestId || state.debug.eventEpoch != observed ||
                m_impl->pressureSignal.load(std::memory_order_acquire);
        });
    }
}

std::optional<CodecFailureRecord> DataCodecExecutionResources::GrowWorkersLocked() noexcept {
    auto& state = *m_impl->state;
    const auto needed = std::min(state.debug.limits.computeLimit, state.runningTasks + state.debug.queuedTasks);
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
    state.changed.wait(lock, [&] {
        return state.debug.eventEpoch != observedEpoch || state.debug.runStopped || state.debug.closing || stop.stop_requested();
    });
}
void DataCodecExecutionResources::SetWaitReason(ResourceWaitReason reason,
    const SlotLease* slot, const TerminalWork* work) noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    auto& state = *m_impl->state;
    std::optional<std::size_t> index;
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
    case ResourceWaitReason::TerminalWork:
    case ResourceWaitReason::OrderedCommit: context.expected = ResourceWakeEvent::TerminalCompleted; break;
    case ResourceWaitReason::OwnerConsumption:
    case ResourceWaitReason::PressureDrain: context.expected = ResourceWakeEvent::OwnerRetired; break;
    case ResourceWaitReason::PressureRecovery: context.expected = ResourceWakeEvent::PressureSample; break;
    case ResourceWaitReason::InputIO: context.expected = ResourceWakeEvent::ReaderReturned; break;
    case ResourceWaitReason::OutputIO: context.expected = ResourceWakeEvent::WriterReturned; break;
    case ResourceWaitReason::RunDrain: context.expected = ResourceWakeEvent::RunDrained; break;
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
    output.capturedAt = ResourceClock::now();
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
            waits.slots + (state.waitSlots ? elapsed : ResourceClock::duration{}),
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
    const resource::CapacityRejection& rejection, const bool fatal) noexcept {
    {
        auto& state = *m_impl->state;
        std::lock_guard lock(state.mutex);
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
        auto failure = MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
            "capacity-admission-rejected", "MemoryStore", "required owned storage capacity is unavailable");
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
        return requiredBytes == 0u || (current.reservedBytes <= current.limitBytes &&
            requiredBytes <= current.limitBytes - current.reservedBytes);
    };
    while (!enough() && m_impl->caches->TrimOne()) {}
}
std::size_t DataCodecExecutionResources::Concurrency() const noexcept {
    std::lock_guard lock(m_impl->state->mutex);
    return m_impl->state->debug.limits.computeLimit;
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
