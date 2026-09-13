#ifndef DATACODEC_PLATFORM_WASM_WASMBROWSERFILEBYTERANGEREADER_H
#define DATACODEC_PLATFORM_WASM_WASMBROWSERFILEBYTERANGEREADER_H

#include "DataCodec/Storage/ByteIO/ByteRange.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace datacodec::wasm {

// 宿主加载随平台一起提供的 datacodec_browser_files.js
// Module.dataCodecBrowserFiles 必须由 createDataCodecBrowserFiles 创建
// fileId 来自 register，所有读取和会话消费完成后才可调用 release
// 主线程读取依赖 Asyncify，后台读取依赖主线程事件循环及 pthread
class WasmBrowserFileByteRangeReader final : public IByteRangeReader {
public:
    WasmBrowserFileByteRangeReader(
        std::uint32_t fileId,
        std::uint64_t byteSize);

    [[nodiscard]] std::uint64_t ByteSize() const noexcept override;
    bool ReadAt(
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error = nullptr) override;

    [[nodiscard]] std::uint32_t FileId() const noexcept;
    [[nodiscard]] std::string LastReadError() const;

private:
    bool FailRead(std::string message, std::string* error);
    std::uint32_t m_fileId{0u};
    std::uint64_t m_byteSize{0u};
    mutable std::mutex m_errorMutex;
    std::string m_lastReadError;
};

[[nodiscard]] std::shared_ptr<IByteRangeReader> CreateWasmBrowserFileByteRangeReader(
    std::uint32_t fileId,
    std::uint64_t byteSize,
    std::string* error = nullptr);

void ReleaseWasmBrowserFile(std::uint32_t fileId) noexcept;

} // namespace datacodec::wasm

#endif
