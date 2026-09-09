#ifndef DATACODEC_CODEC_NUMERICARRAY_NUMERICARRAYBUFFER_H
#define DATACODEC_CODEC_NUMERICARRAY_NUMERICARRAYBUFFER_H

#include "DataCodec/Codec/NumericArray/NumericArrayLayout.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <cstddef>
#include <array>
#include <optional>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <libpressio.h>
namespace datacodec::numericarray {

struct PressioDataDeleter {
    void operator()(pressio_data* data) const {
        if (data != nullptr) {
            pressio_data_free(data);
        }
    }
};

using PressioDataHandle = std::unique_ptr<pressio_data, PressioDataDeleter>;

enum class NumericBufferSample : std::size_t {
    Raw,
    ComponentRaw,
    ComponentOwnedPayloads,
    BaseDecoded,
    ResidualRaw,
    ResidualDecoded,
    BundleScratch,
    Output,
    EncodedInput,
    Converted,
    ReferencePrimary,
    ReferenceResampled,
    ReferenceShifted,
    DecodedComponent,
    WaveletReferenceComponent,
    WaveletReferenceLow,
    WaveletReferenceHigh,
    WaveletLowDelta,
    WaveletHighDelta,
    WaveletLow,
    WaveletHigh,
    WaveletReconstructed,
    WaveletLowBlob,
    WaveletHighBlob,
    ReferencePreparedDelta,
    OrdinaryCandidate,
    ReferenceCandidate,
    ReaderOrder,
    ReferenceReaderOrder,
    Count,
};

// 每块只携带固定取样记录，worker 不导出报告，不保存数组引用
struct NumericArrayBlockCapacitySamples {
    std::array<BufferCapacitySample, static_cast<std::size_t>(NumericBufferSample::Count)> values{{
        BufferCapacitySample{"numeric.raw"},
        BufferCapacitySample{"numeric.component_raw"},
        BufferCapacitySample{"numeric.component_owned_payload_arrays"},
        BufferCapacitySample{"numeric.base_decoded"},
        BufferCapacitySample{"numeric.residual_raw"},
        BufferCapacitySample{"numeric.residual_decoded"},
        BufferCapacitySample{"numeric.bundle_scratch"},
        BufferCapacitySample{"numeric.output"},
        BufferCapacitySample{"numeric.encoded_input"},
        BufferCapacitySample{"numeric.converted"},
        BufferCapacitySample{"numeric.reference_primary"},
        BufferCapacitySample{"numeric.reference_resampled"},
        BufferCapacitySample{"numeric.reference_shifted"},
        BufferCapacitySample{"numeric.decoded_component"},
        BufferCapacitySample{"wavelet.reference_component"},
        BufferCapacitySample{"wavelet.reference_low"},
        BufferCapacitySample{"wavelet.reference_high"},
        BufferCapacitySample{"wavelet.low_delta"},
        BufferCapacitySample{"wavelet.high_delta"},
        BufferCapacitySample{"wavelet.low"},
        BufferCapacitySample{"wavelet.high"},
        BufferCapacitySample{"wavelet.reconstructed"},
        BufferCapacitySample{"wavelet.low_blob"},
        BufferCapacitySample{"wavelet.high_blob"},
        BufferCapacitySample{"reference.prepared_delta"},
        BufferCapacitySample{"reference.ordinary_candidate"},
        BufferCapacitySample{"reference.encoded_candidate"},
        BufferCapacitySample{"numeric.reader_order"},
        BufferCapacitySample{"numeric.reference_reader_order"},
    }};

    template<class T>
    void Observe(const NumericBufferSample kind, const T& storage) noexcept {
        values[static_cast<std::size_t>(kind)].Observe(storage);
    }
};

struct NumericArrayBufferLayout : public NumericArrayLayout {
    std::vector<std::size_t> shape;

    [[nodiscard]] std::size_t ValueCount() const {
        if (!this->shape.empty()) {
            std::size_t count = 1u;
            for (const auto extent : this->shape) {
                if (extent == 0u) {
                    return 0u;
                }
                count = validation::SaturatingMulSizeT(count, extent);
            }
            return count;
        }
        return NumericArrayLayout::ValueCount();
    }

    [[nodiscard]] std::size_t ByteCount() const {
        return validation::SaturatingMulSizeT(ValueCount(), valueSize);
    }

    [[nodiscard]] std::vector<std::size_t> BuildShape() const {
        if (!this->shape.empty()) {
            return this->shape;
        }
        return NumericArrayLayout::BuildShape();
    }
};

struct NumericArrayBufferView {
    const void* data{nullptr};
    NumericArrayBufferLayout layout;
};

struct MutableNumericArrayBufferView {
    void* data{nullptr};
    NumericArrayBufferLayout layout;
};

struct NumericArraySegmentSpec {
    std::size_t elementOffset{0};
    std::size_t elementCount{0};
    CompressorConfig compressor;
};

class NumericArrayEncodedBytes {
public:
    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept {
        if (this->pressioData != nullptr) {
            return this->pressioBytes;
        }
        return std::span<const std::uint8_t>(this->ownedBytes.data(), this->ownedBytes.size());
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return this->Bytes().size();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return this->Bytes().empty();
    }

    // 库内部结果容量未知，自有 vector 按真实 capacity 单独报告
    [[nodiscard]] std::optional<std::uint64_t> OwnedCapacityBytes() const noexcept {
        if (pressioData != nullptr) { return std::nullopt; }
        return static_cast<std::uint64_t>(ownedBytes.capacity());
    }

    void Reset() {
        std::vector<std::uint8_t>().swap(this->ownedBytes);
        this->pressioBytes = {};
        this->pressioData.reset();
    }

    void TakeVector(std::vector<std::uint8_t>&& bytes) {
        this->Reset();
        this->ownedBytes = std::move(bytes);
    }

    void CopyFrom(const std::span<const std::uint8_t> bytes) {
        this->Reset();
        this->ownedBytes.assign(bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> MaterializeVector() const {
        const auto bytes = this->Bytes();
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    void TakePressioData(PressioDataHandle data, const std::span<const std::uint8_t> bytes) {
        this->Reset();
        this->pressioBytes = bytes;
        this->pressioData = std::move(data);
    }

private:
    std::vector<std::uint8_t> ownedBytes;
    PressioDataHandle pressioData;
    std::span<const std::uint8_t> pressioBytes;
};

struct NumericArrayCompressedSegment {
    std::size_t elementOffset{0};
    NumericArrayBufferLayout layout;
    CompressorConfig compressor;
    std::vector<std::uint8_t> bytes;
};

inline NumericArrayBufferLayout MakeNumericArrayComponentLayout(
    const DataType dataType,
    const std::size_t valueSize,
    const std::size_t elementCount) {
    NumericArrayBufferLayout layout;
    layout.dataType = dataType;
    layout.valueSize = valueSize;
    layout.elementCount = elementCount;
    layout.componentCount = 1u;
    layout.shape = {elementCount};
    return layout;
}

inline NumericArrayBufferLayout MakeNumericArrayBufferLayout(
    const NumericArrayLayout& logical,
    std::vector<std::size_t> shape = {}) {
    NumericArrayBufferLayout layout;
    layout.dataType = logical.dataType;
    layout.valueSize = logical.valueSize;
    layout.elementCount = logical.elementCount;
    layout.componentCount = logical.componentCount;
    layout.shape = std::move(shape);
    return layout;
}

} // namespace datacodec::numericarray

#endif
