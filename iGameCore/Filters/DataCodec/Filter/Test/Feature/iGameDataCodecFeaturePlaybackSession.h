#include "DataCodec/Filter/Adapter/iGameCellTypeMapping.h"
#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#ifndef iGameDataCodecFeaturePlaybackSession_h
#define iGameDataCodecFeaturePlaybackSession_h

#include <DataCodec/Filter/Test/Data/iGameDataCodecDataGenerator.h>
#include <DataCodec/Filter/Adapter/iGameBlockTreeAdapter.h>
#include <DataCodec/Filter/Adapter/iGameDataCodecAttributeCatalog.h>
#include <DataCodec/Filter/Adapter/iGameFramePresentationBridge.h>
#include <DataCodec/API/Entry/PlaybackSession.h>
#include <DataCodec/Filter/Telemetry/iGameDataCodecTelemetryCapture.h>
#include <DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h>
#include <DataCodec/API/Adapter/DecodedFrameTypes.h>
#include <DataCodec/Storage/FramePackage/FramePackageIO.h>
#include <DataCodec/API/Params/TimeSeriesControlParams.h>
#include <DataCodec/Test/Feature/DataCodecFeatureDecodeReferenceCache.h>
#include <DataCodec/Test/Feature/DataCodecFeatureDecodeTaskCoordinator.h>
#include <DataCodec/Filter/Test/Feature/iGameDataCodecFeatureLocalization.h>
#include <DataCodec/Filter/Test/Feature/iGameDataCodecFeatureProgress.h>
#include <DataCodec/Test/Feature/DataCodecFeatureDecodedFrameCache.h>
#include <DataCodec/Filter/Test/Feature/iGameDataCodecFeatureStreamingFrameCache.h>
#include <DataCodec/Storage/ByteIO/FileByteRangeIO.h>
#include <IGDC/iGameIGDCFrameSequence.h>
#include <IGDC/iGameIGDCReader.h>
#include <IGDC/iGameIGDCWriter.h>
#include <iGameDrawObject.h>
#include <iGameFileIO.h>
#include <iGameStreamingData.h>
#include <iGameStringArray.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

class SyntheticStreamingFrameProvider final : public iGame::IStreamingFrameProvider {
public:
    explicit SyntheticStreamingFrameProvider(std::vector<iGame::DataObject::Pointer> frames)
        : m_frames(std::move(frames)) {}

    [[nodiscard]] std::vector<iGame::Object::Pointer> RequestFrame(
        const unsigned int ordinal) override {
        return ordinal < m_frames.size()
            ? std::vector<iGame::Object::Pointer>{m_frames[ordinal]}
            : std::vector<iGame::Object::Pointer>{};
    }

    void NotifyFramePresented(unsigned int) override {}
    void ClearCachedFrames() override {}
    [[nodiscard]] std::size_t CachedFrameCount() const override { return 0u; }

private:
    std::vector<iGame::DataObject::Pointer> m_frames;
};


struct BlockingPlaybackAssemblyState {
    std::mutex mutex;
    std::condition_variable condition;
    bool started{false};
    bool released{false};
};

class BlockingPlaybackReader final : public ::datacodec::IByteRangeReader {
public:
    BlockingPlaybackReader(std::shared_ptr<::datacodec::IByteRangeReader> reader,
                           std::shared_ptr<BlockingPlaybackAssemblyState> state)
        : m_reader(std::move(reader)), m_state(std::move(state)) {}
    std::uint64_t ByteSize() const noexcept override { return m_reader->ByteSize(); }
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output, std::string* error) override {
        if (armed.load()) {
            std::unique_lock lock(m_state->mutex);
            m_state->started = true;
            m_state->condition.notify_all();
            m_state->condition.wait(lock, [this] { return m_state->released; });
        }
        return m_reader->ReadAt(offset, output, error);
    }
    std::atomic<bool> armed{false};
private:
    std::shared_ptr<::datacodec::IByteRangeReader> m_reader;
    std::shared_ptr<BlockingPlaybackAssemblyState> m_state;
};
bool HasOnlyDrawableSubObjects(const iGame::DataObject::Pointer& object) {
    if (object == nullptr) { return false; }
    if (!object->HasSubDataObject()) { return true; }
    for (auto iterator = object->SubDataObjectIteratorBegin();
         iterator != object->SubDataObjectIteratorEnd(); ++iterator) {
        const auto child = iGame::DynamicCast<iGame::DrawObject>(iterator->second);
        if (child == nullptr || !HasOnlyDrawableSubObjects(child)) { return false; }
    }
    return true;
}

bool HasDirectAttributeNamed(
        const iGame::DataObject::Pointer& object,
        const std::string& name) {
    auto* attributeSet = object != nullptr ? object->GetAttributeSet() : nullptr;
    auto attributes = attributeSet != nullptr ? attributeSet->GetAllAttributes() : nullptr;
    if (attributes == nullptr) { return false; }
    for (int index = 0; index < attributes->GetNumberOfElements(); ++index) {
        const auto& attribute = attributes->GetElement(index);
        if (!attribute.isDeleted && attribute.pointer != nullptr &&
            attribute.pointer->GetName() == name) {
            return true;
        }
    }
    return false;
}

std::size_t DirectAttributeCount(const iGame::DataObject::Pointer& object) {
    auto* attributeSet = object != nullptr ? object->GetAttributeSet() : nullptr;
    auto attributes = attributeSet != nullptr ? attributeSet->GetAllAttributes() : nullptr;
    if (attributes == nullptr) { return 0u; }
    std::size_t count = 0u;
    for (int index = 0; index < attributes->GetNumberOfElements(); ++index) {
        const auto& attribute = attributes->GetElement(index);
        if (!attribute.isDeleted && attribute.pointer != nullptr) { ++count; }
    }
    return count;
}

bool VerifyDrawableHierarchyTraversal(const iGame::DataObject::Pointer& object) {
    auto drawObject = iGame::DynamicCast<iGame::DrawObject>(object);
    if (drawObject == nullptr || !HasOnlyDrawableSubObjects(drawObject)) { return false; }
    drawObject->SetVisibility(false);
    drawObject->SetVisibility(true);
    drawObject->SetShellRenderingOption(false);
    return drawObject->GetVisibility();
}

bool TestTimeSeriesControlPolicy() {
    auto params = ::datacodec::MakeDefaultEncodeControlParams();
    ::datacodec::CompressorConfig compressor;
    params.defaultAttrControl.regionControl.defaultPrecision =
        ::datacodec::MakeNumericArrayRegionPrecision(compressor);
    params.defaultAttrControl.regionControl.regions.push_back(
        ::datacodec::MakeNumericArrayRegionPrecision(compressor));
    params.defaultAttrControl.regionRuns.push_back({.begin = 0u, .count = 1u, .regionId = 1u});
    params.attrControl["A"] = params.defaultAttrControl;
    params.attrReference.temporalField.keyFrameInterval = 0u;
    params.geometryReference.temporalField.keyFrameInterval = 0u;
    params.attrReference.temporalField.forcePredFrames = true;
    params.geometryReference.temporalField.forcePredFrames = true;

    ::datacodec::PrepareTimeSeriesControlParamsForRandomAccess(params);
    return params.defaultAttrControl.regionControl.regions.empty() &&
        params.defaultAttrControl.regionRuns.empty() &&
        params.attrControl["A"].regionControl.regions.empty() &&
        params.attrControl["A"].regionRuns.empty() &&
        params.attrReference.temporalField.keyFrameInterval == 8u &&
        params.geometryReference.temporalField.keyFrameInterval == 8u &&
        !params.attrReference.temporalField.forcePredFrames &&
        !params.geometryReference.temporalField.forcePredFrames;
}

bool TestSingleLeafFrameSequence() {
    constexpr std::uint32_t frameCount = 3u;
    auto source = iGame::DrawObject::New();
    auto timeFrames = iGame::StreamingData::New();
    if (source == nullptr || timeFrames == nullptr) { return false; }

    std::vector<iGame::DataObject::Pointer> frames;
    frames.reserve(frameCount);
    for (std::uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex) {
        auto frame = iGame::datacodec_test::BuildSyntheticSurfaceSmokeObject();
        auto metadata = iGame::StringArray::New();
        if (frame == nullptr || metadata == nullptr) { return false; }
        timeFrames->AddTimeStep(static_cast<float>(frameIndex), metadata, StreamingType::MultiSubFiles);
        frames.push_back(frame);
    }
    timeFrames->SetFrameProvider(std::make_shared<SyntheticStreamingFrameProvider>(frames));
    source->SetName("DataCodecSingleLeafFrameSequenceTest");
    source->AddSubDataObject(frames.front());
    source->SetTimeFrames(timeFrames);

    const auto outputPath = std::filesystem::temp_directory_path() /
        "igame_datacodec_single_leaf_sequence_test.igc";
    auto writer = iGame::IGDCWriter::New();
    std::vector<iGame::DataCodecEncodeAttributeDescriptor> descriptors;
    if (!iGame::CollectDataCodecEncodeRepresentativeAttributeCatalog(source, descriptors)) { return false; }
    std::vector<::datacodec::AttributeTarget> targets;
    for (const auto& descriptor: descriptors) {
        if (descriptor.name == "synthetic_surface_point_scalar") { targets.push_back(descriptor.target); }
    }
    if (targets.empty()) { return false; }
    writer->SetAttributeTargets(targets);
    writer->SetFilePath(outputPath.string());
    iGame::iGameDataCodecTelemetryCapture recordSinks;
    recordSinks.CaptureSessions(
        ::datacodec::kRunLifecycleRecordMask |
        ::datacodec::RunRecordKind::Message);
    writer->SetTelemetrySink(recordSinks.Sink());
    if (!writer->WriteToFile(source, outputPath.string())) {
        for (const auto& message : recordSinks.SnapshotMessages()) {
            std::cerr << message.text << '\n';
        }
        std::cerr << "single-leaf frame sequence encode failed\n";
        return false;
    }

    const auto paths = writer->GetWrittenFilePaths();
    bool valid = paths.size() == frameCount;
    if (!valid) {
        std::cerr << "single-leaf frame sequence wrote " << paths.size()
                  << " files instead of " << frameCount << '\n';
    }
    std::string error;
    for (const auto& path: paths) {
        auto reader = std::make_shared<::datacodec::FileByteRangeReader>(std::filesystem::path(path));
        ::datacodec::FramePackage framePackage;
        const auto metadataRead = ::datacodec::FramePackageIO::ReadMetadata(*reader, framePackage, &error);
        if (!metadataRead || framePackage.leaves.size() != 1u) {
            std::cerr << "single-leaf frame metadata failed for " << path
                      << ": " << error << ", leaves=" << framePackage.leaves.size() << '\n';
            valid = false;
        }
    }
    {
        auto reader = iGame::IGDCReader::New();
        reader->SetFilePath(paths.front());
        reader->SetSelectedFramePaths(paths);
        reader->SetLoadAllAvailableAttributes(false);
        if (!reader->Execute() || reader->GetOutput() == nullptr ||
            reader->GetOutput()->GetNumberOfSubDataObjects() != 0 ||
            HasDirectAttributeNamed(
                    reader->GetOutput(), "synthetic_surface_point_scalar") ||
            reader->GetOutput()->PeekTimeFrames() == nullptr ||
            reader->GetOutput()->PeekTimeFrames()->GetTimeNum() != frameCount ||
            reader->GetAttributeDataSource() == nullptr) {
            std::cerr << "single-leaf on-demand playback initialization failed\n";
            valid = false;
        } else {
            auto source = reader->GetAttributeDataSource();
            const auto descriptors = source->Attributes();
            const auto prepareResult = !descriptors.empty()
                ? source->PrepareAttribute(descriptors.front().target)
                : iGame::AttributeDataLoadResult{};
            const auto commitResult = prepareResult.success
                ? source->CommitAttribute(descriptors.front().target)
                : iGame::AttributeDataLoadResult{};
            if (descriptors.empty() || !prepareResult.success || !commitResult.success ||
                !HasDirectAttributeNamed(
                    reader->GetOutput(), "synthetic_surface_point_scalar")) {
                std::cerr << "single-leaf on-demand first-frame attribute load failed: prepare="
                          << prepareResult.error << ", commit=" << commitResult.error << '\n';
                valid = false;
            }
            const auto lastFrame = reader->GetOutput()->PeekTimeFrames()->GetTargetTimeFrameData(
                    frameCount - 1u);
            const auto lastFrameRoot = !lastFrame.empty()
                ? iGame::DynamicCast<iGame::DataObject>(lastFrame.front())
                : iGame::DataObject::Pointer{};
            const auto presentation = iGame::PrepareDataCodecFramePresentation(
                reader->GetOutput(),
                lastFrameRoot);
            const auto lastFrameHadAttributeBeforeLoad = HasDirectAttributeNamed(
                lastFrameRoot,
                "synthetic_surface_point_scalar");
            auto lastSource = source->ForFrameObject(lastFrameRoot);
            const auto lastDescriptors = lastSource != nullptr
                ? lastSource->Attributes()
                : std::vector<iGame::AttributeDataDescriptor>{};
            const auto lastPrepareResult = !lastDescriptors.empty()
                ? lastSource->PrepareAttribute(lastDescriptors.front().target)
                : iGame::AttributeDataLoadResult{};
            const auto lastCommitResult = lastPrepareResult.success
                ? lastSource->CommitAttribute(lastDescriptors.front().target)
                : iGame::AttributeDataLoadResult{};
            if (lastFrameRoot == nullptr || lastSource == nullptr ||
                lastFrameRoot->GetNumberOfSubDataObjects() != 0 ||
                !presentation.recognized || !presentation.drawableReady ||
                presentation.reusedTopologyLeafCount != 1u ||
                lastFrameHadAttributeBeforeLoad ||
                lastDescriptors.empty() || !lastPrepareResult.success || !lastCommitResult.success ||
                !HasDirectAttributeNamed(lastFrameRoot, "synthetic_surface_point_scalar")) {
                std::cerr << "single-leaf on-demand playback attribute load failed: prepare="
                          << lastPrepareResult.error << ", commit=" << lastCommitResult.error << '\n';
                valid = false;
            }
        }
    }
    std::error_code removeError;
    for (const auto& path: paths) { std::filesystem::remove(path, removeError); }
    return valid;
}

} // 匿名命名空间

namespace iGame::datacodec_test
{
using namespace ::datacodec;
using namespace ::datacodec::test;

inline int RunDataCodecFeaturePlaybackSession(const int argc = 0, char** argv = nullptr) {
    if (!RuniGameDataCodecFeatureStreamingFrameCache()) {
        std::cerr << "iGame streaming frame cache adapter test failed\n";
        return 1;
    }
    if (argc > 1) {
        std::vector<std::string> selectedPaths;
        selectedPaths.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) { selectedPaths.emplace_back(argv[index]); }
        auto reader = iGame::IGDCReader::New();
        reader->SetFilePath(selectedPaths.front());
        reader->SetSelectedFramePaths(selectedPaths);
        if (!reader->Execute() || reader->GetOutput() == nullptr ||
            iGame::DynamicCast<iGame::DrawObject>(reader->GetOutput()) == nullptr) {
            for (const auto& message: reader->GetMessages()) { std::cerr << message.text << '\n'; }
            std::cerr << "real frame sequence decode failed\n";
            return 1;
        }
        const auto timeFrames = reader->GetOutput()->PeekTimeFrames();
        const auto visibleFrameCount = timeFrames != nullptr ? timeFrames->GetTimeNum() : 0u;
        if (visibleFrameCount == 0u || visibleFrameCount != selectedPaths.size()) { return 1; }
        const auto firstMetadata = timeFrames->GetTargetTimeFrame(0u).GetMetaData();
        std::cout << "visibleFrames=" << visibleFrameCount
                  << ", firstFrame="
                  << (firstMetadata != nullptr && firstMetadata->GetNumberOfElements() > 1
                          ? firstMetadata->GetElement(1)
                          : "unknown")
                  << ", rootSubObjects=" << reader->GetOutput()->GetNumberOfSubDataObjects()
                  << ", rootAttributes=" << DirectAttributeCount(reader->GetOutput())
                  << '\n';
        for (unsigned int frameIndex = 0u; frameIndex < visibleFrameCount; ++frameIndex) {
            const auto frameData = timeFrames->GetTargetTimeFrameData(frameIndex);
            if (frameData.empty()) {
                std::cerr << "continuous playback frame " << frameIndex << " decode failed\n";
                return 1;
            }
            timeFrames->NotifyFramePresented(frameIndex);
        }
        std::cout << "continuousFramesDecoded=" << visibleFrameCount << '\n';
        auto fileOutput = iGame::FileIO::ReadFile(selectedPaths.front());
        auto fileDrawObject = iGame::DynamicCast<iGame::DrawObject>(fileOutput);
        if (fileDrawObject == nullptr || DirectAttributeCount(fileOutput) == 0u) {
            std::cerr << "FileIO playback root has no direct attributes\n";
            return 1;
        }
        const auto renderableObject = fileDrawObject->GetRenderableObject();
        std::cout << "fileRootAttributes=" << DirectAttributeCount(fileOutput)
                  << ", renderableAttributes=" << DirectAttributeCount(renderableObject)
                  << '\n';
        fileDrawObject->SetShellRenderingOption(false);
        if (!fileDrawObject->ViewCloudPicture(nullptr, 0, -1, false)) {
            std::cerr << "FileIO playback root rejected its first direct attribute\n";
            return 1;
        }
        return 0;
    }
    if (RunDataCodecFeatureHostLocalization() != 0) { return 1; }
    if (RunDataCodecFeatureProgress() != 0) { return 1; }
    if (::datacodec::test::RunDataCodecFeatureDecodedFrameCache() != 0) { return 1; }
    if (::datacodec::test::RunDataCodecFeatureDecodeReferenceCache() != 0) { return 1; }
    if (::datacodec::test::RunDataCodecFeatureDecodeTaskCoordinator() != 0) { return 1; }
    if (!TestTimeSeriesControlPolicy()) {
        std::cerr << "time-series control policy test failed\n";
        return 1;
    }
    if (!TestSingleLeafFrameSequence()) {
        std::cerr << "single-leaf frame sequence test failed\n";
        return 1;
    }

    constexpr std::uint32_t frameCount = 5u;
    auto source = iGame::DrawObject::New();
    auto timeFrames = iGame::StreamingData::New();
    if (source == nullptr || timeFrames == nullptr) { return 1; }

    std::vector<iGame::DataObject::Pointer> frames;
    frames.reserve(frameCount);
    for (std::uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex) {
        auto frame = BuildSyntheticMultiBlockSmokeObject();
        auto metadata = iGame::StringArray::New();
        if (frame == nullptr || metadata == nullptr) { return 1; }
        timeFrames->AddTimeStep(static_cast<float>(frameIndex), metadata, StreamingType::MultiSubFiles);
        frames.push_back(frame);
    }
    timeFrames->SetFrameProvider(std::make_shared<SyntheticStreamingFrameProvider>(frames));
    source->SetName("DataCodecPlaybackSessionTest");
    source->AddSubDataObject(frames.front());
    source->SetTimeFrames(timeFrames);

    const auto outputPath = std::filesystem::temp_directory_path() / "igame_datacodec_playback_session_test.igc";
    auto writer = iGame::IGDCWriter::New();
    auto encodeParams = ::datacodec::MakeDefaultEncodeControlParams();
    encodeParams.attrReference.enabled = true;
    encodeParams.attrReference.temporalField.keyFrameInterval = 3u;
    encodeParams.geometryReference.temporalField.keyFrameInterval = 3u;
    std::vector<iGame::DataCodecEncodeAttributeDescriptor> representativeDescriptors;
    if (!iGame::CollectDataCodecEncodeRepresentativeAttributeCatalog(source, representativeDescriptors)) {
        return 1;
    }
    std::vector<::datacodec::AttributeTarget> selectedTargets;
    for (const auto& descriptor: representativeDescriptors) {
        if (descriptor.name == "synthetic_surface_point_scalar") {
            selectedTargets.push_back(descriptor.target);
        }
    }
    if (selectedTargets.empty()) { return 1; }
    writer->SetAttributeTargets(selectedTargets);
    auto encodeDefinition = ::datacodec::MakeDefaultEncodeConfigurationParams();
    encodeDefinition.controlParams = std::move(encodeParams);
    writer->SetEncodeControls(encodeDefinition);
    writer->SetFilePath(outputPath.string());
    iGame::iGameDataCodecTelemetryCapture recordSinks;
    recordSinks.CaptureSessions(
        ::datacodec::kRunLifecycleRecordMask |
        ::datacodec::RunRecordKind::Message);
    writer->SetTelemetrySink(recordSinks.Sink());
    if (!writer->WriteToFile(source, outputPath.string())) {
        for (const auto& message : recordSinks.SnapshotMessages()) {
            std::cerr << message.text << '\n';
        }
        std::cerr << "playback fixture encode failed\n";
        return 1;
    }

    const auto writtenPaths = writer->GetWrittenFilePaths();
    if (writtenPaths.size() != frameCount) {
        std::cerr << "frame sequence did not write one IGC per frame\n";
        return 1;
    }
    for (std::uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex) {
        const auto expectedPath = iGame::BuildIGDCFrameSequencePath(outputPath, frameIndex);
        if (std::filesystem::path(writtenPaths[frameIndex]) != expectedPath ||
            !std::filesystem::is_regular_file(expectedPath)) {
            std::cerr << "frame sequence path is invalid\n";
            return 1;
        }
    }

    iGame::IGDCFrameSequence sequence;
    std::string error;
    if (!iGame::ResolveIGDCFrameSequence(writtenPaths.front(), sequence, &error) ||
        sequence.decodeSources.size() != frameCount ||
        sequence.selectedFrameIndices != std::vector<std::uint32_t>({0u})) {
        std::cerr << error << '\n';
        return 1;
    }
    iGame::IGDCFrameSequence selectedSequence;
    if (!iGame::ResolveIGDCFrameSelection(
                {writtenPaths[4], writtenPaths[1]}, selectedSequence, &error) ||
        selectedSequence.entryFrameIndex != 1u ||
        selectedSequence.selectedFrameIndices != std::vector<std::uint32_t>({1u, 4u}) ||
        selectedSequence.decodeSources.size() != frameCount) {
        std::cerr << "explicit frame selection was not sorted independently from dependencies\n";
        return 1;
    }
    for (const auto& frameSource: sequence.decodeSources) {
        ::datacodec::FramePackage framePackage;
        if (!::datacodec::FramePackageIO::ReadMetadata(*::datacodec::EncodedInputAccess::Open(frameSource.input), framePackage, &error) ||
            framePackage.leaves.size() != 2u) {
            std::cerr << "multi-leaf frame package layout is invalid\n";
            return 1;
        }
    }

    ::datacodec::PlaybackSession sparseSession;
    if (!sparseSession.OpenSequence(
                {
                        .decodeSources = selectedSequence.decodeSources,
                        .playbackFrameOrder = selectedSequence.selectedFrameIndices,
                        .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
                        .resources = {.mode = ::datacodec::CodecResourceMode::Fixed},
                        .decodedFrameCachePolicy = ::datacodec::DecodedFrameCachePolicy{
                                .prefetchEnabled = true,
                        },
                },
                &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto sparseFirst = sparseSession.RequestFrame({.frameIndex = 1u});
    if (!sparseFirst.success || iGame::DataObjectFromDecodedFrame(sparseFirst.frame) == nullptr) { return 1; }
    sparseSession.NotifyFramePresented(1u);
    sparseSession.WaitForPrefetch();
    const auto sparseResidents = sparseSession.CachedDecodedFrameIndices();
    if (sparseResidents != std::vector<std::uint32_t>({1u, 4u}) ||
        !sparseSession.CachedDecodedFrame(2u).IsMiss() ||
        !sparseSession.CachedDecodedFrame(3u).IsMiss()) {
        std::cerr << "sparse playback resident frames:";
        for (const auto frameIndex : sparseResidents) { std::cerr << ' ' << frameIndex; }
        std::cerr << '\n';
        const auto diagnosticFrame = sparseSession.RequestFrame({.frameIndex = 4u});
        for (const auto& message : diagnosticFrame.messages) { std::cerr << message.text << '\n'; }
        return 1;
    }
    if (sparseSession.RequestFrame({.frameIndex = 2u}).success) {
        std::cerr << "unselected dependency frame was exposed as a playback frame\n";
        return 1;
    }
    sparseSession.ClearDecodedFrameCache();

    auto blockingState = std::make_shared<BlockingPlaybackAssemblyState>();
    auto blockingSources = sequence.decodeSources;
    std::shared_ptr<BlockingPlaybackReader> blockedInput;
    for (auto& source : blockingSources) {
        if (source.frameIndex != 3u) { continue; }
        blockedInput = std::make_shared<BlockingPlaybackReader>(
            ::datacodec::EncodedInputAccess::Open(source.input), blockingState);
        source.input = ::datacodec::EncodedInputAccess::Retain(blockedInput);
    }
    ::datacodec::PlaybackSession prioritySession;
    if (!prioritySession.OpenSequence({
            .decodeSources = blockingSources,
            .playbackFrameOrder = {0u, 1u, 2u, 3u, 4u},
            .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
            .resources = {.mode = ::datacodec::CodecResourceMode::Fixed},
            .decodedFrameCachePolicy = ::datacodec::DecodedFrameCachePolicy{
                    .prefetchEnabled = true,
            },
    }, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!prioritySession.RequestFrame({.frameIndex = 2u}).success) { return 1; }
    blockedInput->armed = true;
    prioritySession.NotifyFramePresented(2u);
    {
        std::unique_lock<std::mutex> lock(blockingState->mutex);
        if (!blockingState->condition.wait_for(
                    lock,
                    std::chrono::seconds(2),
                    [blockingState]() { return blockingState->started; })) {
            std::cerr << "prefetch did not enter the blocking decode assembly\n";
            blockingState->released = true;
            lock.unlock();
            blockingState->condition.notify_all();
            return 1;
        }
    }
    std::promise<void> userStarted;
    auto userStartedFuture = userStarted.get_future();
    auto currentFrameFuture = std::async(std::launch::async, [&prioritySession, &userStarted]() {
        userStarted.set_value();
        return prioritySession.RequestFrame({.frameIndex = 1u});
    });
    userStartedFuture.wait();
    {
        std::lock_guard<std::mutex> lock(blockingState->mutex);
        blockingState->released = true;
    }
    blockingState->condition.notify_all();
    const auto priorityResult = currentFrameFuture.get();
    prioritySession.WaitForPrefetch();
    if (!priorityResult.success) {
        std::cerr << "pending foreground request did not complete after prefetch\n";
        if (priorityResult.failure) {
            std::cerr << priorityResult.failure->reason.data() << ": "
                      << priorityResult.failure->message.data() << '\n';
        }
        for (const auto& message : priorityResult.messages) { std::cerr << message.text << '\n'; }
        return 1;
    }

    ::datacodec::PlaybackSession session;
    if (!session.OpenSequence(
                {
                        .decodeSources = sequence.decodeSources,
                        .playbackFrameOrder = {0u, 1u, 2u, 3u, 4u},
                        .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
                        .resources = {.mode = ::datacodec::CodecResourceMode::Fixed},
                        .decodedFrameCachePolicy =
                                ::datacodec::DecodedFrameCachePolicy{
                                        .prefetchEnabled = true,
                                },
                },
                &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const auto frame2 = session.RequestFrame({
            .frameIndex = 2u,
    });
    if (!frame2.success || iGame::DataObjectFromDecodedFrame(frame2.frame) == nullptr) {
        for (const auto& message: frame2.messages) { std::cerr << message.text << '\n'; }
        std::cerr << "frame 2 request failed\n";
        return 1;
    }
    const auto references = session.CachedDecodeReferenceFrameIndices();
    if (std::find(references.begin(), references.end(), 0u) == references.end()) {
        std::cerr << "frame 0 reference is not resident\n";
        return 1;
    }

    if (!session.CachedDecodedFrame(3u).IsMiss()) {
        std::cerr << "prefetch started before the current frame was delivered\n";
        return 1;
    }
    session.NotifyFramePresented(2u);
    session.WaitForPrefetch();
    if (!session.CachedDecodedFrame(3u).IsHit()) {
        std::cerr << "frame 3 was not prefetched\n";
        return 1;
    }
    const auto frame3 = session.RequestFrame({
            .frameIndex = 3u,
    });
    if (!frame3.success || !frame3.decodedFrameCacheHit) {
        std::cerr << "frame 3 prefetch cache was not hit\n";
        return 1;
    }

    for (const auto frameIndex: {2u, 1u}) {
        const auto result = session.RequestFrame({
                .frameIndex = frameIndex,
        });
        if (!result.success || iGame::DataObjectFromDecodedFrame(result.frame) == nullptr) { return 1; }
    }
    session.WaitForPrefetch();
    if (session.CachedDecodedFrameIndices().size() > 4u) { return 1; }

    session.ClearDecodedFrameCache();
    session.Reset();
    if (!session.OpenSequence(
                {
                        .decodeSources = sequence.decodeSources,
                        .playbackFrameOrder = {0u, 1u, 2u, 3u, 4u},
                        .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
                        .resources = {.mode = ::datacodec::CodecResourceMode::Fixed},
                        .decodedFrameCachePolicy =
                                ::datacodec::DecodedFrameCachePolicy{
                                        .prefetchEnabled = false,
                                },
                },
                &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto frame4 = session.RequestFrame({
            .frameIndex = 4u,
    });
    const auto frame4References = session.CachedDecodeReferenceFrameIndices();
    if (!frame4.success || iGame::DataObjectFromDecodedFrame(frame4.frame) == nullptr ||
        std::find(frame4References.begin(), frame4References.end(), 3u) == frame4References.end()) {
        std::cerr << "random frame access did not retain the planned key frame\n";
        return 1;
    }
    session.NotifyFramePresented(4u);
    session.WaitForPrefetch();
    if (session.CachedDecodedFrameIndices() != std::vector<std::uint32_t>({4u})) {
        std::cerr << "disabled prefetch still populated the full-frame cache\n";
        return 1;
    }
    const auto decodeReferencesBeforePlaybackClear =
            session.CachedDecodeReferenceFrameIndices();
    session.ClearDecodedFrameCache();
    if (!session.CachedDecodedFrameIndices().empty()) {
        std::cerr << "default playback frame cache was not cleared\n";
        return 1;
    }
    if (decodeReferencesBeforePlaybackClear.empty() ||
        session.CachedDecodeReferenceFrameIndices() !=
                decodeReferencesBeforePlaybackClear) {
        std::cerr << "clearing playback frames also cleared DataCodec decode references\n";
        return 1;
    }

    session.Reset();
    if (!session.OpenSequence({
            .decodeSources = sequence.decodeSources,
            .playbackFrameOrder = {0u, 1u, 2u, 3u, 4u},
            .cellTypeMapping = std::make_shared<iGame::iGameCellTypeMapping>(),
            .resources = {.mode = ::datacodec::CodecResourceMode::Fixed},
        }, &error)) { return 1; }
    auto retained = session.RequestFrame({.frameIndex = 2u});
    if (!retained.success || !retained.frame) { return 1; }
    const auto* retainedData = &retained.frame->Data();
    for (const auto index : {3u, 4u, 1u, 0u}) {
        if (!session.RequestFrame({.frameIndex = index}).success ||
            session.CachedDecodedFrameIndices().size() > 2u) { return 1; }
    }
    session.ClearDecodedFrameCache();
    if (!session.CachedDecodedFrameIndices().empty() || &retained.frame->Data() != retainedData) {
        return 1;
    }

    session.Reset();
    auto reader = iGame::IGDCReader::New();
    reader->SetFilePath(writtenPaths.back());
    if (!reader->Execute() || reader->GetOutput() == nullptr ||
        reader->GetOutput()->PeekTimeFrames() == nullptr ||
        reader->GetOutput()->PeekTimeFrames()->GetTimeNum() != 1u ||
        !VerifyDrawableHierarchyTraversal(reader->GetOutput())) {
        std::cerr << "single selected frame reader did not preserve a one-frame timeline\n";
        return 1;
    }
    auto firstReadTimeFrames = reader->GetOutput()->PeekTimeFrames();
    firstReadTimeFrames->DisableCache();
    reader = nullptr;
    firstReadTimeFrames = nullptr;
    auto reloadedReader = iGame::IGDCReader::New();
    reloadedReader->SetFilePath(writtenPaths.back());
    const auto reloadSucceeded = reloadedReader->Execute();
    if (!reloadSucceeded || reloadedReader->GetOutput() == nullptr) {
        std::cerr << "single-frame reload unexpectedly used the default decoded-frame cache\n";
        return 1;
    }
    iGame::iGameBlockTreeAdapter decodedAdapter(reloadedReader->GetOutput());
    std::size_t selectedNameCount = 0u;
    std::size_t unselectedNameCount = 0u;
    for (const auto& leaf: decodedAdapter.GetLeaves()) {
        auto* attributeSet = leaf.object != nullptr ? leaf.object->GetAttributeSet() : nullptr;
        auto attributes = attributeSet != nullptr ? attributeSet->GetAllAttributes() : nullptr;
        if (attributes == nullptr) { continue; }
        for (int attributeIndex = 0; attributeIndex < attributes->GetNumberOfElements(); ++attributeIndex) {
            const auto& attribute = attributes->GetElement(attributeIndex);
            if (attribute.isDeleted || attribute.pointer == nullptr) { continue; }
            if (attribute.pointer->GetName() == "synthetic_surface_point_scalar") {
                ++selectedNameCount;
            }
            if (attribute.pointer->GetName() == "synthetic_surface_cell_scalar") {
                ++unselectedNameCount;
            }
        }
    }
    if (selectedNameCount != 2u || unselectedNameCount != 0u) {
        std::cerr << "selected ATTR names were not applied consistently across frames\n";
        return 1;
    }

    auto multiReader = iGame::IGDCReader::New();
    multiReader->SetFilePath(writtenPaths.back());
    multiReader->SetSelectedFramePaths({writtenPaths.back(), writtenPaths[1]});
    if (!multiReader->Execute() || multiReader->GetOutput() == nullptr ||
        multiReader->GetOutput()->PeekTimeFrames() == nullptr ||
        multiReader->GetOutput()->PeekTimeFrames()->GetTimeNum() != 2u ||
        multiReader->GetOutput()->PeekTimeFrames()->GetTargetTimeValue(0u) != 1.0f ||
        !VerifyDrawableHierarchyTraversal(multiReader->GetOutput())) {
        std::cerr << "multi-frame reader did not start from the lowest selected frame\n";
        return 1;
    }
    auto selectedTimeFrames = multiReader->GetOutput()->PeekTimeFrames();
    const auto firstSelectedMetadata = selectedTimeFrames->GetTargetTimeFrame(0u).GetMetaData();
    const auto secondSelectedMetadata = selectedTimeFrames->GetTargetTimeFrame(1u).GetMetaData();
    if (firstSelectedMetadata == nullptr || secondSelectedMetadata == nullptr ||
        firstSelectedMetadata->GetElement(1) != "1" ||
        secondSelectedMetadata->GetElement(1) != "4" ||
        selectedTimeFrames->GetTargetTimeFrame(0u).GetISCached() ||
        selectedTimeFrames->GetTargetTimeFrame(1u).GetISCached() ||
        !selectedTimeFrames->CachedFrameIndices().empty() ||
        selectedTimeFrames->GetCurrentCacheCount() == 0u ||
        selectedTimeFrames->GetCurrentCacheCount() > 2u) {
        std::cerr << "IGDCReader must delegate sparse frame retention to the bounded DataCodec cache\n";
        return 1;
    }
    selectedTimeFrames->NotifyFramePresented(0u);
    selectedTimeFrames->EnableCache(2u);
    const auto delegatedFrame = selectedTimeFrames->GetTargetTimeFrameData(1u);
    if (delegatedFrame.empty() ||
        selectedTimeFrames->GetTargetTimeFrame(1u).GetISCached() ||
        !selectedTimeFrames->CachedFrameIndices().empty() ||
        selectedTimeFrames->GetCurrentCacheCount() == 0u) {
        std::cerr << "StreamingData must obtain delegated frames without creating a second owning cache\n";
        return 1;
    }
    selectedTimeFrames->ClearCache();
    if (selectedTimeFrames->GetCurrentCacheCount() != 0u) {
        std::cerr << "StreamingData did not clear its displayed cache count\n";
        return 1;
    }
    const auto replayedFrame = selectedTimeFrames->GetTargetTimeFrameData(1u);
    if (replayedFrame.empty() || replayedFrame == delegatedFrame ||
        !selectedTimeFrames->CachedFrameIndices().empty() || delegatedFrame.empty()) {
        std::cerr << "cache clearing must rebuild the frame and preserve previously delivered objects\n";
        return 1;
    }
    std::error_code removeError;
    for (const auto& path: writtenPaths) { std::filesystem::remove(path, removeError); }
    std::cout << "DataCodec playback session test passed\n";
    return 0;
}

} // datacodec::test命名空间

#endif
