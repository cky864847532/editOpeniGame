#include "DataCodec/Platform/ResourceProbe.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <thread>
#include <charconv>
#include <filesystem>
#include <sstream>
#include <vector>
#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <fstream>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#endif

namespace datacodec {
namespace {

void Restrict(std::optional<std::uint64_t>& value, std::optional<std::uint64_t> limit) noexcept {
    if (limit) { value = value ? std::min(*value, *limit) : *limit; }
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
std::optional<std::uint64_t> ReadUnsigned(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string token;
    if (!(input >> token) || token == "max" || token == "-1") { return std::nullopt; }
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) { return std::nullopt; }
    return value;
}

bool HasController(std::string_view list, std::string_view controller) noexcept {
    std::size_t offset = 0u;
    while (offset <= list.size()) {
        const auto end = list.find(',', offset);
        const auto item = list.substr(offset, end == std::string_view::npos ? list.size() - offset : end - offset);
        if (item == controller) { return true; }
        if (end == std::string_view::npos) { break; }
        offset = end + 1u;
    }
    return false;
}

std::string DecodeMountPath(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0u; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3u < value.size() &&
            value[i + 1u] >= '0' && value[i + 1u] <= '7' &&
            value[i + 2u] >= '0' && value[i + 2u] <= '7' &&
            value[i + 3u] >= '0' && value[i + 3u] <= '7') {
            decoded.push_back(static_cast<char>((value[i + 1u] - '0') * 64 +
                (value[i + 2u] - '0') * 8 + value[i + 3u] - '0'));
            i += 3u;
        } else { decoded.push_back(value[i]); }
    }
    return decoded;
}

void ApplyCpuQuota(ResourceSample& sample, std::uint64_t quota, std::uint64_t period) {
    if (quota == 0u || period == 0u) { return; }
    const auto cpus = quota / period + (quota % period != 0u ? 1u : 0u);
    const auto bound = static_cast<std::size_t>(std::min<std::uint64_t>(
        cpus, std::numeric_limits<std::size_t>::max()));
    sample.allowedComputeThreads = std::min(sample.allowedComputeThreads.value_or(bound), bound);
}

void ProbeCgroups(ResourceSample& sample) {
    struct Membership { std::string controllers; std::filesystem::path path; };
    std::vector<Membership> memberships;
    std::ifstream membershipFile("/proc/self/cgroup");
    std::string line;
    while (std::getline(membershipFile, line)) {
        const auto first = line.find(':');
        const auto second = first == std::string::npos ? first : line.find(':', first + 1u);
        if (second == std::string::npos) { continue; }
        memberships.push_back({line.substr(first + 1u, second - first - 1u), line.substr(second + 1u)});
    }
    std::ifstream mounts("/proc/self/mountinfo");
    while (std::getline(mounts, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) { continue; }
        std::istringstream post(line.substr(separator + 3u));
        std::string fs, source, superOptions;
        if (!(post >> fs >> source >> superOptions) || (fs != "cgroup" && fs != "cgroup2")) { continue; }
        std::istringstream pre(line.substr(0u, separator));
        std::string id, parent, device, root, mount;
        if (!(pre >> id >> parent >> device >> root >> mount)) { continue; }
        const auto rootPath = std::filesystem::path(DecodeMountPath(root)).lexically_normal();
        const auto mountPath = std::filesystem::path(DecodeMountPath(mount)).lexically_normal();
        for (const auto& member : memberships) {
            const bool v2 = fs == "cgroup2" && member.controllers.empty();
            const bool memory = v2 || (HasController(member.controllers, "memory") && HasController(superOptions, "memory"));
            const bool cpu = v2 || (HasController(member.controllers, "cpu") && HasController(superOptions, "cpu"));
            if (!memory && !cpu) { continue; }
            const auto relative = member.path.lexically_normal().lexically_relative(rootPath);
            if (relative.empty() || relative.is_absolute() ||
                (!relative.empty() && *relative.begin() == "..")) { continue; }
            auto current = (mountPath / relative).lexically_normal();
            for (;;) {
                if (memory) {
                    auto limit = ReadUnsigned(current / (v2 ? "memory.max" : "memory.limit_in_bytes"));
                    // cgroup v1 用接近 LONG_MAX 的页对齐值表示无限
                    if (!v2 && limit && *limit >= (std::uint64_t{1} << 60u)) { limit.reset(); }
                    const auto used = ReadUnsigned(current / (v2 ? "memory.current" : "memory.usage_in_bytes"));
                    Restrict(sample.cgroupLimitBytes, limit);
                    if (limit && used) {
                        Restrict(sample.cgroupRemainingBytes, *limit - std::min(*limit, *used));
                    }
                }
                if (cpu) {
                    if (v2) {
                        std::ifstream quotaFile(current / "cpu.max");
                        std::string quotaToken;
                        std::uint64_t period = 0u, quota = 0u;
                        if (quotaFile >> quotaToken >> period) {
                            const auto parsed = std::from_chars(quotaToken.data(), quotaToken.data() + quotaToken.size(), quota);
                            if (parsed.ec == std::errc{} && parsed.ptr == quotaToken.data() + quotaToken.size()) {
                                ApplyCpuQuota(sample, quota, period);
                            }
                        }
                    } else {
                        const auto quota = ReadUnsigned(current / "cpu.cfs_quota_us");
                        const auto period = ReadUnsigned(current / "cpu.cfs_period_us");
                        if (quota && period) { ApplyCpuQuota(sample, *quota, *period); }
                    }
                }
                if (current == mountPath) { break; }
                const auto next = current.parent_path();
                if (next == current || next.empty()) { break; }
                current = next;
            }
        }
    }
    Restrict(sample.hardLimitBytes, sample.cgroupLimitBytes);
    Restrict(sample.hardRemainingBytes, sample.cgroupRemainingBytes);
}
#endif

}

bool ResourceWaitRequiresEventLoop() noexcept {
#if defined(__EMSCRIPTEN__)
    return emscripten_is_main_browser_thread();
#else
    return false;
#endif
}

void YieldResourceWaitToEventLoop() {
#if defined(__EMSCRIPTEN__)
    emscripten_sleep(0);
#endif
}

ResourceSample ProbeResources() {
    ResourceSample result;
    const auto hardware = std::thread::hardware_concurrency();
    if (hardware != 0u) { result.allowedComputeThreads = hardware; }
#if defined(_WIN32)
    result.externalSpillAvailable = true;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.physicalTotalBytes = memory.ullTotalPhys;
        result.availableBytes = memory.ullAvailPhys;
        result.commitLimitBytes = memory.ullTotalPageFile;
        result.commitRemainingBytes = memory.ullAvailPageFile;
        result.hardLimitBytes = memory.ullTotalVirtual;
        result.hardRemainingBytes = memory.ullAvailVirtual;
        result.addressSpaceLimitBytes = memory.ullTotalVirtual;
        result.addressSpaceRemainingBytes = memory.ullAvailVirtual;
        Restrict(result.hardRemainingBytes, result.commitRemainingBytes);
    }
    DWORD_PTR processMask = 0u;
    DWORD_PTR systemMask = 0u;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0u) {
        result.allowedComputeThreads = static_cast<std::size_t>(std::popcount(processMask));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr)) {
        if ((job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0u) {
            result.processLimitBytes = job.ProcessMemoryLimit;
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
                result.processRemainingBytes = *result.processLimitBytes - std::min(*result.processLimitBytes,
                    static_cast<std::uint64_t>(counters.PrivateUsage));
            }
        }
        if ((job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0u) {
            result.jobLimitBytes = job.JobMemoryLimit;
            JOBOBJECT_LIMIT_VIOLATION_INFORMATION usage{};
            if (QueryInformationJobObject(nullptr, JobObjectLimitViolationInformation, &usage, sizeof(usage), nullptr)) {
                result.jobRemainingBytes = *result.jobLimitBytes - std::min(*result.jobLimitBytes,
                    static_cast<std::uint64_t>(usage.JobMemory));
            }
        }
        Restrict(result.hardLimitBytes, result.processLimitBytes);
        Restrict(result.hardLimitBytes, result.jobLimitBytes);
        Restrict(result.hardRemainingBytes, result.processRemainingBytes);
        Restrict(result.hardRemainingBytes, result.jobRemainingBytes);
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpuRate{};
    if (QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &cpuRate, sizeof(cpuRate), nullptr) &&
        (cpuRate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) != 0u) {
        DWORD rate = 0u;
        // Windows ABI 的高十六位保存 MaxRate，部分 MinGW 头只声明同位置的 CpuRate
        if ((cpuRate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_MIN_MAX_RATE) != 0u) { rate = cpuRate.CpuRate >> 16u; }
        else if ((cpuRate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) != 0u) { rate = cpuRate.CpuRate; }
        if (rate != 0u) {
            const auto machine = static_cast<std::uint64_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            const auto allowed = static_cast<std::size_t>(std::max<std::uint64_t>(1u, (machine * rate + 9999u) / 10000u));
            result.allowedComputeThreads = std::min(result.allowedComputeThreads.value_or(allowed), allowed);
        }
    }
    HANDLE pressure = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (pressure != nullptr) {
        BOOL low = FALSE;
        if (QueryMemoryResourceNotification(pressure, &low)) {
            result.pressure = low ? PressureLevel::Low : PressureLevel::Normal;
        }
        CloseHandle(pressure);
    }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    result.externalSpillAvailable = true;
    std::ifstream info("/proc/meminfo");
    std::string key;
    std::uint64_t kib = 0u;
    std::string unit;
    while (info >> key >> kib >> unit) {
        if (unit != "kB" || kib > std::numeric_limits<std::uint64_t>::max() / 1024u) { continue; }
        if (key == "MemTotal:") { result.physicalTotalBytes = kib * 1024u; }
        if (key == "MemAvailable:") { result.availableBytes = kib * 1024u; }
        if (result.physicalTotalBytes && result.availableBytes) { break; }
    }
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_COUNT(&cpus) > 0) {
        result.allowedComputeThreads = static_cast<std::size_t>(CPU_COUNT(&cpus));
    }
    ProbeCgroups(result);
    rlimit addressSpace{};
    if (getrlimit(RLIMIT_AS, &addressSpace) == 0 && addressSpace.rlim_cur != RLIM_INFINITY) {
        result.addressSpaceLimitBytes = static_cast<std::uint64_t>(addressSpace.rlim_cur);
        std::ifstream statm("/proc/self/statm");
        std::uint64_t pages = 0u;
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if (statm >> pages && pageSize > 0 &&
            pages <= std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(pageSize)) {
            const auto used = pages * static_cast<std::uint64_t>(pageSize);
            result.addressSpaceRemainingBytes = *result.addressSpaceLimitBytes -
                std::min(*result.addressSpaceLimitBytes, used);
        }
        Restrict(result.hardLimitBytes, result.addressSpaceLimitBytes);
        Restrict(result.hardRemainingBytes, result.addressSpaceRemainingBytes);
    }
#elif defined(__EMSCRIPTEN__)
    result.hardLimitBytes = static_cast<std::uint64_t>(emscripten_get_heap_max());
    result.addressSpaceLimitBytes = result.hardLimitBytes;
    const auto logicalCores = emscripten_num_logical_cores();
    if (logicalCores > 0) { result.allowedComputeThreads = static_cast<std::size_t>(logicalCores); }
#if defined(DATACODEC_RUNTIME_THREAD_LIMIT) && DATACODEC_RUNTIME_THREAD_LIMIT > 0
    result.runtimeThreadLimit = static_cast<std::size_t>(DATACODEC_RUNTIME_THREAD_LIMIT);
#else
    // 线程池布局由同一个 WASM 构建契约提供
    result.runtimeThreadLimit = result.allowedComputeThreads.value_or(1u) + DATACODEC_WASM_POOL_OVERHEAD;
#endif
    result.reservedHostThreads = DATACODEC_WASM_HOST_THREADS;
#if !defined(__EMSCRIPTEN_PTHREADS__)
    result.threaded = false;
    result.allowedComputeThreads = 1u;
#endif
#endif
#if defined(DATACODEC_MAX_PARALLEL_WORKERS) && DATACODEC_MAX_PARALLEL_WORKERS > 0
    result.allowedComputeThreads = std::min(result.allowedComputeThreads.value_or(1u),
        static_cast<std::size_t>(DATACODEC_MAX_PARALLEL_WORKERS));
#endif
    result.sampledAt = std::chrono::steady_clock::now();
    return result;
}

struct ResourcePressureMonitor::Impl {
#if defined(_WIN32)
    Impl(Wake wakeFunction, void* wakeContext) : wake(wakeFunction), context(wakeContext) {
        low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
    }
    ~Impl() {
        ClearWait();
        if (low) { CloseHandle(low); }
        if (high) { CloseHandle(high); }
    }
    static VOID CALLBACK OnPressure(PVOID argument, BOOLEAN) noexcept {
        auto& self = *static_cast<Impl*>(argument);
        self.delivered.store(true, std::memory_order_release);
        if (self.wake) { self.wake(self.context); }
    }
    void ClearWait() noexcept {
        if (wait) {
            UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
            wait = nullptr;
        }
    }
    void Observe(const ResourceSample& sample) noexcept {
        if (sample.pressure == PressureLevel::Unknown) { return; }
        const bool wantHigh = sample.pressure == PressureLevel::Low || sample.pressure == PressureLevel::Critical;
        if (wait && waitingForHigh == wantHigh && !delivered.load(std::memory_order_acquire)) { return; }
        ClearWait();
        waitingForHigh = wantHigh;
        delivered.store(false, std::memory_order_release);
        const auto handle = wantHigh ? high : low;
        if (handle) {
            RegisterWaitForSingleObject(&wait, handle, OnPressure, this, INFINITE, WT_EXECUTEONLYONCE);
        }
    }
    Wake wake;
    void* context;
    HANDLE low{nullptr};
    HANDLE high{nullptr};
    HANDLE wait{nullptr};
    bool waitingForHigh{false};
    std::atomic_bool delivered{false};
#else
    Impl(Wake, void*) {}
    void Observe(const ResourceSample&) noexcept {}
#endif
};

ResourcePressureMonitor::ResourcePressureMonitor(Wake wake, void* context)
    : m_impl(std::make_unique<Impl>(wake, context)) {}
ResourcePressureMonitor::~ResourcePressureMonitor() = default;
void ResourcePressureMonitor::Observe(const ResourceSample& sample) noexcept { m_impl->Observe(sample); }

}
