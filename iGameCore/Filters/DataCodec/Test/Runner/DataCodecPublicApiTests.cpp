#include <DataCodec/API/Entry/DataCodecEncodeEntry.h>
#include <DataCodec/API/Entry/DataCodecDecodeEntry.h>
#include <DataCodec/API/Entry/DataCodecFrameSequenceEncode.h>
#include <DataCodec/API/Entry/InspectEncodedInput.h>
#include <DataCodec/API/Entry/PackageDecodeSession.h>
#include <DataCodec/API/Entry/PlaybackSession.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace datacodec;

// 第三方接入只提供业务输入，并通过公开入口接收拥有型结果
class PointsInput final : public IEncodeAdapter {
public:
    std::array<float, 9u> points{0, 0, 0, 1, 0, 0, 0, 1, 0};
    MeshType GetMeshType() const override { return MeshType::PointSet; }
    std::string GetName() const override { return "public-api"; }
    std::size_t GetNumberOfPoints() const override { return 3u; }
    const float* TryGetPointsF32() const override { return points.data(); }
    void GetPoint(std::size_t index, double output[3]) const override {
        for (std::size_t i = 0; i < 3u; ++i) { output[i] = points.at(index * 3u + i); }
    }
    bool DescribeTopology(TopologyInputDescriptor& output, std::string*) const override {
        output = {};
        output.pointCount = 3u;
        return true;
    }
    std::size_t GetNumberOfCells() const override { return 0u; }
    std::size_t GetCellIdBufferSize() const override { return 0u; }
    const IndexType* GetCellIdBufferPtr() const override { return nullptr; }
    const IndexType* GetCellIdOffsetPtr() const override { return nullptr; }
    bool IsFixedCellSize() const override { return false; }
    int GetFixedCellSize() const override { return 0; }
    std::size_t GetCellFaceBufferSize() const override { return 0u; }
    std::size_t GetNumberOfPointAttrs() const override { return 0u; }
    std::size_t GetNumberOfCellAttrs() const override { return 0u; }
    const IEncodeAttrView& GetPointAttr(std::size_t) const override { throw std::out_of_range("attribute"); }
    const IEncodeAttrView& GetCellAttr(std::size_t) const override { throw std::out_of_range("attribute"); }
    void ResetInput() override {}
};

void Require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("datacodec-public-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    try {
        std::filesystem::create_directories(directory);
        std::array<std::uint8_t, 128u> shared{};
        std::array<std::uint8_t, 16u> independent{};
        const std::array<InputMemoryView, 4u> ranges{{
            {shared.data(), 0u, 32u, 128u}, {shared.data(), 16u, 48u, {}},
            {independent.data(), 0u, 16u, 16u}, {nullptr, 0u, 12u, {}}}};
        const auto observed = ObserveInputMemory(ranges);
        Require(observed.describedBytes == 92u && observed.knownCapacityBytes == 144u &&
            observed.unknownCapacityRegions == 1u, "shared input observation lost or duplicated allocations");
        bool invalidCapacity = false;
        try {
            const InputMemoryView invalid{shared.data(), 120u, 16u, 128u};
            (void)ObserveInputMemory(std::span<const InputMemoryView>(&invalid, 1u));
        } catch (const std::invalid_argument&) { invalidCapacity = true; }
        Require(invalidCapacity, "invalid input capacity was accepted");
        invalidCapacity = false;
        try {
            const std::array<InputMemoryView, 2u> inconsistent{{
                {shared.data(), 0u, 16u, 32u}, {shared.data(), 16u, 48u, {}}}};
            (void)ObserveInputMemory(inconsistent);
        } catch (const std::invalid_argument&) { invalidCapacity = true; }
        Require(invalidCapacity, "overlapping views exceeded a shared capacity");
        const auto file = directory / "points.igc";
        auto input = std::make_shared<PointsInput>();
        const auto expected = input->points;
        const std::weak_ptr<PointsInput> lifetime = input;
        EncodeRequest request{.input = EncodeInput::LeafAdapter(input),
            .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
            .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 1u}};
        request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
        input.reset();
        std::stop_source stopped;
        stopped.request_stop();
        request.stopToken = stopped.get_token();
        const auto cancelled = Encode(request);
        Require(!cancelled.success && cancelled.failure && cancelled.failure->cancelled &&
            cancelled.encodedBytes.empty(), "cancelled encode published a result");
        request.stopToken = {};
        auto encoded = Encode(request);
        Require(encoded.success && !encoded.encodedBytes.empty(), "memory encode failed");
        Require(encoded.inputMemory.describedBytes == sizeof(expected) &&
            encoded.inputMemory.knownCapacityBytes == 0u && encoded.inputMemory.unknownCapacityRegions == 1u,
            "default adapter observation invented an allocation capacity");
        request.output = EncodeOutput::File(file, EncodePackageKind::LeafPackage);
        Require(Encode(request).success, "file encode failed");
        request = {};
        Require(lifetime.expired(), "completed encode retained business input");
        auto memory = EncodedInput::Memory(std::move(encoded.encodedBytes));
        Require(memory.ObserveMemory().knownCapacityBytes >= memory.ObserveMemory().describedBytes &&
            memory.ObserveMemory().unknownCapacityRegions == 0u, "owned encoded input lost its capacity");
        DecodedData retained;
        for (const auto& source : {memory, EncodedInput::File(file)}) {
            Require(InspectEncodedInput(source).success, "public input inspection failed");
            const auto decoded = DecodePackage({.input = source});
            Require(decoded.success && decoded.output.leaves.size() == 1u, "public decode failed");
            Require(decoded.inputMemory.describedBytes == source.ObserveMemory().describedBytes,
                "decode did not expose its input observation");
            const auto& geometry = decoded.output.leaves.front().geometry;
            Require(geometry.values.size() == sizeof(expected) &&
                std::memcmp(geometry.values.data(), expected.data(), sizeof(expected)) == 0,
                "public result changed geometry");
            PackageDecodeSession session;
            auto opened = session.Open({.decode = {.input = source}});
            Require(opened.success, "public session open failed");
            Require(opened.inputMemory.describedBytes == source.ObserveMemory().describedBytes &&
                session.RequestAttributes({}).inputMemory.describedBytes == opened.inputMemory.describedBytes,
                "explicit session lost the original input observation");
            retained = std::move(opened.output);
            session.Reset();
        }
        memory = {};
        Require(retained.leaves.size() == 1u &&
            std::memcmp(retained.leaves.front().geometry.values.data(), expected.data(), sizeof(expected)) == 0,
            "session close invalidated a delivered result");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
