#ifndef DATACODEC_TEST_SUITE_DATACODECTESTSUITE_H
#define DATACODEC_TEST_SUITE_DATACODECTESTSUITE_H

#include "DataCodec/Test/Feature/DataCodecFeatureAdapterRoundTrip.h"
#include "DataCodec/Test/Feature/DataCodecFeatureByteRange.h"
#include "DataCodec/Test/Feature/DataCodecFeatureBudget.h"
#include "DataCodec/Test/Feature/DataCodecFeatureRobustness.h"
#include "DataCodec/Test/Feature/DataCodecFeatureCellGraphTopology.h"
#include "DataCodec/Test/Feature/DataCodecFeatureFailure.h"
#include "DataCodec/Test/Feature/DataCodecFeatureStorageOwnership.h"
#include "DataCodec/Test/Feature/DataCodecFeatureMortonResources.h"
#include "DataCodec/Test/Feature/DataCodecFeatureTopologyExecution.h"
#include "DataCodec/Test/Feature/DataCodecFeatureNumericExecution.h"
#include "DataCodec/Test/Feature/DataCodecFeatureNumericDecodeExecution.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"
#include "DataCodec/Test/Feature/DataCodecFeatureLocalization.h"
#include "DataCodec/Test/Feature/DataCodecFeatureOutputSinks.h"
#include "DataCodec/Test/Feature/DataCodecFeaturePackageIdentity.h"
#include "DataCodec/Test/Feature/DataCodecFeaturePipelineContracts.h"
#include "DataCodec/Test/Feature/DataCodecFeatureReferenceCodecs.h"
#include "DataCodec/Test/Feature/DataCodecFeatureRegionPrecision.h"
#include "DataCodec/Test/Feature/DataCodecFeatureTelemetry.h"
#include "DataCodec/Test/Feature/DataCodecFeatureTopologyObserver.h"
#include "DataCodec/Test/Feature/DataCodecFeatureValidation.h"

namespace datacodec::test {

TestResult RunDataCodecCpuControlTests();

[[nodiscard]] inline TestResult RunDataCodecSelfTest() noexcept {
    auto result = RunDataCodecFeatureAdapterRoundTrip();
    const auto appendResult = [&](const TestResult& addition) {
        if (!addition.passed) {
            result.passed = false;
            result.failures.insert(
                result.failures.end(),
                addition.failures.begin(),
                addition.failures.end());
        }
        result.AppendDiagnostics(addition.diagnostics);
    };
    auto referenceResult = RunDataCodecFeatureReferenceCodecs();
    appendResult(referenceResult);
    auto pipelineResult = RunDataCodecFeaturePipelineContracts();
    appendResult(pipelineResult);
    appendResult(RunDataCodecFeatureByteRange());
    appendResult(RunDataCodecFeatureBudget());
    appendResult(RunDataCodecFeatureRobustness());
    appendResult(RunDataCodecFeatureFailure());
    appendResult(RunDataCodecFeatureStorageOwnership());
    appendResult(RunDataCodecFeatureMortonResources());
    appendResult(RunDataCodecFeatureTopologyExecution());
    appendResult(RunDataCodecFeatureNumericExecution());
    appendResult(RunDataCodecFeatureNumericDecodeExecution());
    appendResult(RunDataCodecFeatureExecutionMechanism());
    appendResult(RunDataCodecFeatureCellGraphTopology());
    appendResult(RunDataCodecFeatureLocalization());
    appendResult(RunDataCodecFeatureOutputSinks());
    appendResult(RunDataCodecFeaturePackageIdentity());
    appendResult(RunDataCodecFeatureRegionPrecision());
    appendResult(RunDataCodecFeatureTelemetry());
    appendResult(RunDataCodecFeatureTopologyObserver());
    appendResult(RunDataCodecFeatureValidation());
    return result;
}

} // datacodec::test命名空间

#endif
