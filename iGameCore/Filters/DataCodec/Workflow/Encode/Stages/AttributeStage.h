#ifndef DATACODEC_WORKFLOW_ENCODE_STAGES_ATTRIBUTESTAGE_H
#define DATACODEC_WORKFLOW_ENCODE_STAGES_ATTRIBUTESTAGE_H

#include "DataCodec/Codec/Attributes/AttributeEncode.h"
#include "DataCodec/Runtime/Failure/EncodeFailureManagement.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
namespace datacodec {

inline encodeimpl::AttributeEncodeRuntime MakeAttributeEncodeRuntime(
    EncodeContext& context,
    EncodeLeafWorkspace& workspace,
    std::shared_ptr<AttributeSpooler> attributeOutput) {
    return encodeimpl::AttributeEncodeRuntime{
        .data = encodeimpl::AttributeEncodeData{
            .adapter = *context.adapter,
            .storageParams = workspace.StorageParams(),
            .controlParams = *context.controlParams,
            .pointOrderSource = workspace.PointOrderSource(),
            .cellOrderSource = workspace.CellOrderSource(),
            .keyFrameReference = context.attributeKeyFrameReference,
            .temporalRole = context.attributeTemporalRole,
            .keyFrameIndex = context.attributeKeyFrameIndex,
        },
        .schedule = encodeimpl::AttributeEncodeSchedule{
            .attributeTiming = workspace.AttributeEncodeTimingRef(),
            .pointReferenceSchedule = workspace.MutableAttributeReferenceSchedule(AttrAttachment::Point),
            .cellReferenceSchedule = workspace.MutableAttributeReferenceSchedule(AttrAttachment::Cell),
        },
        .cache = encodeimpl::AttributeEncodeCache{
            .cacheResources = workspace.CacheResourcesRef(),
            .byteStoreSession = workspace.ByteStoreSessionRef(),
            .currentReferenceCache = context.currentAttributeReferenceCache,
            .attributeOutput = std::move(attributeOutput),
        },
        .context = encodeimpl::AttributeEncodeContext{
            .referenceScheduleMutex = workspace.AttributeReferenceScheduleMutex(),
            .referenceScheduleBuildMutex = workspace.AttributeReferenceScheduleBuildMutex(),
            .recordCapacitySamples = MakeCapacityRecordCallback(context.runRecords),
        },
    };
}

inline bool ExecuteAttributeField(
    EncodeContext& context,
    EncodeLeafWorkspace& workspace,
    const encodeimpl::AttributeFieldRequest& request) {
    if (context.adapter == nullptr || context.controlParams == nullptr) {
        FailEncodeStage(
            context,
            workspace,
            request.stageName,
            CodecErrorCode::MissingInput,
            "attribute field requires a valid encode adapter and resolved control parameters");
        return false;
    }
    const auto& dependency = context.controlParams->attrReference;
    auto attributeOutput = workspace.AttributeOutput();
    auto runtime = MakeAttributeEncodeRuntime(context, workspace, std::move(attributeOutput));
    const auto result = encodeimpl::EncodeAttributeField(runtime, request, dependency);
    if (!result) {
        FailEncodeStage(
            context,
            workspace,
            request.stageName,
            result.code,
            result.message);
        return false;
    }
    return true;
}

class PointAttributeStage final : public EncodeStage {
public:
    StageKind Kind() const noexcept override { return StageKind::PointAttribute; }
    static constexpr std::string_view kTypeName = "PointAttributeStage";

    PointAttributeStage(
        const std::size_t attrIndex,
        const std::size_t stageIndex)
        : m_attrIndex(attrIndex),
          m_stageIndex(stageIndex) {}

    const char* Name() const override { return "PointAttributeStage"; }
    std::size_t StageIndex() const override { return m_stageIndex; }
    bool UsesIndexedName() const override { return true; }

    EncodeStageExecutionStatus Execute(
        EncodeContext& context,
        EncodeLeafWorkspace& workspace) override {
        return ExecuteAttributeField(
            context,
            workspace,
            encodeimpl::AttributeFieldRequest{
                .attachment = AttrAttachment::Point,
                .attrIndex = m_attrIndex,
                .metaIndex = m_stageIndex,
                .stageName = Name(),
            })
            ? EncodeStageExecutionStatus::Completed
            : EncodeStageExecutionStatus::Failed;
    }

private:
    std::size_t m_attrIndex{0u};
    std::size_t m_stageIndex{0u};
};

class CellAttributeStage final : public EncodeStage {
public:
    StageKind Kind() const noexcept override { return StageKind::CellAttribute; }
    static constexpr std::string_view kTypeName = "CellAttributeStage";

    CellAttributeStage(
        const std::size_t attrIndex,
        const std::size_t stageIndex)
        : m_attrIndex(attrIndex),
          m_stageIndex(stageIndex) {}

    const char* Name() const override { return "CellAttributeStage"; }
    std::size_t StageIndex() const override { return m_stageIndex; }
    bool UsesIndexedName() const override { return true; }

    EncodeStageExecutionStatus Execute(
        EncodeContext& context,
        EncodeLeafWorkspace& workspace) override {
        return ExecuteAttributeField(
            context,
            workspace,
            encodeimpl::AttributeFieldRequest{
                .attachment = AttrAttachment::Cell,
                .attrIndex = m_attrIndex,
                .metaIndex = m_stageIndex,
                .stageName = Name(),
            })
            ? EncodeStageExecutionStatus::Completed
            : EncodeStageExecutionStatus::Failed;
    }

private:
    std::size_t m_attrIndex{0u};
    std::size_t m_stageIndex{0u};
};

} // namespace datacodec

#endif
