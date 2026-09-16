#ifndef DATACODEC_RUNTIME_CACHE_DECODEDCACHECOMMIT_H
#define DATACODEC_RUNTIME_CACHE_DECODEDCACHECOMMIT_H

#include "DataCodec/Workflow/Decode/IDecodeAdapter.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedAttributeCacheSet.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Runtime/Cache/DecodedCacheReplay.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyEmit.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Workflow/Decode/DecodedResultBuilder.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
namespace datacodec {

inline void ReleaseDecodedByteStore(
    std::shared_ptr<bytestore::IRandomAccessByteStore>& bytes) noexcept {
    bytes.reset();
}

inline bool ResolveAttributeCommitShape(
    const AttrStorageParams& meta,
    const std::size_t tupleBytes,
    std::size_t& elementCount,
    std::size_t& totalBytes,
    std::string* error = nullptr) {
    elementCount = 0u;
    totalBytes = 0u;
    if (!TryParamSizeToSizeT(meta.elementCount, elementCount)) {
        return validation::AssignError(error, "decoded attribute element count exceeds local size capacity");
    }
    if (!validation::CheckedMulSizeT(
            elementCount,
            tupleBytes,
            totalBytes,
            "decoded attribute byte count",
            error)) {
        return false;
    }
    return true;
}

inline bool CommitGeometryCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    DecodedGeometryCache& geometry,
    std::string* error = nullptr) {
    if (!geometry.complete || geometry.bytes == nullptr) {
        return true;
    }
    if (auto* result = dynamic_cast<DecodedLeafBuilder*>(&adapter)) {
        if (!runtime.Run().IsDriverThread() || runtime.Run().Stopped()) {
            return validation::AssignError(error, "result publication requires an active driver");
        }
        bytestore::ByteStoreSession stores;
        stores.BindRun(runtime.Run());
        if (!result->AcceptGeometry(geometry, stores, error)) { return false; }
        geometry.Release();
        return true;
    }
    if (const auto identity = geometry.nativeOutputIdentity.lock();
        identity && identity == adapter.DecodeStorageIdentity()) {
        if (!runtime.Run().IsDriverThread() || runtime.Run().Stopped()) {
            return validation::AssignError(error, "native output publication requires an active driver");
        }
        if (!adapter.EndPoints(error)) { return false; }
        geometry.Release();
        return true;
    }
    if (!adapter.BeginPoints(geometry.pointCount, geometry.dimension, geometry.dataType, error)) {
        return false;
    }
    if (!ReplayTypedDecodedCache<std::uint8_t>(
            runtime,
            *geometry.bytes,
            geometry.pointCount,
            geometry.dimension * ScalarTypeSize(ToScalarType(geometry.dataType)),
            [&](const std::size_t offset, const std::size_t count, const std::uint8_t* values) {
                return adapter.WritePointsRange(offset, count, values, error);
            },
            error)) {
        (void)adapter.EndPoints(error);
        return false;
    }
    if (!adapter.EndPoints(error)) {
        return false;
    }
    geometry.Release();
    return true;
}

inline bool CommitConnectivityTopologyCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const DecodedTopologyCache& topology,
    DecodedTopologyCache* releaseTarget,
    std::string* error = nullptr) {
    if (topology.connectivity == nullptr ||
        topology.offsets == nullptr ||
        (topology.hasCellTypes && topology.cellTypes == nullptr) ||
        (topology.hasCellPolynomialOrders && topology.cellPolynomialOrders == nullptr)) {
        return validation::AssignError(error, "decoded topology cache is incomplete");
    }
    if (const auto identity = topology.nativeOutputIdentity.lock();
        identity && identity == adapter.DecodeStorageIdentity()) {
        if (!runtime.Run().IsDriverThread() || runtime.Run().Stopped()) {
            return validation::AssignError(error, "native output publication requires an active driver");
        }
        if (!adapter.EndTopology(error)) { return false; }
        if (releaseTarget) { releaseTarget->Release(); }
        return true;
    }
    if (!adapter.BeginTopology(topology.cellCount, topology.connectivityCount, topology.hasOffsets, error)) {
        return false;
    }
    if (!ReplayTypedDecodedCache<IndexType>(
            runtime,
            *topology.connectivity,
            topology.connectivityCount,
            1u,
            [&](const std::size_t offset, const std::size_t count, const IndexType* values) {
                return adapter.WriteConnectivityRange(offset, values, count, error);
            },
            error)) {
        (void)adapter.EndTopology(error);
        return false;
    }
    if (releaseTarget != nullptr) {
        ReleaseDecodedByteStore(releaseTarget->connectivity);
    }
    if (topology.hasOffsets &&
        !ReplayTypedDecodedCache<IndexType>(
            runtime,
            *topology.offsets,
            topology.cellCount + 1u,
            1u,
            [&](const std::size_t offset, const std::size_t count, const IndexType* values) {
                return adapter.WriteOffsetsRange(offset, values, count, error);
            },
            error)) {
        (void)adapter.EndTopology(error);
        return false;
    }
    if (releaseTarget != nullptr) {
        ReleaseDecodedByteStore(releaseTarget->offsets);
    }
    if (topology.hasCellTypes &&
        !ReplayTypedDecodedCache<IndexType>(
            runtime,
            *topology.cellTypes,
            topology.cellCount,
            1u,
            [&](const std::size_t offset, const std::size_t count, const IndexType* values) {
                return adapter.WriteCellTypesRange(offset, values, count, error);
            },
            error)) {
        (void)adapter.EndTopology(error);
        return false;
    }
    if (releaseTarget != nullptr && topology.hasCellTypes) {
        ReleaseDecodedByteStore(releaseTarget->cellTypes);
    }
    if (topology.hasCellPolynomialOrders &&
        !ReplayTypedDecodedCache<std::uint16_t>(
            runtime,
            *topology.cellPolynomialOrders,
            topology.cellCount,
            1u,
            [&](const std::size_t offset, const std::size_t count, const std::uint16_t* values) {
                return adapter.WriteCellPolynomialOrdersRange(offset, values, count, error);
            },
            error)) {
        (void)adapter.EndTopology(error);
        return false;
    }
    if (releaseTarget != nullptr && topology.hasCellPolynomialOrders) {
        ReleaseDecodedByteStore(releaseTarget->cellPolynomialOrders);
    }
    return adapter.EndTopology(error);
}

inline bool CommitConnectivityTopologyCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const DecodedTopologyCache& topology,
    std::string* error = nullptr) {
    return CommitConnectivityTopologyCache(adapter, runtime, topology, nullptr, error);
}

inline bool CommitConnectivityTopologyCacheAndRelease(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    DecodedTopologyCache& topology,
    std::string* error = nullptr) {
    return CommitConnectivityTopologyCache(adapter, runtime, topology, &topology, error);
}

inline bool CommitPolyhedronTopologyCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const DecodedPolyhedronCache& polyhedron,
    DecodedPolyhedronCache* releaseTarget,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    if (!adapter.SupportsPolyhedronTopology()) {
        return validation::AssignError(error, "decode adapter does not support polyhedron topology");
    }
    polyhedron::PolyhedronTopologyStreamHeader header;
    header.cellCount = polyhedron.cellCount;
    header.faceCount = polyhedron.faceCount;
    header.uniqueVertexIdCount = polyhedron.uniqueVertexIdCount;
    header.localFaceVertexIdCount = polyhedron.localFaceVertexIdCount;

    std::uint64_t batchCount = 0u;
    if (!polyhedron::EmitPolyhedronCacheToAdapter(
            runtime,
            adapter,
            header,
            polyhedron.uniqueVertexCounts,
            polyhedron.cellFaceCounts,
            polyhedron.faceVertexCounts,
            polyhedron.cellUniqueVertexIds,
            polyhedron.localFaceVertexIds,
            batchCount,
            error, recordCapacitySamples)) {
        return false;
    }
    if (releaseTarget != nullptr) {
        releaseTarget->Release();
    }
    return true;
}

inline bool CommitPolyhedronTopologyCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const DecodedPolyhedronCache& polyhedron,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    return CommitPolyhedronTopologyCache(adapter, runtime, polyhedron, nullptr, error, recordCapacitySamples);
}

inline bool CommitPolyhedronTopologyCacheAndRelease(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    DecodedPolyhedronCache& polyhedron,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    return CommitPolyhedronTopologyCache(adapter, runtime, polyhedron, &polyhedron, error, recordCapacitySamples);
}

inline bool CommitTopologyCache(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const DecodedTopologyCache& topology,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    if (auto* result = dynamic_cast<DecodedLeafBuilder*>(&adapter)) {
        if (!runtime.Run().IsDriverThread() || runtime.Run().Stopped()) {
            return validation::AssignError(error, "result publication requires an active driver");
        }
        bytestore::ByteStoreSession stores;
        stores.BindRun(runtime.Run());
        return result->AcceptTopology(topology, stores, error);
    }
    switch (topology.kind) {
        case DecodedTopologyCache::Kind::Structured:
            return adapter.SetStructuredAxisSize(topology.structuredAxisSize.data(), error);
        case DecodedTopologyCache::Kind::Connectivity:
            return CommitConnectivityTopologyCache(adapter, runtime, topology, error);
        case DecodedTopologyCache::Kind::Polyhedron:
            return topology.polyhedron.complete &&
                CommitPolyhedronTopologyCache(adapter, runtime, topology.polyhedron, error, recordCapacitySamples);
        case DecodedTopologyCache::Kind::None:
            return true;
    }
    return true;
}

inline bool CommitTopologyCacheAndRelease(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    DecodedTopologyCache& topology,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    if (dynamic_cast<DecodedLeafBuilder*>(&adapter)) {
        if (!CommitTopologyCache(adapter, runtime, topology, error, recordCapacitySamples)) { return false; }
        topology.Release();
        return true;
    }
    switch (topology.kind) {
        case DecodedTopologyCache::Kind::Structured:
            return adapter.SetStructuredAxisSize(topology.structuredAxisSize.data(), error);
        case DecodedTopologyCache::Kind::Connectivity:
            return CommitConnectivityTopologyCacheAndRelease(adapter, runtime, topology, error);
        case DecodedTopologyCache::Kind::Polyhedron:
            return topology.polyhedron.complete &&
                CommitPolyhedronTopologyCacheAndRelease(adapter, runtime, topology.polyhedron, error, recordCapacitySamples);
        case DecodedTopologyCache::Kind::None:
            return true;
    }
    return true;
}

struct AttributeCommitField {
    std::size_t attrIndex{0u};
    const AttrStorageParams* meta{nullptr};
    std::shared_ptr<bytestore::IRandomAccessByteStore> bytes;
    std::size_t tupleBytes{0u};
    std::size_t elementCount{0u};
    std::size_t totalBytes{0u};
    bool adapterBacked{false};
};

inline bool WriteAttributeCommitField(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    const AttributeCommitField& field,
    std::string* error = nullptr) {
    if (field.meta == nullptr || field.bytes == nullptr) {
        return validation::AssignError(error, "decoded attribute commit field is incomplete");
    }
    if (field.tupleBytes == 0u) {
        return true;
    }

    return ReplayTypedDecodedCache<std::uint8_t>(
        runtime,
        *field.bytes,
        field.elementCount,
        field.tupleBytes,
        [&](const std::size_t offset, const std::size_t count, const std::uint8_t* values) {
            std::size_t byteCount = 0u;
            if (!validation::CheckedMulSizeT(
                    count,
                    field.tupleBytes,
                    byteCount,
                    "decoded attribute replay byte count",
                    error)) {
                return false;
            }
            return adapter.WriteAttributeRange(
                field.attrIndex,
                offset,
                count,
                values,
                byteCount,
                error);
        },
        error);
}

inline bool CommitAttributeCacheFields(
    IDecodeAdapter& adapter,
    const CacheResources& runtime,
    DecodedAttributeCacheSet& attributes,
    const std::span<const std::size_t> attrIndices,
    std::string* error = nullptr) {
    if (attrIndices.empty()) { return true; }
    if (!attributes.IsInitialized()) {
        return validation::AssignError(error, "decoded attribute cache set is not initialized");
    }
    auto& root = runtime.Run();
    if (!root.IsDriverThread()) {
        return validation::AssignError(error, "decoded attribute commit requires the driver");
    }
    for (const auto attrIndex : attrIndices) {
        if (root.Stopped()) { return false; }
        if (attrIndex >= attributes.FieldCount() || !attributes.Complete(attrIndex)) {
            return validation::AssignError(error, "requested decoded attribute cache field is incomplete");
        }
        const auto* meta = attributes.Meta(attrIndex);
        auto bytes = attributes.Bytes(attrIndex);
        if (meta == nullptr || bytes == nullptr) {
            return validation::AssignError(error, "decoded attribute cache field is missing");
        }
        if (auto* result = dynamic_cast<DecodedLeafBuilder*>(&adapter)) {
            bytestore::ByteStoreSession stores;
            stores.BindRun(root);
            if (!result->AcceptAttribute(attrIndex, *meta, *bytes, stores, error)) { return false; }
            continue;
        }
        const auto tupleBytes = DecodeAttributeTupleBytes(*meta);
        std::size_t elementCount = 0u, totalBytes = 0u;
        if (!ResolveAttributeCommitShape(*meta, tupleBytes, elementCount, totalBytes, error)) { return false; }
        AttributeCommitField field{attrIndex, meta, std::move(bytes), tupleBytes,
            elementCount, totalBytes, attributes.AdapterBacked(attrIndex)};
        // 每个字段的构造、窗口写入和挂接均在 driver 完成
        // 已在宿主数组中完成的字段直接挂接，回放字段仍需重型阶段准入
        std::optional<HeavyPhaseLease> phase;
        if (!field.adapterBacked) {
            phase = WaitForHeavyPhase(root);
            if (!phase || !adapter.BeginAttribute(attrIndex, *meta, error) ||
                !WriteAttributeCommitField(adapter, runtime, field, error)) { return false; }
        }
        if (root.Stopped() || !adapter.EndAttribute(attrIndex, error)) { return false; }
    }
    return true;
}

} // namespace datacodec

#endif
