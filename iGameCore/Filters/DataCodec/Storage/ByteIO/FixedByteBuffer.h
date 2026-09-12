#ifndef DATACODEC_STORAGE_BYTEIO_FIXEDBYTEBUFFER_H
#define DATACODEC_STORAGE_BYTEIO_FIXEDBYTEBUFFER_H

#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include <span>
#include <stdexcept>

namespace datacodec {

// lease 始终比 backing 晚释放，数组地址满足基本数值类型的对齐要求
struct FixedByteBacking {
    FixedByteBacking() = default;
    FixedByteBacking(resource::ResidentByteBudget::Lease reservation,
        resource::ResidentByteBudget::AllocatedArray storage, std::size_t bytes) noexcept
        : lease(std::move(reservation)), array(std::move(storage)), size(bytes) {}
    FixedByteBacking(FixedByteBacking&& other) noexcept
        : lease(std::move(other.lease)), array(std::move(other.array)), size(std::exchange(other.size, 0u)) {}
    FixedByteBacking& operator=(FixedByteBacking&& other) noexcept {
        if (this != &other) {
            array.reset();
            lease = std::move(other.lease);
            array = std::move(other.array);
            size = std::exchange(other.size, 0u);
        }
        return *this;
    }
    resource::ResidentByteBudget::Lease lease;
    resource::ResidentByteBudget::AllocatedArray array;
    std::size_t size{0u};
    std::span<std::uint8_t> Span() noexcept { return {array.get(), size}; }
    std::span<const std::uint8_t> Span() const noexcept { return {array.get(), size}; }
    std::unique_ptr<std::uint8_t[]> Transfer() noexcept {
        auto result = array.Transfer();
        lease.Reset();
        size = 0u;
        return result;
    }
};

// 数值函数填写调用方的固定范围，越界统一由执行边界收束为契约失败
template<class T>
class FixedArrayView {
public:
    using value_type = T;
    FixedArrayView() = default;
    explicit FixedArrayView(std::span<T> storage) noexcept : m_storage(storage) {}
    T* data() const noexcept { return m_storage.data(); }
    std::size_t size() const noexcept { return m_size; }
    std::size_t capacity() const noexcept { return m_storage.size(); }
    bool empty() const noexcept { return m_size == 0u; }
    T* begin() const noexcept { return data(); }
    T* end() const noexcept { return m_size ? data() + m_size : data(); }
    T& operator[](std::size_t i) const { if (i >= m_size) { Fail(); } return data()[i]; }
    T& back() const { return (*this)[m_size - 1u]; }
    void clear() noexcept { m_size = 0u; }
    void reserve(std::size_t n) const { if (n > capacity()) { Fail(); } }
    void resize(std::size_t n) { reserve(n); m_size = n; }
    void resize(std::size_t n, T value) {
        reserve(n);
        if (n > m_size) { std::fill(data() + m_size, data() + n, value); }
        m_size = n;
    }
    void assign(std::size_t n, T value) { resize(n); std::fill(begin(), end(), value); }
    template<class It> requires (!std::is_integral_v<It>)
    void assign(It first, It last) {
        const auto n = static_cast<std::size_t>(std::distance(first, last));
        resize(n); std::copy(first, last, begin());
    }
    void push_back(T value) { reserve(m_size + 1u); data()[m_size++] = value; }
    std::span<T> Span() const noexcept { return {data(), m_size}; }
    operator std::span<T>() const noexcept { return Span(); }
    operator std::span<const T>() const noexcept { return Span(); }
private:
    [[noreturn]] static void Fail() { throw std::length_error("decode workspace layout exceeded"); }
    std::span<T> m_storage;
    std::size_t m_size{0u};
};

class FixedScratchBuffer {
public:
    FixedScratchBuffer() = default;
    explicit FixedScratchBuffer(FixedArrayView<std::uint8_t> bytes) : m_bytes(bytes) {
        m_bytes.resize(m_bytes.capacity());
    }
    FixedArrayView<std::uint8_t>& Bytes() noexcept { return m_bytes; }
    const FixedArrayView<std::uint8_t>& Bytes() const noexcept { return m_bytes; }
    std::span<std::uint8_t> Span() noexcept { return m_bytes.Span(); }
    std::span<const std::uint8_t> Span() const noexcept { return m_bytes.Span(); }
    void Release() noexcept { m_bytes = {}; }
private:
    FixedArrayView<std::uint8_t> m_bytes;
};

}
#endif
