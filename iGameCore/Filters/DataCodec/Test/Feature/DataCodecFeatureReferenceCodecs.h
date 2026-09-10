#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREREFERENCECODECS_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREREFERENCECODECS_H

#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Params/ReferenceControlParams.h"
#include "DataCodec/Runtime/Cache/TransferCache/ReferenceTransferCacheBuilder.h"
#include "DataCodec/Test/Common/ReferenceCodecTestHarness.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace datacodec::test {

template<typename TValue>
inline std::vector<TValue> MakeReferencePrecisionValues(
    const std::size_t tupleCount,
    const std::size_t componentCount,
    const bool currentValues) {
    std::vector<TValue> values(tupleCount * componentCount, TValue{});
    for (std::size_t tupleIndex = 0u; tupleIndex < tupleCount; ++tupleIndex) {
        const auto position = static_cast<double>(tupleIndex);
        for (std::size_t componentIndex = 0u;
             componentIndex < componentCount;
             ++componentIndex) {
            const auto scale = componentIndex == 0u
                ? 120.0
                : (componentIndex == 1u ? 1.5 : 0.015);
            const auto reference = scale * (
                std::sin(position * (0.009 + componentIndex * 0.002)) +
                0.25 * std::cos(position * (0.003 + componentIndex * 0.001)));
            const auto value = currentValues
                ? (1.15 + componentIndex * 0.03) * reference +
                    scale * 0.07 +
                    scale * 0.004 * std::sin(position * 0.017)
                : reference;
            values[tupleIndex * componentCount + componentIndex] =
                static_cast<TValue>(value);
        }
    }
    return values;
}

template<typename TValue>
inline bool CheckReferencePrecision(
    TestResult& result,
    const std::vector<TValue>& expected,
    const std::vector<std::uint8_t>& decodedBytes,
    const std::size_t componentCount,
    const CompressorConfig& compressor,
    const std::string& caseName) {
    if (!Require(
            result,
            decodedBytes.size() == expected.size() * sizeof(TValue),
            caseName + ".decodeSize",
            "reference decoded byte size is invalid")) {
        return false;
    }
    const auto* decoded = reinterpret_cast<const TValue*>(decodedBytes.data());
    std::vector<double> componentMinimum(
        componentCount,
        std::numeric_limits<double>::infinity());
    std::vector<double> componentMaximum(
        componentCount,
        -std::numeric_limits<double>::infinity());
    for (std::size_t valueIndex = 0u; valueIndex < expected.size(); ++valueIndex) {
        const auto componentIndex = valueIndex % componentCount;
        const auto value = static_cast<double>(expected[valueIndex]);
        componentMinimum[componentIndex] = std::min(componentMinimum[componentIndex], value);
        componentMaximum[componentIndex] = std::max(componentMaximum[componentIndex], value);
    }
    const auto absoluteIterator = compressor.options.find("pressio:abs");
    const auto relativeIterator = compressor.options.find("pressio:rel");
    std::vector<double> maximumErrors(componentCount, 0.0);
    bool precise = true;
    for (std::size_t valueIndex = 0u; valueIndex < expected.size(); ++valueIndex) {
        const auto componentIndex = valueIndex % componentCount;
        const auto absoluteError = std::abs(
            static_cast<double>(decoded[valueIndex]) -
            static_cast<double>(expected[valueIndex]));
        maximumErrors[componentIndex] = std::max(
            maximumErrors[componentIndex],
            absoluteError);
        double errorBound = std::numeric_limits<double>::infinity();
        if (absoluteIterator != compressor.options.end()) {
            errorBound = absoluteIterator->second;
        }
        if (relativeIterator != compressor.options.end()) {
            errorBound = std::min(
                errorBound,
                relativeIterator->second *
                    (componentMaximum[componentIndex] - componentMinimum[componentIndex]));
        }
        precise = precise && absoluteError <= std::nextafter(
            errorBound,
            std::numeric_limits<double>::infinity());
    }
    for (std::size_t componentIndex = 0u;
         componentIndex < componentCount;
         ++componentIndex) {
        result.AddDiagnostic(
            caseName + ".component" + std::to_string(componentIndex) +
            ".max_abs_error=" + std::to_string(maximumErrors[componentIndex]));
    }
    return Require(
        result,
        precise,
        caseName + ".precision",
        "reference codec exceeded a component precision bound");
}

template<typename TValue>
inline bool RunReferenceCodecPrecisionCase(
    TestResult& result,
    const NumericArrayReferenceCodecId codecId,
    const CompressorConfig& compressor,
    const std::size_t tupleCount,
    const std::size_t componentCount,
    const std::string& caseName) {
    const auto reference = MakeReferencePrecisionValues<TValue>(
        tupleCount,
        componentCount,
        false);
    const auto current = MakeReferencePrecisionValues<TValue>(
        tupleCount,
        componentCount,
        true);
    ScratchByteBufferPool scratchBytePool;
    NumericArrayReferenceEncodedBlock encoded;
    numericarray::NumericArrayBlockCapacitySamples encodeSamples;
    numericarray::NumericArrayBlockCapacitySamples decodeSamples;
    std::vector<std::uint8_t> decodedBytes;
    std::string error;
    const bool encodedOk = EncodeDecodeReferenceTestBlock(
        codecId,
        compressor,
        scratchBytePool,
        current,
        reference,
        componentCount,
        NumericArrayReferenceKind::IntraArray,
        encoded,
        decodedBytes,
        &error, &encodeSamples, &decodeSamples);
    if (!Require(
            result,
            encodedOk,
            caseName + ".roundTrip",
            error.empty() ? "reference codec round trip failed" : error)) {
        return false;
    }
    if constexpr (std::is_floating_point_v<TValue>) {
        if (codecId == NumericArrayReferenceCodecId::Wavelet) {
            using Sample = numericarray::NumericBufferSample;
            const auto& lowDelta = encodeSamples.values[static_cast<std::size_t>(Sample::WaveletLowDelta)];
            const auto& referenceComponent = decodeSamples.values[static_cast<std::size_t>(Sample::WaveletReferenceComponent)];
            const auto& reconstructed = decodeSamples.values[static_cast<std::size_t>(Sample::WaveletReconstructed)];
            Require(result, lowDelta.capacityBytes.value_or(0u) >= ((tupleCount + 1u) / 2u) * sizeof(double) &&
                referenceComponent.capacityBytes.value_or(0u) >= tupleCount * sizeof(double) &&
                reconstructed.capacityBytes.value_or(0u) >= tupleCount * sizeof(double) &&
                referenceComponent.scopeId != reconstructed.scopeId,
                caseName + ".waveletCapacitySamples", "floating Wavelet arrays must carry independent actual capacity samples alongside precision checks");
        } else {
            using Sample = numericarray::NumericBufferSample;
            const auto& prepared = encodeSamples.values[static_cast<std::size_t>(Sample::ReferencePreparedDelta)];
            const auto& candidate = encodeSamples.values[static_cast<std::size_t>(Sample::ReferenceCandidate)];
            Require(result, prepared.capacityBytes.value_or(0u) >= current.size() * sizeof(TValue) &&
                candidate.capacityBytes.value_or(0u) >= ReferenceEncodedPayloadBytes(encoded) &&
                prepared.scopeId != candidate.scopeId,
                caseName + ".referenceCapacitySamples", "prepared residual and encoded candidate must retain independent actual capacities");
        }
    }
    return CheckReferencePrecision(
        result,
        current,
        decodedBytes,
        componentCount,
        compressor,
        caseName);
}

inline bool RunReferenceCodecDefaultCase(TestResult& result) {
    const IntraFieldReferenceControlParams intra;
    const TemporalFieldReferenceControlParams temporal;
    return Require(
               result,
               intra.codec == IntraFieldReferenceCodec::Affine,
               "referenceCodec.defaults.intra",
               "intra-field reference default is not affine") &&
        Require(
            result,
            temporal.codec == TemporalFieldReferenceCodec::Predictor,
            "referenceCodec.defaults.temporal",
            "temporal reference default is not predictor") &&
        Require(
            result,
            !temporal.predictor.enableLocalWindowSearch,
            "referenceCodec.defaults.offsetSearch",
            "temporal predictor offset search is enabled by default");
}

inline bool RunReferenceCodecNonFiniteCase(TestResult& result) {
    auto reference = MakeReferencePrecisionValues<float>(33u, 1u, false);
    auto current = MakeReferencePrecisionValues<float>(33u, 1u, true);
    current[7] = std::numeric_limits<float>::quiet_NaN();
    bool rejectedAll = true;
    for (const auto codecId : {
             NumericArrayReferenceCodecId::Predictor,
             NumericArrayReferenceCodecId::Affine,
             NumericArrayReferenceCodecId::Wavelet}) {
        ScratchByteBufferPool scratchBytePool;
        NumericArrayReferenceEncodedBlock encoded;
        std::vector<std::uint8_t> decodedBytes;
        std::string error;
        rejectedAll = rejectedAll && !EncodeDecodeReferenceTestBlock(
            codecId,
            MakeRelativeErrorNumericArrayCompressor(1.0e-4),
            scratchBytePool,
            current,
            reference,
            1u,
            NumericArrayReferenceKind::IntraArray,
            encoded,
            decodedBytes,
            &error);
    }
    return Require(
        result,
        rejectedAll,
        "referenceCodec.nonFinite",
        "a reference codec accepted a non-finite input value");
}

inline bool RunReferenceCodecTypeDispatchCase(TestResult& result) {
    std::vector<std::int32_t> reference(33u, 0);
    std::vector<std::int32_t> current(33u, 0);
    for (std::size_t index = 0u; index < current.size(); ++index) {
        reference[index] = static_cast<std::int32_t>(index) * 7 - 80;
        current[index] = reference[index] + static_cast<std::int32_t>(index % 5u);
    }
    bool rejectedAll = true;
    for (const auto codecId : {
             NumericArrayReferenceCodecId::Predictor,
             NumericArrayReferenceCodecId::Affine}) {
        ScratchByteBufferPool scratchBytePool;
        NumericArrayReferenceEncodedBlock encoded;
        std::vector<std::uint8_t> decodedBytes;
        std::string error;
        rejectedAll = rejectedAll && !EncodeDecodeReferenceTestBlock(
            codecId,
            MakeLosslessNumericArrayCompressor(),
            scratchBytePool,
            current,
            reference,
            1u,
            NumericArrayReferenceKind::IntraArray,
            encoded,
            decodedBytes,
            &error);
    }
    return Require(
        result,
        rejectedAll,
        "referenceCodec.typeDispatch",
        "a floating-point reference codec accepted int32 data");
}

inline bool RunIntegerWaveletCase(TestResult& result) {
    constexpr std::size_t kTupleCount = 4097u;
    constexpr std::size_t kComponentCount = 2u;
    std::vector<std::int32_t> reference(kTupleCount * kComponentCount, 0);
    std::vector<std::int32_t> current(reference.size(), 0);
    for (std::size_t valueIndex = 0u; valueIndex < current.size(); ++valueIndex) {
        reference[valueIndex] = static_cast<std::int32_t>(
            (valueIndex * 7919u) % 1000003u) - 500001;
        current[valueIndex] = reference[valueIndex] +
            static_cast<std::int32_t>(valueIndex % 17u) - 8;
    }
    ScratchByteBufferPool scratchBytePool;
    NumericArrayReferenceEncodedBlock encoded;
    numericarray::NumericArrayBlockCapacitySamples encodeSamples;
    numericarray::NumericArrayBlockCapacitySamples decodeSamples;
    std::vector<std::uint8_t> decodedBytes;
    std::string error;
    const bool roundTrip = EncodeDecodeReferenceTestBlock(
        NumericArrayReferenceCodecId::Wavelet,
        MakeLosslessNumericArrayCompressor(),
        scratchBytePool,
        current,
        reference,
        kComponentCount,
        NumericArrayReferenceKind::IntraArray,
        encoded,
        decodedBytes,
        &error, &encodeSamples, &decodeSamples);
    using Sample = numericarray::NumericBufferSample;
    const auto& encodedLow = encodeSamples.values[static_cast<std::size_t>(Sample::WaveletLow)];
    const auto& decodedLow = decodeSamples.values[static_cast<std::size_t>(Sample::WaveletLow)];
    const auto& decodedDelta = decodeSamples.values[static_cast<std::size_t>(Sample::WaveletLowDelta)];
    Require(result, encodedLow.capacityBytes.value_or(0u) >= (kTupleCount / 2u) * sizeof(std::uint64_t) &&
        decodedLow.capacityBytes.value_or(0u) >= (kTupleCount / 2u) * sizeof(std::uint64_t) &&
        decodedDelta.capacityBytes.value_or(0u) >= (kTupleCount / 2u) * sizeof(std::uint64_t) &&
        decodedDelta.scopeId != decodedLow.scopeId,
        "referenceCodec.wavelet.int32.capacity", "integer wavelet samples must reflect uint64 work arrays independently of int32 input");
    return Require(
               result,
               roundTrip,
               "referenceCodec.wavelet.int32.roundTrip",
               error.empty() ? "integer wavelet round trip failed" : error) &&
        Require(
            result,
            decodedBytes == std::vector<std::uint8_t>(
                NumericValueBytes(current).begin(),
                NumericValueBytes(current).end()),
            "referenceCodec.wavelet.int32.lossless",
            "integer wavelet reconstruction is not lossless");
}

inline bool RunBoundedProbePreparedPayloadCase(TestResult& result) {
    constexpr std::size_t kTupleCount = 8193u;
    constexpr std::size_t kComponentCount = 3u;
    const auto reference = MakeReferencePrecisionValues<float>(
        kTupleCount,
        kComponentCount,
        false);
    const auto current = MakeReferencePrecisionValues<float>(
        kTupleCount,
        kComponentCount,
        true);
    const auto meta = MakeReferenceTestMeta<float>(kTupleCount, kComponentCount);

    numericarray::NumericArraySource currentSource;
    numericarray::NumericArraySource referenceSource;
    std::string error;
    if (!numericarray::MakeNumericArrayLayoutFromMeta(meta, currentSource.layout, &error) ||
        !numericarray::MakeNumericArrayLayoutFromMeta(meta, referenceSource.layout, &error)) {
        return Require(
            result,
            false,
            "referenceCodec.boundedProbe.layout",
            error.empty() ? "failed to prepare bounded probe layouts" : error);
    }
    currentSource.values = MakeCompactNumericArrayView(
        current.data(),
        ScalarType::Float32,
        kTupleCount,
        static_cast<int>(kComponentCount));
    referenceSource.values = MakeCompactNumericArrayView(
        reference.data(),
        ScalarType::Float32,
        kTupleCount,
        static_cast<int>(kComponentCount));

    numericarrayreference::NumericArrayReferenceSourceData referenceData{
        .candidate = NumericArrayReferenceCandidate{
            .scope = NumericArrayReferenceScope::IntraArray,
            .localParentFieldIndex = 0u,
        },
        .meta = meta,
        .source = referenceSource,
    };
    DataCodecExecutionResources root(CodecResourceParams{});
    CodecRunScope scope(root);
    bytestore::ByteStoreSession byteStoreSession;
    byteStoreSession.BindRun(root);
    std::shared_ptr<bytestore::IByteSource> transferCache;
    std::vector<NumericArrayBlockLayoutParams> blockLayouts;
    using Sample = numericarray::NumericBufferSample;
    std::array<std::array<std::uint64_t, 4u>, 2u> capacities{};
    std::array<std::uint64_t, 2u> rawScopeIds{};
    std::size_t sampleCalls = 0u;
    bool samplesHeldSlot = true;
    const auto built = numericarrayreference::BuildNumericArrayReferenceTransferCache(
        meta,
        MakeAbsoluteErrorNumericArrayCompressor(1.0e-3),
        currentSource,
        referenceData,
        NumericArrayReferenceCodecId::Affine,
        numericarrayreference::NumericArrayReferenceTransferControl{
            .affineBlockRSquared = 0.0,
            .selectionMode = ReferenceSelectionMode::Auto,
            .autoSelectionStrategy = ReferenceAutoSelectionStrategy::BoundedProbe,
        },
        root,
        transferCache,
        byteStoreSession,
        &blockLayouts,
        &error,
        "reference_bounded_probe_test",
        [&](std::span<const BufferCapacitySample> samples) {
            ResourceDebugSnapshot snapshot;
            samplesHeldSlot &= CopyExecutionSnapshot(root, snapshot) && snapshot.admittedBlocks != 0u;
            if (sampleCalls < capacities.size()) {
                const std::array kinds{Sample::Raw, Sample::ReferencePrimary,
                    Sample::OrdinaryCandidate, Sample::ReferenceCandidate};
                for (std::size_t i = 0u; i < kinds.size(); ++i) {
                    capacities[sampleCalls][i] = samples[static_cast<std::size_t>(kinds[i])].sampledPeakBytes.value_or(0u);
                }
                rawScopeIds[sampleCalls] = samples[static_cast<std::size_t>(Sample::Raw)].scopeId;
            }
            ++sampleCalls;
        });
    constexpr auto fullBytes = kTupleCount * kComponentCount * sizeof(float);
    constexpr auto probeBytes = numericarrayreference::kReferenceProbeElementCount * kComponentCount * sizeof(float);
    Require(result, sampleCalls == 2u && samplesHeldSlot && rawScopeIds[0] != rawScopeIds[1] &&
        capacities[0][0] == fullBytes && capacities[0][1] == fullBytes &&
        (capacities[0][2] != 0u || capacities[0][3] != 0u) &&
        capacities[1][0] == probeBytes && capacities[1][1] == probeBytes &&
        capacities[1][2] != 0u && capacities[1][3] != 0u,
        "referenceCodec.boundedProbe.capacities", "full staging and bounded probe must have distinct sample identities and owned candidates inside the slot");
    return Require(
               result,
               built,
               "referenceCodec.boundedProbe.build",
               error.empty() ? "bounded probe transfer build failed" : error) &&
        Require(
            result,
            transferCache != nullptr && transferCache->CanRead(),
            "referenceCodec.boundedProbe.transfer",
            "bounded probe transfer cache is unavailable") &&
        Require(
            result,
            blockLayouts.size() == 1u,
            "referenceCodec.boundedProbe.blockCount",
            "bounded probe transfer produced an unexpected block count");
}

inline void RunWindowedReferenceResampleCase(TestResult& result) {
    constexpr std::size_t components = 3u;
    constexpr std::size_t tupleBytes = components * sizeof(double);
    for (const bool sparse : {false, true}) {
        const std::size_t referenceCount = sparse ? 1000000001u : 2u * (kIoWindowBytes / tupleBytes) + 3u;
        const std::size_t targetCount = sparse ? 11u : referenceCount + 7u;
        struct SourceState { mutable std::size_t maxReadBytes{0u}, totalTuples{0u}; } state;
        numericarray::NumericArraySource source{
            .values = NumericArrayView{
                .scalarType = ScalarType::Float64, .layout = ArrayLayout::GetterOnly,
                .origin = ViewBufferOrigin::Borrowed, .tupleCount = referenceCount, .componentCount = components,
                .userData = &state,
                .getTupleBytes = [](const void*, std::size_t index, void* output, std::string*) {
                    double values[components];
                    for (std::size_t c = 0u; c < components; ++c) { values[c] = index * 0.125 + c; }
                    std::memcpy(output, values, sizeof(values));
                    return true;
                },
                .getTupleRangeBytes = [](const void* data, std::size_t begin, std::size_t count,
                                         void* output, std::size_t byteCount, std::string*) {
                    const auto& state = *static_cast<const SourceState*>(data);
                    state.maxReadBytes = std::max(state.maxReadBytes, byteCount);
                    state.totalTuples += count;
                    auto* values = static_cast<double*>(output);
                    for (std::size_t i = 0u; i < count; ++i) {
                        for (std::size_t c = 0u; c < components; ++c) {
                            values[i * components + c] = (begin + i) * 0.125 + c;
                        }
                    }
                    return byteCount == count * tupleBytes;
                },
            },
            .layout = numericarray::MakeNumericArrayLayout(DataType::Float64, sizeof(double), referenceCount, components),
        };
        numericarray::NumericArrayReader reader;
        ScratchByteBufferPool scratch;
        ScratchByteBuffer output;
        std::string error;
        bool matches = numericarray::BuildNumericArrayReader(source, reader, &error) &&
            numericarrayreference::BuildNumericArrayNormalizedResampledSourceRangeBytes(
                reader, MakeReferenceTestMeta<double>(referenceCount, components),
                MakeReferenceTestMeta<double>(targetCount, components), scratch,
                0u, targetCount, output, &error);
        const auto* values = reinterpret_cast<const double*>(output.Span().data());
        for (std::size_t i = 0u; matches && i < targetCount; ++i) {
            const auto position = (static_cast<double>(i) / (targetCount - 1u)) * (referenceCount - 1u);
            const auto left = static_cast<std::size_t>(std::floor(position));
            const auto right = std::min(left + 1u, referenceCount - 1u);
            const auto weightRight = position - left;
            const auto weightLeft = 1.0 - weightRight;
            for (std::size_t c = 0u; matches && c < components; ++c) {
                const auto expected = (left * 0.125 + c) * weightLeft + (right * 0.125 + c) * weightRight;
                matches = values[i * components + c] == expected;
            }
        }
        Require(result, matches && state.maxReadBytes <= kIoWindowBytes &&
            (!sparse || state.totalTuples <= 2u * targetCount),
            "referenceCodec.windowedResample", error.empty() ?
                "windowed interpolation must preserve arithmetic and avoid unused sparse source ranges" : error);
    }
}

inline void RunReferenceBlockFlowCase(TestResult& result) {
    constexpr std::size_t count = 2u * numericarray::kSpatialBlockElementCount + 7u;
    constexpr std::uint64_t limit = 32u * 1024u * 1024u;
    for (const bool externalSpill : {false, true}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 2u, 4u},
            limit, 2u, true, true, externalSpill});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), externalSpill);
        struct State { DataCodecExecutionResources& root; mutable bool valid{true}; } state{root};
        numericarray::NumericArraySource source{
            .values = NumericArrayView{
                .scalarType = ScalarType::Float32, .layout = ArrayLayout::GetterOnly,
                .origin = ViewBufferOrigin::Borrowed, .tupleCount = count, .componentCount = 1u,
                .userData = &state,
                .getTupleBytes = [](const void* data, std::size_t index, void* output, std::string*) {
                    const auto& state = *static_cast<const State*>(data);
                    if (index % numericarray::kSpatialBlockElementCount == 0u) {
                        ResourceDebugSnapshot snapshot;
                        state.valid &= CopyExecutionSnapshot(state.root, snapshot) && snapshot.admittedBlocks != 0u;
                    }
                    const float value = static_cast<float>(std::sin(index * 0.013));
                    std::memcpy(output, &value, sizeof(value));
                    return true;
                },
            },
            .layout = numericarray::MakeNumericArrayLayout(DataType::Float32, sizeof(float), count, 1u),
        };
        const auto meta = MakeReferenceTestMeta<float>(count, 1u);
        auto referenceMeta = meta;
        referenceMeta.blockLayouts.push_back(NumericArrayBlockLayoutParams{
            .elementOffset = 0u, .elementCount = count});
        numericarrayreference::NumericArrayReferenceSourceData reference{
            .candidate = {.scope = NumericArrayReferenceScope::IntraArray, .localParentFieldIndex = 0u},
            .meta = std::move(referenceMeta), .source = source,
        };
        std::shared_ptr<bytestore::IByteSource> output;
        std::vector<NumericArrayBlockLayoutParams> layouts;
        std::string error;
        std::size_t sampledBlocks = 0u;
        bool sampledCandidates = true;
        const bool success = numericarrayreference::BuildNumericArrayReferenceTransferCache(meta,
            MakeAbsoluteErrorNumericArrayCompressor(0.001), source, reference,
            NumericArrayReferenceCodecId::Wavelet,
            numericarrayreference::NumericArrayReferenceTransferControl{
                .selectionMode = ReferenceSelectionMode::Forced},
            root, output, session, &layouts, &error, "reference_block_flow",
            [&](std::span<const BufferCapacitySample> samples) {
                using Sample = numericarray::NumericBufferSample;
                const auto expectedBytes = (sampledBlocks < 2u ? numericarray::kSpatialBlockElementCount : 7u) * sizeof(float);
                ResourceDebugSnapshot current;
                sampledCandidates &= CopyExecutionSnapshot(root, current) && current.admittedBlocks != 0u &&
                    samples[static_cast<std::size_t>(Sample::Raw)].capacityBytes.value_or(0u) >= expectedBytes &&
                    samples[static_cast<std::size_t>(Sample::ReferencePrimary)].capacityBytes.value_or(0u) >= expectedBytes &&
                    samples[static_cast<std::size_t>(Sample::ReferenceCandidate)].sampledPeakBytes.value_or(0u) != 0u;
                ++sampledBlocks;
            });
        Require(result, sampledBlocks == 3u && sampledCandidates,
            "referenceCodec.candidate-capacities", "each full reference candidate and staging array must be sampled before its slot retires");
        ResourceDebugSnapshot snapshot;
        Require(result, success && state.valid && layouts.size() == 3u &&
            layouts.back().elementCount == 7u && scope.Finish(success) && CopyExecutionSnapshot(root, snapshot) &&
            snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u && snapshot.lastRetired == 2u,
            "referenceCodec.blockFlow", error.empty() ?
                "reference input and ordered results must use fixed blocks and one root flow" : error);
        output.reset();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
            root.Scratch().SnapshotStats().activeBlockCount == 0u,
            "referenceCodec.flowRelease", "reference inputs and candidates must be released after consumption");
    }
}

[[nodiscard]] inline TestResult RunDataCodecFeatureReferenceCodecs() noexcept {
    TestResult result;
    try {
        RunReferenceCodecDefaultCase(result);
        const std::vector<NumericArrayReferenceCodecId> codecs{
            NumericArrayReferenceCodecId::Predictor,
            NumericArrayReferenceCodecId::Affine,
            NumericArrayReferenceCodecId::Wavelet,
        };
        for (const auto codecId : codecs) {
            const auto codecName = std::to_string(static_cast<std::uint16_t>(codecId));
            RunReferenceCodecPrecisionCase<float>(
                result,
                codecId,
                MakeAbsoluteErrorNumericArrayCompressor(1.0e-3),
                4097u,
                3u,
                "referenceCodec." + codecName + ".float32.absolute");
            RunReferenceCodecPrecisionCase<double>(
                result,
                codecId,
                MakeRelativeErrorNumericArrayCompressor(1.0e-5),
                4097u,
                3u,
                "referenceCodec." + codecName + ".float64.relative");
        }
        RunReferenceCodecNonFiniteCase(result);
        RunReferenceCodecTypeDispatchCase(result);
        RunIntegerWaveletCase(result);
        RunBoundedProbePreparedPayloadCase(result);
        RunWindowedReferenceResampleCase(result);
        RunReferenceBlockFlowCase(result);
    } catch (const std::exception& exception) {
        result.AddFailure("referenceCodec.exception", exception.what());
    } catch (...) {
        result.AddFailure("referenceCodec.exception", "unknown exception");
    }
    return result;
}

} // namespace datacodec::test

#endif
