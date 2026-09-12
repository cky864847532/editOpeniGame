#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREREGIONPRECISION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREREGIONPRECISION_H

#include "DataCodec/API/Params/NumericArrayParams.h"
#include "DataCodec/Codec/NumericArray/NumericArrayRegionPlan.h"
#include "DataCodec/Codec/NumericArray/NumericArrayBlockDecode.h"
#include "DataCodec/Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <span>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <string>
#include <vector>

namespace datacodec::test {

inline CompressorConfig MakeRegionPrecisionTestCompressor(const double absolutePrecision) {
    CompressorConfig compressor;
    compressor.options["pressio:abs"] = absolutePrecision;
    return compressor;
}

inline NumericArrayRegionControlParams MakeRegionPrecisionTestControl(
    const double defaultPrecision,
    const double customPrecision) {
    NumericArrayRegionControlParams control;
    control.defaultPrecision = MakeNumericArrayRegionPrecision(
        MakeRegionPrecisionTestCompressor(defaultPrecision));
    control.regions.push_back(MakeNumericArrayRegionPrecision(
        MakeRegionPrecisionTestCompressor(customPrecision)));
    control.runPolicy.maxRunsPerRegion = 16u;
    control.runPolicy.minCoreRunLength = 1u;
    control.runPolicy.coreMinRatio = 0.0;
    control.runPolicy.minLongestRunRatio = 0.0;
    control.runPolicy.maxFragmentRunLength = 16u;
    control.runPolicy.maxFragmentElementRatio = 1.0;
    control.runPolicy.maxCoalesceGap = 16u;
    control.runPolicy.maxExpansionRatio = 1.0;
    control.runPolicy.maxRefinedElementRatio = 8.0;
    return control;
}

[[nodiscard]] inline TestResult RunDataCodecFeatureRegionPrecision() noexcept {
    TestResult result;
    const auto checkLayeredBlock = [&]<typename Value>() {
        constexpr std::uint32_t count = 33u, components = 3u;
        std::vector<Value> input(count * components);
        for (std::size_t i = 0u; i < input.size(); ++i) {
            input[i] = static_cast<Value>(std::sin(static_cast<double>(i) * 0.37) * 10.0 + i * 0.013);
        }
        auto control = MakeRegionPrecisionTestControl(0.1, 0.01);
        control.regions.push_back(MakeNumericArrayRegionPrecision(MakeRegionPrecisionTestCompressor(0.001)));
        const std::array<RegionRun, 2u> runs{{{3u, 7u, 1u}, {19u, 9u, 2u}}};
        auto params = numericarray::MakeNumericArrayBlockParams(numericarray::MakeNumericArrayLayout(
            std::is_same_v<Value, float> ? DataType::Float32 : DataType::Float64, sizeof(Value), count, components));
        params.regionControl = &control;
        numericarray::PreparedRegionPrecision prepared;
        numericarray::NumericArrayBlockCapacitySamples encodeSamples, decodeSamples;
        params.capacitySamples = &encodeSamples;
        ScratchByteBufferPool scratch(0u);
        std::vector<std::uint8_t> encoded, decoded;
        NumericArrayBytesCodec codec;
        NumericArrayBlockLayoutParams layout;
        std::string error;
        const bool encodedOk = numericarray::PrepareRegionPrecision(control, prepared, &error) &&
            numericarray::ResolveEncodedLayeredResidualNumericArrayBlockBytes(params, prepared, 0u, count,
                {reinterpret_cast<const std::uint8_t*>(input.data()), input.size() * sizeof(Value)},
                runs, encoded, codec, layout, &error, &scratch);
        params.capacitySamples = &decodeSamples;
        const bool decodedOk = encodedOk && numericarray::ResolveDecodedLayeredResidualNumericArrayBlockBytes(
            params, count, layout.backgroundCompressor, layout.backgroundEncodedByteLength,
            layout.componentLayouts, layout.regionLayers, encoded, decoded, &error);
        bool precise = decodedOk && decoded.size() == input.size() * sizeof(Value);
        for (std::size_t tuple = 0u; precise && tuple < count; ++tuple) {
            const double tolerance = tuple >= 19u && tuple < 28u ? 0.00101 :
                tuple >= 3u && tuple < 10u ? 0.01001 : 0.10001;
            for (std::size_t component = 0u; precise && component < components; ++component) {
                const auto index = tuple * components + component;
                Value actual{};
                std::memcpy(&actual, decoded.data() + index * sizeof(Value), sizeof(Value));
                precise &= std::abs(static_cast<double>(actual) - input[index]) <= tolerance;
            }
        }
        std::uint64_t largestLayer = 0u;
        for (const auto& layer : layout.regionLayers) { largestLayer = std::max<std::uint64_t>(largestLayer, layer.refinedElementCount); }
        const auto sample = [](const auto& samples, numericarray::NumericBufferSample kind) {
            return samples.values[static_cast<std::size_t>(kind)].sampledPeakBytes.value_or(0u);
        };
        Require(result, precise && layout.regionLayers.size() == 2u && largestLayer != 0u &&
            sample(encodeSamples, numericarray::NumericBufferSample::BaseDecoded) >= count * sizeof(Value) &&
            sample(encodeSamples, numericarray::NumericBufferSample::ResidualRaw) >= largestLayer * sizeof(Value) &&
            sample(encodeSamples, numericarray::NumericBufferSample::ResidualDecoded) >= largestLayer * sizeof(Value) &&
            sample(encodeSamples, numericarray::NumericBufferSample::Output) == encoded.capacity() &&
            sample(decodeSamples, numericarray::NumericBufferSample::ResidualDecoded) >= largestLayer * components * sizeof(Value) &&
            scratch.SnapshotStats().activeBlockCount == 0u,
            std::is_same_v<Value, float> ? "regionPrecision.layered-capacity-float32" : "regionPrecision.layered-capacity-float64",
            "encoded=" + std::to_string(encodedOk) + ";decoded=" + std::to_string(decodedOk) +
            ";precise=" + std::to_string(precise) + ";layers=" + std::to_string(layout.regionLayers.size()) +
            ";largest=" + std::to_string(largestLayer) +
            ";residual_raw_peak=" + std::to_string(sample(encodeSamples, numericarray::NumericBufferSample::ResidualRaw)) +
            ";residual_decoded_peak=" + std::to_string(sample(decodeSamples, numericarray::NumericBufferSample::ResidualDecoded)) +
            ";error=" + error);
        if (encodedOk && !encoded.empty()) {
            encoded.pop_back();
            Require(result, !numericarray::ResolveDecodedLayeredResidualNumericArrayBlockBytes(params, count,
                layout.backgroundCompressor, layout.backgroundEncodedByteLength, layout.componentLayouts,
                layout.regionLayers, encoded, decoded, &error),
                "regionPrecision.truncated-layer", "a truncated final residual must fail without accepting the background as a complete block");
        }
    };
    checkLayeredBlock.template operator()<float>();
    checkLayeredBlock.template operator()<double>();
    std::vector<RegionRun> customRuns{
        RegionRun{.begin = 2u, .count = 2u, .regionId = 1u},
    };

    Require(result, numericarray::SortAndValidateRegionRunsForEncode(customRuns, 6u, 1u),
        "regionPrecision.prepareRuns", "the field run index must be validated once before planning");

    {
        auto control = MakeRegionPrecisionTestControl(0.01, 0.1);
        numericarray::LayeredResidualRegionPlan plan;
        numericarray::PreparedRegionPrecision prepared;
        std::string error;
        const auto built = numericarray::PrepareRegionPrecision(control, prepared, &error) &&
            numericarray::BuildNormalizedRegionPlansFromRegionRuns(
            std::span<const RegionRun>(customRuns.data(), customRuns.size()),
            0u,
            6u,
            prepared,
            plan,
            &error);
        Require(
            result,
            built,
            "regionPrecision.defaultComplement.build",
            error.empty() ? "default complement plan was rejected" : error);
        const bool hasExpectedComplement =
            built &&
            plan.layers.size() == 1u &&
            plan.layers[0].runs.size() == 2u &&
            plan.layers[0].runs[0].begin == 0u &&
            plan.layers[0].runs[0].count == 2u &&
            plan.layers[0].runs[1].begin == 4u &&
            plan.layers[0].runs[1].count == 2u;
        Require(
            result,
            hasExpectedComplement,
            "regionPrecision.defaultComplement.runs",
            "stricter default precision did not produce the custom-region complement");
    }

    {
        auto control = MakeRegionPrecisionTestControl(0.1, 0.01);
        numericarray::LayeredResidualRegionPlan plan;
        numericarray::PreparedRegionPrecision prepared;
        std::string error;
        const auto built = numericarray::PrepareRegionPrecision(control, prepared, &error) &&
            numericarray::BuildNormalizedRegionPlansFromRegionRuns(
            std::span<const RegionRun>(customRuns.data(), customRuns.size()),
            0u,
            6u,
            prepared,
            plan,
            &error);
        Require(
            result,
            built,
            "regionPrecision.customRefinement.build",
            error.empty() ? "custom refinement plan was rejected" : error);
        const bool hasExpectedCustomRun =
            built &&
            plan.layers.size() == 1u &&
            plan.layers[0].runs.size() == 1u &&
            plan.layers[0].runs[0].begin == 2u &&
            plan.layers[0].runs[0].count == 2u;
        Require(
            result,
            hasExpectedCustomRun,
            "regionPrecision.customRefinement.runs",
            "stricter custom precision did not retain its selected run");
    }

    {
        auto control = MakeRegionPrecisionTestControl(0.01, 0.1);
        numericarray::LayeredResidualRegionPlan plan;
        numericarray::PreparedRegionPrecision prepared;
        std::string error;
        const auto built = numericarray::PrepareRegionPrecision(control, prepared, &error) &&
            numericarray::BuildNormalizedRegionPlansFromRegionRuns(
            std::span<const RegionRun>(customRuns.data(), customRuns.size()),
            1u,
            4u,
            prepared,
            plan,
            &error);
        const bool hasExpectedClippedComplement =
            built &&
            plan.layers.size() == 1u &&
            plan.layers[0].runs.size() == 2u &&
            plan.layers[0].runs[0].begin == 0u &&
            plan.layers[0].runs[0].count == 1u &&
            plan.layers[0].runs[1].begin == 3u &&
            plan.layers[0].runs[1].count == 1u;
        Require(
            result,
            hasExpectedClippedComplement,
            "regionPrecision.defaultComplement.blockClip",
            error.empty() ? "default complement was not clipped to the block" : error);
    }

    {
        NumericArrayControlParams control;
        control.regionControl = MakeRegionPrecisionTestControl(0.01, 0.1);
        constexpr ParamSize blockSize = numericarray::kSpatialBlockElementCount;
        control.regionRuns = {{blockSize + 2u, 2u, 1u}, {2u, 2u, 1u}};
        numericarray::NumericArrayBlockParams params;
        numericarray::ApplyNumericArrayControlParams(params, control);
        constexpr auto bytes = 2u * sizeof(RegionRun);
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{bytes, 1u, 1u},
            bytes, 1u, false, true, false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), false);
        auto phase = WaitForHeavyPhase(root);
        encodeimpl::NumericEncodeRegionState field;
        std::string error;
        const bool prepared = phase && encodeimpl::PrepareNumericEncodeRegions(
            params, 3u * blockSize, root, *phase, session, field, &error);
        Require(result, prepared && field.owner->ResidentSizeHint() == bytes &&
            field.runs.size() == 2u && field.runs[0].begin == 2u &&
            control.regionRuns[0].begin == blockSize + 2u,
            "regionPrecision.fieldOwner", error.empty() ? "one exact owner must sort the borrowed input copy" : error);
        if (prepared) {
            const auto first = numericarray::FindIntersectingRegionRuns(field.runs, 0u, blockSize);
            const auto second = numericarray::FindIntersectingRegionRuns(field.runs, blockSize, blockSize);
            const auto last = numericarray::FindIntersectingRegionRuns(field.runs, 2u * blockSize, blockSize);
            numericarray::LayeredResidualRegionPlan plan;
            const bool planned = numericarray::BuildNormalizedRegionPlansFromRegionRuns(
                last, 2u * blockSize, blockSize, field.precision, plan, &error);
            Require(result, first.size() == 1u && second.size() == 1u && last.empty() && planned &&
                plan.layers.size() == 1u && plan.layers[0].runs.size() == 1u &&
                plan.layers[0].runs[0].count == blockSize,
                "regionPrecision.localIntersection", "empty intersection must retain the strict default precision");
            auto sharedOwner = field.owner;
            field = {};
            auto denied = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
                sizeof(RegionRun), ::datacodec::MemoryDemandKind::RequiredContinuation, "region_capacity_denied", &error);
            Require(result, !denied, "regionPrecision.sharedCapacity", "shared owner must keep the full run capacity");
            sharedOwner.reset();
            auto reused = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
                bytes, ::datacodec::MemoryDemandKind::RequiredContinuation, "region_capacity_reused", &error);
            Require(result, reused != nullptr, "regionPrecision.lastOwner", "last owner release must return run capacity");
        }
    }
    {
        std::vector<RegionRun> overlap{{3u, 2u, 1u}, {2u, 2u, 1u}};
        std::vector<RegionRun> empty{{0u, 0u, 1u}};
        std::vector<RegionRun> outside{{5u, 2u, 1u}};
        std::vector<RegionRun> invalidLabel{{0u, 1u, 2u}};
        Require(result, !numericarray::SortAndValidateRegionRunsForEncode(overlap, 6u, 1u) &&
            !numericarray::SortAndValidateRegionRunsForEncode(empty, 6u, 1u) &&
            !numericarray::SortAndValidateRegionRunsForEncode(outside, 6u, 1u) &&
            !numericarray::SortAndValidateRegionRunsForEncode(invalidLabel, 6u, 1u),
            "regionPrecision.invalidRuns", "invalid field runs must fail during field preparation");
    }
    return result;
}

} // namespace datacodec::test

#endif
