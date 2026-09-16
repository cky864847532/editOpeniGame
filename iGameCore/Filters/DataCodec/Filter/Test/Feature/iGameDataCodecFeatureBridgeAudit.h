#include "DataCodec/Filter/Adapter/iGameCellTypeMapping.h"
#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#ifndef IGAME_DATACODEC_FEATURE_BRIDGE_AUDIT_H
#define IGAME_DATACODEC_FEATURE_BRIDGE_AUDIT_H

#include "DataCodec/Filter/Test/Data/iGameDataCodecDataGenerator.h"
#include "DataCodec/Filter/Adapter/iGameEncodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameBlockTreeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Adapter/iGameFramePresentationBridge.h"
#include "DataCodec/Filter/Playback/iGameDataCodecStreamingFrameProvider.h"
#include "DataCodec/Filter/Playback/iGameFrameSequenceDecodeBridge.h"
#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "IGDC/iGameIGDCReader.h"
#include "IGDC/iGameIGDCWriter.h"
#include "Codec/iGameWasmDecodedModelRegistry.h"
#include "Codec/iGameWasmDataCodecBridge.h"
#include "DataCodec/API/Entry/InspectEncodedInput.h"
#include <chrono>
#include <fstream>
#include <array>
#include <stdexcept>

namespace datacodec::test {

class ThrowingPlaybackSink final : public IRunRecordSink {
public:
    std::array<std::size_t, 3u> phases{};
    RunRecordMask Interests() const noexcept override { return RunRecordBit(RunRecordKind::Progress); }
    void Submit(const RunRecord& record) override {
        if (const auto* progress = std::get_if<RunProgressRecord>(&record)) {
            ++phases[static_cast<std::size_t>(progress->phase)];
        }
        throw std::runtime_error("injected diagnostic export failure");
    }
};

class RegistryInvalidatingReader final : public IByteRangeReader {
public:
    RegistryInvalidatingReader(std::shared_ptr<const EncodedBuffer> bytes,
        iGame::iGameWasmDecodedModelRegistry& registry) : reader(std::move(bytes)), registry(registry) {}
    std::uint64_t ByteSize() const noexcept override { return reader.ByteSize(); }
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error) override {
        if (!invalidated) { invalidated = true; registry.Clear(); }
        return reader.ReadAt(offset, output, error);
    }
private:
    MemoryByteRangeReader reader;
    iGame::iGameWasmDecodedModelRegistry& registry;
    bool invalidated{false};
};

inline TestResult RunNativeBridgeAudit() {
    TestResult result;
    auto original = iGame::datacodec_test::BuildSyntheticPointSetSmokeObject();
    original->GetAttributeSet()->AddAttribute(IG_SCALAR, IG_POINT,
        iGame::datacodec_test::BuildSyntheticScalarArray("second", 4u, 100.0f));
    {
        std::string error;
        const auto index = std::numeric_limits<std::uint32_t>::max();
        Require(result, iGame::PrepareDataCodecDecodedLeaf(original, {}, index, &error) &&
            iGame::DataCodecFrameIndex(original.get()) == index,
            "native.bridge.frame-identity", "maximum uint32 frame identity was narrowed");
    }
    EncodeRequest request{
        .input = EncodeInput::LeafAdapter(std::make_shared<iGame::iGameEncodeAdapter>(original)),
        .output = EncodeOutput::Memory(),
        .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u},
    };
    request.configuration.pipelineControl.pointOrder = EncodePointOrderMode::Original;
    auto encoded = Encode(request);
    if (!Require(result, encoded.success, "native.bridge.encode", "native fixture encoding failed")) { return result; }
    Require(result, encoded.inputMemory.describedBytes > 0u &&
        encoded.inputMemory.knownCapacityBytes >= encoded.inputMemory.describedBytes &&
        encoded.inputMemory.unknownCapacityRegions == 0u,
        "native.input-capacity", "native input arrays did not report their actual capacities");
    auto bytes = std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes));
    {
        iGame::iGameWasmDecodedModelRegistry registry;
        auto reader = std::make_shared<RegistryInvalidatingReader>(bytes, registry);
        registry.Store(4u, {.deferredInput = EncodedInputAccess::Retain(reader)});
        std::string error;
        std::optional<CodecFailureRecord> failure;
        Require(result, !registry.RestoreRawData(4u, request.resources, &error, &failure) &&
            failure && failure->cancelled && registry.ModelIds().empty(),
            "wasm.restore-invalidated", "restoration published into a registry changed during input IO");
    }
    for (const auto code : {CodecErrorCode::IncompleteInput, CodecErrorCode::UnsupportedVersion, CodecErrorCode::InvalidFormat}) {
        auto invalid = std::make_shared<std::vector<std::uint8_t>>(bytes->span().begin(), bytes->span().end());
        if (code == CodecErrorCode::IncompleteInput) { invalid->resize(2u); }
        else if (code == CodecErrorCode::UnsupportedVersion) { (*invalid)[4] ^= 0x7fu; }
        else { (*invalid)[0] ^= 0x7fu; }
        const auto source = EncodedInput::Memory(invalid);
        const auto decoded = iGame::DecodeiGameWasmDataCodec({.input = source, .resources = request.resources});
        Require(result, !decoded.success && decoded.decodeResult.failure && decoded.decodeResult.failure->code == code,
            "wasm.error-code", "Wasm decode discarded the precise header failure");
        iGame::iGameWasmDecodedModelRegistry registry;
        registry.Store(3u, {.deferredInput = source});
        std::optional<CodecFailureRecord> failure;
        std::string error;
        Require(result, !registry.RestoreRawData(3u, request.resources, &error, &failure) && failure && failure->code == code,
            "wasm.restore-error-code", "raw-data restoration discarded the precise header failure");
        if (code == CodecErrorCode::UnsupportedVersion) {
            Require(result, failure && failure->actualVersion && failure->supportedVersion,
                "wasm.version-detail", "Wasm restoration discarded version metadata");
        }
        PlaybackSession playback;
        Require(result, !playback.Open({.input = source}, &error, &failure) && failure && failure->code == code,
            "playback.open-error", "playback open discarded the precise header failure");
        const auto sequence = iGame::DecodeFrameSequence({
            .decodeSources = {{.frameIndex = 0u, .input = source, .sourceIdentity = {.stableId = "invalid", .revision = "1"}}},
            .selectedFrameOrder = {0u}, .resources = request.resources});
        Require(result, !sequence.success && sequence.failure && sequence.failure->code == code,
            "native.sequence-error", "frame sequence bridge discarded the precise header failure");
    }
    {
        iGame::iGameWasmDecodedModelRegistry registry;
        const auto source = EncodedInput::Memory(bytes);
        const auto identity = InspectEncodedInput(source).sourceIdentity;
        registry.Store(1u, {.sourceIdentity = identity, .deferredInput = source});
        Require(result, registry.FindBySource(identity) == 1 && registry.FindBySource(identity, true) == 0,
            "wasm.surface-capability", "surface-only hit incorrectly advertises original data");
        std::string error;
        auto restored = registry.RestoreRawData(1u, request.resources, &error);
        Require(result, restored && registry.FindBySource(identity, true) == 1 &&
            !registry.Find(1u)->deferredInput,
            "wasm.restore-original", error);
        Require(result, registry.RestoreRawData(1u, request.resources, &error) == restored,
            "wasm.restore-reuse", "restoring an open original session decoded another object");
        auto wrongIdentity = identity;
        wrongIdentity.stableId += "-mismatch";
        registry.Store(2u, {.sourceIdentity = wrongIdentity, .deferredInput = source});
        Require(result, !registry.RestoreRawData(2u, request.resources, &error) &&
            !registry.Find(2u)->codec && registry.Find(2u)->deferredInput,
            "wasm.restore-identity", "restoration accepted an unrelated source or discarded retry input");
        registry.Clear();
        Require(result, restored && iGame::DynamicCast<iGame::PointSet>(restored)->GetNumberOfPoints() == 4u,
            "wasm.restore-lifetime", "registry close invalidated delivered original data");
    }
    {
        auto decoded = DecodePackage({.input = EncodedInput::Memory(bytes),
            .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
            .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u}});
        if (!Require(result, decoded.success && decoded.output.leaves.size() == 1u,
            "native.owned.decode", decoded.failure ? FormatCodecFailure(*decoded.failure) : "missing owned leaf")) { return result; }
        auto& leaf = decoded.output.leaves.front();
        const auto* geometryAddress = leaf.geometry.values.data();
        const auto* attributeAddress = leaf.attributes.front().values.data();
        std::weak_ptr<const void> lifetime = leaf.geometry.values.Owner();
        iGame::iGameDecodeAdapter consumer;
        std::string error;
        Require(result, consumer.Import(leaf, false, &error), "native.owned.import", error);
        auto native = iGame::DynamicCast<iGame::PointSet>(consumer.TakeDataObject());
        Require(result, native && reinterpret_cast<const std::uint8_t*>(native->GetPoints()->RawPointer()) == geometryAddress,
            "native.owned.points", "native points copied the decoded array");
        auto attribute = native->GetAttributeSet()->GetAllAttributes()->GetElement(0).pointer;
        auto values = iGame::DynamicCast<iGame::FloatArray>(attribute);
        Require(result, values && reinterpret_cast<const std::uint8_t*>(values->RawPointer()) == attributeAddress,
            "native.owned.attribute", "native attribute copied the decoded array");
        decoded = {};
        Require(result, !lifetime.expired() && native->GetPoints()->GetNumberOfPoints() == 4u,
            "native.owned.lifetime", "native object lost transferred geometry");
        native->GetPoints()->AddPoint(3.0f, 4.0f, 5.0f);
        Require(result, lifetime.expired() && native->GetPoints()->GetNumberOfPoints() == 5u,
            "native.owned.growth", "native growth retained the old allocation or lost elements");
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("igame-bridge-audit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto path = directory / "leaf.igc";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    }
    for (const bool memory : {true, false}) {
        auto reader = iGame::IGDCReader::New();
        reader->SetLoadAllAvailableAttributes(false);
        reader->SetResourceParams(request.resources);
        if (memory) { reader->SetMemoryInput(bytes, bytes->span()); }
        else { reader->SetFilePath(path.string()); }
        if (!Require(result, reader->Execute(), "native.bridge.reader", "ordinary reader failed")) { continue; }
        auto source = reader->GetAttributeDataSource();
        reader = nullptr;
        if (!Require(result, source != nullptr && source->Attributes().size() == 2u,
            "native.bridge.on-demand", "ordinary reader did not retain its attribute session")) { continue; }
        auto root = source->RootObject();
        const auto catalog = source->Attributes();
        for (const auto index : {1u, 0u}) {
            const auto& target = catalog[index].target;
            const auto prepared = source->PrepareAttribute(target);
            const auto committed = source->CommitAttribute(target);
            const auto metadata = source->Attribute(target);
            Require(result, prepared.success && committed.success && metadata &&
                metadata->nativeIndex == committed.nativeIndex && committed.nativeIndex == (index == 1u ? 0 : 1),
                "native.bridge.attribute-index", "on-demand index does not identify the committed object array");
        }
        source.reset();
        Require(result, root && root->GetAttributeSet()->GetNumberOfAttributes() == 2u,
            "native.bridge.session-close", "closing the attribute source invalidated delivered arrays");
    }
    {
        auto writer = iGame::IGDCWriter::New();
        writer->SetResourceParams(request.resources);
        iGame::FileWriter* base = writer.get();
        Require(result, base->WriteToFile(original, path.string()), "native.bridge.writer", "native writer failed");
        const auto before = std::filesystem::file_size(path);
        Require(result, !base->GenerateBuffers() && !base->SaveBufferDataToFile() &&
            !base->SaveBufferDataToFileWithWindows() && !base->SaveBufferDataToFileWithLinux() &&
            !base->SaveBufferDataToFileWithMac() && std::filesystem::file_size(path) == before && before != 0u,
            "native.bridge.writer-base", "buffer API truncated a completed IGDC output");
    }

    request.input = EncodeInput::BlockTreeAdapter(std::make_shared<iGame::iGameBlockTreeAdapter>(original));
    auto frameBytes = Encode(request);
    if (!Require(result, frameBytes.success, "native.bridge.frame-encode", "frame fixture encoding failed")) { return result; }
    auto session = std::make_shared<PlaybackSession>();
    std::string error;
    if (!Require(result, session->Open({
        .input = ::datacodec::EncodedInputAccess::Retain(std::make_shared<MemoryByteRangeReader>(std::move(frameBytes.encodedBytes))),
        .sourceIdentity = {.stableId = "bridge-audit-frame", .revision = "1"},
        .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
        .resources = request.resources,
        .decodedFrameCachePolicy = {.enabled = false, .prefetchEnabled = false},
        .loadAllAvailableAttributes = false,
    }, &error), "native.bridge.playback-open", error)) { return result; }
    auto throwing = std::make_shared<ThrowingPlaybackSink>();
    const auto decoded = session->RequestFrame({.frameIndex = 0u, .runRecordSink = throwing});
    Require(result, decoded.success && throwing->DiagnosticsIncomplete() &&
        throwing->phases[static_cast<std::size_t>(RunProgressPhase::Begin)] != 0u &&
        throwing->phases[static_cast<std::size_t>(RunProgressPhase::Finish)] != 0u,
        "native.bridge.throwing-sink", "throwing progress sink changed playback completion");
    const auto failed = session->RequestFrame({.frameIndex = 99u, .runRecordSink = throwing});
    Require(result, !failed.success, "native.bridge.throwing-failure", "unknown frame unexpectedly succeeded");
    auto provider = std::make_shared<iGame::DataCodecStreamingFrameProvider>(session, std::vector<std::uint32_t>{0u}, false);
    iGame::DataObject::Pointer retained;
    for (const bool reverse : {true, false}) {
        const auto objects = provider->RequestFrame(0u);
        if (!Require(result, objects.size() == 1u, "native.bridge.frame-request", "frame request failed")) { continue; }
        auto source = provider->AttributeSourceForFrame(objects.front());
        if (!Require(result, source && source->Attributes().size() == 2u,
            "native.bridge.cache-disabled-source", "cache-disabled playback lost its attribute access")) { continue; }
        retained = source->RootObject();
        const auto catalog = source->Attributes();
        for (std::size_t order = 0u; order < 2u; ++order) {
            const auto& target = catalog[reverse ? 1u - order : order].target;
            const auto prepared = source->PrepareAttribute(target);
            const auto committed = source->CommitAttribute(target);
            const auto metadata = source->Attribute(target);
            Require(result, prepared.success && committed.success && metadata &&
                metadata->nativeIndex == static_cast<int>(order),
                "native.bridge.recreated-index", "attribute identity leaked from an earlier frame object");
        }
        Require(result, provider->CachedFrameCount() == 0u && !provider->CacheEnabled(),
            "native.bridge.cache-disabled", "disabled cache retained full frames");
    }
    provider->ConfigureCacheCapacity(1u);
    Require(result, provider->CacheEnabled() && !provider->SupportsCacheCountLimit(),
        "native.bridge.cache-enable", "provider cache controls disagree with the core");
    provider->ClearCachedFrames();
    session->Reset();
    Require(result, retained != nullptr, "native.bridge.playback-close", "delivered frame disappeared after close");
    return result;
}

} // 命名空间 datacodec::test
#endif
