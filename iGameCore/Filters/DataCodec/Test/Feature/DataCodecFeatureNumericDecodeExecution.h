#ifndef DATACODEC_TEST_FEATURE_NUMERICDECODEEXECUTION_H
#define DATACODEC_TEST_FEATURE_NUMERICDECODEEXECUTION_H

#include "DataCodec/Codec/Geometry/GeometryDecode.h"
#include "DataCodec/Codec/Attributes/AttributeDecode.h"
#include "DataCodec/Runtime/Cache/TransferCache/ReferenceTransferCacheBuilder.h"
#include "DataCodec/Test/Feature/DataCodecFeatureExecutionMechanism.h"

#include <cmath>
#include <cstring>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureNumericDecodeExecution() {
    TestResult result;
    static constexpr std::uint64_t limit = 32u * 1024u * 1024u;
    {
        constexpr auto blockSize = numericarray::kSpatialBlockElementCount;
        const std::size_t count = 2u * blockSize + 7u;
        GeometryStorageParams meta;
        meta.dataType = DataType::Float64;
        meta.elementCount = count;
        meta.dimension = 3;
        meta.codecType = EncodedFieldCodecType::NumericArrayBlocks;
        std::vector<double> source(count * 3u);
        for (std::size_t i = 0u; i < source.size(); ++i) { source[i] = std::sin(i * 0.017) + i % 3u; }
        std::vector<std::uint8_t> payload;
        ScratchByteBufferPool scratch;
        CompressorConfig compressor;
        compressor.options["pressio:abs"] = 0.001;
        bool encoded = true;
        std::string error;
        for (std::size_t offset = 0u; encoded && offset < count;) {
            const auto size = std::min<std::size_t>(blockSize, count - offset);
            numericarrayreference::OrdinaryNumericArrayEncodedBlock block;
            encoded = numericarrayreference::BuildOrdinaryNumericArrayEncodedBlock(meta, compressor,
                static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(size),
                {reinterpret_cast<const std::uint8_t*>(source.data() + offset * 3u), size * 3u * sizeof(double)},
                scratch, block, &error);
            meta.blockLayouts.push_back(std::move(block.layout));
            payload.insert(payload.end(), block.bytes.begin(), block.bytes.end());
            offset += size;
        }
        Require(result, encoded, "numericDecode.fixture", error);
        if (!encoded) { return result; }
        {
            auto oversized = meta;
            oversized.elementCount = blockSize + 1u;
            oversized.blockLayouts.resize(1u);
            oversized.blockLayouts.front().elementCount = blockSize + 1u;
            DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 1u, 1u},
                limit, 1u, false, true});
            CodecRunScope scope(root);
            CacheResources resources;
            resources.BindRun(root);
            bytestore::ByteStoreSession session;
            session.BindStorage(root.StorageCapacity(), false);
            struct UnreadStream {
                std::size_t reads{0u};
                std::uint64_t Position() const noexcept { return 0u; }
                bool ReadBytes(void*, std::size_t, std::string*) { ++reads; return false; }
            } stream;
            DecodedGeometryCache geometry;
            GeometryDecodeRuntime runtime{
                .data = {.meta = oversized, .payloadBytes = oversized.blockLayouts.front().encodedByteLength},
                .cache = {.cacheResources = resources, .byteStoreSession = session, .geometry = geometry},
            };
            const auto decoded = DecodeGeometryBlocks(runtime, stream);
            Require(result, !decoded && decoded.code == CodecErrorCode::InvalidInput &&
                !decoded.message.empty() && stream.reads == 0u && !geometry.bytes &&
                root.StorageCapacity()->Snapshot().peakReservedBytes == 0u,
                "numericDecode.rejectOversized", "oversized blocks must fail before reading or allocating output");
        }
        for (const bool truncated : {false, true}) {
            DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, 2u, 4u},
                limit, 2u, true, true, false});
            CodecRunScope scope(root);
            CacheResources resources;
            resources.BindRun(root);
            bytestore::ByteStoreSession session;
            session.BindStorage(root.StorageCapacity(), false);
            CodecStorageParams storage;
            AttrStorageParams attr;
            static_cast<NumericArrayStorageParams&>(attr) = meta;
            attr.name = "bounded_attribute";
            attr.binaryCount = payload.size();
            storage.attrParams.push_back(attr);
            storage.attrPayloadOrder.push_back(0u);
            DecodedAttributeCacheSet attributes;
            bool unexpectedReference = false;
            auto referenceDecoder = [&](auto&, const auto&, const auto&, std::string*) {
                unexpectedReference = true;
                return false;
            };
            decodeimpl::detail::AttributePayloadDecodeRuntime runtime{
                .data = {.storageParams = storage},
                .cache = {.cacheResources = resources, .byteStoreSession = session, .attributes = attributes},
            };
            const std::size_t target = 0u;
            const auto input = std::span<const std::uint8_t>(payload).first(payload.size() - (truncated ? 1u : 0u));
            error.clear();
            const bool decoded = decodeimpl::detail::DecodeAttributePayloadRangesToCache(
                runtime, input, std::span<const std::size_t>(&target, 1u), referenceDecoder, &error);
            bool matches = !unexpectedReference && decoded != truncated;
            if (decoded) {
                std::vector<double> output(source.size());
                matches = matches && attributes.Complete(0u) && attributes.Bytes(0u)->Read(0u,
                    {reinterpret_cast<std::uint8_t*>(output.data()), output.size() * sizeof(double)}, &error);
                for (std::size_t i = 0u; matches && i < source.size(); ++i) {
                    matches = std::abs(output[i] - source[i]) <= 0.00101;
                }
            } else {
                matches = matches && !attributes.Complete(0u) && !attributes.Bytes(0u);
            }
            scope.Finish(decoded);
            attributes.Reset();
            ResourceDebugSnapshot snapshot;
            Require(result, matches && CopyExecutionSnapshot(root, snapshot) &&
                snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u &&
                root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "numericDecode.attributeFlow", "truncated=" + std::to_string(truncated) + ";decoded=" + std::to_string(decoded) +
                ";error=" + error + ";failure=" + (root.FirstFailure() ? root.FirstFailure()->message.data() : "none"));
        }
        for (const bool file : {false, true}) {
            for (const bool threaded : {false, true}) {
                const std::size_t compute = threaded ? 2u : 1u;
                const std::size_t slots = threaded ? 4u : 1u;
                DataCodecExecutionResources root(ResolvedResourceConfiguration{{limit, compute, slots},
                    limit, compute, threaded, true, file});
                CodecRunScope scope(root);
                CacheResources resources;
                resources.BindRun(root);
                bytestore::ByteStoreSession session;
                session.BindStorage(root.StorageCapacity(), file);
                struct Stream {
                    const std::vector<std::uint8_t>& bytes;
                    DataCodecExecutionResources& root;
                    std::size_t compute, slots, offset{0u}, reads{0u};
                    bool valid{true};
                    std::uint64_t Position() const noexcept { return offset; }
                    bool ReadBytes(void* target, std::size_t count, std::string* error) {
                        ResourceDebugSnapshot snapshot;
                        valid &= CopyExecutionSnapshot(root, snapshot) && snapshot.admittedBlocks != 0u &&
                            snapshot.activeComputeUnits != 0u && !snapshot.singleRecordFlow && count <= kIoWindowBytes;
                        if (++reads == 1u) {
                            valid &= root.UpdateLimits({limit, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
                        } else if (reads == 2u) {
                            valid &= root.UpdateLimits({limit, compute, slots}, true, ResourceDecisionReason::MechanismCheck);
                        }
                        if (offset > bytes.size() || count > bytes.size() - offset) {
                            return validation::AssignError(error, "injected truncated numeric payload");
                        }
                        std::memcpy(target, bytes.data() + offset, count);
                        offset += count;
                        return true;
                    }
                } stream{payload, root, compute, slots};
                DecodedGeometryCache geometry;
                DecodedGeometryReferenceCache reference;
                GeometryDecodeRuntime runtime{
                    .data = {.meta = meta, .payloadBytes = payload.size()},
                    .cache = {.cacheResources = resources, .byteStoreSession = session,
                        .geometry = geometry, .referenceCache = &reference},
                };
                const auto decoded = DecodeGeometryBlocks(runtime, stream);
                bool matches = decoded && stream.valid && geometry.complete && reference.IsComplete();
                std::vector<float> points(source.size());
                std::vector<double> original(source.size());
                matches = matches && geometry.bytes->Read(0u,
                    {reinterpret_cast<std::uint8_t*>(points.data()), points.size() * sizeof(float)}, &error) &&
                    reference.ReadRange(0u, count, original.data(), original.size() * sizeof(double), &error);
                for (std::size_t i = 0u; matches && i < source.size(); ++i) {
                    matches = std::abs(original[i] - source[i]) <= 0.00101 &&
                        points[i] == static_cast<float>(original[i]);
                }
                ResourceDebugSnapshot snapshot;
                Require(result, matches && scope.Finish(decoded.success) && CopyExecutionSnapshot(root, snapshot) &&
                    snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u &&
                    snapshot.lastRetired == 2u && snapshot.limits.slotLimit == slots,
                    "numericDecode.geometryFlow", decoded.message.empty() ?
                        "numeric decode must preserve conversion, raw reference and bounded block admission" : decoded.message);
                geometry.Release();
                reference.Reset();
                Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u &&
                    root.Scratch().SnapshotStats().activeBlockCount == 0u,
                    "numericDecode.ownerRelease", "geometry and reference owners must release their separate capacity");
            }
        }
    }
    return result;
}

} // DataCodec 测试命名空间

#endif
