#include "DataCodec/Storage/ByteIO/EncodedInputAccess.h"
#include "DataCodec/Storage/ByteStore/ByteStoreInterface.h"

// 适配器实现存储接口时不得引入执行资源和具体存储
#if defined(DATACODEC_STORAGE_BYTESTORE_BYTESTORE_H) || defined(DATACODEC_RUNTIME_EXECUTION_DATACODECEXECUTIONRESOURCES_H)
#error Native storage interface includes runtime implementation
#endif

#include "DataCodec/API/Entry/PackageDecodeSession.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include "DataCodec/Test/Feature/DataCodecFeatureAdapterRoundTrip.h"
#include "DataCodec/Test/Feature/DataCodecFeatureBridgeAudit.h"

#include <iostream>

namespace {

datacodec::test::TestResult TestIndependentAttributeDecode() {
    using namespace datacodec;
    using namespace datacodec::test;
    TestResult result;
    // 同类字段跨边界供给，并覆盖多块尾部与不同分量类型的切换
    for (const std::size_t count : std::array<std::size_t, 2>{17u, numericarray::kSpatialBlockElementCount + 3u}) {
        TestDataset data;
        data.points.resize(count * 3u);
        for (std::size_t i = 0; i < count; ++i) { data.points[i * 3u] = static_cast<float>(i); }
        for (std::size_t f = 0; f < 6u; ++f) {
            TestNumericField field;
            field.name = "independent_" + std::to_string(f);
            field.componentCount = f == 2u || f == 3u ? 3u : 1u;
            field.values.resize(count * field.componentCount);
            for (std::size_t i = 0; i < field.values.size(); ++i) { field.values[i] = static_cast<float>((i * 17u + f * 31u) % 65536u) / 1024.0f; }
            data.pointFields.push_back(std::move(field));
        }
        auto configuration = MakeDefaultEncodeConfigurationParams();
        configuration.controlParams.attrReference.intraField.codec = IntraFieldReferenceCodec::Disabled;
        auto encoded = Encode({.input = EncodeInput::LeafAdapter(std::make_shared<TestEncodeAdapter>(data)),
            .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage), .configuration = configuration,
            .resources = {.mode = CodecResourceMode::Unlimited}});
        if (!Require(result, encoded.success, "independent.encode", "independent fixture encoding failed")) { return result; }
        auto owner = std::make_shared<const EncodedBuffer>(std::move(encoded.encodedBytes));
        for (const bool subset : {false, true}) {
            DecodePackageRequest request{.input = EncodedInput::Memory(owner), .resources = {.mode = CodecResourceMode::Unlimited}};
            if (subset) {
                request.attributeSelection = AttributeSelectionMode::Explicit;
                request.attributeTargets = {{.attrIndex = 1u}, {.attrIndex = 3u}, {.attrIndex = 5u}};
            }
            const auto decoded = DecodePackage(request);
            if (!Require(result, decoded.success && decoded.output.leaves.size() == 1u,
                    "independent.decode", "independent fields did not decode")) { return result; }
            const auto& leaf = decoded.output.leaves.front();
            Require(result, leaf.attributes.size() == (subset ? 3u : 6u), "independent.count", "unexpected attribute selection");
            const auto* points = reinterpret_cast<const float*>(leaf.geometry.values.data());
            for (const auto& field : leaf.attributes) {
                const auto& expected = data.pointFields.at(field.sourceIndex);
                bool equal = field.values.size() == expected.values.size() * sizeof(float);
                const auto* values = reinterpret_cast<const float*>(field.values.data());
                for (std::size_t i = 0; equal && i < count; ++i) {
                    const auto source = static_cast<std::size_t>(points[i * 3u]);
                    for (std::size_t c = 0; c < expected.componentCount; ++c) {
                        equal &= source < count && values[i * expected.componentCount + c] == expected.values[source * expected.componentCount + c];
                    }
                }
                Require(result, equal, "independent.values", "independent field values differ from the source");
            }
        }
        // 截断输入必须失败，失败请求结束后仍能重新解码完整输入
        const auto failed = DecodePackage({.input = EncodedInput::Memory(owner, owner->span().first(owner->size() / 2u)),
            .resources = {.mode = CodecResourceMode::Unlimited}});
        Require(result, !failed.success, "independent.truncated", "truncated input unexpectedly succeeded");
        const auto reopened = DecodePackage({.input = EncodedInput::Memory(owner), .resources = {.mode = CodecResourceMode::Unlimited}});
        Require(result, reopened.success, "independent.retry", "decoding after failure did not recover");
    }
    return result;
}

datacodec::test::TestResult TestPackageSession() {
    using namespace datacodec;
    using namespace datacodec::test;
    TestResult result;
    const auto dataset = MakeAdapterRoundTripDataset();
    auto encodeAdapterOwner = std::make_shared<TestEncodeAdapter>(dataset);
    auto& encodeAdapter = *encodeAdapterOwner;
    auto encoded = Encode({
        .input = EncodeInput::LeafAdapter(encodeAdapterOwner, {}, dataset.name, "PointSet"),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
        .attributeSelection = AttributeSelectionMode::AllAvailable,
        .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u},
    });
    if (!Require(result, encoded.success, "session.encode", "encoding failed")) return result;
    const auto reader = std::make_shared<MemoryByteRangeReader>(std::move(encoded.encodedBytes));
    PackageDecodeSession session;
    PackageDecodeSessionOpenRequest open{
        .decode = {
            .input = ::datacodec::EncodedInputAccess::Retain(reader),
            .attributeSelection = AttributeSelectionMode::None,
            .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u},
        },
        .sourceIdentity = {.stableId = "core.session.test", .revision = "1"},
        .encodedInputCachePolicy = {.enabled = true},
    };
    const auto opened = session.Open(open);
    if (!Require(result, opened.success && session.IsOpen(), "session.open", "session could not open")) {
        if (opened.failure) result.AddDiagnostic(FormatCodecFailure(*opened.failure));
        return result;
    }
    Require(result, opened.output.leaves.size() == 1u && opened.output.leaves[0].attributes.empty() &&
        opened.output.leaves[0].geometry.values.size() == dataset.points.size() * sizeof(float) &&
        std::memcmp(opened.output.leaves[0].geometry.values.data(), dataset.points.data(), dataset.points.size() * sizeof(float)) == 0,
        "session.geometry", "geometry-only open produced unexpected data");
    Require(result, session.InputCacheStatistics().stores > 0u,
        "session.cache", "session did not own the encoded input cache");
    std::vector<AttributeTarget> targets;
    for (const auto& descriptor : session.AvailableAttributes()) targets.push_back(descriptor.target);
    if (!Require(result, targets.size() == dataset.pointFields.size(), "session.catalog", "attribute catalog mismatch")) return result;

    PackageDecodeSessionAttributeRequest attributes{
        .targets = targets,
        .mode = AttributeDecodeRequestMode::DecodeToCache,
    };
    const auto prepared = session.RequestAttributes(attributes);
    if (!Require(result, prepared.success, "session.prepare", "attribute preparation failed")) {
        if (prepared.failure) result.AddDiagnostic(FormatCodecFailure(*prepared.failure));
        return result;
    }
    for (const auto& descriptor : session.AvailableAttributes()) {
        Require(result, descriptor.decoded && !descriptor.committed,
            "session.prepare-state", "preparation committed output prematurely");
    }
    attributes.mode = AttributeDecodeRequestMode::CommitCached;
    const auto committed = session.RequestAttributes(attributes);
    if (committed.success && committed.output.leaves.size() == 1u) {
        const auto& actual = committed.output.leaves[0].attributes;
        Require(result, actual.size() == dataset.pointFields.size(), "session.attribute-count", "attribute count mismatch");
        for (std::size_t i = 0; i < std::min(actual.size(), dataset.pointFields.size()); ++i) {
            const auto& expected = dataset.pointFields[i].values;
            Require(result, actual[i].values.size() == expected.size() * sizeof(float) &&
                std::memcmp(actual[i].values.data(), expected.data(), expected.size() * sizeof(float)) == 0,
                "session.attribute-values", "committed attribute differs from the source");
        }
    }
    Require(result, committed.success && committed.output.leaves.size() == 1u && prepared.output.leaves.empty(),
        "session.commit", "cached commit did not deliver owned attributes");
    for (const auto& descriptor : session.AvailableAttributes()) {
        Require(result, descriptor.committed, "session.commit-state", "committed attribute missing from catalog");
    }
    session.Reset();
    Require(result, !session.IsOpen() && session.AvailableAttributes().empty(), "session.reset", "session retained open state");

    Require(result, session.Open(open).success, "session.reopen", "session could not reopen");
    std::stop_source stop;
    stop.request_stop();
    attributes.mode = AttributeDecodeRequestMode::DecodeAndCommit;
    attributes.stopToken = stop.get_token();
    const auto cancelled = session.RequestAttributes(attributes);
    Require(result, !cancelled.success && cancelled.cancelled && !session.IsOpen(),
        "session.cancel", "cancelled request did not clean up its session");
    Require(result, session.Open(open).success, "session.reopen-after-cancel", "cancelled session could not reopen");
    attributes.stopToken = {};
    attributes.targets.front().attrIndex = std::numeric_limits<std::size_t>::max();
    const auto failed = session.RequestAttributes(attributes);
    Require(result, !failed.success && failed.failure && !session.IsOpen(),
        "session.invalid-target", "invalid attribute request did not close the session");
    Require(result, session.Open(open).success, "session.reopen-after-failure", "failed session could not reopen");
    return result;
}

bool PrintResult(const datacodec::test::TestResult& result) {
    for (const auto& failure : result.failures) {
        std::cerr << failure.check << ": " << failure.message << '\n';
    }
    if (!result.passed) for (const auto& message : result.diagnostics) std::cerr << message << '\n';
    return result.passed;
}

} // 匿名命名空间

int main() {
    try {
        const bool roundTrip = PrintResult(datacodec::test::RunDataCodecFeatureAdapterRoundTrip());
        const bool session = PrintResult(TestPackageSession());
        const bool bridge = PrintResult(datacodec::test::RunDataCodecFeatureBridgeAudit());
        const bool independent = PrintResult(TestIndependentAttributeDecode());
        return roundTrip && session && bridge && independent ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
