#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURETOPOLOGYEXECUTION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURETOPOLOGYEXECUTION_H

#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyBlockEncode.h"
#include "DataCodec/Codec/Topology/TopologyDecode.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyEncode.h"
#include "DataCodec/Test/Adapter/DataCodecTestAdapter.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"
#include "DataCodec/Workflow/Decode/Stages/TopoDecodeStage.h"
#include "DataCodec/Workflow/Decode/Stages/DecodeCommitStage.h"

namespace datacodec::test {

inline TestResult RunDataCodecFeatureTopologyExecution() {
    TestResult result;
    for (unsigned kind = 0u; kind < 3u; ++kind) {
        DataCodecExecutionResources root(CodecResourceParams{.mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u});
        CodecRunScope scope(root);
        DecodeContext context(root);
        TestDecodeAdapter adapter;
        context.adapter = &adapter;
        DecodeLeafWorkspace workspace;
        RunBinding binding(workspace, root);
        CodecStorageParams params;
        params.meshType = kind == 1u ? MeshType::UnstructuredMesh : MeshType::PointSet;
        params.topoParams.cellCount = kind == 0u ? 0u : 1u;
        params.topoParams.cellBufferSize = kind == 0u ? 0u : 3u;
        workspace.SetStorageParams(std::move(params));
        DecodedTopologyReferenceCacheStore references;
        context.topologyReferenceStore = &references;
        context.topologyReferenceKey = "missing-frame-topology";
        TopoDecodeStage stage;
        stage.Execute(context, workspace);
        const bool success = !root.FirstFailure().has_value();
        const bool completeEmpty = kind == 0u && workspace.topology && workspace.topology->complete &&
            workspace.topology->kind == DecodedTopologyCache::Kind::None &&
            CommitDecodedTopologyIfPresent(context, workspace) &&
            references.Get(context.topologyReferenceKey) == workspace.topology;
        Require(result, success == (kind == 0u) &&
            (kind == 0u ? completeEmpty : workspace.topology == nullptr) &&
            root.StorageCapacity()->Snapshot().reservedBytes == 0u && scope.Finish(success) == success,
            kind == 0u ? "topology.pointset-no-topology-reference" : "topology.nonempty-missing-reference",
            "topology-free point frames must not require a reference; nonempty topology must retain reference validation");
    }
    for (const bool externalSpill : {false, true}) {
        const std::size_t size = kIoWindowBytes + 16u;
        auto capacity = std::make_shared<resource::ResidentByteBudget>(externalSpill ? 0u : size);
        bytestore::ByteStoreSession session;
        session.BindStorage(capacity, externalSpill);
        auto source = session.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, size, ::datacodec::MemoryDemandKind::RequiredContinuation, "range_source");
        std::vector<std::uint8_t> payload(size);
        for (std::size_t i = 0u; i < size; ++i) { payload[i] = static_cast<std::uint8_t>(i % 251u); }
        Require(result, source && source->WriteBytesAt(0u, payload) && source->Seal(),
            "topology.range-source", "the complete topology source must be sealed before sharing ranges");
        std::weak_ptr<bytestore::IByteSource> weak = source;
        auto range = std::make_shared<bytestore::SubrangeByteSource>(source, 3u, size - 7u);
        auto tail = std::make_shared<bytestore::SubrangeByteSource>(source, size - 4u, 4u);
        {
            bytestore::SubrangeByteSource invalid(source, std::numeric_limits<std::uint64_t>::max(), 8u);
            std::span<const std::uint8_t> bytes;
            Require(result, !invalid.CanRead() &&
                invalid.PrepareContiguousBytes(bytes) == ContiguousViewStatus::Error &&
                !range->Read(range->ByteSizeHint(), std::span<std::uint8_t>(payload).first(1u)),
                "topology.range-bounds", "overflowing source ranges and reads outside a slice must fail");
        }
        source.reset();
        session.ReleaseAll();
        class CheckingWriter final : public bytestore::IByteWriter {
        public:
            bool Write(std::span<const std::uint8_t> bytes, std::string*) override {
                ++calls;
                if (reject || bytes.size() > kIoWindowBytes) { return false; }
                for (const auto value : bytes) {
                    if (value != static_cast<std::uint8_t>((3u + consumed++) % 251u)) { return false; }
                }
                return true;
            }
            std::size_t consumed{0u}, calls{0u};
            bool reject{false};
        } writer, rejected;
        rejected.reject = true;
        Require(result, range->CopyTo(writer) && writer.consumed == size - 7u && writer.calls == 2u &&
            !range->CopyTo(rejected) && rejected.calls == 1u &&
            range->ResidentSizeHint() == 0u && capacity->Snapshot().reservedBytes == (externalSpill ? 0u : size),
            "topology.shared-range", "ranges must survive session release, copy by fixed windows and retain one source capacity");
        range.reset();
        Require(result, !weak.expired(), "topology.range-last-reader", "the final range consumer must keep the source alive");
        tail.reset();
        Require(result, weak.expired() && capacity->Snapshot().reservedBytes == 0u,
            "topology.range-release", "the last shared range must release its sole source owner");
    }
    for (const bool cancel : {false, true}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{4096u, 1u, 1u},
            4096u, 1u, true, true, false});
        CodecRunScope scope(root);
        root.UpdateLimits({4096u, 1u, 1u}, false, ResourceDecisionReason::MechanismCheck);
        bool observedWait = false;
        std::thread controller([&] {
            const auto deadline = ResourceClock::now() + std::chrono::seconds(2);
            ResourceDebugSnapshot snapshot;
            while (ResourceClock::now() < deadline) {
                if (root.TryCopyResourceDebugSnapshot(snapshot) &&
                    snapshot.waiting == ResourceWaitReason::PressureRecovery) {
                    observedWait = !snapshot.heavyPhaseAdmitted && snapshot.admittedBlocks == 0u &&
                        root.StorageCapacity()->Snapshot().reservedBytes == 0u;
                    break;
                }
                std::this_thread::yield();
            }
            if (cancel || !observedWait) { root.RequestStop(); }
            else { root.UpdateLimits({4096u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck); }
        });
        auto phase = WaitForHeavyPhase(root);
        controller.join();
        const bool admitted = phase.has_value();
        phase.reset();
        Require(result, observedWait && admitted == !cancel && scope.Finish(!cancel) == !cancel,
            "topology.phase-gate", "pending heavy admission must own no capacity and wake on recovery or cancellation");
    }
    for (const bool holdPhase : {false, true}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{4096u, 1u, 1u},
            4096u, 1u, true, true, false});
        CodecRunScope scope(root);
        auto slot = holdPhase ? std::optional<SlotLease>{} : root.TryAcquireSlot();
        auto phase = holdPhase ? root.TryAcquireHeavyPhase() : std::optional<HeavyPhaseLease>{};
        const bool held = slot.has_value() || phase.has_value();
        const auto nested = WaitForHeavyPhase(root);
        const auto failure = root.FirstFailure();
        slot.reset();
        phase.reset();
        Require(result, held && !nested && failure &&
            std::string_view(failure->reason.data()) == "phase-not-drained" && !scope.Finish(false),
            "topology.reject-self-wait", "a driver holding an admission must fail instead of waiting for its own retirement");
    }
    {
        bytestore::ByteStoreSession session;
        auto capacity = std::make_shared<resource::ResidentByteBudget>(6u * sizeof(IndexType));
        session.BindStorage(capacity, false);
        auto provider = MakeStoreBackedWritableRemapProvider(3u, session, false, "source");
        const std::array<IndexType, 3u> order{2, 0, 1};
        Require(result, provider && provider->AppendRange(order) && provider->EndWrite(),
            "topology.remap-source", "the shared source provider must be complete");
        std::shared_ptr<bytestore::MemoryStore> owner;
        std::span<const IndexType> values;
        Require(result, topocodec::PrepareTopologyRemapValues(provider.get(), 3u, session, owner, values, nullptr) &&
            std::equal(values.begin(), values.end(), order.begin(), order.end()) &&
            capacity->Snapshot().reservedBytes == 2u * sizeof(order) && owner->WritableBytes().empty(),
            "topology.remap-exact", "the source and complete continuous remap array must own separate exact capacities");
        std::array<IndexType, 3u> copy{};
        bool allocationFreeRead = false;
        {
            RejectAllocationsScope reject;
            allocationFreeRead = provider->ReadRange(0u, copy) && copy == order && !provider->ReadRange(2u, copy);
        }
        Require(result, allocationFreeRead && rejectedAllocationCount == 0u,
            "topology.remap-direct-read", "span reads must fill caller storage and reject invalid ranges without allocation");
        owner.reset();
        values = {};
        Require(result, capacity->Snapshot().reservedBytes == sizeof(order),
            "topology.remap-independent", "consuming the continuous copy must preserve the original provider");
        const IdentityRemapProvider identity(3u);
        Require(result, topocodec::PrepareTopologyRemapValues(&identity, 3u, session, owner, values, nullptr) &&
            !owner && values.empty() && capacity->Snapshot().reservedBytes == sizeof(order),
            "topology.identity-exempt", "identity order must not allocate a full domain array");
        auto occupied = capacity->TryReserve(sizeof(order));
        Require(result, occupied && !topocodec::PrepareTopologyRemapValues(provider.get(), 3u, session,
            owner, values, nullptr) && !owner && values.empty(),
            "topology.remap-required", "a necessary continuous remap cannot choose a different representation after rejection");
    }

    for (std::size_t deniedIndex = 0u; deniedIndex < 5u; ++deniedIndex) {
        const auto limit = deniedIndex * sizeof(IndexType);
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u}, limit, 1u, false, true, false});
        CodecRunScope scope(root);
        CacheResources runtime;
        runtime.BindRun(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        TopoStorageParams topo;
        topo.cellCount = 1u;
        topo.polyhedronFaceVertexCount = 1u;
        topo.polyhedronVertexCount = 1u;
        topo.cellBufferSize = 1u;
        topo.polyhedronStreamLayouts.resize(5u);
        for (std::size_t i = 0u; i < 4u; ++i) { topo.polyhedronStreamLayouts[i].encodedByteLength = 1u; }
        struct UnreadStream {
            std::size_t reads{0u};
            bool ReadBytes(void*, std::size_t, std::string*) { ++reads; return false; }
        } stream;
        DecodedTopologyCache cache;
        std::string error;
        const bool decoded = polyhedron::DecodePolyhedronTopologyStreamsToCache(runtime, session, cache, topo, stream, &error);
        ResourceDebugSnapshot snapshot;
        Require(result, !decoded && stream.reads == 0u && !error.empty() &&
            cache.kind == DecodedTopologyCache::Kind::None && CopyExecutionSnapshot(root, snapshot) &&
            snapshot.failure && snapshot.capacityRejection &&
            snapshot.capacityRejection->requestedBytes == sizeof(IndexType) &&
            snapshot.capacityRejection->reservedBytes == limit && snapshot.capacityRejection->ownerCount == deniedIndex &&
            snapshot.storage.reservedBytes == 0u && !snapshot.heavyPhaseAdmitted &&
            snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u && !scope.Finish(false),
            "polyhedron.index-capacity-denial-" + std::to_string(deniedIndex),
            "each of the five required index allocations must reject before input I/O and roll back all preceding unpublished owners");
    }

    for (const bool truncated : {false, true}) {
        constexpr std::uint64_t limit = 4096u;
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u}, limit, 1u, true, true, false});
        CodecRunScope scope(root);
        CacheResources runtime;
        runtime.BindRun(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        TopoStorageParams topo;
        topo.cellCount = 1u;
        topo.polyhedronFaceVertexCount = 1u;
        topo.polyhedronVertexCount = 1u;
        topo.cellBufferSize = 1u;
        topo.polyhedronStreamLayouts.resize(5u);
        for (std::size_t i = 0u; i < 4u; ++i) { topo.polyhedronStreamLayouts[i].encodedByteLength = 1u; }
        struct Stream {
            DataCodecExecutionResources& root;
            bool truncated;
            std::array<std::uint8_t, 4> bytes{1u, 1u, 1u, 42u};
            std::size_t cursor{0u};
            bool valid{true};
            bool ReadBytes(void* target, std::size_t count, std::string*) {
                ResourceDebugSnapshot snapshot;
                valid &= CopyExecutionSnapshot(root, snapshot) && snapshot.heavyPhaseAdmitted &&
                    snapshot.activeComputeUnits == 1u && count <= kIoWindowBytes;
                if (cursor + count > bytes.size() - (truncated ? 1u : 0u)) { return false; }
                std::memcpy(target, bytes.data() + cursor, count);
                cursor += count;
                return true;
            }
        } stream{root, truncated};
        DecodedTopologyCache cache;
        std::string error;
        std::size_t sampleGroups = 0u;
        bool samplesValid = true;
        const auto driver = std::this_thread::get_id();
        const bool decoded = polyhedron::DecodePolyhedronTopologyStreamsToCache(runtime, session, cache, topo, stream, &error,
            [&](std::span<const BufferCapacitySample> samples) {
                ++sampleGroups;
                ResourceDebugSnapshot snapshot;
                samplesValid &= driver == std::this_thread::get_id() && CopyExecutionSnapshot(root, snapshot) &&
                    snapshot.heavyPhaseAdmitted && snapshot.activeComputeUnits == 0u;
                for (const auto& sample : samples) {
                    if (sample.name == "polyhedron.index_write_window" && sample.capacityBytes) {
                        samplesValid &= *sample.capacityBytes <= kIoWindowBytes;
                    }
                }
            });
        bool valid = stream.valid && decoded != truncated && samplesValid && sampleGroups == (truncated ? 0u : 6u);
        if (decoded) {
            polyhedron::PolyhedronBatchOutput batch;
            batch.capacitySamples.emplace();
            auto phase = WaitForHeavyPhase(root);
            const polyhedron::PolyhedronBatchRange range{0u, 1u, 0u, 0u, 0u, 1u, 1u, 1u};
            const auto memory = polyhedron::MakePolyhedronBatchMemoryLayout(range);
            DecodeBlockWorkspace blockWorkspace(root.Scratch(), memory);
            auto reservation = root.StorageCapacity()->TryReserve(blockWorkspace.TakeReusable());
            if (!reservation) { return result; }
            blockWorkspace.Allocate(*root.StorageCapacity(), std::move(*reservation));
            valid = valid && phase && RunTerminalWork(root, *phase, [&](WorkerContext&) {
                const auto& p = cache.polyhedron;
                return polyhedron::BuildPolyhedronCellBatch(root, range,
                    p.uniqueVertexCounts, p.cellFaceCounts, p.faceVertexCounts,
                    p.cellUniqueVertexIds, p.localFaceVertexIds, batch, blockWorkspace, &error);
            }) && std::ranges::equal(batch.scratch.cellUniqueVertexIds, std::vector<IndexType>{42u}) &&
                std::ranges::equal(batch.scratch.localFaceVertexIds, std::vector<IndexType>{0u}) &&
                std::ranges::equal(batch.scratch.faceVertexOffsets, std::vector<IndexType>({0u, 1u}));
            const auto& offsets = batch.capacitySamples->values[
                static_cast<std::size_t>(polyhedron::PolyhedronBufferSample::FaceVertexOffsets)];
            const auto& counts = batch.capacitySamples->values[
                static_cast<std::size_t>(polyhedron::PolyhedronBufferSample::UniqueCounts)];
            valid &= offsets.capacityBytes == batch.scratch.faceVertexOffsets.capacity() * sizeof(IndexType) &&
                counts.capacityBytes.value_or(0u) >= sizeof(IndexType) && offsets.scopeId != counts.scopeId;
        }
        root.Scratch().ClearFixed();
        scope.Finish(decoded);
        cache.Release();
        ResourceDebugSnapshot snapshot;
        Require(result, valid && CopyExecutionSnapshot(root, snapshot) && !snapshot.heavyPhaseAdmitted &&
            snapshot.activeComputeUnits == 0u && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "polyhedron.decode-phase", "five-stream decode must use prepared owners and terminal compute with failure cleanup");
    }

    {
        class StreamWriter final : public polyhedron::IPolyhedronTopologyStreamWriter {
        public:
            std::array<std::vector<std::uint8_t>, 5> bytes;
            bool valid{true};
            std::size_t uniqueWrites{0u};
            bool WriteStreamBytes(polyhedron::PolyhedronTopologyStreamKind kind,
                std::span<const std::uint8_t> values, std::string*) override {
                valid &= values.size() <= kIoWindowBytes;
                const auto index = static_cast<std::size_t>(kind);
                if (kind == polyhedron::PolyhedronTopologyStreamKind::CellUniqueVertexIds) { ++uniqueWrites; }
                bytes[index].insert(bytes[index].end(), values.begin(), values.end());
                return true;
            }
        } writer;
        polyhedron::PolyhedronTopologyStreamEncoder encoder(writer);
        std::vector<IndexType> ids(kIoWindowBytes, 127u);
        ids.back() = 128u;
        const std::array<IndexType, 1> faces{3u};
        const std::array<IndexType, 3> local{0u, 1u, 2u};
        std::string error;
        const auto index = static_cast<std::size_t>(polyhedron::PolyhedronTopologyStreamKind::CellUniqueVertexIds);
        const bool success = encoder.AppendCell({ids, faces, local}, &error) && encoder.Finish(&error);
        polyhedron::PolyhedronCapacitySamples windowSamples;
        encoder.ObserveCapacities(windowSamples);
        const auto& uniqueWindow = windowSamples.values[
            static_cast<std::size_t>(polyhedron::PolyhedronBufferSample::StreamUniqueIds)];
        Require(result, success && writer.valid && writer.uniqueWrites == 2u &&
            writer.bytes[index].size() == kIoWindowBytes + 1u &&
            writer.bytes[index][kIoWindowBytes - 1u] == 0x80u && writer.bytes[index].back() == 1u &&
            uniqueWindow.capacityBytes == kIoWindowBytes,
            "polyhedron.fixed-stream-window", "a varint crossing the fixed stream window must preserve all bytes");
    }

    TestDataset dataset;
    dataset.name = "bounded_topology";
    dataset.meshType = MeshType::UnstructuredMesh;
    dataset.points.resize(64u * 3u);
    dataset.cellOffsets.push_back(0u);
    for (std::size_t cell = 0u; cell < 61u; ++cell) {
        const auto n = cell == 27u ? 257u : 3u + cell % 7u;
        for (std::size_t i = 0u; i < n; ++i) { dataset.cellConnectivity.push_back((cell + i) % 64u); }
        dataset.cellOffsets.push_back(static_cast<IndexType>(dataset.cellConnectivity.size()));
        dataset.cellTypes.push_back(7u);
        dataset.cellPolynomialOrders.push_back(static_cast<std::uint16_t>(cell % 4u + 1u));
    }
    TestEncodeAdapter adapter(dataset);
    std::vector<IndexType> pointInverse(64u), cellOrder(61u);
    for (std::size_t i = 0u; i < pointInverse.size(); ++i) { pointInverse[i] = 63u - i; }
    for (std::size_t i = 0u; i < cellOrder.size(); ++i) { cellOrder[i] = 60u - i; }
    auto pointSource = *RemapOrderSource::TryComputed(MakeVectorRemapProvider(pointInverse));
    auto cellSource = *RemapOrderSource::TryComputed(MakeVectorRemapProvider(cellOrder));

    for (const bool threaded : {false, true}) {
        for (const std::size_t slots : {1u, 4u}) {
            if (!threaded && slots != 1u) { continue; }
            const auto compute = std::min<std::size_t>(slots, threaded ? 2u : 1u);
            for (const bool externalSpill : {false, true}) {
                constexpr std::uint64_t limit = 4u * 1024u * 1024u;
                DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, compute, slots},
                    limit, threaded ? 2u : 1u, threaded, true, externalSpill});
                CodecRunScope scope(root);
                bytestore::ByteStoreSession session;
                session.BindStorage(root.StorageCapacity(), externalSpill);
                topocodec::TopologyEncodeInput input{
                    .data = {adapter, pointSource, cellSource}, .execution = {root, 7u}, .runtime = {session}};
                std::size_t commits = 0u, peakSlots = 0u;
                bool validOwnership = true;
                bool sampledOrders = true;
                bool sampledGrammar = true;
                const auto driver = std::this_thread::get_id();
                input.context.recordCapacitySamples = [&](std::span<const BufferCapacitySample> samples) {
                    ResourceDebugSnapshot snapshot;
                    if (!CopyExecutionSnapshot(root, snapshot)) { validOwnership = false; return; }
                    peakSlots = std::max(peakSlots, snapshot.admittedBlocks);
                    validOwnership &= snapshot.admittedBlocks > 0u && !snapshot.heavyPhaseAdmitted &&
                        snapshot.activeComputeUnits <= (threaded ? 2u : 1u);
                    validOwnership &= std::this_thread::get_id() == driver &&
                        samples.size() == static_cast<std::size_t>(topocodec::TopologyBufferSample::Count);
                    for (const auto& sample : samples) {
                        if (sample.name == "topology.grammar.main_events" || sample.name == "topology.cell_scratch") {
                            validOwnership &= sample.capacityBytes.value_or(0u) != 0u && sample.sampledAtNanoseconds != 0u;
                        }
                    }
                    const auto& orders = samples[static_cast<std::size_t>(topocodec::TopologyBufferSample::PolynomialOrders)];
                    const auto& encodedOrders = samples[static_cast<std::size_t>(topocodec::TopologyBufferSample::PolynomialOrderBytes)];
                    const auto cellsInBlock = std::min<std::size_t>(7u, 61u - commits * 7u);
                    sampledOrders &= orders.capacityBytes.value_or(0u) >= cellsInBlock * sizeof(std::uint16_t) &&
                        encodedOrders.capacityBytes.value_or(0u) != 0u && orders.scopeId != encodedOrders.scopeId;
                    for (std::size_t i = static_cast<std::size_t>(topocodec::TopologyBufferSample::GrammarOffsets);
                         i <= static_cast<std::size_t>(topocodec::TopologyBufferSample::SeedAuxStream); ++i) {
                        sampledGrammar &= samples[i].capacityBytes.has_value() && samples[i].sampledAtNanoseconds != 0u;
                        for (std::size_t j = static_cast<std::size_t>(topocodec::TopologyBufferSample::GrammarOffsets); j < i; ++j) {
                            sampledGrammar &= samples[i].scopeId != samples[j].scopeId;
                        }
                    }
                    if (++commits == 2u) {
                        validOwnership &= root.UpdateLimits({limit, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
                    } else if (commits == 3u) {
                        validOwnership &= root.UpdateLimits({limit, compute, slots}, true,
                            ResourceDecisionReason::MechanismCheck);
                    }
                };
                topocodec::TopologyEncodeResult output;
                std::string error;
                const bool success = topocodec::EncodeTopologyToTransferCache(input, output, &error);
                ResourceDebugSnapshot final;
                Require(result, success && validOwnership && peakSlots <= slots && commits == 9u &&
                    CopyExecutionSnapshot(root, final) && final.admittedBlocks == 0u && final.activeComputeUnits == 0u &&
                    final.lastRetired == 8u && session.SnapshotStats().storeCount == 1u,
                    "topology.bounded-flow", "every block must hold its slot through commit and retire while limits change");
                Require(result, sampledOrders, "topology.polynomial-capacities",
                    "cell orders and their encoded stream must be distinct sampled arrays inside the block slot");
                Require(result, sampledGrammar, "topology.grammar-array-identities",
                    "all ten grammar arrays must have distinct actual samples including zero-capacity arrays before slot retirement");
                std::uint64_t byteOffset = 0u;
                std::size_t decodedCells = 0u;
                bool replayOk = success && output.transferCache != nullptr;
                for (const auto& layout : output.topo.connectivityLayout.blockLayouts) {
                    if (!replayOk) { break; }
                    const auto total = layout.connectivityByteCount + layout.cellSizeByteCount +
                        layout.cellPolynomialOrderByteCount + layout.cellTypeByteCount;
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total));
                    replayOk = output.transferCache->Read(byteOffset, bytes);
                    const auto view = std::span<const std::uint8_t>(bytes);
                    std::vector<IndexType> sizes, types, connectivity;
                    std::vector<std::uint16_t> orders;
                    replayOk = replayOk && layout.cellOffset == decodedCells && layout.cellPolynomialOrderByteCount != 0u &&
                        topocodec::blockcodec::DecodeUnsignedSequence<IndexType>(view.subspan(layout.connectivityByteCount,
                            layout.cellSizeByteCount), layout.cellCount, sizes) &&
                        topocodec::blockcodec::DecodeUnsignedSequence<std::uint16_t>(view.subspan(
                            layout.connectivityByteCount + layout.cellSizeByteCount, layout.cellPolynomialOrderByteCount),
                            layout.cellCount, orders) &&
                        topocodec::blockcodec::DecodeUnsignedSequence<IndexType>(view.subspan(
                            layout.connectivityByteCount + layout.cellSizeByteCount + layout.cellPolynomialOrderByteCount,
                            layout.cellTypeByteCount),
                            layout.cellCount, types) &&
                        topocodec::blockcodec::DecodeConnectivity(view.first(layout.connectivityByteCount), sizes,
                            64u, layout.cellCount, layout.connectivityCount, 0, connectivity);
                    std::size_t offset = 0u;
                    for (std::size_t i = 0u; replayOk && i < layout.cellCount; ++i) {
                        const auto old = cellOrder[decodedCells + i];
                        const auto begin = dataset.cellOffsets[old], end = dataset.cellOffsets[old + 1u];
                        replayOk = sizes[i] == end - begin && types[i] == dataset.cellTypes[old] &&
                            orders[i] == dataset.cellPolynomialOrders[old];
                        for (auto j = begin; replayOk && j < end; ++j) {
                            replayOk = offset < connectivity.size() && connectivity[offset++] == pointInverse[dataset.cellConnectivity[j]];
                        }
                    }
                    replayOk &= offset == connectivity.size();
                    decodedCells += layout.cellCount;
                    byteOffset += total;
                }
                Require(result, replayOk && decodedCells == dataset.CellCount() && byteOffset == output.topo.binaryCount,
                    "topology.format-roundtrip", "ordered four-stream blocks must decode exact variable connectivity and remapped indices");
                Require(result, scope.Finish(success), "topology.flow-finish", "the complete topology flow must finish cleanly");
                if (success) {
                    for (const bool concurrentObserver : {false, true})
                    for (const bool rejectObserver : {false, true}) {
                        DataCodecExecutionResources decodeRoot(ResolvedResourceConfiguration{{limit, compute, slots},
                            limit, threaded ? 2u : 1u, threaded, true, externalSpill});
                        CodecRunScope decodeScope(decodeRoot);
                        CacheResources runtime;
                        runtime.BindRun(decodeRoot);
                        bytestore::ByteStoreSession decodeSession;
                        decodeSession.BindStorage(decodeRoot.StorageCapacity(), externalSpill);
                        CodecStorageParams storage;
                        storage.topoParams = output.topo;
                        storage.geomParams.elementCount = 64u;
                        DecodedTopologyCache decoded;
                        struct Stream {
                            bytestore::IByteSource& source;
                            DataCodecExecutionResources& root;
                            std::size_t offset{0u};
                            bool valid{true};
                            std::uint64_t Position() const noexcept { return offset; }
                            bool ReadBytes(void* target, std::size_t count, std::string* error) {
                                ResourceDebugSnapshot snapshot;
                                valid &= CopyExecutionSnapshot(root, snapshot) && snapshot.admittedBlocks > 0u &&
                                    snapshot.activeComputeUnits > 0u && count <= kIoWindowBytes;
                                if (!source.Read(offset, {static_cast<std::uint8_t*>(target), count}, error)) { return false; }
                                offset += count;
                                return true;
                            }
                        } stream{*output.transferCache, decodeRoot};
                        class Observer final : public IDecodeTopologyBlockObserver {
                        public:
                            DataCodecExecutionResources* root{};
                            std::thread::id driver{std::this_thread::get_id()};
                            bool reject{false}, valid{true}, concurrent{false};
                            std::mutex mutex;
                            std::size_t blocks{0u}, ended{0u};
                            bool SupportsConcurrentBlocks() const noexcept override { return concurrent; }
                            bool BeginConnectivityTopology(const ConnectivityTopologyDecodeInfo& info, std::string*) override {
                                ResourceDebugSnapshot snapshot;
                                valid &= CopyExecutionSnapshot(*root, snapshot) && snapshot.heavyPhaseAdmitted &&
                                    info.workerCapacity == root->WorkerCapacity();
                                return true;
                            }
                            bool ObserveConnectivityBlock(DecodedConnectivityTopologyBlock block, std::string*) override {
                                std::lock_guard lock(mutex);
                                ResourceDebugSnapshot snapshot;
                                valid &= CopyExecutionSnapshot(*root, snapshot) && snapshot.admittedBlocks > 0u &&
                                    (concurrent ? snapshot.activeComputeUnits > 0u && !block.owner &&
                                        block.workerIndex < root->WorkerCapacity() &&
                                        (!root->Threaded() || driver != std::this_thread::get_id()) :
                                        driver == std::this_thread::get_id() && block.blockIndex == blocks) &&
                                    block.offsets.front() == 0u && block.offsets.back() == block.connectivity.size();
                                ++blocks;
                                return !reject || block.blockIndex != 1u;
                            }
                            bool EndConnectivityTopology(std::string*) override { ++ended; return true; }
                        };
                        auto observer = std::make_shared<Observer>();
                        observer->root = &decodeRoot;
                        observer->reject = rejectObserver;
                        observer->concurrent = concurrentObserver;
                        TopologyDecodeRuntime decodeRuntime{.data = {storage},
                            .cache = {runtime, decodeSession, decoded}, .context = {.topologyBlockObserver = observer}};
                        std::size_t capacityBlocks = 0u;
                        bool capacityValid = true;
                        decodeRuntime.context.recordCapacitySamples = [&](std::span<const BufferCapacitySample> samples) {
                            ++capacityBlocks;
                            capacityValid &= std::this_thread::get_id() == driver;
                            for (const auto& sample : samples) {
                                if (sample.name == "topology.read_copy.connectivity") {
                                    capacityValid &= sample.capacityBytes == 0u;
                                }
                                if (sample.name == "topology.encoded.connectivity" ||
                                    sample.name == "topology.grammar.offsets" || sample.name == "topology.adjusted_offsets" ||
                                    sample.name == "topology.encoded.polynomial_orders" || sample.name == "topology.polynomial_orders") {
                                    capacityValid &= sample.capacityBytes.value_or(0u) != 0u;
                                }
                            }
                        };
                        const auto status = DecodeTopologyFieldToCache(decodeRuntime, stream);
                        Require(result, status.success != rejectObserver && stream.valid && observer->valid &&
                            capacityValid && (rejectObserver ? (concurrentObserver ? capacityBlocks < 9u : capacityBlocks == 1u) : capacityBlocks == 9u) &&
                            observer->ended == 1u && (rejectObserver ? !decoded.complete : decoded.complete),
                            "topology.decode-flow", "topology decode must bound observer work, commit in order and finish the observer once");
                        decodeScope.Finish(status.success);
                        decoded.Release();
                        ResourceDebugSnapshot snapshot;
                        Require(result, CopyExecutionSnapshot(decodeRoot, snapshot) && snapshot.admittedBlocks == 0u &&
                            snapshot.activeComputeUnits == 0u && decodeRoot.StorageCapacity()->Snapshot().reservedBytes == 0u,
                            "topology.decode-drain", "observer rejection and successful decode must drain all records and owners");
                    }
                }
                output = {};
                Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                    "topology.owner-release", "all stage arrays and the consumed spool must release their capacities");
            }
        }
    }
    for (const bool cancel : {false, true}) {
        const auto original = RemapOrderSource::Original();
        const std::uint64_t limit = cancel ? 4096u : 0u;
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 2u}, limit, 1u, true, true, false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        topocodec::TopologyEncodeInput input{
            .data = {adapter, original, original}, .execution = {root, 7u}, .runtime = {session}};
        if (cancel) {
            input.context.recordCapacitySamples = [&](std::span<const BufferCapacitySample>) { root.RequestStop(); };
        }
        topocodec::TopologyEncodeResult output;
        const bool success = topocodec::EncodeTopologyToTransferCache(input, output);
        ResourceDebugSnapshot snapshot;
        Require(result, !success && !output.transferCache && !scope.Finish(success) &&
            CopyExecutionSnapshot(root, snapshot) && snapshot.admittedBlocks == 0u &&
            snapshot.activeComputeUnits == 0u && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "topology.failure-drain", "commit cancellation and spool capacity failure must drain tasks and release all owned data");
    }
    return result;
}

} // DataCodec 测试命名空间

#endif
