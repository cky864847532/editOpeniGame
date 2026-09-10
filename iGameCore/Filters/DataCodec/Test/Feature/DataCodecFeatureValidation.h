#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREVALIDATION_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREVALIDATION_H

#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedAttributeCacheSet.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Validation/Attribute/AttributeValidator.h"
#include "DataCodec/Validation/Filter/FilterCommitValidator.h"
#include "DataCodec/Validation/Geometry/GeometryValidator.h"
#include "DataCodec/Validation/Policy/ValidationRuleIndex.h"
#include "DataCodec/Validation/Storage/StorageValidator.h"
#include "DataCodec/Validation/Topology/TopologyValidator.h"
#include "DataCodec/Validation/Workflow/DecodeValidationLifecycle.h"

#include <array>
#include <memory>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace datacodec::test {

template<class Value>
inline void CheckFiniteValidationWindows(TestResult& result) {
    constexpr std::size_t valueCount = 2u * 65536u + 7u;
    class Store final : public bytestore::IRandomAccessByteStore {
    public:
        explicit Store(int mode) : mode(mode) {}
        std::uint64_t ByteSizeHint() const noexcept override { return valueCount * sizeof(Value); }
        bool CanRead() const noexcept override { return true; }
        bool CopyTo(bytestore::IByteWriter&, std::string*) override { return false; }
        bool AppendBytes(std::span<const std::uint8_t>, std::string*) override { return false; }
        bool ResizeBytes(std::uint64_t, std::string*) override { return false; }
        bool WriteBytesAt(std::uint64_t, std::span<const std::uint8_t>, std::string*) override { return false; }
        bool Seal(std::string*) override { return true; }
        bool Read(std::uint64_t offset, std::span<std::uint8_t> bytes, std::string*) const override {
            const auto expected = std::min<std::size_t>(65536u, valueCount - readValues);
            bounded &= offset == readValues * sizeof(Value) && bytes.size() == expected * sizeof(Value);
            ++reads;
            if (mode == 2 && reads == 2u) { return false; }
            for (std::size_t i = 0u; i < expected; ++i) {
                const Value value = mode == 1 && readValues + i == valueCount - 1u
                    ? std::numeric_limits<Value>::quiet_NaN() : static_cast<Value>(0.5);
                std::memcpy(bytes.data() + i * sizeof(Value), &value, sizeof(value));
            }
            readValues += expected;
            return true;
        }
        int mode;
        mutable std::size_t reads{0u}, readValues{0u};
        mutable bool bounded{true};
    };
    for (const int mode : {0, 1, 2}) {
        auto source = std::make_shared<Store>(mode);
        const auto validated = validation::ValidateFiniteNumericStore<Value>(source, valueCount,
            validation::ValidationDomain::Geometry, "finite-window-fixture");
        Require(result, validated.success == (mode == 0) && source->bounded && source->reads == (mode == 2 ? 2u : 3u),
            "validation.finite-window-" + std::to_string(sizeof(Value)) + "-" + std::to_string(mode),
            "finite validation must scan fixed value windows and stop at a tail NaN or failed range without loading the whole field");
    }
}

[[nodiscard]] inline TestResult RunDataCodecFeatureValidation() noexcept {
    TestResult result;
    CheckFiniteValidationWindows<float>(result);
    CheckFiniteValidationWindows<double>(result);

    Require(
        result,
        !validation::StorageValidator::ValidateDecodeInput(nullptr),
        "validation.storage.invalid",
        "storage validator accepted a null reader");
    MemoryByteRangeReader preambleReader(std::make_shared<const std::vector<std::uint8_t>>(6u));
    Require(
        result,
        static_cast<bool>(validation::StorageValidator::ValidateDecodeInput(&preambleReader)),
        "validation.storage.valid",
        "storage validator rejected a complete preamble");

    CodecStorageParams params;
    params.geomParams.elementCount = 2u;
    params.geomParams.dimension = 3;
    DecodedGeometryCache geometry;
    geometry.pointCount = 2u;
    geometry.dimension = 3u;
    geometry.complete = true;
    Require(
        result,
        static_cast<bool>(validation::GeometryValidator::ValidateDecodedCacheShape(params, geometry)),
        "validation.geometry.valid",
        "geometry validator rejected a matching cache");
    geometry.pointCount = 1u;
    Require(
        result,
        !validation::GeometryValidator::ValidateDecodedCacheShape(params, geometry),
        "validation.geometry.invalid",
        "geometry validator accepted a mismatched cache");

    CodecStorageParams finiteParams;
    finiteParams.geomParams.elementCount = 2u;
    finiteParams.geomParams.dimension = 1;
    finiteParams.geomParams.dataType = DataType::Float32;
    bytestore::ByteStoreSession geometryStoreSession;
    geometryStoreSession.BindStorage(std::make_shared<resource::ResidentByteBudget>(1024u), true);
    DecodedGeometryCache finiteGeometry;
    std::string finiteError;
    const std::array<float, 2u> finiteGeometryValues{1.0f, 2.0f};
    const auto finiteGeometryReady = finiteGeometry.Initialize(
        2u,
        1u,
        geometryStoreSession,
        &finiteError) &&
        finiteGeometry.bytes->WriteBytesAt(
            0u,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(finiteGeometryValues.data()),
                sizeof(finiteGeometryValues)),
            &finiteError) &&
        finiteGeometry.bytes->Seal(&finiteError);
    finiteGeometry.complete = finiteGeometryReady;
    Require(
        result,
        finiteGeometryReady && static_cast<bool>(
            validation::GeometryValidator::ValidateFiniteValues(
                finiteParams,
                finiteGeometry)),
        "validation.geometry.finite.valid",
        finiteError.empty() ? "geometry finite validator rejected finite values" : finiteError);

    bytestore::ByteStoreSession nonFiniteGeometryStoreSession;
    nonFiniteGeometryStoreSession.BindStorage(std::make_shared<resource::ResidentByteBudget>(1024u), true);
    DecodedGeometryCache nonFiniteGeometry;
    const std::array<float, 2u> nonFiniteGeometryValues{
        1.0f,
        std::numeric_limits<float>::quiet_NaN(),
    };
    const auto nonFiniteGeometryReady = nonFiniteGeometry.Initialize(
        2u,
        1u,
        nonFiniteGeometryStoreSession,
        &finiteError) &&
        nonFiniteGeometry.bytes->WriteBytesAt(
            0u,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(nonFiniteGeometryValues.data()),
                sizeof(nonFiniteGeometryValues)),
            &finiteError) &&
        nonFiniteGeometry.bytes->Seal(&finiteError);
    nonFiniteGeometry.complete = nonFiniteGeometryReady;
    Require(
        result,
        nonFiniteGeometryReady && !validation::GeometryValidator::ValidateFiniteValues(
            finiteParams,
            nonFiniteGeometry),
        "validation.geometry.finite.invalid",
        "geometry finite validator accepted a non-finite value");

    params.topoParams.cellCount = 2u;
    params.topoParams.cellBufferSize = 6u;
    params.topoParams.hasCellTypes = 1u;
    auto topology = std::make_shared<DecodedTopologyCache>();
    topology->kind = DecodedTopologyCache::Kind::Connectivity;
    topology->cellCount = 2u;
    topology->connectivityCount = 6u;
    topology->hasCellTypes = true;
    topology->complete = true;
    Require(
        result,
        static_cast<bool>(validation::TopologyValidator::ValidateDecodedCacheShape(params, topology)),
        "validation.topology.valid",
        "topology validator rejected a matching cache");
    topology->connectivityCount = 5u;
    Require(
        result,
        !validation::TopologyValidator::ValidateDecodedCacheShape(params, topology),
        "validation.topology.invalid",
        "topology validator accepted a mismatched cache");
    topology->complete = true;
    Require(
        result,
        static_cast<bool>(validation::TopologyValidator::ValidateReferenceState(true, topology)),
        "validation.topology.reference.valid",
        "topology reference validator rejected a complete reference");
    topology->complete = false;
    Require(
        result,
        !validation::TopologyValidator::ValidateReferenceState(true, topology),
        "validation.topology.reference.invalid",
        "topology reference validator accepted an incomplete reference");

    DecodedAttributeCacheSet attributes;
    Require(
        result,
        static_cast<bool>(validation::AttributeValidator::ValidateDecodedCacheShape(
            params,
            attributes,
            {})),
        "validation.attribute.valid",
        "attribute validator rejected an empty target set");
    params.attrParams.resize(1u);
    const std::array<std::size_t, 1u> attrIndices{0u};
    Require(
        result,
        !validation::AttributeValidator::ValidateDecodedCacheShape(
            params,
            attributes,
            std::span<const std::size_t>(attrIndices)),
        "validation.attribute.invalid",
        "attribute validator accepted an uninitialized cache");

    CodecStorageParams finiteAttributeParams;
    finiteAttributeParams.attrParams.resize(1u);
    finiteAttributeParams.attrParams[0].elementCount = 2u;
    finiteAttributeParams.attrParams[0].dimension = 1;
    finiteAttributeParams.attrParams[0].dataType = DataType::Float32;
    bytestore::ByteStoreSession attributeSession;
    attributeSession.BindStorage(std::make_shared<resource::ResidentByteBudget>(1024u), true);
    DecodedAttributeCacheSet finiteAttributes;
    const std::array<float, 2u> finiteAttributeValues{3.0f, 4.0f};
    const auto finiteAttributesReady = finiteAttributes.Initialize(
        finiteAttributeParams,
        attributeSession,
        &finiteError) &&
        finiteAttributes.BeginAttribute(0u, finiteAttributeParams.attrParams[0], &finiteError) &&
        finiteAttributes.WriteAttributeRange(
            0u,
            0u,
            2u,
            finiteAttributeValues.data(),
            sizeof(finiteAttributeValues),
            &finiteError) &&
        finiteAttributes.EndAttribute(0u, &finiteError);
    Require(
        result,
        finiteAttributesReady && static_cast<bool>(
            validation::AttributeValidator::ValidateFiniteValues(
                finiteAttributeParams,
                finiteAttributes,
                std::span<const std::size_t>(attrIndices))),
        "validation.attribute.finite.valid",
        finiteError.empty() ? "attribute finite validator rejected finite values" : finiteError);

    DecodedAttributeCacheSet nonFiniteAttributes;
    const std::array<float, 2u> nonFiniteAttributeValues{
        3.0f,
        std::numeric_limits<float>::infinity(),
    };
    const auto nonFiniteAttributesReady = nonFiniteAttributes.Initialize(
        finiteAttributeParams,
        attributeSession,
        &finiteError) &&
        nonFiniteAttributes.BeginAttribute(0u, finiteAttributeParams.attrParams[0], &finiteError) &&
        nonFiniteAttributes.WriteAttributeRange(
            0u,
            0u,
            2u,
            nonFiniteAttributeValues.data(),
            sizeof(nonFiniteAttributeValues),
            &finiteError) &&
        nonFiniteAttributes.EndAttribute(0u, &finiteError);
    Require(
        result,
        nonFiniteAttributesReady && !validation::AttributeValidator::ValidateFiniteValues(
            finiteAttributeParams,
            nonFiniteAttributes,
            std::span<const std::size_t>(attrIndices)),
        "validation.attribute.finite.invalid",
        "attribute finite validator accepted a non-finite value");

    Require(
        result,
        static_cast<bool>(validation::FilterCommitValidator::ValidateDecodedOutput(true, true)),
        "validation.filter.valid",
        "filter validator rejected a successful output");
    Require(
        result,
        !validation::FilterCommitValidator::ValidateDecodedOutput(true, false),
        "validation.filter.invalid",
        "filter validator accepted a missing output");

    std::set<std::string> lifecycleNames;
    for (const auto node : validation::kDecodeValidationLifecycle) {
        lifecycleNames.emplace(validation::DecodeValidationNodeName(node));
    }
    Require(
        result,
        lifecycleNames.size() == validation::kDecodeValidationLifecycle.size(),
        "validation.lifecycle.stable",
        "decode validation lifecycle names are not unique");

    std::set<std::string_view> indexedRules;
    bool ruleCoverageValid = true;
    bool coversRequired = false;
    bool coversStrict = false;
    bool coversAudit = false;
    bool coversEncode = false;
    bool coversDecode = false;
    for (const auto& descriptor : validation::kValidationRuleIndex) {
        ruleCoverageValid = ruleCoverageValid && !descriptor.rule.empty() &&
            descriptor.policies != validation::ValidationPolicyCoverage::None &&
            descriptor.operations != validation::ValidationOperationCoverage::None &&
            indexedRules.emplace(descriptor.rule).second;
        coversRequired = coversRequired || validation::HasCoverage(
            descriptor.policies,
            validation::ValidationPolicyCoverage::Required);
        coversStrict = coversStrict || validation::HasCoverage(
            descriptor.policies,
            validation::ValidationPolicyCoverage::Strict);
        coversAudit = coversAudit || validation::HasCoverage(
            descriptor.policies,
            validation::ValidationPolicyCoverage::Audit);
        coversEncode = coversEncode || validation::HasCoverage(
            descriptor.operations,
            validation::ValidationOperationCoverage::Encode);
        coversDecode = coversDecode || validation::HasCoverage(
            descriptor.operations,
            validation::ValidationOperationCoverage::Decode);
    }
    Require(
        result,
        ruleCoverageValid,
        "validation.rules.index",
        "validation rule index contains an invalid or duplicate descriptor");
    Require(
        result,
        validation::FindValidationRule("package.preamble") != nullptr &&
            validation::FindValidationRule("filter.output.present") != nullptr,
        "validation.rules.lookup",
        "validation rule lookup failed for a production rule");
    Require(
        result,
        coversRequired && coversStrict && coversAudit &&
            coversEncode && coversDecode,
        "validation.rules.coverage",
        "validation rule index does not cover every policy and operation class");

    const auto requiredConfiguration = CodecControlParamsFactory::MakeDecodeConfiguration(
        DataCodecDecodeOptions{
            .validationProfile = DataCodecDecodeValidationProfile::Required,
        });
    const auto auditConfiguration = CodecControlParamsFactory::MakeDecodeConfiguration(
        DataCodecDecodeOptions{
            .validationProfile = DataCodecDecodeValidationProfile::Audit,
        });
    Require(
        result,
        requiredConfiguration.controlParams.validation.decodeMode ==
                DecodeValidationMode::Required &&
            !requiredConfiguration.controlParams.validation.validateTopologyReferences &&
            !requiredConfiguration.controlParams.validation.validateFloatingPointValues &&
            auditConfiguration.controlParams.validation.decodeMode ==
                DecodeValidationMode::Strict &&
            auditConfiguration.controlParams.validation.validateTopologyReferences &&
            auditConfiguration.controlParams.validation.validateFloatingPointValues,
        "validation.policy.execution",
        "validation profile switches do not select the expected execution rules");
    return result;
}

} // namespace datacodec::test

#endif
