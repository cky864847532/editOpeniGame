#ifndef DATACODEC_CODEC_REMAP_COMMON_MORTONREMAPBUILDER_H
#define DATACODEC_CODEC_REMAP_COMMON_MORTONREMAPBUILDER_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/NumericArray/SpatialBlockLayout.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"
#include "DataCodec/Codec/Remap/RemapProvider.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace datacodec {
namespace mortonremap {

inline constexpr std::uint32_t kMortonBucketMask16 = 0xffffu;
inline constexpr std::size_t kMortonBucketCount16 = 1u << 16u;
inline constexpr std::size_t kMortonLeafBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMortonRunRecordBytes = sizeof(std::uint16_t) + sizeof(IndexType);
inline constexpr std::size_t kMortonRunBufferBytes = 1u * 1024u * 1024u;
inline constexpr std::size_t kMortonHighBucketBufferBytes = 1024u;

struct MortonRemapOptions {
    DataCodecExecutionResources& resources;
    std::string resourcePrefix{"remap.morton"};
    std::function<void(double)> progressCallback{};
    callback::CapacityCallback recordCapacitySamples;
    WritableRemapProviderFactory providerFactory{};
    bytestore::ByteStoreSession* byteStoreSession{nullptr};
    bool buildInverse{false};
};

struct MortonRemapResult {
    std::shared_ptr<IRemapProvider> orderProvider;
    std::shared_ptr<IRemapProvider> inverseProvider;
};

struct MortonKeyedIndex {
    std::uint32_t key{0u};
    IndexType index{0u};
};

struct MortonLowKeyLeaf {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint64_t count{0};
};

inline void InvokeProgress(
    const MortonRemapOptions& options,
    const double normalized) {
    try { callback::InvokeProgress(options.progressCallback, normalized); }
    catch (...) { options.resources.RecordDiagnosticExportFailure(); }
}

enum class MortonArraySample : std::size_t {
    Keyed,
    Order,
    HighCounts,
    HighOffsets,
    HighWriteOffsets,
    SliceOffsets,
    UsedBytes,
    Scratch,
    OrderedElements,
    LowBuckets,
    LowCounts,
    LowOffsets,
    TouchedLowBuckets,
    Leaves,
    RunReadWindow,
    Count,
};

struct MortonCapacitySamples {
    std::array<BufferCapacitySample, static_cast<std::size_t>(MortonArraySample::Count)> values{{
        BufferCapacitySample{"morton.keyed"},
        BufferCapacitySample{"morton.order_work"},
        BufferCapacitySample{"morton.high_counts"},
        BufferCapacitySample{"morton.high_offsets"},
        BufferCapacitySample{"morton.high_write_offsets"},
        BufferCapacitySample{"morton.slice_offsets"},
        BufferCapacitySample{"morton.slice_used_bytes"},
        BufferCapacitySample{"morton.scratch"},
        BufferCapacitySample{"morton.ordered_elements"},
        BufferCapacitySample{"morton.low_buckets"},
        BufferCapacitySample{"morton.low_counts"},
        BufferCapacitySample{"morton.low_offsets"},
        BufferCapacitySample{"morton.touched_low_buckets"},
        BufferCapacitySample{"morton.leaves"},
        BufferCapacitySample{"morton.run_read_window"},
    }};

    template<class T>
    void Observe(MortonArraySample kind, const T& storage) noexcept {
        values[static_cast<std::size_t>(kind)].Observe(storage);
    }
};

inline void EmitMortonSamples(const MortonRemapOptions& options,
    const std::optional<MortonCapacitySamples>& samples) noexcept {
    if (!samples || !options.recordCapacitySamples) { return; }
    try { options.recordCapacitySamples(samples->values); }
    catch (...) { options.resources.RecordDiagnosticExportFailure(); }
}

// 分段由 driver 逐次提交，终端工作不等待同池子任务
 template<class TWork>
inline bool RunMortonRanges(DataCodecExecutionResources& root, const HeavyPhaseLease& phase,
    const std::size_t count, TWork&& work) {
    for (std::size_t first = 0u; first < count;) {
        if (root.Stopped()) { return false; }
        const auto end = first + std::min<std::size_t>(numericarray::kSpatialBlockElementCount, count - first);
        if (!RunTerminalWork(root, phase, [&](WorkerContext&) { return work(first, end); })) { return false; }
        first = end;
    }
    return true;
}

inline constexpr std::size_t ResolveLeafBudgetElements() noexcept {
    return std::max<std::size_t>(1u, kMortonLeafBytes / (sizeof(IndexType) * 2u + sizeof(std::uint16_t)));
}

inline constexpr std::size_t MortonHighSliceRecords() noexcept {
    return (kMortonHighBucketBufferBytes + kMortonRunRecordBytes - 1u) / kMortonRunRecordBytes;
}

inline constexpr std::size_t MortonHighSlabLowerBound(const std::size_t elementCount) noexcept {
    // 分桶路径中 sum(min(桶元素数, 切片容量)) 至少为 min(总元素数, 切片容量)
    return elementCount > ResolveLeafBudgetElements()
        ? std::min(elementCount, MortonHighSliceRecords()) * kMortonRunRecordBytes : 0u;
}

class RemapScratchRun final {
public:
    RemapScratchRun() = default;
    explicit RemapScratchRun(std::shared_ptr<bytestore::IByteStore> store)
        : m_store(std::move(store)) {}

    RemapScratchRun(const RemapScratchRun&) = delete;
    RemapScratchRun& operator=(const RemapScratchRun&) = delete;

    RemapScratchRun(RemapScratchRun&&) noexcept = default;
    RemapScratchRun& operator=(RemapScratchRun&&) noexcept = default;

    ~RemapScratchRun() { Release(); }

    [[nodiscard]] bool IsValid() const noexcept { return m_store != nullptr; }
    [[nodiscard]] const bytestore::IByteSource* ByteSource() const noexcept { return m_store.get(); }

    bool WriteRecordBytes(
        const std::size_t recordOffset,
        const std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) {
        if (m_store == nullptr) {
            return validation::AssignError(error, "Morton scratch run store is missing");
        }
        std::uint64_t byteOffset = 0u;
        if (!ResolveByteRange(recordOffset, bytes.size(), byteOffset, error)) {
            return false;
        }
        return m_store->WriteAt(byteOffset, bytes, error);
    }

    bool ReadRecordBytes(
        const std::size_t recordOffset,
        const std::span<std::uint8_t> bytes,
        std::string* error = nullptr) const {
        if (m_store == nullptr) {
            return validation::AssignError(error, "Morton scratch run store is missing");
        }
        std::uint64_t byteOffset = 0u;
        if (!ResolveByteRange(recordOffset, bytes.size(), byteOffset, error)) {
            return false;
        }
        return m_store->Read(byteOffset, bytes, error);
    }

    void Release() noexcept {
        if (m_store != nullptr) {
            m_store.reset();
        }
    }

private:
    bool ResolveByteRange(
        const std::size_t recordOffset,
        const std::size_t byteCount,
        std::uint64_t& byteOffset,
        std::string* error) const {
        if (!validation::CanMulU64(static_cast<std::uint64_t>(recordOffset), kMortonRunRecordBytes)) {
            validation::AssignError(error, "Morton scratch run byte offset exceeds addressable size");
            return false;
        }
        byteOffset = static_cast<std::uint64_t>(recordOffset) * kMortonRunRecordBytes;
        if (m_store == nullptr || byteOffset > m_store->ByteSizeHint() ||
            byteCount > m_store->ByteSizeHint() - byteOffset) {
            validation::AssignError(error, "Morton scratch run byte range exceeds its fixed size");
            return false;
        }
        return true;
    }

    std::shared_ptr<bytestore::IByteStore> m_store;
};

class RemapScratchSpooler final {
public:
    explicit RemapScratchSpooler(bytestore::ByteStoreSession& session)
        : m_session(session) {}

    [[nodiscard]] RemapScratchRun CreateRun(
        const std::string_view label,
        const std::size_t recordCount,
        std::string* error = nullptr,
        std::span<const resource::StorageOwnerDescription> coexist = {}) {
        std::size_t bytes = 0u;
        if (!validation::CheckedMulSizeT(recordCount, kMortonRunRecordBytes, bytes,
                "Morton run bytes", error)) {
            return {};
        }
        return RemapScratchRun(m_session.CreateSizedStore(bytestore::ByteStorePurpose::Ranged,
            bytes, ::datacodec::MemoryDemandKind::RequiredContinuation, std::string(label), error, coexist));
    }

private:
    bytestore::ByteStoreSession& m_session;
};

inline std::array<std::uint8_t, kMortonRunRecordBytes> EncodeRunRecord(
    const std::uint16_t lowKey,
    const IndexType elementId) noexcept {
    static_assert(sizeof(IndexType) == sizeof(std::uint32_t));
    const auto value = static_cast<std::uint32_t>(elementId);
    return {static_cast<std::uint8_t>(lowKey & 0xffu),
        static_cast<std::uint8_t>((lowKey >> 8u) & 0xffu),
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8u) & 0xffu),
        static_cast<std::uint8_t>((value >> 16u) & 0xffu),
        static_cast<std::uint8_t>((value >> 24u) & 0xffu)};
}

inline std::uint16_t DecodeRunLowKey(const std::uint8_t* record) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(record[0]) |
        (static_cast<std::uint16_t>(record[1]) << 8u));
}

inline IndexType DecodeRunElementId(const std::uint8_t* record) noexcept {
    const auto value =
        static_cast<std::uint32_t>(record[2]) |
        (static_cast<std::uint32_t>(record[3]) << 8u) |
        (static_cast<std::uint32_t>(record[4]) << 16u) |
        (static_cast<std::uint32_t>(record[5]) << 24u);
    return static_cast<IndexType>(value);
}

template<typename TKeyGetter>
inline bool WriteHighBucketRun(
    const std::size_t elementCount,
    TKeyGetter&& keyGetter,
    const std::vector<std::size_t>& highCounts,
    const std::vector<std::size_t>& highOffsets,
    std::vector<std::size_t>& highWriteOffsets,
    bytestore::ByteStoreSession& session,
    RemapScratchRun& run,
    const MortonRemapOptions& options,
    const HeavyPhaseLease& phase,
    std::string* error = nullptr,
    const bytestore::IByteSource* keyCacheSource = nullptr) {
    if (highCounts.size() != kMortonBucketCount16 || highOffsets.size() != kMortonBucketCount16) {
        return validation::AssignError(error, "Morton high tables have an invalid size");
    }
    constexpr auto recordsPerSlice = MortonHighSliceRecords();
    std::vector<std::size_t> sliceOffsets(kMortonBucketCount16 + 1u, 0u);
    std::vector<std::size_t> usedBytes(kMortonBucketCount16, 0u);
    std::size_t totalRecords = 0u;
    for (std::size_t bucket = 0u; bucket < kMortonBucketCount16; ++bucket) {
        if (highOffsets[bucket] != totalRecords ||
            !validation::CheckedAddSizeT(totalRecords, highCounts[bucket], totalRecords,
                "Morton high record count", error)) {
            return validation::AssignError(error, "Morton high offsets do not cover the record sequence");
        }
        std::size_t sliceBytes = 0u;
        if (!validation::CheckedMulSizeT(std::min(highCounts[bucket], recordsPerSlice),
                kMortonRunRecordBytes, sliceBytes, "Morton high slice", error) ||
            !validation::CheckedAddSizeT(sliceOffsets[bucket], sliceBytes, sliceOffsets[bucket + 1u],
                "Morton high slab", error)) {
            return false;
        }
    }
    if (totalRecords != elementCount) {
        return validation::AssignError(error, "Morton high counts do not match the element count");
    }
    // 必要连续数组先完整准入，后续 run 独立确定后端
    bytestore::KnownStorageOwners coexist;
    coexist.Add(keyCacheSource);
    auto slab = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous,
        sliceOffsets.back(), ::datacodec::MemoryDemandKind::RequiredContinuation, "morton_high_slab", error, coexist.Entries());
    if (!slab) { return false; }
    std::optional<MortonCapacitySamples> samples;
    if (options.recordCapacitySamples) {
        samples.emplace();
        samples->Observe(MortonArraySample::SliceOffsets, sliceOffsets);
        samples->Observe(MortonArraySample::UsedBytes, usedBytes);
    }
    const auto label = options.resourcePrefix.empty()
        ? std::string("remap_morton_high_run")
        : options.resourcePrefix + ".high_run";
    RemapScratchSpooler scratchSpooler(session);
    coexist.Add(slab.get());
    run = scratchSpooler.CreateRun(label, elementCount, error, coexist.Entries());
    if (!run.IsValid()) {
        return validation::AssignError(error, "failed to open Morton high bucket run");
    }

    highWriteOffsets = highOffsets;
    const auto flush = [&](const std::size_t bucket) {
        const auto bytes = usedBytes[bucket];
        if (bytes == 0u) { return true; }
        if (!run.WriteRecordBytes(highWriteOffsets[bucket],
                slab->ContiguousBytes().subspan(sliceOffsets[bucket], bytes), error)) {
            return false;
        }
        highWriteOffsets[bucket] += bytes / kMortonRunRecordBytes;
        usedBytes[bucket] = 0u;
        return true;
    };
    const auto progressStep = std::max<std::size_t>(elementCount / 64u, 1u);
    if (!RunMortonRanges(options.resources, phase, elementCount, [&](std::size_t first, std::size_t end) {
    for (std::size_t elementIndex = first; elementIndex < end; ++elementIndex) {
        if (options.resources.Stopped()) { return false; }
        const auto key = keyGetter(elementIndex);
        const auto highBucket = static_cast<std::uint16_t>((key >> 16u) & kMortonBucketMask16);
        const auto lowKey = static_cast<std::uint16_t>(key & kMortonBucketMask16);
        auto& used = usedBytes[highBucket];
        if (highWriteOffsets[highBucket] - highOffsets[highBucket] + used / kMortonRunRecordBytes >=
                highCounts[highBucket]) {
            return validation::AssignError(error, "Morton key changed after the high-count pass");
        }
        const auto record = EncodeRunRecord(lowKey, static_cast<IndexType>(elementIndex));
        if (!slab->WriteAt(sliceOffsets[highBucket] + used, record, error)) {
            return false;
        }
        used += record.size();
        if (used == sliceOffsets[highBucket + 1u] - sliceOffsets[highBucket] && !flush(highBucket)) {
            return false;
        }

        if ((elementIndex + 1u) % progressStep == 0u || elementIndex + 1u == elementCount) {
            InvokeProgress(
                options,
                0.35 + 0.15 * (static_cast<double>(elementIndex + 1u) / static_cast<double>(elementCount)));
        }
    }

    return true;
    })) { return false; }

    if (!RunTerminalWork(options.resources, phase, [&](WorkerContext&) {
    for (std::size_t highBucket = 0; highBucket < kMortonBucketCount16; ++highBucket) {
        if (options.resources.Stopped() || !flush(highBucket)) {
            return false;
        }
    }

    for (std::size_t highBucket = 0; highBucket < kMortonBucketCount16; ++highBucket) {
        if (highWriteOffsets[highBucket] != highOffsets[highBucket] + highCounts[highBucket]) {
            return validation::AssignError(error, "Morton high bucket run write count mismatch");
        }
    }
    return true;
    })) { return false; }
    EmitMortonSamples(options, samples);
    return true;
}

inline bool CountRunSegmentLowKeys(
    const RemapScratchRun& run,
    const std::size_t recordBegin,
    const std::size_t recordCount,
    const std::size_t runBufferBytes,
    std::vector<std::size_t>& lowCounts,
    std::vector<std::uint32_t>& touchedLowBuckets,
    DataCodecExecutionResources& root, const HeavyPhaseLease& phase,
    std::string* error = nullptr, MortonCapacitySamples* samples = nullptr) {
    touchedLowBuckets.clear();
    const auto recordsPerRead = std::max<std::size_t>(1u, runBufferBytes / kMortonRunRecordBytes);
    std::vector<std::uint8_t> buffer(recordsPerRead * kMortonRunRecordBytes);
    if (samples != nullptr) { samples->Observe(MortonArraySample::RunReadWindow, buffer); }
    std::size_t remaining = recordCount;
    std::size_t currentRecordOffset = recordBegin;
    while (remaining > 0u) {
        if (root.Stopped()) { return false; }
        const auto currentRecords = std::min<std::size_t>(remaining, recordsPerRead);
        const auto currentBytes = currentRecords * kMortonRunRecordBytes;
        if (!RunTerminalWork(root, phase, [&](WorkerContext&) {
        if (!run.ReadRecordBytes(
                currentRecordOffset,
                std::span<std::uint8_t>(buffer.data(), currentBytes),
                error)) {
            return false;
        }
        for (std::size_t recordIndex = 0; recordIndex < currentRecords; ++recordIndex) {
            const auto* record = buffer.data() + recordIndex * kMortonRunRecordBytes;
            const auto lowKey = DecodeRunLowKey(record);
            if (lowCounts[lowKey] == 0u) {
                touchedLowBuckets.push_back(lowKey);
            }
            lowCounts[lowKey]++;
        }
        return true;
        })) { return false; }
        remaining -= currentRecords;
        currentRecordOffset += currentRecords;
    }
    return true;
}

inline void ResetLowWorkspace(
    const std::vector<std::uint32_t>& touchedLowBuckets,
    std::vector<std::size_t>& lowCounts,
    std::vector<std::size_t>& lowOffsets) {
    for (const auto lowBucket : touchedLowBuckets) {
        lowCounts[lowBucket] = 0u;
        lowOffsets[lowBucket] = 0u;
    }
}

inline std::vector<MortonLowKeyLeaf> BuildLowKeyLeaves(
    const std::vector<std::size_t>& lowCounts,
    const std::size_t leafBudgetElements) {
    std::vector<MortonLowKeyLeaf> leaves;
    std::uint32_t currentBegin = 0u;
    std::uint32_t currentEnd = 0u;
    std::uint64_t currentCount = 0u;
    bool hasCurrent = false;

    const auto pushCurrent = [&]() {
        if (!hasCurrent || currentCount == 0u) {
            return;
        }
        leaves.push_back(MortonLowKeyLeaf{
            .begin = currentBegin,
            .end = currentEnd,
            .count = currentCount,
        });
    };

    for (std::uint32_t lowKey = 0u; lowKey < kMortonBucketCount16; ++lowKey) {
        const auto count = static_cast<std::uint64_t>(lowCounts[lowKey]);
        if (count == 0u) {
            continue;
        }
        if (count > leafBudgetElements) {
            pushCurrent();
            hasCurrent = false;
            currentCount = 0u;
            leaves.push_back(MortonLowKeyLeaf{
                .begin = lowKey,
                .end = static_cast<std::uint32_t>(lowKey + 1u),
                .count = count,
            });
            continue;
        }
        if (!hasCurrent) {
            currentBegin = lowKey;
            currentEnd = static_cast<std::uint32_t>(lowKey + 1u);
            currentCount = count;
            hasCurrent = true;
            continue;
        }
        if (currentCount + count > leafBudgetElements) {
            leaves.push_back(MortonLowKeyLeaf{
                .begin = currentBegin,
                .end = currentEnd,
                .count = currentCount,
            });
            currentBegin = lowKey;
            currentEnd = static_cast<std::uint32_t>(lowKey + 1u);
            currentCount = count;
            continue;
        }
        currentCount += count;
        currentEnd = static_cast<std::uint32_t>(lowKey + 1u);
    }
    if (hasCurrent && currentCount > 0u) {
        leaves.push_back(MortonLowKeyLeaf{
            .begin = currentBegin,
            .end = currentEnd,
            .count = currentCount,
        });
    }
    return leaves;
}

inline bool ReadRunSegmentLeaf(
    const RemapScratchRun& run,
    const std::size_t recordBegin,
    const std::size_t recordCount,
    const MortonLowKeyLeaf& leaf,
    const std::size_t runBufferBytes,
    std::vector<IndexType>& scratch,
    std::vector<std::uint16_t>& lowBuckets,
    DataCodecExecutionResources& root, const HeavyPhaseLease& phase,
    std::string* error = nullptr, MortonCapacitySamples* samples = nullptr) {
    scratch.clear();
    lowBuckets.clear();
    scratch.reserve(static_cast<std::size_t>(leaf.count));
    lowBuckets.reserve(static_cast<std::size_t>(leaf.count));

    const auto recordsPerRead = std::max<std::size_t>(1u, runBufferBytes / kMortonRunRecordBytes);
    std::vector<std::uint8_t> buffer(recordsPerRead * kMortonRunRecordBytes);
    if (samples != nullptr) { samples->Observe(MortonArraySample::RunReadWindow, buffer); }
    std::size_t remaining = recordCount;
    std::size_t currentRecordOffset = recordBegin;
    while (remaining > 0u) {
        if (root.Stopped()) { return false; }
        const auto currentRecords = std::min<std::size_t>(remaining, recordsPerRead);
        const auto currentBytes = currentRecords * kMortonRunRecordBytes;
        if (!RunTerminalWork(root, phase, [&](WorkerContext&) {
        if (!run.ReadRecordBytes(
                currentRecordOffset,
                std::span<std::uint8_t>(buffer.data(), currentBytes),
                error)) {
            return false;
        }
        for (std::size_t recordIndex = 0; recordIndex < currentRecords; ++recordIndex) {
            const auto* record = buffer.data() + recordIndex * kMortonRunRecordBytes;
            const auto lowKey = DecodeRunLowKey(record);
            if (lowKey < leaf.begin || lowKey >= leaf.end) {
                continue;
            }
            lowBuckets.push_back(lowKey);
            scratch.push_back(DecodeRunElementId(record));
        }
        return true;
        })) { return false; }
        remaining -= currentRecords;
        currentRecordOffset += currentRecords;
    }
    return scratch.size() == leaf.count && lowBuckets.size() == leaf.count;
}

inline bool ReadRunSegmentSingleLowKeyChunk(
    const RemapScratchRun& run,
    const std::size_t recordBegin,
    const std::size_t recordCount,
    const std::uint16_t lowKeyFilter,
    const std::uint64_t skipCount,
    const std::size_t maxCount,
    const std::size_t runBufferBytes,
    std::vector<IndexType>& scratch,
    DataCodecExecutionResources& root, const HeavyPhaseLease& phase,
    std::string* error = nullptr, MortonCapacitySamples* samples = nullptr) {
    scratch.clear();
    scratch.reserve(maxCount);
    std::uint64_t skipped = 0u;

    const auto recordsPerRead = std::max<std::size_t>(1u, runBufferBytes / kMortonRunRecordBytes);
    std::vector<std::uint8_t> buffer(recordsPerRead * kMortonRunRecordBytes);
    if (samples != nullptr) { samples->Observe(MortonArraySample::RunReadWindow, buffer); }
    std::size_t remaining = recordCount;
    std::size_t currentRecordOffset = recordBegin;
    while (remaining > 0u && scratch.size() < maxCount) {
        if (root.Stopped()) { return false; }
        const auto currentRecords = std::min<std::size_t>(remaining, recordsPerRead);
        const auto currentBytes = currentRecords * kMortonRunRecordBytes;
        if (!RunTerminalWork(root, phase, [&](WorkerContext&) {
        if (!run.ReadRecordBytes(
                currentRecordOffset,
                std::span<std::uint8_t>(buffer.data(), currentBytes),
                error)) {
            return false;
        }
        for (std::size_t recordIndex = 0; recordIndex < currentRecords && scratch.size() < maxCount; ++recordIndex) {
            const auto* record = buffer.data() + recordIndex * kMortonRunRecordBytes;
            const auto lowKey = DecodeRunLowKey(record);
            if (lowKey != lowKeyFilter) {
                continue;
            }
            if (skipped < skipCount) {
                ++skipped;
                continue;
            }
            scratch.push_back(DecodeRunElementId(record));
        }
        return true;
        })) { return false; }
        remaining -= currentRecords;
        currentRecordOffset += currentRecords;
    }
    return scratch.size() == maxCount;
}

inline void BuildBufferedLeafOrder(
    const MortonLowKeyLeaf& leaf,
    const std::vector<IndexType>& scratch,
    const std::vector<std::uint16_t>& lowBuckets,
    const std::vector<std::size_t>& lowCounts,
    std::vector<std::size_t>& lowOffsets,
    std::vector<IndexType>& orderedElements) {
    orderedElements.resize(scratch.size());
    std::size_t runningOffset = 0u;
    for (std::size_t lowKey = leaf.begin; lowKey < leaf.end; ++lowKey) {
        lowOffsets[lowKey] = runningOffset;
        runningOffset += lowCounts[lowKey];
    }
    for (std::size_t index = 0; index < scratch.size(); ++index) {
        const auto lowKey = lowBuckets[index];
        orderedElements[lowOffsets[lowKey]++] = scratch[index];
    }
}

struct MortonRemapOutput {
    IWritableRemapProvider* orderProvider{nullptr};
    IWritableRemapProvider* inverseProvider{nullptr};
    DataCodecExecutionResources& root;
    const HeavyPhaseLease& phase;
    std::size_t nextIndex{0u};

    bool Append(std::span<const IndexType> order, std::string* error = nullptr) {
        if (orderProvider == nullptr) {
            return validation::AssignError(error, "Morton remap output provider is null");
        }
        constexpr std::size_t window = kIoWindowBytes / sizeof(IndexType);
        for (std::size_t first = 0u; first < order.size();) {
            if (root.Stopped()) { return false; }
            const auto count = std::min(window, order.size() - first);
            const auto values = order.subspan(first, count);
            if (!RunTerminalWork(root, phase, [&](WorkerContext&) {
                if (!orderProvider->AppendRange(values, error)) { return false; }
                if (inverseProvider != nullptr) {
                    for (std::size_t i = 0u; i < values.size(); ++i) {
                        if (root.Stopped() || !inverseProvider->WriteAt(values[i],
                                static_cast<IndexType>(nextIndex + i), error)) { return false; }
                    }
                }
                return true;
            })) { return false; }
            nextIndex += count;
            first += count;
        }
        return true;
    }
};

inline bool AppendRunSegmentByMortonKey(
    const RemapScratchRun& run,
    const std::size_t recordBegin,
    const std::size_t recordCount,
    const std::size_t leafBudgetElements,
    const std::size_t runBufferBytes,
    MortonRemapOutput& output,
    std::vector<IndexType>& scratch,
    std::vector<IndexType>& orderedElements,
    std::vector<std::uint16_t>& lowBuckets,
    std::vector<std::size_t>& lowCounts,
    std::vector<std::size_t>& lowOffsets,
    std::vector<std::uint32_t>& touchedLowBuckets,
    std::string* error = nullptr, MortonCapacitySamples* samples = nullptr) {
    auto fail = [&]() {
        ResetLowWorkspace(touchedLowBuckets, lowCounts, lowOffsets);
        return false;
    };

    if (!CountRunSegmentLowKeys(
            run,
            recordBegin,
            recordCount,
            runBufferBytes,
            lowCounts,
            touchedLowBuckets,
            output.root, output.phase,
            error, samples)) {
        return fail();
    }

    std::vector<MortonLowKeyLeaf> leaves;
    if (!RunTerminalWork(output.root, output.phase, [&](WorkerContext&) {
            leaves = BuildLowKeyLeaves(lowCounts, leafBudgetElements);
            return true;
        })) { return fail(); }
    if (samples != nullptr) { samples->Observe(MortonArraySample::Leaves, leaves); }
    std::size_t outputOffset = 0u;
    for (const auto& leaf : leaves) {
        if (leaf.count == 0u) {
            continue;
        }
        const auto isSingleLowKeyLeaf =
            static_cast<std::size_t>(leaf.end) == static_cast<std::size_t>(leaf.begin) + 1u;
        if (leaf.count > leafBudgetElements && isSingleLowKeyLeaf) {
            std::uint64_t emitted = 0u;
            while (emitted < leaf.count) {
                const auto currentCount = static_cast<std::size_t>(
                    std::min<std::uint64_t>(leaf.count - emitted, leafBudgetElements));
                if (!ReadRunSegmentSingleLowKeyChunk(
                        run,
                        recordBegin,
                        recordCount,
                        static_cast<std::uint16_t>(leaf.begin),
                        emitted,
                        currentCount,
                        runBufferBytes,
                        scratch,
                        output.root, output.phase,
                        error, samples)) {
                    return fail();
                }
                if (!output.Append(std::span<const IndexType>(scratch.data(), scratch.size()), error)) {
                    return fail();
                }
                outputOffset += scratch.size();
                emitted += scratch.size();
            }
            continue;
        }
        if (!ReadRunSegmentLeaf(
                run,
                recordBegin,
                recordCount,
                leaf,
                runBufferBytes,
                scratch,
                lowBuckets,
                output.root, output.phase,
                error, samples)) {
            return fail();
        }
        if (!RunTerminalWork(output.root, output.phase, [&](WorkerContext&) {
        BuildBufferedLeafOrder(
            leaf,
            scratch,
            lowBuckets,
            lowCounts,
            lowOffsets,
            orderedElements);
        return true;
        })) { return fail(); }
        if (!output.Append(std::span<const IndexType>(orderedElements.data(), orderedElements.size()), error)) {
            return fail();
        }
        outputOffset += static_cast<std::size_t>(leaf.count);
    }

    ResetLowWorkspace(touchedLowBuckets, lowCounts, lowOffsets);
    return outputOffset == recordCount;
}

template<typename TKeyGetter>
inline bool BuildInMemoryMortonRemapProvider(
    const std::size_t elementCount, TKeyGetter&& keyGetter, MortonRemapResult& result,
    const MortonRemapOptions& options, const HeavyPhaseLease& phase, std::string* error) {
    auto provider = options.providerFactory(elementCount, false, options.resourcePrefix + ".order", error, {});
    if (!provider) { return false; }
    bytestore::KnownStorageOwners coexist;
    if (const auto* stored = dynamic_cast<const RemapStoreProvider*>(provider.get())) {
        coexist.Add(stored->ByteSource());
    }
    std::shared_ptr<IWritableRemapProvider> inverse;
    if (options.buildInverse) {
        inverse = options.providerFactory(elementCount, true, options.resourcePrefix + ".inverse", error, coexist.Entries());
        if (!inverse) { return false; }
    }
    std::optional<MortonCapacitySamples> samples;
    if (options.recordCapacitySamples) { samples.emplace(); }
    std::vector<MortonKeyedIndex> keyed;
    std::vector<IndexType> order;
    if (!RunTerminalWork(options.resources, phase, [&](WorkerContext&) {
        keyed.reserve(elementCount);
        for (std::size_t i = 0u; i < elementCount; ++i) {
            if (options.resources.Stopped()) { return false; }
            keyed.push_back({keyGetter(i), static_cast<IndexType>(i)});
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& left, const auto& right) {
            return left.key != right.key ? left.key < right.key : left.index < right.index;
        });
        order.reserve(elementCount);
        for (const auto& entry : keyed) { order.push_back(entry.index); }
        return true;
    })) { return false; }
    if (samples) {
        samples->Observe(MortonArraySample::Keyed, keyed);
        samples->Observe(MortonArraySample::Order, order);
    }
    EmitMortonSamples(options, samples);
    MortonRemapOutput output{provider.get(), inverse.get(), options.resources, phase};
    if (!output.Append(order, error) || !provider->EndWrite(error) ||
        (inverse && !inverse->EndRandomWrite(error))) { return false; }
    result.orderProvider = std::move(provider);
    result.inverseProvider = std::move(inverse);
    InvokeProgress(options, 1.0);
    return true;
}

template<typename TKeyGetter>
inline bool BuildMortonRemapProvider(
    const std::size_t elementCount,
    TKeyGetter&& keyGetter,
    MortonRemapResult& result,
    const MortonRemapOptions& options,
    std::string* error = nullptr) {
    result = {};
    if (elementCount > static_cast<std::size_t>(std::numeric_limits<IndexType>::max())) {
        return validation::AssignError(error, "Morton remap element count exceeds IndexType range");
    }
    if (elementCount <= 1u) {
        result.orderProvider = std::make_shared<IdentityRemapProvider>(elementCount);
        if (options.buildInverse) {
            result.inverseProvider = std::make_shared<IdentityRemapProvider>(elementCount);
        }
        InvokeProgress(options, 1.0);
        return true;
    }
    if (options.byteStoreSession == nullptr) {
        return validation::AssignError(error, "Morton remap requires a byte store session");
    }
    if (!options.providerFactory) {
        return validation::AssignError(error, "Morton remap requires a writable remap provider factory");
    }
    auto phase = WaitForHeavyPhase(options.resources);
    if (!phase) { return false; }
    constexpr auto leafBudgetElements = ResolveLeafBudgetElements();
    if (elementCount <= leafBudgetElements) {
        return BuildInMemoryMortonRemapProvider(
            elementCount,
            std::forward<TKeyGetter>(keyGetter),
            result,
            options,
            *phase,
            error);
    }

    std::size_t keyCacheBytes = 0u;
    if (!validation::CheckedMulSizeT(elementCount, sizeof(std::uint32_t), keyCacheBytes,
            "Morton key cache", error)) { return false; }
    std::shared_ptr<bytestore::MemoryStore> keyCache;
    if (options.resources.OptionalRetentionAllowed()) {
        auto capacity = options.resources.StorageCapacity();
        const auto tag = capacity->NewOwner(resource::StorageOwnerPurpose::Optional, "morton-key-cache");
        resource::CapacityRejection rejection;
        auto lease = capacity->TryReserve(keyCacheBytes, &rejection, tag);
        if (!lease) { options.resources.RecordCapacityRejection(rejection, false); }
        if (lease) {
            // 获准后只申请一次，异常直接交给现有 failure 链
            keyCache = options.byteStoreSession->CreateReservedMemoryStore(std::move(*lease), error);
            if (!keyCache) { return false; }
        }
    }
    const bool useKeyCache = keyCache != nullptr;

    std::optional<MortonCapacitySamples> samples;
    if (options.recordCapacitySamples) { samples.emplace(); }
    std::vector<std::size_t> highCounts(kMortonBucketCount16, 0u);
    std::vector<std::size_t> highOffsets(kMortonBucketCount16, 0u);
    std::vector<std::size_t> highWriteOffsets(kMortonBucketCount16, 0u);

    const auto progressStep = std::max<std::size_t>(elementCount / 64u, 1u);
    if (!RunMortonRanges(options.resources, *phase, elementCount, [&](std::size_t first, std::size_t end) {
    for (std::size_t elementIndex = first; elementIndex < end; ++elementIndex) {
        if (options.resources.Stopped()) { return false; }
        const auto key = keyGetter(elementIndex);
        if (useKeyCache) {
            if (!keyCache->WriteAt(elementIndex * sizeof(key),
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&key), sizeof(key)), error)) {
                return false;
            }
        }
        const auto highBucket = static_cast<std::uint16_t>((key >> 16u) & kMortonBucketMask16);
        highCounts[highBucket]++;
        if ((elementIndex + 1u) % progressStep == 0u || elementIndex + 1u == elementCount) {
            InvokeProgress(
                options,
                0.10 + 0.25 * (static_cast<double>(elementIndex + 1u) / static_cast<double>(elementCount)));
        }
    }
    return true;
    })) { return false; }

    std::size_t maxHighBucketSize = 0u;
    std::size_t nonEmptyHighBuckets = 0u;
    std::size_t runningOffset = 0u;
    for (std::size_t bucket = 0; bucket < kMortonBucketCount16; ++bucket) {
        const auto bucketSize = highCounts[bucket];
        highOffsets[bucket] = runningOffset;
        highWriteOffsets[bucket] = runningOffset;
        runningOffset += bucketSize;
        if (bucketSize > 0u) {
            ++nonEmptyHighBuckets;
            maxHighBucketSize = std::max(maxHighBucketSize, bucketSize);
        }
    }

    auto& byteStoreSession = *options.byteStoreSession;
    RemapScratchRun highRun;
    const auto highRunKeyGetter = [&](const std::size_t elementIndex) {
        if (!useKeyCache) { return keyGetter(elementIndex); }
        std::uint32_t key = 0u;
        std::memcpy(&key, keyCache->ContiguousBytes().data() + elementIndex * sizeof(key), sizeof(key));
        return key;
    };

    if (!WriteHighBucketRun(
            elementCount,
            highRunKeyGetter,
            highCounts,
            highOffsets,
            highWriteOffsets,
            byteStoreSession,
            highRun,
            options,
            *phase,
            error, keyCache.get())) {
        return false;
    }
    keyCache.reset();
    InvokeProgress(options, 0.50);

    if (samples) {
        samples->Observe(MortonArraySample::HighCounts, highCounts);
        samples->Observe(MortonArraySample::HighOffsets, highOffsets);
        samples->Observe(MortonArraySample::HighWriteOffsets, highWriteOffsets);
    }
    EmitMortonSamples(options, samples);
    ReleaseVectorStorage(highWriteOffsets);
    if (samples) { samples->Observe(MortonArraySample::HighWriteOffsets, highWriteOffsets); }

    const auto maxBufferedBucketSize = std::min<std::size_t>(maxHighBucketSize, leafBudgetElements);

    std::vector<IndexType> scratch;
    scratch.reserve(maxBufferedBucketSize);
    std::vector<IndexType> orderedElements;
    orderedElements.reserve(maxBufferedBucketSize);
    std::vector<std::uint16_t> lowBuckets;
    lowBuckets.reserve(maxBufferedBucketSize);
    std::vector<std::size_t> lowCounts(kMortonBucketCount16, 0u);
    std::vector<std::size_t> lowOffsets(kMortonBucketCount16, 0u);
    std::vector<std::uint32_t> touchedLowBuckets;
    touchedLowBuckets.reserve(std::min<std::size_t>(maxBufferedBucketSize, kMortonBucketCount16));

    bytestore::KnownStorageOwners coexist;
    coexist.Add(highRun.ByteSource());
    const auto makeWritableProvider = [&](const bool randomWrite, const char* label)
        -> std::shared_ptr<IWritableRemapProvider> {
        auto created = options.providerFactory(elementCount, randomWrite, label, error, coexist.Entries());
        if (const auto* stored = dynamic_cast<const RemapStoreProvider*>(created.get())) {
            coexist.Add(stored->ByteSource());
        }
        return created;
    };

    auto provider = makeWritableProvider(false, "order");
    if (provider == nullptr) {
        return false;
    }
    std::shared_ptr<IWritableRemapProvider> inverseProvider;
    if (options.buildInverse) {
        inverseProvider = makeWritableProvider(true, "inverse");
        if (inverseProvider == nullptr) {
            return false;
        }
    }

    MortonRemapOutput output{
        .orderProvider = provider.get(),
        .inverseProvider = inverseProvider.get(),
        .root = options.resources,
        .phase = *phase,
        .nextIndex = 0u,
    };

    std::size_t processedHighBuckets = 0u;
    std::size_t spilledHighBuckets = 0u;
    std::size_t largestSpilledBucket = 0u;
    for (std::size_t highBucket = 0; highBucket < kMortonBucketCount16; ++highBucket) {
        const auto bucketSize = highCounts[highBucket];
        if (bucketSize == 0u) {
            continue;
        }
        const auto bucketBegin = highOffsets[highBucket];
        if (bucketSize > leafBudgetElements) {
            ++spilledHighBuckets;
            largestSpilledBucket = std::max(largestSpilledBucket, bucketSize);
        }
        if (!AppendRunSegmentByMortonKey(
                highRun,
                bucketBegin,
                bucketSize,
                leafBudgetElements,
                kMortonRunBufferBytes,
                output,
                scratch,
                orderedElements,
                lowBuckets,
                lowCounts,
                lowOffsets,
                touchedLowBuckets,
                error, samples ? &*samples : nullptr)) {
            return false;
        }

        ++processedHighBuckets;
        if (processedHighBuckets % 512u == 0u || processedHighBuckets == nonEmptyHighBuckets) {
            InvokeProgress(
                options,
                0.50 + 0.45 *
                    (static_cast<double>(processedHighBuckets) /
                     static_cast<double>(std::max<std::size_t>(nonEmptyHighBuckets, 1u))));
        }
    }

    if (samples) {
        samples->Observe(MortonArraySample::Scratch, scratch);
        samples->Observe(MortonArraySample::OrderedElements, orderedElements);
        samples->Observe(MortonArraySample::LowBuckets, lowBuckets);
        samples->Observe(MortonArraySample::HighCounts, highCounts);
        samples->Observe(MortonArraySample::HighOffsets, highOffsets);
        samples->Observe(MortonArraySample::LowCounts, lowCounts);
        samples->Observe(MortonArraySample::LowOffsets, lowOffsets);
        samples->Observe(MortonArraySample::TouchedLowBuckets, touchedLowBuckets);
    }
    EmitMortonSamples(options, samples);
    if (!provider->EndWrite(error)) {
        return false;
    }
    if (inverseProvider != nullptr && !inverseProvider->EndRandomWrite(error)) {
        return false;
    }

    highRun.Release();
    ReleaseVectorStorage(scratch);
    ReleaseVectorStorage(orderedElements);
    ReleaseVectorStorage(lowBuckets);
    ReleaseVectorStorage(highCounts);
    ReleaseVectorStorage(highOffsets);
    ReleaseVectorStorage(lowCounts);
    ReleaseVectorStorage(lowOffsets);
    ReleaseVectorStorage(touchedLowBuckets);
    if (samples) {
        samples->Observe(MortonArraySample::Scratch, scratch);
        samples->Observe(MortonArraySample::OrderedElements, orderedElements);
        samples->Observe(MortonArraySample::LowBuckets, lowBuckets);
        samples->Observe(MortonArraySample::HighCounts, highCounts);
        samples->Observe(MortonArraySample::HighOffsets, highOffsets);
        samples->Observe(MortonArraySample::LowCounts, lowCounts);
        samples->Observe(MortonArraySample::LowOffsets, lowOffsets);
        samples->Observe(MortonArraySample::TouchedLowBuckets, touchedLowBuckets);
    }
    EmitMortonSamples(options, samples);
    InvokeProgress(options, 1.0);

    result.orderProvider = std::move(provider);
    result.inverseProvider = std::move(inverseProvider);
    return true;
}

} // Morton 重排命名空间
} // DataCodec 命名空间

#endif
