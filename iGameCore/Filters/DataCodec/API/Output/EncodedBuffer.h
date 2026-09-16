#ifndef DATACODEC_API_OUTPUT_ENCODEDBUFFER_H
#define DATACODEC_API_OUTPUT_ENCODEDBUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace datacodec {
class MemoryByteRangeOutput;

// 编码结果只携带存储 owner，执行根结束后仍可读取
class EncodedBuffer final {
public:
    EncodedBuffer() noexcept = default;
    EncodedBuffer(const EncodedBuffer&) = delete;
    EncodedBuffer& operator=(const EncodedBuffer&) = delete;
    EncodedBuffer(EncodedBuffer&& other) noexcept
        : m_bytes(std::move(other.m_bytes)), m_size(std::exchange(other.m_size, 0u)),
          m_capacity(std::exchange(other.m_capacity, 0u)) {}
    EncodedBuffer& operator=(EncodedBuffer&& other) noexcept {
        if (this != &other) {
            m_bytes = std::move(other.m_bytes);
            m_size = std::exchange(other.m_size, 0u);
            m_capacity = std::exchange(other.m_capacity, 0u);
        }
        return *this;
    }
    ~EncodedBuffer() = default;

    [[nodiscard]] const std::uint8_t* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0u; }
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept;

private:
    friend class MemoryByteRangeOutput;
    EncodedBuffer(std::unique_ptr<std::uint8_t[]> bytes, std::size_t size, std::size_t capacity) noexcept
        : m_bytes(std::move(bytes)), m_size(size), m_capacity(capacity) {}
    std::unique_ptr<std::uint8_t[]> m_bytes;
    std::size_t m_size{0u};
    std::size_t m_capacity{0u};
};

}

#endif
