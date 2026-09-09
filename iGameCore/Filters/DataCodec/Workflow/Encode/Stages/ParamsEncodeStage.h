#ifndef DATACODEC_WORKFLOW_ENCODE_STAGES_PARAMSENCODESTAGE_H
#define DATACODEC_WORKFLOW_ENCODE_STAGES_PARAMSENCODESTAGE_H

#include "DataCodec/Runtime/Context/EncodeContext.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Runtime/Workspace/EncodeLeafWorkspace.h"
#include "DataCodec/Runtime/Failure/EncodeFailureManagement.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"

#include <cstdint>
#include <memory>
#include <string>
namespace datacodec {

class ParamsEncodeStage final : public EncodeStage {
public:
    static constexpr std::string_view kTypeName = "ParamsEncodeStage";

    const char* Name() const override { return "ParamsEncodeStage"; }

    void PrepareOutputs(EncodeContext&, EncodeLeafWorkspace& workspace) override {
        workspace.MarkTransferCachePending(workspace.TransferCacheLayout().params);
    }

    // 汇总所有编码 stage 写入的元数据，并生成最终 params transfer cache
    EncodeStageExecutionStatus Execute(
        EncodeContext& context,
        EncodeLeafWorkspace& workspace) override {
        if (workspace.TransferCacheLayout().attributes.has_value() && !ValidateAttribute(context, workspace)) {
            return EncodeStageExecutionStatus::Failed;
        }
        if (workspace.TransferCacheLayout().attributes.has_value()) {
            workspace.MarkTransferCacheReady(*workspace.TransferCacheLayout().attributes);
        }
        if (workspace.TransferCacheLayout().params == kInvalidTransferCacheIndex) {
            FailEncodeStage(
                context,
                workspace,
                kTypeName,
                CodecErrorCode::PipelineFailure,
                "params transfer cache layout was not initialized");
            return EncodeStageExecutionStatus::Failed;
        }
        auto phase = WaitForHeavyPhase(context.resources);
        if (!phase) { return EncodeStageExecutionStatus::Failed; }
        std::vector<std::uint8_t> paramsBytes;
        std::string error;
        RefreshAttributeDecodeScheduleHints(workspace.StorageParams());
        if (!SerializeCodecStorageParams(workspace.StorageParams(), paramsBytes, &error)) {
            FailEncodeStage(
                context,
                workspace,
                kTypeName,
                CodecErrorCode::InvalidInput,
                "failed to serialize params: " + error);
            return EncodeStageExecutionStatus::Failed;
        }
        RecordVectorCapacitySample(context.runRecords, "params.encode.serialized_bytes", paramsBytes);
        RecordCodecStorageParamsCapacity(context.runRecords, workspace.StorageParams());
        auto params = workspace.ByteStoreSessionRef().CreateSizedStore(
            bytestore::ByteStorePurpose::Ranged, paramsBytes.size(), "encoded_params", &error);
        if (!params) {
            FailEncodeStage(context, workspace, kTypeName, CodecErrorCode::EncodeFailure,
                "failed to prepare params owner: " + error);
            return EncodeStageExecutionStatus::Failed;
        }
        for (std::size_t offset = 0u; offset < paramsBytes.size();) {
            const auto count = std::min<std::size_t>(kIoWindowBytes, paramsBytes.size() - offset);
            if (context.resources.Stopped() || !params->WriteBytesAt(offset,
                    std::span<const std::uint8_t>(paramsBytes).subspan(offset, count), &error)) {
                FailEncodeStage(context, workspace, kTypeName, CodecErrorCode::EncodeFailure,
                    "failed to write params owner: " + error);
                return EncodeStageExecutionStatus::Failed;
            }
            offset += count;
        }
        if (!params->Seal(&error)) {
            FailEncodeStage(context, workspace, kTypeName, CodecErrorCode::EncodeFailure,
                "failed to seal params owner: " + error);
            return EncodeStageExecutionStatus::Failed;
        }
        workspace.PublishTransferCache(
            workspace.TransferCacheLayout().params,
            std::move(params),
            EncodedFieldCodecType::Params);
        return EncodeStageExecutionStatus::Completed;
    }

private:
    static bool ValidateAttribute(EncodeContext& context, EncodeLeafWorkspace& workspace) {
        const auto attributeOutput = workspace.AttributeOutput();
        if (attributeOutput == nullptr) {
            FailEncodeStage(
                context,
                workspace,
                kTypeName,
                CodecErrorCode::PipelineFailure,
                "attribute output was not initialized");
            return false;
        }
        if (const auto missingRecords = attributeOutput->MissingRecordCount(); missingRecords > 0u) {
            FailEncodeStage(
                context,
                workspace,
                kTypeName,
                CodecErrorCode::EncodeFailure,
                "attribute source is missing " + std::to_string(missingRecords) + " records");
            return false;
        }
        return true;
    }
};

} // namespace datacodec

#endif
