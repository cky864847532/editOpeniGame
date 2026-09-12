#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREPACKAGEIDENTITY_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREPACKAGEIDENTITY_H

#include "DataCodec/Storage/Common/BinaryValueIO.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Storage/FramePackage/FramePackageIO.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageByteWriter.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageIO.h"
#include "DataCodec/Storage/Package/PackageBinaryHeader.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace datacodec::test {

inline std::vector<std::uint8_t> MakePackageIdentityTestHeader(
    const std::uint32_t magic,
    const std::uint16_t version,
    const PackageIdentity identity) {
    std::vector<std::uint8_t> bytes;
    detail::AppendScalar(bytes, magic);
    detail::AppendScalar(bytes, version);
    detail::AppendScalar(bytes, identity.high);
    detail::AppendScalar(bytes, identity.low);
    return bytes;
}

inline bool CheckPackageIdentityTestHeader(
    const std::uint32_t magic,
    const std::uint16_t version,
    const PackageIdentity identity,
    const PackageBinaryFormat expectedFormat) {
    MemoryByteRangeReader reader(std::make_shared<const std::vector<std::uint8_t>>(
        MakePackageIdentityTestHeader(magic, version, identity)));
    PackageInspection inspection;
    std::string error;
    return InspectPackage(reader, inspection, &error) &&
        inspection.format == expectedFormat &&
        inspection.version == version &&
        inspection.identity == identity &&
        inspection.sourceIdentity.IsStable();
}

inline void RunMultiLeafOwnerCase(TestResult& result) {
    for (const bool denyOutput : {false, true}) {
        constexpr std::uint64_t limit = 16u * 1024u * 1024u;
        const std::array<std::size_t, 3u> sizes{kIoWindowBytes + 11u, 16384u, 8192u};
        const auto payloadBytes = sizes[0] + sizes[1] + sizes[2];
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u},
            limit, 1u, false, true, false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        FramePackage frame;
        frame.rootName = "multi-owner";
        std::vector<std::shared_ptr<LeafPackage>> leaves;
        std::vector<FramePackageIO::LeafPackageWriter> writers;
        std::array<std::weak_ptr<bytestore::IByteSource>, 3u> weak;
        std::shared_ptr<bytestore::IByteSource> retained;
        std::string error;
        bool prepared = true;
        std::size_t appendCalls = 0u;
        for (std::size_t i = 0u; prepared && i < sizes.size(); ++i) {
            auto source = session.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, sizes[i], ::datacodec::MemoryDemandKind::RequiredContinuation, "multi_leaf_payload", &error);
            const std::vector<std::uint8_t> input(sizes[i], static_cast<std::uint8_t>(i + 11u));
            prepared = source && source->WriteAt(0u, input, &error) && source->Seal(&error);
            if (!prepared) { break; }
            weak[i] = source;
            if (i == 2u) { retained = source; }
            auto leaf = std::make_shared<LeafPackage>();
            leaf->path = "/" + std::to_string(i);
            const auto fields = i == 0u ? 2u : 1u;
            leaf->rawFieldBytes = leafpackagewire::ComputeRawLeafPackageSize(fields, fields * sizes[i]);
            for (std::size_t field = 0u; field < fields; ++field) {
                leaf->fields.push_back(LeafPackage::Field{.type = FieldType::Attribute,
                    .compressionType = EncodedFieldCompressionType::None, .rawSize = sizes[i], .source = source});
            }
            FramePackageIO::LeafPackageWriter writer;
            prepared = FramePackageIO::MakeFrameLeafPackageWriter(leaf, writer, &error);
            const auto append = std::move(writer.appendLeafPackage);
            writer.appendLeafPackage = [&, append](bytestore::IByteWriter& sink, std::string* failure) {
                ++appendCalls;
                return append(sink, failure);
            };
            frame.leaves.push_back(FramePackageLeafRecord{.path = leaf->path, .name = "leaf"});
            leaves.push_back(std::move(leaf));
            writers.push_back(std::move(writer));
        }
        std::uint64_t frameBytes = 0u;
        prepared &= FramePackageIO::ComputeFramePackageByteSize(frame, writers, frameBytes, &error);
        Require(result, prepared && root.StorageCapacity()->Snapshot().reservedBytes == payloadBytes,
            "packageIdentity.multi-leaf-shared-input", "leaf descriptors and segmented writers must share the three real payload owners without duplicate reservations");
        if (!prepared) { continue; }
        session.ReleaseAll();
        MemoryByteRangeOutput output(root);
        if (denyOutput) {
            root.UpdateLimits({payloadBytes + frameBytes - 1u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
            ResourceDebugSnapshot snapshot;
            Require(result, !output.PrepareExactSize(frameBytes, &error) && appendCalls == 0u &&
                root.TryCopyResourceDebugSnapshot(snapshot) && snapshot.capacityRejection &&
                snapshot.capacityRejection->requestedBytes == frameBytes &&
                snapshot.storage.reservedBytes == payloadBytes,
                "packageIdentity.multi-leaf-output-denied", "complete output must reject before consuming any leaf when live inputs and output cannot coexist");
            writers.clear();
            leaves.clear();
            retained.reset();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u && !scope.Finish(false),
                "packageIdentity.multi-leaf-denied-cleanup", "rejected output must release all leaf owners during failure cleanup");
            continue;
        }
        const bool written = output.PrepareExactSize(frameBytes, &error) &&
            FramePackageIO::WriteToSink(frame, writers, output, nullptr, &error);
        Require(result, written && appendCalls == 3u &&
            root.StorageCapacity()->Snapshot().reservedBytes == payloadBytes + frameBytes,
            "packageIdentity.multi-leaf-output-overlap", "exact complete output and all retained leaf inputs must coexist at their measured capacities");
        auto owner = std::make_shared<const EncodedBuffer>(output.TakeBytes());
        writers.clear();
        std::uint64_t remaining = payloadBytes;
        for (std::size_t i = 0u; i < leaves.size(); ++i) {
            leaves[i].reset();
            if (i != 2u) { remaining -= sizes[i]; }
            Require(result, weak[i].expired() == (i != 2u) &&
                root.StorageCapacity()->Snapshot().reservedBytes == remaining + frameBytes,
                "packageIdentity.multi-leaf-retire-" + std::to_string(i),
                "each final leaf reference must release its own capacity while an external consumer remains valid");
        }
        std::array<std::uint8_t, 1u> tail{};
        Require(result, retained->Read(sizes[2] - 1u, tail, &error) && tail[0] == 13u,
            "packageIdentity.multi-leaf-last-input", "session release and frame submission must preserve the final input consumer");
        retained.reset();
        auto reader = std::make_shared<MemoryByteRangeReader>(owner);
        FramePackage decoded;
        bool replayed = written && FramePackageIO::ReadMetadata(*reader, decoded, &error) && decoded.leaves.size() == 3u;
        std::vector<LeafPackage> parsed(3u);
        for (std::size_t i = 0u; replayed && i < parsed.size(); ++i) {
            const auto& record = decoded.leaves[i];
            replayed = LeafPackageIO::ReadFromByteRange(reader, record.leafPackageByteOffset,
                record.leafPackageByteSize, parsed[i], &error) && parsed[i].fields.size() == (i == 0u ? 2u : 1u);
            for (const auto& field : parsed[i].fields) {
                replayed &= field.source && field.source->Read(sizes[i] - 1u, tail, &error) && tail[0] == i + 11u;
            }
        }
        owner.reset();
        reader.reset();
        Require(result, replayed && scope.Finish(written) &&
            root.StorageCapacity()->Snapshot().reservedBytes == frameBytes,
            "packageIdentity.parsed-ranges-share-output", "parsed field ranges must retain exactly one complete output after the request and original reader retire");
        parsed.clear();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "packageIdentity.parsed-ranges-final-release", "the final parsed range must release the complete encoded buffer");
    }
}

[[nodiscard]] inline TestResult RunDataCodecFeaturePackageIdentity() noexcept {
    TestResult result;
    PackageIdentityBuilder firstBuilder("package-identity-test");
    firstBuilder.AddString("same-name");
    firstBuilder.AddUnsigned(1234u);
    const auto first = firstBuilder.Finish();
    PackageIdentityBuilder repeatedBuilder("package-identity-test");
    repeatedBuilder.AddString("same-name");
    repeatedBuilder.AddUnsigned(1234u);
    const auto repeated = repeatedBuilder.Finish();
    PackageIdentityBuilder secondBuilder("package-identity-test");
    secondBuilder.AddString("other-name");
    secondBuilder.AddUnsigned(1234u);
    const auto second = secondBuilder.Finish();
    Require(
        result,
        first.IsValid() && second.IsValid() && first == repeated && first != second,
        "packageIdentity.deterministic",
        "package identity is not deterministic or name-sensitive");

    constexpr std::uint64_t kTenGiB = 10ull * 1024ull * 1024ull * 1024ull;
    const auto largeFileSampleRanges = BuildPackageIdentitySampleRanges(kTenGiB);
    std::uint64_t sampledBytes = 0u;
    for (const auto& range : largeFileSampleRanges) {
        sampledBytes += static_cast<std::uint64_t>(range.byteCount);
    }
    Require(
        result,
        sampledBytes <= 192u * 1024u,
        "packageIdentity.largeFileSampleBudget",
        "10 GiB sparse identity sampling exceeds the fixed budget");

    const auto makeContentLeaf = [](const std::uint8_t attributeMarker) {
        constexpr std::size_t kFieldBytes = 16u * 1024u;
        std::vector<std::uint8_t> topologyBytes(kFieldBytes, 0x5au);
        std::vector<std::uint8_t> attributeBytes(kFieldBytes, 0x3cu);
        attributeBytes.front() = attributeMarker;
        LeafPackage leaf;
        leaf.path = "content-identity-leaf";
        leaf.rawFieldBytes = leafpackagewire::ComputeRawLeafPackageSize(
            2u,
            topologyBytes.size() + attributeBytes.size());
        leaf.fields.push_back(LeafPackage::Field{
            .type = FieldType::Topology,
            .compressionType = EncodedFieldCompressionType::None,
            .rawSize = topologyBytes.size(),
            .source = std::make_shared<bytestore::VectorByteSource>(std::move(topologyBytes)),
        });
        leaf.fields.push_back(LeafPackage::Field{
            .type = FieldType::Attribute,
            .compressionType = EncodedFieldCompressionType::None,
            .rawSize = attributeBytes.size(),
            .source = std::make_shared<bytestore::VectorByteSource>(std::move(attributeBytes)),
        });
        return leaf;
    };
    auto firstContentLeaf = makeContentLeaf(0x11u);
    auto secondContentLeaf = makeContentLeaf(0x22u);
    PackageIdentity firstContentIdentity;
    PackageIdentity secondContentIdentity;
    std::string contentIdentityError;
    const auto firstContentComputed = LeafPackageIO::ComputeLeafPackageIdentity(
        firstContentLeaf,
        firstContentIdentity,
        &contentIdentityError);
    const auto secondContentComputed = LeafPackageIO::ComputeLeafPackageIdentity(
        secondContentLeaf,
        secondContentIdentity,
        &contentIdentityError);
    Require(
        result,
        firstContentComputed && secondContentComputed &&
            firstContentIdentity != secondContentIdentity,
        "packageIdentity.attributeContent",
        contentIdentityError.empty()
            ? "attribute change did not alter package identity"
            : contentIdentityError);

    const auto identityHex = PackageIdentityToHex(first);
    PackageIdentity parsed;
    Require(
        result,
        identityHex.size() == 32u && TryParsePackageIdentityHex(identityHex, parsed) && parsed == first,
        "packageIdentity.textRoundTrip",
        "package identity text round trip failed");

    Require(
        result,
        CheckPackageIdentityTestHeader(
            leafpackagewire::kLeafPackageMagic,
            leafpackagewire::kLeafPackageVersion,
            first,
            PackageBinaryFormat::LeafPackage),
        "packageIdentity.leafHeader",
        "leaf package identity header read failed");
    Require(
        result,
        CheckPackageIdentityTestHeader(
            framepackagewire::kFramePackageMagic,
            framepackagewire::kFramePackageVersion,
            second,
            PackageBinaryFormat::FramePackage),
        "packageIdentity.frameHeader",
        "frame package identity header read failed");

    auto versionMismatchBytes = MakePackageIdentityTestHeader(
        leafpackagewire::kLeafPackageMagic,
        2u,
        first);
    MemoryByteRangeReader versionMismatchReader(
        std::make_shared<const std::vector<std::uint8_t>>(std::move(versionMismatchBytes)));
    PackageInspection versionMismatchInspection;
    std::string versionMismatchError;
    const auto versionMismatchRead = InspectPackage(
        versionMismatchReader,
        versionMismatchInspection,
        &versionMismatchError);
    Require(
        result,
        !versionMismatchRead,
        "packageIdentity.versionMismatch",
        "mismatched package version was accepted");
    Require(
        result,
        versionMismatchError == "版本不符合",
        "packageIdentity.versionMismatchError",
        "package version rejection did not use the unified version message");

    const auto sourceIdentity = MakePackageDecodeSourceIdentity(
        first,
        leafpackagewire::kLeafPackageVersion,
        1234u);
    Require(result, sourceIdentity.IsStable(), "packageIdentity.cacheStable", "package source identity is not stable");
    Require(
        result,
        sourceIdentity.stableId.find(identityHex) != std::string::npos,
        "packageIdentity.cacheId",
        "package source identity does not contain the package identity");
    Require(
        result,
        sourceIdentity.revision == "igdc-v1:bytes:1234",
        "packageIdentity.cacheRevision",
        "package source identity revision mismatch");

    DataCodecExecutionResources outputRoot(ResolvedResourceConfiguration{
        {1024u * 1024u, 1u, 1u}, 1024u * 1024u, 1u, false, true});
    MemoryByteRangeOutput leafOutput(outputRoot);
    LeafPackageByteWriter leafWriter;
    std::string leafError;
    const auto leafWritten =
        leafWriter.BeginPackageToSink(
            0u,
            0u,
            leafOutput,
            &leafError,
            "identity-test") &&
        leafWriter.EndPackage(&leafError);
    Require(result, leafWritten, "packageIdentity.leafWrite", leafError.empty() ? "leaf package write failed" : leafError);
    auto leafOwner = std::make_shared<const EncodedBuffer>(leafOutput.TakeBytes());
    if (leafWritten) {
        auto leafReader = std::make_shared<MemoryByteRangeReader>(leafOwner);
        LeafPackage decodedLeaf;
        const auto leafRead = LeafPackageIO::ReadFromByteRange(
            leafReader,
            0u,
            leafReader->ByteSize(),
            decodedLeaf,
            &leafError);
        Require(result, leafRead, "packageIdentity.leafRead", leafError.empty() ? "leaf package read failed" : leafError);
        Require(result, decodedLeaf.identity.IsValid(), "packageIdentity.leafRoundTrip", "decoded leaf identity is invalid");
    }

    MemoryByteRangeOutput repeatedLeafOutput(outputRoot);
    LeafPackageByteWriter repeatedLeafWriter;
    const auto repeatedLeafWritten =
        repeatedLeafWriter.BeginPackageToSink(
            0u,
            0u,
            repeatedLeafOutput,
            &leafError,
            "identity-test") &&
        repeatedLeafWriter.EndPackage(&leafError);
    PackageIdentity firstLeafIdentity;
    PackageIdentity repeatedLeafIdentity;
    const auto deterministicLeafIdentity =
        leafWritten && repeatedLeafWritten &&
        TryReadPackageIdentityPrefix(leafOwner->span(), firstLeafIdentity) &&
        TryReadPackageIdentityPrefix(repeatedLeafOutput.Bytes(), repeatedLeafIdentity) &&
        firstLeafIdentity == repeatedLeafIdentity;
    Require(
        result,
        deterministicLeafIdentity,
        "packageIdentity.leafDeterministic",
        "equivalent leaf packages produced different identities");

    FramePackage frame;
    frame.frameIndex = 7u;
    frame.timeValue = 1.5f;
    frame.rootName = "identity-test";
    MemoryByteRangeOutput frameOutput(outputRoot);
    std::string frameError;
    const auto frameWritten = FramePackageIO::WriteToSink(frame, {}, frameOutput, nullptr, &frameError);
    Require(result, frameWritten, "packageIdentity.frameWrite", frameError.empty() ? "frame package write failed" : frameError);
    auto frameOwner = std::make_shared<const EncodedBuffer>(frameOutput.TakeBytes());
    if (frameWritten) {
        MemoryByteRangeReader frameReader(frameOwner);
        FramePackage decodedFrame;
        const auto frameRead = FramePackageIO::ReadMetadata(frameReader, decodedFrame, &frameError);
        Require(result, frameRead, "packageIdentity.frameRead", frameError.empty() ? "frame package read failed" : frameError);
        Require(result, decodedFrame.identity.IsValid(), "packageIdentity.frameIdentity", "decoded frame identity is invalid");
        Require(result, decodedFrame.frameIndex == frame.frameIndex, "packageIdentity.frameIndex", "decoded frame index mismatch");
        Require(result, decodedFrame.rootName == frame.rootName, "packageIdentity.frameName", "decoded frame name mismatch");
    }

    MemoryByteRangeOutput repeatedFrameOutput(outputRoot);
    const auto repeatedFrameWritten = FramePackageIO::WriteToSink(
        frame,
        {},
        repeatedFrameOutput,
        nullptr,
        &frameError);
    auto renamedFrame = frame;
    renamedFrame.rootName = "identity-best";
    MemoryByteRangeOutput renamedFrameOutput(outputRoot);
    const auto renamedFrameWritten = FramePackageIO::WriteToSink(
        renamedFrame,
        {},
        renamedFrameOutput,
        nullptr,
        &frameError);
    PackageIdentity firstFrameIdentity;
    PackageIdentity repeatedFrameIdentity;
    PackageIdentity renamedFrameIdentity;
    const auto frameIdentityContract =
        frameWritten && repeatedFrameWritten && renamedFrameWritten &&
        TryReadPackageIdentityPrefix(frameOwner->span(), firstFrameIdentity) &&
        TryReadPackageIdentityPrefix(repeatedFrameOutput.Bytes(), repeatedFrameIdentity) &&
        TryReadPackageIdentityPrefix(renamedFrameOutput.Bytes(), renamedFrameIdentity) &&
        firstFrameIdentity == repeatedFrameIdentity &&
        firstFrameIdentity != renamedFrameIdentity;
    Require(
        result,
        frameIdentityContract,
        "packageIdentity.frameDeterministic",
        "frame package identity is not deterministic or name-sensitive");
    RunMultiLeafOwnerCase(result);
    return result;
}

} // namespace datacodec::test

#endif
