#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURESTORAGEOWNERSHIP_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURESTORAGEOWNERSHIP_H

#include "DataCodec/Codec/Remap/RemapOrderSource.h"
#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Storage/ByteStore/SegmentedBinaryObject.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedAttributeCacheSet.h"
#include "DataCodec/Codec/Topology/TopologyDecodeCacheSink.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Feature/DataCodecFeatureOutputResources.h"
#include "DataCodec/Test/Feature/DataCodecFeatureCapacityDiagnostics.h"
#include "DataCodec/Test/Feature/DataCodecFeatureStorageAudit.h"

#include <array>
#include <memory>
#include <optional>
#include <type_traits>

namespace datacodec::test {

[[nodiscard]] inline TestResult RunDataCodecFeatureStorageOwnership() {
    TestResult result = RunDataCodecFeatureOutputResources();
    const auto diagnostics = RunDataCodecFeatureCapacityDiagnostics();
    result.passed &= diagnostics.passed;
    result.failures.insert(result.failures.end(), diagnostics.failures.begin(), diagnostics.failures.end());
    result.AppendDiagnostics(diagnostics.diagnostics);
    const auto audit = RunDataCodecFeatureStorageAudit();
    result.passed &= audit.passed;
    result.failures.insert(result.failures.end(), audit.failures.begin(), audit.failures.end());
    result.AppendDiagnostics(audit.diagnostics);
    static_assert(!std::is_copy_constructible_v<resource::ResidentByteBudget::Lease>);
    static_assert(std::is_nothrow_move_constructible_v<resource::ResidentByteBudget::Lease>);
    resource::ResidentByteBudget budget(20u);
    auto first = budget.TryReserve(8u);
    auto second = budget.TryReserve(10u);
    Require(result, first && second && !budget.TryReserve(3u) && budget.Snapshot().reservedBytes == 18u,
        "capacity.sum", "capacity leases must share the same exact reservation limit");
    *second = std::move(*first);
    Require(result, first->Bytes() == 0u && second->Bytes() == 8u && budget.Snapshot().reservedBytes == 8u,
        "capacity.move-assignment", "moving a lease must retire its previous reservation once");
    first.reset();
    second.reset();
    Require(result, budget.Snapshot().reservedBytes == 0u && budget.Snapshot().peakReservedBytes == 18u,
        "capacity.return", "lease destruction must return its reservation without changing the peak");

    resource::ResidentByteBudget zero(0u);
    Require(result, zero.TryReserve(0u).has_value() && !zero.TryReserve(1u),
        "capacity.zero", "zero capacity must reject every positive reservation");
    std::optional<resource::ResidentByteBudget::Lease> survivor;
    {
        resource::ResidentByteBudget temporary(8u);
        survivor = temporary.TryReserve(8u);
    }
    {
        RejectAllocationsScope reject;
        survivor.reset();
    }
    Require(result, !survivor && rejectedAllocationCount == 0u,
        "capacity.state-lifetime", "returning a lease after the budget facade dies must not allocate");

    const std::array<std::uint8_t, 8> input{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{8u, 1u, 1u}, 8u, 1u, false, true, true});
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), true);
        auto granted = root.StorageCapacity()->TryReserve(input.size());
        Require(result, granted && root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck),
            "reserved.before-downsize", "a granted capacity lease must remain valid when the limit changes");
        auto memory = granted ? session.CreateReservedMemoryStore(std::move(*granted)) : nullptr;
        Require(result, memory && memory->ByteSizeHint() == input.size() && memory->WriteAt(0u, input) &&
            root.StorageCapacity()->Snapshot().reservedBytes == input.size(),
            "reserved.consume-once", "reserved storage must consume the original lease without a second admission");
        resource::ResidentByteBudget other(input.size());
        auto foreign = other.TryReserve(input.size());
        Require(result, !session.CreateReservedMemoryStore(std::move(*foreign)) &&
            !session.CreateReservedMemoryStore({}) && other.Snapshot().reservedBytes == 0u &&
            root.StorageCapacity()->Snapshot().reservedBytes == input.size(),
            "reserved.wrong-root", "a foreign or empty lease must be rejected and retired without charging the bound root");
        memory.reset();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "reserved.release", "the resulting memory owner must return the granted capacity on final release");
    }
    {
        const std::array<IndexType, 3u> order{2, 0, 1};
        const std::array<IndexType, 3u> inverse{1, 2, 0};
        for (const bool useFile : {false, true}) {
            const std::uint64_t limit = useFile ? 0u : 2u * sizeof(order);
            DataCodecExecutionResources root(ResolvedResourceConfiguration{
                {limit, 1u, 1u}, 2u * sizeof(order), 1u, false, true, true});
            bytestore::ByteStoreSession session;
            session.BindStorage(root.StorageCapacity(), useFile);
            auto forward = MakeStoreBackedWritableRemapProvider(order.size(), session, false, "order");
            auto backward = MakeStoreBackedWritableRemapProvider(order.size(), session, true, "inverse");
            Require(result, forward && backward &&
                root.StorageCapacity()->Snapshot().reservedBytes == limit &&
                forward->ResidentSizeHint() == (useFile ? 0u : sizeof(order)),
                "remap.exact-owners", "order and inverse must acquire complete fixed storage once");
            if (!forward || !backward) { continue; }
            IndexType value = 0;
            std::vector<IndexType> replay;
            Require(result, !forward->ReadAt(0u, value, nullptr) &&
                !backward->ReadRange(0u, order.size(), replay, nullptr) &&
                !forward->WriteAt(0u, 0) && !backward->AppendRange(order),
                "remap.writer-state", "unfinished storage must reject reads and the other writer protocol");
            Require(result, forward->AppendRange(std::span(order).first(1u)) && !forward->EndWrite() &&
                root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
                forward->AppendRange(std::span(order).subspan(1u)) &&
                !forward->AppendRange(std::span(order).first(1u)) && forward->EndWrite(),
                "remap.granted-downsize", "partial writes must finish within the granted owner after a limit decrease");
            Require(result, !backward->WriteAt(order.size(), 0) &&
                backward->WriteAt(2u, inverse[2]) && backward->WriteAt(0u, inverse[0]) &&
                backward->WriteAt(1u, inverse[1]) && backward->EndRandomWrite() &&
                !backward->WriteAt(0u, 0) && !forward->AppendRange(order) &&
                !forward->EndWrite() && !backward->EndRandomWrite(),
                "remap.seal", "random writes must stay in range and completed providers must be immutable");
            Require(result, root.UpdateLimits({2u * sizeof(order), 1u, 1u}, true,
                    ResourceDecisionReason::MechanismCheck) &&
                forward->ResidentSizeHint() == (useFile ? 0u : sizeof(order)) &&
                backward->ReadRange(0u, order.size(), replay, nullptr) &&
                std::equal(replay.begin(), replay.end(), inverse.begin(), inverse.end()),
                "remap.fixed-backend", "raising the limit must preserve the fixed backend and inverse values");
            auto retained = forward;
            session.ReleaseAll();
            forward.reset();
            backward.reset();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == (useFile ? 0u : sizeof(order)) &&
                retained->ReadRange(0u, order.size(), replay, nullptr) &&
                std::equal(replay.begin(), replay.end(), order.begin(), order.end()) &&
                !retained->ReadRange(order.size(), 1u, replay, nullptr),
                "remap.shared-release", "completed providers must remain readable after session and other consumers release");
            retained.reset();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "remap.last-release", "only the last provider reference returns its complete capacity");
        }
        bytestore::ByteStoreSession rejected;
        auto capacity = std::make_shared<resource::ResidentByteBudget>(0u);
        rejected.BindStorage(capacity, false);
        Require(result, !MakeStoreBackedWritableRemapProvider(order.size(), rejected, false, "required") &&
            rejected.SnapshotStats().storeCount == 0u,
            "remap.required-capacity", "necessary remap storage must reject unavailable capacity without spill");
        auto empty = MakeStoreBackedWritableRemapProvider(0u, rejected, false, "empty");
        std::vector<IndexType> replay;
        Require(result, empty && empty->EndWrite() && empty->ReadRange(0u, 0u, replay, nullptr) && replay.empty(),
            "remap.empty", "empty providers must complete without a positive reservation");
        empty.reset();
        rejected.BindStorage(capacity, true);
        Require(result, !MakeStoreBackedWritableRemapProvider(std::numeric_limits<std::size_t>::max(),
                rejected, false, "overflow"),
            "remap.overflow", "index-byte multiplication must fail before selecting a backend");
    }
    {
        const std::array<std::uint8_t, 2u * mortonremap::kMortonRunRecordBytes> records{
            1u, 0u, 4u, 0u, 0u, 0u, 2u, 0u, 7u, 0u, 0u, 0u};
        for (const bool useFile : {false, true}) {
            auto capacity = std::make_shared<resource::ResidentByteBudget>(useFile ? 0u : records.size());
            bytestore::ByteStoreSession session;
            session.BindStorage(capacity, useFile);
            mortonremap::RemapScratchSpooler spooler(session);
            auto run = spooler.CreateRun("run", 2u);
            Require(result, run.IsValid() && capacity->Snapshot().reservedBytes == (useFile ? 0u : records.size()) &&
                run.WriteRecordBytes(1u, std::span(records).subspan(mortonremap::kMortonRunRecordBytes)) &&
                run.WriteRecordBytes(0u, std::span(records).first(mortonremap::kMortonRunRecordBytes)),
                "morton.run-exact", "a run must acquire its full record count before writes");
            std::array<std::uint8_t, records.size()> replay{};
            Require(result, !run.WriteRecordBytes(2u, std::span(records).first(1u)) &&
                !run.WriteRecordBytes(std::numeric_limits<std::size_t>::max(), records) &&
                !run.ReadRecordBytes(1u, replay) && run.ReadRecordBytes(0u, replay) && replay == records,
                "morton.run-range", "out-of-range access must not enlarge or corrupt a fixed run");
            auto moved = std::move(run);
            session.ReleaseAll();
            Require(result, !run.IsValid() && moved.ReadRecordBytes(0u, replay) && replay == records,
                "morton.run-move", "moving a run and ending registration must preserve its data");
            moved.Release();
            Require(result, capacity->Snapshot().reservedBytes == 0u,
                "morton.run-release", "run consumption must return its exact capacity");
            Require(result, !spooler.CreateRun("overflow", std::numeric_limits<std::size_t>::max()).IsValid(),
                "morton.run-overflow", "run size overflow must fail before storage creation");
        }
    }
    {
        const std::array<IndexType, 6u> connectivity{0, 1, 2, 2, 3, 0};
        const std::array<IndexType, 3u> offsets{0, 3, 6};
        const std::array<IndexType, 2u> types{5, 5};
        const std::array<std::uint16_t, 2u> orders{1u, 2u};
        for (const bool externalSpill : {false, true}) {
            const auto limit = sizeof(connectivity) + (externalSpill ? 0u : sizeof(offsets) + sizeof(types) + sizeof(orders));
            auto capacity = std::make_shared<resource::ResidentByteBudget>(limit);
            bytestore::ByteStoreSession session;
            session.BindStorage(capacity, externalSpill);
            DecodedTopologyCache cache;
            topology::CacheTopologyDecodeSink sink(cache, session);
            const bool ready = sink.BeginConnectivityTopology(2u, 6u, true, true, true);
            Require(result, ready && cache.connectivity->ByteSizeHint() == sizeof(connectivity) &&
                cache.offsets->ByteSizeHint() == sizeof(offsets) && cache.cellTypes->ByteSizeHint() == sizeof(types) &&
                cache.cellPolynomialOrders->ByteSizeHint() == sizeof(orders) &&
                capacity->Snapshot().reservedBytes == limit &&
                (dynamic_cast<bytestore::MemoryStore*>(cache.offsets.get()) == nullptr) == externalSpill,
                "topology.exact-owners", "connectivity, offsets, types and orders must independently acquire exact storage");
            if (!ready) { continue; }
            Require(result, sink.WriteConnectivityRange(0u, connectivity) && sink.WriteOffsetsRange(0u, offsets) &&
                sink.WriteCellTypesRange(0u, types) && sink.WriteCellPolynomialOrdersRange(0u, orders) &&
                sink.EndConnectivityTopology(nullptr),
                "topology.sink-write", "the topology sink must fill and seal all fixed-size targets");
            auto retained = cache.connectivity;
            DecodedTopologyCache moved(std::move(cache));
            session.ReleaseAll();
            std::array<IndexType, 3u> readOffsets{};
            Require(result, moved.complete && moved.offsets->Read(0u,
                std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(readOffsets.data()), sizeof(readOffsets))) &&
                readOffsets == offsets,
                "topology.session-read", "mixed topology stores must survive moves and registration-session release");
            moved.Release();
            Require(result, capacity->Snapshot().reservedBytes == sizeof(connectivity),
                "topology.shared-owner", "releasing topology must preserve an independently retained connectivity owner");
            retained.reset();
            Require(result, capacity->Snapshot().reservedBytes == 0u,
                "topology.last-release", "the last connectivity owner must return its exact capacity");
        }
        auto capacity = std::make_shared<resource::ResidentByteBudget>(sizeof(connectivity));
        bytestore::ByteStoreSession session;
        session.BindStorage(capacity, false);
        DecodedTopologyCache cache;
        Require(result, !cache.InitializeConnectivity(2u, 6u, true, true, true, session) &&
            cache.kind == DecodedTopologyCache::Kind::None && !cache.connectivity &&
            capacity->Snapshot().reservedBytes == 0u && session.SnapshotStats().storeCount == 0u,
            "topology.partial-rollback", "a later target denial must release previously admitted targets before returning");
        Require(result, cache.InitializeConnectivity(2u, 6u, false, false, false, session) &&
            cache.offsets->ByteSizeHint() == 0u && !cache.cellTypes && !cache.cellPolynomialOrders &&
            capacity->Snapshot().reservedBytes == sizeof(connectivity),
            "topology.fixed-cell-targets", "absent fields must not reserve positive capacity");
        cache.Release();
        const auto maximum = std::numeric_limits<std::size_t>::max();
        Require(result, !cache.InitializeConnectivity(maximum, 0u, true, false, false, session) &&
            !cache.InitializeConnectivity(1u, maximum, false, false, false, session) &&
            !cache.InitializeConnectivity(maximum, 0u, false, true, false, session) &&
            !cache.InitializeConnectivity(maximum, 0u, false, false, true, session) &&
            capacity->Snapshot().reservedBytes == 0u && cache.kind == DecodedTopologyCache::Kind::None,
            "topology.count-overflow", "all topology count arithmetic must be validated before target creation");
    }
    {
        CodecStorageParams params;
        params.attrParams.resize(3u);
        for (auto& meta : params.attrParams) {
            meta.elementCount = 2u;
            meta.dimension = 1;
            meta.dataType = DataType::Float32;
        }
        params.attrParams[1].dataType = DataType::Float64;
        const std::array<double, 2u> doubles{3., 7.};
        for (const bool externalSpill : {false, true}) {
            const auto limit = externalSpill ? 8u : 24u;
            DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u}, 24u, 1u, false, true, externalSpill});
            bytestore::ByteStoreSession session;
            session.BindStorage(root.StorageCapacity(), externalSpill);
            DecodedAttributeCacheSet fields;
            Require(result, fields.Initialize(params, session) &&
                root.StorageCapacity()->Snapshot().reservedBytes == 0u && !fields.Bytes(0u) && !fields.Bytes(2u),
                "attributes.lazy-fields", "metadata initialization must not reserve or allocate complete field arrays");
            const bool ready = fields.BeginAttribute(0u, params.attrParams[0]) &&
                fields.BeginAttribute(1u, params.attrParams[1]);
            Require(result, ready && fields.Bytes(0u)->ByteSizeHint() == 8u &&
                fields.Bytes(1u)->ByteSizeHint() == 16u && !fields.Bytes(2u) &&
                root.StorageCapacity()->Snapshot().reservedBytes == limit &&
                (dynamic_cast<bytestore::MemoryStore*>(fields.Bytes(1u).get()) == nullptr) == externalSpill,
                "attributes.individual-admission", "only requested fields must select storage with their exact scalar sizes");
            if (!ready) { continue; }
            Require(result, root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
                fields.WriteAttributeRange(0u, 0u, 2u, input.data(), input.size()) && fields.EndAttribute(0u) &&
                fields.WriteAttributeRange(1u, 0u, 2u, doubles.data(), sizeof(doubles)) && fields.EndAttribute(1u),
                "attributes.granted-write", "complete admitted fields must finish after the root capacity decreases");
            auto retained = fields.Bytes(0u);
            fields.ReleaseFieldBytes(0u);
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == limit,
                "attributes.shared-release", "releasing one field view must not return another consumer's live capacity");
            DecodedAttributeCacheSet moved(std::move(fields));
            session.ReleaseAll();
            std::array<double, 2u> read{};
            Require(result, moved.ReadRange(1u, 0u, 2u, read.data(), sizeof(read)) && read == doubles,
                "attributes.move-and-session", "moved completed fields must remain readable after session release");
            moved.Reset();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 8u,
                "attributes.reset", "cache reset must preserve storage held by a remaining field consumer");
            retained.reset();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "attributes.last-release", "the last shared field reference must return its capacity");
        }
        class HostAttributeStore final : public bytestore::IRandomAccessByteStore {
        public:
            std::array<std::uint8_t, 8u> bytes{};
            std::size_t resizeCount{0u};
            std::uint64_t ByteSizeHint() const noexcept override { return bytes.size(); }
            bool CanRead() const noexcept override { return true; }
            bool ResizeBytes(std::uint64_t size, std::string*) override { ++resizeCount; return size == bytes.size(); }
            bool Seal(std::string*) override { return true; }
            bool AppendBytes(std::span<const std::uint8_t>, std::string*) override { return false; }
            bool WriteBytesAt(std::uint64_t offset, std::span<const std::uint8_t> data, std::string*) override {
                if (offset > bytes.size() || data.size() > bytes.size() - offset) { return false; }
                std::copy(data.begin(), data.end(), bytes.begin() + offset);
                return true;
            }
            bool Read(std::uint64_t offset, std::span<std::uint8_t> data, std::string*) const override {
                if (offset > bytes.size() || data.size() > bytes.size() - offset) { return false; }
                std::copy_n(bytes.begin() + offset, data.size(), data.begin());
                return true;
            }
            bool CopyTo(bytestore::IByteWriter& writer, std::string* error) override { return writer.Write(bytes, error); }
        };
        auto capacity = std::make_shared<resource::ResidentByteBudget>(0u);
        bytestore::ByteStoreSession session;
        session.BindStorage(capacity, false);
        DecodedAttributeCacheSet fields;
        auto host = std::make_shared<HostAttributeStore>();
        Require(result, fields.Initialize(params, session) && fields.BindAttributeStore(0u, host, true) &&
            fields.BeginAttribute(0u, params.attrParams[0]) &&
            fields.WriteAttributeRange(0u, 0u, 2u, input.data(), input.size()) && fields.EndAttribute(0u) &&
            host->resizeCount == 1u && host->bytes == input && capacity->Snapshot().reservedBytes == 0u &&
            session.SnapshotStats().storeCount == 0u,
            "attributes.host-exempt", "host output must retain its allocation protocol without root capacity admission");
        Require(result, !fields.BeginAttribute(1u, params.attrParams[1]) && !fields.Bytes(1u),
            "attributes.necessary-denied", "owned positive-size fields must reject zero capacity without external spill");
        auto oversized = params.attrParams[1];
        oversized.elementCount = std::numeric_limits<decltype(oversized.elementCount)>::max();
        Require(result, !fields.BeginAttribute(1u, oversized) && !fields.Bytes(1u) &&
            capacity->Snapshot().reservedBytes == 0u,
            "attributes.overflow", "complete attribute overflow must fail before acquiring storage");
    }
    {
        const std::array<float, 6u> points{1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
        const std::array<double, 6u> referenceValues{1., 2., 3., 4., 5., 6.};
        GeometryStorageParams meta;
        meta.elementCount = 2u;
        meta.dimension = 3;
        meta.dataType = DataType::Float64;
        for (const bool externalSpill : {false, true}) {
            const auto limit = externalSpill ? sizeof(points) : sizeof(points) + sizeof(referenceValues);
            auto capacity = std::make_shared<resource::ResidentByteBudget>(limit);
            DecodedGeometryCache geometry;
            DecodedGeometryReferenceCache reference;
            std::shared_ptr<bytestore::IRandomAccessByteStore> retained;
            {
                bytestore::ByteStoreSession session;
                session.BindStorage(capacity, externalSpill);
                std::string error;
                const bool ready = geometry.Initialize(2u, 3u, session, &error) &&
                    reference.BeginGeometry(meta, session, &error);
                Require(result, ready && geometry.bytes->ByteSizeHint() == sizeof(points) &&
                    reference.Bytes()->ByteSizeHint() == sizeof(referenceValues) &&
                    capacity->Snapshot().reservedBytes == limit &&
                    (dynamic_cast<bytestore::MemoryStore*>(reference.Bytes().get()) == nullptr) == externalSpill,
                    "geometry.sized-owners", "float output and original-type reference must admit their own exact sizes");
                if (!ready) { continue; }
                Require(result, geometry.bytes->WriteBytesAt(0u,
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(points.data()), sizeof(points))) &&
                    reference.WriteRange(0u, 2u, referenceValues.data(), sizeof(referenceValues)) &&
                    reference.EndGeometry(),
                    "geometry.write", "sized geometry stores must accept the complete declared fields");
                retained = geometry.bytes;
                geometry.Release();
            }
            std::array<double, 6u> readReference{};
            std::array<float, 6u> readPoints{};
            Require(result, capacity->Snapshot().reservedBytes == limit &&
                reference.ReadRange(0u, 2u, readReference.data(), sizeof(readReference)) &&
                readReference == referenceValues && retained && retained->Read(0u,
                    std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(readPoints.data()), sizeof(readPoints))) &&
                readPoints == points,
                "geometry.session-survivor", "geometry owners must stay readable after their registration session ends");
            reference.Reset();
            Require(result, capacity->Snapshot().reservedBytes == sizeof(points),
                "geometry.reference-release", "dropping the reference must preserve the other geometry owner");
            retained.reset();
            Require(result, capacity->Snapshot().reservedBytes == 0u,
                "geometry.last-release", "the last output consumer must return the remaining geometry capacity");
        }
        bytestore::ByteStoreSession zero;
        zero.BindStorage(std::make_shared<resource::ResidentByteBudget>(0u), false);
        DecodedGeometryCache rejected;
        DecodedGeometryReferenceCache reference;
        Require(result, !rejected.Initialize(2u, 3u, zero) && !rejected.bytes &&
            !reference.BeginGeometry(meta, zero) && !reference.IsInitialized(),
            "geometry.no-capacity", "necessary geometry must reject unavailable memory without spill capability");
        Require(result, rejected.Initialize(0u, 3u, zero) && rejected.bytes->ByteSizeHint() == 0u,
            "geometry.empty", "an empty geometry needs no positive memory reservation");
        rejected.Release();
        zero.BindStorage(std::make_shared<resource::ResidentByteBudget>(0u), true);
        Require(result, !rejected.Initialize(std::numeric_limits<std::size_t>::max(), 3u, zero) && !rejected.bytes,
            "geometry.output-overflow", "output byte-count overflow must fail before choosing any backend");
        meta.elementCount = std::numeric_limits<decltype(meta.elementCount)>::max();
        Require(result, !reference.BeginGeometry(meta, zero) && !reference.Bytes(),
            "geometry.reference-overflow", "reference byte-count overflow must fail before creating storage");
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{8u, 1u, 1u}, 32u, 1u, false, true, true});
        bytestore::ByteStoreSession sized;
        sized.BindStorage(root.StorageCapacity(), root.ExternalSpillAvailable());
        auto memory = sized.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, 8u, ::datacodec::MemoryDemandKind::RequiredContinuation);
        Require(result, memory && dynamic_cast<bytestore::MemoryStore*>(memory.get()) != nullptr &&
            memory->ByteSizeHint() == 8u && memory->ResidentSizeHint() == 8u &&
            root.StorageCapacity()->Snapshot().reservedBytes == 8u && memory->WriteAt(0u, input),
            "sized.exact-admission", "known-length memory must reserve exactly once before the first write");
        auto file = sized.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, 8u, ::datacodec::MemoryDemandKind::RequiredContinuation);
        Require(result, file && dynamic_cast<bytestore::FileBackedStreamStore*>(file.get()) != nullptr &&
            file->ByteSizeHint() == 8u && file->ResidentSizeHint() == 0u && file->WriteAt(0u, input) && file->Seal(),
            "sized.ranged-file", "denied memory admission may choose a file before allocation and writing");
        Require(result, !sized.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 1u, ::datacodec::MemoryDemandKind::RequiredContinuation) &&
            root.StorageCapacity()->Snapshot().reservedBytes == 8u,
            "sized.contiguous-required", "necessary contiguous storage must never select a file");
        Require(result, root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            memory && memory->WriteAt(0u, input) && memory->Seal(),
            "sized.granted-downsize", "an already granted exact store must finish after the limit decreases");
        auto empty = sized.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 0u, ::datacodec::MemoryDemandKind::RequiredContinuation);
        Require(result, empty && empty->ResidentSizeHint() == 0u && empty->ByteSizeHint() == 0u,
            "sized.empty", "an empty store must need no positive capacity even when U exceeds M");
        Require(result, root.UpdateLimits({32u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck),
            "sized.increase", "test setup must permit the later reservation");
        std::array<std::uint8_t, 8> replay{};
        Require(result, file && file->Read(0u, replay) && replay == input && file->ResidentSizeHint() == 0u,
            "sized.fixed-file", "an increase must preserve the selected file and its bytes");
        const auto beforeFailure = sized.SnapshotStats();
        bool failedCreation = false;
        {
            RejectAllocationsScope reject;
            try {
                failedCreation = !sized.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, 8u, ::datacodec::MemoryDemandKind::RequiredContinuation);
            } catch (const std::bad_alloc&) {
                failedCreation = true;
            }
        }
        Require(result, failedCreation && rejectedAllocationCount == 1u &&
            root.StorageCapacity()->Snapshot().reservedBytes == 8u &&
            sized.SnapshotStats().storeCount == beforeFailure.storeCount &&
            sized.SnapshotStats().managedFileBytes == beforeFailure.managedFileBytes,
            "sized.creation-rollback", "failed owner creation must release its lease without choosing a file");
        bytestore::ByteStoreSession noSpill;
        noSpill.BindStorage(std::make_shared<resource::ResidentByteBudget>(0u), false);
        Require(result, !noSpill.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, 1u, ::datacodec::MemoryDemandKind::RequiredContinuation) &&
            noSpill.SnapshotStats().storeCount == 0u,
            "sized.no-spill", "no-spill runtime must reject necessary sized memory when admission is denied");
        memory.reset();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "sized.last-release", "only the memory owner's last reference returns its capacity");
    }
    for (const bool externalSpillAvailable : {false, true}) {
        ResourceSample sample;
        sample.threaded = false;
        sample.allowedComputeThreads = 1u;
        sample.externalSpillAvailable = externalSpillAvailable;
        const auto config = ResolveResourceConfiguration(CodecResourceParams{
            .mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u,
            .ownedStorageLimitBytes = 32u}, sample);
        DataCodecExecutionResources root(config);
        Require(result, root.ExternalSpillAvailable() == externalSpillAvailable,
            "append.platform-capability", "the execution root must preserve resolved spill capability");
        bytestore::ByteStoreSession appendSession;
        Require(result, !appendSession.CreateAppendableByteStore() && !appendSession.CreateManagedByteStore(),
            "append.unbound", "unbound storage must not create memory or file owners");
        appendSession.BindStorage(root.StorageCapacity(), root.ExternalSpillAvailable());
        auto append = appendSession.CreateAppendableByteStore("fixed_backend");
        Require(result, append && (dynamic_cast<bytestore::MemoryStore*>(append.get()) != nullptr) ==
            !externalSpillAvailable && append->AppendBytes(input),
            "append.fixed-backend", "append backend must follow platform capability at creation");
        if (!append) { continue; }
        const auto initial = appendSession.SnapshotStats();
        Require(result, initial.storeCount == 1u && initial.reservedBytes == (externalSpillAvailable ? 0u : 8u) &&
            initial.managedFileBytes == (externalSpillAvailable ? 8u : 0u),
            "append.accounting", "file logical bytes must not enter owned memory capacity");
        Require(result, root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            append->AppendBytes(input) == externalSpillAvailable,
            "append.downsize", "existing file output may grow at zero M; memory growth must be rejected");
        Require(result, root.UpdateLimits({32u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            append->Seal(), "append.seal", "limit changes must not replace the selected owner");
        std::array<std::uint8_t, 8> replay{};
        Require(result, append->Read(0u, replay) && replay == input && !append->AppendBytes(input),
            "append.read-after-seal", "sealed output must remain readable and reject later writes");
        appendSession.Reset();
        Require(result, append->Read(0u, replay) && replay == input,
            "append.reset-survivor", "session cleanup must preserve a shared append result");
        append.reset();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "append.last-release", "last append owner must return its actual capacity");
        bytestore::ByteStoreSession moved(std::move(appendSession));
        auto next = moved.CreateAppendableByteStore("moved_backend");
        Require(result, next && (dynamic_cast<bytestore::MemoryStore*>(next.get()) != nullptr) ==
            !externalSpillAvailable, "append.moved-binding", "session move must preserve backend capability");
        if (!externalSpillAvailable) {
            Require(result, !moved.CreateManagedByteStore(), "append.no-external-file",
                "a runtime without external storage must reject file creation");
            bool allocationFailed = false;
            {
                RejectAllocationsScope reject;
                allocationFailed = next && !next->AppendBytes(input);
            }
            Require(result, allocationFailed && rejectedAllocationCount == 1u &&
                moved.SnapshotStats().managedFileBytes == 0u && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "append.no-failure-backend-switch", "real allocation failure must roll back without choosing a file");
        }
    }
    auto growingBudget = std::make_shared<resource::ResidentByteBudget>(26u);
    auto growing = std::make_shared<bytestore::MemoryStore>(growingBudget);
    Require(result, growing->Append(input) && growing->Resize(9u) && growing->ResidentSizeHint() == 12u &&
        growing->Resize(13u) && growing->ResidentSizeHint() == 14u &&
        growingBudget->Snapshot().reservedBytes == 14u && growingBudget->Snapshot().peakReservedBytes == 26u,
        "capacity.growth-overlap", "growth must reserve the entire new array while the old array remains alive");
    std::array<std::uint8_t, 8> read{};
    Require(result, growing->Read(0u, read) && read == input && !growing->Resize(15u) &&
        growing->ByteSizeHint() == 13u && growing->ResidentSizeHint() == 14u,
        "capacity.rejection-preserves-data", "rejected growth must preserve the original size, capacity and bytes");
    bool shrunk = false;
    {
        RejectAllocationsScope reject;
        shrunk = growing->Resize(4u) && growing->ResidentSizeHint() == 14u;
    }
    Require(result, shrunk && rejectedAllocationCount == 0u && growingBudget->Snapshot().reservedBytes == 14u,
        "capacity.logical-shrink", "logical shrink must not allocate or return still-owned capacity");

    auto failedBudget = std::make_shared<resource::ResidentByteBudget>(64u);
    auto failed = std::make_shared<bytestore::MemoryStore>(failedBudget);
    Require(result, failed->Append(input), "capacity.failure-setup", "initial allocation must succeed");
    bool rejected = false;
    {
        RejectAllocationsScope reject;
        rejected = !failed->Resize(12u);
    }
    Require(result, rejected && rejectedAllocationCount == 1u && failedBudget->Snapshot().reservedBytes == 8u &&
        failed->ResidentSizeHint() == 8u && failed->Read(0u, read) && read == input,
        "capacity.allocation-rollback", "allocation failure must roll back its new lease and retain old data");
    Require(result, failed->Append(failed->ContiguousBytes()) && failed->ByteSizeHint() == 16u &&
        failed->Read(8u, read) && read == input,
        "capacity.self-append", "self append must rebase its source after the old allocation is replaced");

    bytestore::ByteStoreSession session;
    Require(result, session.CreateMemoryStore() == nullptr && session.SnapshotStats().reservedBytes == 0u,
        "owner.unbound-session", "an unbound session must not create unlimited memory storage");
    session.BindStorage(std::make_shared<resource::ResidentByteBudget>(8u), true);
    auto owner = session.CreateMemoryStore();
    Require(result, owner->Append(input), "owner.setup", "shared store setup must succeed");
    auto alias = owner;
    session.ReleaseAll();
    session.Reset();
    auto denied = session.CreateMemoryStore();
    Require(result, owner->Read(0u, read) && read == input && !denied->Resize(1u) &&
        session.SnapshotStats().reservedBytes == 8u,
        "owner.session-reset", "session cleanup must preserve externally held owners and reservations");
    owner.reset();
    Require(result, alias->Read(0u, read) && read == input && session.SnapshotStats().reservedBytes == 8u,
        "owner.shared-reference", "remaining consumers must retain readable storage and its reservation");
    alias.reset();
    Require(result, session.SnapshotStats().reservedBytes == 0u && denied->Resize(1u),
        "owner.last-reference", "last owner destruction must return capacity for subsequent admission");

    std::shared_ptr<bytestore::MemoryStore> escaped;
    std::shared_ptr<resource::ResidentByteBudget> escapedCapacity;
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{16u, 1u, 1u}, 32u, 1u, false, true});
        bytestore::ByteStoreSession leafSession;
        bytestore::ByteStoreSession referenceSession;
        leafSession.BindStorage(root.StorageCapacity(), true);
        referenceSession.BindStorage(root.StorageCapacity(), true);
        auto leaf = leafSession.CreateMemoryStore();
        escaped = referenceSession.CreateMemoryStore();
        Require(result, leaf->Append(input) && escaped->Append(input) &&
            root.StorageCapacity()->Snapshot().reservedBytes == 16u,
            "owner.same-root", "leaf and reference stores must reserve from the same capacity state");
        leafSession.Register(leaf);
        leafSession.Register(leaf);
        Require(result, leafSession.SnapshotStats().storeCount == 1u &&
            leafSession.SnapshotStats().residentBytes == 8u,
            "owner.unique-registration", "registering a shared owner repeatedly must not duplicate audit bytes");
        Require(result, root.UpdateLimits({8u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            !referenceSession.CreateMemoryStore()->Resize(1u) && escaped->Read(0u, read) && read == input,
            "owner.root-downsize", "downsize must preserve existing arrays and reject only further growth");
        leaf.reset();
        escapedCapacity = root.StorageCapacity();
    }
    Require(result, escapedCapacity->Snapshot().reservedBytes == 8u && escaped->Read(0u, read) && read == input,
        "owner.root-lifetime", "an output must retain its capacity and data after root execution is destroyed");
    {
        RejectAllocationsScope reject;
        escaped.reset();
    }
    Require(result, escapedCapacity->Snapshot().reservedBytes == 0u && rejectedAllocationCount == 0u,
        "owner.root-last-release", "last owner destruction after root shutdown must return capacity without allocation");

    static_assert(!std::is_copy_constructible_v<EncodedBuffer>);
    static_assert(std::is_nothrow_move_constructible_v<EncodedBuffer>);
    EncodedBuffer encodedOutput;
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{16u, 1u, 1u}, 16u, 1u, false, true});
        escapedCapacity = root.StorageCapacity();
        MemoryByteRangeOutput output(root);
        Require(result, output.PrepareExactSize(8u) && output.Bytes().empty() &&
            escapedCapacity->Snapshot().reservedBytes == 8u &&
            root.UpdateLimits({4u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck),
            "output.prepare-layout", "the complete layout must reserve once before consuming source data");
        Require(result, output.WriteAt(6u, std::span(input).first(2u)) && output.Finalize(8u),
            "output.prepared-downsize", "a prepared output must remain writable after the root limit is reduced");
        const auto* address = output.Bytes().data();
        {
            RejectAllocationsScope reject;
            encodedOutput = output.TakeBytes();
        }
        Require(result, encodedOutput.data() == address && encodedOutput.size() == 8u &&
            encodedOutput.span()[0] == 0u && encodedOutput.span()[5] == 0u && encodedOutput.span()[6] == input[0] &&
            rejectedAllocationCount == 0u && escapedCapacity->Snapshot().reservedBytes == 8u,
            "output.move-owner", "finalization must transfer the same allocation and initialize unwritten gaps");
    }
    const auto* encodedAddress = encodedOutput.data();
    {
        MemoryByteRangeReader reader(std::move(encodedOutput));
        Require(result, reader.ContiguousRange(0u, 8u).data() == encodedAddress &&
            escapedCapacity->Snapshot().reservedBytes == 8u && encodedOutput.empty(),
            "output.reader-owner", "the decoder reader must retain the original output allocation without copying");
    }
    Require(result, escapedCapacity->Snapshot().reservedBytes == 0u,
        "output.reader-release", "the last reader must release the encoded allocation after the root is destroyed");
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{8u, 1u, 1u}, 8u, 1u, false, true});
        auto limited = root.StorageCapacity();
        MemoryByteRangeOutput output(root);
        Require(result, output.WriteAt(0u, input) && !output.WriteAt(8u, std::span(input).first(1u)) &&
            !output.Finalize(8u) && output.TakeBytes().empty() && limited->Snapshot().reservedBytes == 8u,
            "output.failed-growth", "rejected growth must fail the output and preserve its existing allocation until cleanup");
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{16u, 1u, 1u}, 16u, 1u, false, true});
        auto limited = root.StorageCapacity();
        MemoryByteRangeOutput output(root);
        bool rejectedAllocation = false;
        {
            RejectAllocationsScope reject;
            rejectedAllocation = !output.PrepareExactSize(8u) && output.TakeBytes().empty();
        }
        Require(result, rejectedAllocation && rejectedAllocationCount == 1u && limited->Snapshot().reservedBytes == 0u,
            "output.allocation-failure", "failed initial output allocation must release its reservation without retrying");
    }

    class CountingWriter final : public bytestore::IByteWriter {
    public:
        bool Write(std::span<const std::uint8_t> bytes, std::string*) override {
            count += bytes.size();
            return true;
        }
        std::size_t count{0u};
    } writer;
    auto sharedBytes = std::make_shared<bytestore::MemoryStore>(failedBudget);
    Require(result, sharedBytes->Append(input), "owner.segment-setup", "segment setup must succeed");
    auto segmented = std::make_shared<bytestore::SegmentedBinaryObject>();
    Require(result, segmented->AddSegment(sharedBytes) && segmented->CopyTo(writer) && writer.count == input.size() &&
        !segmented->CanRead() && sharedBytes->Read(0u, read) && read == input,
        "owner.segment-consumption", "one-shot consumption must drop its reference without clearing another owner's bytes");

    auto provider = std::make_shared<VectorRemapProvider>(std::vector<IndexType>{2u, 0u, 1u});
    auto source = RemapOrderSource::TryComputed(provider);
    const auto retained = source->Handle();
    (void)source->Release();
    IndexType index = 0u;
    Require(result, retained->Size() == 3u && retained->ReadAt(0u, index, nullptr) && index == 2u,
        "owner.remap-reference", "dropping one remap source must preserve another consumer's provider");
    return result;
}

} // DataCodec 测试命名空间

#endif
