#ifndef DATACODEC_API_OUTPUT_DECODEDBUFFER_H
#define DATACODEC_API_OUTPUT_DECODEDBUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace datacodec {
class DecodedBufferAccess;

// 完成结果仅共享数组分配，独立于预算、缓存及执行根
class DecodedBuffer final {
public:
    DecodedBuffer() = default;
    DecodedBuffer(const DecodedBuffer&) = default;
    DecodedBuffer& operator=(const DecodedBuffer&) = default;
    DecodedBuffer(DecodedBuffer&& other) noexcept
        : m_bytes(std::move(other.m_bytes)), m_size(std::exchange(other.m_size, 0u)),
          m_capacity(std::exchange(other.m_capacity, 0u)) {}
    DecodedBuffer& operator=(DecodedBuffer&& other) noexcept {
        if (this != &other) {
            m_bytes = std::move(other.m_bytes);
            m_size = std::exchange(other.m_size, 0u);
            m_capacity = std::exchange(other.m_capacity, 0u);
        }
        return *this;
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return m_bytes.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0u; }
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return {data(), size()}; }
    [[nodiscard]] std::shared_ptr<const void> Owner() const noexcept { return m_bytes; }

private:
    friend class DecodedBufferAccess;
    std::shared_ptr<std::uint8_t[]> m_bytes;
    std::size_t m_size{0u};
    std::size_t m_capacity{0u};
};

} // 命名空间 datacodec
#endif
