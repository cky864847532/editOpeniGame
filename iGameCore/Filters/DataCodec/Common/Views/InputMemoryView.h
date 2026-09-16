#ifndef DATACODEC_COMMON_VIEWS_INPUTMEMORYVIEW_H
#define DATACODEC_COMMON_VIEWS_INPUTMEMORYVIEW_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace datacodec {

// 只描述借用范围，寿命由输入 owner 保证，容量不计入核心自有预算
// 同一分配的子视图使用相同 allocationIdentity 和相对于分配起点的偏移
struct InputMemoryView {
    const void* allocationIdentity{nullptr};
    std::uint64_t offsetBytes{0u};
    std::uint64_t sizeBytes{0u};
    std::optional<std::uint64_t> capacityBytes;
};

struct InputMemoryObservation {
    std::uint64_t describedBytes{0u};
    std::uint64_t knownCapacityBytes{0u};
    std::size_t unknownCapacityRegions{0u};
};

// 共享分配按身份合并，重叠范围只计一次，未知容量单独报告
inline InputMemoryObservation ObserveInputMemory(std::span<const InputMemoryView> views) {
    struct Region {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        std::optional<std::uint64_t> capacity;
    };
    const auto add = [](std::uint64_t& value, std::uint64_t bytes) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - value) {
            throw std::overflow_error("input memory observation exceeds byte capacity");
        }
        value += bytes;
    };
    std::map<const void*, Region, std::less<const void*>> regions;
    InputMemoryObservation result;
    for (const auto& view : views) {
        auto end = view.offsetBytes;
        add(end, view.sizeBytes);
        if (view.capacityBytes && end > *view.capacityBytes) {
            throw std::invalid_argument("input memory view exceeds its declared allocation capacity");
        }
        if (!view.allocationIdentity) {
            add(result.describedBytes, view.sizeBytes);
            if (view.capacityBytes) { add(result.knownCapacityBytes, *view.capacityBytes); }
            else { ++result.unknownCapacityRegions; }
            continue;
        }
        auto& region = regions[view.allocationIdentity];
        region.ranges.emplace_back(view.offsetBytes, end);
        if (view.capacityBytes) { region.capacity = std::max(region.capacity.value_or(0u), *view.capacityBytes); }
    }
    for (auto& [identity, region] : regions) {
        (void)identity;
        std::sort(region.ranges.begin(), region.ranges.end());
        std::uint64_t end = 0u;
        for (const auto& range : region.ranges) {
            if (range.second > end) {
                add(result.describedBytes, range.second - std::max(range.first, end));
                end = range.second;
            }
        }
        if (region.capacity && end > *region.capacity) {
            throw std::invalid_argument("shared input views exceed the declared allocation capacity");
        }
        if (region.capacity) { add(result.knownCapacityBytes, *region.capacity); }
        else { ++result.unknownCapacityRegions; }
    }
    return result;
}

} // 命名空间 datacodec
#endif
