#ifndef DATACODEC_CODEC_NUMERICARRAY_NUMERICDECODEMEMORYPLAN_H
#define DATACODEC_CODEC_NUMERICARRAY_NUMERICDECODEMEMORYPLAN_H

#include "DataCodec/Codec/NumericArray/NumericArrayBlockFormat.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Runtime/Execution/DecodeBlockWorkspace.h"

namespace datacodec::numericarray {

struct NumericDecodeMemoryLayout : DecodeBlockMemoryPlan {
    DecodeMemoryRange input, raw, converted, reference, scratch;
};

inline std::size_t NumericComponentWorkBytes(std::size_t n, std::size_t valueSize,
    std::span<const NumericArrayComponentLayoutParams> components, std::size_t prefix = 0u) {
    DecodeBlockMemoryPlan layout;
    layout.Append<std::uint8_t>(DecodeMemoryRegion::Temporary, prefix);
    layout.Append<std::uint8_t>(DecodeMemoryRegion::Temporary, DecodeBlockMemoryPlan::Multiply(n, valueSize));
    const bool residual = std::any_of(components.begin(), components.end(), [](const auto& component) {
        return component.bytesCodec == NumericArrayBytesCodec::IntegerDeltaLiteralRunVarint;
    });
    if (residual) { layout.Append<std::uint64_t>(DecodeMemoryRegion::Temporary, n); }
    return layout.regionBytes[2];
}

inline NumericDecodeMemoryLayout MakeNumericDecodeMemoryLayout(
    const NumericArrayStorageParams& meta, const NumericArrayBlockLayoutParams& block,
    const NumericArrayStorageParams* referenceMeta, bool geometry) {
    if (meta.dimension <= 0 || NumericArrayValueSize(meta) == 0u ||
        block.elementCount > std::numeric_limits<std::size_t>::max() ||
        block.encodedByteLength > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("invalid numeric memory shape");
    }
    using Region = DecodeMemoryRegion;
    NumericDecodeMemoryLayout result;
    const auto n = static_cast<std::size_t>(block.elementCount);
    const auto width = static_cast<std::size_t>(NumericArrayValueSize(meta));
    const auto c = static_cast<std::size_t>(meta.dimension);
    const auto tuple = DecodeBlockMemoryPlan::Multiply(c, width);
    const auto raw = DecodeBlockMemoryPlan::Multiply(n, tuple);
    result.block = block.elementOffset;
    result.input = result.Append<std::uint8_t>(Region::Input, static_cast<std::size_t>(block.encodedByteLength));
    result.raw = result.Append<std::uint8_t>(Region::Output, raw);
    if (geometry && meta.dataType != DataType::Float32) {
        auto converted = result.Append<float>(Region::Output, DecodeBlockMemoryPlan::Multiply(n, c));
        result.converted = converted;
    }
    std::size_t compute = NumericComponentWorkBytes(n, width, block.componentLayouts);
    if (block.mode == NumericArrayBlockMode::LayeredResidual) {
        for (const auto& layer : block.regionLayers) {
            if (layer.refinedElementCount > n) { throw std::length_error("residual shape exceeds numeric block"); }
            compute = std::max(compute, NumericComponentWorkBytes(static_cast<std::size_t>(layer.refinedElementCount),
                width, layer.componentLayouts, DecodeBlockMemoryPlan::Multiply(layer.refinedElementCount, tuple)));
        }
    }
    std::size_t preparation = 0u;
    if (block.referenceKind != NumericArrayReferenceKind::None) {
        if (!referenceMeta || referenceMeta->dimension != meta.dimension || referenceMeta->dataType != meta.dataType ||
            referenceMeta->elementCount == 0u) { throw std::invalid_argument("numeric reference shape is unavailable"); }
        result.reference = result.Append<std::uint8_t>(Region::Temporary, raw);
        // 参考准备和计算共用后续临时区，参考范围贯穿两个阶段
        const auto window = std::min<std::uint64_t>(referenceMeta->elementCount,
            std::max<std::size_t>(1u, kIoWindowBytes / tuple));
        preparation = DecodeBlockMemoryPlan::Multiply(DecodeBlockMemoryPlan::Add(window, 2u), tuple);
        if (block.mode == NumericArrayBlockMode::WaveletReference) {
            compute = DecodeBlockMemoryPlan::Multiply(IsIntegerNumericArrayDataType(meta.dataType)
                ? (n / 2u) * 2u : n, sizeof(double));
        }
    }
    result.Append<double>(Region::Temporary, 0u);
    result.scratch = result.Append<std::uint8_t>(Region::Temporary, std::max(compute, preparation));
    (void)result.TotalBytes();
    return result;
}

}
#endif
