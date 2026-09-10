#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREENCODEDINPUTCACHE_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREENCODEDINPUTCACHE_H

#include "DataCodec/Runtime/Cache/DecodeCacheRuntime.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace datacodec::test::feature_encoded_input_cache {

class CountingInputReader final : public IByteRangeReader {
public:
    explicit CountingInputReader(std::vector<std::uint8_t> bytes, std::size_t failAt = 0u)
        : m_bytes(std::move(bytes)), m_failAt(failAt) {}
    std::uint64_t ByteSize() const noexcept override { return m_bytes.size(); }
    bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output,
                std::string* error = nullptr) override {
        ++readCount;
        maxWindow = std::max(maxWindow, output.size());
        if (readCount == m_failAt) {
            return validation::AssignError(error, "injected input read failure");
        }
        if (offset > m_bytes.size() || output.size() > m_bytes.size() - offset) {
            return validation::AssignError(error, "input read outside buffer");
        }
        std::memcpy(output.data(), m_bytes.data() + offset, output.size());
        return true;
    }
    std::size_t readCount{0u};
    std::size_t maxWindow{0u};
private:
    std::vector<std::uint8_t> m_bytes;
    std::size_t m_failAt;
};

inline DataCodecExecutionResources MakeRun(std::uint64_t capacity) {
    return DataCodecExecutionResources(ResolvedResourceConfiguration{
        .initialLimits = {capacity, 1u, 1u},
        .storageCeilingBytes = capacity,
        .computeCeiling = 1u,
        .threaded = false,
    });
}

inline DecodeSourceIdentity Source(const std::string& revision) {
    return {.stableId = "encoded-input-test", .revision = revision};
}

inline EncodedInputBuffer Input(std::uint8_t value) {
    return std::make_shared<MemoryByteRangeReader>(
        std::make_shared<const std::vector<std::uint8_t>>(1u, value));
}

inline bool TestCountAndActiveConsumer() {
    EncodedInputLruCache cache;
    cache.Configure(2u);
    auto consumer = Input(1u);
    std::weak_ptr<IByteRangeReader> lifetime = consumer;
    if (!cache.Store(Source("a"), consumer, EncodedInputAccessKind::UserRequest).IsStored() ||
        !cache.Store(Source("b"), Input(2u), EncodedInputAccessKind::Prefetch).IsStored()) {
        return false;
    }
    (void)cache.Find(Source("a"), EncodedInputAccessKind::UserRequest);
    if (!cache.Store(Source("c"), Input(3u), EncodedInputAccessKind::UserRequest).IsStored() ||
        !cache.Find(Source("b"), EncodedInputAccessKind::UserRequest).IsMiss() ||
        !cache.TrimOne() || lifetime.expired()) { return false; }
    std::array<std::uint8_t, 1u> byte{};
    if (!consumer->ReadAt(0u, byte) || byte[0] != 1u) { return false; }
    consumer.reset();
    cache.Configure(0u);
    return lifetime.expired() && cache.Statistics().residentInputs == 0u &&
        cache.Store(Source("d"), Input(4u), EncodedInputAccessKind::UserRequest).IsRejectedByPolicy();
}

inline bool TestBorrowedInputAndRootIsolation() {
    auto run = MakeRun(64u);
    CodecRunScope scope(run);
    auto host = Input(3u);
    auto& runtime = run.Caches();
    std::string error;
    auto retained = runtime.EncodedInputLoader().Load(run, runtime.DefaultEncodedInputCache(),
        Source("borrowed"), host, EncodedInputAccessKind::UserRequest, &error);
    auto other = MakeRun(64u);
    return scope && retained == host && error.empty() &&
        run.StorageCapacity()->Snapshot().reservedBytes == 0u &&
        other.Caches().DefaultEncodedInputCache()->Find(
            Source("borrowed"), EncodedInputAccessKind::UserRequest).IsMiss() &&
        scope.Finish(true);
}

inline bool TestExactCapacityWindowsAndLifetime() {
    constexpr std::size_t size = 2u * kIoWindowBytes + 13u;
    auto run = MakeRun(size);
    CodecRunScope scope(run);
    auto source = std::make_shared<CountingInputReader>(std::vector<std::uint8_t>(size, 77u));
    auto& runtime = run.Caches();
    auto cache = runtime.DefaultEncodedInputCache();
    std::string error;
    auto first = runtime.EncodedInputLoader().Load(run, cache, Source("large"), source,
        EncodedInputAccessKind::UserRequest, &error);
    auto second = runtime.EncodedInputLoader().Load(run, cache, Source("large"), source,
        EncodedInputAccessKind::UserRequest, &error);
    if (!first || first == source || second != first || !error.empty() ||
        source->readCount != 3u || source->maxWindow != kIoWindowBytes ||
        run.StorageCapacity()->Snapshot().reservedBytes != size) { return false; }
    if (!run.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck)) { return false; }
    run.ServiceDriverEvents();
    std::array<std::uint8_t, 1u> tail{};
    if (cache->Statistics().residentInputs != 0u ||
        run.StorageCapacity()->Snapshot().reservedBytes != size ||
        !first->ReadAt(size - 1u, tail) || tail[0] != 77u) { return false; }
    second.reset();
    first.reset();
    return run.StorageCapacity()->Snapshot().reservedBytes == 0u && scope.Finish(true);
}

inline bool TestAdmissionDenialDoesNotStartRead() {
    auto run = MakeRun(1u);
    CodecRunScope scope(run);
    auto source = std::make_shared<CountingInputReader>(std::vector<std::uint8_t>(4u, 2u));
    auto& runtime = run.Caches();
    std::string error;
    auto result = runtime.EncodedInputLoader().Load(run, runtime.DefaultEncodedInputCache(),
        Source("denied"), source, EncodedInputAccessKind::UserRequest, &error);
    return result == source && source->readCount == 0u && error.empty() &&
        run.StorageCapacity()->Snapshot().reservedBytes == 0u && scope.Finish(true);
}

inline bool TestNecessaryStorageReclaimsOnlyReleasedOwners() {
    auto run = MakeRun(64u);
    CodecRunScope scope(run);
    auto source = std::make_shared<CountingInputReader>(std::vector<std::uint8_t>(64u, 5u));
    auto& runtime = run.Caches();
    std::string error;
    auto retained = runtime.EncodedInputLoader().Load(run, runtime.DefaultEncodedInputCache(),
        Source("necessary"), source, EncodedInputAccessKind::UserRequest, &error);
    if (!retained || retained == source) { return false; }
    bytestore::ByteStoreSession stores;
    stores.BindRun(run);
    auto phase = WaitForHeavyPhase(run);
    if (!phase) { return false; }
    auto denied = stores.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 1u, "denied", &error);
    if (denied || runtime.DefaultEncodedInputCache()->Statistics().residentInputs != 0u ||
        run.StorageCapacity()->Snapshot().reservedBytes != 64u ||
        !run.Stopped() || !run.FirstFailure() || error.empty()) { return false; }
    // 必要容量拒绝终止当前请求，外部消费者在失败收束后仍持有真实数组
    stores.UnbindRun();
    phase.reset();
    if (scope.Finish(false)) { return false; }
    std::array<std::uint8_t, 1u> tail{};
    if (!retained->ReadAt(63u, tail) || tail[0] != 5u ||
        run.StorageCapacity()->Snapshot().reservedBytes != 64u) { return false; }
    retained.reset();
    if (run.StorageCapacity()->Snapshot().reservedBytes != 0u) { return false; }
    // 独立后续请求复用根，首错和停止状态由 BeginRun 重置
    CodecRunScope nextScope(run);
    if (!nextScope || run.FirstFailure() || run.Stopped()) { return false; }
    stores.BindRun(run);
    phase = WaitForHeavyPhase(run);
    if (!phase) { return false; }
    error.clear();
    auto acquired = stores.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 64u, "necessary", &error);
    if (!acquired || !error.empty() || run.StorageCapacity()->Snapshot().reservedBytes != 64u) { return false; }
    acquired.reset();
    stores.UnbindRun();
    phase.reset();
    return run.StorageCapacity()->Snapshot().reservedBytes == 0u && nextScope.Finish(true);
}

inline bool TestStartedReadFailureDoesNotReplay() {
    const auto size = kIoWindowBytes + 7u;
    auto run = MakeRun(size);
    CodecRunScope scope(run);
    auto source = std::make_shared<CountingInputReader>(std::vector<std::uint8_t>(size), 2u);
    auto& runtime = run.Caches();
    std::string error;
    const auto result = runtime.EncodedInputLoader().Load(run, runtime.DefaultEncodedInputCache(),
        Source("failure"), source, EncodedInputAccessKind::UserRequest, &error);
    return !result && run.FirstFailure().has_value() && !error.empty() &&
        source->readCount == 2u && runtime.DefaultEncodedInputCache()->Statistics().residentInputs == 0u &&
        run.StorageCapacity()->Snapshot().reservedBytes == 0u && !scope.Finish(false);
}

}

namespace datacodec::test {
inline int RunDataCodecFeatureEncodedInputCache() {
    using namespace feature_encoded_input_cache;
    if (!RunNamedCheck("encodedInputCache.count-consumer", TestCountAndActiveConsumer) ||
        !RunNamedCheck("encodedInputCache.borrowed-root-isolation", TestBorrowedInputAndRootIsolation) ||
        !RunNamedCheck("encodedInputCache.exact-windows-last-owner", TestExactCapacityWindowsAndLifetime) ||
        !RunNamedCheck("encodedInputCache.denied-before-read", TestAdmissionDenialDoesNotStartRead) ||
        !RunNamedCheck("encodedInputCache.necessary-live-owner", TestNecessaryStorageReclaimsOnlyReleasedOwners) ||
        !RunNamedCheck("encodedInputCache.read-failure-no-replay", TestStartedReadFailureDoesNotReplay)) {
        std::cerr << "DataCodec encoded input cache feature test failed\n";
        return 1;
    }
    return 0;
}
}

#endif
