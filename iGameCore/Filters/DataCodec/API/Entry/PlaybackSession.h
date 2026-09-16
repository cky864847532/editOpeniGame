#ifndef DATACODEC_API_ENTRY_PLAYBACKSESSION_H
#define DATACODEC_API_ENTRY_PLAYBACKSESSION_H

#include "DataCodec/API/Adapter/ICellTypeMapping.h"
#include "DataCodec/API/Adapter/IDecodedFrameAttributeAccess.h"
#include "DataCodec/API/Adapter/DecodedFrameTypes.h"
#include "DataCodec/API/Adapter/EncodedInputTypes.h"
#include "DataCodec/API/Adapter/IRunRecordSink.h"
#include "DataCodec/API/Input/EncodedInput.h"
#include "DataCodec/API/Params/CodecParamDefaults.h"


#include "DataCodec/API/Input/FrameDecodeSource.h"


#include "DataCodec/API/Params/DataCodecControlParams.h"
#include "DataCodec/API/Params/DecodedFrameCacheParams.h"
#include "DataCodec/API/Params/EncodedInputCacheParams.h"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace datacodec
{

struct PlaybackOpenRequest {
    EncodedInput input;
    DecodeSourceIdentity sourceIdentity;
    std::shared_ptr<const ICellTypeMapping> cellTypeMapping;
    const DecodeControlParams* controlParams{nullptr};
    const DataCodecDecodeConfigurationSource* configurationSource{nullptr};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    // 会话内部创建并持有唯一运行状态
    CodecResourceParams resources;
    DecodedFrameCachePolicy decodedFrameCachePolicy;
    EncodedInputCachePolicy encodedInputCachePolicy;
    // true 表示每帧在同一次 pipeline 中完成全部属性解压
    bool loadAllAvailableAttributes{true};
};

struct PlaybackSequenceOpenRequest {
    // decodeSources包含依赖规划可以读取的全部帧
    std::vector<FrameDecodeSource> decodeSources;
    // playbackFrameOrder只包含调用方选择并准备播放的帧
    std::vector<std::uint32_t> playbackFrameOrder;
    std::shared_ptr<const ICellTypeMapping> cellTypeMapping;
    const DecodeControlParams* controlParams{nullptr};
    const DataCodecDecodeConfigurationSource* configurationSource{nullptr};
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
    // 会话内部创建并持有唯一运行状态
    CodecResourceParams resources;
    DecodedFrameCachePolicy decodedFrameCachePolicy;
    EncodedInputCachePolicy encodedInputCachePolicy;
    // true 表示每帧在同一次 pipeline 中完成全部属性解压
    bool loadAllAvailableAttributes{true};
};

struct PlaybackFrameRequest {
    std::uint32_t frameIndex{0u};
    std::uint32_t progressFrameOrdinal{0u};
    std::uint32_t progressFrameCount{0u};
    std::shared_ptr<IRunRecordSink> runRecordSink;
};

struct PlaybackFrameResult {
    bool success{false};
    std::optional<CodecFailureRecord> failure;
    bool decodedFrameCacheHit{false};
    bool cancelled{false};
    DecodedFrame::Pointer frame;
    std::vector<TelemetryMessageRecord> messages;
};

class PlaybackSession final : public std::enable_shared_from_this<PlaybackSession> {
public:
    PlaybackSession();
    ~PlaybackSession();

    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    [[nodiscard]] bool Open(const PlaybackOpenRequest& request, std::string* error = nullptr,
                            std::optional<CodecFailureRecord>* failure = nullptr);
    [[nodiscard]] bool OpenSequence(const PlaybackSequenceOpenRequest& request,
                                    std::string* error = nullptr,
                                    std::optional<CodecFailureRecord>* failure = nullptr);
    [[nodiscard]] PlaybackFrameResult RequestFrame(const PlaybackFrameRequest& request);
    [[nodiscard]] DecodedFrameAttributeResult RequestDecodedFrameAttributes(
        const DecodedFrame::Pointer& frame,
        const DecodedFrameAttributeRequest& request);
    [[nodiscard]] std::vector<DecodeAttributeDescriptor> AvailableFrameAttributes(
        const DecodedFrame::Pointer& frame) const;
    [[nodiscard]] IDecodedFrameAttributeAccess::Pointer CreateAttributeAccess(
        DecodedFrame::Pointer frame);
    void NotifyFramePresented(std::uint32_t frameIndex);
    void ConfigureDecodedFrameCachePolicy(const DecodedFrameCachePolicy& policy);
    [[nodiscard]] DecodedFrameCachePolicy GetDecodedFrameCachePolicy() const;
    void ConfigureEncodedInputCachePolicy(const EncodedInputCachePolicy& policy);
    [[nodiscard]] EncodedInputCachePolicy GetEncodedInputCachePolicy() const;
    void ClearDecodedFrameCache();
    [[nodiscard]] DecodedFrameCacheLookupResult CachedDecodedFrame(std::uint32_t frameIndex) const;
    [[nodiscard]] std::vector<std::uint32_t> CachedDecodedFrameIndices() const;
    [[nodiscard]] std::vector<std::uint32_t> CachedDecodeReferenceFrameIndices() const;
    [[nodiscard]] bool IsOpen() const;
    void WaitForPrefetch();
    [[nodiscard]] std::vector<TelemetryMessageRecord> TakeBackgroundMessages();
    void Reset();

private:
    [[nodiscard]] DecodedFrameAttributeResult RequestDecodedFrameAttributesInRun(
        const DecodedFrame::Pointer&, const DecodedFrameAttributeRequest&);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // datacodec命名空间

#endif
