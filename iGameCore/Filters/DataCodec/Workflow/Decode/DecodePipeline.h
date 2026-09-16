#ifndef DATACODEC_WORKFLOW_DECODE_DECODEPIPELINE_H
#define DATACODEC_WORKFLOW_DECODE_DECODEPIPELINE_H

#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Runtime/Failure/PipelineFailureManagement.h"
#include "DataCodec/Validation/Policy/CodecValidationPolicy.h"
#include "DataCodec/Runtime/Workspace/DecodeLeafWorkspace.h"
#include "DataCodec/Workflow/Common/PipelineStageNode.h"
#include "DataCodec/Workflow/Decode/Stages/AttrDecodeStage.h"
#include "DataCodec/Workflow/Decode/Stages/DecodeCommitStage.h"
#include "DataCodec/Workflow/Decode/Stages/GeometryDecodeStage.h"
#include "DataCodec/Workflow/Decode/Stages/FieldDecodeInput.h"
#include "DataCodec/Workflow/Decode/Stages/ParamsDecodeStage.h"
#include "DataCodec/Workflow/Decode/Stages/TopoDecodeStage.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Log/Telemetry/TelemetryCpuControl.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryControl.h"

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace datacodec {

using DecodeStageNode = PipelineStageNode<std::unique_ptr<DecodeStage>>;

struct DecodePipelineOptions {
    CodecValidationPolicy validationPolicy;
};

class DecodePipeline {
public:
    explicit DecodePipeline(DecodePipelineOptions options = {}) : m_options(std::move(options)) {}

    [[nodiscard]] static FieldDecodeInput BindFieldInput(
        const DecodeLeafWorkspace& workspace,
        const FieldType type,
        const std::size_t ordinal = 0u) {
        return FieldDecodeInput{
            type,
            workspace.packageFields.FindField(type, ordinal),
        };
    }

    static bool ExecuteParamsAndTopologyStages(
        DecodeContext& context,
        DecodeLeafWorkspace& workspace,
        std::string* error = nullptr) {
        DecodeFailureGuard guard(context, workspace);
        return guard.Run("DecodePipeline", CodecErrorCode::PipelineFailure, [&]() {
            ParamsDecodeStage paramsStage(BindFieldInput(workspace, FieldType::Params));
            ExecuteStage(paramsStage, context, workspace);
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode pipeline stopped while preparing topology reference params");
                return false;
            }
            if (!EnsureAdapterSupportsDecodedParams(context, workspace, error)) {
                return false;
            }

            TopoDecodeStage topologyStage(BindFieldInput(workspace, FieldType::Topology));
            ExecuteStage(topologyStage, context, workspace);
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode pipeline stopped while preparing topology reference");
                return false;
            }
            return true;
        });
    }

    static bool ExecuteParamsAndGeometryStages(
        DecodeContext& context,
        DecodeLeafWorkspace& workspace,
        std::string* error = nullptr) {
        DecodeFailureGuard guard(context, workspace);
        return guard.Run("DecodePipeline", CodecErrorCode::PipelineFailure, [&]() {
            ParamsDecodeStage paramsStage(BindFieldInput(workspace, FieldType::Params));
            ExecuteStage(paramsStage, context, workspace);
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode pipeline stopped while preparing geometry reference params");
                return false;
            }

            GeometryDecodeStage geometryStage(BindFieldInput(workspace, FieldType::Geometry));
            ExecuteStage(geometryStage, context, workspace);
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode pipeline stopped while preparing geometry reference");
                return false;
            }
            return true;
        });
    }

    // 预览固定 decode stage schedule
    std::vector<DecodeStageId> DescribeStageIds(const DecodeContext& context) const {
        DecodeLeafWorkspace workspace;
        RunBinding binding(workspace, context.resources);
        workspace.Reset(context.leafPackage);
        std::vector<DecodeStageId> ids;
        ParamsDecodeStage paramsStage(BindFieldInput(workspace, FieldType::Params));
        ids.push_back(paramsStage.Id());
        for (const auto& stageNode : BuildStageSchedule(context, workspace)) {
            ids.push_back(stageNode.stage->Id());
        }
        return ids;
    }

    // 运行一次单帧单块 decode pipeline
    void Execute(DecodeContext& context) const {
        DecodeLeafWorkspace workspace;
        Execute(context, workspace);
    }

    void Execute(DecodeContext& context, DecodeLeafWorkspace& workspace) const {
        RunBinding binding(workspace, context.resources);
        DecodeFailureGuard guard(context, workspace);
        guard.Run("DecodePipeline", CodecErrorCode::PipelineFailure, [&]() {
            SubmitProgress(
                context,
                RunProgressPhase::Begin,
                0.0,
                DataCodecMessageId::DecodeStarted,
                false);
            PrepareWorkspace(context, workspace, m_options);

            ParamsDecodeStage paramsStage(BindFieldInput(workspace, FieldType::Params));
            ExecuteStage(paramsStage, context, workspace);
            if (context.HasFailure() || workspace.StopRequested()) {
                return true;
            }
            std::string error;
            if (!EnsureAdapterSupportsDecodedParams(context, workspace, &error)) {
                return true;
            }
            if (!PrepareDirectAttributeDecodeStores(context, workspace, &error)) {
                FailDecodePipeline(
                    context,
                    workspace,
                    CodecErrorCode::PipelineFailure,
                    "failed to prepare direct attribute decode stores: " + error);
                return true;
            }

            auto stageNodes = BuildStageSchedule(context, workspace);
            if (!RunStageSchedule(context, workspace, stageNodes, &error)) {
                FailDecodePipeline(
                    context,
                    workspace,
                    CodecErrorCode::PipelineFailure,
                    "failed to run stage schedule: " + error);
                return false;
            }
            SubmitProgress(
                context,
                RunProgressPhase::Finish,
                1.0,
                DataCodecMessageId::DecodeCompleted,
                true);
            return true;
        });
        RecordRuntimeResourceUsage(context, workspace);
    }

    void ExecuteAttributes(DecodeContext& context, DecodeLeafWorkspace& workspace) const {
        RunBinding binding(workspace, context.resources);
        DecodeFailureGuard guard(context, workspace);
        guard.Run("DecodePipeline", CodecErrorCode::PipelineFailure, [&]() {
            if (!workspace.MatchesLeafPackage(context.leafPackage)) {
                FailDecodePipeline(
                    context,
                    workspace,
                    CodecErrorCode::InvalidInput,
                    "attribute supplement does not match the prepared leaf package");
                return false;
            }
            const auto targetAttrIndices = ResolveAttributeDecodeIndices(
                context.attributeTargets,
                context.frameIndex,
                context.leafPackage != nullptr ? context.leafPackage->path : BlockPath{},
                context.attributeSelection,
                workspace.StorageParams().attrParams.size());
            if (targetAttrIndices.empty()) {
                FailDecodePipeline(
                    context,
                    workspace,
                    CodecErrorCode::InvalidInput,
                    "attribute supplement requires at least one attribute target");
                return false;
            }
            const auto requiresDecode = context.attributeRequestMode != AttributeDecodeRequestMode::CommitCached;
            const auto requiresCommit = context.attributeRequestMode != AttributeDecodeRequestMode::DecodeToCache;
            const auto hasDecodeWork = requiresDecode && std::any_of(
                targetAttrIndices.begin(),
                targetAttrIndices.end(),
                [&workspace](const std::size_t attrIndex) {
                    return !workspace.attributes.Complete(attrIndex);
                });
            const auto hasCommitWork = requiresCommit && std::any_of(
                targetAttrIndices.begin(),
                targetAttrIndices.end(),
                [&workspace](const std::size_t attrIndex) {
                    return !workspace.AttributeCommitted(attrIndex);
                });
            if (!hasDecodeWork && !hasCommitWork) {
                SubmitProgress(
                    context,
                    RunProgressPhase::Finish,
                    1.0,
                    DataCodecMessageId::AttributeRequestCompleted,
                    true);
                return true;
            }

            workspace.PrepareSupplementRun(
                m_options.validationPolicy);
            SubmitProgress(
                context,
                RunProgressPhase::Begin,
                0.0,
                DataCodecMessageId::AttributeProcessingStarted,
                false);
            if (hasDecodeWork) {
                std::string prepareStoreError;
                if (!PrepareDirectAttributeDecodeStores(context, workspace, &prepareStoreError)) {
                    FailDecodePipeline(
                        context,
                        workspace,
                        CodecErrorCode::PipelineFailure,
                        prepareStoreError.empty()
                            ? "failed to prepare direct attribute decode stores"
                            : prepareStoreError);
                    return false;
                }
                AttrDecodeStage attributeStage(BindFieldInput(workspace, FieldType::Attribute));
                ExecuteStage(attributeStage, context, workspace);
                if (context.HasFailure() || workspace.StopRequested()) {
                    return true;
                }
            }
            if (hasCommitWork) {
                SubmitStageStartProgress(context, StageKind::Commit);
                CommitAttributeOutput(context, workspace);
                if (context.HasFailure() || workspace.StopRequested()) {
                    return true;
                }
                SubmitStageProgress(context, StageKind::Commit);
            }
            SubmitProgress(
                context,
                RunProgressPhase::Finish,
                1.0,
                DataCodecMessageId::AttributeProcessingCompleted,
                true);
            return true;
        });
        RecordRuntimeResourceUsage(context, workspace);
    }

private:
    static void RecordRuntimeResourceUsage(
        DecodeContext& context,
        const DecodeLeafWorkspace&) noexcept {
        RecordRootCapacityAudit(context.runRecords, context.resources);
        RecordCpuControlSummary(context);
        RecordMemoryControlSummary(context);
        if (context.adapter != nullptr) {
            context.runRecords.TryExport([&] {
                RecordBufferCapacitySamples(context.runRecords, context.adapter->CapacitySamples());
            });
        }
    }

    static void AddStage(
        std::vector<DecodeStageNode>& stageNodes,
        std::unique_ptr<DecodeStage> stage,
        std::vector<DecodeStageId> dependencies = {}) {
        stageNodes.push_back(DecodeStageNode{std::move(stage), std::move(dependencies)});
    }

    static void PrepareWorkspace(
        DecodeContext& context,
        DecodeLeafWorkspace& workspace,
        const DecodePipelineOptions& options) {
        workspace.Reset(context.leafPackage);
        workspace.SetValidationPolicy(options.validationPolicy);
    }

    static std::vector<DecodeStageNode> BuildStageSchedule(
        const DecodeContext& context,
        const DecodeLeafWorkspace& workspace) {
        std::vector<DecodeStageNode> stageNodes;
        auto geometry = std::make_unique<GeometryDecodeStage>(BindFieldInput(workspace, FieldType::Geometry));
        auto topology = std::make_unique<TopoDecodeStage>(BindFieldInput(workspace, FieldType::Topology));
        std::vector<DecodeStageId> commitDeps{
            geometry->Id(),
            topology->Id(),
        };
        AddStage(stageNodes, std::move(geometry));
        AddStage(stageNodes, std::move(topology));
        const auto targetAttrIndices = ResolveAttributeDecodeIndices(
            context.attributeTargets,
            context.frameIndex,
            context.leafPackage != nullptr ? context.leafPackage->path : BlockPath{},
            context.attributeSelection,
            workspace.StorageParams().attrParams.size());
        if (!targetAttrIndices.empty()) {
            auto attribute = std::make_unique<AttrDecodeStage>(BindFieldInput(workspace, FieldType::Attribute));
            commitDeps.push_back(attribute->Id());
            AddStage(stageNodes, std::move(attribute));
        }
        AddStage(stageNodes, std::make_unique<DecodeCommitStage>(), std::move(commitDeps));
        return stageNodes;
    }

    static bool EnsureAdapterSupportsDecodedParams(
        DecodeContext& context,
        DecodeLeafWorkspace& workspace,
        std::string* error = nullptr) {
        if (!workspace.StorageParams().topoParams.isPolyhedron ||
            context.adapter->SupportsPolyhedronTopology()) {
            return true;
        }
        const std::string message = "decode adapter does not support polyhedron topology";
        FailDecodePipeline(context, workspace, CodecErrorCode::PipelineFailure, message);
        validation::AssignError(error, message);
        return false;
    }

    static void ExecuteStage(DecodeStage& stage, DecodeContext& context, DecodeLeafWorkspace& workspace) {
        const auto stageName = stage.Id().name;
        const auto collectTiming = context.runRecords.Wants(RunRecordKind::StageTiming);
        SubmitStageStartProgress(context, stage.Kind());
        RecordMemoryTraceStageEvent(context, stageName, true);
        const auto startTime = callback::StartTiming(collectTiming);
        try {
            stage.Execute(context, workspace);
            if (collectTiming) {
                RecordStageTiming(
                    stageName,
                    context,
                    callback::ElapsedMilliseconds(startTime));
            }
            RecordMemoryTraceStageEvent(context, stageName, false);
            SubmitStageProgress(context, stage.Kind());
        } catch (...) {
            RecordMemoryTraceStageEvent(context, stageName, false);
            throw;
        }
    }

    static double DecodeStageProgress(const StageKind kind) {
        if (kind == StageKind::Parameters) return 0.10;
        if (kind == StageKind::Geometry) return 0.35;
        if (kind == StageKind::Topology) return 0.55;
        if (kind == StageKind::Attribute) return 0.85;
        if (kind == StageKind::Commit) return 0.95;
        return 0.0;
    }

    static double DecodeStageStartProgress(const StageKind kind) {
        if (kind == StageKind::Parameters) return 0.02;
        if (kind == StageKind::Geometry) return 0.15;
        if (kind == StageKind::Topology) return 0.35;
        if (kind == StageKind::Attribute) return 0.60;
        if (kind == StageKind::Commit) return 0.90;
        return 0.0;
    }

    static DataCodecMessageId DecodeStageProgressMessageId(const StageKind kind) {
        if (kind == StageKind::Parameters) return DataCodecMessageId::DecodeParams;
        if (kind == StageKind::Geometry) return DataCodecMessageId::DecodeGeometry;
        if (kind == StageKind::Topology) return DataCodecMessageId::DecodeTopology;
        if (kind == StageKind::Attribute) return DataCodecMessageId::DecodeAttribute;
        if (kind == StageKind::Commit) return DataCodecMessageId::DecodeCommit;
        return DataCodecMessageId::DecodeInProgress;
    }

    static void SubmitProgress(
        DecodeContext& context,
        const RunProgressPhase phase,
        const double normalized,
        const DataCodecMessageId messageId,
        const bool success) {
        context.runRecords.SubmitProgress(
            phase,
            normalized,
            messageId,
            {},
            success);
    }

    static void SubmitStageProgress(DecodeContext& context, const StageKind kind) {
        const double normalized = DecodeStageProgress(kind);
        if (normalized <= 0.0) return;
        SubmitProgress(
            context,
            RunProgressPhase::Update,
            normalized,
            DecodeStageProgressMessageId(kind),
            false);
    }

    static void SubmitStageStartProgress(DecodeContext& context, const StageKind kind) {
        const double normalized = DecodeStageStartProgress(kind);
        if (normalized <= 0.0) return;
        SubmitProgress(
            context,
            RunProgressPhase::Update,
            normalized,
            DecodeStageProgressMessageId(kind),
            false);
    }

    bool RunStageSchedule(
        DecodeContext& context,
        DecodeLeafWorkspace& workspace,
        const std::vector<DecodeStageNode>& stageNodes,
        std::string* error = nullptr) const {
        std::vector<std::vector<std::size_t>> dependents(stageNodes.size());
        std::vector<std::size_t> remainingDependencies(stageNodes.size(), 0u);
        for (std::size_t index = 0u; index < stageNodes.size(); ++index) {
            remainingDependencies[index] = stageNodes[index].dependencies.size();
            for (const auto& dependency : stageNodes[index].dependencies) {
                const auto predecessor = detail::FindStageIndex(stageNodes, dependency);
                if (predecessor == static_cast<std::size_t>(-1)) {
                    FailDecodePipeline(context, workspace, CodecErrorCode::PipelineFailure,
                        "decode stage depends on a missing stage");
                    return false;
                }
                dependents[predecessor].push_back(index);
            }
        }
        std::vector<bool> completed(stageNodes.size(), false);
        for (std::size_t count = 0u; count < stageNodes.size(); ++count) {
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode pipeline stopped");
                return false;
            }
            std::size_t next = 0u;
            while (next < stageNodes.size() && (completed[next] || remainingDependencies[next] != 0u)) { ++next; }
            if (next == stageNodes.size()) {
                FailDecodePipeline(context, workspace, CodecErrorCode::PipelineFailure,
                    "decode stage dependency cycle");
                return false;
            }
            const auto stageName = stageNodes[next].stage->Id().name;
            const auto timing = context.runRecords.Wants(RunRecordKind::StageTiming);
            const auto start = callback::StartTiming(timing);
            try {
                SubmitStageStartProgress(context, stageNodes[next].stage->Kind());
                RecordMemoryTraceStageEvent(context, stageName, true);
                stageNodes[next].stage->Execute(context, workspace);
                if (timing) { RecordStageTiming(stageName, context, callback::ElapsedMilliseconds(start)); }
                RecordMemoryTraceStageEvent(context, stageName, false);
                SubmitStageProgress(context, stageNodes[next].stage->Kind());
            } catch (const std::bad_alloc&) {
                context.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                    "allocation-failed", "DecodePipeline", "memory allocation failed"));
                workspace.RequestStop();
                return false;
            } catch (const std::exception& exception) {
                FailDecodePipeline(context, workspace, CodecErrorCode::PipelineFailure, exception.what());
                AssignFailureOrError(context, error, "decode stage failed");
                return false;
            } catch (...) {
                FailDecodePipeline(context, workspace, CodecErrorCode::PipelineFailure, "decode stage failed");
                AssignFailureOrError(context, error, "decode stage failed");
                return false;
            }
            if (context.HasFailure() || workspace.StopRequested()) {
                AssignFailureOrError(context, error, "decode stage failed");
                return false;
            }
            completed[next] = true;
            for (const auto dependent : dependents[next]) { --remainingDependencies[dependent]; }
        }
        return true;
    }

    static void RecordStageTiming(
        const std::string_view stageName,
        DecodeContext& context,
        const double elapsedMs) {
        if (!context.runRecords.Wants(RunRecordKind::StageTiming)) {
            return;
        }
        context.runRecords.RecordStageTiming(
            std::string(stageName),
            elapsedMs,
            ResolveStageCategory(stageName));
    }

    static TelemetryStageCategory ResolveStageCategory(const std::string_view stageName) {
        return ResolveTelemetryStageCategory(stageName);
    }

    DecodePipelineOptions m_options;
};

} // namespace datacodec

#endif
