#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURESTORAGEAUDIT_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURESTORAGEAUDIT_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Log/Telemetry/TelemetryMemoryTrace.h"
#include "DataCodec/Log/Telemetry/Sinks/TelemetrySessionSink.h"
#include "DataCodec/Log/Report/DataCodecProcessReportJson.h"
#include "DataCodec/Runtime/Record/RunRecordEmitter.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockEncode.h"
#include "DataCodec/Codec/NumericArray/NumericArrayReader.h"

#include <array>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureStorageAudit() {
    TestResult result;
    std::shared_ptr<bytestore::MemoryStore> survivor;
    std::shared_ptr<resource::ResidentByteBudget> capacity;
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, false, true});
        capacity = run.StorageCapacity();
        bytestore::ByteStoreSession session;
        session.BindStorage(capacity, false);
        auto reserved = capacity->TryReserve(8u);
        Require(result, reserved && capacity->Snapshot().reservedBytes == 8u &&
            capacity->AllocatedStorage().liveBytes == 0u && capacity->AllocatedStorage().allocationCount == 0u,
            "audit.reservation-only", "a reservation must not appear as an actual allocated array");
        survivor = reserved ? session.CreateReservedMemoryStore(std::move(*reserved)) : nullptr;
        Require(result, survivor && capacity->AllocatedStorage().liveBytes == 8u &&
            capacity->AllocatedStorage().liveArrayCount == 1u,
            "audit.actual-allocation", "successful exact storage must produce one actual array event");
        if (survivor) {
            auto alias = survivor;
            Require(result, survivor->Resize(9u) && capacity->AllocatedStorage().liveBytes == 12u &&
                capacity->AllocatedStorage().peakLiveBytes == 20u &&
                capacity->AllocatedStorage().allocationCount == 2u && capacity->AllocatedStorage().liveArrayCount == 1u,
                "audit.growth-overlap", "the old eight-byte array and new twelve-byte array must coexist in the event peak");
            bool failed = false;
            {
                RejectAllocationsScope reject;
                failed = !survivor->Resize(17u);
            }
            const auto afterFailure = capacity->AllocatedStorage();
            Require(result, failed && afterFailure.liveBytes == 12u && afterFailure.peakLiveBytes == 20u &&
                afterFailure.allocationCount == 2u && afterFailure.failedAllocationCount == 1u &&
                capacity->Snapshot().reservedBytes == 12u,
                "audit.failed-allocation", "real allocation failure must return the new reservation without fake allocation/free events");
            session.ReleaseAll();
            alias.reset();
            Require(result, capacity->AllocatedStorage().liveBytes == 12u && capacity->AllocatedStorage().liveArrayCount == 1u,
                "audit.shared-owner", "session cleanup and shared consumers must not double-count or destroy the array");
        }
        TelemetrySessionSink sink(kRunLifecycleRecordMask | RunRecordKind::ResourceUsage, 0u,
            TelemetrySessionDetail::ProcessSummary);
        RunRecordEmitter records(&run);
        records.Reset(RunRecordInfo{.runKind = TelemetryRunKind::Encode}, &sink);
        records.BeginRun();
        RecordRootCapacityAudit(records, run);
        RecordRootCapacityAudit(records, run);
        records.EndRun(RunEndRecord{.success = true});
        const auto sessions = sink.SnapshotCompletedSessions();
        Require(result, sessions.size() == 1u && sessions.front().stages.size() == 4u &&
            sessions.front().stages[0].resource.trackedCapacityBytes == 12u &&
            sessions.front().stages[0].resource.eventPeakCapacityBytes == 20u &&
            !sessions.front().stages[1].resource.eventPeakCapacityBytes,
            "audit.coverage-fields", "root storage event peaks and retained scratch samples must carry distinct coverage");
        const auto nodes = BuildTelemetryProcessNodes(sessions, TelemetryRunKind::Encode);
        bool exact = false;
        if (!nodes.empty()) {
            for (const auto& detail : nodes.front().details) {
                if (detail.name == "tracked.storage.arrays.capacity_bytes") {
                    exact = std::get_if<std::uint64_t>(&detail.value) && std::get<std::uint64_t>(detail.value) == 12u;
                }
            }
        }
        Require(result, exact, "audit.snapshot-no-sum", "repeated observations must retain one capacity value instead of adding snapshots");
    }
    Require(result, capacity->AllocatedStorage().liveBytes == 12u,
        "audit.root-lifetime", "a surviving result must remain tracked after the execution root is destroyed");
    {
        RejectAllocationsScope reject;
        survivor.reset();
    }
    Require(result, capacity->AllocatedStorage().liveBytes == 0u && capacity->AllocatedStorage().liveArrayCount == 0u &&
        capacity->Snapshot().reservedBytes == 0u && rejectedAllocationCount == 0u,
        "audit.final-owner", "final result destruction must release actual storage and reservation without allocation");
    {
        std::array<BufferCapacitySample, 2> samples{{
            BufferCapacitySample{"audit.explicit.vector"},
            BufferCapacitySample{"audit.unknown.vector"},
        }};
        std::vector<std::uint64_t> values;
        values.reserve(37u);
        values.resize(2u);
        const auto actualCapacity = values.capacity() * sizeof(std::uint64_t);
        {
            RejectAllocationsScope reject;
            samples[0].Observe(values);
        }
        Require(result, samples[0].capacityBytes == actualCapacity &&
            !samples[1].capacityBytes && samples[0].scopeId != samples[1].scopeId &&
            rejectedAllocationCount == 0u,
            "audit.explicit-capacity", "a sample must use actual capacity and keep unobserved arrays unknown without allocation");
        values.clear();
        samples[0].Observe(values);
        Require(result, samples[0].capacityBytes == actualCapacity,
            "audit.clear-retains", "clearing logical elements must not report the retained array as freed");
        std::vector<std::uint64_t>{}.swap(values);
        samples[0].Observe(values);
        TelemetrySessionSink sink(kRunLifecycleRecordMask | RunRecordKind::ResourceUsage, 0u,
            TelemetrySessionDetail::ProcessSummary);
        RunRecordEmitter records;
        records.Reset(RunRecordInfo{.runKind = TelemetryRunKind::Encode}, &sink);
        records.BeginRun();
        RecordBufferCapacitySamples(records, samples);
        RecordBufferCapacitySamples(records, samples);
        records.EndRun(RunEndRecord{.success = true});
        const auto sessions = sink.SnapshotCompletedSessions();
        Require(result, sessions.size() == 1u && sessions.front().stages.size() == 2u &&
            sessions.front().stages.front().resource.trackedCapacityBytes == 0u &&
            sessions.front().stages.front().resource.sampledPeakCapacityBytes == actualCapacity &&
            !sessions.front().stages.front().resource.eventPeakCapacityBytes &&
            sessions.front().stages.front().resource.capacityCoverage == TelemetryCapacityCoverage::SampledBuffer,
            "audit.sample-retirement", "retired capacity must report zero and retain only its sampled peak while unknown arrays stay omitted");
    }
    {
        CodecStorageParams params;
        params.geomParams.blockLayouts.reserve(7u);
        params.geomParams.blockLayouts.resize(1u);
        params.geomParams.blockLayouts.front().componentLayouts.resize(32u);
        params.topoParams.connectivityLayout.blockLayouts.reserve(3u);
        params.attrParams.resize(2u);
        params.attrParams[0].blockLayouts.reserve(5u);
        params.attrParams[1].blockLayouts.reserve(9u);
        const auto geometryCapacity = params.geomParams.blockLayouts.capacity() * sizeof(NumericArrayBlockLayoutParams);
        const auto attributeCapacity = (params.attrParams[0].blockLayouts.capacity() +
            params.attrParams[1].blockLayouts.capacity()) * sizeof(NumericArrayBlockLayoutParams);
        TelemetrySessionSink sink(kRunLifecycleRecordMask | RunRecordKind::ResourceUsage, 0u,
            TelemetrySessionDetail::ProcessSummary);
        RunRecordEmitter records;
        records.Reset(RunRecordInfo{.runKind = TelemetryRunKind::Decode}, &sink);
        records.BeginRun();
        RecordCodecStorageParamsCapacity(records, params);
        records.EndRun(RunEndRecord{.success = true});
        const auto sessions = sink.SnapshotCompletedSessions();
        Require(result, sessions.size() == 1u && sessions.front().stages.size() == 4u &&
            sessions.front().stages[0].resource.trackedCapacityBytes == geometryCapacity &&
            sessions.front().stages[2].resource.trackedCapacityBytes == params.attrParams.capacity() * sizeof(AttrStorageParams) &&
            sessions.front().stages[3].resource.trackedCapacityBytes == attributeCapacity &&
            !sessions.front().stages[0].resource.eventPeakCapacityBytes,
            "audit.metadata-outer-arrays", "metadata samples must count actual outer capacities and keep nested allocations and event peaks unclaimed");
    }
    {
        numericarray::NumericArrayEncodedBytes encoded;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(61u);
        bytes.resize(3u);
        const auto capacityBytes = bytes.capacity();
        encoded.TakeVector(std::move(bytes));
        Require(result, encoded.OwnedCapacityBytes() == capacityBytes && encoded.Size() == 3u,
            "audit.numeric-owned-payload", "owned encoded bytes must expose vector capacity independently of payload length");
        numericarray::PressioDataHandle library(pressio_data_new_empty(pressio_byte_dtype, 0u, nullptr));
        Require(result, library != nullptr, "audit.numeric-library-create", "the library handle must be available for the exemption test");
        if (library) {
            encoded.TakePressioData(std::move(library), {});
            Require(result, !encoded.OwnedCapacityBytes(), "audit.numeric-library-exempt",
                "library-owned payload capacity must remain unknown even for a zero-length result");
        }
        std::vector<std::uint32_t> values(36u);
        std::iota(values.begin(), values.end(), 0u);
        auto params = numericarray::MakeNumericArrayBlockParams(
            numericarray::MakeNumericArrayLayout(DataType::UInt32, sizeof(std::uint32_t), 12u, 3u));
        numericarray::NumericArrayBlockCapacitySamples samples;
        params.capacitySamples = &samples;
        ScratchByteBufferPool scratch(0u);
        std::vector<std::uint8_t> payload;
        std::vector<NumericArrayComponentLayoutParams> layouts;
        NumericArrayBytesCodec codec;
        std::string error;
        const bool success = numericarray::ResolveEncodedNumericArrayBlockBytes(params, {}, 12u,
            {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(std::uint32_t)},
            payload, codec, &error, &scratch, &layouts);
        const auto stats = scratch.SnapshotStats();
        Require(result, success && stats.allocationCount == 1u && stats.activeBlockCount == 0u &&
            samples.values[static_cast<std::size_t>(numericarray::NumericBufferSample::ComponentRaw)].capacityBytes.value_or(0u) >= 12u * sizeof(std::uint32_t) &&
            samples.values[static_cast<std::size_t>(numericarray::NumericBufferSample::Output)].capacityBytes == payload.capacity(),
            "audit.serial-component-scratch", error.empty() ? "all components must reuse one raw scratch array and sample actual output capacity" : error);
        numericarray::NumericArrayBlockCapacitySamples decodeSamples;
        params.capacitySamples = &decodeSamples;
        std::vector<std::uint8_t> decoded;
        const bool decodedOk = success && numericarray::ResolveDecodedNumericArrayBlockBytes(
            params, {}, 12u, codec, layouts, payload, decoded, &error);
        Require(result, decodedOk && decoded.size() == values.size() * sizeof(std::uint32_t) &&
            std::memcmp(decoded.data(), values.data(), decoded.size()) == 0 &&
            decodeSamples.values[static_cast<std::size_t>(numericarray::NumericBufferSample::DecodedComponent)].capacityBytes.value_or(0u) >= 12u * sizeof(std::uint32_t) &&
            !decodeSamples.values[static_cast<std::size_t>(numericarray::NumericBufferSample::ReferencePrimary)].capacityBytes,
            "audit.numeric-decode-capacity", error.empty() ? "component decode samples must preserve exact integer reconstruction and leave unused reference storage unknown" : error);
    }
    {
        const std::array<std::uint32_t, 9> values{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        auto order = MakeVectorRemapProvider(std::vector<IndexType>{2u, 1u, 0u});
        numericarray::NumericArraySource source;
        source.values.scalarType = ScalarType::UInt32;
        source.values.layout = ArrayLayout::CompactAOS;
        source.values.origin = ViewBufferOrigin::Borrowed;
        source.values.data = values.data();
        source.values.tupleCount = 3u;
        source.values.componentCount = 3u;
        source.layout = numericarray::MakeNumericArrayLayout(DataType::UInt32, sizeof(std::uint32_t), 3u, 3u);
        source.orderProvider = order.get();
        numericarray::NumericArrayReader reader;
        BufferCapacitySample remapSample{"reader.order"};
        std::vector<std::uint8_t> output;
        std::string error;
        const std::array<std::uint32_t, 6> expected{6u, 7u, 8u, 3u, 4u, 5u};
        const bool copied = numericarray::BuildNumericArrayReader(source, reader, &error) &&
            reader.ReadElements(0u, 2u, output, &error, &remapSample);
        Require(result, copied && output.size() == sizeof(expected) &&
            std::memcmp(output.data(), expected.data(), sizeof(expected)) == 0 &&
            remapSample.capacityBytes.value_or(0u) >= 2u * sizeof(IndexType),
            "audit.reader-order", "remapped reads must preserve tuple order and sample only the local index array");
        source.orderProvider = nullptr;
        BufferCapacitySample directSample{"reader.direct_order"};
        const bool direct = numericarray::BuildNumericArrayReader(source, reader, &error) &&
            reader.ReadElements(0u, 2u, output, &error, &directSample);
        Require(result, direct && directSample.capacityBytes == 0u &&
            std::memcmp(output.data(), values.data(), 6u * sizeof(std::uint32_t)) == 0,
            "audit.reader-borrowed", "direct input views must not be counted as an owned remap array");
    }
    {
        TelemetrySession session;
        session.runId = 7001u;
        session.runKind = TelemetryRunKind::Encode;
        session.success = true;
        session.AddStageRecord({.name = "topology.output", .category = TelemetryStageCategory::Topology,
            .resource = MakeLogicalTelemetryResourceUsage(8000u)});
        auto nodes = BuildTelemetryProcessNodes({session}, TelemetryRunKind::Encode);
        Require(result, nodes.size() == 1u && !nodes.front().memory.valid,
            "audit.logical-not-physical", "logical output bytes must not become a physical memory observation");
        const auto observation = MakeTelemetryMemoryResourceUsage(100u, 120u, 180u);
        session.AddStageRecord({.name = "memory.run", .resource = observation});
        nodes = BuildTelemetryProcessNodes({session}, TelemetryRunKind::Encode);
        DataCodecProcessReport report;
        report.processes = std::move(nodes);
        CompleteDataCodecProcessReportMemory(report);
        const auto json = SerializeDataCodecProcessReportJson(report);
        Require(result, report.memory.sampledPeakWorkingSetBytes == 180u &&
            observation.workingSetBytes == 120u && json.find("sampledPeakWorkingSetBytes") != std::string::npos &&
            json.find("stage-boundary-snapshots") != std::string::npos &&
            json.find("\"scope\": \"process\"") != std::string::npos,
            "audit.process-sample-scope", "RSS samples must identify process scope and a sampled maximum independently of owned capacity");
    }
    return result;
}

}

#endif
