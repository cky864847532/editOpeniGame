#ifndef DATACODEC_WORKFLOW_FRAME_FRAMEENCODEEXECUTOR_H
#define DATACODEC_WORKFLOW_FRAME_FRAMEENCODEEXECUTOR_H

#include "DataCodec/API/Adapter/IBlockTreeAdapter.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Runtime/Record/ProgressRangeRunRecordSink.h"
#include "DataCodec/Runtime/Record/RunRecordDispatcher.h"
#include "DataCodec/Runtime/Record/RunRecordTimestamp.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldEncode.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/FramePackage/FramePackageFormat.h"
#include "DataCodec/Workflow/Leaf/LeafEncodeExecutor.h"
#include "DataCodec/Workflow/Session/EncodeSessionWorkspace.h"
#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

struct FrameEncodeRequest {
    IBlockTreeAdapter* blockTreeAdapter{nullptr};
    std::string rootName;
    std::uint32_t frameIndex{0u};
    std::uint32_t frameCount{1u};
    float timeValue{0.0f};
    const CodecControlParams* controlParams{nullptr};
    EncodePipelineControlParams pipelineControl;
    DataCodecEncodeConfigurationSource configurationSource;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    IRunRecordSink* runRecordSink{nullptr};
    IByteRangeOutput* outputSink{nullptr};
    std::span<const AttributeTarget> attributeTargets;
    EncodeSessionWorkspace* workspace{nullptr};
    std::optional<FrameReferenceNeeds> nextFrameReferences;
};

struct FrameEncodeResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    bool hasEncodedOutput{false};
    EncodedBuffer encodedBytes;
    std::uint64_t encodedByteCount{0u};
    std::vector<TelemetryMessageRecord> messages;
};

class FrameEncodeExecutor {
public:
    static bool CanEncodeFrame(const IBlockTreeAdapter* adapter) {
        return adapter != nullptr && !adapter->GetLeafRecords().empty();
    }

    static FrameEncodeResult Execute(const FrameEncodeRequest& request, DataCodecExecutionResources& resources) {
        FrameEncodeResult result;
        try {
            const auto runStart = callback::Now();
            RunRecordDispatcher recordDispatcher;
            recordDispatcher.AddSink(request.runRecordSink);
            RunRecordEmitter runRecords;
            runRecords.Reset(
                RunRecordInfo{
                    .generatedAtUtc = runrecorddetail::MakeTimestampUtc(),
                    .runKind = TelemetryRunKind::Encode,
                    .objectName = request.rootName.empty() && request.blockTreeAdapter != nullptr
                        ? request.blockTreeAdapter->GetRootName()
                        : request.rootName,
                    .meshType = "FramePackage",
                    .language = request.language,
                },
                &recordDispatcher);
            runRecords.BeginRun();

            if (request.blockTreeAdapter == nullptr) {
                AddError(result, runRecords, "FrameEncodeExecutor", "DataCodec frame package encoder requires a block tree adapter");
                FinalizeRun(result, runRecords, false, runStart);
                return result;
            }
            MemoryByteRangeOutput memorySink(resources);
            IByteRangeOutput* outputSink = request.outputSink != nullptr ? request.outputSink : &memorySink;

            SubmitProgress(
                runRecords,
                RunProgressPhase::Begin,
                0.0,
                DataCodecMessageId::EncodePreparing,
                false);

            EncodeSessionWorkspace localSession;
            auto& session = request.workspace != nullptr ? *request.workspace : localSession;
            bool completedSuccessfully = false;
            struct SessionFailureCleanup {
                EncodeSessionWorkspace& session;
                bool& completedSuccessfully;

                ~SessionFailureCleanup() noexcept {
                    if (!completedSuccessfully) {
                        try {
                            session.ResetSession();
                        } catch (...) {
                        }
                    }
                }
            } sessionFailureCleanup{session, completedSuccessfully};
            FrameEncodeState framePlan;
            std::string sessionError;
            if (!session.PrepareFrame(
                    DataCodecEncodeFrameInput{
                        .blockTreeAdapter = request.blockTreeAdapter,
                        .rootName = request.rootName,
                        .frameIndex = request.frameIndex,
                        .frameCount = request.frameCount,
                        .timeValue = request.timeValue,
                        .controlParams = request.controlParams,
                        .attributeTargets = request.attributeTargets,
                        .nextFrameReferences = request.nextFrameReferences,
                    },
                    framePlan,
                    resources,
                    &sessionError)) {
                AddError(
                    result,
                    runRecords,
                    "FrameEncodeExecutor",
                    sessionError.empty()
                        ? "failed to build DataCodec temporal frame plan"
                        : sessionError);
                SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                FinalizeRun(result, runRecords, false, runStart);
                return result;
            }
            std::vector<FramePackageIO::LeafPackageWriter> leafPackageWriters;
            std::vector<EncodedLeafFieldBundle> fieldBundles;
            std::vector<std::shared_ptr<LeafPackage>> leafPackages;
            const auto leafCount = framePlan.leaves.size();
            leafPackageWriters.reserve(leafCount);
            fieldBundles.reserve(leafCount);
            leafPackages.reserve(leafCount);

            for (std::size_t leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
                LeafEncodeRun leafRun;
                if (!session.PrepareLeaf(framePlan, leafIndex, leafRun, resources, &sessionError)) {
                    AddError(
                        result,
                        runRecords,
                        "FrameEncodeExecutor",
                        sessionError.empty()
                            ? "failed to prepare DataCodec frame leaf"
                            : sessionError);
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                const auto leafWorkBegin = leafCount == 0u
                    ? 0.0
                    : kFrameLeafWorkProgressEnd *
                        static_cast<double>(leafIndex) / static_cast<double>(leafCount);
                const auto leafWorkEnd = leafCount == 0u
                    ? kFrameLeafWorkProgressEnd
                    : kFrameLeafWorkProgressEnd *
                        static_cast<double>(leafIndex + 1u) / static_cast<double>(leafCount);
                const auto leafEncodeEnd = leafWorkBegin +
                    (leafWorkEnd - leafWorkBegin) * kLeafEncodeProgressFraction;
                const auto leafPackageEnd = leafWorkBegin +
                    (leafWorkEnd - leafWorkBegin) * kLeafPackageProgressFraction;
                SubmitProgress(
                    runRecords,
                    RunProgressPhase::Update,
                    leafWorkBegin,
                    leafCount == 1u
                        ? DataCodecMessageId::EncodeSingleBlock
                        : DataCodecMessageId::EncodeBlock,
                    false,
                    leafCount == 1u
                        ? std::initializer_list<DataCodecMessageArgument>{}
                        : std::initializer_list<DataCodecMessageArgument>{
                            {"index", std::to_string(leafIndex + 1u)},
                            {"count", std::to_string(leafCount)}});
                ProgressRangeRunRecordSink leafRecords(
                    &recordDispatcher,
                    leafWorkBegin,
                    leafEncodeEnd,
                    0u,
                    0u,
                    {},
                    runRecords.RunId());

                EncodedLeafFieldBundle fieldBundle;
                if (!EncodeLeafToFieldBundle(
                        leafRun,
                        request,
                        &leafRecords,
                        runRecords,
                        fieldBundle,
                        result)) {
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                session.CompleteLeafReferences(framePlan, leafRun);
                auto leafPackage = std::make_shared<LeafPackage>();
                std::string writerError;
                if (!BuildLeafPackageFromEncodedFieldBundle(
                        fieldBundle.path,
                        fieldBundle.paramsSource,
                        fieldBundle.segments,
                        *leafPackage,
                        &writerError)) {
                    AddError(result, runRecords, "FrameEncodeExecutor", "failed to build frame leaf package: " + writerError);
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                SubmitProgress(
                    runRecords,
                    RunProgressPhase::Update,
                    leafEncodeEnd,
                    request.pipelineControl.packageFields.mode == PackageFieldEncodingMode::Zstd
                        ? DataCodecMessageId::EncodePackageCompress
                        : DataCodecMessageId::EncodePackageWrite,
                    false);
                double lastPackageProgress = leafEncodeEnd;
                if (!CompressLeafPackageFields(
                        *leafPackage,
                        fieldBundle,
                        request.pipelineControl.packageFields,
                        resources,
                        runRecords,
                        [&](const std::uint64_t completedBytes, const std::uint64_t totalBytes) {
                            const auto normalized = totalBytes == 0u
                                ? leafPackageEnd
                                : leafEncodeEnd +
                                    (leafPackageEnd - leafEncodeEnd) *
                                        static_cast<double>(completedBytes) /
                                        static_cast<double>(totalBytes);
                            if (normalized >= lastPackageProgress + 0.002 || completedBytes >= totalBytes) {
                                lastPackageProgress = normalized;
                                SubmitProgress(
                                    runRecords,
                                    RunProgressPhase::Update,
                                    normalized,
                                    DataCodecMessageId::None,
                                    false);
                            }
                        },
                        &writerError)) {
                    AddError(result, runRecords, "FrameEncodeExecutor", "failed to encode frame leaf package fields: " + writerError);
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                SubmitProgress(
                    runRecords,
                    RunProgressPhase::Update,
                    leafPackageEnd,
                    DataCodecMessageId::None,
                    false);
                const auto* packageFieldStage =
                    request.pipelineControl.packageFields.mode == PackageFieldEncodingMode::Zstd
                        ? "PackageFieldZstd.Streaming"
                        : "PackageFieldRaw.Streaming";
                AddInfo(result, runRecords, packageFieldStage, "stage result=Completed");

                FramePackageIO::LeafPackageWriter leafPackageWriter;
                if (!FramePackageIO::MakeFrameLeafPackageWriter(
                        leafPackage,
                        leafPackageWriter,
                        &writerError)) {
                    AddError(result, runRecords, "FrameEncodeExecutor", "failed to prepare frame leaf package: " + writerError);
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                fieldBundles.push_back(std::move(fieldBundle));
                leafPackages.push_back(std::move(leafPackage));
                leafPackageWriters.push_back(std::move(leafPackageWriter));

                if (!session.CommitEncodedLeaf(
                        framePlan,
                        leafRun,
                        leafPackageWriters.back().leafPackageByteSize,
                        &sessionError)) {
                    AddError(
                        result,
                        runRecords,
                        "FrameEncodeExecutor",
                        sessionError.empty()
                            ? "failed to commit DataCodec frame leaf"
                            : sessionError);
                    SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                    FinalizeRun(result, runRecords, false, runStart);
                    return result;
                }
                SubmitProgress(
                    runRecords,
                    RunProgressPhase::Update,
                    leafWorkEnd,
                    DataCodecMessageId::None,
                    false);
            }

            std::string writeError;
            auto outputPhase = WaitForHeavyPhase(resources);
            if (!outputPhase) {
                AddError(result, runRecords, "FrameEncodeExecutor", "frame output admission was stopped");
                FinalizeRun(result, runRecords, false, runStart);
                return result;
            }
            std::uint64_t frameByteCount = 0u;
            SubmitProgress(
                runRecords,
                RunProgressPhase::Update,
                kFrameLeafWorkProgressEnd,
                DataCodecMessageId::EncodeWriteFile,
                false);
            double lastWriteProgress = kFrameLeafWorkProgressEnd;
            bytestore::KnownStorageOwners outputInputs;
            if (request.outputSink == nullptr) {
                for (const auto& leafPackage : leafPackages) {
                    for (const auto& field : leafPackage->fields) {
                        outputInputs.Add(field.source.get());
                        if (outputInputs.Entries().size() == 17u) { break; }
                    }
                    if (outputInputs.Entries().size() == 17u) { break; }
                }
                for (const auto& bundle : fieldBundles) {
                    if (outputInputs.Entries().size() == 17u) { break; }
                    outputInputs.Add(bundle.paramsSource.get());
                    for (const auto& owner : bundle.backingOwners) {
                        outputInputs.Add(owner.get());
                        if (outputInputs.Entries().size() == 17u) { break; }
                    }
                }
            }
            if (request.outputSink == nullptr &&
                (!FramePackageIO::ComputeFramePackageByteSize(framePlan.framePackage, leafPackageWriters,
                    frameByteCount, &writeError) ||
                 !memorySink.PrepareExactSize(frameByteCount, &writeError, outputInputs.Entries()))) {
                AddError(result, runRecords, "FrameEncodeExecutor", "failed to prepare frame output: " + writeError);
                FinalizeRun(result, runRecords, false, runStart);
                return result;
            }
            const bool frameWritten = [&] {
                ScopedRunStageTiming timing(runRecords, "PackageWrite.Frame");
                return FramePackageIO::WriteToSink(
                    framePlan.framePackage,
                    leafPackageWriters,
                    *outputSink,
                    &frameByteCount,
                    &writeError,
                    [&](const std::uint64_t completedBytes, const std::uint64_t totalBytes) {
                        const auto normalized = totalBytes == 0u
                            ? kFrameOutputProgressEnd
                            : kFrameLeafWorkProgressEnd +
                                (kFrameOutputProgressEnd - kFrameLeafWorkProgressEnd) *
                                    static_cast<double>(completedBytes) /
                                    static_cast<double>(totalBytes);
                        if (normalized >= lastWriteProgress + 0.002 || completedBytes >= totalBytes) {
                            lastWriteProgress = normalized;
                            SubmitProgress(
                                runRecords,
                                RunProgressPhase::Update,
                                normalized,
                                DataCodecMessageId::None,
                                false);
                        }
                    });
            }();
            if (!frameWritten) {
                AddError(result, runRecords, "FrameEncodeExecutor", "failed to write frame package: " + writeError);
                SubmitProgress(runRecords, RunProgressPhase::Finish, 1.0, DataCodecMessageId::None, false);
                FinalizeRun(result, runRecords, false, runStart);
                return result;
            }
            SubmitProgress(
                runRecords,
                RunProgressPhase::Update,
                kFrameFinalizeProgress,
                DataCodecMessageId::EncodeFinalizeFile,
                false);

            result.success = true;
            result.hasEncodedOutput = true;
            result.encodedByteCount = frameByteCount;
            if (request.outputSink == nullptr) {
                result.encodedBytes = memorySink.TakeBytes();
                result.encodedByteCount = static_cast<std::uint64_t>(result.encodedBytes.size());
            }
            SubmitProgress(
                runRecords,
                RunProgressPhase::Finish,
                1.0,
                DataCodecMessageId::EncodeCompleted,
                true);
            FinalizeRun(result, runRecords, true, runStart);
            completedSuccessfully = true;
            return result;
        } catch (const std::bad_alloc&) {
            if (!result.failure) {
                result.failure = MakeCodecFailureRecord(
                    CodecErrorCode::EncodeFailure, "allocation-failed", "FrameEncodeExecutor",
                    "memory allocation failed");
            }
        } catch (const std::exception& exception) {
            if (!result.failure) {
                result.failure = MakeCodecFailureRecord(
                    CodecErrorCode::EncodeFailure, "exception", "FrameEncodeExecutor", exception.what());
            }
        } catch (...) {
            if (!result.failure) {
                result.failure = MakeCodecFailureRecord(
                    CodecErrorCode::EncodeFailure, "unknown-exception", "FrameEncodeExecutor", "unknown exception");
            }
        }
        result.success = false;
        result.hasEncodedOutput = false;
        result.encodedBytes = {};
        result.encodedByteCount = 0u;
        return result;
    }

private:
    static constexpr double kFrameLeafWorkProgressEnd = 0.92;
    static constexpr double kLeafEncodeProgressFraction = 0.75;
    static constexpr double kLeafPackageProgressFraction = 0.98;
    static constexpr double kFrameOutputProgressEnd = 0.985;
    static constexpr double kFrameFinalizeProgress = 0.99;

    static void AddError(
        FrameEncodeResult& result,
        RunRecordEmitter&,
        const std::string_view origin,
        const std::string_view text) noexcept {
        if (!result.failure) {
            result.failure = MakeCodecFailureRecord(CodecErrorCode::EncodeFailure, "operation-failed", origin, text);
        }
    }

    static void AddInfo(
        FrameEncodeResult& result,
        RunRecordEmitter& runRecords,
        std::string origin,
        std::string text) {
        TelemetryMessageRecord message{
            .severity = TelemetryMessageSeverity::Info,
            .origin = std::move(origin),
            .text = std::move(text),
        };
        runRecords.AddMessage(message);
        AppendRetainedTelemetryMessage(result.messages, message);
    }

    static void AppendMessages(
        FrameEncodeResult& result,
        const std::vector<TelemetryMessageRecord>& messages) {
        AppendRetainedTelemetryMessages(result.messages, messages);
    }

    static void SubmitProgress(
        RunRecordEmitter& runRecords,
        const RunProgressPhase phase,
        const double normalized,
        const DataCodecMessageId messageId,
        const bool success,
        std::initializer_list<DataCodecMessageArgument> arguments = {}) {
        if (messageId == DataCodecMessageId::None) {
            runRecords.SubmitProgress(RunProgressRecord{
                .phase = phase,
                .normalized = normalized,
                .success = success,
            });
            return;
        }
        runRecords.SubmitProgress(
            phase,
            normalized,
            messageId,
            arguments,
            success);
    }

    static bool EncodeLeafToFieldBundle(
        LeafEncodeRun& leafRun,
        const FrameEncodeRequest& request,
        IRunRecordSink* leafRecordSink,
        RunRecordEmitter& runRecords,
        EncodedLeafFieldBundle& fieldBundle,
        FrameEncodeResult& result) {
        fieldBundle.Release();
        if (leafRun.context == nullptr) {
            AddError(result, runRecords, "FrameEncodeExecutor", "DataCodec frame leaf context is not prepared");
            return false;
        }
        auto leafResult = LeafEncodeExecutor::Execute({
            .context = leafRun.context.get(),
            .pipelineControl = request.pipelineControl,
            .configurationSource = request.configurationSource,
            .language = request.language,
            .runRecordSink = leafRecordSink,
            .fieldBundleOutput = &fieldBundle,
            .includeTopology = leafRun.frameLeaf.topologyMode == TopologyOwnershipMode::Owned,
        });

        if (leafResult.failure && !result.failure) {
            result.failure = leafResult.failure;
        }
        if (leafResult.success) {
            AppendMessages(result, leafResult.messages);
        }
        if (!leafResult.hasEncodedOutput) {
            AddError(result, runRecords, "FrameEncodeExecutor", "DataCodec leaf " + leafRun.leaf.path + " encode failed");
            return false;
        }
        return true;
    }

    static bool CompressLeafPackageFields(
        LeafPackage& leafPackage,
        EncodedLeafFieldBundle& fieldBundle,
        const PackageFieldEncodingParams& packageFields,
        DataCodecExecutionResources& resources,
        RunRecordEmitter& runRecords,
        const std::function<void(std::uint64_t, std::uint64_t)>& progressCallback,
        std::string* error) {
        ScopedRunStageTiming timing(runRecords, "PackageFields.Frame");
        if (fieldBundle.byteStoreSession == nullptr) {
            return validation::AssignError(error, "encoded leaf field bundle has no byte store session");
        }
        auto phase = WaitForHeavyPhase(resources);
        if (!phase) { return false; }
        // LeafPackage 已共享全部字段 owner，及时解除 bundle 的重复引用
        fieldBundle.paramsSource.reset();
        fieldBundle.segments.clear();
        fieldBundle.backingOwners.clear();
        std::uint64_t totalRawBytes = 0u;
        for (const auto& field : leafPackage.fields) {
            if (field.rawSize != 0u && field.source != nullptr &&
                packageFields.mode == PackageFieldEncodingMode::Zstd) {
                totalRawBytes = validation::SaturatingAddU64(totalRawBytes, field.rawSize);
            }
        }
        std::uint64_t completedRawBytes = 0u;
        if (progressCallback) {
            progressCallback(0u, totalRawBytes);
        }
        for (std::size_t fieldIndex = 0u; fieldIndex < leafPackage.fields.size(); ++fieldIndex) {
            auto& field = leafPackage.fields[fieldIndex];
            if (field.rawSize == 0u || field.source == nullptr) {
                field.compressionType = EncodedFieldCompressionType::None;
                continue;
            }
            if (packageFields.mode == PackageFieldEncodingMode::Raw) {
                field.compressionType = EncodedFieldCompressionType::None;
                continue;
            }
            const auto fieldRawBytes = static_cast<std::uint64_t>(field.rawSize);
            auto compressedStore = bytestore::CreateAppendableByteStore(
                *fieldBundle.byteStoreSession,
                "frame_leaf_field_" + std::to_string(fieldIndex) + "_zstd",
                error);
            if (compressedStore == nullptr) {
                return false;
            }
            bytestore::AppendableByteStoreWriter compressedWriter(compressedStore, resources);
            EncodedFieldCompressionType compressionType{EncodedFieldCompressionType::None};
            std::uint64_t rawByteSize = 0u;
            if (!EncodeLeafPackageFieldToWriter(
                    *field.source,
                    field.type,
                    packageFields,
                    LeafPackageFieldEncodeRuntime{
                        .run = resources,
                        .phase = *phase,
                        .progressCallback = [&](const std::uint64_t fieldCompletedBytes, const std::uint64_t) {
                            if (progressCallback) {
                                progressCallback(
                                    validation::SaturatingAddU64(
                                        completedRawBytes,
                                        std::min(fieldCompletedBytes, fieldRawBytes)),
                                    totalRawBytes);
                            }
                        },
                    },
                    compressedWriter,
                    compressionType,
                    rawByteSize,
                    error) ||
                !compressedStore->Seal(error)) {
                return false;
            }
            if (rawByteSize != field.rawSize) {
                return validation::AssignError(error, "frame leaf field raw byte size changed before package encoding");
            }
            field.compressionType = compressionType;
            field.source = std::move(compressedStore);
            completedRawBytes = validation::SaturatingAddU64(
                completedRawBytes,
                fieldRawBytes);
        }
        return true;
    }

    static void FinalizeRun(
        FrameEncodeResult& result,
        RunRecordEmitter& runRecords,
        const bool success,
        const callback::PhaseTimePoint runStart) {
        const RunEndRecord completion{
            .success = success,
            .elapsedMs = callback::ElapsedMilliseconds(runStart),
            .outputBytes = result.encodedByteCount,
        };
        if (!success && result.failure) {
            runRecords.TryEndFailedRun(*result.failure, completion);
            result.messages = runRecords.TakeMessages();
            return;
        }
        runRecords.EndRun(completion);
    }
};

} // namespace datacodec

#endif
