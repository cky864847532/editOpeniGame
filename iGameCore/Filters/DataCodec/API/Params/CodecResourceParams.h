#ifndef DATACODEC_API_PARAMS_CODECRESOURCEPARAMS_H
#define DATACODEC_API_PARAMS_CODECRESOURCEPARAMS_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace datacodec {

enum class CodecResourceMode : std::uint8_t { Fixed = 0, Adaptive = 1, Unlimited = 2 };

enum class CodecThreadMode : std::uint8_t { Fixed = 0, Adaptive = 1 };

[[nodiscard]] inline constexpr const char* CodecThreadModeName(CodecThreadMode mode) noexcept {
    switch (mode) {
    case CodecThreadMode::Fixed: return "Fixed";
    case CodecThreadMode::Adaptive: return "Adaptive";
    }
    return "Unknown";
}

[[nodiscard]] inline constexpr const char* CodecResourceModeName(CodecResourceMode mode) noexcept {
    switch (mode) {
    case CodecResourceMode::Fixed: return "Fixed";
    case CodecResourceMode::Adaptive: return "Adaptive";
    case CodecResourceMode::Unlimited: return "Unlimited";
    }
    return "Unknown";
}

struct CodecResourceParams {
    CodecResourceMode mode{CodecResourceMode::Adaptive};
    std::optional<std::size_t> maxComputeThreads;
    // 清单内自有存储的容量上限，零表示禁止新增正容量，Unlimited 必须留空
    std::optional<std::uint64_t> ownedStorageLimitBytes;
    CodecThreadMode threadMode{CodecThreadMode::Fixed};
    // Adaptive 指定系统空闲比例，Fixed 留空；线程上限只用于 Fixed
    std::optional<double> targetCpuIdleRatio;
    // Adaptive 的系统可用物理内存保留比例，省略时采用 0.20
    std::optional<double> targetAvailableMemoryRatio;
};

}

#endif
