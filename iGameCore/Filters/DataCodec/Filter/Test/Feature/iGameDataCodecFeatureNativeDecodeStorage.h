#ifndef IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H
#define IGAME_DATACODEC_FEATURE_NATIVE_DECODE_STORAGE_H

#include "DataCodec/Filter/Adapter/iGameDecodeAdapter.h"
#include "DataCodec/Runtime/Cache/DecodedCacheCommit.h"
#include "DataCodec/Workflow/Session/CodecRunEntry.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <array>
#include <cstring>
#include <thread>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureNativeDecodeStorage() {
    TestResult result;
    constexpr std::uint64_t MiB = 1024u * 1024u;
    const auto bytes = [](const auto& values) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values));
    };
    for (const bool offsets : {false, true}) {
        iGame::DataObject::Pointer retained;
        std::weak_ptr<resource::ResidentByteBudget> budget;
        const std::array<float, 9u> points{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
        const std::array<IndexType, 3u> ids{0u, 1u, 2u};
        const std::array<IndexType, 2u> starts{0u, 3u};
        const std::array<IndexType, 1u> types{static_cast<IndexType>(iGame::IG_TRIANGLE)};
        const std::array<double, 3u> values{3.25, -7.5, 9007199254740994.0};
        {
            DataCodecExecutionResources root(CodecResourceParams{.mode = CodecResourceMode::Fixed,
                .maxComputeThreads = 1u, .ownedStorageLimitBytes = MiB});
            CodecRunScope scope(root);
            budget = root.StorageCapacity();
            CacheResources runtime;
            runtime.BindRun(root);
            bytestore::ByteStoreSession stores;
            stores.BindRun(root);
            DecodedLeafBuilder builder;
            builder.SetMeshType(MeshType::UnstructuredMesh, nullptr);
            DecodedGeometryCache geometry;
            DecodedTopologyCache topology;
            DecodedAttributeCacheSet attributes;
            CodecStorageParams params;
            AttrStorageParams meta;
            meta.name = "owned-double";
            meta.type = AttrRole::Scalar;
            meta.attachmentType = AttrAttachment::Point;
            meta.dataType = DataType::Float64;
            meta.dimension = 1;
            meta.elementCount = values.size();
            params.attrParams.push_back(meta);
            std::string error;
            const bool ready = scope && geometry.Initialize(3u, 3u, DataType::Float32, stores, &error) &&
                topology.InitializeConnectivity(1u, 3u, offsets, true, false, stores, &error) &&
                attributes.Initialize(params, stores) && attributes.BeginAttribute(0u, meta, &error);
            if (!Require(result, ready, "native.owned.prepare", error)) { continue; }
            bool ok = geometry.bytes->WriteBytesAt(0u, bytes(points), &error) && geometry.bytes->Seal(&error) &&
                topology.connectivity->WriteBytesAt(0u, bytes(ids), &error) && topology.connectivity->Seal(&error) &&
                topology.cellTypes->WriteBytesAt(0u, bytes(types), &error) && topology.cellTypes->Seal(&error) &&
                (!offsets || topology.offsets->WriteBytesAt(0u, bytes(starts), &error)) && topology.offsets->Seal(&error) &&
                attributes.WriteAttributeRange(0u, 0u, values.size(), values.data(), sizeof(values), &error) &&
                attributes.EndAttribute(0u, &error);
            geometry.complete = topology.complete = true;
            const auto* pointsAddress = geometry.bytes->ContiguousBytes().data();
            const auto* idsAddress = topology.connectivity->ContiguousBytes().data();
            const auto* valuesAddress = attributes.Bytes(0u)->ContiguousBytes().data();
            const std::array<std::size_t, 1u> indices{0u};
            bool foreignThreadRejected = false;
            std::jthread foreign([&] {
                std::string failure;
                foreignThreadRejected = !CommitAttributeCacheFields(builder, runtime, attributes, indices, &failure);
            });
            foreign.join();
            Require(result, foreignThreadRejected, "native.owned.driver", "attribute publication accepted a non-driver thread");
            ok = ok && CommitGeometryCache(builder, runtime, geometry, &error) &&
                CommitTopologyCache(builder, runtime, topology, &error) &&
                CommitAttributeCacheFields(builder, runtime, attributes, indices, &error);
            iGame::iGameDecodeAdapter consumer;
            ok = ok && consumer.Import(builder.output, false, &error);
            if (!Require(result, ok, "native.owned.publish", error)) { continue; }
            retained = consumer.TakeDataObject();
            auto mesh = iGame::DynamicCast<iGame::UnstructuredMesh>(retained);
            auto attribute = iGame::DynamicCast<iGame::DoubleArray>(mesh->GetAttributeSet()->GetAllAttributes()->GetElement(0).pointer);
            Require(result, reinterpret_cast<const std::uint8_t*>(mesh->GetPoints()->RawPointer()) == pointsAddress &&
                reinterpret_cast<const std::uint8_t*>(mesh->GetCells()->GetCellIdArray()->RawPointer()) == idsAddress &&
                reinterpret_cast<const std::uint8_t*>(attribute->RawPointer()) == valuesAddress &&
                std::memcmp(attribute->RawPointer(), values.data(), sizeof(values)) == 0,
                "native.owned.zero-copy", "native geometry, connectivity or attributes copied their allocation");
            Require(result, !topology.connectivity->WriteBytesAt(0u, bytes(ids), &error),
                "native.owned.sealed-reference", "published reference remained writable through the codec store");
            root.RequestStop();
            Require(result, !CommitTopologyCache(builder, runtime, topology, &error),
                "native.owned.cancel", "cancelled output was published");
        }
        auto mesh = iGame::DynamicCast<iGame::UnstructuredMesh>(retained);
        Require(result, budget.expired() && mesh && std::memcmp(mesh->GetPoints()->RawPointer(), points.data(), sizeof(points)) == 0,
            "native.owned.after-run", "native result retained execution state or lost its geometry");
    }
    return result;
}
} // 命名空间 datacodec::test
#endif