#ifndef DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEPERFORMANCE_H
#define DATACODEC_TEST_EXPERIMENT_DATACODECRESOURCEPERFORMANCE_H

#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Log/Telemetry/TelemetryResidentSet.h"
#include "DataCodec/Platform/ResourceProbe.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>

namespace datacodec::test {
namespace resource_experiment {

// 只保留单个根作用域峰值的最大值，不将不同根或取样对象相加
class CapacitySink final : public IRunRecordSink {
public:
    RunRecordMask Interests() const noexcept override { return RunRecordBit(RunRecordKind::ResourceUsage); }
    void Submit(const RunRecord& record) override {
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
private:
    mutable std::mutex m_mutex;
    std::optional<std::uint64_t> m_peak;
};

inline TestDataset MakeDataset(std::size_t tuples) {
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
    return data;
}

inline bool Matches(const TestDataset& input, const TestDecodeAdapter& output) {
    if (!output.Committed() || output.Points() != input.points ||
        output.Attributes().size() != input.pointFields.size()) { return false; }
    for (std::size_t i = 0u; i < input.pointFields.size(); ++i) {
        const auto& expected = input.pointFields[i];
        const auto& actual = output.Attributes()[i];
        if (!actual.complete || actual.metadata.name != expected.name ||
            actual.bytes.size() != expected.values.size() * sizeof(float) ||
            std::memcmp(actual.bytes.data(), expected.values.data(), actual.bytes.size()) != 0) { return false; }
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
    std::uint64_t storageBytes, std::size_t maxThreads, bool audit = true) {
    using namespace resource_experiment;
    TestResult result;
    if (tuples == 0u || tuples > std::numeric_limits<std::size_t>::max() / (7u * sizeof(float)) ||
        repetitions == 0u || repetitions > 100u || maxThreads == 0u) {
        result.AddFailure("resourcePerformance.arguments", "invalid experiment size, repetition count or thread limit");
        return result;
    }
    const auto environment = ProbeResources();
    const auto data = MakeDataset(tuples);
    std::vector<AttributeTarget> targets{{.attrIndex = 0u}, {.attrIndex = 1u}};
    struct Configuration { const char* name; CodecResourceParams params; };
    const std::array configs{
        Configuration{"fixed_low", {CodecResourceMode::Fixed, 1u, storageBytes}},
        Configuration{"fixed_mid", {CodecResourceMode::Fixed, std::max<std::size_t>(1u, maxThreads / 2u), storageBytes}},
        Configuration{"fixed_high", {CodecResourceMode::Fixed, maxThreads, storageBytes}},
        Configuration{"adaptive", {CodecResourceMode::Adaptive, maxThreads, storageBytes}},
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
    environmentRow << ",external_spill=" << environment.externalSpillAvailable << ",audit=" << audit;
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
            encodeRequest.input = EncodeInput::LeafAdapter(&encodeAdapter, {}, data.name, "PointSet");
            encodeRequest.output = EncodeOutput::Memory(EncodePackageKind::LeafPackage);
            encodeRequest.resources = config.params;
            // 此实验按原始顺序核对数值，重排语义由独立宿主用例验证
            encodeRequest.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
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
                matches = decoded.success && Matches(data, decodeAdapter);
            }
            const auto rssDecoded = ResidentSample();
            const auto encodeMs = Milliseconds(encodeEnd - encodeBegin);
            const bool success = encoded.success && decoded.success && matches;
            const auto* failure = encoded.failure ? &*encoded.failure : (decoded.failure ? &*decoded.failure : nullptr);
            std::ostringstream row;
            row << "resource_csv," << config.name << ',' << iteration << ',' << (iteration == 0u) << ',' << tuples
                << ',' << *config.params.maxComputeThreads << ',' << storageBytes << ',' << success << ','
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
    return result;
}

} // DataCodec 测试命名空间

#endif
