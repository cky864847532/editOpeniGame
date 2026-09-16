#ifndef DATACODEC_API_INPUT_ENCODEDINPUT_H
#define DATACODEC_API_INPUT_ENCODEDINPUT_H

#include "DataCodec/API/Output/EncodedBuffer.h"
#include "DataCodec/Common/Views/InputMemoryView.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace datacodec {
class EncodedInputAccess;

// 路径只描述来源，共享字节保持只读，具体范围 IO 由核心创建
class EncodedInput final {
public:
    EncodedInput() noexcept = default;
    [[nodiscard]] static EncodedInput File(std::filesystem::path path);
    [[nodiscard]] static EncodedInput Memory(std::shared_ptr<const void> owner,
        std::span<const std::uint8_t> bytes, std::optional<std::uint64_t> capacity = {});
    [[nodiscard]] static EncodedInput Memory(std::shared_ptr<const std::vector<std::uint8_t>> bytes);
    [[nodiscard]] static EncodedInput Memory(std::shared_ptr<const EncodedBuffer> bytes);
    [[nodiscard]] static EncodedInput Memory(EncodedBuffer bytes);
    [[nodiscard]] static EncodedInput BrowserFile(std::uint32_t id, std::uint64_t size,
        std::shared_ptr<const void> owner = {});
    [[nodiscard]] explicit operator bool() const noexcept { return m_impl != nullptr; }
    [[nodiscard]] InputMemoryObservation ObserveMemory() const;
private:
    friend class EncodedInputAccess;
    struct Impl;
    explicit EncodedInput(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}
    std::shared_ptr<const Impl> m_impl;
};

} // 命名空间 datacodec
#endif
