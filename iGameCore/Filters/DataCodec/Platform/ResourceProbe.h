#ifndef DATACODEC_PLATFORM_RESOURCEPROBE_H
#define DATACODEC_PLATFORM_RESOURCEPROBE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>

namespace datacodec {

enum class PressureLevel : std::uint8_t { Unknown, Normal, Low, Critical };

struct ResourceSample {
    std::optional<std::uint64_t> physicalTotalBytes;
    std::optional<std::uint64_t> availableBytes;
    std::optional<std::uint64_t> hardLimitBytes;
    std::optional<std::uint64_t> hardRemainingBytes;
    std::optional<std::uint64_t> commitLimitBytes;
    std::optional<std::uint64_t> commitRemainingBytes;
    std::optional<std::uint64_t> processLimitBytes;
    std::optional<std::uint64_t> processRemainingBytes;
    std::optional<std::uint64_t> jobLimitBytes;
    std::optional<std::uint64_t> jobRemainingBytes;
    std::optional<std::size_t> allowedComputeThreads;
    std::optional<std::size_t> runtimeThreadLimit;
    std::size_t reservedHostThreads{0u};
    std::optional<std::uint64_t> cgroupLimitBytes;
    std::optional<std::uint64_t> cgroupRemainingBytes;
    std::optional<std::uint64_t> addressSpaceLimitBytes;
    std::optional<std::uint64_t> addressSpaceRemainingBytes;
    PressureLevel pressure{PressureLevel::Unknown};
    bool threaded{true};
    bool externalSpillAvailable{false};
    std::chrono::steady_clock::time_point sampledAt{};
};

ResourceSample ProbeResources();

// 事件循环线程等待后台任务时需要让出宿主执行权
bool ResourceWaitRequiresEventLoop() noexcept;
void YieldResourceWaitToEventLoop();

// 原生通知只唤醒控制线程，平台句柄与回调收束由此对象独占
class ResourcePressureMonitor final {
public:
    using Wake = void (*)(void*) noexcept;
    ResourcePressureMonitor(Wake, void*);
    ~ResourcePressureMonitor();
    ResourcePressureMonitor(const ResourcePressureMonitor&) = delete;
    ResourcePressureMonitor& operator=(const ResourcePressureMonitor&) = delete;
    void Observe(const ResourceSample&) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
