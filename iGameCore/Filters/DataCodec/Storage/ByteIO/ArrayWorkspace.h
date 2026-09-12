#ifndef DATACODEC_STORAGE_BYTEIO_ARRAYWORKSPACE_H
#define DATACODEC_STORAGE_BYTEIO_ARRAYWORKSPACE_H

#include "DataCodec/Storage/ByteIO/FixedByteBuffer.h"
#include "DataCodec/Runtime/Execution/DecodeBlockMemoryPlan.h"
#include <vector>

namespace datacodec {

// 共用算法的输出适配，编码传入自有 vector，解码传入预备范围
template<class T>
class MutableArray {
public:
    using value_type = T;
    MutableArray(std::vector<T>& v) noexcept : m_vector(&v) {}
    MutableArray(FixedArrayView<T>& v) noexcept : m_fixed(&v) {}
    T* data() const noexcept { return m_vector ? m_vector->data() : m_fixed->data(); }
    std::size_t size() const noexcept { return m_vector ? m_vector->size() : m_fixed->size(); }
    std::size_t capacity() const noexcept { return m_vector ? m_vector->capacity() : m_fixed->capacity(); }
    bool empty() const noexcept { return size() == 0u; }
    T* begin() const noexcept { return data(); }
    T* end() const noexcept { return size() ? data() + size() : data(); }
    T& operator[](std::size_t i) const { return m_vector ? (*m_vector)[i] : (*m_fixed)[i]; }
    T& back() const { return (*this)[size() - 1u]; }
    void clear() { if (m_vector) { m_vector->clear(); } else { m_fixed->clear(); } }
    void reserve(std::size_t n) { if (m_vector) { m_vector->reserve(n); } else { m_fixed->reserve(n); } }
    void resize(std::size_t n) { if (m_vector) { m_vector->resize(n); } else { m_fixed->resize(n); } }
    void resize(std::size_t n, T value) { if (m_vector) { m_vector->resize(n, value); } else { m_fixed->resize(n, value); } }
    void assign(std::size_t n, T value) { if (m_vector) { m_vector->assign(n, value); } else { m_fixed->assign(n, value); } }
    template<class It> requires (!std::is_integral_v<It>)
    void assign(It first, It last) { if (m_vector) { m_vector->assign(first, last); } else { m_fixed->assign(first, last); } }
    void push_back(T value) { if (m_vector) { m_vector->push_back(value); } else { m_fixed->push_back(value); } }
    void insert(T* where, std::size_t n, T value) {
        if (where != end()) { throw std::logic_error("workspace array only supports append"); }
        const auto start = size();
        resize(DecodeBlockMemoryPlan::Add(start, n));
        std::fill(data() + start, end(), value);
    }
    operator std::span<T>() const noexcept { return {data(), size()}; }
    operator std::span<const T>() const noexcept { return {data(), size()}; }
private:
    std::vector<T>* m_vector{nullptr};
    FixedArrayView<T>* m_fixed{nullptr};
};

// 临时区域按词法作用域复用，容量由同一布局计算预先提供
class ArrayWorkspace {
public:
    explicit ArrayWorkspace(std::span<std::uint8_t> bytes) noexcept : m_bytes(bytes) {}
    class Scope {
    public:
        explicit Scope(ArrayWorkspace* arena) noexcept : m_arena(arena), m_mark(arena ? arena->m_used : 0u) {}
        ~Scope() { if (m_arena) { m_arena->m_used = m_mark; } }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        ArrayWorkspace* m_arena;
        std::size_t m_mark;
    };
    template<class T> FixedArrayView<T> Take(std::size_t count) {
        const auto padding = (alignof(T) - m_used % alignof(T)) % alignof(T);
        const auto offset = DecodeBlockMemoryPlan::Add(m_used, padding);
        const auto bytes = DecodeBlockMemoryPlan::Multiply(count, sizeof(T));
        if (offset > m_bytes.size() || bytes > m_bytes.size() - offset) {
            throw std::length_error("decode temporary layout exceeded");
        }
        m_used = offset + bytes;
        return FixedArrayView<T>({reinterpret_cast<T*>(m_bytes.data() + offset), count});
    }
private:
    std::span<std::uint8_t> m_bytes;
    std::size_t m_used{0u};
};

template<class T>
class WorkingArray {
public:
    using value_type = T;
    // nullptr 仅用于编码及独立算法调用，生产解码显式传入固定区域
    WorkingArray(ArrayWorkspace* arena, std::size_t capacity)
        : m_fixed(arena ? arena->Take<T>(capacity) : FixedArrayView<T>()),
          m_view(arena ? MutableArray<T>(m_fixed) : MutableArray<T>(m_owned)) {
        m_view.reserve(capacity);
    }
    WorkingArray(const WorkingArray&) = delete;
    WorkingArray& operator=(const WorkingArray&) = delete;
    operator MutableArray<T>() { return m_view; }
    operator std::span<const T>() const { return {data(), size()}; }
    operator std::span<T>() { return {data(), size()}; }
    T* data() const { return m_view.data(); }
    std::size_t size() const { return m_view.size(); }
    std::size_t capacity() const { return m_view.capacity(); }
    bool empty() const { return m_view.empty(); }
    T* begin() const { return m_view.begin(); }
    T* end() const { return m_view.end(); }
    T& operator[](std::size_t i) const { return m_view[i]; }
    void clear() { m_view.clear(); }
    void reserve(std::size_t n) { m_view.reserve(n); }
    void resize(std::size_t n) { m_view.resize(n); }
    void resize(std::size_t n, T value) { m_view.resize(n, value); }
    void assign(std::size_t n, T value) { m_view.assign(n, value); }
    void push_back(T value) { m_view.push_back(value); }
private:
    std::vector<T> m_owned;
    FixedArrayView<T> m_fixed;
    MutableArray<T> m_view;
};

}
#endif
