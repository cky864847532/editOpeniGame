#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREOUTPUTRESOURCES_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREOUTPUTRESOURCES_H

#include "DataCodec/Runtime/Cache/DecodeCacheRuntime.h"
#include "DataCodec/Storage/ByteStore/SegmentedBinaryObject.h"
#include "DataCodec/Storage/LeafPackage/LeafPackageFieldEncode.h"
#include "DataCodec/Storage/LeafPackage/EncodedLeafFieldBundle.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <array>
#include <thread>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureOutputResources() {
    TestResult result;
    for (const bool externalSpill : {false, true}) {
        const auto limit = externalSpill ? 0u : 8u;
        DataCodecExecutionResources root(ResolvedResourceConfiguration{
            {limit, 1u, 1u}, limit, 1u, false, true, externalSpill});
        CodecRunScope request(root);
        auto phase = WaitForHeavyPhase(root);
        bytestore::ByteStoreSession stores;
        stores.BindRun(root);
        auto source = stores.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, 8u, "params_test");
        const std::array<std::uint8_t, 8u> expected{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        LeafPackage package;
        const bool packaged = phase && source && source->WriteBytesAt(0u, expected) && source->Seal() &&
            BuildLeafPackageFromEncodedFieldBundle("params-owner", source, {}, package);
        Require(result, packaged && package.fields.front().source.get() == source.get(),
            "output.params-shared-owner", "leaf packaging must reuse the controlled parameter owner");
        source.reset();
        stores.ReleaseAll();
        std::array<std::uint8_t, 8u> actual{};
        Require(result, packaged && package.fields.front().source->Read(0u, actual) && actual == expected &&
            root.StorageCapacity()->Snapshot().reservedBytes == limit,
            "output.params-session-release", "parameter source must survive session registration cleanup");
        package = {};
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "output.params-final-release", "the final package reference must release parameter capacity");
    }
    {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, false, true});
        CodecRunScope request(root);
        bytestore::ByteStoreSession stores;
        stores.BindRun(root);
        auto cached = stores.CreateMemoryStore();
        const std::array<std::uint8_t, 32u> cacheBytes{};
        cached->Append(cacheBytes);
        cached->Seal();
        auto reader = std::make_shared<MemoryByteRangeReader>(
            std::static_pointer_cast<const void>(cached), cached->ContiguousBytes());
        auto cache = root.Caches().DefaultEncodedInputCache();
        const bool cachedOk = cache->Store({"output-growth", "1"}, reader,
            EncodedInputAccessKind::UserRequest).IsStored();
        reader.reset();
        cached.reset();
        auto phase = WaitForHeavyPhase(root);
        auto output = stores.CreateMemoryStore();
        bytestore::AppendableByteStoreWriter writer(output, root);
        const std::array<std::uint8_t, 24u> first{};
        const std::array<std::uint8_t, 1u> last{7u};
        const bool grew = writer.Write(first) && writer.Write(last);
        Require(result, cachedOk && phase && grew && output->ByteSizeHint() == 25u &&
            cache->Statistics().residentInputs == 0u && root.StorageCapacity()->Snapshot().reservedBytes == 36u &&
            root.StorageCapacity()->Snapshot().peakReservedBytes == 60u,
            "output.driver-growth-reclaim",
            "driver growth must drop optional cache references before reserving the complete replacement array");
        output.reset();
        phase.reset();
    }
    for (const auto limit : {12u, 16u}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u}, limit, 1u, false, true});
        CodecRunScope request(root);
        bytestore::ByteStoreSession stores;
        stores.BindRun(root);
        auto source = stores.CreateMemoryStore();
        const std::array<std::uint8_t, 8u> input{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        source->Append(input);
        source->Seal();
        bytestore::SegmentedBinaryObject segmented;
        segmented.AddSegment(source);
        source.reset();
        EncodedBuffer output;
        std::string error;
        const bool copied = segmented.Materialize(root, output, &error);
        if (limit == 12u) {
            Require(result, !copied && segmented.CanRead() && output.empty() &&
                root.StorageCapacity()->Snapshot().reservedBytes == 8u,
                "output.materialize-admission",
                "a denied complete output must preserve all one-shot source segments");
        } else {
            Require(result, copied && !segmented.CanRead() && output.size() == input.size() &&
                std::equal(input.begin(), input.end(), output.span().begin()) &&
                root.StorageCapacity()->Snapshot().reservedBytes == 8u &&
                root.StorageCapacity()->Snapshot().peakReservedBytes == 16u,
                "output.materialize-owner",
                "materialization must consume segments only after full admission and return the same controlled owner");
        }
    }
    for (const auto compute : {1u, 2u, 4u}) {
        DataCodecExecutionResources root(ResolvedResourceConfiguration{
            {0u, compute, compute}, 0u, compute, true, true});
        CodecRunScope request(root);
        auto phase = WaitForHeavyPhase(root);
        if (!request || !phase) {
            Require(result, false, "output.package-admission", "package test requires a live phase");
            continue;
        }
        const auto driver = std::this_thread::get_id();
        const auto expectedUnits = compute >= 3u ? compute : 1u;
        class Writer final : public bytestore::IByteWriter {
        public:
            Writer(DataCodecExecutionResources& root, std::thread::id driver, std::size_t expected)
                : root(root), driver(driver), expected(expected) {}
            bool Write(std::span<const std::uint8_t> data, std::string*) override {
                ++writes;
                ResourceDebugSnapshot snapshot;
                if (root.TryCopyResourceDebugSnapshot(snapshot)) {
                    observed = true;
                    valid &= snapshot.heavyPhaseAdmitted && snapshot.activeComputeUnits == expected &&
                        snapshot.exclusiveUnits == expected && std::this_thread::get_id() != driver;
                }
                bytes.insert(bytes.end(), data.begin(), data.end());
                return true;
            }
            std::uint64_t ByteSizeHint() const noexcept override { return bytes.size(); }
            DataCodecExecutionResources& root;
            std::thread::id driver;
            std::size_t expected;
            std::size_t writes{0u};
            bool observed{false};
            bool valid{true};
            std::vector<std::uint8_t> bytes;
        } writer(root, driver, expectedUnits);
        std::vector<std::uint8_t> raw(65536u, 13u);
        bytestore::VectorByteSource input(raw);
        EncodedFieldCompressionType compression{};
        std::uint64_t rawSize = 0u;
        std::string error;
        const bool encoded = EncodeLeafPackageFieldToWriter(input, FieldType::Geometry,
            PackageFieldEncodingParams{}, {.run = root, .phase = *phase},
            writer, compression, rawSize, &error);
        std::vector<std::uint8_t> decoded;
        const bool decodedOk = encoded && codec::ZstdCodec::Decompress(writer.bytes, raw.size(), decoded, &error);
        Require(result, encoded && writer.writes != 0u && writer.observed && writer.valid &&
            decodedOk && decoded == raw && rawSize == raw.size(),
            "output.package-compute-units",
            "package Zstd must execute exclusively using acquired units and preserve the field bytes");
        phase.reset();
        Require(result, request.Finish(encoded), "output.package-retirement",
            "package computation and output phase must both retire before request completion");
    }
    {
        codec::ZstdStreamingEncoder encoder;
        std::string error;
        Require(result, !encoder.Initialize(3, 0u, 8u, &error),
            "output.no-worker-recommendation", "zero compute units must not trigger hardware worker recommendation");
    }
    return result;
}

}

#endif
