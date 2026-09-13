#include "DataCodec/Storage/ByteStore/ByteStoreInterface.h"

// 适配器实现存储接口时不得引入执行资源和具体存储
#if defined(DATACODEC_STORAGE_BYTESTORE_BYTESTORE_H) || defined(DATACODEC_RUNTIME_EXECUTION_DATACODECEXECUTIONRESOURCES_H)
#error Native storage interface includes runtime implementation
#endif

#include "DataCodec/API/Entry/PackageDecodeSession.h"
#include "DataCodec/Storage/ByteIO/FileByteRangeIO.h"
#include "DataCodec/Test/Feature/DataCodecFeatureAdapterRoundTrip.h"

#include <iostream>

namespace {

datacodec::test::TestResult TestPackageSession() {
    using namespace datacodec;
    using namespace datacodec::test;
    TestResult result;
    const auto dataset = MakeAdapterRoundTripDataset();
    TestEncodeAdapter encodeAdapter(dataset);
    auto encoded = Encode({
        .input = EncodeInput::LeafAdapter(&encodeAdapter, {}, dataset.name, "PointSet"),
        .output = EncodeOutput::Memory(EncodePackageKind::LeafPackage),
        .attributeSelection = AttributeSelectionMode::AllAvailable,
        .resources = {.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u},
    });
    if (!Require(result, encoded.success, "session.encode", "encoding failed")) return result;
    const auto reader = std::make_shared<MemoryByteRangeReader>(std::move(encoded.encodedBytes));
    TestDecodeAdapter initial;
    PackageDecodeSession session;
    PackageDecodeSessionOpenRequest open{
        .decode = {
            .inputReader = reader,
            .leafAdapter = &initial,
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
    Require(result, initial.Points() == dataset.points && initial.Attributes().empty(),
        "session.geometry", "geometry-only open produced unexpected data");
    Require(result, session.InputCacheStatistics().stores > 0u,
        "session.cache", "session did not own the encoded input cache");
    std::vector<AttributeTarget> targets;
    for (const auto& descriptor : session.AvailableAttributes()) targets.push_back(descriptor.target);
    if (!Require(result, targets.size() == dataset.pointFields.size(), "session.catalog", "attribute catalog mismatch")) return result;

    int created = 0;
    PackageDecodeSessionAttributeRequest attributes{
        .targets = targets,
        .mode = AttributeDecodeRequestMode::DecodeToCache,
        .createAdapter = [&](const BlockPath&, std::string*) -> std::unique_ptr<IDecodeAdapter> {
            ++created;
            return std::make_unique<TestDecodeAdapter>();
        },
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
    bool inspected = false;
    attributes.afterDecode = [&](IDecodeAdapter& adapter, std::span<const AttributeTarget>) {
        inspected = true;
        const auto& actual = static_cast<TestDecodeAdapter&>(adapter).Attributes();
        Require(result, actual.size() == dataset.pointFields.size(), "session.attribute-count", "attribute count mismatch");
        for (std::size_t i = 0; i < std::min(actual.size(), dataset.pointFields.size()); ++i) {
            const auto& expected = dataset.pointFields[i].values;
            Require(result, actual[i].complete && actual[i].bytes.size() == expected.size() * sizeof(float) &&
                std::memcmp(actual[i].bytes.data(), expected.data(), expected.size() * sizeof(float)) == 0,
                "session.attribute-values", "committed attribute differs from the source");
        }
    };
    const auto committed = session.RequestAttributes(attributes);
    Require(result, committed.success && inspected && created == 1,
        "session.commit", "cached commit did not reuse the prepared adapter");
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
    attributes.createAdapter = [](const BlockPath&, std::string*) -> std::unique_ptr<IDecodeAdapter> {
        throw std::runtime_error("injected adapter failure");
    };
    const auto failed = session.RequestAttributes(attributes);
    Require(result, !failed.success && failed.failure && !session.IsOpen(),
        "session.exception", "adapter exception did not close the session");
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
        return roundTrip && session ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
