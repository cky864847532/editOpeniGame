#ifndef DATACODEC_WORKFLOW_LEAF_LEAFDECODEEXECUTOR_H
#define DATACODEC_WORKFLOW_LEAF_LEAFDECODEEXECUTOR_H

#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Validation/Workflow/DecodeValidationLifecycle.h"
#include "DataCodec/Workflow/Decode/DecodePipeline.h"
#include "DataCodec/Runtime/Failure/PipelineFailureManagement.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"

#include <exception>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

struct LeafDecodeRequest {
    IDecodeAdapter* adapter{nullptr};
    const LeafPackage* leafPackage{nullptr};
    DecodedAttributeReference* attributeKeyFrameReference{nullptr};
    DecodedGeometryReference* geometryKeyFrameReference{nullptr};
    DecodedGeometryReferenceCache* currentGeometryReferenceCache{nullptr};
    DecodedTopologyReferenceCacheStore* topologyReferenceStore{nullptr};
    std::string topologyReferenceKey;
    std::uint32_t topologyOwnerFrameIndex{0u};
    std::uint32_t frameIndex{0u};
    AttributeSelectionMode attributeSelection{AttributeSelectionMode::None};
    std::span<const AttributeTarget> attributeTargets;
    DecodeLeafWorkspace* workspace{nullptr};
    bool supplementAttributesOnly{false};
    AttributeDecodeRequestMode attributeRequestMode{AttributeDecodeRequestMode::DecodeAndCommit};
    DecodeControlParams controlParams{MakeDefaultDecodeControlParams()};
    DecodeExecutionOptions execution{MakeDefaultDecodeExecutionOptions()};
    DataCodecDecodeConfigurationSource configurationSource;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    IRunRecordSink* runRecordSink{nullptr};
    std::stop_token stopToken;
    DataCodecExecutionResources* resources{nullptr};
};

struct LeafDecodeResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    bool committedOutput{false};
    std::vector<TelemetryMessageRecord> messages;
};

class LeafDecodeExecutor {
public:
    [[nodiscard]] static LeafDecodeResult Execute(const LeafDecodeRequest& request) {
        LeafDecodeResult result;
        if (!request.resources) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::PipelineFailure,
                "missing-run", "LeafDecodeExecutor", "leaf decoding requires an active resource root");
            return result;
        }
        DecodeContext context(*request.resources);
        context.adapter = request.adapter;
        try {
            bool completedSuccessfully = false;
            struct FailureCleanupScope {
                const LeafDecodeRequest& request;
                DecodeContext* context{nullptr};
                bool& completedSuccessfully;

                ~FailureCleanupScope() noexcept {
                    if (completedSuccessfully) {
                        return;
                    }
                    if (context != nullptr) {
                        if (request.workspace != nullptr) {
                            CleanupAfterDecodeFailure(*context, *request.workspace);
                        } else {
                            context->CleanupOnFailure();
                        }
                        return;
                    }
                    if (request.workspace != nullptr) {
                        request.workspace->CleanupOnFailure();
                    }
                    if (request.adapter != nullptr) {
                        try {
                            request.adapter->Abort();
                        } catch (...) {
                        }
                    }
                }
            } failureCleanup{request, &context, completedSuccessfully};
            const auto& controlParams = request.controlParams;

            if (request.adapter == nullptr) {
                FinalizeEarlyFailure(
                    request,
                    std::string(validation::DecodeValidationNodeName(
                        validation::DecodeValidationNode::AlgorithmPrecondition)),
                    CodecErrorCode::MissingInput,
                    "DataCodec leaf decoder requires a decode adapter",
                    result);
                return result;
            }
            if (request.leafPackage == nullptr) {
                FinalizeEarlyFailure(
                    request,
                    std::string(validation::DecodeValidationNodeName(
                        validation::DecodeValidationNode::AlgorithmPrecondition)),
                    CodecErrorCode::MissingInput,
                    "DataCodec leaf decoder requires a leaf package",
                    result);
                return result;
            }

            context.leafPackage = request.leafPackage;
            context.attributeKeyFrameReference = request.attributeKeyFrameReference;
            context.geometryKeyFrameReference = request.geometryKeyFrameReference;
            context.currentGeometryReferenceCache = request.currentGeometryReferenceCache;
            context.topologyReferenceStore = request.topologyReferenceStore;
            context.topologyReferenceKey = request.topologyReferenceKey;
            context.frameIndex = request.frameIndex;
            context.language = request.language;
            context.attributeSelection = request.attributeSelection;
            context.attributeTargets = request.attributeTargets;
            context.attributeRequestMode = request.attributeRequestMode;
            context.topologyOutputMode = request.execution.topologyOutputMode;
            context.topologyBlockObserver = request.execution.topologyBlockObserver;

            const auto contextCreateResult = context.Initialize(request.runRecordSink);
            if (!contextCreateResult) {
                context.RecordFailure(
                    validation::DecodeValidationNodeName(validation::DecodeValidationNode::AlgorithmPrecondition),
                    CodecErrorCode::MissingInput,
                    contextCreateResult.message);
                result.failure = context.FirstFailure();
                CleanupDecodeFailure(request, context);
                context.runRecords.TryEndFailedRun(*result.failure, context.runSummary);
                result.messages = context.runRecords.TakeMessages();
                return result;
            }

            TelemetryMemoryTraceRecorder memoryTrace;
            context.memoryTrace = &memoryTrace;
            context.runRecords.BeginRun();
            context.AddInfo(
                "DecodePipelineConfiguration",
                "runtime=" + std::string(DataCodecRuntimeProfileName(
                    request.configurationSource.runtimeProfile)) +
                    "; validation=" +
                        (controlParams.validation.StrictDecodeEnabled()
                            ? "Strict"
                            : "Required"));
            if (context.runRecords.Requests(RunCollectionKind::MemoryTrace)) {
                std::string memoryTraceError;
                if (!memoryTrace.Start(context.runRecords.RunInfo(), &memoryTraceError)) {
                    context.runRecords.AddWarning("TelemetryMemoryTrace", memoryTraceError);
                }
            }
            const auto startTime = callback::Now();

            std::stop_callback stopBinding(request.stopToken, [&] {
                if (!context.resources.Stopped()) { context.resources.RequestStop(); }
            });

            try {
                DecodePipeline pipeline({
                    .validationPolicy = controlParams.validation,
                });
                if (request.supplementAttributesOnly) {
                    if (request.workspace == nullptr) {
                        context.RecordFailure(
                            "LeafDecodeExecutor",
                            CodecErrorCode::MissingInput,
                            "attribute supplement requires a persistent decode workspace");
                    } else {
                        pipeline.ExecuteAttributes(context, *request.workspace);
                    }
                } else if (request.workspace != nullptr) {
                    pipeline.Execute(context, *request.workspace);
                } else {
                    pipeline.Execute(context);
                }

                const auto requiresAdapterCommit =
                    !request.supplementAttributesOnly ||
                    request.attributeRequestMode != AttributeDecodeRequestMode::DecodeToCache;
                if (!context.HasFailure() && requiresAdapterCommit) {
                    CommitOutput(context, result);
                }
            } catch (const std::bad_alloc&) {
                context.RecordFailure(MakeCodecFailureRecord(
                    CodecErrorCode::DecodeFailure, "allocation-failed", "LeafDecodeExecutor",
                    "memory allocation failed"));
            } catch (const std::exception& exception) {
                context.RecordFailure(MakeCodecFailureRecord(
                    CodecErrorCode::DecodeFailure, "exception", "LeafDecodeExecutor", exception.what()));
            } catch (...) {
                context.RecordFailure(
                    validation::DecodeValidationNodeName(validation::DecodeValidationNode::AlgorithmExecution),
                    CodecErrorCode::DecodeFailure,
                    "unexpected decode exception");
            }

            const auto requiresAdapterCommit =
                !request.supplementAttributesOnly ||
                request.attributeRequestMode != AttributeDecodeRequestMode::DecodeToCache;
            if (context.HasFailure() || (requiresAdapterCommit && !result.committedOutput)) {
                if (!context.HasFailure() && requiresAdapterCommit) {
                    context.RecordFailure(
                        std::string(validation::DecodeValidationNodeName(
                            validation::DecodeValidationNode::Commit)),
                        CodecErrorCode::DecodeFailure,
                        "decode did not commit output");
                }
                CleanupDecodeFailure(request, context);
            }
            result.success = !context.HasFailure() && (!requiresAdapterCommit || result.committedOutput);
            result.failure = context.FirstFailure();
            if (result.failure) {
                context.memoryTrace = nullptr;
                context.runRecords.TryEndFailedRun(*result.failure, context.runSummary);
                result.messages = context.runRecords.TakeMessages();
                return result;
            }

            context.runSummary.elapsedMs = callback::ElapsedMilliseconds(startTime);
            context.runSummary.success = result.success;
            context.runSummary.inputBytes = request.leafPackage->EncodedFieldBytes();
            context.runSummary.outputBytes = request.leafPackage->rawFieldBytes;
            CaptureMemoryTrace(context, memoryTrace);
            context.memoryTrace = nullptr;
            context.runRecords.EndRun(context.runSummary);
            result.messages = context.runRecords.TakeMessages();
            completedSuccessfully = result.success;
            return result;
        } catch (const std::bad_alloc&) {
            context.RecordFailure(MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "allocation-failed", "LeafDecodeExecutor",
                "memory allocation failed"));
        } catch (const std::exception& exception) {
            context.RecordFailure(MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "exception", "LeafDecodeExecutor", exception.what()));
        } catch (...) {
            context.RecordFailure(MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "unknown-exception", "LeafDecodeExecutor", "unknown exception"));
        }
        result.success = false;
        result.failure = context.FirstFailure();
        return result;
    }

private:
    static void CleanupDecodeFailure(
        const LeafDecodeRequest& request,
        DecodeContext& context) noexcept {
        if (request.workspace != nullptr) {
            CleanupAfterDecodeFailure(context, *request.workspace);
            return;
        }
        context.CleanupOnFailure();
    }

    static void FinalizeEarlyFailure(
        const LeafDecodeRequest&,
        const std::string_view origin,
        const CodecErrorCode code,
        const std::string_view message,
        LeafDecodeResult& result) noexcept {
        result.failure = MakeCodecFailureRecord(code, "invalid-input", origin, message);
    }

    static void CommitOutput(
        DecodeContext& context,
        LeafDecodeResult& result) {
        try {
            std::string commitError;
            if (!context.adapter->Commit(&commitError)) {
                context.RecordFailure(
                    std::string(validation::DecodeValidationNodeName(
                        validation::DecodeValidationNode::Commit)),
                    CodecErrorCode::DecodeFailure,
                    commitError.empty() ? "failed to commit decoded output" : commitError);
            } else {
                result.committedOutput = true;
            }
        } catch (const std::bad_alloc&) {
            context.RecordFailure(MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "allocation-failed",
                validation::DecodeValidationNodeName(validation::DecodeValidationNode::Commit),
                "memory allocation failed"));
        } catch (const std::exception& exception) {
            context.RecordFailure(MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "exception",
                validation::DecodeValidationNodeName(validation::DecodeValidationNode::Commit),
                exception.what()));
        } catch (...) {
            context.RecordFailure(
                validation::DecodeValidationNodeName(validation::DecodeValidationNode::Commit),
                CodecErrorCode::DecodeFailure,
                "failed to commit decoded output");
        }
    }

    static void CaptureMemoryTrace(
        DecodeContext& context,
        TelemetryMemoryTraceRecorder& memoryTrace) {
        if (!memoryTrace.Active()) {
            return;
        }
        context.runRecords.TryExport([&] {
        std::string stopError;
        if (memoryTrace.Stop(&stopError)) {
            RecordTelemetryMemoryTraceSummary(
                context.runRecords,
                memoryTrace.Summary());
        } else {
            context.AddWarning("TelemetryMemoryTrace", stopError);
        }
        });
    }
};

} // namespace datacodec

#endif
