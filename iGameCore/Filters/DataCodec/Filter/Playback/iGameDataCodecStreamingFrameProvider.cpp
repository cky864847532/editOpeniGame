#include "DataCodec/Filter/Playback/iGameDataCodecStreamingFrameProvider.h"

#include "DataCodec/Filter/Output/iGameDataCodecOutputBinding.h"
#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Filter/Adapter/iGameDecodedFrameAttributeDataSource.h"
#include "Log/iGameLogger.h"

#include <utility>

IGAME_NAMESPACE_BEGIN

DataCodecStreamingFrameProvider::DataCodecStreamingFrameProvider(
        std::shared_ptr<::datacodec::PlaybackSession> session,
        std::vector<std::uint32_t> playbackFrameOrder,
        const bool enableConsoleLog)
    : m_session(std::move(session)),
      m_playbackFrameOrder(std::move(playbackFrameOrder)),
      m_enableConsoleLog(enableConsoleLog) {}

std::vector<Object::Pointer> DataCodecStreamingFrameProvider::RequestFrame(
        const unsigned int ordinal) {
    if (m_session == nullptr || ordinal >= m_playbackFrameOrder.size()) { return {}; }
    const auto result = m_session->RequestFrame({
            .frameIndex = m_playbackFrameOrder[ordinal],
            .progressFrameOrdinal = ordinal,
            .progressFrameCount = static_cast<std::uint32_t>(m_playbackFrameOrder.size()),
            .runRecordSink = MakeiGameDataCodecOutputRecordSink(
                {},
                {},
                true,
                m_enableConsoleLog),
    });
    const auto output = DataObjectFromDecodedFrame(result.frame);
    if (result.success && output != nullptr) {
        auto source = std::make_shared<DecodedFrameAttributeDataSource>(m_session->CreateAttributeAccess(result.frame), output);
        {
            std::lock_guard lock(m_attributeMutex);
            m_currentAttributeSource = std::move(source);
        }
        return {output};
    }
    for (const auto& message : result.messages) {
        IGAME_CORE_ERROR(
                "DataCodec frame {} decode failed [{}]: {}",
                m_playbackFrameOrder[ordinal],
                message.origin,
                message.text);
    }
    if (result.messages.empty()) {
        IGAME_CORE_ERROR(
                "DataCodec frame {} decode failed without diagnostic messages",
                m_playbackFrameOrder[ordinal]);
    }
    return {};
}

void DataCodecStreamingFrameProvider::NotifyFramePresented(const unsigned int ordinal) {
    if (m_session == nullptr || ordinal >= m_playbackFrameOrder.size()) { return; }
    m_currentOrdinal.store(ordinal);
    m_session->NotifyFramePresented(m_playbackFrameOrder[ordinal]);
}

void DataCodecStreamingFrameProvider::ConfigureCacheCapacity(
        const unsigned int bufferedFrameCount) {
    if (m_session == nullptr) { return; }
    auto policy = m_session->GetDecodedFrameCachePolicy();
    policy.enabled = bufferedFrameCount != 0u;
    m_session->ConfigureDecodedFrameCachePolicy(policy);
}

void DataCodecStreamingFrameProvider::ClearCachedFrames() {
    if (m_session != nullptr) { m_session->ClearDecodedFrameCache(); }
}

std::shared_ptr<IAttributeDataSource> DataCodecStreamingFrameProvider::AttributeSourceForFrame(
    const Object::Pointer& object) const {
    std::lock_guard lock(m_attributeMutex);
    if (m_currentAttributeSource == nullptr || m_currentAttributeSource->RootObject().get() != object.get()) { return {}; }
    return std::exchange(m_currentAttributeSource, {});
}

std::size_t DataCodecStreamingFrameProvider::CachedFrameCount() const {
    if (m_session == nullptr) { return 0u; }
    const auto cachedFrames = m_session->CachedDecodedFrameIndices();
    const auto currentOrdinal = m_currentOrdinal.load();
    if (currentOrdinal >= m_playbackFrameOrder.size()) { return cachedFrames.size(); }
    const auto currentFrameIndex = m_playbackFrameOrder[currentOrdinal];
    return cachedFrames.size() - static_cast<std::size_t>(
        std::find(cachedFrames.begin(), cachedFrames.end(), currentFrameIndex) != cachedFrames.end());
}

IGAME_NAMESPACE_END
