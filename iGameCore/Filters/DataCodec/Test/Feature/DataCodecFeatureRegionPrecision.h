#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREREGIONPRECISION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREREGIONPRECISION_H

#include "DataCodec/API/Params/NumericArrayParams.h"
#include "DataCodec/Codec/NumericArray/NumericArrayRegionPlan.h"
#include "DataCodec/Runtime/Cache/TransferCache/Common/NumericArrayTransferCacheBuilder.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <span>
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
                sizeof(RegionRun), "region_capacity_denied", &error);
            Require(result, !denied, "regionPrecision.sharedCapacity", "shared owner must keep the full run capacity");
            sharedOwner.reset();
            auto reused = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
                bytes, "region_capacity_reused", &error);
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
