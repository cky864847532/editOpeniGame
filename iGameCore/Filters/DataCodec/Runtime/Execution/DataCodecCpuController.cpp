#include "DataCodec/Runtime/Execution/DataCodecCpuController.h"

#include <algorithm>
#include <cmath>

namespace datacodec {

const char* CpuCalibrationResultName(CpuCalibrationResult value) noexcept {
    switch (value) {
    case CpuCalibrationResult::Pending: return "Pending";
    case CpuCalibrationResult::Measuring: return "Measuring";
    case CpuCalibrationResult::Estimated: return "Estimated";
    case CpuCalibrationResult::Unusable: return "Unusable";
    case CpuCalibrationResult::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

const char* CpuDecisionReasonName(CpuDecisionReason value) noexcept {
    switch (value) {
    case CpuDecisionReason::NoChange: return "NoChange";
    case CpuDecisionReason::Startup: return "Startup";
    case CpuDecisionReason::Shrink: return "Shrink";
    case CpuDecisionReason::Grow: return "Grow";
    case CpuDecisionReason::CapacityChanged: return "CapacityChanged";
    case CpuDecisionReason::CalibrationStep: return "CalibrationStep";
    case CpuDecisionReason::CalibrationFinished: return "CalibrationFinished";
    case CpuDecisionReason::SampleUnavailable: return "SampleUnavailable";
    }
    return "Unknown";
}

void DataCodecCpuController::Reset(CodecThreadMode mode, double idle, std::size_t ceiling,
    cpu_control::Clock::time_point now) noexcept {
    *this = {};
    m_snapshot.mode = mode;
    m_snapshot.targetIdleRatio = idle;
    m_snapshot.quota = mode == CodecThreadMode::Fixed ? static_cast<double>(ceiling) : 0.0;
    m_snapshot.initialized = mode == CodecThreadMode::Fixed;
    m_lastValid = now;
    ResetWindow(now);
}

void DataCodecCpuController::ResetWindow(cpu_control::Clock::time_point now) noexcept {
    m_windowStarted = now;
    m_idleSum = 0.0;
    m_samples = 0u;
}

bool DataCodecCpuController::Estimate(double beforeIdle, double afterIdle,
    double beforeQuota, double afterQuota) noexcept {
    const double delta = afterQuota - beforeQuota;
    if (!std::isfinite(delta) || std::abs(delta) < 1e-6) { return false; }
    const double response = -static_cast<double>(m_snapshot.logicalProcessors) * (afterIdle - beforeIdle) / delta;
    if (!std::isfinite(response) || response < 1e-6) { return false; }
    m_snapshot.response = response;
    m_snapshot.shrinkGain = std::clamp(cpu_control::shrinkAlpha / response,
        cpu_control::minimumGain, cpu_control::maximumGain);
    m_snapshot.growthGain = std::clamp(cpu_control::growthAlpha / response,
        cpu_control::minimumGain, cpu_control::maximumGain);
    ++m_snapshot.gainUpdates;
    return true;
}

CpuControlDecision DataCodecCpuController::Advance(cpu_control::Clock::time_point now,
    const CpuUsageSample& sample, const CpuControlFlow& flow) noexcept {
    using namespace cpu_control;
    CpuControlDecision decision;
    if (m_snapshot.mode == CodecThreadMode::Fixed) { return decision; }
    if (m_snapshot.calibration == CpuCalibrationResult::Measuring &&
        now - m_calibrationStarted >= calibrationDeadline) {
        m_snapshot.calibration = CpuCalibrationResult::TimedOut;
        m_snapshot.calibrationElapsed = now - m_calibrationStarted;
        m_phase = 0u;
        m_responsePending = false;
        ResetWindow(now);
        decision.reason = CpuDecisionReason::CalibrationFinished;
    }
    const bool valid = sample.idleRatio && std::isfinite(*sample.idleRatio) &&
        *sample.idleRatio >= 0.0 && *sample.idleRatio <= 1.0 && sample.logicalProcessors != 0u &&
        sample.sampledAt <= now && now - sample.sampledAt <= maximumSampleAge;
    m_snapshot.sampleValid = valid;
    if (!valid) {
        decision.reason = CpuDecisionReason::SampleUnavailable;
        decision.sampleFailure = now - m_lastValid >= maximumSampleAge;
        ResetWindow(now);
        m_responsePending = false;
        return decision;
    }
    if (sample.sampledAt <= m_lastSample) {
        decision.sampleFailure = now - m_lastValid >= maximumSampleAge;
        return decision;
    }
    m_lastSample = sample.sampledAt;
    m_lastValid = sample.sampledAt;
    m_snapshot.observedIdleRatio = sample.idleRatio;
    m_snapshot.logicalProcessors = sample.logicalProcessors;
    const double ceiling = static_cast<double>(flow.computeCeiling);
    if (!m_snapshot.initialized) {
        m_snapshot.initialized = true;
        m_snapshot.quota = std::clamp(static_cast<double>(sample.logicalProcessors) *
            (*sample.idleRatio - m_snapshot.targetIdleRatio), 0.0, ceiling);
        ResetWindow(now);
        return {CpuDecisionReason::Startup, false};
    }
    if (m_snapshot.quota > ceiling) {
        m_snapshot.quota = ceiling;
        m_responsePending = false;
        if (m_snapshot.calibration == CpuCalibrationResult::Measuring) {
            m_snapshot.calibration = CpuCalibrationResult::Unusable;
            m_snapshot.calibrationElapsed = now - m_calibrationStarted;
            m_phase = 0u;
        }
        ResetWindow(now);
        return {CpuDecisionReason::CapacityChanged, false};
    }
    if (m_snapshot.calibration == CpuCalibrationResult::Pending && flow.hasWork &&
        m_snapshot.quota >= 0.25) {
        m_snapshot.calibration = CpuCalibrationResult::Measuring;
        m_calibrationStarted = now;
        m_beforeQuota = m_snapshot.quota;
        m_phase = 1u;
        ResetWindow(now);
    }
    m_idleSum += *sample.idleRatio;
    ++m_samples;
    if (now - m_windowStarted < window) { return decision; }
    const double idle = m_idleSum / static_cast<double>(m_samples);
    ResetWindow(now);
    if (m_phase == 1u) {
        m_beforeIdle = idle;
        m_afterQuota = std::max(0.0, m_beforeQuota - std::min(2.0, m_beforeQuota * 0.25));
        m_snapshot.quota = m_afterQuota;
        m_phase = 2u;
        ++m_snapshot.adjustments;
        return {CpuDecisionReason::CalibrationStep, false};
    }
    if (m_phase == 2u) {
        m_snapshot.calibration = Estimate(m_beforeIdle, idle, m_beforeQuota, m_afterQuota)
            ? CpuCalibrationResult::Estimated : CpuCalibrationResult::Unusable;
        m_snapshot.calibrationElapsed = now - m_calibrationStarted;
        m_phase = 0u;
        decision.reason = CpuDecisionReason::CalibrationFinished;
    } else if (m_responsePending) {
        Estimate(m_beforeIdle, idle, m_beforeQuota, m_afterQuota);
        m_responsePending = false;
    }
    const double error = idle - m_snapshot.targetIdleRatio;
    if (std::abs(error) <= idleTolerance || (error > 0.0 && (!flow.queued || flow.growthBlocked))) {
        return decision;
    }
    const double gain = error < 0.0 ? m_snapshot.shrinkGain : m_snapshot.growthGain;
    const double maximumStep = std::max(0.25, std::min(4.0, std::max(1.0, m_snapshot.quota) * 0.25));
    const double delta = std::clamp(gain * static_cast<double>(sample.logicalProcessors) * error,
        -maximumStep, maximumStep);
    const double next = std::clamp(m_snapshot.quota + delta, 0.0, ceiling);
    if (next != m_snapshot.quota) {
        m_beforeQuota = m_snapshot.quota;
        m_afterQuota = next;
        m_beforeIdle = idle;
        m_responsePending = flow.hasWork && !flow.growthBlocked;
        m_snapshot.quota = next;
        ++m_snapshot.adjustments;
        if (decision.reason == CpuDecisionReason::NoChange) {
            decision.reason = delta < 0.0 ? CpuDecisionReason::Shrink : CpuDecisionReason::Grow;
        }
    }
    return decision;
}

void CpuPermitBudget::Reset(double quota, cpu_control::Clock::time_point now) noexcept {
    m_quota = quota;
    m_credit = quota * cpu_control::burstSeconds;
    m_updated = now;
}

void CpuPermitBudget::Accrue(cpu_control::Clock::time_point now, std::size_t active) noexcept {
    if (now <= m_updated) { return; }
    m_credit = std::min(m_quota * cpu_control::burstSeconds,
        m_credit + (m_quota - static_cast<double>(active)) * std::chrono::duration<double>(now - m_updated).count());
    m_updated = now;
}

void CpuPermitBudget::SetQuota(double quota, cpu_control::Clock::time_point now, std::size_t active) noexcept {
    Accrue(now, active);
    m_quota = quota;
    m_credit = std::min(m_credit, quota * cpu_control::burstSeconds);
}

cpu_control::Clock::time_point CpuPermitBudget::NextWake(cpu_control::Clock::time_point now,
    std::size_t active) const noexcept {
    const double rate = m_quota - static_cast<double>(active);
    if (rate <= 0.0 || m_credit >= 0.0) { return now + cpu_control::sampleInterval; }
    // 有界定时等待及时观察新的额度与取消，计算许可不积累空闲突发量
    const auto seconds = std::clamp(-m_credit / rate, 0.001, 0.05);
    return now + std::chrono::duration_cast<cpu_control::Clock::duration>(std::chrono::duration<double>(seconds));
}

}
