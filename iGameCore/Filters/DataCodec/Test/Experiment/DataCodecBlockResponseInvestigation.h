#pragma once

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace datacodec::test {

// 对照组只提供有界文件读取，隔离桌面文件映射与预取对物理响应的影响
class ResponseStreamReader final : public IByteRangeReader {
public:
    explicit ResponseStreamReader(const std::filesystem::path& path)
        : m_input(path, std::ios::binary), m_size(std::filesystem::file_size(path)) {
        if (!m_input) { throw std::runtime_error("response stream reader could not open input"); }
    }
    std::uint64_t ByteSize() const noexcept override { return m_size; }
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error) override {
        if (offset > m_size || output.size() > m_size - offset) {
            return validation::AssignError(error, "response stream range exceeds file");
        }
        std::lock_guard lock(m_mutex);
        m_input.clear();
        m_input.seekg(static_cast<std::streamoff>(offset));
        m_input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        return m_input && static_cast<std::size_t>(m_input.gcount()) == output.size() ? true :
            validation::AssignError(error, "response stream read failed");
    }
private:
    std::ifstream m_input;
    std::uint64_t m_size;
    std::mutex m_mutex;
};

// 调查进程独立读取物理与提交状态，不把观测数据接入生产审计或调度
struct ResponsePhysicalSample {
    std::uint64_t available{}, workingSet{}, privateBytes{}, faults{}, commitRemaining{};
    bool valid{};
};
inline ResponsePhysicalSample ReadResponsePhysicalSample() {
    MEMORYSTATUSEX system{};
    system.dwLength = sizeof(system);
    PROCESS_MEMORY_COUNTERS_EX process{};
    ResponsePhysicalSample out;
    out.valid = GlobalMemoryStatusEx(&system) && GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process));
    if (out.valid) {
        out.available = system.ullAvailPhys; out.workingSet = process.WorkingSetSize;
        out.privateBytes = process.PrivateUsage; out.faults = process.PageFaultCount;
        out.commitRemaining = system.ullAvailPageFile;
    }
    return out;
}

class BlockResponseInvestigationRun {
    struct Row {
        const char* point{};
        double ms{}, sampleAge{};
        ResponsePhysicalSample physical;
        std::uint64_t sampledAvailable{}, reserve{}, recovery{}, limit{}, reserved{}, allocated{}, allocations{},
            retained{}, reused{}, acquired{}, flow{}, block{}, plan{}, missing{}, next{}, retired{};
        std::size_t admitted{}, computing{}, queued{}, slots{};
        double previewRatio{};
        bool gate{};
    };
    DataCodecExecutionResources& m_run;
    IRunRecordSink& m_sink;
    ResourceClock::time_point m_start{ResourceClock::now()};
    std::mutex m_mutex;
    std::vector<Row> m_rows;
    std::atomic<std::size_t> m_missed{}, m_dropped{};
    bool m_probe{}, m_granted{}, m_admitted{}, m_completed{};
    std::uint64_t m_probeFlow{}, m_probeBlock{}, m_attempts{};
    std::optional<ResourceClock::time_point> m_waitSince;
    std::jthread m_sampler;
    BlockResponseInvestigation m_previous;

    void Capture(const char* point, const DecodeBlockMemoryPlan* plan = nullptr, std::uint64_t missing = 0u) noexcept {
        ResourceDebugSnapshot s;
        if (!m_run.TryCopyResourceDebugSnapshot(s)) { ++m_missed; return; }
        const auto physical = ReadResponsePhysicalSample();
        const auto scratch = m_run.Scratch().SnapshotStats();
        Row row;
        row.point = point;
        row.ms = std::chrono::duration<double, std::milli>(ResourceClock::now() - m_start).count();
        row.sampleAge = std::chrono::duration<double, std::milli>(ResourceClock::now() - s.resourceSample.sampledAt).count();
        row.physical = physical; row.sampledAvailable = s.resourceSample.availableBytes.value_or(0u);
        row.reserve = s.memory.reserveBytes; row.recovery = s.memory.recoveryBytes;
        row.limit = s.limits.ownedStorageLimitBytes.value_or(0u); row.reserved = s.storage.reservedBytes;
        row.allocated = s.allocatedStorage.liveBytes; row.allocations = s.allocatedStorage.allocationCount;
        row.retained = scratch.retainedBytes; row.reused = scratch.reusedBlockCount; row.acquired = scratch.acquiredBytes;
        row.flow = s.flowId; row.block = plan ? plan->block : 0u; row.plan = plan ? plan->TotalBytes() : 0u;
        row.missing = missing; row.next = s.nextWorkBytes; row.retired = s.lastRetired.value_or(0u);
        row.previewRatio = s.memory.previewRatio;
        row.admitted = s.admittedBlocks; row.computing = s.activeComputeUnits; row.queued = s.queuedTasks;
        row.slots = s.limits.slotLimit; row.gate = s.gateOpen;
        std::lock_guard lock(m_mutex);
        if (m_rows.size() < m_rows.capacity()) { m_rows.push_back(row); }
        else { ++m_dropped; }
    }

    void OnBlock(const char* point, const DecodeBlockMemoryPlan& plan, std::uint64_t missing) noexcept {
        const std::string_view name(point);
        // 边界干预仅允许单线程单槽位，在真实等待且满足保留目标时尝试一块
        if (m_probe && !m_admitted && name == "admission.try") {
            ResourceDebugSnapshot s;
            if (m_run.TryCopyResourceDebugSnapshot(s) && s.byteWaiting && s.nextWorkBytes != 0u &&
                s.admittedBlocks == 0u && s.activeComputeUnits == 0u && s.queuedTasks == 0u && !m_run.Stopped()) {
                if (!m_waitSince) { m_waitSince = ResourceClock::now(); }
                const auto physical = ReadResponsePhysicalSample();
                if (ResourceClock::now() - *m_waitSince >= std::chrono::milliseconds(500) && physical.valid &&
                    physical.available >= s.memory.reserveBytes &&
                    missing <= physical.available - s.memory.reserveBytes && missing <= physical.commitRemaining) {
                    Capture("probe.before", &plan, missing);
                    auto limits = s.limits;
                    limits.ownedStorageLimitBytes = s.storage.reservedBytes + missing;
                    limits.computeLimit = 1u; limits.slotLimit = 1u;
                    ++m_attempts;
                    m_granted = m_run.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck);
                    m_probeFlow = s.flowId; m_probeBlock = plan.block;
                }
            }
        }
        const bool probeBlock = m_probe && m_granted && plan.block == m_probeBlock;
        const auto ordinal = plan.block / 65536u;
        const bool selected = ordinal < 2u || ordinal % 64u == 0u || probeBlock;
        if (selected && name != "admission.try") { Capture(point, &plan, missing); }
        if (probeBlock && name == "allocated") {
            m_admitted = true;
            ResourceDebugSnapshot s;
            if (m_run.TryCopyResourceDebugSnapshot(s)) {
                m_run.UpdateLimits(s.limits, false, ResourceDecisionReason::MechanismCheck);
            }
        }
        if (probeBlock && m_admitted && name == "retired") {
            m_completed = true;
            // 记录单块结束后的短时物理响应，再正常取消剩余解码
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            Capture("probe.settled", &plan, missing);
            m_run.RequestStop();
        }
    }
public:
    BlockResponseInvestigationRun(DataCodecExecutionResources& run, IRunRecordSink& sink, bool probe)
        : m_run(run), m_sink(sink), m_probe(probe), m_previous(blockResponseInvestigation) {
        m_rows.reserve(16000u);
        blockResponseInvestigation = {this, [](void* self, DataCodecExecutionResources&, const char* point,
            const DecodeBlockMemoryPlan& plan, std::uint64_t missing) noexcept {
            static_cast<BlockResponseInvestigationRun*>(self)->OnBlock(point, plan, missing);
        }};
        m_sampler = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                Capture("sample");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    ~BlockResponseInvestigationRun() {
        m_sampler.request_stop();
        if (m_sampler.joinable()) { m_sampler.join(); }
        blockResponseInvestigation = m_previous;
    }
    void StageMessage(std::string_view message) {
        if (message.starts_with("point=attributes.prepare.begin;")) { Capture("host.prepare.begin"); }
        if (message.starts_with("point=attributes.prepare.end;")) { Capture("host.prepare.end"); }
        if (message.starts_with("point=stage.begin.AttrDecodeStage;")) { Capture("attributes.begin"); }
        if (message.starts_with("point=commit.geometry.begin;")) { Capture("geometry.commit.begin"); }
        if (message.starts_with("point=commit.attributes.end;")) { Capture("attributes.commit.end"); }
    }
    bool ProbeCompleted() const noexcept { return m_completed; }
    void Finish() {
        m_sampler.request_stop();
        if (m_sampler.joinable()) { m_sampler.join(); }
        Capture("finish");
        const auto elapsed = std::chrono::duration<double>(ResourceClock::now() - m_start).count();
        std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.ms < b.ms; });
        for (const auto& r : m_rows) {
            std::ostringstream out;
            out << "RESPONSE point=" << r.point << " ms=" << r.ms << " valid=" << r.physical.valid
                << " available=" << r.physical.available << " ws=" << r.physical.workingSet
                << " private=" << r.physical.privateBytes << " faults=" << r.physical.faults
                << " commit_remaining=" << r.physical.commitRemaining
                << " sample_available=" << r.sampledAvailable << " sample_age_ms=" << r.sampleAge
                << " reserve=" << r.reserve << " recovery=" << r.recovery << " limit=" << r.limit
                << " reserved=" << r.reserved << " allocated=" << r.allocated << " allocations=" << r.allocations
                << " retained=" << r.retained << " reused=" << r.reused << " acquired=" << r.acquired
                << " flow=" << r.flow << " block=" << r.block << " plan=" << r.plan << " missing=" << r.missing
                << " next=" << r.next << " retired=" << r.retired << " admitted=" << r.admitted
                << " computing=" << r.computing << " queued=" << r.queued << " slots=" << r.slots
                << " gate=" << r.gate << " preview_ratio=" << r.previewRatio;
            RunMessageRecord message;
            message.message.code = "block-response-investigation"; message.message.text = out.str();
            m_sink.TrySubmit(message);
        }
        std::ostringstream out;
        out << "RESPONSE_END samples=" << m_rows.size() << " missed=" << m_missed.load()
            << " dropped=" << m_dropped.load() << " elapsed_seconds=" << elapsed
            << " probe=" << m_probe << " granted=" << m_granted << " probe_admitted=" << m_admitted
            << " probe_completed=" << m_completed << " attempts=" << m_attempts << " flow=" << m_probeFlow
            << " block=" << m_probeBlock;
        RunMessageRecord message;
        message.message.code = "block-response-investigation"; message.message.text = out.str();
        m_sink.TrySubmit(message);
    }
};
}
