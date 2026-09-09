#ifndef DATACODEC_WORKFLOW_FRAMESEQUENCE_FRAMESEQUENCEENCODEEXECUTOR_H
#define DATACODEC_WORKFLOW_FRAMESEQUENCE_FRAMESEQUENCEENCODEEXECUTOR_H

#include "DataCodec/API/Adapter/IBlockTreeAdapter.h"
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
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

struct FrameSequenceEncodeFrame {
    std::unique_ptr<IBlockTreeAdapter> blockTreeAdapter;
    std::string rootName;
    std::uint32_t frameIndex{0u};
    float timeValue{0.0f};
    std::vector<AttributeTarget> attributeTargets;
};

class IFrameSequenceEncodeSource {
public:
    virtual ~IFrameSequenceEncodeSource() = default;
    [[nodiscard]] virtual std::size_t FrameCount() const noexcept = 0;
    virtual bool LoadFrame(
        std::size_t frameOrdinal,
        FrameSequenceEncodeFrame& frame,
        std::string* error = nullptr) = 0;
};

class IFrameSequenceOutputSink {
public:
    virtual ~IFrameSequenceOutputSink() = default;
    [[nodiscard]] virtual std::unique_ptr<IByteRangeOutput> OpenFrame(
        std::size_t frameOrdinal,
        std::uint32_t frameIndex,
        std::string* error = nullptr) = 0;
    virtual bool CommitFrame(
        std::size_t frameOrdinal,
        std::uint32_t frameIndex,
        std::uint64_t encodedByteCount,
        std::string* error = nullptr) = 0;
    virtual void AbortSequence() noexcept = 0;
};

struct FrameSequenceEncodeRequest {
    IFrameSequenceEncodeSource* source{nullptr};
    IFrameSequenceOutputSink* outputSink{nullptr};
    const CodecControlParams* controlParams{nullptr};
    EncodePipelineControlParams pipelineControl;
    DataCodecEncodeConfigurationSource configurationSource;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    IRunRecordSink* runRecordSink{nullptr};
    CodecResourceParams resources;
};

struct FrameSequenceEncodeResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    std::size_t encodedFrameCount{0u};
    std::uint64_t encodedByteCount{0u};
    std::vector<TelemetryMessageRecord> messages;
};

class FrameSequenceEncodeExecutor final {
public:
    [[nodiscard]] static FrameSequenceEncodeResult Execute(
        const FrameSequenceEncodeRequest& request) noexcept {
        FrameSequenceEncodeResult result;
        if (!request.source || !request.outputSink || request.source->FrameCount() == 0u) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::InvalidInput,
                "invalid-sequence-input", "FrameSequenceEncodeExecutor", "frame sequence requires a source, output sink and frames");
            return result;
        }
        try {
            DataCodecExecutionResources resources(request.resources);
            CodecRunScope run(resources);
            if (!run) { result.failure = resources.FirstFailure(); return result; }
            try {
                result = ExecuteInRun(request, resources);
                if (result.failure) { resources.RecordFailure(*result.failure); }
            } catch (...) {
                RecordExecutionException(resources, "FrameSequenceEncodeExecutor");
            }
            result.success = run.Finish(result.success);
            if (auto failure = resources.FirstFailure()) { result.failure = failure; }
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
        if (!result.success && request.outputSink) { request.outputSink->AbortSequence(); }
        return result;
    }

private:
    static FrameSequenceEncodeResult ExecuteInRun(
        const FrameSequenceEncodeRequest& request, DataCodecExecutionResources& resources) {
        FrameSequenceEncodeResult result;
        RunRecordDispatcher recordDispatcher;
        recordDispatcher.AddSink(request.runRecordSink);
        RunRecordEmitter runRecords;
        runRecords.Reset(
            RunRecordInfo{
                .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
                .runKind = TelemetryRunKind::Encode,
                .objectName = "FrameSequence",
                .meshType = "FrameSequence",
                .language = request.language,
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
        if (request.outputSink == nullptr) {
            AddError(result, runRecords, "frame sequence encode requires an output sink");
            return result;
        }
        const auto frameCount = request.source->FrameCount();
        if (frameCount == 0u) {
            AddError(result, runRecords, "frame sequence encode requires at least one frame");
            return result;
        }

        std::optional<CodecControlParams> defaultControlParams;
        const auto* controlParams = request.controlParams;
        if (!controlParams) {
            defaultControlParams.emplace(MakeDefaultEncodeControlParams());
            controlParams = &*defaultControlParams;
        }
        SubmitProgress(runRecords, RunProgressPhase::Begin, 0.0, false);
        EncodeSessionWorkspace workspace;

        try {
            for (std::size_t frameOrdinal = 0u; frameOrdinal < frameCount; ++frameOrdinal) {
                const auto frameBegin = static_cast<double>(frameOrdinal) /
                    static_cast<double>(frameCount);
                const auto frameEnd = static_cast<double>(frameOrdinal + 1u) /
                    static_cast<double>(frameCount);
                const auto loadEnd = frameBegin + (frameEnd - frameBegin) * 0.1;

                FrameSequenceEncodeFrame frame;
                std::string error;
                if (!request.source->LoadFrame(frameOrdinal, frame, &error) ||
                    frame.blockTreeAdapter == nullptr) {
                    AddError(
                        result,
                        runRecords,
                        error.empty() ? "failed to load frame sequence input" : std::move(error));
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
                    return result;
                }
                SubmitProgress(runRecords, RunProgressPhase::Update, loadEnd, true);

                auto output = request.outputSink->OpenFrame(
                    frameOrdinal,
                    frame.frameIndex,
                    &error);
                if (output == nullptr) {
                    AddError(
                        result,
                        runRecords,
                        error.empty() ? "failed to open frame sequence output" : std::move(error));
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
                    return result;
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
                    .pipelineControl = request.pipelineControl,
                    .configurationSource = request.configurationSource,
                    .language = request.language,
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
                if (!request.outputSink->CommitFrame(
                        frameOrdinal,
                        frame.frameIndex,
                        frameResult.encodedByteCount,
                        &error)) {
                    AddError(
                        result,
                        runRecords,
                        error.empty() ? "failed to commit frame sequence output" : std::move(error));
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, false);
                    return result;
                }
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
