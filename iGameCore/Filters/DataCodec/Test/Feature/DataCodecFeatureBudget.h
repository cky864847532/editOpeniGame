#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREBUDGET_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREBUDGET_H

#include <DataCodec/Storage/ByteIO/ByteSource.h>
#include <DataCodec/Storage/ByteIO/Window/WindowedCopy.h>
#include <DataCodec/Storage/LeafPackage/LeafPackageFieldDecodeStream.h>
#include <DataCodec/Storage/LeafPackage/LeafPackageFieldEncode.h>
#include <DataCodec/Workflow/Decode/Stages/AttrDecodeStage.h>
#include <DataCodec/Runtime/Cache/DecodeCache/DecodedGeometryCache.h>
#include <DataCodec/Runtime/Cache/DecodeCache/DecodedIndexCache.h>
#include <DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamDecode.h>
#include <DataCodec/API/Params/CodecParamDefaults.h>
#include <DataCodec/Test/Common/DataCodecTestResult.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace datacodec::test::feature_budget {

using datacodec::test::Require;
using datacodec::test::TestResult;
using datacodec::ScratchByteBufferPool;
using datacodec::CodecControlParamsFactory;
using datacodec::DataCodecRuntimeProfile;
using datacodec::DecodedGeometryCache;
using datacodec::DecodedIndexCache;
using datacodec::bytestore::IByteWriter;
using datacodec::bytestore::ByteStoreSession;
using datacodec::bytestore::VectorByteSource;
using datacodec::window::WindowedByteSourceReader;
using datacodec::window::CopyByteSourceByWindow;
using datacodec::window::CopyByteSourceRangeByWindow;

class VectorByteWriter final : public IByteWriter {
public:
    bool Write(const std::span<const std::uint8_t> bytes, std::string*) override {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& Bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

inline void PrintResult(const TestResult& result) {
    for (const auto& failure : result.failures) {
        std::cerr << failure.check << ": " << failure.message << '\n';
    }
}

inline void RequireBytes(
    TestResult& result,
    const std::span<const std::uint8_t> actual,
    const std::vector<std::uint8_t>& expected,
    const std::string& path) {
    Require(result, actual.size() == expected.size(), path + ".size", "byte count mismatch");
    Require(
        result,
        std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()),
        path + ".bytes",
        "bytes mismatch");
}

inline void RequireBytes(
    TestResult& result,
    const std::vector<std::uint8_t>& actual,
    const std::vector<std::uint8_t>& expected,
    const std::string& path) {
    RequireBytes(
        result,
        std::span<const std::uint8_t>(actual.data(), actual.size()),
        expected,
        path);
}

inline bool TestByteStoreResidentBudgetAndSealLifecycle() {
    TestResult result;
    ByteStoreSession session;
    session.BindStorage(std::make_shared<resource::ResidentByteBudget>(8u), true);
    auto first = session.CreateMemoryStore();
    auto second = session.CreateMemoryStore();
    std::string error;
    Require(
        result,
        first != nullptr && first->ResizeBytes(8u, &error),
        "budget.byteStore.firstResidentAllocation",
        error.empty() ? "first resident allocation failed" : error);
    error.clear();
    Require(
        result,
        second != nullptr && !second->ResizeBytes(1u, &error),
        "budget.byteStore.rejectResidentOverflow",
        "resident allocation should fail after the session limit is exhausted");
    error.clear();
    Require(
        result,
        first->ResizeBytes(4u, &error) && !second->ResizeBytes(4u, &error),
        "budget.byteStore.preserveCapacityOnShrink",
        "logical shrink must preserve the allocation and its capacity lease");
    const auto storeStats = session.SnapshotStats();
    Require(
        result,
        storeStats.residentBytes == 8u &&
            storeStats.peakReservedBytes == 8u &&
            storeStats.capacityLimitBytes == 8u,
        "budget.byteStore.stats",
        "byte store resident statistics do not match the configured limit");

    for (const bool useMemoryStore : {true, false}) {
        ByteStoreSession appendSession;
        appendSession.BindStorage(std::make_shared<resource::ResidentByteBudget>(1024u), !useMemoryStore);
        auto appendStore = bytestore::CreateAppendableByteStore(
            appendSession,
            useMemoryStore ? "append_memory_test" : "append_managed_test",
            &error);
        const std::array<std::uint8_t, 3u> input{1u, 2u, 3u};
        std::array<std::uint8_t, 3u> output{};
        error.clear();
        Require(
            result,
            appendStore != nullptr && appendStore->AppendBytes(input, &error),
            useMemoryStore ? "budget.append.memory.write" : "budget.append.managed.write",
            error.empty() ? "append write failed" : error);
        error.clear();
        Require(
            result,
            !appendStore->Read(0u, output, &error),
            useMemoryStore ? "budget.append.memory.readBeforeSeal" : "budget.append.managed.readBeforeSeal",
            "append store should reject reads before seal");
        error.clear();
        Require(
            result,
            appendStore->Seal(&error) && appendStore->Read(0u, output, &error) && output == input,
            useMemoryStore ? "budget.append.memory.readAfterSeal" : "budget.append.managed.readAfterSeal",
            error.empty() ? "sealed append store did not replay its bytes" : error);
        error.clear();
        Require(
            result,
            !appendStore->AppendBytes(input, &error),
            useMemoryStore ? "budget.append.memory.rejectAfterSeal" : "budget.append.managed.rejectAfterSeal",
            "append store should reject writes after seal");
    }

    PrintResult(result);
    return result.passed;
}

inline bool TestScratchPoolRetentionLimits() {
    TestResult result;
    ScratchByteBufferPool pool;
    pool.SetRetainedCount(1u);
    {
        auto first = pool.Acquire(4u);
    }
    {
        auto reused = pool.Acquire(4u);
    }
    {
        auto oversized = pool.Acquire(kMaxRetainedScratchBlockBytes + 1u);
    }
    const auto stats = pool.SnapshotStats();
    Require(
        result,
        stats.retainedBytes <= 8u &&
            stats.reusedBlockCount == 1u &&
            stats.allocationCount == 2u,
        "budget.scratchPool.retention",
        "scratch pool retention and reuse statistics do not match the configured limits");
    PrintResult(result);
    return result.passed;
}

// 在真实范围源上限制单次 I/O，验证窗口尾部和失败后停止搬运
class WindowCheckedSource final : public bytestore::IByteSource {
public:
    explicit WindowCheckedSource(const std::vector<std::uint8_t>& bytes)
        : m_bytes(bytes) {}
    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override { return m_bytes.size(); }
    [[nodiscard]] bool CanRead() const noexcept override { return true; }
    bool Read(const std::uint64_t offset, const std::span<std::uint8_t> output,
              std::string* error = nullptr) const override {
        ++readCount;
        maxReadBytes = std::max(maxReadBytes, output.size());
        if (readCount == failRead || output.size() > kIoWindowBytes ||
            offset > m_bytes.size() || output.size() > m_bytes.size() - offset) {
            return validation::AssignError(error, "test ranged source rejected read");
        }
        std::copy_n(m_bytes.data() + static_cast<std::size_t>(offset), output.size(), output.data());
        return true;
    }
    bool CopyTo(IByteWriter&, std::string* error = nullptr) override {
        return validation::AssignError(error, "test source requires ranged reads");
    }
    mutable std::size_t readCount{0u};
    mutable std::size_t maxReadBytes{0u};
    std::size_t failRead{0u};
private:
    const std::vector<std::uint8_t>& m_bytes;
};

class WindowCheckedWriter final : public IByteWriter {
public:
    bool Write(const std::span<const std::uint8_t> input, std::string* error = nullptr) override {
        ++writeCount;
        maxWriteBytes = std::max(maxWriteBytes, input.size());
        if (writeCount == failWrite || input.size() > kIoWindowBytes) {
            return validation::AssignError(error, "test writer rejected write");
        }
        bytes.insert(bytes.end(), input.begin(), input.end());
        return true;
    }
    std::vector<std::uint8_t> bytes;
    std::size_t writeCount{0u};
    std::size_t maxWriteBytes{0u};
    std::size_t failWrite{0u};
};

inline std::vector<std::uint8_t> MakeWindowTestBytes() {
    std::vector<std::uint8_t> bytes(2u * kIoWindowBytes + 37u);
    std::uint32_t state = 0x19327acdu;
    for (auto& value : bytes) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        value = static_cast<std::uint8_t>(state);
    }
    return bytes;
}

inline bool TestWindowedByteSourceReaderChunks() {
    TestResult result;
    const auto input = MakeWindowTestBytes();
    WindowCheckedSource source(input);
    ScratchByteBufferPool pool;
    WindowedByteSourceReader reader(source, pool);
    std::string error;
    std::size_t offset = 0u;
    std::size_t count = 0u;
    for (;;) {
        std::span<const std::uint8_t> bytes;
        bool hasBytes = false;
        if (!reader.Next(bytes, hasBytes, &error)) {
            Require(result, false, "window.reader.read", error);
            break;
        }
        if (!hasBytes) { break; }
        const auto expectedBytes = std::min(kIoWindowBytes, input.size() - offset);
        Require(result, bytes.size() == expectedBytes &&
            std::equal(bytes.begin(), bytes.end(), input.begin() + offset),
            "window.reader.bytes", "ranged reader changed the window or tail bytes");
        offset += bytes.size();
        ++count;
    }
    Require(result, offset == input.size() && count == 3u && source.readCount == 3u &&
        source.maxReadBytes == kIoWindowBytes,
        "window.reader.bound", "reader must perform two full windows and one tail");
    PrintResult(result);
    return result.passed;
}

inline bool TestWindowedCopyRangesAndFailures() {
    TestResult result;
    const auto input = MakeWindowTestBytes();
    ScratchByteBufferPool pool;
    std::string error;
    VectorByteSource contiguous(input);
    WindowCheckedWriter directWriter;
    Require(result, CopyByteSourceByWindow(contiguous, directWriter, pool, &error) &&
        directWriter.bytes == input && directWriter.writeCount == 3u &&
        directWriter.maxWriteBytes == kIoWindowBytes,
        "window.copy.contiguous", "contiguous copy must preserve bounded writes");

    WindowCheckedSource ranged(input);
    WindowCheckedWriter rangedWriter;
    Require(result, CopyByteSourceByWindow(ranged, rangedWriter, pool, &error) &&
        rangedWriter.bytes == input && ranged.readCount == 3u,
        "window.copy.ranged", "ranged copy must preserve the complete payload");
    WindowCheckedWriter rangeWriter;
    Require(result, CopyByteSourceRangeByWindow(ranged, 13u, input.size() - 20u,
        rangeWriter, pool, &error) && rangeWriter.bytes ==
            std::vector<std::uint8_t>(input.begin() + 13u, input.end() - 7u),
        "window.copy.range", "offset copy must preserve windows and the partial tail");
    const auto previousReads = ranged.readCount;
    WindowCheckedWriter emptyWriter;
    Require(result, CopyByteSourceRangeByWindow(ranged, input.size(), 0u,
        emptyWriter, pool, &error) && ranged.readCount == previousReads &&
        !CopyByteSourceRangeByWindow(ranged, input.size(), 1u, emptyWriter, pool, &error) &&
        !CopyByteSourceRangeByWindow(ranged, 1u, std::numeric_limits<std::uint64_t>::max(),
            emptyWriter, pool, &error) && ranged.readCount == previousReads,
        "window.copy.rangeValidation", "invalid and empty ranges must perform no I/O");

    WindowCheckedSource failedSource(input);
    failedSource.failRead = 2u;
    WindowCheckedWriter partialWriter;
    Require(result, !CopyByteSourceByWindow(failedSource, partialWriter, pool, &error) &&
        failedSource.readCount == 2u && partialWriter.bytes.size() == kIoWindowBytes,
        "window.copy.readFailure", "read failure must stop before publishing the failed window");
    WindowCheckedSource sourceForFailedWriter(input);
    WindowCheckedWriter failedWriter;
    failedWriter.failWrite = 2u;
    Require(result, !CopyByteSourceByWindow(sourceForFailedWriter, failedWriter, pool, &error) &&
        sourceForFailedWriter.readCount == 2u && failedWriter.bytes.size() == kIoWindowBytes,
        "window.copy.writeFailure", "write failure must prevent reading subsequent windows");
    PrintResult(result);
    return result.passed;
}

inline bool TestFixedWindowFieldDecode() {
    TestResult result;
    const auto input = MakeWindowTestBytes();
    DataCodecExecutionResources root(CodecResourceParams{
        .mode = CodecResourceMode::Fixed, .maxComputeThreads = 1u,
        .ownedStorageLimitBytes = 0u});
    CacheResources resources;
    resources.BindRun(root);
    std::string error;
    for (const auto mode : {PackageFieldEncodingMode::Raw, PackageFieldEncodingMode::Zstd}) {
        CodecRunScope request(root);
        auto phase = WaitForHeavyPhase(root);
        if (!request || !phase) { return false; }
        WindowCheckedSource inputSource(input);
        WindowCheckedWriter encodedWriter;
        EncodedFieldCompressionType compression{};
        std::uint64_t rawSize = 0u;
        PackageFieldEncodingParams params;
        params.mode = mode;
        const auto encoded = EncodeLeafPackageFieldToWriter(inputSource, FieldType::Geometry,
            params, LeafPackageFieldEncodeRuntime{.run = root, .phase = *phase},
            encodedWriter, compression, rawSize, &error);
        Require(result, encoded && rawSize == input.size() &&
            encodedWriter.maxWriteBytes <= kIoWindowBytes,
            "window.field.encode", error);
        if (!encoded) { continue; }
        auto source = std::make_shared<WindowCheckedSource>(encodedWriter.bytes);
        decodefield::FieldDecodeStreamReader reader;
        const auto opened = compression == EncodedFieldCompressionType::None
            ? reader.OpenRaw(source, rawSize, resources, &error)
            : reader.Open(source, rawSize, resources, &error);
        Require(result, opened, "window.field.open", error);
        if (!opened) { continue; }
        std::vector<std::uint8_t> decoded;
        bool moved = false;
        for (;;) {
            decodefield::FieldOutputSegment segment;
            bool hasSegment = false;
            if (!reader.ReadNext(segment, hasSegment, &error)) {
                Require(result, false, "window.field.decode", error);
                break;
            }
            if (!hasSegment) { break; }
            Require(result, segment.rawOffset == decoded.size() && segment.bytes.size() <= kIoWindowBytes,
                "window.field.offset", "field output must have consecutive bounded segments");
            decoded.insert(decoded.end(), segment.bytes.begin(), segment.bytes.end());
            if (!moved) {
                // 输入窗口尚有余量时移动 reader，验证流状态与缓冲所有权一起迁移
                decodefield::FieldDecodeStreamReader next(std::move(reader));
                reader = std::move(next);
                moved = true;
            }
        }
        Require(result, decoded == input && source->maxReadBytes <= kIoWindowBytes,
            "window.field.roundTrip", "field windows changed the decoded payload");
        if (compression == EncodedFieldCompressionType::ZSTD) {
            encodedWriter.bytes.pop_back();
            decodefield::FieldDecodeStreamReader truncated;
            Require(result, truncated.Open(source, rawSize, resources, &error),
                "window.field.truncatedOpen", error);
            bool rejected = false;
            for (;;) {
                decodefield::FieldOutputSegment segment;
                bool hasSegment = false;
                if (!truncated.ReadNext(segment, hasSegment, &error)) { rejected = true; break; }
                if (!hasSegment) { break; }
            }
            Require(result, rejected, "window.field.truncated", "truncated Zstd must fail");
        }
    }
    PrintResult(result);
    return result.passed;
}

inline bool TestAttributePayloadSizedStorage() {
    TestResult result;
    const auto input = MakeWindowTestBytes();
    std::vector<std::uint8_t> encoded;
    std::string error;
    if (!codec::ZstdCodec::Compress(input, 1, 1u, encoded, &error)) {
        Require(result, false, "payload.setup", error);
        PrintResult(result);
        return false;
    }
    for (const bool externalSpill : {false, true}) {
        const auto limit = externalSpill ? 0u : static_cast<std::uint64_t>(input.size());
        DataCodecExecutionResources root(ResolvedResourceConfiguration{
            {limit, 1u, 1u}, static_cast<std::uint64_t>(input.size()), 1u, false, true, externalSpill});
        DecodeLeafWorkspace workspace;
        RunBinding binding(workspace, root);
        auto source = std::make_shared<WindowCheckedSource>(encoded);
        LeafPackageField field;
        field.source = source;
        field.rawSize = input.size();
        field.compressionType = EncodedFieldCompressionType::ZSTD;
        std::shared_ptr<bytestore::IByteSource> owner;
        std::span<const std::uint8_t> payload;
        const auto prepared = SpoolAttributePayloadToByteStore(field, workspace, owner, payload, &error);
        Require(result, prepared && owner && owner->ByteSizeHint() == input.size() &&
            root.StorageCapacity()->Snapshot().reservedBytes == limit &&
            source->maxReadBytes <= kIoWindowBytes,
            "payload.sized-owner", "full attribute payload must use the root sized store and bounded source reads");
        if (!prepared || !owner) { continue; }
        WindowCheckedWriter replay;
        Require(result, CopyByteSourceByWindow(*owner, replay, root.Scratch(), &error) && replay.bytes == input &&
            payload.empty() == externalSpill,
            "payload.range-replay", "both fixed backends must expose the same decoded bytes");
        payload = {};
        owner.reset();
        Require(result, root.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "payload.release", "last payload owner must release the complete controlled capacity");
        if (!externalSpill) {
            const auto readCount = source->readCount;
            Require(result, root.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
                !SpoolAttributePayloadToByteStore(field, workspace, owner, payload, &error) &&
                source->readCount == readCount && !owner,
                "payload.admit-before-read", "capacity rejection must happen before reading compressed input");
        }
    }
    PrintResult(result);
    return result.passed;
}

inline bool TestDecodeBusinessOptions() {
    TestResult result;
    const auto defaults = CodecControlParamsFactory::MakeDecodeConfiguration({});
    const auto audit = CodecControlParamsFactory::MakeDecodeConfiguration(
        DataCodecDecodeOptions{
            .validationProfile = DataCodecDecodeValidationProfile::Audit,
            .enableDecodedResultCache = false,
            .enableEncodedInputCache = true,
        });
    Require(result,
        defaults.decodedFrameCachePolicy.enabled &&
        !defaults.encodedInputCachePolicy.enabled &&
        !audit.decodedFrameCachePolicy.enabled &&
        audit.encodedInputCachePolicy.enabled,
        "configuration.decode.cacheSwitches",
        "cache business switches must be applied independently");
    Require(result,
        audit.controlParams.validation.decodeMode == DecodeValidationMode::Strict &&
        audit.controlParams.validation.validateTopologyReferences &&
        audit.controlParams.validation.validateFloatingPointValues,
        "configuration.decode.validation",
        "audit validation must compose with cache switches");
    PrintResult(result);
    return result.passed;
}

inline bool TestRuntimeProfileIsSourceMetadata() {
    TestResult result;
    const auto native = CodecControlParamsFactory::MakeEncodeConfiguration(
        {}, DataCodecRuntimeProfile::Native);
    for (const auto profile : {DataCodecRuntimeProfile::Wasm4GiB, DataCodecRuntimeProfile::Wasm16GiB}) {
        const auto encode = CodecControlParamsFactory::MakeEncodeConfiguration({}, profile);
        const auto decode = CodecControlParamsFactory::MakeDecodeConfiguration({}, profile);
        Require(result,
            encode.source.runtimeProfile == profile && decode.source.runtimeProfile == profile &&
            encode.pipelineControl.pointOrder == native.pipelineControl.pointOrder &&
            encode.pipelineControl.cellOrder == native.pipelineControl.cellOrder &&
            encode.pipelineControl.packageFields.zstdLevel == native.pipelineControl.packageFields.zstdLevel &&
            decode.decodedFrameCachePolicy.enabled && !decode.encodedInputCachePolicy.enabled,
            "configuration.runtime.sourceOnly",
            "runtime source metadata must preserve the selected business configuration");
    }
    PrintResult(result);
    return result.passed;
}

inline bool TestPolyhedronIndexStoresShareCapacity() {
    TestResult result;
    const std::array<std::uint64_t, 5u> counts{2u, 3u, 5u, 7u, 11u};
    const auto aggregateBytes = static_cast<std::uint64_t>(28u * sizeof(datacodec::IndexType));
    for (const bool externalSpill : {false, true}) {
        const auto limit = externalSpill ? 0u : aggregateBytes;
        auto capacity = std::make_shared<resource::ResidentByteBudget>(limit);
        ByteStoreSession session;
        session.BindStorage(capacity, externalSpill);
        std::array<DecodedIndexCache, 5u> caches;
        for (std::size_t index = 0u; index < caches.size(); ++index) {
            std::string error;
            std::vector<datacodec::IndexType> values(counts[index], static_cast<datacodec::IndexType>(index + 1u));
            const bool ready = caches[index].Initialize(counts[index], session, &error) &&
                caches[index].Append(values, &error) && caches[index].PrepareForRead(&error);
            Require(result, ready && caches[index].ByteSource()->ByteSizeHint() == values.size() * sizeof(datacodec::IndexType),
                "polyhedron.index-sized", "each complete index stream must acquire its exact size and accept every value");
            if (!ready) { continue; }
            DecodedIndexCache moved(std::move(caches[index]));
            std::vector<datacodec::IndexType> read;
            Require(result, moved.ReadRange(0u, values.size(), read) && read == values,
                "polyhedron.index-read", "memory and file index owners must both survive moves and read back exactly");
            caches[index] = std::move(moved);
        }
        Require(result, capacity->Snapshot().reservedBytes == limit,
            "polyhedron.index-shared-capacity", "all live in-memory streams must charge the same root once");
        session.ReleaseAll();
        for (auto& cache : caches) { cache.Release(); }
        Require(result, capacity->Snapshot().reservedBytes == 0u,
            "polyhedron.index-release", "stream owners must return all capacity after session closure");
    }
    ByteStoreSession session;
    auto capacity = std::make_shared<resource::ResidentByteBudget>(sizeof(datacodec::IndexType));
    session.BindStorage(capacity, false);
    DecodedIndexCache cache;
    Require(result, !cache.Initialize(2u, session) && cache.ByteSource() == nullptr && cache.Count() == 0u &&
        capacity->Snapshot().reservedBytes == 0u,
        "polyhedron.index-no-spill", "necessary index storage must fail cleanly when exact admission is denied");
    Require(result, !cache.Initialize(std::numeric_limits<std::uint64_t>::max(), session) && !cache.ByteSource(),
        "polyhedron.index-overflow", "index byte-count overflow must fail before allocation");
    PrintResult(result);
    return result.passed;
}

} // namespace datacodec::test::feature_budget

namespace datacodec::test {

inline int RunDataCodecFeatureBudget() {
    if (!feature_budget::TestByteStoreResidentBudgetAndSealLifecycle() ||
        !feature_budget::TestScratchPoolRetentionLimits() ||
        !feature_budget::TestWindowedByteSourceReaderChunks() ||
        !feature_budget::TestWindowedCopyRangesAndFailures() ||
        !feature_budget::TestFixedWindowFieldDecode() ||
        !feature_budget::TestAttributePayloadSizedStorage() ||
        !feature_budget::TestDecodeBusinessOptions() ||
        !feature_budget::TestRuntimeProfileIsSourceMetadata() ||
        !feature_budget::TestPolyhedronIndexStoresShareCapacity()) {
        return 1;
    }
    std::cout << "DataCodec budget feature tests passed\n";
    return 0;
}

} // namespace datacodec::test

#endif
