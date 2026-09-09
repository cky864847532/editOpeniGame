#ifndef DATACODEC_RUNTIME_EXECUTION_PARALLELEXECUTION_H
#define DATACODEC_RUNTIME_EXECUTION_PARALLELEXECUTION_H

#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include <algorithm>
#include <deque>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace datacodec {

namespace numericarray { class NumericArrayCompressorState; }

class WorkerContext final {
public:
    WorkerContext(ScratchByteBufferPool& scratch, std::size_t index) noexcept;
    ~WorkerContext();
    WorkerContext(const WorkerContext&) = delete;
    WorkerContext& operator=(const WorkerContext&) = delete;
    numericarray::NumericArrayCompressorState& NumericCompressor();
    void ReleaseCompressor() noexcept;
    std::stop_token StopToken() const noexcept { return m_stop; }
    ScratchByteBufferPool& Scratch() noexcept { return m_scratch; }
    std::size_t ComputeUnits() const noexcept { return m_units; }
    std::size_t Index() const noexcept { return m_index; }
private:
    friend class DataCodecExecutionResources;
    ScratchByteBufferPool& m_scratch;
    std::size_t m_index;
    std::size_t m_units{1u};
    std::stop_token m_stop;
    std::unique_ptr<numericarray::NumericArrayCompressorState> m_compressor;
};

bool RunTerminalWork(DataCodecExecutionResources&, const SlotLease&,
                     TerminalWork::Function,
                     TerminalWorkKind kind = TerminalWorkKind::Ordinary) noexcept;
bool RunTerminalWork(DataCodecExecutionResources&, const HeavyPhaseLease&,
                     TerminalWork::Function,
                     TerminalWorkKind kind = TerminalWorkKind::Ordinary) noexcept;

inline std::optional<HeavyPhaseLease> WaitForHeavyPhase(DataCodecExecutionResources& run) {
    for (;;) {
        run.ServiceDriverEvents();
        const auto observed = run.EventEpoch();
        if (run.Stopped()) { return std::nullopt; }
        if (auto phase = run.TryAcquireHeavyPhase()) {
            run.SetWaitReason(ResourceWaitReason::None);
            return phase;
        }
        if (run.Stopped()) { return std::nullopt; }
        run.SetWaitReason(ResourceWaitReason::PressureRecovery);
        run.WaitForChange(observed);
    }
}

template<class Input, class Output>
struct BlockRecord {
    SlotLease slot;
    std::optional<Input> input;
    std::optional<Output> output;
    std::shared_ptr<TerminalWork> work;

    void Retire() noexcept {
        work.reset();
        output.reset();
        input.reset();
        slot.Reset();
    }
};

// 作用域先收束任务，再析构仍被终端函数访问的块记录
class RunDrainGuard final {
public:
    explicit RunDrainGuard(DataCodecExecutionResources& run) noexcept : m_run(run) {}
    ~RunDrainGuard() { if (!m_complete) { m_run.CancelAndWaitRun(); } }
    void Complete() noexcept { m_complete = true; }
private:
    DataCodecExecutionResources& m_run;
    bool m_complete{false};
};

// 只在 catch 中调用，首错发布必须先于任务收束和块记录析构
void RecordExecutionException(DataCodecExecutionResources&, std::string_view origin) noexcept;

template<class Input, class Output, class More, class Read, class Compute, class Commit,
         class DescribeWork = std::nullptr_t>
bool RunOrderedBlocks(DataCodecExecutionResources& run, More&& more, Read&& read,
                      Compute&& compute, Commit&& commit, const bool singleRecord = false,
                      DescribeWork&& describeWork = nullptr) noexcept {
    try {
        std::deque<std::shared_ptr<BlockRecord<Input, Output>>> records;
        std::optional<ResourceWorkType> activeWorkType;
        std::optional<ResourceWorkType> nextWorkType;
        RunDrainGuard drain(run);
        try {
        if (!run.BeginFlow(singleRecord)) { return false; }
        while (more() || !records.empty()) {
            const bool hasMore = more();
            if constexpr (!std::is_same_v<std::remove_cvref_t<DescribeWork>, std::nullptr_t>) {
                // 单记录流的下一块游标由提交推进，提交前不读取其类型
                if (hasMore && !nextWorkType && (!singleRecord || records.empty())) {
                    nextWorkType = describeWork();
                }
            }
            const bool changingType = nextWorkType && (!activeWorkType || *nextWorkType != *activeWorkType);
            // 后继类型已有元数据，排空期间不把它计为当前类型的独立供给
            run.SetFlowProgress(hasMore && (!changingType || records.empty()));
            run.ServiceDriverEvents();
            const auto observed = run.EventEpoch();
            if (run.Stopped()) { return false; }
            bool progressed = false;
            if (!records.empty() &&
                run.Completion(*records.front()->work) == BlockCompletion::Succeeded) {
                auto& record = *records.front();
                // 最后一块的 End/Seal 由提交函数完成，完成后才归还槽位
                run.SetWaitReason(ResourceWaitReason::OutputIO, &record.slot);
                if (!commit(*record.output) || !run.CommitSlot(record.slot)) {
                    run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                        "block-commit-failed", "RunOrderedBlocks", "block commit failed"));
                    return false;
                }
                record.Retire();
                run.SetWaitReason(ResourceWaitReason::None);
                records.pop_front();
                // 提交可能推进供给游标，下一轮重新读取供给状态
                // 已从独立读取游标识别的下一块类型保留到实际读入
                continue;
            }
            if (hasMore && (!changingType || records.empty())) {
                if (changingType) {
                    run.SetWorkType(*nextWorkType);
                    if (run.Stopped()) { return false; }
                    activeWorkType = nextWorkType;
                    run.SetFlowProgress(true);
                }
                if (auto slot = run.TryAcquireSlot()) {
                    auto record = std::make_shared<BlockRecord<Input, Output>>();
                    record->slot = std::move(*slot);
                    record->input.emplace();
                    record->output.emplace();
                    run.SetWaitReason(ResourceWaitReason::InputIO, &record->slot);
                    const bool readSucceeded = [&] {
                        if constexpr (std::is_invocable_r_v<bool, Read&, Input&, const SlotLease&>) {
                            return read(*record->input, record->slot);
                        } else {
                            return read(*record->input);
                        }
                    }();
                    run.SetWaitReason(ResourceWaitReason::None);
                    if (!readSucceeded) {
                        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                            "block-read-failed", "RunOrderedBlocks", "block read failed"));
                        return false;
                    }
                    nextWorkType.reset();
                    auto* current = record.get();
                    record->work = std::make_shared<TerminalWork>([current, &compute](WorkerContext& worker) {
                        return compute(*current->input, *current->output, worker);
                    });
                    // 先将记录放入受 guard 保护的窗口，再发布终端任务
                    records.push_back(std::move(record));
                    if (!run.SubmitTerminal(records.back()->slot, records.back()->work)) { return false; }
                    progressed = true;
                }
            }
            if (!progressed) {
                run.SetWaitReason(records.empty() ? ResourceWaitReason::SlotCapacity : ResourceWaitReason::OrderedCommit,
                    records.empty() ? nullptr : &records.front()->slot);
                run.WaitForChange(observed);
                run.SetWaitReason(ResourceWaitReason::None);
            }
        }
        run.SetFlowProgress(false);
        drain.Complete();
        return !run.Stopped();
        } catch (...) {
            RecordExecutionException(run, "RunOrderedBlocks");
        }
    } catch (...) {
        RecordExecutionException(run, "RunOrderedBlocks");
    }
    return false;
}

inline constexpr std::size_t kInvalidParallelWorkerIndex = std::numeric_limits<std::size_t>::max();
inline thread_local std::size_t g_currentParallelWorkerIndex = kInvalidParallelWorkerIndex;

inline std::size_t CurrentParallelWorkerIndex() noexcept {
    return g_currentParallelWorkerIndex;
}

class ParallelWorkerIndexScope final {
public:
    explicit ParallelWorkerIndexScope(const std::size_t workerIndex) noexcept
        : m_previous(g_currentParallelWorkerIndex) {
        g_currentParallelWorkerIndex = workerIndex;
    }

    ParallelWorkerIndexScope(const ParallelWorkerIndexScope&) = delete;
    ParallelWorkerIndexScope& operator=(const ParallelWorkerIndexScope&) = delete;

    ~ParallelWorkerIndexScope() {
        g_currentParallelWorkerIndex = m_previous;
    }

private:
    std::size_t m_previous{kInvalidParallelWorkerIndex};
};

inline std::size_t ClampDataCodecWorkerCount(std::size_t workerCount) noexcept {
    workerCount = std::max<std::size_t>(workerCount, 1u);
#if defined(DATACODEC_MAX_PARALLEL_WORKERS) && DATACODEC_MAX_PARALLEL_WORKERS > 0
    workerCount = std::min<std::size_t>(
        workerCount,
        static_cast<std::size_t>(DATACODEC_MAX_PARALLEL_WORKERS));
#endif
    return workerCount;
}

class InlineParallelTaskGroup final : public IParallelTaskGroup {
public:
    explicit InlineParallelTaskGroup(std::stop_token = {}) {}

    void Submit(std::function<void()> task) override {
        if (!task) {
            return;
        }
        ParallelWorkerIndexScope workerIndexScope(0u);
        task();
    }

    void Wait() override {}
};

class InlineParallelTaskRunner final : public IParallelTaskRunner {
public:
    [[nodiscard]] std::unique_ptr<IParallelTaskGroup> CreateGroup(
        const std::stop_token stopToken = {}) override {
        return std::make_unique<InlineParallelTaskGroup>(stopToken);
    }

    [[nodiscard]] std::size_t Concurrency() const noexcept override { return 1u; }
};

inline std::size_t ResolveParallelTaskCount(
    const std::size_t taskCount,
    const IParallelTaskRunner* runner,
    const std::size_t workerLimit = 0u) noexcept {
    if (taskCount == 0u || runner == nullptr || CurrentParallelWorkerIndex() != kInvalidParallelWorkerIndex) {
        return taskCount == 0u ? 0u : 1u;
    }
    auto concurrency = ClampDataCodecWorkerCount(runner->Concurrency());
    if (workerLimit != 0u) {
        concurrency = std::min(concurrency, ClampDataCodecWorkerCount(workerLimit));
    }
    return std::min(taskCount, concurrency);
}

inline bool ShouldParallelizeRange(
    const std::size_t taskCount,
    const std::size_t minTaskCount,
    const IParallelTaskRunner* runner,
    const std::size_t workerLimit = 0u) noexcept {
    return taskCount >= minTaskCount &&
        ResolveParallelTaskCount(taskCount, runner, workerLimit) > 1u;
}

template<typename Func>
inline void ParallelForChunks(
    const std::size_t begin,
    const std::size_t end,
    Func&& process,
    IParallelTaskRunner* runner = nullptr,
    const std::size_t workerLimit = 0u,
    const std::stop_token stopToken = {}) {
    if (end <= begin || stopToken.stop_requested()) {
        return;
    }

    const auto taskCount = end - begin;
    const auto resolvedWorkerCount = ResolveParallelTaskCount(taskCount, runner, workerLimit);
    if (resolvedWorkerCount <= 1u) {
        process(begin, end);
        return;
    }

    const auto chunkSize = (taskCount + resolvedWorkerCount - 1u) / resolvedWorkerCount;
    auto group = runner->CreateGroup(stopToken);
    if (group == nullptr) {
        throw std::runtime_error("parallel task runner returned an unavailable task group");
    }
    for (std::size_t chunkBegin = begin; chunkBegin < end; chunkBegin += chunkSize) {
        const auto chunkEnd = std::min(end, chunkBegin + chunkSize);
        group->Submit([chunkBegin, chunkEnd, &process, stopToken]() {
            if (!stopToken.stop_requested()) {
                process(chunkBegin, chunkEnd);
            }
        });
    }
    group->Wait();
}

template<typename Func>
inline void ParallelForChunksThreshold(
    const std::size_t begin,
    const std::size_t end,
    const std::size_t minTaskCount,
    Func&& process,
    IParallelTaskRunner* runner = nullptr,
    const std::size_t workerLimit = 0u,
    const std::stop_token stopToken = {}) {
    if (end <= begin || stopToken.stop_requested()) {
        return;
    }

    if (!ShouldParallelizeRange(end - begin, minTaskCount, runner, workerLimit)) {
        process(begin, end);
        return;
    }

    ParallelForChunks(
        begin,
        end,
        std::forward<Func>(process),
        runner,
        workerLimit,
        stopToken);
}

} // namespace datacodec

#endif
