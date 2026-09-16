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
        return roundTrip && session && bridge ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
