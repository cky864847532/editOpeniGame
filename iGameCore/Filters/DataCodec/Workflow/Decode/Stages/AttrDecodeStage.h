#ifndef DATACODEC_WORKFLOW_DECODE_STAGES_ATTRDECODESTAGE_H
#define DATACODEC_WORKFLOW_DECODE_STAGES_ATTRDECODESTAGE_H

#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryControl.h"

#include "DataCodec/Codec/Attributes/AttributeDecode.h"
#include "DataCodec/Codec/Attributes/AttributeReferenceDecode.h"
#include "DataCodec/Codec/SubCodec/ZstdCodec.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Runtime/Failure/DecodeFailureManagement.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h"
#include "DataCodec/Workflow/Decode/Stages/FieldDecodeInput.h"
#include "DataCodec/Workflow/Common/PipelineStageBase.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace datacodec {

// [DC防护:阶段] attribute decode 入口校验字段 raw size 和 params 声明的字节数契约
inline bool ValidateAttributeFieldSize(
    const CodecStorageParams& params,
    const LeafPackageField& field,
    std::string* error = nullptr) {
    ParamSize expectedBytes = 0u;
    if (!CalculateAttributePayloadBytes(params, expectedBytes, error)) {
        return false;
    }
    if (static_cast<std::uint64_t>(field.rawSize) != expectedBytes) {
        return validation::AssignError(error, "attribute field raw size does not match params binaryCount");
    }
    return true;
}

inline const char* AttributeAttachmentName(const AttrAttachment attachment) noexcept {
    switch (attachment) {
        case AttrAttachment::Point:
            return "point";
        case AttrAttachment::Cell:
            return "cell";
        default:
            return "unknown";
    }
}

inline const char* DataTypeName(const DataType dataType) noexcept {
    switch (dataType) {
        case DataType::Float32:
            return "float32";
        case DataType::Float64:
            return "float64";
        case DataType::Int32:
            return "int32";
        case DataType::Int64:
            return "int64";
        case DataType::UInt32:
            return "uint32";
        case DataType::UInt64:
            return "uint64";
        case DataType::Int8:
            return "int8";
        case DataType::UInt8:
            return "uint8";
        case DataType::Int16:
            return "int16";
        case DataType::UInt16:
            return "uint16";
        default:
            return "unknown";
    }
}

inline ParamSize BytesPerElementMilli(const ParamSize byteCount, const ParamSize elementCount) noexcept {
    if (elementCount == 0u) {
        return 0u;
    }
    if (byteCount > std::numeric_limits<ParamSize>::max() / 1000u) {
        return std::numeric_limits<ParamSize>::max();
    }
    return byteCount * 1000u / elementCount;
}

inline std::string FormatAttributeDecodeTimingScope(
    const decodeimpl::detail::AttributeDecodeTimingDetail& detail) {
    const auto name = detail.name.empty() ? std::string("<unnamed>") : detail.name;
    return "name=" + name +
        ";attachment=" + AttributeAttachmentName(detail.attachmentType) +
        ";dataType=" + DataTypeName(detail.dataType) +
        ";elements=" + std::to_string(detail.elementCount) +
        ";dimension=" + std::to_string(detail.dimension) +
        ";valueSize=" + std::to_string(detail.valueSize) +
        ";rawBytes=" + std::to_string(detail.rawValueBytes) +
        ";binaryBytes=" + std::to_string(detail.binaryCount) +
        ";encodedBlockBytes=" + std::to_string(detail.encodedBlockBytes) +
        ";binaryBytesPerElementX1000=" + std::to_string(
            BytesPerElementMilli(detail.binaryCount, detail.elementCount)) +
        ";blocks=" + std::to_string(detail.blockCount) +
        ";nonRefBlocks=" + std::to_string(detail.nonReferenceBlocks) +
        ";refBlocks=" + std::to_string(detail.referenceBlocks) +
        ";intraRefBlocks=" + std::to_string(detail.intraReferenceBlocks) +
        ";temporalRefBlocks=" + std::to_string(detail.temporalReferenceBlocks) +
        ";affineBlocks=" + std::to_string(detail.affineReferenceBlocks) +
        ";waveletBlocks=" + std::to_string(detail.waveletReferenceBlocks) +
        ";predictorBlocks=" + std::to_string(detail.predictorReferenceBlocks) +
        ";layeredResidualBlocks=" + std::to_string(detail.layeredResidualBlocks) +
        ";regionLayers=" + std::to_string(detail.regionLayerCount) +
        ";componentLayouts=" + std::to_string(detail.componentLayoutCount) +
        ";payloadReadMs=" + std::to_string(detail.payloadBlockReadMs) +
        ";ordinaryDecodeMs=" + std::to_string(detail.ordinaryDecodeMs) +
        ";referenceResolveMs=" + std::to_string(detail.referenceResolveMs) +
        ";temporalKeyEnsureMs=" + std::to_string(detail.temporalKeyReferenceEnsureMs) +
        ";referenceRangeResolveMs=" + std::to_string(detail.referenceRangeResolveMs) +
        ";referenceDecodeMs=" + std::to_string(detail.referenceDecodeMs) +
        ";affineReferenceDecodeMs=" + std::to_string(detail.affineReferenceDecodeMs) +
        ";waveletReferenceDecodeMs=" + std::to_string(detail.waveletReferenceDecodeMs) +
        ";predictorReferenceDecodeMs=" + std::to_string(detail.predictorReferenceDecodeMs) +
        ";cacheWriteMs=" + std::to_string(detail.cacheWriteMs) +
        ";ordinaryDecodedBytes=" + std::to_string(detail.ordinaryDecodedBytes) +
        ";referenceDecodedBytes=" + std::to_string(detail.referenceDecodedBytes) +
        ";referenceBytesRead=" + std::to_string(detail.referenceBytesRead) +
        ";intraReferenceWorksetBytes=" + std::to_string(detail.intraReferenceWorksetBytes) +
        ";temporalReferenceWorksetBytes=" + std::to_string(detail.temporalReferenceWorksetBytes) +
        ";resampledReferenceWorksetBytes=" + std::to_string(detail.resampledReferenceWorksetBytes) +
        ";predictorRangeStagingBytes=" + std::to_string(detail.predictorRangeStagingBytes) +
        ";predictorShiftedCopyBytes=" + std::to_string(detail.predictorShiftedCopyBytes) +
        ";waveletLowBlobBytes=" + std::to_string(detail.waveletLowBlobBytes) +
        ";waveletHighBlobBytes=" + std::to_string(detail.waveletHighBlobBytes) +
        ";cacheWriteBytes=" + std::to_string(detail.cacheWriteBytes) +
        ";temporalKeyFieldCacheHits=" + std::to_string(detail.temporalKeyFieldCacheHits) +
        ";temporalKeyFieldCacheMisses=" + std::to_string(detail.temporalKeyFieldCacheMisses) +
        ";predictorZeroOffsetBlocks=" + std::to_string(detail.predictorZeroOffsetBlocks) +
        ";predictorContinuousShiftBlocks=" + std::to_string(detail.predictorContinuousShiftBlocks) +
        ";predictorBoundaryClampBlocks=" + std::to_string(detail.predictorBoundaryClampBlocks);
}

inline bool PrepareDirectAttributeDecodeStores(
    DecodeContext& context,
    DecodeLeafWorkspace& workspace,
    std::string* error = nullptr) {
    ScopedRunStageTiming timing(context.runRecords, "AttributeStorePrepare", TelemetryStageCategory::Attribute);
    if (context.adapter == nullptr || !context.adapter->SupportsAttributeDecodeStore()) {
        return true;
    }
    const auto attrIndices = ResolveAttributeDecodeIndices(
        context.attributeTargets,
        context.frameIndex,
        context.leafPackage != nullptr ? context.leafPackage->path : BlockPath{},
        context.attributeSelection,
        workspace.StorageParams().attrParams.size());
    if (attrIndices.empty()) {
        return true;
    }
    if (!workspace.attributes.IsInitialized() &&
        !workspace.attributes.Initialize(
            workspace.StorageParams(),
            workspace.ByteStoreSessionRef(),
            error)) {
        return false;
    }
    RecordSchedulerInvestigation(context, workspace, "attributes.prepare.begin");
    for (const auto attrIndex : attrIndices) {
        if (attrIndex >= workspace.StorageParams().attrParams.size()) {
            return validation::AssignError(error, "attribute decode store index is out of range");
        }
        if (workspace.attributes.Complete(attrIndex) || workspace.attributes.AdapterBacked(attrIndex)) {
            continue;
        }
        auto store = context.adapter->CreateAttributeDecodeStore(
            attrIndex,
            workspace.StorageParams().attrParams[attrIndex],
            error);
        if (store == nullptr ||
            !workspace.attributes.BindAttributeStore(
                attrIndex,
                std::move(store),
                true,
                error)) {
            return false;
        }
    }
    RecordSchedulerInvestigation(context, workspace, "attributes.prepare.end");
    return workspace.CacheResourcesRef().Run().SynchronizeMemoryAfterPreparation();
}

class AttrDecodeStage final : public DecodeStage {
public:
    static constexpr std::string_view kTypeName = "AttrDecodeStage";

    AttrDecodeStage() = default;
    explicit AttrDecodeStage(FieldDecodeInput input) : m_input(input) {}

    const char* Name() const override { return "AttrDecodeStage"; }
    [[nodiscard]] bool UsesInternalParallelism() const noexcept override { return true; }

    // 把属性 domain 按 block 解入 decoded attribute cache
    void Execute(DecodeContext& context, DecodeLeafWorkspace& workspace) override {
        RecordSchedulerInvestigation(context, workspace, "attributes.begin");
        const auto targetAttrIndices = ResolveAttributeDecodeIndices(
            context.attributeTargets,
            context.frameIndex,
            context.leafPackage != nullptr ? context.leafPackage->path : BlockPath{},
            context.attributeSelection,
            workspace.StorageParams().attrParams.size());
        if (targetAttrIndices.empty()) {
            return;
        }
        for (const auto attrIndex : targetAttrIndices) {
            if (attrIndex >= workspace.StorageParams().attrParams.size()) {
                FailDecodeStage(
                    context,
                    workspace,
                    "AttrDecodeStage",
                    CodecErrorCode::InvalidInput,
                    "attribute target index is out of range");
                return;
            }
        }
        if (!m_input.HasField()) {
            if (!workspace.StorageParams().attrParams.empty()) {
                FailDecodeStage(
                    context,
                    workspace,
                    "AttrDecodeStage",
                    CodecErrorCode::MissingInput,
                    "failed to find attribute field");
            }
            return;
        }

        std::string sizeError;
        if (!ValidateAttributeFieldSize(workspace.StorageParams(), *m_input.field, &sizeError)) {
            FailDecodeStage(
                context,
                workspace,
                "AttrDecodeStage",
                CodecErrorCode::DecodeFailure,
                "failed to validate attribute field size: " + sizeError);
            return;
        }

        if (workspace.StorageParams().attrParams.empty()) {
            return;
        }

        auto* attributeKeyFrameReference = context.attributeKeyFrameReference;
        std::shared_ptr<bytestore::IByteSource> payloadOwner;
        struct PayloadGuard {
            DecodeLeafWorkspace& workspace;
            ~PayloadGuard() { workspace.ClearPreparedAttributePayload(); }
        } payloadGuard{workspace};
        std::string payloadError;
        const auto collectTiming = context.runRecords.Wants(RunRecordKind::StageTiming);
        const auto spoolStart = callback::StartTiming(collectTiming);
        if (!decodefield::PrepareLeafPackageFieldPayload(*m_input.field, workspace.CacheResourcesRef(),
                workspace.ByteStoreSessionRef(), payloadOwner, &payloadError)) {
            FailDecodeStage(context, workspace, "AttrDecodeStage", CodecErrorCode::PipelineFailure,
                "failed to prepare attribute payload: " + payloadError);
            return;
        }
        workspace.SetPreparedAttributePayload(m_input.field, payloadOwner);
        const std::string payloadMode = m_input.field->compressionType == EncodedFieldCompressionType::None
            ? "shared_source" : "prepared_store";
        if (collectTiming) {
            context.runRecords.RecordStageTiming(
                "AttrPayloadPrepareStage",
                callback::ElapsedMilliseconds(spoolStart),
                TelemetryStageCategory::General,
                std::move(payloadMode));
        }

        AttributeReferenceDecodeHelper referenceDecoder(workspace.CacheResourcesRef());

        decodeimpl::detail::AttributeDecodeTimingCallback attributeTimingCallback;
        if (collectTiming) {
            attributeTimingCallback = [&context, &workspace](const decodeimpl::detail::AttributeDecodeTimingDetail& detail) {
                context.runRecords.RecordStageTiming(
                    "AttrDecodeStage[" + std::to_string(detail.attrIndex) + "]",
                    detail.elapsedMs,
                    TelemetryStageCategory::General,
                    FormatAttributeDecodeTimingScope(detail));
                RecordSchedulerInvestigation(context, workspace, "attribute.done." + std::to_string(detail.attrIndex));
            };
        }

        std::string decodeError;
        decodeimpl::detail::AttributePayloadDecodeRuntime decodeRuntime{
            .data = decodeimpl::detail::AttributeDecodeData{
                .storageParams = workspace.StorageParams(),
                .attributeKeyFrameReference = attributeKeyFrameReference,
            },
            .cache = decodeimpl::detail::AttributeDecodeCache{
                .cacheResources = workspace.CacheResourcesRef(),
                .byteStoreSession = workspace.ByteStoreSessionRef(),
                .attributes = workspace.attributes,
            },
            .context = decodeimpl::detail::AttributeDecodeContext{
                .recordCapacitySamples = MakeCapacityRecordCallback(context.runRecords),
                .timingCallback = std::move(attributeTimingCallback),
            },
        };
        const auto decoded = decodeimpl::detail::DecodeAttributePayloadRangesToCache(
            decodeRuntime, *payloadOwner, targetAttrIndices, referenceDecoder, &decodeError);
        RecordSchedulerInvestigation(context, workspace, decoded ? "attributes.end" : "attributes.failed");
        if (!decoded) {
            FailDecodeStage(
                context,
                workspace,
                "AttrDecodeStage",
                CodecErrorCode::PipelineFailure,
                "failed to decode attribute: " + decodeError);
            return;
        }
    }

private:
    FieldDecodeInput m_input;
};

} // namespace datacodec

#endif
