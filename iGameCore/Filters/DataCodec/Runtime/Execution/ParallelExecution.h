#ifndef DATACODEC_RUNTIME_EXECUTION_PARALLELEXECUTION_H
#define DATACODEC_RUNTIME_EXECUTION_PARALLELEXECUTION_H

#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Runtime/Execution/DecodeBlockWorkspace.h"

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

// 临时块响应调查入口，仅由测试 driver 绑定，空指针时不采集
struct BlockResponseInvestigation {
    void* context{};
    void (*callback)(void*, DataCodecExecutionResources&, const char*,
                 const DecodeBlockMemoryPlan&, std::uint64_t) noexcept{};
    void Record(DataCodecExecutionResources& run, const char* point,
                const DecodeBlockWorkspace* workspace, std::uint64_t missing = 0u) const noexcept {
        if (callback && workspace) { callback(context, run, point, workspace->Plan(), missing); }
    }
};
inline thread_local BlockResponseInvestigation blockResponseInvestigation;

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
    std::unique_ptr<DecodeBlockWorkspace> workspace;
    std::optional<Input> input;
    std::optional<Output> output;
    std::shared_ptr<TerminalWork> work;
    std::uint64_t investigationMissing{};

    void Retire() noexcept {
        work.reset();
        output.reset();
        input.reset();
        workspace.reset();
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
         class DescribeWork = std::nullptr_t, class DescribeMemory = std::nullptr_t>
bool RunOrderedBlocks(DataCodecExecutionResources& run, More&& more, Read&& read,
                      Compute&& compute, Commit&& commit, const bool singleRecord = false,
                      DescribeWork&& describeWork = nullptr, DescribeMemory&& describeMemory = nullptr) noexcept {
    try {
        const auto investigation = blockResponseInvestigation;
        std::deque<std::shared_ptr<BlockRecord<Input, Output>>> records;
        std::optional<ResourceWorkType> activeWorkType;
        std::optional<ResourceWorkType> nextWorkType;
        std::unique_ptr<DecodeBlockWorkspace> nextWorkspace;
        std::uint64_t nextBytes = 0u;
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
                investigation.Record(run, "commit.begin", record.workspace.get(), record.investigationMissing);
                // 最后一块的 End/Seal 由提交函数完成，完成后才归还槽位
                run.SetWaitReason(ResourceWaitReason::OutputIO, &record.slot);
                const bool committed = [&] {
                    if constexpr (std::is_invocable_r_v<bool, Commit&, Output&, DecodeBlockWorkspace&>) {
                        return commit(*record.output, *record.workspace);
                    } else { return commit(*record.output); }
                }();
                if (!committed || !run.CommitSlot(record.slot)) {
                    run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                        "block-commit-failed", "RunOrderedBlocks", "block commit failed"));
                    return false;
                }
                investigation.Record(run, "commit.end", record.workspace.get(), record.investigationMissing);
                const auto retiredPlan = investigation.callback && record.workspace ?
                    record.workspace->Plan() : DecodeBlockMemoryPlan{};
                const auto retiredMissing = record.investigationMissing;
                record.Retire();
                if (investigation.callback) { investigation.callback(investigation.context, run, "retired", retiredPlan, retiredMissing); }
                run.SetWaitReason(ResourceWaitReason::None);
                records.pop_front();
                // 提交可能推进供给游标，下一轮重新读取供给状态
                // 已从独立读取游标识别的下一块类型保留到实际读入
                continue;
            }
            if (hasMore && (!changingType || records.empty()) && (!singleRecord || records.empty())) {
                if (changingType) {
                    run.SetWorkType(*nextWorkType);
                    if (run.Stopped()) { return false; }
                    activeWorkType = nextWorkType;
                    run.SetFlowProgress(true);
                }
                if constexpr (!std::is_same_v<std::remove_cvref_t<DescribeMemory>, std::nullptr_t>) {
                    if (!nextWorkspace) {
                        auto plan = describeMemory();
                        nextWorkspace = std::make_unique<DecodeBlockWorkspace>(run.Scratch(), plan);
                        nextBytes = nextWorkspace->TakeReusable();
                        investigation.Record(run, "plan", nextWorkspace.get(), nextBytes);
                    }
                    run.ReclaimOptionalStorage(nextBytes);
                    if (!run.CheckNecessaryCapacity(nextBytes)) { return false; }
                }
                resource::ResidentByteBudget::Lease reservation;
                investigation.Record(run, "admission.try", nextWorkspace.get(), nextBytes);
                if (auto slot = run.TryAcquireSlot(nextBytes, reservation)) {
                    auto record = std::make_shared<BlockRecord<Input, Output>>();
                    record->slot = std::move(*slot);
                    record->investigationMissing = nextBytes;
                    if (nextWorkspace) {
                        nextWorkspace->Allocate(*run.StorageCapacity(), std::move(reservation));
                        record->workspace = std::move(nextWorkspace);
                    }
                    investigation.Record(run, "allocated", record->workspace.get(), record->investigationMissing);
                    record->input.emplace();
                    record->output.emplace();
                    run.SetWaitReason(ResourceWaitReason::InputIO, &record->slot);
                    const bool readSucceeded = [&] {
                        if constexpr (std::is_invocable_r_v<bool, Read&, Input&, const SlotLease&, DecodeBlockWorkspace&>) {
                            return read(*record->input, record->slot, *record->workspace);
                        } else if constexpr (std::is_invocable_r_v<bool, Read&, Input&, const SlotLease&>) {
                            return read(*record->input, record->slot);
                        } else {
                            return read(*record->input);
                        }
                    }();
                    run.SetWaitReason(ResourceWaitReason::None);
                    investigation.Record(run, "read.end", record->workspace.get(), record->investigationMissing);
                    if (!readSucceeded) {
                        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                            "block-read-failed", "RunOrderedBlocks", "block read failed"));
                        return false;
                    }
                    nextWorkType.reset();
                    auto* current = record.get();
                    record->work = std::make_shared<TerminalWork>([current, &compute, &run, investigation](WorkerContext& worker) {
                        investigation.Record(run, "compute.begin", current->workspace.get(), current->investigationMissing);
                        const bool success = [&] {
                            if constexpr (std::is_invocable_r_v<bool, Compute&, Input&, Output&, WorkerContext&, DecodeBlockWorkspace&>) {
                                return compute(*current->input, *current->output, worker, *current->workspace);
                            } else { return compute(*current->input, *current->output, worker); }
                        }();
                        investigation.Record(run, "compute.end", current->workspace.get(), current->investigationMissing);
                        return success;
                    });
                    // 先将记录放入受 guard 保护的窗口，再发布终端任务
                    records.push_back(std::move(record));
                    if (!run.SubmitTerminal(records.back()->slot, records.back()->work)) { return false; }
                    progressed = true;
                }
            }
            if (!progressed) {
                ResourceDebugSnapshot snapshot;
                run.TryCopyResourceDebugSnapshot(snapshot);
                run.SetWaitReason(snapshot.byteWaiting ? ResourceWaitReason::ByteCapacity :
                    records.empty() ? ResourceWaitReason::SlotCapacity : ResourceWaitReason::OrderedCommit,
                    records.empty() ? nullptr : &records.front()->slot);
                run.WaitForChange(observed);
                run.SetWaitReason(ResourceWaitReason::None);
            }
        }
        run.SetFlowProgress(false);
        run.ClearByteWait();
        run.Scratch().ClearFixed();
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

} // namespace datacodec

#endif
