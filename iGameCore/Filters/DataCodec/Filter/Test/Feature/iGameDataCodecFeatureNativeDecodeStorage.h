#ifndef IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H
#define IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H

#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Filter/Adapter/iGameEncodeAdapter.h"
#include "DataCodec/Runtime/Cache/DecodedCacheCommit.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <array>
#include <cstring>
#include <thread>

namespace datacodec::test {
// 用真实宿主适配器核验直接发布、混合回放和取消，逐值检查输出
inline void TestNativeAttributeCommit(TestResult& result) {
    using namespace datacodec;
    using namespace std::chrono_literals;
    constexpr std::uint64_t MiB = 1024u * 1024u;
    const std::array<double, 2u> values{3.25, -7.5};
    for (const auto scenario : {0, 1, 2, 3}) {
        const bool mixed = scenario == 1, cancelled = scenario == 2, incomplete = scenario == 3;
        RuntimeResourceLimits limits{MiB, 1u, 2u};
        DataCodecExecutionResources root(ResolvedResourceConfiguration{limits, MiB, 1u, true, true});
        CodecRunScope scope(root);
        CacheResources runtime;
        runtime.BindRun(root);
        bytestore::ByteStoreSession stores;
        stores.BindStorage(root.StorageCapacity(), false);
        CodecStorageParams params;
        params.attrParams.resize(mixed ? 2u : 1u);
        for (std::size_t i = 0u; i < params.attrParams.size(); ++i) {
            auto& meta = params.attrParams[i];
            meta.name = "commit-check-" + std::to_string(i);
            meta.type = AttrRole::Scalar;
            meta.attachmentType = AttrAttachment::Point;
            meta.dataType = DataType::Float64;
            meta.dimension = 1;
            meta.elementCount = values.size();
        }
        iGame::iGameDecodeAdapter adapter;
        DecodedAttributeCacheSet attributes;
        if (!scope || !adapter.SetMeshType(MeshType::PointSet) || !attributes.Initialize(params, stores)) { Require(result, false, "native.attribute.prepare", "attribute commit fixture setup failed"); return; }
        auto native = adapter.CreateAttributeDecodeStore(0u, params.attrParams[0]);
        if (!native || !attributes.BindAttributeStore(0u, native, true)) { Require(result, false, "native.attribute.prepare", "attribute commit fixture setup failed"); return; }
        std::vector<std::size_t> indices;
        for (std::size_t i = 0u; i < params.attrParams.size(); ++i) {
            if (!attributes.BeginAttribute(i, params.attrParams[i]) ||
                !attributes.WriteAttributeRange(i, 0u, values.size(), values.data(), sizeof(values)) ||
                (!incomplete && !attributes.EndAttribute(i))) { Require(result, false, "native.attribute.prepare", "attribute commit fixture setup failed"); return; }
            indices.push_back(i);
        }
        // 直接发布仍需由活动请求的 driver 调用
        if (scenario == 0) {
            bool rejected = false;
            std::jthread other([&] {
                std::string error;
                rejected = !CommitAttributeCacheFields(adapter, runtime, attributes, indices, &error) && !error.empty();
            });
            other.join();
            Require(result, rejected, "native.attribute_publish_requires_driver", "attribute publication contract failed");
        }
        if (!root.UpdateLimits(limits, false, ResourceDecisionReason::MechanismCheck)) { Require(result, false, "native.attribute.prepare", "attribute commit fixture setup failed"); return; }
        if (cancelled) { root.RequestStop(); }
        bool observedReplayWait = false;
        // 看门线程限定等待时间，混合路径在观察到等待后开门
        std::jthread reopen([&](std::stop_token stop) {
            const auto deadline = ResourceClock::now() + 500ms;
            while (!stop.stop_requested() && ResourceClock::now() < deadline) {
                ResourceDebugSnapshot s;
                if (mixed && root.TryCopyResourceDebugSnapshot(s) && !s.gateOpen &&
                    s.waiting == ResourceWaitReason::PressureRecovery && !s.heavyPhaseAdmitted) {
                    observedReplayWait = true;
                    root.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck);
                    return;
                }
                std::this_thread::sleep_for(1ms);
            }
            if (!stop.stop_requested()) { root.UpdateLimits(limits, true, ResourceDecisionReason::MechanismCheck); }
        });
        std::string error;
        const bool committed = CommitAttributeCacheFields(adapter, runtime, attributes, indices, &error);
        ResourceDebugSnapshot after;
        const bool closed = root.TryCopyResourceDebugSnapshot(after) && !after.gateOpen;
        reopen.request_stop();
        reopen.join();
        auto object = adapter.TakeDataObject();
        iGame::iGameEncodeAdapter output(object);
        const auto expectedCount = cancelled || incomplete ? 0u : params.attrParams.size();
        bool contents = output.GetNumberOfPointAttrs() == expectedCount;
        for (std::size_t i = 0u; contents && i < expectedCount; ++i) {
            const auto& attribute = output.GetPointAttr(i);
            for (std::size_t j = 0u; j < values.size(); ++j) {
                double value = 0.0;
                attribute.GetTuple(j, &value);
                contents &= value == values[j];
            }
        }
        attributes.Reset();
        native.reset();
        stores.ReleaseAll();
        const bool finished = scope.Finish(committed);
        if (scenario == 0) { Require(result, committed && closed && contents && finished, "native.attribute_publish_completes_with_closed_gate", "attribute publication contract failed"); }
        if (mixed) { Require(result, committed && observedReplayWait && contents && finished, "native.mixed_attribute_replay_waits_and_preserves_values", "attribute publication contract failed"); }
        if (cancelled) { Require(result, !committed && closed && contents && !finished, "native.cancel_prevents_attribute_publish", "attribute publication contract failed"); }
        if (incomplete) { Require(result, !committed && closed && contents && !error.empty() && !finished, "native.incomplete_attribute_is_not_published", "attribute publication contract failed"); }
    }
}

inline TestResult RunDataCodecFeatureNativeDecodeStorage() {
    TestResult result;
    TestNativeAttributeCommit(result);
    constexpr std::uint64_t MiB = 1024u * 1024u;
    const auto bytes = [](const auto& values) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values));
    };
    for (bool offsets : {false, true}) {
        DataCodecExecutionResources root(CodecResourceParams{.mode = CodecResourceMode::Fixed,
            .maxComputeThreads = 1u, .ownedStorageLimitBytes = MiB});
        CodecRunScope scope(root);
        CacheResources runtime;
        runtime.BindRun(root);
        bytestore::ByteStoreSession stores;
        stores.BindRun(root);
        iGame::iGameDecodeAdapter adapter;
        DecodedGeometryCache geometry;
        DecodedTopologyCache topology;
        std::string error;
        const std::array<float, 9u> points{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
        const std::array<IndexType, 3u> ids{0u, 1u, 2u};
        const std::array<IndexType, 2u> starts{0u, 3u};
        const std::array<IndexType, 1u> types{static_cast<IndexType>(iGame::IG_TRIANGLE)};
        bool ok = scope && adapter.SetMeshType(MeshType::UnstructuredMesh, &error) &&
            geometry.Initialize(3u, 3u, stores, &error, &adapter) &&
            topology.InitializeConnectivity(1u, 3u, offsets, true, false, stores, &error, &adapter);
        Require(result, ok, "native.prepare", error);
        if (!ok) { continue; }
        auto mesh = iGame::DynamicCast<iGame::UnstructuredMesh>(adapter.TakeDataObject());
        auto* pointPointer = mesh->GetPoints()->RawPointer();
        ok = geometry.bytes->WriteBytesAt(0u, bytes(points), &error) &&
            topology.connectivity->WriteBytesAt(0u, bytes(ids), &error) &&
            topology.cellTypes->WriteBytesAt(0u, bytes(types), &error) &&
            (!offsets || topology.offsets->WriteBytesAt(0u, bytes(starts), &error));
        geometry.complete = topology.complete = true;
        // 保留旧输出身份和存储，检查重置后只能重放到新输出
        auto retained = topology.connectivity;
        auto oldIdentity = adapter.DecodeStorageIdentity();
        root.UpdateLimits({MiB, 1u, 2u}, false, ResourceDecisionReason::MechanismCheck);
        ok = ok && CommitGeometryCache(adapter, runtime, geometry, &error) &&
            CommitConnectivityTopologyCache(adapter, runtime, topology, &error);
        Require(result, ok && root.StorageCapacity()->Snapshot().peakReservedBytes == 0u,
            "native.publish-closed", "native publication consumes no controlled capacity and needs no new admission");
        const auto* nativeIds = mesh->GetCells()->GetCellIdArray()->RawPointer();
        Require(result, pointPointer == mesh->GetPoints()->RawPointer() &&
            std::memcmp(pointPointer, points.data(), sizeof(points)) == 0 &&
            std::memcmp(nativeIds, ids.data(), sizeof(ids)) == 0 &&
            mesh->GetCellTypes()->RawPointer()[0] == types[0],
            "native.values", "native geometry and topology preserve every value and the point backing");
        // 对共享视图写入后宿主同步可见，证明提交没有复制连接数组
        const std::array<IndexType, 1u> first{2u};
        Require(result, retained->WriteBytesAt(0u, bytes(first), &error) && nativeIds[0] == 2u,
            "native.shared-backing", "committed connectivity retains the original storage");
        adapter.Abort();
        std::array<IndexType, 3u> read{};
        Require(result, retained->Read(0u, {reinterpret_cast<std::uint8_t*>(read.data()), sizeof(read)}, &error) &&
            read[0] == 2u, "native.shared-lifetime", "shared storage survives adapter abort");
        Require(result, adapter.SetMeshType(MeshType::UnstructuredMesh, &error) &&
            oldIdentity != adapter.DecodeStorageIdentity(), "native.new-identity", "a new output receives a distinct identity");
        root.UpdateLimits({MiB, 1u, 2u}, true, ResourceDecisionReason::MechanismCheck);
        ok = CommitConnectivityTopologyCache(adapter, runtime, topology, &error);
        mesh = iGame::DynamicCast<iGame::UnstructuredMesh>(adapter.TakeDataObject());
        Require(result, ok && mesh->GetCells()->GetCellIdArray()->RawPointer()[0] == 2u &&
            mesh->GetCells()->GetCellIdArray()->RawPointer() != nativeIds,
            "native.reference-replay", "previous output storage is replayed into the new output");
        DecodedGeometryCache invalid;
        Require(result, !invalid.Initialize(3u, 2u, stores, &error, &adapter) && !invalid.bytes,
            "native.invalid-no-fallback", "unsupported native shape fails without allocating a substitute cache");
        DecodedGeometryCache cancelled;
        ok = cancelled.Initialize(3u, 3u, stores, &error, &adapter);
        cancelled.complete = true;
        root.RequestStop();
        Require(result, ok && !CommitGeometryCache(adapter, runtime, cancelled, &error),
            "native.cancel-publication", "cancelled output is not published");
    }
    {
        DataCodecExecutionResources root(CodecResourceParams{.mode = CodecResourceMode::Adaptive,
            .maxComputeThreads = 1u, .targetAvailableMemoryRatio = 0.0});
        CodecRunScope scope(root);
        const auto boundary = ResourceClock::now();
        const bool refreshed = root.SynchronizeMemoryAfterPreparation();
        ResourceDebugSnapshot snapshot;
        Require(result, refreshed && root.TryCopyResourceDebugSnapshot(snapshot) &&
            snapshot.resourceSample.sampledAt > boundary,
            "native.post-prepare-sample", "host preparation waits for a new system observation");
        root.RequestStop();
        Require(result, !root.SynchronizeMemoryAfterPreparation(),
            "native.cancel-sample-wait", "cancellation ends sample synchronization");
    }
    return result;
}
}
#endif
