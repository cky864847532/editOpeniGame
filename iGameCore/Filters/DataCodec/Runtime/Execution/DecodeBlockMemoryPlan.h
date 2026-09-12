#ifndef DATACODEC_RUNTIME_EXECUTION_DECODEBLOCKMEMORYPLAN_H
#define DATACODEC_RUNTIME_EXECUTION_DECODEBLOCKMEMORYPLAN_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace datacodec {

enum class DecodeMemoryRegion : std::uint8_t { Input, Output, Temporary };
struct DecodeMemoryRange {
    DecodeMemoryRegion region{};
    std::size_t offset{0u};
    std::size_t bytes{0u};
    std::size_t alignment{1u};
};

// 描述只包含确定形状，不读正文、不申请工作数组
struct DecodeBlockMemoryPlan {
    std::array<std::size_t, 3u> regionBytes{};
    std::uint64_t block{0u};

    static std::size_t Add(std::size_t a, std::size_t b) {
        if (b > std::numeric_limits<std::size_t>::max() - a) {
            throw std::length_error("decode memory layout addition overflow");
        }
        return a + b;
    }
    static std::size_t Multiply(std::size_t a, std::size_t b) {
        if (a != 0u && b > std::numeric_limits<std::size_t>::max() / a) {
            throw std::length_error("decode memory layout multiplication overflow");
        }
        return a * b;
    }
    template<class T>
    DecodeMemoryRange Append(DecodeMemoryRegion region, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= alignof(std::max_align_t));
        auto& size = regionBytes[static_cast<std::size_t>(region)];
        const auto offset = Add(size, (alignof(T) - size % alignof(T)) % alignof(T));
        const auto bytes = Multiply(count, sizeof(T));
        size = Add(offset, bytes);
        return {region, offset, bytes, alignof(T)};
    }
    std::size_t TotalBytes() const { return Add(Add(regionBytes[0], regionBytes[1]), regionBytes[2]); }
};

}
#endif
