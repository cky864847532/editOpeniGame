#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREMORTONRESOURCES_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREMORTONRESOURCES_H

#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Codec/Remap/PointRemapBuilder.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Feature/DataCodecFeatureRemapAnalysis.h"

#include <array>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureMortonResources() {
    TestResult result = RunDataCodecFeatureRemapAnalysis();
    using namespace mortonremap;
    constexpr std::size_t MiB = 1024u * 1024u;
    static_assert(kMortonLeafBytes == 8u * MiB && kMortonRunBufferBytes == MiB);
    static_assert(ResolveLeafBudgetElements() == (8u * MiB) / 10u);
    {
        constexpr std::size_t count = 6u;
        constexpr auto bytes = count * sizeof(IndexType);
        DataCodecExecutionResources root({{bytes, 1u, 1u}, bytes, 1u, false, true, false});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        MortonRemapOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "owners"),
            .byteStoreSession = &session, .buildInverse = true};
        MortonRemapResult output;
        std::size_t keyCalls = 0u;
        const bool refused = !BuildMortonRemapProvider(count, [&](std::size_t i) {
            ++keyCalls;
            return static_cast<std::uint32_t>(i);
        }, output, options);
        ResourceDebugSnapshot snapshot;
        Require(result, refused && keyCalls == 0u && !output.orderProvider &&
            root.TryCopyResourceDebugSnapshot(snapshot) && snapshot.storage.reservedBytes == 0u &&
            snapshot.capacityRejection && snapshot.capacityRejection->ownerCount == 1u &&
            snapshot.capacityRejection->owners[0].capacityBytes == bytes &&
            snapshot.capacityRejection->requestedBytes == bytes,
            "morton.inverse-owner-refusal", "inverse refusal must identify the live order owner before any key computation and roll back both outputs");
    }

    // 小数据经相同定长 provider 发布，稳定次序与逆序必须同时成立
    for (const bool useFile : {false, true}) {
        const std::array<std::uint32_t, 6u> keys{9u, 1u, 9u, 0u, 1u, 3u};
        const std::vector<IndexType> expected{3, 1, 4, 5, 0, 2};
        const std::vector<IndexType> expectedInverse{4, 1, 5, 0, 2, 3};
        const auto bytes = useFile ? 0u : 2u * keys.size() * sizeof(IndexType);
        DataCodecExecutionResources root({{bytes, 1u, 1u}, bytes, 1u, false, true, useFile});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), useFile);
        MortonRemapOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "small"),
            .byteStoreSession = &session, .buildInverse = true};
        MortonRemapResult output;
        bool admittedCompute = true;
        bool sampledArrays = false;
        const auto driver = std::this_thread::get_id();
        options.recordCapacitySamples = [&](std::span<const BufferCapacitySample> samples) {
            ResourceDebugSnapshot snapshot;
            sampledArrays = root.TryCopyResourceDebugSnapshot(snapshot) && snapshot.heavyPhaseAdmitted &&
                snapshot.activeComputeUnits == 0u && driver == std::this_thread::get_id() &&
                samples[static_cast<std::size_t>(MortonArraySample::Keyed)].capacityBytes.value_or(0u) >= keys.size() * sizeof(MortonKeyedIndex) &&
                samples[static_cast<std::size_t>(MortonArraySample::Order)].capacityBytes.value_or(0u) >= keys.size() * sizeof(IndexType);
        };
        const auto getter = [&](std::size_t i) {
            ResourceDebugSnapshot snapshot;
            admittedCompute &= root.TryCopyResourceDebugSnapshot(snapshot) &&
                snapshot.heavyPhaseAdmitted && snapshot.activeComputeUnits == 1u;
            return keys[i];
        };
        const bool success = BuildMortonRemapProvider(keys.size(), getter, output, options);
        std::vector<IndexType> order, inverse;
        Require(result, success && admittedCompute && sampledArrays && output.orderProvider && output.inverseProvider &&
            output.orderProvider->ReadRange(0u, keys.size(), order, nullptr) && order == expected &&
            output.inverseProvider->ReadRange(0u, keys.size(), inverse, nullptr) && inverse == expectedInverse &&
            root.StorageCapacity()->Snapshot().reservedBytes == bytes,
            "morton.small-publish", "small sorting must publish stable order and inverse through exact owners");
        output = {};
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "morton.small-release", "published small remap must release its complete capacity");
        options.providerFactory = {};
        Require(result, !BuildMortonRemapProvider(keys.size(), getter, output, options) && !output.orderProvider,
            "morton.no-unmanaged-exit", "small remap must reject a missing provider factory before sorting");
    }

    {
        constexpr std::size_t count = numericarray::kSpatialBlockElementCount + 65u;
        std::vector<float> points(count * 3u);
        for (std::size_t i = 0u; i < count; ++i) {
            points[i * 3u] = static_cast<float>(i % 257u) - 128.0f;
            points[i * 3u + 1u] = static_cast<float>(i % 113u) - 56.0f;
            points[i * 3u + 2u] = static_cast<float>(i % 67u) - 33.0f;
        }
        points.back() = -200.0f;
        DataCodecExecutionResources root({{64u * MiB, 2u, 4u}, 64u * MiB, 2u, true, true, true});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        pointremap::BuildOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "point-parallel"),
            .byteStoreSession = &session};
        std::vector<IndexType> expected, expectedInverse;
        for (unsigned layout = 0u; layout < 3u; ++layout) {
            pointremap::RemapProviders output;
            NumericArrayView view;
            view.scalarType = ScalarType::Float32;
            view.layout = layout == 0u ? ArrayLayout::GetterOnly : ArrayLayout::CompactAOS;
            view.data = points.data();
            view.tupleCount = count;
            view.componentCount = 3;
            view.userData = points.data();
            view.getTupleBytes = [](const void* data, std::size_t i, void* value, std::string*) {
                std::memcpy(value, static_cast<const float*>(data) + i * 3u, 3u * sizeof(float));
                return true;
            };
            const bool success = layout == 2u
                ? pointremap::BuildPointMortonRemapProviders(points.data(), count, output, nullptr, options)
                : pointremap::BuildPointMortonRemapProviders(view, output, nullptr, options);
            std::vector<IndexType> order, inverse;
            const bool read = success && output.orderProvider && output.inverseProvider &&
                output.orderProvider->ReadRange(0u, count, order, nullptr) &&
                output.inverseProvider->ReadRange(0u, count, inverse, nullptr);
            if (layout == 0u) { expected = order; expectedInverse = inverse; }
            Require(result, read && order == expected && inverse == expectedInverse,
                "morton.point-bounds", "parallel bounds and keys must match the sequential getter for negative coordinates and a tail extremum");
        }
    }

    // 跨两个算法规模验证并行键生成、重复键稳定次序与完整逆映射
    for (const auto count : std::array<std::size_t, 2u>{2u * numericarray::kSpatialBlockElementCount + 17u,
                            ResolveLeafBudgetElements() + 17u}) {
        DataCodecExecutionResources root({{64u * MiB, 2u, 4u}, 64u * MiB, 2u, true, true, true});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        MortonRemapOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "parallel"),
            .byteStoreSession = &session, .buildInverse = true, .parallelKeyRead = true};
        std::mutex mutex;
        std::condition_variable changed;
        unsigned entered = 0u;
        bool concurrent = true;
        const auto getter = [&](std::size_t i) {
            if (i == 0u || i == numericarray::kSpatialBlockElementCount) {
                std::unique_lock lock(mutex);
                ++entered;
                changed.notify_all();
                concurrent &= changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered == 2u; });
            }
            return static_cast<std::uint32_t>(16u - i % 17u);
        };
        MortonRemapResult output;
        const bool success = BuildMortonRemapProvider(count, getter, output, options);
        std::vector<IndexType> order, inverse, expected;
        expected.reserve(count);
        for (std::size_t residue = 17u; residue != 0u;) {
            --residue;
            for (auto i = residue; i < count; i += 17u) { expected.push_back(static_cast<IndexType>(i)); }
        }
        bool valid = success && concurrent && entered == 2u && output.orderProvider && output.inverseProvider &&
            output.orderProvider->ReadRange(0u, count, order, nullptr) && order == expected &&
            output.inverseProvider->ReadRange(0u, count, inverse, nullptr);
        for (std::size_t i = 0u; valid && i < count; ++i) { valid = inverse[order[i]] == i; }
        Require(result, valid, "morton.parallel-stable", "concurrent key ranges must preserve stable order and every inverse entry");
        output = {};
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "morton.parallel-release", "parallel remap must release its cache and output owners");
    }
    for (const bool cancel : {false, true}) {
        DataCodecExecutionResources root({{64u * MiB, 2u, 4u}, 64u * MiB, 2u, true, true, true});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindRun(root);
        MortonRemapOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "parallel-failure"),
            .byteStoreSession = &session, .buildInverse = true, .parallelKeyRead = true};
        MortonRemapResult output;
        const bool success = BuildMortonRemapProvider(ResolveLeafBudgetElements() + 17u,
            [&](std::size_t i) -> std::uint32_t {
                if (i == numericarray::kSpatialBlockElementCount) {
                    if (cancel) { root.RequestStop(); }
                    else { throw std::runtime_error("parallel key failure"); }
                }
                return 0u;
            }, output, options);
        ResourceDebugSnapshot snapshot;
        Require(result, !success && !output.orderProvider && root.TryCopyResourceDebugSnapshot(snapshot) &&
            snapshot.activeComputeUnits == 0u && snapshot.admittedBlocks == 0u &&
            root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "morton.parallel-drain", "key exceptions and cancellation must drain workers before releasing cache storage");
        scope.Finish(false);
    }

    // 桶集中与全部桶分散分别验证真实切片和窗口尾部
    for (const bool dispersed : {false, true}) {
        const std::size_t count = dispersed ? kMortonBucketCount16 : 403u;
        const std::size_t slabBytes = dispersed ? count * kMortonRunRecordBytes : 1026u;
        for (const bool useFile : {false, true}) {
            const std::size_t runBytes = count * kMortonRunRecordBytes;
            const auto limit = slabBytes + (useFile ? 0u : runBytes);
            DataCodecExecutionResources root({{limit, 1u, 1u}, limit, 1u, false, true, useFile});
            CodecRunScope scope(root);
            bytestore::ByteStoreSession session;
            session.BindStorage(root.StorageCapacity(), useFile);
            std::vector<std::size_t> counts(kMortonBucketCount16, 0u);
            std::vector<std::size_t> offsets(kMortonBucketCount16, 0u), written;
            if (dispersed) { std::fill(counts.begin(), counts.end(), 1u); }
            else { counts[7u] = count; }
            for (std::size_t i = 1u; i < offsets.size(); ++i) { offsets[i] = offsets[i - 1u] + counts[i - 1u]; }
            std::uint64_t observedSlab = 0u;
            bool sampledSliceTables = false;
            MortonRemapOptions options{.resources = root,
                .recordCapacitySamples = [&](std::span<const BufferCapacitySample> samples) {
                    sampledSliceTables = samples[static_cast<std::size_t>(MortonArraySample::SliceOffsets)].capacityBytes.value_or(0u) >=
                        (kMortonBucketCount16 + 1u) * sizeof(std::size_t);
                }, .byteStoreSession = &session};
            const auto getter = [&](std::size_t i) {
                observedSlab = root.StorageCapacity()->AllocatedStorage().liveBytes - (useFile ? 0u : runBytes);
                const auto high = dispersed ? count - 1u - i : 7u;
                return static_cast<std::uint32_t>((high << 16u) | (i & 0xffffu));
            };
            auto phase = WaitForHeavyPhase(root);
            RemapScratchRun run;
            const bool success = WriteHighBucketRun(count, getter, counts, offsets, written, session, run, options, *phase);
            Require(result, success && sampledSliceTables && observedSlab == slabBytes &&
                root.StorageCapacity()->Snapshot().reservedBytes == (useFile ? 0u : runBytes),
                "morton.slab-exact", "a high run must release its exact slab as soon as bucket writing completes");
            bool replayOk = success;
            std::array<std::uint8_t, 17u * kMortonRunRecordBytes> window{};
            for (std::size_t begin = 0u; replayOk && begin < count; begin += 17u) {
                const auto n = std::min<std::size_t>(17u, count - begin);
                replayOk = run.ReadRecordBytes(begin, std::span(window).first(n * kMortonRunRecordBytes));
                for (std::size_t i = 0u; replayOk && i < n; ++i) {
                    const auto expected = dispersed ? count - 1u - (begin + i) : begin + i;
                    const auto* record = window.data() + i * kMortonRunRecordBytes;
                    replayOk = DecodeRunElementId(record) == expected && DecodeRunLowKey(record) == (expected & 0xffffu);
                }
            }
            Require(result, replayOk, "morton.slab-replay", "flushing slices must preserve every high/low key and record including tails");
            run.Release();
            Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "morton.slab-and-run-release", "consumed slab and run must leave no retained capacity");
            Require(result, root.UpdateLimits({slabBytes - 1u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
                !WriteHighBucketRun(count, getter, counts, offsets, written, session, run, options, *phase) &&
                !run.IsValid() && root.StorageCapacity()->Snapshot().reservedBytes == 0u,
                "morton.slab-required", "insufficient mandatory slab capacity must fail before creating a run without resizing slices");
        }
    }

    // 超过固定内排阈值且所有 key 相同，跨叶输出仍需保持稳定次序
    constexpr auto largeCount = ResolveLeafBudgetElements() + 3u;
    for (const bool cacheAllowedByCapacity : {false, true}) {
        const std::size_t limit = cacheAllowedByCapacity ? 32u * MiB : 1026u;
        DataCodecExecutionResources root({{limit, 1u, 1u}, limit, 1u, false, true, true});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), true);
        std::size_t keyCalls = 0u;
        std::uint64_t observedCache = 0u;
        bool sampledRun = false;
        std::array<std::uint64_t, static_cast<std::size_t>(MortonArraySample::Count)> peaks{};
        bool sampledInsidePhase = true;
        MortonRemapOptions options{.resources = root,
            .recordCapacitySamples = [&](std::span<const BufferCapacitySample> samples) {
                ResourceDebugSnapshot snapshot;
                sampledInsidePhase &= root.TryCopyResourceDebugSnapshot(snapshot) && snapshot.heavyPhaseAdmitted;
                for (std::size_t i = 0u; i < samples.size(); ++i) {
                    peaks[i] = std::max(peaks[i], samples[i].sampledPeakBytes.value_or(0u));
                }
                const auto& window = samples[static_cast<std::size_t>(MortonArraySample::RunReadWindow)];
                if (window.capacityBytes) {
                    sampledRun = *window.capacityBytes >= kMortonRunBufferBytes - kMortonRunRecordBytes &&
                        *window.capacityBytes <= kMortonRunBufferBytes;
                }
            }, .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "large"),
            .byteStoreSession = &session, .buildInverse = cacheAllowedByCapacity};
        MortonRemapResult output;
        const bool success = BuildMortonRemapProvider(largeCount, [&](std::size_t) {
            if (++keyCalls == 1u) { observedCache = root.StorageCapacity()->AllocatedStorage().liveBytes; }
            return 17u;
        }, output, options);
        bool replayOk = success;
        std::vector<IndexType> values;
        for (std::size_t begin = 0u; replayOk && begin < largeCount; begin += 4096u) {
            const auto n = std::min<std::size_t>(4096u, largeCount - begin);
            replayOk = output.orderProvider->ReadRange(begin, n, values, nullptr);
            for (std::size_t i = 0u; replayOk && i < n; ++i) { replayOk = values[i] == begin + i; }
            if (replayOk && options.buildInverse) {
                replayOk = output.inverseProvider->ReadRange(begin, n, values, nullptr);
                for (std::size_t i = 0u; replayOk && i < n; ++i) { replayOk = values[i] == begin + i; }
            }
        }
        Require(result, replayOk && sampledRun && keyCalls == largeCount * (cacheAllowedByCapacity ? 1u : 2u) &&
            observedCache == (cacheAllowedByCapacity ? largeCount * sizeof(std::uint32_t) : 0u) &&
            root.StorageCapacity()->Snapshot().reservedBytes ==
                (cacheAllowedByCapacity ? 2u * largeCount * sizeof(IndexType) : 0u),
            "morton.external-sort", "fixed external leaves must preserve duplicates and use the one selected cache mode");
        const auto peak = [&](MortonArraySample sample) { return peaks[static_cast<std::size_t>(sample)]; };
        const auto tableBytes = kMortonBucketCount16 * sizeof(std::size_t);
        Require(result, sampledInsidePhase &&
            peak(MortonArraySample::HighCounts) == tableBytes &&
            peak(MortonArraySample::HighOffsets) == tableBytes &&
            peak(MortonArraySample::HighWriteOffsets) == tableBytes &&
            peak(MortonArraySample::LowCounts) == tableBytes &&
            peak(MortonArraySample::LowOffsets) == tableBytes &&
            peak(MortonArraySample::TouchedLowBuckets) == kMortonBucketCount16 * sizeof(std::uint32_t) &&
            peak(MortonArraySample::Leaves) == sizeof(MortonLowKeyLeaf),
            "morton.fixed-table-capacities", "fixed bucket tables and the single-key leaf descriptor must report actual arrays inside the heavy phase");
        Require(result,
            peak(MortonArraySample::Scratch) == ResolveLeafBudgetElements() * sizeof(IndexType) &&
            peak(MortonArraySample::OrderedElements) == ResolveLeafBudgetElements() * sizeof(IndexType) &&
            peak(MortonArraySample::LowBuckets) == ResolveLeafBudgetElements() * sizeof(std::uint16_t),
            "morton.leaf-array-capacities", "external sorting scratch arrays must stay at the fixed leaf capacity across the tail leaf");
        output = {};
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "morton.external-release", "all external sorting capacity must be returned after output consumption");
    }
    {
        DataCodecExecutionResources root({{32u * MiB, 1u, 1u}, 32u * MiB, 1u, false, true, true});
        CodecRunScope scope(root);
        bytestore::ByteStoreSession session;
        session.BindStorage(root.StorageCapacity(), true);
        MortonRemapOptions options{.resources = root,
            .providerFactory = MakeStoreBackedWritableRemapProviderFactory(session, "failure"),
            .byteStoreSession = &session};
        MortonRemapResult output;
        std::size_t keyCalls = 0u;
        bool failed = false;
        try {
            failed = !BuildMortonRemapProvider(largeCount, [&](std::size_t) -> std::uint32_t {
                if (++keyCalls == 3u) { throw std::runtime_error("key failure"); }
                return 0u;
            }, output, options);
        } catch (const std::runtime_error&) { failed = true; }
        Require(result, failed && keyCalls == 3u && !output.orderProvider &&
            root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "morton.key-failure", "a failing key computation must release cache capacity without recomputing");
        scope.Finish(false);
        CodecRunScope allocationScope(root);
        const auto factory = options.providerFactory;
        options.providerFactory = [&](std::size_t count, bool randomWrite, std::string_view label, std::string* error,
                                     std::span<const resource::StorageOwnerDescription> coexist) {
            RejectAllocationsScope rejection;
            return factory(count, randomWrite, label, error, coexist);
        };
        keyCalls = 0u;
        failed = false;
        try {
            failed = !BuildMortonRemapProvider(6u, [&](std::size_t) { ++keyCalls; return 0u; }, output, options);
        } catch (const std::bad_alloc&) { failed = true; }
        Require(result, failed && keyCalls == 0u && rejectedAllocationCount == 1u && !output.orderProvider &&
            root.StorageCapacity()->Snapshot().reservedBytes == 0u && session.SnapshotStats().managedFileBytes == 0u,
            "morton.provider-allocation-failure", "mandatory provider allocation failure must happen before sorting and release every acquired owner");
    }
    return result;
}

} // DataCodec 测试命名空间

#endif
