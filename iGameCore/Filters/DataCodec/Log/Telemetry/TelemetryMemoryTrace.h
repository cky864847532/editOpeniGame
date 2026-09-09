#ifndef DATACODEC_LOG_TELEMETRY_TELEMETRYMEMORYTRACE_H
#define DATACODEC_LOG_TELEMETRY_TELEMETRYMEMORYTRACE_H

#include "DataCodec/API/Adapter/RunRecord.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Log/Telemetry/TelemetryResidentSet.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace datacodec {

// 每个样本保留自身作用域与取样峰值，独立对象的峰值不相加
template<class Records>
inline void RecordBufferCapacitySamples(
    Records& records,
    const std::span<const BufferCapacitySample> samples) noexcept {
    if (!records.Wants(RunRecordKind::ResourceUsage)) { return; }
    records.TryExport([&] {
        for (const auto& sample : samples) {
            if (!sample.capacityBytes.has_value()) { continue; }
            TelemetryResourceUsage usage;
            usage.valid = true;
            usage.capacityCoverage = TelemetryCapacityCoverage::SampledBuffer;
            usage.capacityScopeId = sample.scopeId;
            usage.capacitySampleNanoseconds = sample.sampledAtNanoseconds;
            usage.trackedCapacityBytes = sample.capacityBytes;
            usage.sampledPeakCapacityBytes = sample.sampledPeakBytes;
            records.RecordResourceUsage(std::string(sample.name), usage);
        }
    });
}

// 编解码阶段复用同一导出回调，回调构造失败只记录诊断缺失
template<class Records>
inline callback::CapacityCallback MakeCapacityRecordCallback(Records& records) noexcept {
    callback::CapacityCallback result;
    if (records.Wants(RunRecordKind::ResourceUsage)) {
        records.TryExport([&] {
            result = [&records](const std::span<const BufferCapacitySample> samples) {
                RecordBufferCapacitySamples(records, samples);
            };
        });
    }
    return result;
}

// 一次性临时数组在实际存活时取样，不推断未观察的重分配峰值
template<class Records, class T, class Allocator>
inline void RecordVectorCapacitySample(
    Records& records,
    const std::string_view name,
    const std::vector<T, Allocator>& values) noexcept {
    if (!records.Wants(RunRecordKind::ResourceUsage)) { return; }
    BufferCapacitySample sample{name};
    sample.Observe(values);
    RecordBufferCapacitySamples(records, {&sample, 1u});
}

// 只读取明确列出的外层数组，同一时刻存在的属性布局数组按集合记录
template<class Records>
inline void RecordCodecStorageParamsCapacity(Records& records, const CodecStorageParams& params) noexcept {
    if (!records.Wants(RunRecordKind::ResourceUsage)) { return; }
    std::array<BufferCapacitySample, 4> samples{{
        BufferCapacitySample{"metadata.geometry.block_layout_outer_array"},
        BufferCapacitySample{"metadata.topology.block_layout_outer_array"},
        BufferCapacitySample{"metadata.attribute_descriptor_outer_array"},
        BufferCapacitySample{"metadata.attributes.block_layout_outer_arrays"},
    }};
    samples[0].Observe(params.geomParams.blockLayouts);
    samples[1].Observe(params.topoParams.connectivityLayout.blockLayouts);
    samples[2].Observe(params.attrParams);
    std::uint64_t attributeLayoutCapacity = 0u;
    for (const auto& attribute : params.attrParams) {
        attributeLayoutCapacity += static_cast<std::uint64_t>(attribute.blockLayouts.capacity()) *
            sizeof(NumericArrayBlockLayoutParams);
    }
    samples[3].Observe(attributeLayoutCapacity);
    RecordBufferCapacitySamples(records, samples);
}

// 根数组事件与空闲 scratch 取样分别导出，跨叶快照不作容量加总
template<class Records, class Resources>
inline void RecordRootCapacityAudit(Records& records, Resources& resources) noexcept {
    if (!records.Wants(RunRecordKind::ResourceUsage)) { return; }
    records.TryExport([&] {
        const auto allocated = resources.StorageCapacity()->AllocatedStorage();
        TelemetryResourceUsage storage;
        storage.valid = true;
        storage.capacityCoverage = TelemetryCapacityCoverage::OwnedStorageArrays;
        storage.capacityScopeId = allocated.scopeId;
        storage.capacitySampleNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(allocated.capturedAt.time_since_epoch()).count());
        storage.trackedCapacityBytes = allocated.liveBytes;
        storage.eventPeakCapacityBytes = allocated.peakLiveBytes;
        records.RecordResourceUsage("tracked.storage.arrays", storage);

        const auto retained = resources.Scratch().SnapshotStats();
        TelemetryResourceUsage scratch;
        scratch.valid = true;
        scratch.capacityCoverage = TelemetryCapacityCoverage::RetainedScratch;
        scratch.capacityScopeId = allocated.scopeId;
        scratch.capacitySampleNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        scratch.trackedCapacityBytes = retained.retainedBytes;
        records.RecordResourceUsage("tracked.scratch.retained", scratch);
    });
}

struct TelemetryMemoryModuleSummary {
    std::string name;
    std::uint64_t beforeWorkingSetBytes{0u};
    std::uint64_t afterWorkingSetBytes{0u};
    std::uint64_t sampledPeakWorkingSetBytes{0u};
};

struct TelemetryMemoryTraceSummary {
    bool valid{false};
    std::uint64_t beforeWorkingSetBytes{0u};
    std::uint64_t afterWorkingSetBytes{0u};
    std::uint64_t sampledPeakWorkingSetBytes{0u};
    std::vector<TelemetryMemoryModuleSummary> modules;
};

class TelemetryMemoryTraceRecorder {
public:
    TelemetryMemoryTraceRecorder() = default;
    TelemetryMemoryTraceRecorder(const TelemetryMemoryTraceRecorder&) = delete;
    TelemetryMemoryTraceRecorder& operator=(const TelemetryMemoryTraceRecorder&) = delete;

    ~TelemetryMemoryTraceRecorder() {
        std::string ignored;
        (void)Stop(&ignored);
    }

    bool Start(
        const RunRecordInfo&,
        std::string* error = nullptr) {
        std::string stopError;
        if (!Stop(&stopError)) {
            return validation::AssignError(error, stopError);
        }

        std::uint64_t residentSetBytes = 0u;
        if (!SampleResidentSet(residentSetBytes, error)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_summary = {};
            m_summary.valid = true;
            m_summary.beforeWorkingSetBytes = residentSetBytes;
            m_summary.afterWorkingSetBytes = residentSetBytes;
            m_summary.sampledPeakWorkingSetBytes = residentSetBytes;
        }
        m_active.store(true, std::memory_order_release);
        return true;
    }

    bool Stop(std::string* error = nullptr) {
        if (!m_active.exchange(false, std::memory_order_acq_rel)) {
            return true;
        }
        std::uint64_t residentSetBytes = 0u;
        if (!SampleResidentSet(residentSetBytes, error)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_summary.afterWorkingSetBytes = residentSetBytes;
            m_summary.sampledPeakWorkingSetBytes = std::max(
                m_summary.sampledPeakWorkingSetBytes,
                residentSetBytes);
        }
        return true;
    }

    void EnterScope(std::string, std::string moduleName) {
        if (!Active()) {
            return;
        }
        std::uint64_t residentSetBytes = 0u;
        if (!SampleResidentSet(residentSetBytes, nullptr)) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        RecordModuleSampleLocked(std::move(moduleName), residentSetBytes);
    }

    void LeaveScope(std::string, std::string moduleName) {
        if (!Active()) {
            return;
        }
        std::uint64_t residentSetBytes = 0u;
        if (!SampleResidentSet(residentSetBytes, nullptr)) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        RecordModuleSampleLocked(std::move(moduleName), residentSetBytes);
    }

    [[nodiscard]] bool Active() const noexcept {
        return m_active.load(std::memory_order_acquire);
    }

    [[nodiscard]] TelemetryMemoryTraceSummary Summary() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_summary;
    }

private:
    static bool SampleResidentSet(std::uint64_t& residentSetBytes, std::string* error) {
        return telemetrydetail::GetResidentSetMemoryBytes(residentSetBytes, error);
    }

    void RecordModuleSampleLocked(
        std::string moduleName,
        const std::uint64_t residentSetBytes) {
        m_summary.afterWorkingSetBytes = residentSetBytes;
        m_summary.sampledPeakWorkingSetBytes = std::max(
            m_summary.sampledPeakWorkingSetBytes,
            residentSetBytes);
        const auto iterator = std::find_if(
            m_summary.modules.begin(),
            m_summary.modules.end(),
            [&moduleName](const auto& module) { return module.name == moduleName; });
        if (iterator == m_summary.modules.end()) {
            m_summary.modules.push_back({
                .name = std::move(moduleName),
                .beforeWorkingSetBytes = residentSetBytes,
                .afterWorkingSetBytes = residentSetBytes,
                .sampledPeakWorkingSetBytes = residentSetBytes,
            });
            return;
        }
        iterator->afterWorkingSetBytes = residentSetBytes;
        iterator->sampledPeakWorkingSetBytes = std::max(
            iterator->sampledPeakWorkingSetBytes,
            residentSetBytes);
    }

    std::atomic_bool m_active{false};
    TelemetryMemoryTraceSummary m_summary;
    mutable std::mutex m_mutex;
};

inline std::string ResolveTelemetryMemoryTraceModule(const std::string_view stageName) {
    if (stageName.find("Geometry") != std::string_view::npos) {
        return "geometry";
    }
    if (stageName.find("Topo") != std::string_view::npos) {
        return "topology";
    }
    if (stageName.find("Attr") != std::string_view::npos ||
        stageName.find("Attribute") != std::string_view::npos) {
        return "attribute";
    }
    if (stageName.find("Remap") != std::string_view::npos) {
        return "remap";
    }
    if (stageName.find("Params") != std::string_view::npos) {
        return "params";
    }
    if (stageName.find("Commit") != std::string_view::npos) {
        return "commit";
    }
    return "pipeline";
}

[[nodiscard]] inline TelemetryResourceUsage MakeTelemetryMemoryResourceUsage(
    const std::uint64_t beforeWorkingSetBytes,
    const std::uint64_t afterWorkingSetBytes,
    const std::uint64_t sampledPeakWorkingSetBytes) {
    return TelemetryResourceUsage{
        .valid = true,
        .workingSetBytes = afterWorkingSetBytes,
        .workingSetBeforeBytes = beforeWorkingSetBytes,
        .workingSetAfterBytes = afterWorkingSetBytes,
        .sampledPeakWorkingSetBytes = sampledPeakWorkingSetBytes,
    };
}

template <typename TRunRecords>
inline void RecordTelemetryMemoryTraceSummary(
    TRunRecords& records,
    const TelemetryMemoryTraceSummary& summary) {
    if (!summary.valid) {
        return;
    }
    records.TryExport([&] {
    records.RecordResourceUsage(
        "memory.run",
        MakeTelemetryMemoryResourceUsage(
            summary.beforeWorkingSetBytes,
            summary.afterWorkingSetBytes,
            summary.sampledPeakWorkingSetBytes));
    for (const auto& module : summary.modules) {
        const auto category = module.name == "topology"
            ? TelemetryStageCategory::Topology
            : module.name == "geometry"
                ? TelemetryStageCategory::Geometry
                : module.name == "attribute"
                    ? TelemetryStageCategory::Attribute
                    : module.name == "remap"
                        ? TelemetryStageCategory::Remap
                        : module.name == "params"
                            ? TelemetryStageCategory::Params
                            : module.name == "commit"
                                ? TelemetryStageCategory::Commit
                                : TelemetryStageCategory::General;
        records.RecordResourceUsage(
            "memory." + module.name,
            MakeTelemetryMemoryResourceUsage(
                module.beforeWorkingSetBytes,
                module.afterWorkingSetBytes,
                module.sampledPeakWorkingSetBytes),
            category);
    }
    });
}

template <typename TContext>
inline void RecordMemoryTraceStageEvent(
    TContext& context,
    const std::string_view stageName,
    const bool enter) {
    if (context.memoryTrace == nullptr || !context.memoryTrace->Active()) {
        return;
    }
    context.runRecords.TryExport([&] {
        std::string stage(stageName);
        auto module = ResolveTelemetryMemoryTraceModule(stageName);
        if (enter) {
            context.memoryTrace->EnterScope(std::move(stage), std::move(module));
            return;
        }
        context.memoryTrace->LeaveScope(std::move(stage), std::move(module));
    });
}

} // namespace datacodec

#endif
