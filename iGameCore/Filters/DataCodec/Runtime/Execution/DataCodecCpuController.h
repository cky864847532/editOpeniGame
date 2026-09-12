#pragma once

#include "DataCodec/API/Params/CodecResourceParams.h"
#include "DataCodec/Platform/CpuUsageProbe.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace datacodec {

namespace cpu_control {
using Clock = std::chrono::steady_clock;
inline constexpr auto sampleInterval = std::chrono::milliseconds(50);
inline constexpr auto window = std::chrono::milliseconds(200);
inline constexpr auto calibrationDeadline = std::chrono::seconds(1);
inline constexpr auto maximumSampleAge = std::chrono::seconds(1);
inline constexpr double idleTolerance = 0.02;
inline constexpr double shrinkAlpha = 0.5;
inline constexpr double growthAlpha = 0.125;
inline constexpr double minimumGain = 0.025;
inline constexpr double maximumGain = 1.0;
inline constexpr double burstSeconds = 0.02;
}

enum class CpuCalibrationResult : std::uint8_t { Pending, Measuring, Estimated, Unusable, TimedOut };
const char* CpuCalibrationResultName(CpuCalibrationResult) noexcept;
enum class CpuDecisionReason : std::uint8_t {
    NoChange, Startup, Shrink, Grow, CapacityChanged, CalibrationStep, CalibrationFinished, SampleUnavailable
};
const char* CpuDecisionReasonName(CpuDecisionReason) noexcept;

struct CpuControlFlow {
    std::size_t computeCeiling{1u};
    bool hasWork{false};
    bool queued{false};
    bool growthBlocked{false};
};

struct CpuControlSnapshot {
    CodecThreadMode mode{CodecThreadMode::Fixed};
    double targetIdleRatio{0.0};
    double quota{1.0};
    double shrinkGain{cpu_control::shrinkAlpha};
    double growthGain{cpu_control::growthAlpha};
    std::optional<double> response;
    std::optional<double> observedIdleRatio;
    std::size_t logicalProcessors{0u};
    CpuCalibrationResult calibration{CpuCalibrationResult::Pending};
    cpu_control::Clock::duration calibrationElapsed{};
    std::uint64_t gainUpdates{0u};
    std::uint64_t adjustments{0u};
    bool initialized{false};
    bool sampleValid{false};
};

struct CpuControlDecision {
    CpuDecisionReason reason{CpuDecisionReason::NoChange};
    bool sampleFailure{false};
};

// 只使用短窗口观测和数值保护，不判断外部负载的原因或估计准确性
class DataCodecCpuController final {
public:
    void Reset(CodecThreadMode, double targetIdleRatio, std::size_t ceiling,
        cpu_control::Clock::time_point) noexcept;
    CpuControlDecision Advance(cpu_control::Clock::time_point, const CpuUsageSample&,
        const CpuControlFlow&) noexcept;
    const CpuControlSnapshot& Snapshot() const noexcept { return m_snapshot; }
private:
    void ResetWindow(cpu_control::Clock::time_point) noexcept;
    bool Estimate(double beforeIdle, double afterIdle, double beforeQuota, double afterQuota) noexcept;
    CpuControlSnapshot m_snapshot;
    cpu_control::Clock::time_point m_lastValid{};
    cpu_control::Clock::time_point m_lastSample{};
    cpu_control::Clock::time_point m_windowStarted{};
    cpu_control::Clock::time_point m_calibrationStarted{};
    double m_idleSum{0.0};
    std::size_t m_samples{0u};
    unsigned m_phase{0u};
    double m_beforeIdle{0.0};
    double m_beforeQuota{0.0};
    double m_afterQuota{0.0};
    bool m_responsePending{false};
};

// 许可占用时间用于节拍控制，实际 CPU 消耗由系统采样反馈
class CpuPermitBudget final {
public:
    void Reset(double quota, cpu_control::Clock::time_point) noexcept;
    void Accrue(cpu_control::Clock::time_point, std::size_t activeUnits) noexcept;
    void SetQuota(double quota, cpu_control::Clock::time_point, std::size_t activeUnits) noexcept;
    bool Available() const noexcept { return m_quota > 0.0 && m_credit >= 0.0; }
    cpu_control::Clock::time_point NextWake(cpu_control::Clock::time_point, std::size_t activeUnits) const noexcept;
    double Credit() const noexcept { return m_credit; }
private:
    double m_quota{0.0};
    double m_credit{0.0};
    cpu_control::Clock::time_point m_updated{};
};

}
