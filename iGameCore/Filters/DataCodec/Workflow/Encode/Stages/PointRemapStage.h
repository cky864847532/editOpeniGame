#ifndef DATACODEC_WORKFLOW_ENCODE_STAGES_POINTREMAPSTAGE_H
#define DATACODEC_WORKFLOW_ENCODE_STAGES_POINTREMAPSTAGE_H

#include "DataCodec/Runtime/Context/EncodeContext.h"
#include "DataCodec/Runtime/Workspace/EncodeLeafWorkspace.h"
#include "DataCodec/Runtime/Failure/EncodeFailureManagement.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"
#include "DataCodec/Codec/Remap/PointRemapBuilder.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"

namespace datacodec {

class PointRemapStage final : public EncodeStage {
public:
    static constexpr std::string_view kTypeName = "PointSpatialPartition";

    const char* Name() const override {
        return "PointSpatialPartition.Morton";
    }

    // 构建几何、拓扑和点属性共享的点重排映射状态
    EncodeStageExecutionStatus Execute(
        EncodeContext& context,
        EncodeLeafWorkspace& workspace) override {
        const auto* geometry = workspace.GeometrySourceView();
        if (geometry == nullptr) {
            FailEncodeStage(
                context,
                workspace,
                Name(),
                CodecErrorCode::MissingInput,
                "requires a prepared geometry source");
            return EncodeStageExecutionStatus::Failed;
        }
        if (context.adapter == nullptr ||
            context.adapter->IsStructuredMesh() ||
            geometry->tupleCount <= 1u) {
            FailEncodeStage(
                context,
                workspace,
                Name(),
                CodecErrorCode::InvalidInput,
                "point spatial partition requires an unstructured input with at least two points");
            return EncodeStageExecutionStatus::Failed;
        }

        std::string error;
        pointremap::RemapProviders result;
        auto& byteStoreSession = workspace.ByteStoreSessionRef();
        const auto providerFactory = MakeStoreBackedWritableRemapProviderFactory(
            byteStoreSession,
            "point_remap");
        if (!pointremap::BuildPointMortonRemapProviders(
            *geometry,
            result,
            &error,
            pointremap::BuildOptions{
                .resources = context.resources,
                .providerFactory = providerFactory,
                .byteStoreSession = &byteStoreSession,
                .recordCapacitySamples = MakeCapacityRecordCallback(context.runRecords),
            })) {
            FailEncodeStage(
                context,
                workspace,
                Name(),
                CodecErrorCode::InvalidRemap,
                "failed to build point remap order: " + error);
            return EncodeStageExecutionStatus::Failed;
        }
        if (!workspace.SetPointRemap(
                std::move(result.orderProvider),
                std::move(result.inverseProvider))) {
            FailEncodeStage(
                context,
                workspace,
                Name(),
                CodecErrorCode::InvalidRemap,
                "point spatial partition did not produce a Morton order");
            return EncodeStageExecutionStatus::Failed;
        }
        return EncodeStageExecutionStatus::Completed;
    }

};

} // namespace datacodec

#endif
