#ifndef DATACODEC_WORKFLOW_DECODE_PACKAGEDECODEWORKFLOW_H
#define DATACODEC_WORKFLOW_DECODE_PACKAGEDECODEWORKFLOW_H

#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Runtime/Record/ProgressRangeRunRecordSink.h"
#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"
#include "DataCodec/Runtime/Output/DataCodecOutputRouter.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Workflow/Frame/FrameTopologyOwnership.h"
#include "DataCodec/Workflow/Leaf/LeafDecodeExecutor.h"
#include "DataCodec/Workflow/Session/DecodeSession.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Validation/Storage/StorageValidator.h"
#include "DataCodec/Validation/Workflow/DecodeValidationLifecycle.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace datacodec {

struct ResolvedDecodeRequest : DecodePackageRequest {
    IDecodeAdapter* leafAdapter{nullptr};
    IFramePackageDecodeAssembly* frameAssembly{nullptr};
    std::shared_ptr<IByteRangeReader> inputReader;
    const FramePackage* framePackageMetadata{nullptr};
};

inline constexpr const char* kPackageDecodeWorkflowOrigin = "PackageDecodeWorkflow";

[[nodiscard]] inline TelemetryMessageRecord MakeDecodePackageMessage(
    const TelemetryMessageSeverity severity,
    std::string origin,
    std::string text) {
    return TelemetryMessageRecord{
        .severity = severity,
        .origin = std::move(origin),
        .text = std::move(text),
    };
}

inline void AppendDecodePackageMessages(
    std::vector<TelemetryMessageRecord>& target,
    const std::vector<TelemetryMessageRecord>& messages) {
    target.insert(target.end(), messages.begin(), messages.end());
}

inline void AddDecodePackageMessage(
    DecodePackageResult& result,
    RunRecordEmitter& runRecords,
    const TelemetryMessageSeverity severity,
    const std::string_view origin,
    const std::string_view text) {
    if (severity == TelemetryMessageSeverity::Error) {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "operation-failed", origin, text);
        }
        return;
    }
    auto message = MakeDecodePackageMessage(
        severity,
        std::string(origin),
        std::string(text));
    runRecords.AddMessage(message);
    AppendRetainedTelemetryMessage(result.messages, message);
}

inline void SubmitDecodePackageProgress(
    RunRecordEmitter& runRecords,
    const RunProgressPhase phase,
    const double normalized,
    const DataCodecMessageId messageId,
    std::initializer_list<DataCodecMessageArgument> arguments,
    const bool success) {
    runRecords.SubmitProgress(
        phase,
        callback::NormalizeProgress(normalized),
        messageId,
        arguments,
        success);
}

[[nodiscard]] inline DecodePackageResult DecodeLeafPackage(
    const LeafPackage& leafPackage,
    const ResolvedDecodeRequest& request,
    IRunRecordSink* runRecordSink,
    RunRecordEmitter& packageRecords,
    DecodeSession& session,
    DataCodecExecutionResources& resources) {
    DecodePackageResult result;
    if (request.stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (request.leafAdapter == nullptr) {
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            kPackageDecodeWorkflowOrigin,
            "decode package requires a leaf decode adapter");
        return result;
    }

    const auto frameIndex = request.requestedFrameIndex.value_or(0u);
    auto decodeResult = session.DecodeLeaf(LeafDecodeRequest{
        .adapter = request.leafAdapter,
        .leafPackage = &leafPackage,
        .topologyReferenceKey = request.topologyReferenceKey,
        .topologyOwnerFrameIndex = request.topologyOwnerFrameIndex,
        .frameIndex = frameIndex,
        .attributeSelection = request.attributeSelection,
        .attributeTargets = std::span<const AttributeTarget>(request.attributeTargets),
        .controlParams = request.configuration.controlParams,
        .configurationSource = request.configuration.source,
        .language = request.configuration.language,
        .runRecordSink = runRecordSink,
        .stopToken = request.stopToken,
        .resources = &resources,
    });
    result.failure = decodeResult.failure;
    if (request.stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    result.success = decodeResult.success;
    result.messages = std::move(decodeResult.messages);
    return result;
}

[[nodiscard]] inline DecodePackageResult DecodeLeafByteRange(
    const ResolvedDecodeRequest& request,
    RunRecordEmitter& packageRecords,
    IRunRecordSink* leafRunRecordSink,
    DataCodecExecutionResources& resources,
    DecodeSession* runSession) {
    DecodePackageResult result;
    result.inputBytes = request.inputReader == nullptr ? 0u : request.inputReader->ByteSize();

    LeafPackage leafPackage;
    std::string readError;
    if (!LeafPackageIO::ReadFromByteRange(
            request.inputReader,
            0u,
            result.inputBytes,
            leafPackage,
            &readError, resources.StopToken())) {
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            kPackageDecodeWorkflowOrigin,
            readError.empty() ? "failed to read DataCodec leaf package" : readError);
        return result;
    }

    DecodeSession localSession;
    auto& session = runSession != nullptr ? *runSession : localSession;
    result = DecodeLeafPackage(
        leafPackage,
        request,
        leafRunRecordSink,
        packageRecords,
        session,
        resources);
    result.inputBytes = request.inputReader == nullptr ? 0u : request.inputReader->ByteSize();
    return result;
}

[[nodiscard]] inline DecodePackageResult DecodeFramePackage(
    const FramePackage& framePackage,
    const ResolvedDecodeRequest& request,
    RunRecordEmitter& packageRecords,
    IRunRecordSink* leafRunRecordSink,
    DataCodecExecutionResources& resources,
    DecodeSession* runSession) {
    DecodePackageResult result;
    result.decodedFramePackage = true;
    result.inputBytes = request.inputReader == nullptr ? 0u : request.inputReader->ByteSize();
    if (request.stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }

    if (request.frameAssembly == nullptr) {
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            kPackageDecodeWorkflowOrigin,
            "decode memory frame package requires a frame assembly adapter");
        return result;
    }
    DecodeSession localDecodeSession;
    auto& decodeSession = runSession != nullptr ? *runSession : localDecodeSession;
    std::string assemblyError;
    if (!decodeSession.BeginFramePackage(
            framePackage,
            *request.frameAssembly,
            request.requestedFrameIndex,
            &assemblyError)) {
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            kPackageDecodeWorkflowOrigin,
            assemblyError.empty() ? "failed to begin frame package assembly" : assemblyError);
        return result;
    }

    const auto leafCount = framePackage.leaves.size();
    for (std::size_t leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
        if (request.stopToken.stop_requested()) {
            result.cancelled = true;
            decodeSession.AbortFramePackage();
            return result;
        }
        const auto& leaf = framePackage.leaves[leafIndex];
        const auto segmentBegin = leafCount == 0u
            ? 0.0
            : static_cast<double>(leafIndex) / static_cast<double>(leafCount);
        const auto segmentEnd = leafCount == 0u
            ? 1.0
            : static_cast<double>(leafIndex + 1u) / static_cast<double>(leafCount);
        const auto leafProgressMessageId = leafCount == 1u
            ? DataCodecMessageId::DecodeSingleBlock
            : DataCodecMessageId::DecodeBlock;
        if (leafCount == 1u) {
            SubmitDecodePackageProgress(
                packageRecords,
                RunProgressPhase::Update,
                segmentBegin,
                leafProgressMessageId,
                {},
                false);
        } else {
            SubmitDecodePackageProgress(
                packageRecords,
                RunProgressPhase::Update,
                segmentBegin,
                leafProgressMessageId,
                {
                    {"index", std::to_string(leafIndex + 1u)},
                    {"count", std::to_string(leafCount)},
                },
                false);
        }
        std::vector<DataCodecMessageArgument> leafProgressArguments;
        if (leafCount != 1u) {
            leafProgressArguments = {
                {"index", std::to_string(leafIndex + 1u)},
                {"count", std::to_string(leafCount)},
            };
        }
        const auto leafProgressText = FormatDataCodecMessage(
            packageRecords.Language(),
            leafProgressMessageId,
            leafProgressArguments);

        LeafPackage leafPackage;
        std::string readError;
        if (!LeafPackageIO::ReadFromByteRange(
                request.inputReader,
                leaf.leafPackageByteOffset,
                leaf.leafPackageByteSize,
                leafPackage,
                &readError, resources.StopToken())) {
            AddDecodePackageMessage(
                result,
                packageRecords,
                TelemetryMessageSeverity::Error,
                kPackageDecodeWorkflowOrigin,
                readError.empty() ? "failed to read frame leaf package" : readError);
            decodeSession.AbortFramePackage();
            return result;
        }
        leafPackage.path = leaf.path;

        assemblyError.clear();
        auto leafAdapter = decodeSession.CreateLeafAdapter(leaf, leafPackage, &assemblyError);
        if (leafAdapter == nullptr) {
            AddDecodePackageMessage(
                result,
                packageRecords,
                TelemetryMessageSeverity::Error,
                kPackageDecodeWorkflowOrigin,
                assemblyError.empty() ? "failed to create frame leaf decode adapter" : assemblyError);
            decodeSession.AbortFramePackage();
            return result;
        }

        ProgressRangeRunRecordSink segmentRecords(
            leafRunRecordSink,
            segmentBegin,
            segmentEnd,
            0u,
            0u,
            leafProgressText);
        auto leafRequest = request;
        leafRequest.leafAdapter = leafAdapter.get();
        leafRequest.requestedFrameIndex = framePackage.frameIndex;
        leafRequest.topologyReferenceKey = FrameTopologyOwnership::MakeTopologyOwnerKey(
            leaf.topologyMode == TopologyOwnershipMode::Owned
                ? framePackage.frameIndex
                : leaf.ownerFrameIndex,
            leaf.path);
        leafRequest.topologyOwnerFrameIndex = leaf.topologyMode == TopologyOwnershipMode::Owned
            ? framePackage.frameIndex
            : leaf.ownerFrameIndex;
        auto leafResult = DecodeLeafPackage(
            leafPackage,
            leafRequest,
            &segmentRecords,
            packageRecords,
            decodeSession,
            resources);
        if (leafResult.failure && !result.failure) {
            result.failure = leafResult.failure;
        }
        if (leafResult.success) {
            AppendDecodePackageMessages(result.messages, leafResult.messages);
        }
        if (leafResult.cancelled || request.stopToken.stop_requested()) {
            result.cancelled = true;
            decodeSession.AbortFramePackage();
            return result;
        }
        if (!leafResult.success) {
            if (result.messages.empty()) {
                AddDecodePackageMessage(
                    result,
                    packageRecords,
                    TelemetryMessageSeverity::Error,
                    kPackageDecodeWorkflowOrigin,
                    "failed to decode frame leaf package");
            }
            decodeSession.AbortFramePackage();
            return result;
        }
        assemblyError.clear();
        if (!decodeSession.CommitLeaf(leaf, *leafAdapter, &assemblyError)) {
            AddDecodePackageMessage(
                result,
                packageRecords,
                TelemetryMessageSeverity::Error,
                kPackageDecodeWorkflowOrigin,
                assemblyError.empty() ? "failed to commit frame leaf output" : assemblyError);
            decodeSession.AbortFramePackage();
            return result;
        }
    }

    assemblyError.clear();
    if (request.stopToken.stop_requested()) {
        result.cancelled = true;
        decodeSession.AbortFramePackage();
        return result;
    }
    if (!decodeSession.EndFramePackage(&assemblyError)) {
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            kPackageDecodeWorkflowOrigin,
            assemblyError.empty() ? "failed to end frame package assembly" : assemblyError);
        decodeSession.AbortFramePackage();
        return result;
    }

    result.success = true;
    return result;
}

[[nodiscard]] inline DecodePackageResult ExecutePackageDecodeWorkflowUnchecked(
    const ResolvedDecodeRequest& request,
    RunRecordEmitter& packageRecords,
    IRunRecordSink* leafRunRecordSink,
    DataCodecExecutionResources& resources,
    DecodeSession* runSession) {
    if (request.stopToken.stop_requested()) {
        DecodePackageResult result;
        result.cancelled = true;
        return result;
    }
    const auto inputValidation = validation::StorageValidator::ValidateDecodeInput(
        request.inputReader.get());
    if (!inputValidation) {
        DecodePackageResult result;
        result.failure = MakeCodecFailureRecord(inputValidation.code, "input.validation", "DecodePackage", inputValidation.message);
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            std::string(validation::DecodeValidationNodeName(
                validation::DecodeValidationNode::InputBoundary)),
            inputValidation.message);
        return result;
    }
    if (request.framePackageMetadata != nullptr) {
        return DecodeFramePackage(
            *request.framePackageMetadata,
            request,
            packageRecords,
            leafRunRecordSink,
            resources,
            runSession);
    }

    PackageInspection packageInspection;
    std::string headerError;
    const bool inspected = [&] {
        ScopedRunStageTiming timing(packageRecords, "DecodePackageInspect", TelemetryStageCategory::Params);
        return InspectPackage(*request.inputReader, packageInspection, &headerError, resources.StopToken());
    }();
    if (!inspected) {
        DecodePackageResult result;
        result.failure = packageInspection.failure;
        result.inputBytes = request.inputReader->ByteSize();
        AddDecodePackageMessage(
            result,
            packageRecords,
            TelemetryMessageSeverity::Error,
            std::string(validation::DecodeValidationNodeName(
                validation::DecodeValidationNode::FormatAndParams)),
            headerError.empty() ? "failed to read DataCodec package header" : headerError);
        return result;
    }

    if (packageInspection.format == PackageBinaryFormat::FramePackage) {
        FramePackage framePackage;
        std::string frameReadError;
        if (!FramePackageIO::ReadMetadata(*request.inputReader, framePackage, &frameReadError, resources.StopToken())) {
            DecodePackageResult result;
            result.inputBytes = request.inputReader->ByteSize();
            AddDecodePackageMessage(
                result,
                packageRecords,
                TelemetryMessageSeverity::Error,
                kPackageDecodeWorkflowOrigin,
                frameReadError.empty() ? "failed to read DataCodec frame package" : frameReadError);
            return result;
        }
        return DecodeFramePackage(
            framePackage,
            request,
            packageRecords,
            leafRunRecordSink,
            resources,
            runSession);
    }
    if (packageInspection.format == PackageBinaryFormat::LeafPackage) {
        return DecodeLeafByteRange(
            request,
            packageRecords,
            leafRunRecordSink,
            resources,
            runSession);
    }

    DecodePackageResult result;
    result.inputBytes = request.inputReader->ByteSize();
    AddDecodePackageMessage(
        result,
        packageRecords,
        TelemetryMessageSeverity::Error,
        kPackageDecodeWorkflowOrigin,
        "input does not contain a supported DataCodec package");
    return result;
}

[[nodiscard]] inline DecodePackageResult ExecutePackageDecodeWorkflow(
    const DecodePackageRequest& request,
    DataCodecExecutionResources& resources,
    DecodeSession* runSession, const FramePackage* metadata) {
    RunRecordDispatcher recordDispatcher;
    recordDispatcher.AddSink(request.runRecordSink);
    if (!request.outputSinks.Empty()) {
        recordDispatcher.AddSink(std::make_shared<DataCodecOutputRouter>(request.outputSinks));
    }
    RunRecordEmitter packageRecords;
    packageRecords.Reset(
        RunRecordInfo{
            .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
            .runKind = TelemetryRunKind::Decode,
            .objectName = "Package",
            .meshType = "Package",
            .language = request.configuration.language,
        },
        &recordDispatcher);
    packageRecords.BeginRun();
    const auto runStart = callback::Now();
    packageRecords.SubmitProgress(
        RunProgressPhase::Begin,
        0.0,
        DataCodecMessageId::PackageDecodeStarted);
    ProgressRangeRunRecordSink leafRunRecords(
        &recordDispatcher,
        0.0,
        1.0,
        0u,
        0u,
        {},
        packageRecords.RunId());

    const auto abortSessionOnFailure = [runSession]() noexcept {
        if (runSession == nullptr) {
            return;
        }
        try {
            runSession->AbortFramePackage();
        } catch (...) {
        }
    };
    DecodePackageResult result;
    try {
        ResolvedDecodeRequest resolved;
        static_cast<DecodePackageRequest&>(resolved) = request;
        resolved.inputReader = EncodedInputAccess::Open(request.input);
        resolved.framePackageMetadata = metadata;
        DecodedLeafBuilder leaf(request.cellTypeMapping);
        DecodedDataBuilder frame(request.cellTypeMapping);
        resolved.leafAdapter = &leaf;
        resolved.frameAssembly = &frame;
        result = ExecutePackageDecodeWorkflowUnchecked(
            resolved,
            packageRecords,
            &leafRunRecords,
            resources,
            runSession);
        if (result.success) {
            if (result.decodedFramePackage) {
                result.output = std::move(frame.output);
            } else {
                result.output.frameIndex = request.requestedFrameIndex.value_or(0u);
                result.output.leaves.push_back(std::move(leaf.output));
            }
        }
    } catch (const std::bad_alloc&) {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "allocation-failed", kPackageDecodeWorkflowOrigin,
                "memory allocation failed");
        }
        result.success = false;
        abortSessionOnFailure();
    } catch (const std::exception& exception) {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "exception", kPackageDecodeWorkflowOrigin, exception.what());
        }
        result.success = false;
        abortSessionOnFailure();
    } catch (...) {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, "unknown-exception", kPackageDecodeWorkflowOrigin,
                "unknown exception");
        }
        result.success = false;
        abortSessionOnFailure();
    }

    if (!result.success) {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(
                CodecErrorCode::DecodeFailure, result.cancelled ? "cancelled" : "operation-failed",
                kPackageDecodeWorkflowOrigin, result.cancelled ? "decode cancelled" : "decode did not complete",
                result.cancelled);
        }
        packageRecords.TryEndFailedRun(*result.failure, RunEndRecord{
            .success = false,
            .elapsedMs = callback::ElapsedMilliseconds(runStart),
            .inputBytes = result.inputBytes,
        });
        result.messages = packageRecords.TakeMessages();
        return result;
    }
    packageRecords.SubmitProgress(
        RunProgressPhase::Finish,
        1.0,
        result.success
            ? DataCodecMessageId::PackageDecodeCompleted
            : DataCodecMessageId::PackageDecodeFailed,
        {},
        result.success);
    packageRecords.EndRun(RunEndRecord{
        .success = result.success,
        .elapsedMs = callback::ElapsedMilliseconds(runStart),
        .inputBytes = result.inputBytes,
    });
    return result;
}

} // namespace datacodec

#endif
