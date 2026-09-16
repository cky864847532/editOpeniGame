#ifndef DATACODEC_WORKFLOW_FRAMESEQUENCE_FRAMESEQUENCEENCODEEXECUTOR_H
#define DATACODEC_WORKFLOW_FRAMESEQUENCE_FRAMESEQUENCEENCODEEXECUTOR_H

#include "DataCodec/API/Entry/DataCodecFrameSequenceEncode.h"
#include "DataCodec/Storage/FramePackage/FrameSequenceFileOutput.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Runtime/Record/ProgressRangeRunRecordSink.h"
#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"
#include "DataCodec/Workflow/Frame/FrameEncodeExecutor.h"
#include "DataCodec/Workflow/Session/EncodeSessionWorkspace.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <limits>
#include <new>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

class FrameSequenceEncodeExecutor final {
public:
    [[nodiscard]] static FrameSequenceEncodeResult Execute(
        const FrameSequenceEncodeRequest& request) noexcept {
        FrameSequenceEncodeResult result;
        if (!request.source || request.source->FrameCount() == 0u || request.source->FrameCount() > std::numeric_limits<std::uint32_t>::max()) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::InvalidInput,
                "invalid-sequence-input", "FrameSequenceEncodeExecutor", "frame sequence requires a shared source with a supported frame count");
            return result;
        }
        std::unique_ptr<FrameSequenceFileOutput> files;
        try {
            if (request.files) { files = std::make_unique<FrameSequenceFileOutput>(*request.files); }
            {
            DataCodecExecutionResources resources(request.resources);
            CodecRunScope run(resources);
            if (!run) { result.failure = resources.FirstFailure(); return result; }
            std::stop_callback stop(request.stopToken, [&resources] { resources.RequestStop(); });
            try {
                result = ExecuteInRun(request, resources, files.get());
                if (result.failure) { resources.RecordFailure(*result.failure); }
            } catch (...) {
                RecordExecutionException(resources, "FrameSequenceEncodeExecutor");
            }
            result.success = run.Finish(result.success);
            if (auto failure = resources.FirstFailure()) { result.failure = failure; }
            }
            if (request.stopToken.stop_requested()) {
                result.success = false;
                result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "sequence-cancelled", "FrameSequenceEncodeExecutor", "frame sequence cancelled before publication", true);
            }
            if (result.success && files) {
                std::string error;
                if (!files->Commit(&error)) {
                    result.success = false;
                    result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                        "sequence-publication", "FrameSequenceEncodeExecutor", error);
                }
            }
        } catch (const std::bad_alloc&) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                "allocation-failed", "FrameSequenceEncodeExecutor", "memory allocation failed");
        } catch (const std::exception& error) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                "entry-exception", "FrameSequenceEncodeExecutor", error.what());
        } catch (...) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                "entry-exception", "FrameSequenceEncodeExecutor", "unknown exception");
        }
        if (result.failure) { result.success = false; }
        if (!result.success) {
            result.frames.clear();
            result.encodedFrameCount = 0u;
            result.encodedByteCount = 0u;
        }
        return result;
    }

private:
    static FrameSequenceEncodeResult ExecuteInRun(
        const FrameSequenceEncodeRequest& request, DataCodecExecutionResources& resources, FrameSequenceFileOutput* files) {
        FrameSequenceEncodeResult result;
        RunRecordDispatcher recordDispatcher;
        recordDispatcher.AddSink(request.runRecordSink.get());
        RunRecordEmitter runRecords;
        runRecords.Reset(
            RunRecordInfo{
                .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
                .runKind = TelemetryRunKind::Encode,
                .objectName = "FrameSequence",
                .meshType = "FrameSequence",
                .language = request.configuration.language,
            },
            &recordDispatcher);
        runRecords.BeginRun();
        const auto runStart = callback::Now();
        struct RunFinalizer {
            FrameSequenceEncodeResult& result;
            RunRecordEmitter& records;
            callback::PhaseTimePoint start;

            ~RunFinalizer() noexcept {
                const RunEndRecord end{
                    .success = result.success,
                    .elapsedMs = callback::ElapsedMilliseconds(start),
                    .outputBytes = result.encodedByteCount,
                };
                if (result.failure) { records.TryEndFailedRun(*result.failure, end); }
                else { try { records.EndRun(end); } catch (...) {} }
            }
        } runFinalizer{result, runRecords, runStart};
        if (request.source == nullptr) {
            AddError(result, runRecords, "frame sequence encode requires a frame source");
            return result;
        }
        const auto frameCount = request.source->FrameCount();
        if (frameCount == 0u) {
            AddError(result, runRecords, "frame sequence encode requires at least one frame");
            return result;
        }

        const auto* controlParams = &request.configuration.controlParams;
        SubmitProgress(runRecords, RunProgressPhase::Begin, 0.0, false);
        EncodeSessionWorkspace workspace;
        std::set<std::uint32_t> frameIndices;

        try {
            for (std::size_t frameOrdinal = 0u; frameOrdinal < frameCount; ++frameOrdinal) {
                if (resources.Stopped()) {
                    result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                        "sequence-cancelled", "FrameSequenceEncodeExecutor", "frame sequence cancelled", true);
                    return result;
                }
                const auto frameBegin = static_cast<double>(frameOrdinal) /
                    static_cast<double>(frameCount);
                const auto frameEnd = static_cast<double>(frameOrdinal + 1u) /
                    static_cast<double>(frameCount);
                const auto loadEnd = frameBegin + (frameEnd - frameBegin) * 0.1;

                FrameSequenceEncodeFrame frame;
                std::string error;
                if (!request.source->LoadFrame(frameOrdinal, frame, &error) ||
                    frame.blockTreeAdapter == nullptr || !frameIndices.insert(frame.frameIndex).second) {
                    AddError(
                        result,
                        runRecords,
                        error.empty() ? "failed to load frame sequence input" : std::move(error));
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
                    return result;
                }
                SubmitProgress(runRecords, RunProgressPhase::Update, loadEnd, true);
                const auto inputMemory = ObserveInputMemory(frame.blockTreeAdapter->InputMemoryViews());

                std::filesystem::path finalPath;
                std::unique_ptr<FileByteRangeOutput> output;
                if (files) {
                    output = files->OpenFrame(frame.frameIndex, finalPath, &error);
                    if (!output) {
                        AddError(result, runRecords, error);
                        return result;
                    }
                }
                ProgressRangeRunRecordSink frameRecords(
                    &recordDispatcher,
                    loadEnd,
                    frameEnd,
                    static_cast<std::uint32_t>(frameOrdinal),
                    static_cast<std::uint32_t>(frameCount),
                    {},
                    runRecords.RunId());
                auto frameResult = FrameEncodeExecutor::Execute(FrameEncodeRequest{
                    .blockTreeAdapter = frame.blockTreeAdapter.get(),
                    .rootName = frame.rootName,
                    .frameIndex = frame.frameIndex,
                    .frameCount = static_cast<std::uint32_t>(frameCount),
                    .timeValue = frame.timeValue,
                    .controlParams = controlParams,
                    .pipelineControl = request.configuration.pipelineControl,
                    .configurationSource = request.configuration.source,
                    .language = request.configuration.language,
                    .runRecordSink = &frameRecords,
                    .outputSink = output.get(),
                    .attributeTargets = std::span<const AttributeTarget>(frame.attributeTargets),
                    .workspace = &workspace,
                    .nextFrameReferences = FrameReferenceNeeds{
                        .attributes = TemporalBuilder::ResolveTemporalFieldState(
                            static_cast<std::uint32_t>(frameCount), frame.frameIndex + 1u, controlParams->attrReference),
                        .geometry = TemporalBuilder::ResolveTemporalFieldState(
                            static_cast<std::uint32_t>(frameCount), frame.frameIndex + 1u, controlParams->geometryReference),
                    },
                }, resources);
                if (frameResult.failure && !result.failure) {
                    result.failure = frameResult.failure;
                }
                if (frameResult.success) {
                    AppendRetainedTelemetryMessages(result.messages, frameResult.messages);
                }
                if (!frameResult.success || !frameResult.hasEncodedOutput) {
                    if (result.messages.empty()) {
                        AddError(result, runRecords, "failed to encode frame sequence frame");
                    }
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
                    return result;
                }
                result.frames.push_back({frame.frameIndex, frame.timeValue,
                    std::move(frameResult.encodedBytes), std::move(finalPath), inputMemory});
                result.encodedByteCount += frameResult.encodedByteCount;
                ++result.encodedFrameCount;
            }
        } catch (const std::bad_alloc&) {
            AddError(result, runRecords, "frame sequence encode ran out of memory");
            SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
            return result;
        } catch (const std::exception& exception) {
            AddError(result, runRecords, exception.what());
            SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
            return result;
        }

        result.success = result.encodedFrameCount == frameCount;
        SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, result.success);
        return result;
    }

private:
    static void AddError(
        FrameSequenceEncodeResult& result,
        RunRecordEmitter& runRecords,
        std::string_view text) noexcept {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::EncodeFailure, "operation-failed", "FrameSequenceEncodeExecutor", text);
        }
        (void)runRecords;
    }

    static void SubmitProgress(
        RunRecordEmitter& runRecords,
        const RunProgressPhase phase,
        const double normalized,
        const bool success) {
        runRecords.SubmitProgress(RunProgressRecord{
            .phase = phase,
            .normalized = normalized,
            .success = success,
        });
    }
};

} // namespace datacodec

#endif
