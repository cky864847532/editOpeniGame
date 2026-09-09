#ifndef DATACODEC_RUNTIME_CACHE_ENCODEDINPUTCACHELOADER_H
#define DATACODEC_RUNTIME_CACHE_ENCODEDINPUTCACHELOADER_H

#include "DataCodec/Runtime/Cache/EncodedInputLruCache.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace datacodec {

// 同一根内按来源合并加载，等待者随根首错及取消唤醒
class EncodedInputCacheLoader final {
public:
    [[nodiscard]] EncodedInputBuffer Load(
        DataCodecExecutionResources& run,
        const std::shared_ptr<EncodedInputLruCache>& cache,
        const DecodeSourceIdentity& sourceIdentity,
        const std::shared_ptr<IByteRangeReader>& sourceReader,
        const EncodedInputAccessKind accessKind,
        std::string* error = nullptr) noexcept {
        try {
            if (error != nullptr) { error->clear(); }
            if (!sourceReader || !sourceIdentity.IsStable()) {
                Fail(run, "encoded input source is invalid");
                return Deliver(run, {}, error);
            }
            if (run.Stopped()) { return Deliver(run, {}, error); }
            if (!cache || !run.OptionalRetentionAllowed()) { return sourceReader; }
            const auto lookup = cache->Find(sourceIdentity, accessKind);
            if (lookup.IsError()) {
                Fail(run, lookup.error);
                return Deliver(run, {}, error);
            }
            if (lookup.IsHit()) {
                if (!lookup.value) { Fail(run, "encoded input cache returned a null hit"); }
                return Deliver(run, lookup.value, error);
            }

            std::shared_ptr<InFlight> state;
            {
                std::unique_lock lock(m_mutex);
                const auto existing = m_inFlight.find(sourceIdentity);
                if (existing != m_inFlight.end()) {
                    state = existing->second;
                    const auto stop = run.StopToken();
                    state->ready.wait(lock, stop, [&] { return state->completed; });
                    auto input = state->input;
                    lock.unlock();
                    return Deliver(run, std::move(input), error);
                }
                state = std::make_shared<InFlight>();
                m_inFlight.emplace(sourceIdentity, state);
            }

            EncodedInputBuffer input;
            try {
                input = Prepare(run, sourceReader, error);
                if (input && run.OptionalRetentionAllowed()) {
                    const auto stored = cache->Store(sourceIdentity, input, accessKind);
                    if (stored.IsError()) {
                        Fail(run, stored.error);
                        input.reset();
                    }
                }
            } catch (...) {
                RecordExecutionException(run, "EncodedInputCacheLoader");
            }
            if (run.Stopped()) { input.reset(); }
            {
                std::lock_guard lock(m_mutex);
                state->input = std::move(input);
                state->completed = true;
                m_inFlight.erase(sourceIdentity);
            }
            state->ready.notify_all();
            return Deliver(run, state->input, error);
        } catch (...) {
            RecordExecutionException(run, "EncodedInputCacheLoader");
            return Deliver(run, {}, error);
        }
    }

private:
    struct InFlight {
        std::condition_variable_any ready;
        EncodedInputBuffer input;
        bool completed{false};
    };

    static void Fail(DataCodecExecutionResources& run, std::string_view message) noexcept {
        run.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::DecodeFailure,
            "encoded-input-failed", "EncodedInputCacheLoader", message));
    }

    static EncodedInputBuffer Deliver(DataCodecExecutionResources& run,
                                      EncodedInputBuffer input, std::string* error) noexcept {
        if (run.Stopped()) { input.reset(); }
        if (!input && error != nullptr) {
            try {
                const auto failure = run.FirstFailure();
                error->assign(failure ? failure->message.data() : "encoded input load cancelled");
            } catch (...) {}
        }
        return input;
    }

    static EncodedInputBuffer Prepare(DataCodecExecutionResources& run,
                                       const EncodedInputBuffer& source, std::string* error) {
        // 已有连续输入只保留其宿主或原始自有 owner
        if (source->RetainAllBytes() != nullptr ||
            source->ContiguousRange(0u, source->ByteSize()).size() == source->ByteSize()) {
            return source;
        }
        auto phase = WaitForHeavyPhase(run);
        if (!phase) { return {}; }
        if (!run.OptionalRetentionAllowed()) { return source; }
        const auto size = source->ByteSize();
        std::size_t localSize = 0u;
        if (!validation::CheckedCastSizeT(size, localSize, "encoded input byte size", error)) {
            Fail(run, "encoded input byte size exceeds address space");
            return {};
        }
        auto capacity = run.StorageCapacity();
        const auto tag = capacity->NewOwner(resource::StorageOwnerPurpose::Optional, "encoded-input-prefetch");
        resource::CapacityRejection rejection;
        auto lease = capacity->TryReserve(size, &rejection, tag);
        if (!lease) {
            run.RecordCapacityRejection(rejection, false);
            return source;
        }
        bytestore::ByteStoreSession session;
        session.BindStorage(run.StorageCapacity(), run.ExternalSpillAvailable());
        auto owner = session.CreateReservedMemoryStore(std::move(*lease), error);
        if (!owner) {
            Fail(run, "encoded input allocation failed");
            return {};
        }
        auto bytes = owner->WritableBytes();
        for (std::size_t offset = 0u; offset < localSize;) {
            if (run.Stopped()) { return {}; }
            const auto count = std::min(localSize - offset, kIoWindowBytes);
            if (!source->ReadAtCancellable(offset, bytes.subspan(offset, count), run.StopToken(), error)) {
                Fail(run, error ? std::string_view(*error) : "encoded input read failed");
                return {};
            }
            offset += count;
        }
        if (!owner->Seal(error)) {
            Fail(run, "encoded input seal failed");
            return {};
        }
        return std::make_shared<MemoryByteRangeReader>(owner, owner->ContiguousBytes());
    }

    std::mutex m_mutex;
    std::unordered_map<DecodeSourceIdentity, std::shared_ptr<InFlight>, DecodeSourceIdentityHash> m_inFlight;
};

}

#endif
