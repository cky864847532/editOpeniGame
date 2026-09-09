#include "DataCodec/Filter/Playback/iGameFrameSequenceDecodeBridge.h"

#include "DataCodec/Filter/Adapter/iGameDecodedFrameAttributeDataSource.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Playback/iGameDataCodecStreamingFrameProvider.h"
#include "DataCodec/Runtime/Record/RunRecordSubmit.h"
#include "iGameDrawObject.h"
#include "iGameStreamingData.h"
#include "iGameStringArray.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

IGAME_NAMESPACE_BEGIN

namespace {

void AddFrameSequenceDecodeMessage(
    FrameSequenceDecodeResult& result,
    ::datacodec::IRunRecordSink* runRecordSink,
    std::string text) {
    ::datacodec::TelemetryMessageRecord message{
        .severity = ::datacodec::TelemetryMessageSeverity::Error,
        .origin = "iGameFrameSequenceDecodeBridge",
        .text = std::move(text),
    };
    ::datacodec::SubmitRunMessage(runRecordSink, message);
    AppendRetainedTelemetryMessage(result.messages, message);
}

} // 匿名命名空间

FrameSequenceDecodeResult DecodeFrameSequence(const FrameSequenceDecodeRequest& request) {
    FrameSequenceDecodeResult result;
    if (request.decodeSources.empty() || request.selectedFrameOrder.empty()) {
        AddFrameSequenceDecodeMessage(
            result,
            request.runRecordSink.get(),
            "frame sequence decode request is empty");
        return result;
    }

    auto timeFrames = StreamingData::New();
    for (const auto frameIndex : request.selectedFrameOrder) {
        const auto source = std::find_if(
            request.decodeSources.begin(),
            request.decodeSources.end(),
            [frameIndex](const ::datacodec::FrameDecodeSource& candidate) {
                return candidate.frameIndex == frameIndex;
            });
        if (source == request.decodeSources.end()) {
            AddFrameSequenceDecodeMessage(
                result,
                request.runRecordSink.get(),
                "selected frame is absent from the decode source list");
            return result;
        }
        auto metadata = StringArray::New();
        metadata->AddElement(request.sourceLabel);
        metadata->AddElement(std::to_string(source->frameIndex));
        timeFrames->AddTimeStep(source->timeValue, metadata, StreamingType::IGCFramePackage);
    }
    // 完整帧由 DataCodec 根缓存留存，宿主只保留当前展示结果
    timeFrames->DisableCache();

    auto playback = std::make_shared<::datacodec::PlaybackSession>();
    std::string openError;
    if (!playback->OpenSequence({
            .decodeSources = request.decodeSources,
            .playbackFrameOrder = request.selectedFrameOrder,
            .assemblyFactory = std::make_shared<iGameFramePackageDecodeAssemblyFactory>(),
            .controlParams = request.controlParams,
            .executionOptions = request.executionOptions,
            .configurationSource = request.configurationSource,
            .language = request.language,
            .resources = request.resources,
            .decodedFrameCachePolicy = request.decodedFrameCachePolicy,
            .encodedInputCachePolicy = request.encodedInputCachePolicy,
            .loadAllAvailableAttributes = request.loadAllAvailableAttributes,
        }, &openError)) {
        AddFrameSequenceDecodeMessage(
            result,
            request.runRecordSink.get(),
            openError.empty() ? "failed to open decoded frame sequence" : openError);
        return result;
    }

    const auto target = std::find(
        request.selectedFrameOrder.begin(),
        request.selectedFrameOrder.end(),
        request.targetFrameIndex);
    if (target == request.selectedFrameOrder.end()) {
        AddFrameSequenceDecodeMessage(
            result,
            request.runRecordSink.get(),
            "requested frame is not part of the selected frame sequence");
        return result;
    }
    const auto ordinal = static_cast<std::uint32_t>(
        std::distance(request.selectedFrameOrder.begin(), target));
    auto decoded = playback->RequestFrame({
        .frameIndex = request.targetFrameIndex,
        .progressFrameOrdinal = ordinal,
        .progressFrameCount = static_cast<std::uint32_t>(request.selectedFrameOrder.size()),
        .runRecordSink = request.runRecordSink,
    });
    result.messages = std::move(decoded.messages);
    const auto output = DataObjectFromDecodedFrame(decoded.frame);
    const auto root = DynamicCast<DrawObject>(output);
    if (!decoded.success || root == nullptr) {
        if (result.messages.empty()) {
            AddFrameSequenceDecodeMessage(
                result,
                request.runRecordSink.get(),
                "decoded frame assembly did not produce a drawable root");
        }
        return result;
    }

    auto attributeAccess = playback->CreateAttributeAccess(decoded.frame);
    if (attributeAccess == nullptr) {
        AddFrameSequenceDecodeMessage(
            result,
            request.runRecordSink.get(),
            "decoded frame attribute access is unavailable");
        return result;
    }
    timeFrames->SetFrameProvider(std::make_shared<DataCodecStreamingFrameProvider>(
        std::move(playback),
        request.selectedFrameOrder,
        request.enableConsoleLog));
    root->SetTimeFrames(timeFrames);
    result.success = true;
    result.output = root;
    result.attributeDataSource = std::make_shared<DecodedFrameAttributeDataSource>(
        std::move(attributeAccess));
    return result;
}

IGAME_NAMESPACE_END
