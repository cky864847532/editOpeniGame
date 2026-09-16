#ifndef iGameDataCodecFeatureStreamingFrameCache_h
#define iGameDataCodecFeatureStreamingFrameCache_h

#include "DataCodec/Filter/Adapter/iGameFramePackageDecodeAssembly.h"
#include "DataCodec/Runtime/Cache/DecodedFrameLruCache.h"
#include "iGameDrawObject.h"
#include "iGameStreamingData.h"
#include "iGameStringArray.h"

#include <memory>
#include <unordered_map>

namespace datacodec::test {

inline auto MakeStreamingCacheTestFrame(std::uint32_t index, iGame::DataObject::Pointer) {
    DecodedData data;
    data.frameIndex = index;
    data.leaves.emplace_back();
    return std::make_shared<DecodedFrame>(std::move(data));
}
[[nodiscard]] inline bool RuniGameDataCodecFeatureStreamingFrameCache() {
    auto streamingData = iGame::StreamingData::New();
    auto metadata0 = iGame::StringArray::New();
    auto metadata1 = iGame::StringArray::New();
    streamingData->AddTimeStep(0.0f, metadata0, StreamingType::IGCFramePackage);
    streamingData->AddTimeStep(1.0f, metadata1, StreamingType::IGCFramePackage);
    streamingData->EnableCache(0u);

    const DecodeSourceIdentity source0{.stableId = "streaming-source-0", .revision = "r1"};
    const DecodeSourceIdentity source1{.stableId = "streaming-source-1", .revision = "r1"};
    DecodedFrameLruCache cache;
    cache.Configure(1u);
    const DecodedFrameKey key0{.source = source0, .frameIndex = 10u};
    const DecodedFrameKey key1{.source = source1, .frameIndex = 20u};
    auto frame0 = MakeStreamingCacheTestFrame(
        10u,
        iGame::DrawObject::New());
    const auto stored0 = cache.Store(key0, frame0, DecodedFrameAccessKind::UserRequest);
    const auto found0 = cache.Find(key0, DecodedFrameAccessKind::UserRequest);
    if (!stored0.IsStored() || !found0.IsHit() || found0.value != frame0) {
        return false;
    }

    auto frame1 = MakeStreamingCacheTestFrame(
        20u,
        iGame::DrawObject::New());
    const auto stored1 = cache.Store(key1, frame1, DecodedFrameAccessKind::Prefetch);
    const auto evicted0 = cache.Find(key0, DecodedFrameAccessKind::UserRequest);
    const auto found1 = cache.Find(key1, DecodedFrameAccessKind::UserRequest);
    if (!stored1.IsStored() || !evicted0.IsMiss() || !found1.IsHit() || found1.value != frame1) {
        return false;
    }
    const auto stats = cache.Statistics();
    if (stats.hits < 2u || stats.misses == 0u || stats.evictions == 0u ||
        stats.residentFrames != 1u) {
        return false;
    }

    const auto* retainedData = &frame1->Data();
    cache.InvalidateSource(source1);
    if (!cache.ResidentFrameIndices(source1).empty() ||
        cache.Statistics().residentFrames != 0u ||
        &frame1->Data() != retainedData) {
        return false;
    }
    const DecodedFrameKey wrongRevision{
        .source = DecodeSourceIdentity{
            .stableId = source0.stableId,
            .revision = "r2",
        },
        .frameIndex = 10u,
    };
    return cache.Find(wrongRevision, DecodedFrameAccessKind::UserRequest).IsMiss();
}

} // namespace datacodec::test

#endif
