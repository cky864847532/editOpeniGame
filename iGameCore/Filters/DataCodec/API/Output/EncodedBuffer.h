#ifndef DATACODEC_API_OUTPUT_ENCODEDBUFFER_H
#define DATACODEC_API_OUTPUT_ENCODEDBUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace datacodec {
namespace bytestore { class MemoryStore; }
class MemoryByteRangeOutput;

// 编码结果只携带存储 owner，执行根结束后仍可读取
class EncodedBuffer final {
public:
    EncodedBuffer() noexcept = default;
    EncodedBuffer(const EncodedBuffer&) = delete;
    EncodedBuffer& operator=(const EncodedBuffer&) = delete;
    EncodedBuffer(EncodedBuffer&&) noexcept = default;
    EncodedBuffer& operator=(EncodedBuffer&&) noexcept = default;
    ~EncodedBuffer() = default;

    [[nodiscard]] const std::uint8_t* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0u; }
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept;

private:
    friend class MemoryByteRangeOutput;
    explicit EncodedBuffer(std::shared_ptr<const bytestore::MemoryStore> owner) noexcept
        : m_owner(std::move(owner)) {}
    std::shared_ptr<const bytestore::MemoryStore> m_owner;
};

}

#endif
