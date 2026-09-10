#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREDECODEREFERENCECACHE_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREDECODEREFERENCECACHE_H

#include "DataCodec/Workflow/FrameSequence/FrameSequenceDependencyPlanner.h"
#include "DataCodec/Workflow/Temporal/TemporalBuilder.h"
#include "DataCodec/Runtime/Cache/DecodeReferenceCache.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace datacodec::test::feature_decode_reference_cache
{

inline DecodeSourceIdentity Source(const std::string& revision = "r1") {
    return DecodeSourceIdentity{.stableId = "synthetic-reference-series", .revision = revision};
}

inline DecodeReferenceKey Key(const std::uint32_t frameIndex, const std::string& revision = "r1") {
    return DecodeReferenceKey{.source = Source(revision), .keyFrameIndex = frameIndex};
}

inline DecodeReferenceFrame MakeReferenceFrame(
    const std::uint32_t frameIndex,
    const std::string& topologyKey) {
    auto topology = std::make_shared<DecodedTopologyCache>();
    topology->cellCount = 1u;
    topology->connectivityCount = 4u;
    topology->complete = true;
    DecodeReferenceFrame frame;
    frame.frameIndex = frameIndex;
    frame.complete = true;
    frame.leaves["/leaf"].topology.emplace(
        topologyKey,
        DecodedTopologyReference{.store = std::move(topology)});
    return frame;
}

inline bool TestReferenceCacheUsesActualKeyFrameLRU() {
    DecodeReferenceCache cache;
    cache.Configure(2u);
    cache.Publish(Key(0u), MakeReferenceFrame(0u, "topology-0"));
    cache.Publish(Key(1u), MakeReferenceFrame(1u, "topology-1"));
    {
        const auto touched = cache.Find(Key(0u));
        if (touched == nullptr) { return false; }
    }
    cache.Publish(Key(2u), MakeReferenceFrame(2u, "topology-2"));
    return cache.Find(Key(0u)) != nullptr &&
        cache.Find(Key(1u)) == nullptr &&
        cache.Find(Key(2u)) != nullptr;
}

inline bool TestTrimPreservesReferenceConsumer() {
    DecodeReferenceCache cache;
    cache.Configure(1u);
    cache.Publish(Key(0u), MakeReferenceFrame(0u, "topology"));
    auto consumer = cache.Find(Key(0u));
    std::weak_ptr<DecodeReferenceFrame> lifetime = consumer;
    if (consumer == nullptr || !cache.TrimOne() || cache.TrimOne() ||
        cache.Find(Key(0u)) != nullptr || lifetime.expired() ||
        !consumer->leaves.at("/leaf").topology.at("topology").store->complete) {
        return false;
    }
    consumer.reset();
    cache.Configure(0u);
    cache.Publish(Key(1u), MakeReferenceFrame(1u, "topology"));
    return lifetime.expired() && cache.Find(Key(1u)) == nullptr;
}

inline bool TestRequiredReferencesSurviveRetentionDisabled() {
    DataCodecExecutionResources run(ResolvedResourceConfiguration{
        .initialLimits = {0u, 1u, 1u},
        .storageCeilingBytes = 64u,
        .computeCeiling = 1u,
        .threaded = false,
    });
    CodecRunScope scope(run);
    DecodeReferenceCache cache(&run);
    cache.Configure(1u);
    auto firstDependency = cache.RequireFrame(Key(0u));
    auto secondDependency = cache.RequireFrame(Key(1u));
    auto sharedDependency = cache.RequireFrame(Key(0u));
    auto published = cache.Publish(Key(0u), MakeReferenceFrame(0u, "topology-a"));
    std::weak_ptr<DecodeReferenceFrame> lifetime = published;
    published.reset();
    cache.Publish(Key(1u), MakeReferenceFrame(1u, "topology-b"));
    if (cache.TrimOne() || cache.Statistics().residentFrames != 0u ||
        cache.Find(Key(0u)) == nullptr || cache.Find(Key(1u)) == nullptr) { return false; }
    firstDependency.Reset();
    if (lifetime.expired() || cache.Find(Key(0u)) == nullptr) { return false; }
    sharedDependency.Reset();
    secondDependency.Reset();
    return lifetime.expired() && cache.Find(Key(0u)) == nullptr &&
        cache.Find(Key(1u)) == nullptr && scope.Finish(true);
}

inline bool TestReferenceKindsMergeForOneKeyFrame() {
    DecodeReferenceCache cache;
    cache.Configure(2u);
    cache.Publish(Key(0u), MakeReferenceFrame(0u, "topology-a"));
    cache.Publish(Key(0u), MakeReferenceFrame(0u, "topology-b"));
    const auto frame = cache.Find(Key(0u));
    if (frame == nullptr) { return false; }
    const auto leaf = frame->leaves.find("/leaf");
    return leaf != frame->leaves.end() &&
        leaf->second.topology.contains("topology-a") &&
        leaf->second.topology.contains("topology-b");
}

inline bool TestReferenceRevisionSeparatesEntries() {
    DecodeReferenceCache cache;
    cache.Configure(4u);
    cache.Publish(Key(0u, "r1"), MakeReferenceFrame(0u, "topology"));
    return cache.Find(Key(0u, "r1")) != nullptr && cache.Find(Key(0u, "r2")) == nullptr;
}

inline EncodedBuffer MakeFramePackageBytes(
    const std::uint32_t frameIndex,
    const TemporalFieldRole role,
    const std::uint32_t keyFrameIndex,
    const TopologyOwnershipMode topologyMode,
    const std::uint32_t topologyOwnerFrameIndex) {
    FramePackage package;
    package.frameIndex = frameIndex;
    package.geometryTemporalRole = role;
    package.geometryKeyFrameIndex = keyFrameIndex;
    package.attributeTemporalRole = role;
    package.attributeKeyFrameIndex = keyFrameIndex;
    package.rootName = "decode-reference-cache-test";
    package.leaves.push_back(FramePackageLeafRecord{
        .path = "/leaf",
        .name = "leaf",
        .ownerFrameIndex = topologyOwnerFrameIndex,
        .topologyMode = topologyMode,
    });
    const std::vector<FramePackageIO::LeafPackageWriter> leafWriters(1u);
    DataCodecExecutionResources root(ResolvedResourceConfiguration{
        {1024u * 1024u, 1u, 1u}, 1024u * 1024u, 1u, false, true});
    MemoryByteRangeOutput output(root);
    std::string error;
    if (!FramePackageIO::WriteToSink(package, leafWriters, output, nullptr, &error)) { return {}; }
    return output.TakeBytes();
}

inline FrameSequenceDependencyPlanner::FrameReaderMap MakeDependencyTestReaders() {
    auto frame0 = MakeFramePackageBytes(
        0u, TemporalFieldRole::KeyFrame, 0u, TopologyOwnershipMode::Owned, 0u);
    auto frame1 = MakeFramePackageBytes(
        1u, TemporalFieldRole::PredFrame, 0u, TopologyOwnershipMode::Reused, 0u);
    auto frame2 = MakeFramePackageBytes(
        2u, TemporalFieldRole::PredFrame, 0u, TopologyOwnershipMode::Reused, 0u);
    if (frame0.empty() || frame1.empty() || frame2.empty()) { return {}; }
    FrameSequenceDependencyPlanner::FrameReaderMap readers;
    readers.emplace(0u, std::make_shared<MemoryByteRangeReader>(std::move(frame0)));
    readers.emplace(1u, std::make_shared<MemoryByteRangeReader>(std::move(frame1)));
    readers.emplace(2u, std::make_shared<MemoryByteRangeReader>(std::move(frame2)));
    return readers;
}

inline bool TestDirectReferenceDependencyPlanning() {
    auto readers = MakeDependencyTestReaders();
    if (readers.empty()) { return false; }
    std::string error;
    FrameSequenceDependencyPlanner planner(std::move(readers));
    FrameSequenceDependencyPlan plan;
    return planner.BuildPlan(1u, plan, &error) &&
        plan.decodeOrder == std::vector<std::uint32_t>({0u, 1u}) &&
        plan.referenceFrames == std::vector<std::uint32_t>({0u});
}

inline bool TestKeyFrameAllocationIsDirect() {
    AttrReferenceControlParams attributeParams;
    GeometryReferenceControlParams geometryParams;
    attributeParams.temporalField.keyFrameInterval = 3u;
    geometryParams.temporalField.keyFrameInterval = 3u;
    for (std::uint32_t frameIndex = 0u; frameIndex < 10u; ++frameIndex) {
        const auto expectedKeyFrameIndex = (frameIndex / 3u) * 3u;
        const auto expectedRole = frameIndex == expectedKeyFrameIndex
            ? TemporalFieldRole::KeyFrame
            : TemporalFieldRole::PredFrame;
        const auto attribute = TemporalBuilder::ResolveTemporalFieldState(10u, frameIndex, attributeParams);
        const auto geometry = TemporalBuilder::ResolveTemporalFieldState(10u, frameIndex, geometryParams);
        if (attribute.temporalRole != expectedRole || attribute.keyFrameIndex != expectedKeyFrameIndex ||
            geometry.temporalRole != expectedRole || geometry.keyFrameIndex != expectedKeyFrameIndex) {
            return false;
        }
    }
    return true;
}

} // namespace datacodec::test::feature_decode_reference_cache

namespace datacodec::test
{

inline int RunDataCodecFeatureDecodeReferenceCache() {
    using namespace feature_decode_reference_cache;
    if (!RunNamedCheck("referenceCache.keyframe-lru", TestReferenceCacheUsesActualKeyFrameLRU) ||
        !RunNamedCheck("referenceCache.live-consumer", TestTrimPreservesReferenceConsumer) ||
        !RunNamedCheck("referenceCache.required-retention-disabled", TestRequiredReferencesSurviveRetentionDisabled) ||
        !RunNamedCheck("referenceCache.merged-kinds", TestReferenceKindsMergeForOneKeyFrame) ||
        !RunNamedCheck("referenceCache.revision", TestReferenceRevisionSeparatesEntries) ||
        !RunNamedCheck("referenceCache.direct-dependency", TestDirectReferenceDependencyPlanning) ||
        !RunNamedCheck("referenceCache.direct-keyframe", TestKeyFrameAllocationIsDirect)) {
        std::cerr << "DataCodec decode reference cache feature test failed\n";
        return 1;
    }
    std::cout << "DataCodec decode reference cache feature test passed\n";
    return 0;
}

} // namespace datacodec::test

#endif
