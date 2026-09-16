#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/Test/Experiment/DataCodecResourcePerformance.h"
#include "DataCodec/API/Entry/DataCodecFrameSequenceEncode.h"
#include "DataCodec/API/Entry/PlaybackSession.h"
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
            Config{"adaptive_4", {CodecResourceMode::Adaptive, 4u}},
        };
        std::array<std::vector<double>, 4u> times;
        std::array<std::size_t, 4u> failures{};
        bool allPassed = true;
        for (std::size_t iteration = 0u; iteration <= repeats; ++iteration) {
            for (std::size_t offset = 0u; offset < configs.size(); ++offset) {
                const auto index = (iteration + offset) % configs.size();
                const auto& config = configs[index];
                auto source = std::make_shared<Source>(frames);
                const auto encodeBegin = Clock::now();
                auto encoded = EncodeFrameSequence({
                    .source = source, .configuration = {.controlParams = control,
                    .pipelineControl = {.pointOrder = EncodePointOrderMode::Original, .cellOrder = EncodeCellOrderMode::Original}},
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
                    encoded.frames.size() == frameCount && referenceStages > 0;
                if (success) {
                    PlaybackSequenceOpenRequest request;
                    request.resources = config.resources;
                    request.decodedFrameCachePolicy = {.enabled = false, .prefetchEnabled = false};
                    request.encodedInputCachePolicy.enabled = false;
                    request.loadAllAvailableAttributes = true;
                    for (std::size_t f = 0u; f < frameCount; ++f) {
                        request.decodeSources.push_back({.frameIndex = static_cast<std::uint32_t>(f),
                            .timeValue = static_cast<float>(f), .input = ::datacodec::EncodedInputAccess::Retain(std::make_shared<MemoryByteRangeReader>(std::move(encoded.frames[f].bytes))),
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
                            TestDecodeAdapter consumer;
                            matches &= decoded.frame->Data().leaves.size() == 1u && consumer.Import(decoded.frame->Data().leaves[0]) && resource_experiment::Matches(frames[f], consumer);
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
