#ifndef DATACODEC_COMMON_VIEWS_BUFFERCAPACITYSAMPLE_H
#define DATACODEC_COMMON_VIEWS_BUFFERCAPACITYSAMPLE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace datacodec {

// 仅保存明确接入数组的取样，不持有数组，也不参与容量准入
struct BufferCapacitySample {
    explicit BufferCapacitySample(const std::string_view sampleName) noexcept
        : name(sampleName), scopeId(NextScopeId()) {}

    void Observe(const std::uint64_t bytes) noexcept {
        capacityBytes = bytes;
        sampledPeakBytes = std::max(sampledPeakBytes.value_or(0u), bytes);
        sampledAtNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    template<class T, class Allocator>
    void Observe(const std::vector<T, Allocator>& values) noexcept {
        Observe(static_cast<std::uint64_t>(values.capacity()) * sizeof(T));
    }

    std::string_view name;
    std::uint64_t scopeId{0u};
    std::uint64_t sampledAtNanoseconds{0u};
    std::optional<std::uint64_t> capacityBytes;
    std::optional<std::uint64_t> sampledPeakBytes;

private:
    static std::uint64_t NextScopeId() noexcept {
        static std::atomic<std::uint64_t> next{1u};
        return next.fetch_add(1u, std::memory_order_relaxed);
    }
};

} // namespace datacodec

#endif
