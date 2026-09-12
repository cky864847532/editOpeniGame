#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace datacodec {

struct CpuUsageSample {
    std::optional<double> idleRatio;
    std::size_t logicalProcessors{0u};
    std::chrono::steady_clock::time_point sampledAt{};
};

// 采样句柄由控制线程独占，首轮只建立累计时间基线
class CpuUsageProbe final {
public:
    CpuUsageProbe();
    ~CpuUsageProbe();
    CpuUsageProbe(const CpuUsageProbe&) = delete;
    CpuUsageProbe& operator=(const CpuUsageProbe&) = delete;
    static bool Supported() noexcept;
    bool Reset() noexcept;
    CpuUsageSample Sample() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
