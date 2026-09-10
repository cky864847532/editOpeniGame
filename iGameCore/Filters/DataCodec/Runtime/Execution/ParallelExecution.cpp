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

bool RunTerminalWork(DataCodecExecutionResources& run, const SlotLease& admission,
                     TerminalWork::Function function, TerminalWorkKind kind) noexcept {
    return RunTerminal(run, admission, std::move(function), kind);
}

bool RunTerminalWork(DataCodecExecutionResources& run, const HeavyPhaseLease& admission,
                     TerminalWork::Function function, TerminalWorkKind kind) noexcept {
    return RunTerminal(run, admission, std::move(function), kind);
}

}
