#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURENUMERICEXECUTION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURENUMERICEXECUTION_H

#include "DataCodec/Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"

#include <cmath>
#include <cstring>
#include <type_traits>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureNumericExecution() {
    TestResult result;
    const auto checkType = [&]<typename Value>() {
        constexpr auto count = 2u * numericarray::kSpatialBlockElementCount + 3u;
        constexpr std::size_t components = 3u;
        constexpr std::uint64_t limit = 32u * 1024u * 1024u;
        std::vector<Value> values(static_cast<std::size_t>(count) * components);
        for (std::size_t i = 0u; i < values.size(); ++i) {
            values[i] = static_cast<Value>(std::sin(static_cast<double>(i) * 0.017) + (i % components));
        }
        NumericArrayStorageParams meta;
        meta.dataType = std::is_same_v<Value, float> ? DataType::Float32 : DataType::Float64;
        meta.elementCount = count;
        meta.dimension = components;
        NumericArrayControlParams control;
        control.regionControl.defaultPrecision.compressor.options["pressio:abs"] = 0.001;
        numericarray::NumericArrayBlockParams params;
        Require(result, numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params),
            "numeric.params", "the typed multi-component layout must be valid");
        numericarray::ApplyNumericArrayControlParams(params, control);
        for (const bool threaded : {false, true}) {
            for (const bool externalSpill : {false, true}) {
                const std::size_t compute = threaded ? 2u : 1u, slots = threaded ? 4u : 1u;
                DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, compute, slots},
                    limit, compute, threaded, true, externalSpill});
                CodecRunScope scope(root);
                bytestore::ByteStoreSession session;
                session.BindStorage(root.StorageCapacity(), externalSpill);
                struct InputState {
                    const std::vector<Value>& values;
                    DataCodecExecutionResources& root;
                    mutable std::size_t readBlocks{0u};
                    mutable bool valid{true};
                } state{values, root};
                numericarray::NumericArraySource source{
                    .values = NumericArrayView{
                        .scalarType = std::is_same_v<Value, float> ? ScalarType::Float32 : ScalarType::Float64,
                        .layout = ArrayLayout::GetterOnly, .origin = ViewBufferOrigin::Borrowed,
                        .tupleCount = count, .componentCount = components, .userData = &state,
                        .getTupleBytes = [](const void* data, std::size_t index, void* output, std::string*) {
                            const auto& input = *static_cast<const InputState*>(data);
                            if (index % numericarray::kSpatialBlockElementCount == 0u) {
                                ResourceDebugSnapshot snapshot;
                                input.valid &= CopyExecutionSnapshot(input.root, snapshot) &&
                                    snapshot.admittedBlocks > 0u && !snapshot.heavyPhaseAdmitted;
                                if (++input.readBlocks == 2u) {
                                    input.valid &= input.root.UpdateLimits({limit, 1u, 1u}, true,
                                        ResourceDecisionReason::MechanismCheck);
                                }
                            }
                            std::memcpy(output, input.values.data() + index * components, components * sizeof(Value));
                            return true;
                        },
                    },
                    .layout = numericarray::MakeNumericArrayLayout(meta.dataType, sizeof(Value), count, components),
                };
                numericarray::NumericArrayReader reader;
                Require(result, numericarray::BuildNumericArrayReader(source, reader, nullptr),
                    "numeric.reader", "getter input must be accepted without full materialization");
                std::size_t commits = 0u;
                std::size_t capacityExports = 0u;
                const auto driver = std::this_thread::get_id();
                encodeimpl::NumericArrayTransferCacheRuntime timing;
                timing.recordCapacitySamples = [&](const std::span<const BufferCapacitySample> samples) {
                    ResourceDebugSnapshot snapshot;
                    state.valid &= std::this_thread::get_id() == driver && CopyExecutionSnapshot(root, snapshot) &&
                        snapshot.admittedBlocks > 0u && samples.size() == static_cast<std::size_t>(numericarray::NumericBufferSample::Count) &&
                        samples[static_cast<std::size_t>(numericarray::NumericBufferSample::Raw)].capacityBytes.value_or(0u) > 0u &&
                        samples[static_cast<std::size_t>(numericarray::NumericBufferSample::ComponentRaw)].capacityBytes.value_or(0u) > 0u &&
                        samples[static_cast<std::size_t>(numericarray::NumericBufferSample::Output)].capacityBytes.value_or(0u) > 0u &&
                        samples[static_cast<std::size_t>(numericarray::NumericBufferSample::ReaderOrder)].capacityBytes == 0u;
                    ++capacityExports;
                };
                timing.recordFloatingPointEncodeDuration = [&](std::chrono::nanoseconds) {
                    ResourceDebugSnapshot snapshot;
                    state.valid &= std::this_thread::get_id() == driver && CopyExecutionSnapshot(root, snapshot) &&
                        snapshot.admittedBlocks > 0u;
                    if (++commits == 2u) {
                        state.valid &= root.UpdateLimits({limit, compute, slots}, true, ResourceDecisionReason::MechanismCheck);
                    }
                };
                encodeimpl::NumericArrayTransferCacheResult encoded;
                std::string error;
                const bool success = encodeimpl::BuildNumericArrayTransferCache(params, reader, root,
                    encoded, session, &error, "numeric_execution", &timing);
                ResourceDebugSnapshot snapshot;
                Require(result, success && state.valid && state.readBlocks == 3u && commits == 3u && capacityExports == 3u &&
                    encoded.blockLayouts.size() == 3u && encoded.stats.encodeBlockCount == 3u &&
                    CopyExecutionSnapshot(root, snapshot) && snapshot.admittedBlocks == 0u &&
                    snapshot.activeComputeUnits == 0u && snapshot.lastRetired == 2u &&
                    root.Scratch().SnapshotStats().activeBlockCount == 0u,
                    "numeric.bounded-flow", error.empty() ? "fixed blocks must retire in order through live C/S updates" : error);
                std::uint64_t byteOffset = 0u;
                std::size_t tupleOffset = 0u;
                bool replay = success;
                for (const auto& layout : encoded.blockLayouts) {
                    if (!replay) { break; }
                    std::vector<std::uint8_t> bytes(layout.encodedByteLength), decoded;
                    replay = layout.elementOffset == tupleOffset &&
                        layout.elementCount == std::min<std::size_t>(numericarray::kSpatialBlockElementCount, count - tupleOffset) &&
                        encoded.transferCache->Read(byteOffset, bytes) &&
                        numericarray::ResolveDecodedNumericArrayBlockBytes(params, layout.backgroundCompressor,
                            layout.elementCount, layout.bytesCodec, layout.componentLayouts, bytes, decoded, &error);
                    replay &= decoded.size() == static_cast<std::size_t>(layout.elementCount) * components * sizeof(Value);
                    for (std::size_t i = 0u; replay && i < decoded.size() / sizeof(Value); ++i) {
                        Value value;
                        std::memcpy(&value, decoded.data() + i * sizeof(Value), sizeof(Value));
                        replay = std::abs(static_cast<double>(value - values[tupleOffset * components + i])) <= 0.00101;
                    }
                    byteOffset += bytes.size();
                    tupleOffset += layout.elementCount;
                }
                Require(result, replay && tupleOffset == count && byteOffset == encoded.stats.fragmentBytes,
                    "numeric.fixed-roundtrip", "full blocks and tail must preserve all typed components within the requested error");
                Require(result, scope.Finish(success), "numeric.finish", "the numeric flow must finish with no live work");
                encoded = {};
                Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                    "numeric.output-release", "consuming the encoded output must release its owned capacity");
            }
        }
    };
    checkType.template operator()<float>();
    checkType.template operator()<double>();
    for (const unsigned fault : {0u, 1u, 2u, 3u, 4u}) {
        const std::uint64_t limit = fault == 2u ? 0u : 1024u * 1024u;
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u},
            limit, 1u, true, true, false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        struct FaultInput { DataCodecExecutionResources& root; unsigned kind; } input{root, fault};
        const std::size_t count = fault == 4u ? 0u : 8u;
        NumericArrayStorageParams meta;
        meta.dataType = DataType::Float32;
        meta.elementCount = count;
        meta.dimension = 1u;
        NumericArrayControlParams control;
        control.regionControl.defaultPrecision.compressor.options["pressio:abs"] = 0.001;
        numericarray::NumericArrayBlockParams params;
        numericarray::MakeNumericArrayBlockParamsFromMeta(meta, params);
        numericarray::ApplyNumericArrayControlParams(params, control);
        numericarray::NumericArraySource source{
            .values = NumericArrayView{
                .scalarType = ScalarType::Float32, .layout = ArrayLayout::GetterOnly,
                .origin = ViewBufferOrigin::Borrowed, .tupleCount = count, .componentCount = 1u,
                .userData = &input,
                .getTupleBytes = [](const void* data, std::size_t index, void* output, std::string* error) {
                    const auto& input = *static_cast<const FaultInput*>(data);
                    if (input.kind == 0u) { return validation::AssignError(error, "injected numeric read failure"); }
                    if (input.kind == 1u) { input.root.RequestStop(); }
                    const float value = static_cast<float>(index);
                    std::memcpy(output, &value, sizeof(value));
                    return true;
                },
            },
            .layout = numericarray::MakeNumericArrayLayout(DataType::Float32, sizeof(float), count, 1u),
        };
        numericarray::NumericArrayReader reader;
        Require(result, numericarray::BuildNumericArrayReader(source, reader, nullptr),
            "numeric.failure-reader", "the failure fixture must provide a valid numeric source");
        encodeimpl::NumericArrayTransferCacheRuntime runtime;
        if (fault == 3u) {
            runtime.recordFloatingPointEncodeDuration = [](std::chrono::nanoseconds) { throw std::bad_alloc{}; };
        }
        encodeimpl::NumericArrayTransferCacheResult encoded;
        const bool success = encodeimpl::BuildNumericArrayTransferCache(params, reader, root,
            encoded, session, nullptr, "numeric_failure", &runtime);
        ResourceDebugSnapshot snapshot;
        Require(result, success == (fault == 4u) &&
            (fault == 4u ? encoded.transferCache && encoded.transferCache->ByteSizeHint() == 0u : !encoded.transferCache) &&
            scope.Finish(success) == success && CopyExecutionSnapshot(root, snapshot) &&
            snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u && !snapshot.heavyPhaseAdmitted &&
            root.Scratch().SnapshotStats().activeBlockCount == 0u && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "numeric.failure-drain", "read failure, cancellation, capacity rejection, commit exception and empty fields must clean up");
    }
    return result;
}

} // DataCodec 测试命名空间

#endif
