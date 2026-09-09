#ifndef DATACODEC_PLATFORM_WASM_WASMBROWSERFILEBYTERANGEREADER_H
#define DATACODEC_PLATFORM_WASM_WASMBROWSERFILEBYTERANGEREADER_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"

#include <cstdint>
#include <memory>
#include <string>

namespace datacodec::wasm {

class WasmBrowserFileByteRangeReader final : public IByteRangeReader {
public:
    WasmBrowserFileByteRangeReader(
        std::uint32_t fileId,
        std::uint64_t byteSize);

    [[nodiscard]] std::uint64_t ByteSize() const noexcept override;
    [[nodiscard]] ByteRangePrefetchResult PrefetchRange(
        std::uint64_t offset,
        std::uint64_t byteSize) const override;
    bool ReadAt(
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error = nullptr) override;

    [[nodiscard]] std::uint32_t FileId() const noexcept;

private:
    std::uint32_t m_fileId{0u};
    std::uint64_t m_byteSize{0u};
};

[[nodiscard]] std::shared_ptr<IByteRangeReader> CreateWasmBrowserFileByteRangeReader(
    std::uint32_t fileId,
    std::uint64_t byteSize,
    std::string* error = nullptr);

void ReleaseWasmBrowserFile(std::uint32_t fileId) noexcept;

} // namespace datacodec::wasm

#endif
