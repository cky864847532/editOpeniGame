#include "DataCodec/Test/Experiment/DataCodecResourcePerformance.h"
#include "DataCodec/Workflow/FrameSequence/FrameSequenceEncodeExecutor.h"
#include "DataCodec/Workflow/Session/PlaybackSession.h"
#include <iostream>
#include <iomanip>

namespace {
using namespace datacodec;
using namespace datacodec::test;
using Clock = std::chrono::steady_clock;
constexpr std::size_t frameCount = 6u;
constexpr std::size_t tuples = 262144u;
constexpr std::size_t repeats = 3u;
constexpr std::uint64_t storageBytes = 256u * 1024u * 1024u;
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class Source final : public IFrameSequenceEncodeSource {
public:
    explicit Source(const std::vector<TestDataset>& frames) : m_frames(frames) {}
    std::size_t FrameCount() const noexcept override { return m_frames.size(); }
    bool LoadFrame(std::size_t ordinal, FrameSequenceEncodeFrame& frame, std::string*) override {
        if (ordinal >= m_frames.size()) { return false; }
        frame = {};
        frame.blockTreeAdapter = std::make_unique<TestBlockTreeAdapter>(m_frames[ordinal]);
        frame.rootName = "sequence_benchmark";
        frame.frameIndex = static_cast<std::uint32_t>(ordinal);
        frame.timeValue = static_cast<float>(ordinal);
        for (std::size_t i = 0u; i < m_frames[ordinal].pointFields.size(); ++i) {
            frame.attributeTargets.push_back({frame.frameIndex, "leaf", i});
        }
        return true;
    }
private:
    const std::vector<TestDataset>& m_frames;
};

// 测试宿主保存输出字节，不向编解码内部注入资源设施
class VectorOutput final : public IByteRangeOutput {
public:
    explicit VectorOutput(std::shared_ptr<std::vector<std::uint8_t>> bytes) : m_bytes(std::move(bytes)) {}
    bool WriteAt(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string*) override {
        if (offset > m_bytes->max_size() || bytes.size() > m_bytes->max_size() - offset) { return false; }
        if (offset + bytes.size() > m_bytes->size()) { m_bytes->resize(offset + bytes.size()); }
        std::copy(bytes.begin(), bytes.end(), m_bytes->begin() + static_cast<std::size_t>(offset));
        return true;
    }
    bool Finalize(std::uint64_t size, std::string*) override {
        if (size > m_bytes->max_size()) { return false; }
        m_bytes->resize(size);
        return true;
    }
private:
    std::shared_ptr<std::vector<std::uint8_t>> m_bytes;
};

class Output final : public IFrameSequenceOutputSink {
public:
    std::array<std::shared_ptr<std::vector<std::uint8_t>>, frameCount> frames;
    std::size_t committed{};
    std::unique_ptr<IByteRangeOutput> OpenFrame(std::size_t ordinal, std::uint32_t, std::string*) override {
        if (ordinal >= frames.size()) { return {}; }
        frames[ordinal] = std::make_shared<std::vector<std::uint8_t>>();
        return std::make_unique<VectorOutput>(frames[ordinal]);
    }
    bool CommitFrame(std::size_t ordinal, std::uint32_t, std::uint64_t bytes, std::string*) override {
        if (ordinal >= frames.size() || !frames[ordinal] || frames[ordinal]->size() != bytes || bytes == 0u) { return false; }
        ++committed;
        return true;
    }
    void AbortSequence() noexcept override { frames = {}; committed = 0u; }
};

struct Payload final : IDecodedFramePayload {
    TestDecodeAdapter data;
    std::uint64_t ResidentSizeHint() const noexcept override {
        std::uint64_t bytes = data.Points().size() * sizeof(float);
        for (const auto& field : data.Attributes()) { bytes += field.bytes.size(); }
        return bytes;
    }
};

class Assembly final : public IDecodedFrameAssembly {
public:
    bool BeginFramePackage(const FramePackage&, std::string*) override {
        m_payload = std::make_shared<::Payload>();
        return true;
    }
    bool AddBranch(const FramePackageBranchRecord&, std::string*) override { return true; }
    std::unique_ptr<IDecodeAdapter> CreateLeafAdapter(const FramePackageLeafRecord&, const LeafPackage&, std::string*) override {
        return std::make_unique<TestDecodeAdapter>();
    }
    bool CommitLeaf(const FramePackageLeafRecord&, IDecodeAdapter& adapter, std::string*) override {
        m_payload->data = std::move(static_cast<TestDecodeAdapter&>(adapter));
        return m_payload->data.Committed();
    }
    bool EndFramePackage(std::string*) override { return m_payload && m_payload->data.Committed(); }
    void AbortFramePackage() override { m_payload.reset(); }
    std::unique_ptr<IDecodeAdapter> CreateSupplementAdapter(const BlockPath&, std::string*) const override { return {}; }
    IDecodedFramePayload::Pointer Payload() const noexcept override { return m_payload; }
private:
    std::shared_ptr<::Payload> m_payload;
};

class AssemblyFactory final : public IDecodedFrameAssemblyFactory {
public:
    std::string CacheIdentity() const override { return "sequence-benchmark-all-attributes"; }
    std::shared_ptr<IDecodedFrameAssembly> Create() const override { return std::make_shared<Assembly>(); }
};
}

int main() {
    try {
        std::cout << std::fixed << std::setprecision(4);
        const auto environment = datacodec::ProbeResources();
        std::cout << "sequence_config,frames=" << frameCount << ",tuples_per_frame=" << tuples
                  << ",fields=8,repetitions=" << repeats << ",M=" << storageBytes
                  << ",max_threads=4,key_interval=3,lossless=1,audit=0,prefetch=0,result_cache=0,input_cache=0"
                  << ",available_bytes=" << environment.availableBytes.value_or(0u) << std::endl;
        std::vector<TestDataset> frames;
        frames.reserve(frameCount);
        for (std::size_t f = 0u; f < frameCount; ++f) {
            auto data = resource_experiment::MakeDataset(tuples, "correlated-fields");
            for (auto& field : data.pointFields) {
                for (auto& value : field.values) { value += static_cast<float>(f) * 0.25f; }
            }
            frames.push_back(std::move(data));
        }
        auto control = MakeDefaultCodecControlParams();
        control.attrReference.temporalField.selectionMode = ReferenceSelectionMode::Forced;
        control.geometryReference.temporalField.selectionMode = ReferenceSelectionMode::Forced;
        control.attrReference.temporalField.keyFrameInterval = 3u;
        control.geometryReference.temporalField.keyFrameInterval = 3u;
        struct Config { const char* name; CodecResourceParams resources; };
        const std::array configs{
            Config{"fixed_1", {CodecResourceMode::Fixed, 1u, storageBytes}},
            Config{"fixed_2", {CodecResourceMode::Fixed, 2u, storageBytes}},
            Config{"fixed_4", {CodecResourceMode::Fixed, 4u, storageBytes}},
            Config{"adaptive_4", {CodecResourceMode::Adaptive, 4u, storageBytes}},
        };
        std::array<std::vector<double>, 4u> times;
        std::array<std::size_t, 4u> failures{};
        bool allPassed = true;
        for (std::size_t iteration = 0u; iteration <= repeats; ++iteration) {
            for (std::size_t offset = 0u; offset < configs.size(); ++offset) {
                const auto index = (iteration + offset) % configs.size();
                const auto& config = configs[index];
                Source source(frames);
                Output output;
                const auto encodeBegin = Clock::now();
                const auto encoded = FrameSequenceEncodeExecutor::Execute({
                    .source = &source, .outputSink = &output, .controlParams = &control,
                    .pipelineControl = {.pointOrder = EncodePointOrderMode::Original, .cellOrder = EncodeCellOrderMode::Original},
                    .resources = config.resources,
                });
                const double encodeMs = Milliseconds(encodeBegin);
                const auto referenceStages = std::count_if(encoded.messages.begin(), encoded.messages.end(), [](const auto& message) {
                    return message.origin.starts_with("ReferenceEncode.") && message.text == "stage result=Completed";
                });
                std::size_t decodedCount = 0u;
                std::size_t cacheHits = 0u;
                double decodeMs = 0.0;
                bool matches = true;
                std::string error;
                bool success = encoded.success && encoded.encodedFrameCount == frameCount &&
                    output.committed == frameCount && referenceStages > 0;
                if (success) {
                    PlaybackSequenceOpenRequest request;
                    request.assemblyFactory = std::make_shared<AssemblyFactory>();
                    request.resources = config.resources;
                    request.decodedFrameCachePolicy = {.enabled = false, .prefetchEnabled = false};
                    request.encodedInputCachePolicy.enabled = false;
                    request.loadAllAvailableAttributes = true;
                    for (std::size_t f = 0u; f < frameCount; ++f) {
                        request.decodeSources.push_back({.frameIndex = static_cast<std::uint32_t>(f),
                            .timeValue = static_cast<float>(f), .frameReader = std::make_shared<MemoryByteRangeReader>(output.frames[f]),
                            .sourceIdentity = {.stableId = "sequence-benchmark/frame/" + std::to_string(f), .revision = "1"}});
                        request.playbackFrameOrder.push_back(static_cast<std::uint32_t>(f));
                    }
                    auto begin = Clock::now();
                    auto session = std::make_shared<PlaybackSession>();
                    success = session->OpenSequence(request, &error);
                    decodeMs += Milliseconds(begin);
                    for (std::size_t f = 0u; success && f < frameCount; ++f) {
                        begin = Clock::now();
                        auto decoded = session->RequestFrame({.frameIndex = static_cast<std::uint32_t>(f)});
                        decodeMs += Milliseconds(begin);
                        success = decoded.success && decoded.frame != nullptr;
                        if (!success) {
                            if (decoded.failure) { error = FormatCodecFailure(*decoded.failure); }
                            break;
                        }
                        ++decodedCount;
                        cacheHits += decoded.decodedFrameCacheHit;
                        {
                            auto payload = std::dynamic_pointer_cast<::Payload>(decoded.frame->Payload());
                            matches &= payload && resource_experiment::Matches(frames[f], payload->data);
                        }
                        begin = Clock::now();
                        decoded.frame.reset();
                        session->NotifyFramePresented(static_cast<std::uint32_t>(f));
                        decodeMs += Milliseconds(begin);
                    }
                    begin = Clock::now();
                    session->Reset();
                    session.reset();
                    decodeMs += Milliseconds(begin);
                }
                success = success && matches && decodedCount == frameCount && cacheHits == 0u;
                if (encoded.failure) { error = FormatCodecFailure(*encoded.failure); }
                allPassed &= success;
                if (iteration != 0u) {
                    if (success) { times[index].push_back(encodeMs + decodeMs); }
                    else { ++failures[index]; }
                }
                std::cout << "sequence_run," << config.name << ",iteration=" << iteration << ",warmup=" << (iteration == 0u)
                          << ",success=" << success << ",encoded_frames=" << encoded.encodedFrameCount
                          << ",decoded_frames=" << decodedCount << ",reference_stages=" << referenceStages
                          << ",cache_hits=" << cacheHits << ",encoded_bytes=" << encoded.encodedByteCount
                          << ",encode_ms=" << encodeMs << ",decode_ms=" << decodeMs
                          << ",total_ms=" << encodeMs + decodeMs << ",failure=" << error << std::endl;
            }
        }
        for (std::size_t i = 0u; i < configs.size(); ++i) {
            auto& values = times[i];
            std::sort(values.begin(), values.end());
            std::cout << "sequence_summary," << configs[i].name << ",successes=" << values.size()
                      << ",failures=" << failures[i] << ",median_ms=";
            if (!values.empty()) { std::cout << values[values.size() / 2u]; }
            std::cout << std::endl;
        }
        return allPassed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "sequence_exception," << error.what() << std::endl;
        return 2;
    }
}
