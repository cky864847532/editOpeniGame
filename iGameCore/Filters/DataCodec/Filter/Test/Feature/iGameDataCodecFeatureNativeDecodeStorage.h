#ifndef IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H
#define IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H

#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Runtime/Cache/DecodedCacheCommit.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <array>
#include <cstring>

namespace datacodec::test {
inline TestResult RunDataCodecFeatureNativeDecodeStorage() {
    TestResult result;
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
