#ifndef DATACODEC_STORAGE_BYTEIO_SCRATCHBYTEBUFFER_H
#define DATACODEC_STORAGE_BYTEIO_SCRATCHBYTEBUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>
#include "DataCodec/Storage/ByteIO/FixedByteBuffer.h"

namespace datacodec {

inline constexpr std::size_t kMaxRetainedScratchBlockBytes = 8u * 1024u * 1024u;

struct ScratchByteBufferPoolStats {
    std::size_t maxRetainedBlockCount{0u};
    std::size_t maxRetainedBlockBytes{kMaxRetainedScratchBlockBytes};
    std::size_t activeBlockCount{0u};
    std::size_t retainedBlockCount{0u};
    // acquiredBytes 是借用时的请求量累计，向算法暴露的 vector 扩容峰值不在此处申报
    std::uint64_t acquiredBytes{0u};
    std::uint64_t retainedBytes{0u};
    std::uint64_t reusedBlockCount{0u};
    std::uint64_t allocationCount{0u};
};

namespace scratchdetail {

struct PoolState {
    explicit PoolState(std::size_t ceiling) : retainedLimit(ceiling) {
        freeBlocks.reserve(ceiling);
        fixedBlocks.reserve(ceiling);
    }

    void Return(std::vector<std::uint8_t> block, std::uint64_t generation) noexcept {
        block.clear();
        {
            std::lock_guard lock(mutex);
            --activeBlockCount;
            if (!closed && generation == returnGeneration && block.capacity() != 0u &&
                block.capacity() <= kMaxRetainedScratchBlockBytes &&
                freeBlocks.size() < retainedLimit && freeBlocks.size() < freeBlocks.capacity()) {
                retainedBytes += block.capacity();
                freeBlocks.push_back(std::move(block));
            }
        }
        // 未留存数组在池锁外析构
    }

    mutable std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> freeBlocks;
    std::vector<FixedByteBacking> fixedBlocks;
    std::size_t retainedLimit;
    std::size_t activeBlockCount{0u};
    std::uint64_t acquiredBytes{0u};
    std::uint64_t retainedBytes{0u};
    std::uint64_t reusedBlockCount{0u};
    std::uint64_t allocationCount{0u};
    std::uint64_t returnGeneration{0u};
    bool closed{false};
};

}

class ScratchByteBufferPool;

class ScratchByteBuffer final {
public:
    ScratchByteBuffer() = default;
    ScratchByteBuffer(const ScratchByteBuffer&) = delete;
    ScratchByteBuffer& operator=(const ScratchByteBuffer&) = delete;
    ScratchByteBuffer(ScratchByteBuffer&& other) noexcept
        : m_pool(std::move(other.m_pool)), m_bytes(std::move(other.m_bytes)),
          m_generation(other.m_generation) {}
    ScratchByteBuffer& operator=(ScratchByteBuffer&& other) noexcept {
        if (this != &other) {
            Release();
            m_pool = std::move(other.m_pool);
            m_bytes = std::move(other.m_bytes);
            m_generation = other.m_generation;
        }
        return *this;
    }
    ~ScratchByteBuffer() { Release(); }

    [[nodiscard]] std::vector<std::uint8_t>& Bytes() noexcept { return m_bytes; }
    [[nodiscard]] const std::vector<std::uint8_t>& Bytes() const noexcept { return m_bytes; }
    [[nodiscard]] std::size_t CapacityBytes() const noexcept { return m_bytes.capacity(); }
    [[nodiscard]] std::span<std::uint8_t> Span() noexcept { return m_bytes; }
    [[nodiscard]] std::span<const std::uint8_t> Span() const noexcept { return m_bytes; }

    void Release() noexcept {
        if (auto state = m_pool.lock()) {
            state->Return(std::move(m_bytes), m_generation);
        }
        m_pool.reset();
        std::vector<std::uint8_t>().swap(m_bytes);
    }

private:
    friend class ScratchByteBufferPool;
    ScratchByteBuffer(const std::shared_ptr<scratchdetail::PoolState>& pool,
                      std::vector<std::uint8_t> bytes, std::uint64_t generation) noexcept
        : m_pool(pool), m_bytes(std::move(bytes)), m_generation(generation) {}
    std::weak_ptr<scratchdetail::PoolState> m_pool;
    std::vector<std::uint8_t> m_bytes;
    std::uint64_t m_generation{0u};
};

class ScratchByteBufferPool final {
public:
    explicit ScratchByteBufferPool(std::size_t retainedCountCeiling = 16u)
        : m_state(std::make_shared<scratchdetail::PoolState>(retainedCountCeiling)) {}
    ScratchByteBufferPool(const ScratchByteBufferPool&) = delete;
    ScratchByteBufferPool& operator=(const ScratchByteBufferPool&) = delete;
    ~ScratchByteBufferPool() { Close(); }

    // 准入尝试独占取出精确匹配项，调用方失败时归还整组
    FixedByteBacking TakeFixed(std::size_t bytes) {
        std::lock_guard lock(m_state->mutex);
        for (auto it = m_state->fixedBlocks.begin(); it != m_state->fixedBlocks.end(); ++it) {
            if (it->size != bytes) { continue; }
            auto result = std::move(*it);
            m_state->fixedBlocks.erase(it);
            m_state->retainedBytes -= bytes;
            ++m_state->reusedBlockCount;
            return result;
        }
        return {};
    }
    FixedByteBacking AllocateFixed(resource::ResidentByteBudget& budget,
                                   resource::ResidentByteBudget::Lease lease) {
        if (!budget.Owns(lease)) { throw std::logic_error("foreign workspace lease"); }
        const auto bytes = lease.Bytes();
        auto array = budget.Allocate(lease);
        if (bytes != 0u && array == nullptr) { throw std::bad_alloc(); }
        {
            std::lock_guard lock(m_state->mutex);
            ++m_state->allocationCount;
            m_state->acquiredBytes += bytes;
        }
        return {std::move(lease), std::move(array), static_cast<std::size_t>(bytes)};
    }
    void ReturnFixed(FixedByteBacking block) noexcept {
        {
            std::lock_guard lock(m_state->mutex);
            if (!m_state->closed && block.size != 0u && block.size <= kMaxRetainedScratchBlockBytes &&
                m_state->fixedBlocks.size() + m_state->freeBlocks.size() < m_state->retainedLimit &&
                m_state->fixedBlocks.size() < m_state->fixedBlocks.capacity()) {
                m_state->retainedBytes += block.size;
                m_state->fixedBlocks.push_back(std::move(block));
            }
        }
    }
    void ClearFixed() noexcept {
        for (;;) {
            FixedByteBacking retired;
            {
                std::lock_guard lock(m_state->mutex);
                if (m_state->fixedBlocks.empty()) { return; }
                retired = std::move(m_state->fixedBlocks.back());
                m_state->fixedBlocks.pop_back();
                m_state->retainedBytes -= retired.size;
            }
        }
    }
    std::uint64_t RetainedFixedBytes() const noexcept {
        std::lock_guard lock(m_state->mutex);
        std::uint64_t bytes = 0u;
        for (const auto& block : m_state->fixedBlocks) { bytes += block.size; }
        return bytes;
    }

    // 发布只修改数量，不析构数组；根在发布目标后于锁外调用 TrimRetained
    void SetRetainedCount(std::size_t count) noexcept {
        std::lock_guard lock(m_state->mutex);
        m_state->retainedLimit = m_state->closed ? 0u : std::min(count, m_state->freeBlocks.capacity());
    }

    [[nodiscard]] ScratchByteBuffer Acquire(std::size_t bytes) {
        std::vector<std::uint8_t> block;
        bool reused = false;
        std::uint64_t generation = 0u;
        {
            std::lock_guard lock(m_state->mutex);
            generation = m_state->returnGeneration;
            auto best = m_state->freeBlocks.end();
            for (auto it = m_state->freeBlocks.begin(); it != m_state->freeBlocks.end(); ++it) {
                if (it->capacity() >= bytes &&
                    (best == m_state->freeBlocks.end() || it->capacity() < best->capacity())) {
                    best = it;
                }
            }
            if (best != m_state->freeBlocks.end()) {
                m_state->retainedBytes -= best->capacity();
                block = std::move(*best);
                m_state->freeBlocks.erase(best);
                reused = true;
            }
        }
        block.resize(bytes);
        {
            std::lock_guard lock(m_state->mutex);
            m_state->acquiredBytes += std::min<std::uint64_t>(bytes,
                std::numeric_limits<std::uint64_t>::max() - m_state->acquiredBytes);
            ++m_state->activeBlockCount;
            if (reused) { ++m_state->reusedBlockCount; }
            else { ++m_state->allocationCount; }
        }
        return ScratchByteBuffer(m_state, std::move(block), generation);
    }

    void TrimRetained() noexcept {
        ClearFixed();
        for (;;) {
            std::vector<std::uint8_t> retired;
            {
                std::lock_guard lock(m_state->mutex);
                if (m_state->freeBlocks.size() <= m_state->retainedLimit) { return; }
                retired = std::move(m_state->freeBlocks.back());
                m_state->freeBlocks.pop_back();
                m_state->retainedBytes -= retired.capacity();
            }
        }
    }

    void Clear() noexcept {
        ClearFixed();
        {
            std::lock_guard lock(m_state->mutex);
            ++m_state->returnGeneration;
        }
        for (;;) {
            std::vector<std::uint8_t> retired;
            {
                std::lock_guard lock(m_state->mutex);
                if (m_state->freeBlocks.empty()) { return; }
                retired = std::move(m_state->freeBlocks.back());
                m_state->freeBlocks.pop_back();
                m_state->retainedBytes -= retired.capacity();
            }
        }
    }

    void Close() noexcept {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->closed = true;
            m_state->retainedLimit = 0u;
        }
        Clear();
    }

    [[nodiscard]] ScratchByteBufferPoolStats SnapshotStats() const noexcept {
        std::lock_guard lock(m_state->mutex);
        return ScratchByteBufferPoolStats{
            .maxRetainedBlockCount = m_state->retainedLimit,
            .activeBlockCount = m_state->activeBlockCount,
            .retainedBlockCount = m_state->freeBlocks.size() + m_state->fixedBlocks.size(),
            .acquiredBytes = m_state->acquiredBytes,
            .retainedBytes = m_state->retainedBytes,
            .reusedBlockCount = m_state->reusedBlockCount,
            .allocationCount = m_state->allocationCount,
        };
    }

private:
    std::shared_ptr<scratchdetail::PoolState> m_state;
};

}

#endif
