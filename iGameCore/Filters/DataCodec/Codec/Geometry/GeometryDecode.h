#ifndef DATACODEC_CODEC_GEOMETRY_GEOMETRYDECODE_H
#define DATACODEC_CODEC_GEOMETRY_GEOMETRYDECODE_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/ReferenceCacheNumericArraySource.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockReader.h"
#include "DataCodec/Codec/Reference/DecodedReference.h"
#include "DataCodec/Codec/Reference/NumericArrayReferenceBytes.h"
#include "DataCodec/Codec/Reference/ReferenceCodec.h"
#include "DataCodec/Codec/Reference/PreparedDecodeReference.h"
#include "DataCodec/Common/DataCodecError.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

using GeometryDecodeResult = CodecStatus;

struct GeometryDecodeData {
    const GeometryStorageParams& meta;
    const DecodedGeometryReference* keyFrameReference{nullptr};
    std::uint64_t payloadBytes{0u};
};

struct GeometryDecodeCache {
    const CacheResources& cacheResources;
    bytestore::ByteStoreSession& byteStoreSession;
    DecodedGeometryCache& geometry;
    DecodedGeometryReferenceCache* referenceCache{nullptr};
    IDecodeAdapter* destination{nullptr};
};

struct GeometryDecodeRuntime {
    GeometryDecodeData data;
    GeometryDecodeCache cache;
    callback::CapacityCallback recordCapacitySamples;
};

namespace detail {

inline GeometryDecodeResult MakeGeometryDecodeSuccess() {
    return CodecStatus::Ok();
}

inline GeometryDecodeResult MakeGeometryDecodeFailure(
    const CodecErrorCode code,
    std::string message) {
    return CodecStatus::Failure(code, std::move(message));
}

struct GeometryDecodedBlock {
    NumericArrayBlockHeader header;
    FixedScratchBuffer raw;
    std::optional<numericarray::NumericArrayBlockCapacitySamples> capacitySamples;
};

inline bool ComputeGeometryDecodedBlock(
    const GeometryDecodeRuntime& runtime, const numericarray::NumericArrayBlockParams& sourceParams,
    const numericarray::NumericArrayBlockPayload& input, GeometryDecodedBlock& output,
    WorkerContext& worker, DecodeBlockWorkspace& workspace, std::string* error) {
    auto scratchBytes = workspace.View<std::uint8_t>(input.memory.scratch);
    scratchBytes.resize(scratchBytes.capacity());
    ArrayWorkspace scratch(scratchBytes.Span());
    auto params = sourceParams;
    params.workspace = &scratch;
    params.capacitySamples = output.capacitySamples ? &*output.capacitySamples : nullptr;
    if (params.capacitySamples != nullptr) {
        params.capacitySamples->Observe(numericarray::NumericBufferSample::EncodedInput, input.bytes.Bytes());
    }
    output.header = input.header;
    output.raw = FixedScratchBuffer(workspace.View<std::uint8_t>(input.memory.raw));
    auto& decoded = output.raw.Bytes();
    if (input.header.codecId == NumericArrayReferenceCodecId::NonReference) {
        if (input.header.referenceKind != NumericArrayReferenceKind::None ||
            !numericarray::ResolveDecodedNumericArrayBlockBytes(
                params, input, decoded, error, &worker.NumericCompressor())) { return false; }
    } else {
        if (runtime.data.meta.codecType != EncodedFieldCodecType::Delta) {
            return validation::AssignError(error, "ordinary geometry field contains a reference block");
        }
        const auto block = numericarray::MakeParsedBlockView(input);
        if (!runtime.data.keyFrameReference || !runtime.data.keyFrameReference->store) {
            return validation::AssignError(error, "geometry reference is unavailable");
        }
        GeometryStorageParams referenceMeta;
        numericarray::NumericArraySource source;
        numericarray::NumericArrayReader reader;
        auto reference = FixedScratchBuffer(workspace.View<std::uint8_t>(input.memory.reference));
        if (!BuildGeometryReferenceCacheNumericArraySource(runtime.data.keyFrameReference->store,
                runtime.data.meta, referenceMeta, source, error) ||
            !numericarray::BuildNumericArrayReader(source, reader, error) ||
            !PrepareDecodeReference(reader, referenceMeta, runtime.data.meta, input.header,
                reference.Bytes(), scratch, error)) { return false; }
        if (params.capacitySamples != nullptr) {
            params.capacitySamples->Observe(numericarray::NumericBufferSample::ReferencePrimary, reference.Bytes());
        }
        const auto* codec = ResolveNumericArrayReferenceCodec(input.header.codecId);
        if (codec == nullptr) { return validation::AssignError(error, "unsupported geometry reference codec"); }
        if (!codec->DecodeBlock(NumericArrayReferenceCodecDecodeInput{
                .meta = runtime.data.meta, .block = block, .referenceBytes = reference.Span(),
                .referenceElementOffset = 0u,
                .compressorState = &worker.NumericCompressor(),
                .capacitySamples = params.capacitySamples, .workspace = &scratch}, decoded, error)) { return false; }
    }
    std::size_t expected = 0u;
    if (!numericarray::ResolveNumericArrayBlockRawByteCount(params, input.header.elementCount, expected, error) ||
        decoded.size() != expected) {
        return validation::AssignError(error, "geometry decoded block does not match its logical shape");
    }
    if (params.capacitySamples != nullptr) {
        params.capacitySamples->Observe(numericarray::NumericBufferSample::Output, output.raw.Bytes());
    }
    return true;
}

} // 几何解码内部实现

template<typename TStream>
inline GeometryDecodeResult DecodeGeometryBlocks(GeometryDecodeRuntime& runtime, TStream& stream) {
    const auto& meta = runtime.data.meta;
    auto& root = runtime.cache.cacheResources.Run();
    auto& geometry = runtime.cache.geometry;
    auto* referenceCache = runtime.cache.referenceCache;
    numericarray::NumericArrayBlockParams params;
    std::string error;
    if (!numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params, &error) ||
        !numericarray::ValidateNumericArrayBlockParams(params, &error)) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, std::move(error));
    }
    if (meta.codecType != EncodedFieldCodecType::NumericArrayBlocks && meta.codecType != EncodedFieldCodecType::Delta) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, "unsupported geometry field codec");
    }
    const auto begin = stream.Position();
    std::uint64_t end = 0u, expectedPayload = 0u;
    if (!validation::CheckedAddU64(begin, runtime.data.payloadBytes, end, "geometry payload range", &error)) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, std::move(error));
    }
    for (const auto& layout : meta.blockLayouts) {
        if (!validation::CheckedAddU64(expectedPayload, layout.encodedByteLength, expectedPayload,
                "geometry layout payload bytes", &error)) {
            return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, std::move(error));
        }
    }
    if (expectedPayload != runtime.data.payloadBytes) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, "geometry payload does not match block layouts");
    }
    numericarray::NumericDecodeCursor<TStream> cursor{
        stream, params, meta.blockLayouts, runtime.cache.cacheResources};
    if (!cursor.Prepare(&error)) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, std::move(error));
    }
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return detail::MakeGeometryDecodeFailure(CodecErrorCode::PipelineFailure, "geometry preparation was stopped"); }
    // 完整目标在块流前取得容量，两个真实数组分别持有 owner
    if (!geometry.Initialize(params.elementCount, params.componentCount, params.dataType, runtime.cache.byteStoreSession, &error, runtime.cache.destination) ||
        (referenceCache != nullptr &&
            !referenceCache->BeginGeometry(meta, runtime.cache.byteStoreSession, &error))) {
        geometry.Release();
        if (referenceCache != nullptr) { referenceCache->Reset(); }
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::DecodeFailure, std::move(error));
    }
    if (!root.SynchronizeMemoryAfterPreparation()) {
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::PipelineFailure, "geometry preparation was cancelled");
    }
    const auto finish = [&] {
        if (stream.Position() != end) {
            return validation::AssignError(&error, "geometry decode consumed an unexpected payload size");
        }
        if (referenceCache != nullptr && !referenceCache->EndGeometry(&error)) { return false; }
        if (!geometry.bytes || !geometry.bytes->Seal(&error)) { return false; }
        geometry.complete = true;
        return true;
    };
    if (params.elementCount == 0u) {
        if (finish()) { return detail::MakeGeometryDecodeSuccess(); }
        geometry.Release();
        if (referenceCache != nullptr) { referenceCache->Reset(); }
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::PipelineFailure, std::move(error));
    }
    std::size_t sourceTupleBytes = 0u, targetTupleBytes = 0u;
    if (!validation::CheckedMulSizeT(params.componentCount, params.valueSize, sourceTupleBytes,
            "geometry source tuple", &error) ||
        !validation::CheckedMulSizeT(params.componentCount, params.valueSize, targetTupleBytes,
            "geometry output tuple", &error)) {
        geometry.Release();
        if (referenceCache != nullptr) { referenceCache->Reset(); }
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::InvalidInput, std::move(error));
    }
    ParamSize committedElements = 0u;
    phase.reset();
    numericarray::NumericDecodeMemoryLayout nextMemory;
    const bool success = RunOrderedBlocks<numericarray::NumericArrayBlockPayload, detail::GeometryDecodedBlock>(
        root, [&] { return cursor.HasMore(); },
        [&](numericarray::NumericArrayBlockPayload& input, const SlotLease& slot, DecodeBlockWorkspace& workspace) {
            // driver 串行推进唯一字段流，外层解压使用同一槽位的计算额度
            return RunTerminalWork(root, slot, [&](WorkerContext&) { return cursor.ReadNext(input, nextMemory, workspace, &error); });
        },
        [&](const numericarray::NumericArrayBlockPayload& input, detail::GeometryDecodedBlock& output, WorkerContext& worker, DecodeBlockWorkspace& workspace) {
            std::string localError;
            if (runtime.recordCapacitySamples) { output.capacitySamples.emplace(); }
            if (!detail::ComputeGeometryDecodedBlock(runtime, params, input, output, worker, workspace, &localError)) {
                root.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
                    "geometry-block-decode", "ComputeGeometryDecodedBlock", localError));
                return false;
            }
            return true;
        },
        [&](detail::GeometryDecodedBlock& output) {
            if (output.header.elementOffset != committedElements) {
                return validation::AssignError(&error, "geometry commit order is not contiguous");
            }
            if (referenceCache != nullptr) {
                const auto windowElements = std::max<std::size_t>(1u, kIoWindowBytes / sourceTupleBytes);
                for (std::size_t offset = 0u; offset < output.header.elementCount;) {
                    const auto count = std::min<std::size_t>(windowElements, output.header.elementCount - offset);
                    if (!referenceCache->WriteRange(output.header.elementOffset + offset, count,
                            output.raw.Span().data() + offset * sourceTupleBytes, count * sourceTupleBytes, &error)) {
                        return false;
                    }
                    offset += count;
                }
            }
            const auto points = output.raw.Span();
            std::uint64_t byteOffset = 0u;
            if (!validation::CheckedMulU64(committedElements, targetTupleBytes, byteOffset,
                    "geometry output offset", &error)) { return false; }
            for (std::size_t offset = 0u; offset < points.size();) {
                const auto bytes = std::min(kIoWindowBytes, points.size() - offset);
                if (!geometry.bytes->WriteBytesAt(byteOffset + offset, points.subspan(offset, bytes), &error)) { return false; }
                offset += bytes;
            }
            committedElements += output.header.elementCount;
            if (output.capacitySamples && runtime.recordCapacitySamples) {
                try { runtime.recordCapacitySamples(output.capacitySamples->values); }
                catch (...) { root.RecordDiagnosticExportFailure(); }
            }
            return committedElements != params.elementCount || finish();
        }, false, [&] { return cursor.NextWorkType(ResourceWorkPath::GeometryDecode); }, [&] {
            const auto* referenceMeta = runtime.data.keyFrameReference && runtime.data.keyFrameReference->store
                ? &runtime.data.keyFrameReference->store->StorageParams() : nullptr;
            nextMemory = numericarray::MakeNumericDecodeMemoryLayout(meta, meta.blockLayouts[cursor.nextBlock],
                referenceMeta, false);
            return nextMemory;
        });
    if (!success) {
        geometry.Release();
        if (referenceCache != nullptr) { referenceCache->Reset(); }
        if (error.empty()) { error = "geometry block flow failed"; }
        return detail::MakeGeometryDecodeFailure(CodecErrorCode::DecodeFailure, std::move(error));
    }
    return detail::MakeGeometryDecodeSuccess();
}

} // DataCodec 命名空间

#endif
