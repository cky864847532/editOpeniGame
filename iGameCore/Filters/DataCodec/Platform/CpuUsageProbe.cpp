#include "DataCodec/Platform/CpuUsageProbe.h"

#include <cmath>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace datacodec {

struct CpuUsageProbe::Impl {
#if defined(_WIN32)
    PDH_HQUERY query{nullptr};
    PDH_HCOUNTER idle{nullptr};
    ~Impl() { if (query) { PdhCloseQuery(query); } }
#endif
};

CpuUsageProbe::CpuUsageProbe() : m_impl(std::make_unique<Impl>()) {}
CpuUsageProbe::~CpuUsageProbe() = default;

bool CpuUsageProbe::Supported() noexcept {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool CpuUsageProbe::Reset() noexcept {
#if defined(_WIN32)
    if (m_impl->query) { PdhCloseQuery(m_impl->query); }
    m_impl->query = nullptr;
    m_impl->idle = nullptr;
    // 全机计数器覆盖处理器组，英文接口避免依赖系统显示语言
    return PdhOpenQueryW(nullptr, 0, &m_impl->query) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(m_impl->query, L"\\Processor Information(_Total)\\% Idle Time",
            0, &m_impl->idle) == ERROR_SUCCESS &&
        PdhCollectQueryData(m_impl->query) == ERROR_SUCCESS;
#else
    return false;
#endif
}

CpuUsageSample CpuUsageProbe::Sample() noexcept {
    CpuUsageSample sample;
#if defined(_WIN32)
    sample.logicalProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    PDH_FMT_COUNTERVALUE value{};
    if (m_impl->query && m_impl->idle &&
        PdhCollectQueryData(m_impl->query) == ERROR_SUCCESS &&
        PdhGetFormattedCounterValue(m_impl->idle, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
        (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA) &&
        std::isfinite(value.doubleValue) && value.doubleValue >= 0.0 && value.doubleValue <= 100.0) {
        sample.idleRatio = value.doubleValue / 100.0;
    }
#endif
    sample.sampledAt = std::chrono::steady_clock::now();
    return sample;
}

}
