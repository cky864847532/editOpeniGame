#ifndef DATACODEC_WORKFLOW_LEAF_LEAFENCODEEXECUTOR_H
#define DATACODEC_WORKFLOW_LEAF_LEAFENCODEEXECUTOR_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Adapter/IEncodeAdapter.h"
#include "DataCodec/Runtime/Context/EncodeContext.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Storage/LeafPackage/EncodedLeafFieldBundle.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Workflow/Encode/EncodePipeline.h"
#include "DataCodec/Workflow/Encode/EncodeStoragePlan.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

struct LeafEncodeRequest {
    EncodeContext* context{nullptr};
    EncodePipelineControlParams pipelineControl;
    DataCodecEncodeConfigurationSource configurationSource;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    IRunRecordSink* runRecordSink{nullptr};
    IByteRangeOutput* outputSink{nullptr};
    EncodedLeafFieldBundle* fieldBundleOutput{nullptr};
    bool includeTopology{true};
};

struct LeafEncodeResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    bool hasEncodedOutput{false};
    EncodedBuffer encodedBytes;
    std::uint64_t encodedByteCount{0u};
    std::vector<EncodeStageExecutionRecord> stageExecutions;
    std::vector<TelemetryMessageRecord> messages;
};

class LeafEncodeExecutor {
public:
    static LeafEncodeResult Execute(const LeafEncodeRequest& request) {
        LeafEncodeResult result;
        try {
            if (request.context == nullptr) {
                FinalizeEarlyFailure(
                    request,
                    nullptr,
                    "LeafEncodeExecutor",
                    CodecErrorCode::MissingInput,
                    "DataCodec leaf encoder requires an encode context",
                    result);
                return result;
            }

            EncodeContext& context = *request.context;
            struct ContextFailureCleanup {
                EncodeContext& context;
                LeafEncodeResult& result;

                ~ContextFailureCleanup() noexcept {
                    context.memoryTrace = nullptr;
                    if (!result.success) {
                        context.CleanupOnFailure();
                    }
                }
            } contextFailureCleanup{context, result};
            ResolveContextMetadata(context);
            if (context.adapter == nullptr) {
                FinalizeEarlyFailure(
                    request,
                    &context,
                    "LeafEncodeExecutor",
                    CodecErrorCode::MissingInput,
                    "DataCodec leaf encoder requires an encode adapter",
                    result);
                return result;
            }

            CodecControlParams defaultParams;
            const auto* originalControlParams = context.controlParams;
            struct ControlParamsRestore {
                EncodeContext& context;
                const CodecControlParams* originalControlParams{nullptr};
                ~ControlParamsRestore() noexcept { context.controlParams = originalControlParams; }
            } controlParamsRestore{context, originalControlParams};
            if (context.controlParams == nullptr) {
                defaultParams = CodecControlParamsFactory::MakeEncodeConfiguration(
                    DataCodecEncodeOptions{}).controlParams;
                context.controlParams = &defaultParams;
            }
            context.language = request.language;
            const auto* controlParams = context.controlParams;
            const auto outputKind = request.fieldBundleOutput != nullptr
                ? EncodePipelineOutputKind::EncodedLeafFieldBundle
                : EncodePipelineOutputKind::LeafPackage;
            EncodePipelineBinding pipelineBinding;
            std::string pipelineBindingError;
            if (!ResolveEncodePipelineBinding(
                    *context.adapter,
                    *controlParams,
                    request.pipelineControl,
                    pipelineBinding,
                    &pipelineBindingError,
                    outputKind,
                    request.includeTopology)) {
                FinalizeEarlyFailure(
                    request,
                    &context,
                    "LeafEncodeExecutor",
                    CodecErrorCode::InvalidInput,
                    pipelineBindingError,
                    result);
                return result;
            }
            if (const auto fixedLimit = context.resources.FixedStorageLimitBytes()) {
                const auto analysis = encodestorage::AnalyzeLeaf(*context.adapter, *controlParams,
                    request.pipelineControl, context.attributeTargets, context.frameIndex, context.path,
                    request.includeTopology, context.attributeTemporalRole, context.resources.StopToken());
                if (auto failure = CheckEncodeStorageLowerBound(analysis, fixedLimit)) {
                    context.RecordFailure(*failure);
                    result.failure = std::move(failure);
                    return result;
                }
            }
            const auto contextInitResult = context.Initialize(request.runRecordSink);
            if (!contextInitResult) {
                context.RecordFailure("EncodeContext", CodecErrorCode::MissingInput, contextInitResult.message);
                result.failure = context.FirstFailure();
                context.CleanupOnFailure();
                context.runRecords.TryEndFailedRun(*result.failure, context.runSummary);
                result.messages = context.runRecords.TakeMessages();
                return result;
            }
            TelemetryMemoryTraceRecorder memoryTrace;
            context.memoryTrace = &memoryTrace;
            context.runRecords.BeginRun();
            if (context.runRecords.Requests(RunCollectionKind::MemoryTrace)) {
                std::string memoryTraceError;
                if (!memoryTrace.Start(context.runRecords.RunInfo(), &memoryTraceError)) {
                    context.runRecords.AddWarning("TelemetryMemoryTrace", memoryTraceError);
                }
            }
            const auto startTime = callback::Now();

            context.runRecords.SubmitProgress(
                RunProgressPhase::Begin,
                0.0,
                DataCodecMessageId::EncodePreparing);

            const auto& pipelineDescriptor = pipelineBinding.descriptor;
            context.AddInfo(
                "EncodePipelineConfiguration",
                "runtime=" + std::string(DataCodecRuntimeProfileName(
                    request.configurationSource.runtimeProfile)) +
                    "; pipeline=" + EncodePipelineBindingName(pipelineDescriptor.id) +
                    "; output=" + EncodePipelineOutputKindName(pipelineDescriptor.outputKind) +
                    "; pointOrder=" + EncodePointOrderModeName(pipelineDescriptor.pointOrder) +
                    "; cellOrder=" + EncodeCellOrderModeName(pipelineDescriptor.cellOrder) +
                    "; topology=" + (pipelineDescriptor.includeTopology ? "Owned" : "Reused") +
                    "; packageField=" + PackageFieldEncodingModeName(pipelineDescriptor.packageFields.mode) +
                    "; zstdLevel=" + std::to_string(pipelineDescriptor.packageFields.zstdLevel));
            if (pipelineDescriptor.pointOrder == EncodePointOrderMode::Original) {
                context.AddInfo("PointOrderSource", "kind=Original");
            }
            if (pipelineDescriptor.cellOrder == EncodeCellOrderMode::Original) {
                context.AddInfo("CellOrderSource", "kind=Original");
            }
            EncodePipeline pipeline({
                .binding = std::move(pipelineBinding),
            });
            EncodePipelineResult pipelineResult;
            if (request.fieldBundleOutput != nullptr) {
                pipelineResult = pipeline.ExecuteToFieldBundle(context, *request.fieldBundleOutput);
            } else if (request.outputSink != nullptr) {
                pipelineResult = pipeline.ExecuteToSink(context, *request.outputSink);
            } else {
                pipelineResult = pipeline.Execute(context);
            }
            if (!pipelineResult.success) {
                result.failure = pipelineResult.failure ? pipelineResult.failure : context.FirstFailure();
                if (!result.failure) {
                    result.failure = MakeCodecFailureRecord(
                        CodecErrorCode::EncodeFailure, "operation-failed", "LeafEncodeExecutor",
                        "encode pipeline did not complete");
                }
                context.memoryTrace = nullptr;
                context.CleanupOnFailure();
                context.runRecords.TryEndFailedRun(*result.failure, context.runSummary);
                result.messages = context.runRecords.TakeMessages();
                return result;
            }
            if (pipelineResult.success && request.fieldBundleOutput == nullptr) {
                context.runRecords.SubmitProgress(
                    RunProgressPhase::Finish,
                    1.0,
                    DataCodecMessageId::EncodeCompleted,
                    {},
                    true);
            } else {
                context.runRecords.SubmitProgress(RunProgressRecord{
                    .phase = RunProgressPhase::Finish,
                    .normalized = 1.0,
                    .success = pipelineResult.success,
                });
            }
            context.runSummary.success = pipelineResult.success;
            context.runSummary.elapsedMs = callback::ElapsedMilliseconds(startTime);
            context.runSummary.outputBytes = ResolveEncodedOutputBytes(
                pipelineResult.encodedBytes,
                pipelineResult.encodedByteCount);
            if (memoryTrace.Active()) {
                context.runRecords.TryExport([&] {
                std::string stopError;
                if (memoryTrace.Stop(&stopError)) {
                    RecordTelemetryMemoryTraceSummary(
                        context.runRecords,
                        memoryTrace.Summary());
                } else {
                    context.runRecords.AddWarning("TelemetryMemoryTrace", stopError);
                }
                });
            }
            context.memoryTrace = nullptr;
            context.runRecords.EndRun(context.runSummary);
            result.messages = context.runRecords.TakeMessages();
            result.success = pipelineResult.success;
            result.hasEncodedOutput = pipelineResult.hasEncodedOutput;
            result.encodedBytes = std::move(pipelineResult.encodedBytes);
            result.encodedByteCount = pipelineResult.encodedByteCount;
            result.stageExecutions = std::move(pipelineResult.stageExecutions);
            return result;
        } catch (const std::bad_alloc&) {
            SaveException(request, result, "allocation-failed", "memory allocation failed");
        } catch (const std::exception& exception) {
            SaveException(request, result, "exception", exception.what());
        } catch (...) {
            SaveException(request, result, "unknown-exception", "unknown exception");
        }
        return result;
    }

private:
    static void SaveException(
        const LeafEncodeRequest& request,
        LeafEncodeResult& result,
        const std::string_view reason,
        const std::string_view message) noexcept {
        result.success = false;
        result.hasEncodedOutput = false;
        result.encodedBytes = {};
        result.encodedByteCount = 0u;
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::EncodeFailure, reason, "LeafEncodeExecutor", message);
        }
        if (request.context != nullptr) {
            request.context->RecordFailure(*result.failure);
            result.failure = request.context->FirstFailure();
            request.context->CleanupOnFailure();
        }
    }

    static void FinalizeEarlyFailure(
        const LeafEncodeRequest&,
        EncodeContext* context,
        const std::string_view origin,
        const CodecErrorCode code,
        const std::string_view message,
        LeafEncodeResult& result) noexcept {
        result.failure = MakeCodecFailureRecord(code, "invalid-input", origin, message);
        if (context != nullptr) {
            context->RecordFailure(*result.failure);
            result.failure = context->FirstFailure();
            context->CleanupOnFailure();
        }
    }

    static std::uint64_t ResolveEncodedOutputBytes(
        const EncodedBuffer& encodedBytes,
        const std::uint64_t encodedByteCount) {
        if (!encodedBytes.empty()) {
            return static_cast<std::uint64_t>(encodedBytes.size());
        }
        return encodedByteCount;
    }

    static void ResolveContextMetadata(EncodeContext& context) {
        if (context.adapter != nullptr && context.objectName.empty()) {
            context.objectName = context.adapter->GetName();
        }
        if (context.adapter != nullptr && context.meshType.empty()) {
            context.meshType = MeshTypeName(context.adapter->GetMeshType());
        }
        if (context.meshType.empty()) {
            context.meshType = "unknown";
        }
    }
};

} // namespace datacodec

#endif
