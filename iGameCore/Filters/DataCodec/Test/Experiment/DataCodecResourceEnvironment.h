#ifndef DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEENVIRONMENT_H
#define DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEENVIRONMENT_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <atomic>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace datacodec::test {

// 显式独立进程实验，真实 Job 边界与宿主占用不进入 DataCodec 容量管理
inline TestResult RunDataCodecResourceEnvironment() {
    TestResult result;
#if defined(_WIN32)
    constexpr std::uint64_t mib = 1024u * 1024u;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        result.AddFailure("resourceEnvironment.private", "cannot query process private memory");
        return result;
    }
    struct JobOwner {
        HANDLE handle{CreateJobObjectW(nullptr, nullptr)};
        ~JobOwner() {
            if (handle) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION clear{};
                SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &clear, sizeof(clear));
                CloseHandle(handle);
            }
        }
    } job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = counters.PrivateUsage + 512u * mib;
    if (!job.handle || !SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) || !AssignProcessToJobObject(job.handle, GetCurrentProcess())) {
        result.AddFailure("resourceEnvironment.job", "cannot establish the experiment process Job limit");
        return result;
    }
    const auto environment = ProbeResources();
    if (environment.processLimitBytes != std::optional<std::uint64_t>(limits.ProcessMemoryLimit) ||
        !environment.processRemainingBytes || !environment.availableBytes ||
        *environment.availableBytes < 1024u * mib) {
        result.AddFailure("resourceEnvironment.probe", "Job limit is unavailable or host headroom is insufficient");
        return result;
    }
    const auto threads = std::min<std::size_t>(4u, ResourceComputeCapacity(environment, CodecResourceMode::Adaptive));
    DataCodecExecutionResources run(CodecResourceParams{CodecResourceMode::Adaptive, threads, 128u * mib});
    if (!run.BeginRun()) {
        result.AddFailure("resourceEnvironment.begin", "cannot begin the constrained request");
        return result;
    }
    struct Sample {
        double milliseconds{};
        std::uint64_t remaining{};
        RuntimeResourceLimits limits;
        ResourceControlPhase phase{};
        std::size_t admitted{};
        std::size_t computing{};
        bool pending{};
        bool gate{};
        bool retentionPaused{};
        bool hostLoad{};
    };
    std::vector<Sample> samples;
    samples.reserve(600u);
    std::atomic_uint observerFailure{0u};
    std::atomic_bool loadReleased{false};
    const auto began = ResourceClock::now();
    const auto finishAt = began + std::chrono::seconds(22);
    std::jthread observer([&](std::stop_token stop) {
        struct HostAllocation {
            void* address{};
            ~HostAllocation() { Release(); }
            void Release() noexcept {
                if (address) { VirtualFree(address, 0u, MEM_RELEASE); address = nullptr; }
            }
        } host;
        bool attempted = false;
        auto loadedAt = began;
        while (!stop.stop_requested()) {
            const auto now = ResourceClock::now();
            if (!attempted && now - began >= std::chrono::milliseconds(500)) {
                attempted = true;
                const auto available = ProbeResources();
                constexpr auto retainedHeadroom = 96u * mib;
                if (!available.processRemainingBytes || *available.processRemainingBytes <= retainedHeadroom ||
                    *available.processRemainingBytes - retainedHeadroom > 512u * mib) {
                    observerFailure = 1u;
                    run.RequestStop();
                    break;
                }
                const auto bytes = *available.processRemainingBytes - retainedHeadroom;
                host.address = VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (!host.address) {
                    observerFailure = 2u;
                    run.RequestStop();
                    break;
                }
                // 实际触碰页面，压力来自实验宿主持有的内存
                auto* pages = static_cast<volatile unsigned char*>(host.address);
                for (std::uint64_t offset = 0u; offset < bytes; offset += 4096u) { pages[offset] = 1u; }
                loadedAt = ResourceClock::now();
            }
            if (host.address && now - loadedAt >= std::chrono::seconds(3)) {
                host.Release();
                loadReleased = true;
            }
            ResourceDebugSnapshot state;
            if (run.TryCopyResourceDebugSnapshot(state) && samples.size() < samples.capacity()) {
                samples.push_back({std::chrono::duration<double, std::milli>(now - began).count(),
                    state.resourceSample.processRemainingBytes.value_or(0u), state.limits,
                    state.controlPhase, state.admittedBlocks, state.activeComputeUnits,
                    state.pressurePending, state.gateOpen, state.optionalRetentionPausedByPressure,
                    host.address != nullptr});
            }
            if (now - began >= std::chrono::seconds(26)) {
                observerFailure = 3u;
                run.RequestStop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    std::uint64_t read = 0u, committed = 0u;
    // 固定延迟终端工作用于观察资源机制，本项不报告算法吞吐
    const bool success = RunOrderedBlocks<std::uint64_t, std::uint64_t>(run,
        [&] { return ResourceClock::now() < finishAt; },
        [&](std::uint64_t& input) { input = read++; return true; },
        [](std::uint64_t input, std::uint64_t& output, WorkerContext&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            output = input;
            return true;
        },
        [&](std::uint64_t output) { return output == committed++; });
    observer.request_stop();
    observer.join();
    const bool ended = run.EndRun();
    bool pending = false, hold = false, resumed = false, retentionRestored = false;
    std::size_t initialCompute = 0u, reducedCompute = threads;
    for (std::size_t i = 0u; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        if (i == 0u) { initialCompute = sample.limits.computeLimit; }
        pending |= sample.pending;
        if (sample.phase == ResourceControlPhase::Hold) {
            hold |= sample.admitted == 0u && sample.computing == 0u && !sample.gate;
            reducedCompute = std::min(reducedCompute, sample.limits.computeLimit);
        }
        if (hold && !sample.hostLoad && sample.phase == ResourceControlPhase::Normal && sample.gate) {
            resumed = true;
            retentionRestored |= !sample.retentionPaused;
        }
        const bool changed = i == 0u || sample.phase != samples[i - 1u].phase ||
            sample.pending != samples[i - 1u].pending || sample.gate != samples[i - 1u].gate ||
            sample.retentionPaused != samples[i - 1u].retentionPaused ||
            sample.hostLoad != samples[i - 1u].hostLoad || sample.limits != samples[i - 1u].limits;
        if (!changed) { continue; }
        std::ostringstream row;
        row << "resource_environment_trace,ms=" << sample.milliseconds << ",remaining=" << sample.remaining
            << ",M=" << sample.limits.ownedStorageLimitBytes << ",C=" << sample.limits.computeLimit
            << ",S=" << sample.limits.slotLimit << ",phase=" << static_cast<unsigned>(sample.phase)
            << ",admitted=" << sample.admitted << ",computing=" << sample.computing
            << ",pending=" << sample.pending << ",gate=" << sample.gate
            << ",retention_paused=" << sample.retentionPaused << ",host_load=" << sample.hostLoad;
        result.AddDiagnostic(row.str());
    }
    if (!success || !ended || observerFailure != 0u || read == 0u || read != committed) {
        result.AddFailure("resourceEnvironment.completion", "constrained workload failed to complete in order");
    }
    if (!pending || !hold || !resumed || !retentionRestored || !loadReleased ||
        (initialCompute > 1u && reducedCompute >= initialCompute)) {
        result.AddFailure("resourceEnvironment.response", "real process pressure did not drain, shrink and recover");
    }
    result.AddDiagnostic("resource_environment_scope,Windows_Job_process_limit,host_allocation_at_most_512_MiB,mechanism_only,no_global_pressure_notification_claim");
#else
    result.AddFailure("resourceEnvironment.platform", "this explicit experiment requires Windows Job objects");
#endif
    return result;
}

}

#endif
