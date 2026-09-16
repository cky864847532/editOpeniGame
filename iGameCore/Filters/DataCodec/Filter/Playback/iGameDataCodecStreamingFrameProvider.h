#ifndef iGameDataCodecStreamingFrameProvider_h
#define iGameDataCodecStreamingFrameProvider_h

#include "DataCodec/API/Entry/PlaybackSession.h"
#include "iGameStreamingData.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

IGAME_NAMESPACE_BEGIN

class DataCodecStreamingFrameProvider final : public IStreamingFrameProvider {
public:
    DataCodecStreamingFrameProvider(
            std::shared_ptr<::datacodec::PlaybackSession> session,
            std::vector<std::uint32_t> playbackFrameOrder,
            bool enableConsoleLog);

    [[nodiscard]] std::vector<Object::Pointer> RequestFrame(unsigned int ordinal) override;
    [[nodiscard]] std::shared_ptr<IAttributeDataSource> AttributeSourceForFrame(const Object::Pointer& object) const override;
    [[nodiscard]] bool SupportsCacheCountLimit() const noexcept override { return false; }
    [[nodiscard]] bool CacheEnabled() const override { return m_session != nullptr && m_session->GetDecodedFrameCachePolicy().enabled; }
    void NotifyFramePresented(unsigned int ordinal) override;
    void ConfigureCacheCapacity(unsigned int bufferedFrameCount) override;
    void ClearCachedFrames() override;
    [[nodiscard]] std::size_t CachedFrameCount() const override;

private:
    std::shared_ptr<::datacodec::PlaybackSession> m_session;
    std::vector<std::uint32_t> m_playbackFrameOrder;
    bool m_enableConsoleLog{true};
    std::atomic_uint m_currentOrdinal{std::numeric_limits<unsigned int>::max()};
    mutable std::mutex m_attributeMutex;
    mutable std::shared_ptr<IAttributeDataSource> m_currentAttributeSource;
};

IGAME_NAMESPACE_END

#endif
