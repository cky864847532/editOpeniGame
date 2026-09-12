#ifndef DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEPERFORMANCE_H
#define DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEPERFORMANCE_H

#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Log/Telemetry/TelemetryResidentSet.h"
#include "DataCodec/Platform/ResourceProbe.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <condition_variable>
#include <thread>

namespace datacodec::test {
namespace resource_experiment {

// 只保留单个根作用域峰值的最大值，不将不同根或取样对象相加
class CapacitySink final : public IRunRecordSink {
public:
    RunRecordMask Interests() const noexcept override { return RunRecordKind::ResourceUsage | RunRecordKind::Message; }
    void Submit(const RunRecord& record) override {
        if (const auto* message = std::get_if<RunMessageRecord>(&record); message && message->message.origin == "MemoryControl") {
            std::lock_guard lock(m_mutex);
            m_memory.push_back(message->message.text);
            return;
        }
        const auto* item = std::get_if<RunResourceUsageRecord>(&record);
        if (!item || item->stage.resource.capacityCoverage != TelemetryCapacityCoverage::OwnedStorageArrays) { return; }
        const auto& usage = item->stage.resource;
        if (!usage.eventPeakCapacityBytes) { return; }
        std::lock_guard lock(m_mutex);
        m_peak = std::max(m_peak.value_or(0u), *usage.eventPeakCapacityBytes);
    }
    std::optional<std::uint64_t> Peak() const {
        std::lock_guard lock(m_mutex);
        return m_peak;
    }
    std::vector<std::string> MemorySummaries() const {
        std::lock_guard lock(m_mutex);
        return m_memory;
    }
private:
    mutable std::mutex m_mutex;
    std::optional<std::uint64_t> m_peak;
    std::vector<std::string> m_memory;
};

// 实验侧记录物理余量时间序列，不向调度器反馈采样结果
class PhysicalTimeline final {
public:
    PhysicalTimeline() {
        m_samples.reserve(4096u);
        m_samples.push_back(ProbeResources());
        m_thread = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(m_mutex);
            while (!stop.stop_requested()) {
                m_wake.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
                if (stop.stop_requested()) { break; }
                if (m_samples.size() < m_samples.capacity()) { m_samples.push_back(ProbeResources()); }
            }
        });
    }
    void Finish(TestResult& result) {
        m_thread.request_stop();
        m_thread.join();
        for (const auto& sample : m_samples) {
            std::ostringstream row;
            row << "resource_physical_sample,ms=" <<
                std::chrono::duration<double, std::milli>(sample.sampledAt - m_samples.front().sampledAt).count()
                << ",total=";
            if (sample.physicalTotalBytes) { row << *sample.physicalTotalBytes; }
            row << ",available=";
            if (sample.availableBytes) { row << *sample.availableBytes; }
            row << ",pressure=" << static_cast<unsigned>(sample.pressure);
            result.AddDiagnostic(row.str());
        }
    }
private:
    std::vector<ResourceSample> m_samples;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::jthread m_thread;
};

inline TestDataset MakeDataset(std::size_t tuples, std::string_view profile = "points") {
    TestDataset data;
    data.name = "resource_performance_float32";
    data.points.resize(tuples * 3u);
    data.pointFields.push_back({.name = "scalar", .role = AttrRole::Scalar, .componentCount = 1u});
    data.pointFields.push_back({.name = "vector", .role = AttrRole::Vector, .componentCount = 3u});
    data.pointFields[0].values.resize(tuples);
    data.pointFields[1].values.resize(tuples * 3u);
    for (std::size_t i = 0u; i < tuples; ++i) {
        // 二进制可精确表示的确定数据，不将输入生成时间混入编解码计时
        data.points[i * 3u] = static_cast<float>(i % 251u) / 16.0f;
        data.points[i * 3u + 1u] = static_cast<float>((i / 251u) % 257u) / 16.0f;
        data.points[i * 3u + 2u] = static_cast<float>(i / (251u * 257u)) / 16.0f;
        data.pointFields[0].values[i] = static_cast<float>((i * 17u) % 4093u) / 32.0f;
        for (std::size_t c = 0u; c < 3u; ++c) {
            data.pointFields[1].values[i * 3u + c] =
                static_cast<float>((i * (c + 3u) + c * 11u) % 8191u) / 64.0f;
        }
    }
    if (profile == "many-fields" || profile == "correlated-fields") {
        const auto fields = profile == "many-fields" ? 128u : 8u;
        data.pointFields.clear();
        for (std::size_t f = 0u; f < fields; ++f) {
            TestNumericField field{.name = "field_" + std::to_string(f), .role = AttrRole::Scalar};
            field.values.resize(tuples);
            for (std::size_t i = 0u; i < tuples; ++i) {
                const auto base = static_cast<float>((i * 17u) % 4093u) / 32.0f;
                field.values[i] = profile == "correlated-fields"
                    ? base * static_cast<float>(f + 1u) + static_cast<float>(f) / 16.0f
                    : static_cast<float>((i * (17u + f * 2u) + f) % 4093u) / 32.0f;
            }
            data.pointFields.push_back(std::move(field));
        }
    }
    if (profile == "variable-topology") {
        data.meshType = MeshType::UnstructuredMesh;
        data.cellOffsets.push_back(0u);
        for (std::size_t cell = 0u; cell < tuples; ++cell) {
            const auto count = 3u + cell % 2u;
            for (std::size_t j = 0u; j < count; ++j) {
                data.cellConnectivity.push_back(static_cast<IndexType>((cell + j) % tuples));
            }
            data.cellOffsets.push_back(static_cast<IndexType>(data.cellConnectivity.size()));
        }
    }
    if (profile == "morton") {
        TestNumericField identity{.name = "original_point", .role = AttrRole::Scalar};
        identity.values.resize(tuples);
        for (std::size_t i = 0u; i < tuples; ++i) { identity.values[i] = static_cast<float>(i); }
        data.pointFields.push_back(std::move(identity));
    }
    return data;
}

inline bool Matches(const TestDataset& input, const TestDecodeAdapter& output, bool reordered = false) {
    if (!output.Committed() || output.Mesh() != input.meshType ||
        output.Points().size() != input.points.size() ||
        output.Connectivity() != input.cellConnectivity || output.Offsets() != input.cellOffsets ||
        output.Attributes().size() != input.pointFields.size()) { return false; }
    for (std::size_t i = 0u; i < input.pointFields.size(); ++i) {
        const auto& expected = input.pointFields[i];
        const auto& actual = output.Attributes()[i];
        if (!actual.complete || actual.metadata.name != expected.name ||
            actual.bytes.size() != expected.values.size() * sizeof(float)) { return false; }
        if (!reordered && std::memcmp(actual.bytes.data(), expected.values.data(), actual.bytes.size()) != 0) {
            return false;
        }
    }
    if (!reordered) { return output.Points() == input.points; }
    // 通过随同重排的唯一点号核对排列、几何和全部属性，不把置换误报为数值错误
    const auto& identities = output.Attributes().back().bytes;
    std::vector<bool> seen(input.PointCount(), false);
    for (std::size_t i = 0u; i < input.PointCount(); ++i) {
        float identity{};
        std::memcpy(&identity, identities.data() + i * sizeof(float), sizeof(float));
        if (!std::isfinite(identity) || identity < 0.0f || identity >= static_cast<double>(input.PointCount())) {
            return false;
        }
        const auto original = static_cast<std::size_t>(identity);
        if (static_cast<float>(original) != identity || seen[original]) { return false; }
        seen[original] = true;
        if (!std::equal(output.Points().begin() + i * 3u, output.Points().begin() + (i + 1u) * 3u,
                input.points.begin() + original * 3u)) { return false; }
        for (std::size_t f = 0u; f < input.pointFields.size(); ++f) {
            const auto& field = input.pointFields[f];
            if (std::memcmp(output.Attributes()[f].bytes.data() + i * field.componentCount * sizeof(float),
                    field.values.data() + original * field.componentCount, field.componentCount * sizeof(float)) != 0) {
                return false;
            }
        }
    }
    return true;
}

inline void Number(std::ostream& output, std::optional<std::uint64_t> value) {
    if (value) { output << *value; }
}

inline std::optional<std::uint64_t> ResidentSample() {
    std::uint64_t bytes = 0u;
    return telemetrydetail::GetResidentSetMemoryBytes(bytes) ? std::optional(bytes) : std::nullopt;
}

inline double Milliseconds(std::chrono::steady_clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

}

inline TestResult RunDataCodecResourcePerformance(std::size_t tuples, std::size_t repetitions,
    std::uint64_t storageBytes, std::size_t maxThreads, bool audit = true,
    std::string_view profile = "points") {
    using namespace resource_experiment;
    TestResult result;
    const bool knownProfile = profile == "points" || profile == "many-fields" ||
        profile == "correlated-fields" || profile == "variable-topology" || profile == "morton";
    if (!knownProfile || tuples == 0u || tuples > std::numeric_limits<std::size_t>::max() / (131u * sizeof(float)) ||
        (profile == "morton" && tuples > (1u << 24u)) ||
        (profile == "variable-topology" && (tuples < 4u || tuples > std::numeric_limits<IndexType>::max() / 4u)) ||
        repetitions == 0u || repetitions > 100u || maxThreads == 0u) {
        result.AddFailure("resourcePerformance.arguments", "invalid experiment size, repetition count or thread limit");
        return result;
    }
    const auto environment = ProbeResources();
    const auto data = MakeDataset(tuples, profile);
    PhysicalTimeline physicalTimeline;
    std::vector<AttributeTarget> targets;
    for (std::size_t i = 0u; i < data.pointFields.size(); ++i) { targets.push_back({.attrIndex = i}); }
    struct Configuration { const char* name; CodecResourceParams params; };
    const std::array configs{
        Configuration{"fixed_low", {CodecResourceMode::Fixed, 1u, storageBytes}},
        Configuration{"fixed_mid", {CodecResourceMode::Fixed, std::max<std::size_t>(1u, maxThreads / 2u), storageBytes}},
        Configuration{"fixed_high", {CodecResourceMode::Fixed, maxThreads, storageBytes}},
        Configuration{"adaptive", {CodecResourceMode::Adaptive, maxThreads}},
    };
    std::array<std::vector<double>, 4u> successfulTimes;
    std::array<std::size_t, 4u> failures{};
    std::ostringstream environmentRow;
    environmentRow << "resource_environment,physical_bytes=";
    Number(environmentRow, environment.physicalTotalBytes);
    environmentRow << ",available_bytes=";
    Number(environmentRow, environment.availableBytes);
    environmentRow << ",allowed_threads=";
    if (environment.allowedComputeThreads) { environmentRow << *environment.allowedComputeThreads; }
    environmentRow << ",external_spill=" << environment.externalSpillAvailable << ",audit=" << audit
        << ",profile=" << profile << ",fields=" << data.pointFields.size() << ",cells=" << data.CellCount();
    result.AddDiagnostic(environmentRow.str());
    result.AddDiagnostic("resource_csv,configuration,iteration,warmup,tuples,requested_threads,storage_limit,success,encoded_bytes,encode_ms,decode_ms,total_ms,encode_max_scope_array_peak,decode_max_scope_array_peak,process_rss_before,process_rss_after_encode,process_rss_after_decode,failure_reason");
    // 首轮预热单列，随后轮换配置顺序，全部请求包括线程启动与最终交付
    for (std::size_t iteration = 0u; iteration <= repetitions; ++iteration) {
        for (std::size_t offset = 0u; offset < configs.size(); ++offset) {
            const auto index = (offset + iteration) % configs.size();
            const auto& config = configs[index];
            auto encodeSink = audit ? std::make_shared<CapacitySink>() : nullptr;
            auto decodeSink = audit ? std::make_shared<CapacitySink>() : nullptr;
            TestEncodeAdapter encodeAdapter(data);
            TestDecodeAdapter decodeAdapter;
            EncodeRequest encodeRequest;
            encodeRequest.input = EncodeInput::LeafAdapter(&encodeAdapter, {}, data.name,
                data.meshType == MeshType::PointSet ? "PointSet" : "UnstructuredMesh");
            encodeRequest.output = EncodeOutput::Memory(EncodePackageKind::LeafPackage);
            encodeRequest.resources = config.params;
            encodeRequest.configuration.pipelineControl.pointOrder = profile == "morton"
                ? EncodePointOrderMode::Morton : EncodePointOrderMode::Original;
            encodeRequest.configuration.pipelineControl.cellOrder = EncodeCellOrderMode::Original;
            if (profile == "many-fields") { encodeRequest.configuration.controlParams.attrReference.enabled = false; }
            encodeRequest.runRecordSink = encodeSink;
            const auto rssBefore = ResidentSample();
            const auto encodeBegin = std::chrono::steady_clock::now();
            auto encoded = Encode(encodeRequest);
            const auto encodeEnd = std::chrono::steady_clock::now();
            const auto rssEncoded = ResidentSample();
            DecodePackageResult decoded;
            double decodeMs = 0.0;
            bool matches = false;
            if (encoded.success && encoded.hasEncodedOutput) {
                auto owner = std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes));
                DecodePackageRequest decodeRequest;
                decodeRequest.inputReader = std::make_shared<MemoryByteRangeReader>(owner);
                decodeRequest.leafAdapter = &decodeAdapter;
                decodeRequest.attributeSelection = AttributeSelectionMode::Explicit;
                decodeRequest.attributeTargets = targets;
                decodeRequest.resources = config.params;
                decodeRequest.runRecordSink = decodeSink;
                const auto begin = std::chrono::steady_clock::now();
                decoded = DecodePackage(decodeRequest);
                decodeMs = Milliseconds(std::chrono::steady_clock::now() - begin);
                matches = decoded.success && Matches(data, decodeAdapter, profile == "morton");
            }
            const auto rssDecoded = ResidentSample();
            const auto encodeMs = Milliseconds(encodeEnd - encodeBegin);
            const bool success = encoded.success && decoded.success && matches;
            const auto* failure = encoded.failure ? &*encoded.failure : (decoded.failure ? &*decoded.failure : nullptr);
            std::ostringstream row;
            row << "resource_csv," << config.name << ',' << iteration << ',' << (iteration == 0u) << ',' << tuples
                << ',' << *config.params.maxComputeThreads << ',';
            Number(row, config.params.ownedStorageLimitBytes);
            row << ',' << success << ','
                << encoded.encodedByteCount << ',' << encodeMs << ',' << decodeMs << ',' << encodeMs + decodeMs << ',';
            Number(row, encodeSink ? encodeSink->Peak() : std::nullopt);
            row << ',';
            Number(row, decodeSink ? decodeSink->Peak() : std::nullopt);
            row << ',';
            Number(row, rssBefore);
            row << ',';
            Number(row, rssEncoded);
            row << ',';
            Number(row, rssDecoded);
            row << ',' << (success ? "none" : failure ? failure->reason.data() : "roundtrip-mismatch");
            result.AddDiagnostic(row.str());
            for (const auto& sink : {encodeSink, decodeSink}) {
                if (sink) {
                    for (const auto& memory : sink->MemorySummaries()) {
                        result.AddDiagnostic(std::string("resource_memory,") + config.name + ",iteration=" +
                            std::to_string(iteration) + ',' + memory);
                    }
                }
            }
            if (!success) {
                if (iteration != 0u) { ++failures[index]; }
                result.AddFailure(std::string("resourcePerformance.") + config.name,
                    failure ? failure->message.data() : "encode/decode round-trip failed");
            } else if (iteration != 0u) {
                successfulTimes[index].push_back(encodeMs + decodeMs);
            }
        }
    }
    for (std::size_t i = 0u; i < configs.size(); ++i) {
        auto& times = successfulTimes[i];
        std::sort(times.begin(), times.end());
        std::ostringstream summary;
        summary << "resource_summary," << configs[i].name << ",successes=" << times.size() << ",failures=" << failures[i]
            << ",median_success_ms=";
        if (!times.empty()) {
            const auto middle = times.size() / 2u;
            summary << (times.size() % 2u ? times[middle] : (times[middle - 1u] + times[middle]) / 2.0);
        }
        result.AddDiagnostic(summary.str());
    }
    physicalTimeline.Finish(result);
    return result;
}

// 拆开固定管理成本，计时区间内不生成诊断字符串
inline TestResult RunDataCodecResourceOverhead() {
    using namespace resource_experiment;
    TestResult result;
    constexpr std::size_t repetitions = 100u;
    const auto report = [&](const char* name, std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        std::ostringstream row;
        row << "resource_overhead," << name << ",samples=" << values.size()
            << ",median_ms=" << (values[49u] + values[50u]) / 2.0;
        result.AddDiagnostic(row.str());
    };
    std::vector<double> probes;
    probes.reserve(repetitions);
    ResourceSample sample;
    for (std::size_t i = 0u; i <= repetitions; ++i) {
        const auto begin = ResourceClock::now();
        sample = ProbeResources();
        const auto elapsed = Milliseconds(ResourceClock::now() - begin);
        if (i != 0u) { probes.push_back(elapsed); }
    }
    report("probe", probes);
    std::array<std::vector<double>, 3u> monitorTimes;
    for (auto& values : monitorTimes) { values.reserve(repetitions); }
    for (std::size_t i = 0u; i <= repetitions; ++i) {
        const auto begin = ResourceClock::now();
        auto monitor = std::make_unique<ResourcePressureMonitor>(nullptr, nullptr);
        const auto constructed = ResourceClock::now();
        monitor->Observe(sample);
        const auto observed = ResourceClock::now();
        monitor.reset();
        const auto destroyed = ResourceClock::now();
        if (i != 0u) {
            monitorTimes[0].push_back(Milliseconds(constructed - begin));
            monitorTimes[1].push_back(Milliseconds(observed - constructed));
            monitorTimes[2].push_back(Milliseconds(destroyed - observed));
        }
    }
    report("monitor_create", monitorTimes[0]);
    report("monitor_observe", monitorTimes[1]);
    report("monitor_destroy", monitorTimes[2]);
    for (const auto mode : {CodecResourceMode::Fixed, CodecResourceMode::Adaptive}) {
        std::array<std::vector<double>, 4u> times;
        for (auto& values : times) { values.reserve(repetitions); }
        for (std::size_t i = 0u; i <= repetitions; ++i) {
            const auto begin = ResourceClock::now();
            auto run = std::make_unique<DataCodecExecutionResources>(CodecResourceParams{
                mode, 1u, mode == CodecResourceMode::Fixed ? std::optional<std::uint64_t>(16u * 1024u * 1024u) : std::nullopt});
            const auto constructed = ResourceClock::now();
            const bool began = run->BeginRun();
            const auto started = ResourceClock::now();
            const bool ended = began && run->EndRun();
            const auto finished = ResourceClock::now();
            run.reset();
            const auto destroyed = ResourceClock::now();
            if (!began || !ended) {
                result.AddFailure("resourceOverhead.lifecycle", "empty request lifecycle failed");
                return result;
            }
            if (i != 0u) {
                times[0].push_back(Milliseconds(constructed - begin));
                times[1].push_back(Milliseconds(started - constructed));
                times[2].push_back(Milliseconds(finished - started));
                times[3].push_back(Milliseconds(destroyed - finished));
            }
        }
        const bool fixed = mode == CodecResourceMode::Fixed;
        report(fixed ? "fixed_create" : "adaptive_create", times[0]);
        report(fixed ? "fixed_begin" : "adaptive_begin", times[1]);
        report(fixed ? "fixed_end" : "adaptive_end", times[2]);
        report(fixed ? "fixed_destroy" : "adaptive_destroy", times[3]);
    }
    return result;
}

} // DataCodec 测试命名空间

#endif
