#ifndef DATACODEC_CODEC_REFERENCE_PREPAREDDECODEREFERENCE_H
#define DATACODEC_CODEC_REFERENCE_PREPAREDDECODEREFERENCE_H

#include "DataCodec/Codec/NumericArray/NumericArrayReader.h"
#include "DataCodec/Storage/ByteIO/ArrayWorkspace.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include <cmath>

namespace datacodec {

inline bool PrepareDecodeReference(const numericarray::NumericArrayReader& reader,
    const NumericArrayStorageParams& source, const NumericArrayStorageParams& target,
    const NumericArrayBlockHeader& block, MutableArray<std::uint8_t> output,
    ArrayWorkspace& workspace, std::string* error) {
    ArrayWorkspace::Scope scope(&workspace);
    const auto tuple = DecodeBlockMemoryPlan::Multiply(static_cast<std::size_t>(target.dimension),
        static_cast<std::size_t>(NumericArrayValueSize(target)));
    const auto n = static_cast<std::size_t>(block.elementCount);
    output.resize(DecodeBlockMemoryPlan::Multiply(n, tuple));
    if (n == 0u) { return true; }
    if (source.elementCount == 0u || target.elementCount == 0u || source.dimension != target.dimension ||
        source.dataType != target.dataType) { return validation::AssignError(error, "reference shape mismatch"); }
    const auto shift = block.codecId == NumericArrayReferenceCodecId::Predictor &&
        block.referenceKind == NumericArrayReferenceKind::TemporalKeyFrame ? block.predictorOffset : 0;
    const auto targetIndex = [&](std::size_t i) {
        const auto index = static_cast<std::int64_t>(block.elementOffset + i) + shift;
        return static_cast<std::size_t>(std::clamp<std::int64_t>(index, 0,
            static_cast<std::int64_t>(target.elementCount - 1u)));
    };
    if (source.elementCount == target.elementCount && targetIndex(n - 1u) - targetIndex(0u) + 1u == n) {
        return reader.ReadPreparedElements(targetIndex(0u), n, output, error);
    }
    const auto tuples = static_cast<std::size_t>(std::min<std::uint64_t>(source.elementCount,
        std::max<std::size_t>(1u, kIoWindowBytes / tuple)));
    auto window = workspace.Take<std::uint8_t>(DecodeBlockMemoryPlan::Multiply(tuples, tuple));
    auto left = workspace.Take<std::uint8_t>(tuple);
    auto right = workspace.Take<std::uint8_t>(tuple);
    left.resize(tuple); right.resize(tuple);
    std::size_t begin = 0u, count = 0u;
    const auto readTuple = [&](std::size_t index, FixedArrayView<std::uint8_t>& value) {
        if (index < begin || index - begin >= count) {
            count = std::min<std::size_t>(tuples, source.elementCount - index);
            window.resize(DecodeBlockMemoryPlan::Multiply(count, tuple));
            if (!reader.ReadPreparedElements(index, count, window.Span(), error)) { return false; }
            begin = index;
        }
        std::memcpy(value.data(), window.data() + (index - begin) * tuple, tuple);
        return true;
    };
    for (std::size_t i = 0u; i < n; ++i) {
        const auto index = targetIndex(i);
        const auto position = target.elementCount <= 1u || source.elementCount <= 1u ? 0.0 :
            static_cast<double>(index) / static_cast<double>(target.elementCount - 1u) *
                static_cast<double>(source.elementCount - 1u);
        const auto l = std::min<std::size_t>(static_cast<std::size_t>(std::floor(position)), source.elementCount - 1u);
        const auto r = std::min<std::size_t>(l + 1u, source.elementCount - 1u);
        if (!readTuple(l, left)) { return false; }
        if (source.elementCount == target.elementCount) {
            std::memcpy(output.data() + i * tuple, left.data(), tuple);
            continue;
        }
        if (!readTuple(r, right)) { return false; }
        const auto weight = position - static_cast<double>(l);
        const auto interpolate = [&]<class T>() {
            for (std::size_t c = 0u; c < static_cast<std::size_t>(target.dimension); ++c) {
                T a{}, b{};
                std::memcpy(&a, left.data() + c * sizeof(T), sizeof(T));
                std::memcpy(&b, right.data() + c * sizeof(T), sizeof(T));
                const auto value = static_cast<T>(static_cast<double>(a) * (1.0 - weight) + static_cast<double>(b) * weight);
                std::memcpy(output.data() + i * tuple + c * sizeof(T), &value, sizeof(T));
            }
        };
        if (target.dataType == DataType::Float32) { interpolate.template operator()<float>(); }
        else if (target.dataType == DataType::Float64) { interpolate.template operator()<double>(); }
        else { return validation::AssignError(error, "resampled reference requires floating point values"); }
    }
    return true;
}

}
#endif
