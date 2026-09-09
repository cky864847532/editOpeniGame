#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/NumericArrayCodec.h"

namespace datacodec {

WorkerContext::WorkerContext(ScratchByteBufferPool& scratch, std::size_t index) noexcept
    : m_scratch(scratch), m_index(index) {}
WorkerContext::~WorkerContext() = default;

numericarray::NumericArrayCompressorState& WorkerContext::NumericCompressor() {
    if (!m_compressor) { m_compressor = std::make_unique<numericarray::NumericArrayCompressorState>(); }
    return *m_compressor;
}

void WorkerContext::ReleaseCompressor() noexcept { m_compressor.reset(); }

void RecordExecutionException(DataCodecExecutionResources& run, std::string_view origin) noexcept {
    try { throw; }
    catch (const std::bad_alloc&) {
        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
            "allocation-failed", origin, "memory allocation failed"));
    } catch (const std::exception& error) {
        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
            "exception", origin, error.what()));
    } catch (...) {
        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
            "unknown-exception", origin, "unknown exception"));
    }
}

namespace {

class ParallelTaskGroup final : public IParallelTaskGroup {
    struct Record {
        SlotLease slot;
        std::shared_ptr<TerminalWork> work;
    };
    struct Cancel {
        DataCodecExecutionResources& run;
        void operator()() const noexcept { run.RequestStop(); }
    };
public:
    ParallelTaskGroup(DataCodecExecutionResources& run, std::stop_token stop)
        : m_run(run), m_cancel(stop, Cancel{run}) {
        if (!run.BeginFlow()) { throw std::runtime_error("task group requires a driver flow"); }
    }
    ~ParallelTaskGroup() override {
        try { Wait(); }
        catch (...) { m_run.CancelAndWaitRun(); }
    }
    void Submit(std::function<void()> task) override {
        if (!task) { return; }
        for (;;) {
            const auto epoch = m_run.EventEpoch();
            DrainReady();
            if (m_run.Stopped()) { throw std::runtime_error("task group stopped"); }
            if (auto slot = m_run.TryAcquireSlot()) {
                auto work = std::make_shared<TerminalWork>([task = std::move(task)](WorkerContext&) {
                    task();
                    return true;
                });
                m_records.push_back(Record{std::move(*slot), work});
                if (!m_run.SubmitTerminal(m_records.back().slot, work)) {
                    throw std::runtime_error("terminal task submission failed");
                }
                return;
            }
            m_run.SetWaitReason(m_records.empty() ? ResourceWaitReason::SlotCapacity : ResourceWaitReason::OrderedCommit,
                m_records.empty() ? nullptr : &m_records.front().slot);
            m_run.WaitForChange(epoch);
            m_run.SetWaitReason(ResourceWaitReason::None);
        }
    }
    void Wait() override {
        while (!m_records.empty()) {
            const auto epoch = m_run.EventEpoch();
            DrainReady();
            if (m_run.Stopped()) {
                m_run.CancelAndWaitRun();
                m_records.clear();
                throw std::runtime_error("task group stopped");
            }
            if (!m_records.empty()) {
                m_run.SetWaitReason(ResourceWaitReason::OrderedCommit, &m_records.front().slot);
                m_run.WaitForChange(epoch);
                m_run.SetWaitReason(ResourceWaitReason::None);
            }
        }
    }
private:
    void DrainReady() {
        while (!m_records.empty() && m_run.Completion(*m_records.front().work) == BlockCompletion::Succeeded) {
            if (!m_run.CommitSlot(m_records.front().slot)) { return; }
            m_records.pop_front();
        }
    }
    DataCodecExecutionResources& m_run;
    std::stop_callback<Cancel> m_cancel;
    std::deque<Record> m_records;
};

template<class Admission>
bool RunTerminal(DataCodecExecutionResources& run, const Admission& admission,
                 TerminalWork::Function function, TerminalWorkKind kind) noexcept {
    RunDrainGuard drain(run);
    try {
        auto work = std::make_shared<TerminalWork>(std::move(function), kind);
        if (!run.SubmitTerminal(admission, work)) { return false; }
        for (;;) {
            const auto observed = run.EventEpoch();
            if (run.Stopped()) { return false; }
            if (run.Completion(*work) == BlockCompletion::Succeeded) {
                run.SetWaitReason(ResourceWaitReason::None);
                drain.Complete();
                return true;
            }
            run.SetWaitReason(ResourceWaitReason::TerminalWork, nullptr, work.get());
            run.WaitForChange(observed);
        }
    } catch (...) {
        RecordExecutionException(run, "RunTerminalWork");
    }
    return false;
}

}

std::unique_ptr<IParallelTaskGroup> DataCodecExecutionResources::CreateGroup(std::stop_token stop) {
    return std::make_unique<ParallelTaskGroup>(*this, stop);
}

bool RunTerminalWork(DataCodecExecutionResources& run, const SlotLease& admission,
                     TerminalWork::Function function, TerminalWorkKind kind) noexcept {
    return RunTerminal(run, admission, std::move(function), kind);
}

bool RunTerminalWork(DataCodecExecutionResources& run, const HeavyPhaseLease& admission,
                     TerminalWork::Function function, TerminalWorkKind kind) noexcept {
    return RunTerminal(run, admission, std::move(function), kind);
}

}
