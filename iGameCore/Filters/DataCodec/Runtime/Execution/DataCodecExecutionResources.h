#ifndef DATACODEC_RUNTIME_EXECUTION_DATACODECEXECUTIONRESOURCES_H
#define DATACODEC_RUNTIME_EXECUTION_DATACODECEXECUTIONRESOURCES_H

#include "DataCodec/API/Params/CodecResourceParams.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include "DataCodec/Platform/ResourceProbe.h"

namespace datacodec {

class ScratchByteBufferPool;
class DecodeCacheRuntime;
class WorkerContext;
struct ExecutionState;
struct ControlDecision;

class IParallelTaskGroup {
public:
    virtual ~IParallelTaskGroup() = default;
    virtual void Submit(std::function<void()> task) = 0;
    virtual void Wait() = 0;
};

class IParallelTaskRunner {
public:
    virtual ~IParallelTaskRunner() = default;
    virtual std::unique_ptr<IParallelTaskGroup> CreateGroup(std::stop_token stop = {}) = 0;
    virtual std::size_t Concurrency() const noexcept = 0;
};

using ResourceClock = std::chrono::steady_clock;

struct RuntimeResourceLimits {
    std::uint64_t ownedStorageLimitBytes{0u};
    std::size_t computeLimit{1u};
    std::size_t slotLimit{1u};
    bool operator==(const RuntimeResourceLimits&) const = default;
};

// 平台和模式层解析一次，执行机制只接受确定目标
struct ResolvedResourceConfiguration {
    RuntimeResourceLimits initialLimits;
    std::uint64_t storageCeilingBytes{0u};
    std::size_t computeCeiling{1u};
    bool threaded{true};
    bool gateOpen{true};
    bool externalSpillAvailable{false};
    CodecResourceMode mode{CodecResourceMode::Fixed};
    ResourceSample initialSample;
};

enum class ResourceDecisionReason : std::uint8_t {
    NoChange, Initialize, BeginRequest, MechanismCheck, PressurePending,
    PressureConfirmed, PressureEscalated, Drained, PressureCleared,
    SignalLost, SignalRestored, HardCapabilityChanged, WorkTypeChanged,
    StorageGrowth, ComputeGrowth, SlotGrowth, RetentionRestored, PressureTimeout,
};

enum class ResourceEventKind : std::uint8_t {
    BeginRequest, EndRequest, Limits, Admit, Complete, Commit, Retire,
    Wait, Wake, TrimRequested, TrimCompleted, CapacityRejected, Failure, Stop, Close,
};

enum class ResourceWaitReason : std::uint8_t {
    None, SlotCapacity, TerminalWork, OrderedCommit, OwnerConsumption,
    PressureDrain, PressureRecovery, InputIO, OutputIO, RunDrain,
};

enum class BlockCompletion : std::uint8_t { Pending, Queued, Running, Succeeded, Failed, Cancelled };
enum class ResourceWakeEvent : std::uint8_t {
    None, AdmissionAvailable, TerminalCompleted, OwnerRetired, PressureSample,
    ReaderReturned, WriterReturned, RunDrained,
};
struct ResourceWaitContext {
    ResourceWakeEvent expected{ResourceWakeEvent::None};
    std::uint64_t requestId{0u};
    std::uint64_t flowId{0u};
    std::optional<std::uint64_t> block;
    bool heavyPhase{false};
    std::optional<BlockCompletion> completion;
};
enum class TerminalWorkKind : std::uint8_t { Ordinary, ExclusivePackage };
enum class ResourceControlPhase : std::uint8_t { Normal, Drain, Hold };
enum class ResourceWorkPath : std::uint8_t {
    None, NumericEncode, ReferenceEncode, GeometryDecode, AttributeDecode,
    ConnectivityEncode, ConnectivityDecode, PolyhedronEncode, PolyhedronEmit,
};

struct ResourceWorkType {
    ResourceWorkPath path{ResourceWorkPath::None};
    std::uint32_t codec{0u};
    std::uint32_t scalar{0u};
    std::uint32_t components{0u};
    std::uint64_t blockElements{0u};
    std::uint32_t referencePath{0u};
    std::uint32_t libraryThreads{1u};
    std::uint32_t componentCodecMask{0u};
    bool operator==(const ResourceWorkType&) const = default;
};

struct ResourceEvent {
    ResourceEventKind kind{};
    ResourceDecisionReason reason{};
    ResourceClock::time_point at{};
    std::uint64_t requestId{0u};
    std::uint64_t flowId{0u};
    std::uint64_t sequence{0u};
    RuntimeResourceLimits before;
    RuntimeResourceLimits after;
    std::size_t admitted{0u};
    std::size_t computing{0u};
};
static_assert(sizeof(ResourceEvent) <= 256u);

struct ResourceDebugSnapshot {
    ResourceClock::time_point capturedAt{};
    RuntimeResourceLimits limits;
    resource::CapacitySnapshot storage;
    resource::StorageAllocationSnapshot allocatedStorage;
    std::uint64_t storageCeilingBytes{0u};
    std::size_t computeCeiling{1u};
    std::size_t admittedBlocks{0u};
    std::size_t activeComputeUnits{0u};
    std::size_t queuedTasks{0u};
    std::size_t createdWorkers{0u};
    std::size_t exclusiveUnits{0u};
    bool heavyPhaseAdmitted{false};
    bool singleRecordFlow{false};
    bool gateOpen{false};
    bool runStopped{false};
    bool closing{false};
    bool runActive{false};
    bool optionalRetentionPausedByPressure{false};
    std::uint64_t requestId{0u};
    std::uint64_t flowId{0u};
    std::uint64_t eventEpoch{0u};
    std::uint64_t targetEpoch{0u};
    std::uint64_t trimEpoch{0u};
    std::uint64_t secondaryFailures{0u};
    bool diagnosticsIncomplete{false};
    std::uint64_t diagnosticExportFailures{0u};
    ResourceWaitReason waiting{ResourceWaitReason::None};
    ResourceClock::time_point waitStarted{};
    ResourceWaitContext waitContext;
    ResourceWorkType workType;
    std::optional<std::uint64_t> nextQueuedBlock;
    std::optional<BlockCompletion> nextQueuedCompletion;
    std::optional<std::uint64_t> lastAdmitted;
    std::optional<std::uint64_t> maxCompleted;
    std::optional<std::uint64_t> lastCommitted;
    std::optional<std::uint64_t> lastRetired;
    std::optional<CodecFailureRecord> failure;
    std::optional<resource::CapacityRejection> capacityRejection;
    std::uint64_t capacityRejectionRequestId{0u};
    std::uint64_t capacityRejectionFlowId{0u};
    bool capacityRejectionFatal{false};
    std::array<ResourceEvent, 128u> events{};
    std::size_t eventCount{0u};
    std::size_t nextEvent{0u};
    std::uint64_t overwrittenEvents{0u};
    CodecResourceMode mode{CodecResourceMode::Fixed};
    ResourceControlPhase controlPhase{ResourceControlPhase::Normal};
    ResourceSample resourceSample;
    bool pressurePending{false};
    bool signalValid{false};
    ResourceClock::time_point cooldownUntil{};
    std::optional<ResourceClock::time_point> holdSince;
    std::uint64_t observationEpoch{0u};
    std::size_t observationAdmitted{0u};
    bool observationRetired{false};
    bool automaticObservationApplicable{false};
    std::array<ResourceClock::duration, 4u> observationWaits{};
};
static_assert(sizeof(ResourceDebugSnapshot) <= 64u * 1024u);

class SlotLease final {
public:
    SlotLease() = default;
    SlotLease(const SlotLease&) = delete;
    SlotLease& operator=(const SlotLease&) = delete;
    SlotLease(SlotLease&&) noexcept;
    SlotLease& operator=(SlotLease&&) noexcept;
    ~SlotLease();
    void Reset() noexcept;
    explicit operator bool() const noexcept { return m_state != nullptr; }
private:
    friend class DataCodecExecutionResources;
    SlotLease(std::shared_ptr<ExecutionState>, std::size_t, std::uint64_t) noexcept;
    std::shared_ptr<ExecutionState> m_state;
    std::size_t m_index{0u};
    std::uint64_t m_generation{0u};
};

class HeavyPhaseLease final {
public:
    HeavyPhaseLease() = default;
    HeavyPhaseLease(const HeavyPhaseLease&) = delete;
    HeavyPhaseLease& operator=(const HeavyPhaseLease&) = delete;
    HeavyPhaseLease(HeavyPhaseLease&&) noexcept;
    HeavyPhaseLease& operator=(HeavyPhaseLease&&) noexcept;
    ~HeavyPhaseLease();
    void Reset() noexcept;
    explicit operator bool() const noexcept { return static_cast<bool>(m_admission); }
private:
    friend class DataCodecExecutionResources;
    explicit HeavyPhaseLease(SlotLease admission) noexcept : m_admission(std::move(admission)) {}
    SlotLease m_admission;
};

// 完成状态只在根执行锁内发布与读取，任务闭包在该锁外销毁
class TerminalWork final {
public:
    using Function = std::function<bool(WorkerContext&)>;
    explicit TerminalWork(Function function, TerminalWorkKind kind = TerminalWorkKind::Ordinary)
        : m_function(std::move(function)), m_kind(kind) {}
private:
    friend class DataCodecExecutionResources;
    friend struct ExecutionState;
    Function m_function;
    TerminalWorkKind m_kind;
    BlockCompletion m_completion{BlockCompletion::Pending};
    std::size_t m_admission{0u};
    std::uint64_t m_generation{0u};
};

class DataCodecExecutionResources final : public IParallelTaskRunner {
public:
    explicit DataCodecExecutionResources(const ResolvedResourceConfiguration&);
    explicit DataCodecExecutionResources(const CodecResourceParams&);
    DataCodecExecutionResources(const DataCodecExecutionResources&) = delete;
    DataCodecExecutionResources& operator=(const DataCodecExecutionResources&) = delete;
    ~DataCodecExecutionResources();
    std::unique_ptr<IParallelTaskGroup> CreateGroup(std::stop_token stop = {}) override;
    std::size_t Concurrency() const noexcept override;
    bool Threaded() const noexcept;
    bool IsDriverThread() const noexcept;
    bool ExternalSpillAvailable() const noexcept;

    bool BeginRun() noexcept;
    bool EndRun() noexcept;
    bool BeginFlow(bool singleRecord = false) noexcept;
    void SetFlowProgress(bool moreIndependentBlocks) noexcept;
    void SetWorkType(const ResourceWorkType&) noexcept;
    std::optional<SlotLease> TryAcquireSlot();
    std::optional<HeavyPhaseLease> TryAcquireHeavyPhase();
    bool SubmitTerminal(const SlotLease&, const std::shared_ptr<TerminalWork>&) noexcept;
    bool SubmitTerminal(const HeavyPhaseLease&, const std::shared_ptr<TerminalWork>&) noexcept;
    BlockCompletion Completion(const TerminalWork&) const noexcept;
    bool CommitSlot(const SlotLease&) noexcept;

    bool UpdateLimits(const RuntimeResourceLimits&, bool gateOpen,
                      ResourceDecisionReason, std::string* error = nullptr) noexcept;
    bool RecordFailure(const CodecFailureRecord&, bool closeRoot = false) noexcept;
    std::optional<CodecFailureRecord> FirstFailure() const noexcept;
    bool Stopped() const noexcept;
    std::stop_token StopToken() const noexcept;
    void RequestStop() noexcept;
    void CancelAndWaitRun() noexcept;
    void ShutdownAndJoin() noexcept;

    std::uint64_t EventEpoch() const noexcept;
    void WaitForChange(std::uint64_t observedEpoch, std::stop_token stop = {});
    void SetWaitReason(ResourceWaitReason, const SlotLease* slot = nullptr,
                       const TerminalWork* work = nullptr) noexcept;
    bool TryCopyResourceDebugSnapshot(ResourceDebugSnapshot&) const noexcept;
    void RecordCapacityRejection(const resource::CapacityRejection&, bool fatal) noexcept;
    void RecordDiagnosticExportFailure() noexcept;
    std::shared_ptr<resource::ResidentByteBudget> StorageCapacity() const noexcept;
    ScratchByteBufferPool& Scratch() noexcept;
    DecodeCacheRuntime& Caches() noexcept;
    void ServiceDriverEvents();
    void ReclaimOptionalStorage(std::uint64_t requiredBytes);
    bool OptionalRetentionAllowed() const noexcept;
    std::uint64_t TrimEpoch() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool Submit(const SlotLease&, const std::shared_ptr<TerminalWork>&) noexcept;
    void WorkerMain(std::size_t) noexcept;
    std::optional<CodecFailureRecord> GrowWorkersLocked() noexcept;
    std::optional<CodecFailureRecord> PublishLimitsLocked(const RuntimeResourceLimits&, bool,
        ResourceDecisionReason, std::optional<bool> retentionPaused = {}, bool trim = false,
        bool resetObservation = false) noexcept;
    std::optional<CodecFailureRecord> ApplyControlLocked(ResourceClock::time_point,
                                                        const ResourceSample&) noexcept;
    void ControlMain() noexcept;
};

// workspace 只在本次运行中访问执行根，留存数据不延长执行设施寿命
template<class Workspace>
class RunBinding final {
public:
    RunBinding(Workspace& workspace, DataCodecExecutionResources& run) : m_workspace(workspace) {
        m_workspace.BindRun(run);
    }
    RunBinding(const RunBinding&) = delete;
    RunBinding& operator=(const RunBinding&) = delete;
    ~RunBinding() { m_workspace.UnbindRun(); }
private:
    Workspace& m_workspace;
};

// 一条顶层命令的作用域，内部帧和叶复用已经建立的请求
class CodecRunScope final {
public:
    explicit CodecRunScope(DataCodecExecutionResources& run) noexcept
        : m_run(run), m_active(run.BeginRun()) {}
    CodecRunScope(const CodecRunScope&) = delete;
    CodecRunScope& operator=(const CodecRunScope&) = delete;
    ~CodecRunScope() {
        if (m_active) {
            m_run.CancelAndWaitRun();
            m_run.EndRun();
        }
    }
    explicit operator bool() const noexcept { return m_active; }
    bool Finish(bool success) noexcept {
        if (!m_active) { return false; }
        if (!success || m_run.Stopped()) { m_run.CancelAndWaitRun(); }
        m_active = false;
        return m_run.EndRun() && success && !m_run.Stopped();
    }
private:
    DataCodecExecutionResources& m_run;
    bool m_active;
};

}

#endif
