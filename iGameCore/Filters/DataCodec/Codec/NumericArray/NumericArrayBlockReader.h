#ifndef DATACODEC_CODEC_NUMERICARRAY_NUMERICARRAYBLOCKREADER_H
#define DATACODEC_CODEC_NUMERICARRAY_NUMERICARRAYBLOCKREADER_H

#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockDecode.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Codec/NumericArray/NumericDecodeMemoryPlan.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace datacodec {
namespace numericarray {

struct NumericArrayBlockPayload {
    NumericArrayBlockHeader header;
    CompressorConfig backgroundCompressor;
    ParamSize backgroundEncodedByteLength{0};
    std::vector<NumericArrayComponentLayoutParams> componentLayouts;
    std::vector<NumericArrayRegionLayerLayoutParams> regionLayers;
    std::vector<double> alpha;
    std::vector<double> beta;
    FixedScratchBuffer bytes;
    NumericDecodeMemoryLayout memory;
};

inline ParsedNumericArrayBlock MakeParsedBlockView(const NumericArrayBlockPayload& input) {
    ParsedNumericArrayBlock block;
    block.header = input.header;
    block.backgroundCompressor = input.backgroundCompressor;
    block.backgroundEncodedByteLength = input.backgroundEncodedByteLength;
    block.componentLayouts = input.componentLayouts;
    block.regionLayers = input.regionLayers;
    block.alpha = input.alpha;
    block.beta = input.beta;
    block.bytes = input.bytes.Span();
    return block;
}

template<typename TStream>
inline bool ReadNumericArrayBlockPayload(
    TStream& stream,
    const std::size_t componentCount,
    const NumericArrayBlockLayoutParams& layout,
    const CacheResources& runtime,
    NumericArrayBlockPayload& block, const NumericDecodeMemoryLayout& memory,
    DecodeBlockWorkspace& workspace,
    std::string* error = nullptr) {
    block = {};
    if (!MakeNumericArrayBlockHeader(layout, block.header, error)) {
        return false;
    }
    block.backgroundCompressor = layout.backgroundCompressor;
    block.backgroundEncodedByteLength = layout.backgroundEncodedByteLength;
    block.componentLayouts = layout.componentLayouts;
    block.regionLayers = layout.regionLayers;

    if (block.header.mode == NumericArrayBlockMode::AffineReference) {
        if (layout.alpha.size() != componentCount || layout.beta.size() != componentCount) {
            return validation::AssignError(error, "numeric array affine block layout component count mismatch");
        }
        block.alpha = layout.alpha;
        block.beta = layout.beta;
    }

    block.memory = memory;
    block.bytes = FixedScratchBuffer(workspace.View<std::uint8_t>(memory.input));
    auto& bytes = block.bytes.Bytes();
    for (std::size_t offset = 0u; offset < bytes.size();) {
        if (runtime.Run().Stopped()) { return validation::AssignError(error, "numeric block read cancelled"); }
        const auto count = std::min(kIoWindowBytes, bytes.size() - offset);
        if (!stream.ReadBytes(bytes.data() + offset, count, error)) { return false; }
        if (runtime.Run().Stopped()) { return validation::AssignError(error, "numeric block read cancelled"); }
        offset += count;
    }
    return true;
}

template<class TStream>
struct NumericDecodeCursor {
    TStream& stream;
    const NumericArrayBlockParams& params;
    const std::vector<NumericArrayBlockLayoutParams>& layouts;
    const CacheResources& resources;
    std::size_t nextBlock{0u};

    bool Prepare(std::string* error) {
        ParamSize elements = 0u;
        for (const auto& layout : layouts) {
            NumericArrayBlockHeader header;
            std::size_t rawBytes = 0u;
            if (!MakeNumericArrayBlockHeader(layout, header, error) ||
                !ResolveNumericArrayBlockRawByteCount(params, header.elementCount, rawBytes, error)) { return false; }
            if (header.elementCount == 0u || header.elementOffset != elements || elements > params.elementCount ||
                header.elementCount > kSpatialBlockElementCount || header.elementCount > params.elementCount - elements) {
                return validation::AssignError(error, "numeric array block layouts are not a contiguous complete field");
            }
            elements += header.elementCount;
        }
        return elements == params.elementCount ||
            validation::AssignError(error, "numeric array block layouts do not cover the full field");
    }
    bool HasMore() const noexcept { return nextBlock < layouts.size(); }
    ResourceWorkType NextWorkType(const ResourceWorkPath path) const noexcept {
        const auto& layout = layouts[nextBlock];
        ResourceWorkType key{
            .path = path,
            .codec = static_cast<std::uint32_t>(layout.bytesCodec),
            .scalar = static_cast<std::uint32_t>(params.dataType),
            .components = static_cast<std::uint32_t>(params.componentCount),
            .blockElements = kSpatialBlockElementCount,
            .referencePath = (static_cast<std::uint32_t>(layout.mode) << 8u) |
                static_cast<std::uint32_t>(layout.referenceKind),
        };
        const auto includeCodec = [&](const NumericArrayBytesCodec codec) {
            switch (codec) {
            case NumericArrayBytesCodec::RawBytes: key.componentCodecMask |= 1u; break;
            case NumericArrayBytesCodec::NumericArrayCodec: key.componentCodecMask |= 2u; break;
            case NumericArrayBytesCodec::IntegerDeltaLiteralRunVarint: key.componentCodecMask |= 8u; break;
            }
        };
        includeCodec(layout.bytesCodec);
        for (const auto& component : layout.componentLayouts) { includeCodec(component.bytesCodec); }
        for (const auto& layer : layout.regionLayers) {
            for (const auto& component : layer.componentLayouts) { includeCodec(component.bytesCodec); }
        }
        return key;
    }
    bool ReadNext(NumericArrayBlockPayload& block, const NumericDecodeMemoryLayout& memory,
        DecodeBlockWorkspace& workspace, std::string* error) {
        if (!HasMore()) { return validation::AssignError(error, "numeric decode cursor is exhausted"); }
        if (!ReadNumericArrayBlockPayload(stream, params.componentCount, layouts[nextBlock], resources, block, memory, workspace, error)) {
            return false;
        }
        ++nextBlock;
        return true;
    }
};

inline bool ResolveDecodedNumericArrayBlockBytes(
    const numericarray::NumericArrayBlockParams& params,
    const NumericArrayBlockPayload& block,
    MutableArray<std::uint8_t> decodedBytes,
    std::string* error = nullptr, numericarray::NumericArrayCompressorState* compressorState = nullptr) {
    decodedBytes.clear();
    if (!numericarray::ValidateNumericArrayBlockParams(params, error)) {
        return false;
    }

    if (block.header.mode == NumericArrayBlockMode::LayeredResidual) {
        return numericarray::ResolveDecodedLayeredResidualNumericArrayBlockBytes(
            params,
            block.header.elementCount,
            block.backgroundCompressor,
            block.backgroundEncodedByteLength,
            block.componentLayouts,
            block.regionLayers,
            block.bytes.Span(),
            decodedBytes,
            error, compressorState);
    }
    return numericarray::ResolveDecodedNumericArrayBlockBytes(
        params,
        block.backgroundCompressor,
        block.header.elementCount,
        block.header.bytesCodec,
        block.componentLayouts,
        block.bytes.Span(),
        decodedBytes,
        error, compressorState);
}

} // 数值数组命名空间
} // DataCodec 命名空间

#endif
