#ifndef DATACODEC_WORKFLOW_DECODE_STAGES_GEOMETRYDECODESTAGE_H
#define DATACODEC_WORKFLOW_DECODE_STAGES_GEOMETRYDECODESTAGE_H

#include "DataCodec/Codec/Geometry/GeometryDecode.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"
#include "DataCodec/Runtime/Context/DecodeContext.h"
#include "DataCodec/Runtime/Failure/DecodeFailureManagement.h"
#include "DataCodec/Runtime/Workspace/DecodeLeafWorkspace.h"
#include "DataCodec/Workflow/Decode/Stages/FieldDecodeInput.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"

#include <algorithm>
#include <string>
#include <string_view>
namespace datacodec {

inline void DecodeGeometryField(
    DecodeContext& context,
    DecodeLeafWorkspace& workspace,
    const FieldDecodeInput& input) {
    const auto& meta = workspace.StorageParams().geomParams;
    const auto hasGeometryPayload = meta.elementCount != 0u && std::max(0, meta.dimension) != 0;
    if (!input.HasField()) {
        if (hasGeometryPayload) {
            FailDecodeStage(
                context,
                workspace,
                "GeometryDecodeStage",
                CodecErrorCode::MissingInput,
                "failed to find geometry field");
        }
        return;
    }

    if (!hasGeometryPayload) {
        if (input.field->rawSize != 0u) {
            FailDecodeStage(
                context,
                workspace,
                "GeometryDecodeStage",
                CodecErrorCode::DecodeFailure,
                "geometry field raw size does not match empty params");
        }
        return;
    }

    decodefield::FieldDecodeStreamReader reader;
    std::string error;
    if (!decodefield::OpenLeafPackageFieldDecodeStream(
            *input.field,
            workspace.CacheResourcesRef(),
            reader,
            &error)) {
        FailDecodeStage(
            context,
            workspace,
            "GeometryDecodeStage",
            CodecErrorCode::PipelineFailure,
            "failed to open geometry field: " + error);
        return;
    }
    decodefield::FieldDecodeByteStream stream(reader);

    GeometryDecodeResult result;
    GeometryDecodeRuntime decodeRuntime{
        .data = GeometryDecodeData{
            .meta = meta,
            .payloadBytes = static_cast<std::uint64_t>(input.field->rawSize),
        },
        .cache = GeometryDecodeCache{
            .cacheResources = workspace.CacheResourcesRef(),
            .byteStoreSession = workspace.ByteStoreSessionRef(),
            .geometry = workspace.geometry,
            .referenceCache = context.currentGeometryReferenceCache,
            .destination = context.adapter,
        },
        .recordCapacitySamples = MakeCapacityRecordCallback(context.runRecords),
    };
    decodeRuntime.data.keyFrameReference = context.geometryKeyFrameReference;
    result = DecodeGeometryBlocks(decodeRuntime, stream);

    if (!result) {
        FailDecodeStage(
            context,
            workspace,
            "GeometryDecodeStage",
            result.code,
            result.message);
    }
}

class GeometryDecodeStage final : public DecodeStage {
public:
    static constexpr std::string_view kTypeName = "GeometryDecodeStage";

    GeometryDecodeStage() = default;
    explicit GeometryDecodeStage(FieldDecodeInput input) : m_input(input) {}

    StageKind Kind() const noexcept override { return StageKind::Geometry; }
    const char* Name() const override { return "GeometryDecodeStage"; }

    // 把几何字段交给 Codec 解码并归档失败
    void Execute(DecodeContext& context, DecodeLeafWorkspace& workspace) override {
        DecodeGeometryField(context, workspace, m_input);
    }

private:
    FieldDecodeInput m_input;
};

} // namespace datacodec

#endif
