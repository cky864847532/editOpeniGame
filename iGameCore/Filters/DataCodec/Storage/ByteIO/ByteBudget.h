#ifndef DATACODEC_STORAGE_BYTEIO_BYTEBUDGET_H
#define DATACODEC_STORAGE_BYTEIO_BYTEBUDGET_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include "DataCodec/Common/DataCodecError.h"

namespace datacodec {
class DataCodecExecutionResources;
namespace bytestore { class MemoryStore; }
namespace resource {


struct CapacitySnapshot {
    std::uint64_t limitBytes{0u};
    std::uint64_t reservedBytes{0u};
    std::uint64_t peakReservedBytes{0u};
};

struct StorageAllocationSnapshot {
    std::uint64_t scopeId{0u};
    std::chrono::steady_clock::time_point capturedAt{};
    std::uint64_t liveBytes{0u};
    std::uint64_t peakLiveBytes{0u};
    std::uint64_t liveArrayCount{0u};
    std::uint64_t allocationCount{0u};
    std::uint64_t failedAllocationCount{0u};
};

enum class StorageOwnerPurpose : std::uint8_t { Unspecified, Ranged, Contiguous, Appendable, Optional };

struct StorageOwnerTag {
    std::uint64_t id{0u};
    StorageOwnerPurpose purpose{StorageOwnerPurpose::Unspecified};
    std::array<char, 48u> label{};
    std::array<char, 64u> file{};
    std::uint_least32_t line{0u};
    bool textTruncated{false};
};

struct StorageOwnerDescription {
    StorageOwnerTag owner;
    std::uint64_t capacityBytes{0u};
    std::chrono::steady_clock::time_point capturedAt{};
};

// 拒绝时在同一容量锁内填写，已有 owner 的容量是调用点取样值
struct CapacityRejection {
    std::chrono::steady_clock::time_point checkedAt{};
    std::uint64_t requestedBytes{0u};
    std::uint64_t reservedBytes{0u};
    std::uint64_t limitBytes{0u};
    StorageOwnerTag requester;
    std::array<StorageOwnerDescription, 16u> owners{};
    std::size_t ownerCount{0u};
    bool ownerListTruncated{false};
};
static_assert(std::is_trivially_copyable_v<CapacityRejection>);

class ResidentByteBudget final {
    struct State {
        explicit State(const std::uint64_t limit) noexcept : limitBytes(limit) {
            static std::atomic_uint64_t nextScopeId{0u};
            allocated.scopeId = nextScopeId.fetch_add(1u, std::memory_order_relaxed) + 1u;
        }
        std::mutex mutex;
        std::uint64_t limitBytes;
        std::uint64_t reservedBytes{0u};
        std::uint64_t peakReservedBytes{0u};
        std::uint64_t nextOwnerId{0u};
        // 实际数组事件使用独立锁，容量准入和控制器从不读取审计值
        std::mutex allocationMutex;
        StorageAllocationSnapshot allocated;
    };

public:
    class AllocatedArray final {
    public:
        AllocatedArray() = default;
        AllocatedArray(const AllocatedArray&) = delete;
        AllocatedArray& operator=(const AllocatedArray&) = delete;
        AllocatedArray(AllocatedArray&& other) noexcept
            : m_state(std::move(other.m_state)), m_array(std::move(other.m_array)),
              m_size(std::exchange(other.m_size, 0u)) {}
        AllocatedArray& operator=(AllocatedArray&& other) noexcept {
            if (this != &other) {
                reset();
                m_state = std::move(other.m_state);
                m_array = std::move(other.m_array);
                m_size = std::exchange(other.m_size, 0u);
            }
            return *this;
        }
        ~AllocatedArray() { reset(); }
        std::uint8_t* get() const noexcept { return m_array.get(); }
        bool operator==(std::nullptr_t) const noexcept { return m_array == nullptr; }
        void reset() noexcept {
            auto state = std::move(m_state);
            if (!state) { return; }
            std::lock_guard lock(state->allocationMutex);
            m_array.reset();
            state->allocated.liveBytes -= std::exchange(m_size, 0u);
            --state->allocated.liveArrayCount;
        }
    private:
        friend class ResidentByteBudget;
        AllocatedArray(std::shared_ptr<State> state, std::unique_ptr<std::uint8_t[]> array,
                       const std::uint64_t size) noexcept
            : m_state(std::move(state)), m_array(std::move(array)), m_size(size) {}
        std::shared_ptr<State> m_state;
        std::unique_ptr<std::uint8_t[]> m_array;
        std::uint64_t m_size{0u};
    };

    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : m_state(std::move(other.m_state)), m_bytes(std::exchange(other.m_bytes, 0u)),
              m_owner(std::exchange(other.m_owner, {})) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Reset();
                m_state = std::move(other.m_state);
                m_bytes = std::exchange(other.m_bytes, 0u);
                m_owner = std::exchange(other.m_owner, {});
            }
            return *this;
        }

        ~Lease() { Reset(); }

        void Reset() noexcept {
            auto state = std::move(m_state);
            if (state == nullptr) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            assert(m_bytes <= state->reservedBytes);
            state->reservedBytes -= std::exchange(m_bytes, 0u);
        }

        [[nodiscard]] std::uint64_t Bytes() const noexcept { return m_bytes; }
        [[nodiscard]] const StorageOwnerTag& Owner() const noexcept { return m_owner; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_state != nullptr; }

    private:
        friend class ResidentByteBudget;
        Lease(std::shared_ptr<State> state, const std::uint64_t bytes, StorageOwnerTag owner) noexcept
            : m_state(std::move(state)), m_bytes(bytes), m_owner(owner) {}
        std::shared_ptr<State> m_state;
        std::uint64_t m_bytes{0u};
        StorageOwnerTag m_owner;
    };

    explicit ResidentByteBudget(const std::uint64_t limitBytes)
        : m_state(std::make_shared<State>(limitBytes)) {}

    ResidentByteBudget(const ResidentByteBudget&) = delete;
    ResidentByteBudget& operator=(const ResidentByteBudget&) = delete;

    [[nodiscard]] StorageOwnerTag NewOwner(
        const StorageOwnerPurpose purpose = StorageOwnerPurpose::Unspecified,
        const std::string_view label = "store",
        const std::source_location site = std::source_location::current()) noexcept {
        StorageOwnerTag owner;
        owner.purpose = purpose;
        owner.textTruncated = failuredetail::CopyText(owner.label, label);
        const std::string_view file(site.file_name());
        const auto separator = file.find_last_of("/\\");
        owner.textTruncated |= failuredetail::CopyText(owner.file,
            separator == std::string_view::npos ? file : file.substr(separator + 1u));
        owner.line = site.line();
        std::lock_guard<std::mutex> lock(m_state->mutex);
        owner.id = ++m_state->nextOwnerId;
        return owner;
    }

    [[nodiscard]] std::optional<Lease> TryReserve(const std::uint64_t bytes,
        CapacityRejection* rejection = nullptr, StorageOwnerTag owner = {},
        std::span<const StorageOwnerDescription> coexist = {}) noexcept {
        return TryReserveGrowth(bytes, bytes, rejection, owner, coexist);
    }

    [[nodiscard]] std::optional<Lease> TryReserveGrowth(const std::uint64_t requiredBytes,
        const std::uint64_t preferredBytes, CapacityRejection* rejection = nullptr,
        StorageOwnerTag owner = {}, std::span<const StorageOwnerDescription> coexist = {}) noexcept {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (owner.id == 0u) { owner.id = ++m_state->nextOwnerId; }
        if (requiredBytes != 0u &&
            (m_state->reservedBytes > m_state->limitBytes ||
             requiredBytes > m_state->limitBytes - m_state->reservedBytes)) {
            if (rejection != nullptr) {
                *rejection = {};
                rejection->checkedAt = std::chrono::steady_clock::now();
                rejection->requestedBytes = requiredBytes;
                rejection->reservedBytes = m_state->reservedBytes;
                rejection->limitBytes = m_state->limitBytes;
                rejection->requester = owner;
                rejection->ownerCount = std::min(coexist.size(), rejection->owners.size());
                rejection->ownerListTruncated = coexist.size() > rejection->owners.size();
                std::copy_n(coexist.begin(), rejection->ownerCount, rejection->owners.begin());
            }
            return std::nullopt;
        }
        const auto remaining = m_state->reservedBytes <= m_state->limitBytes
            ? m_state->limitBytes - m_state->reservedBytes : 0u;
        const auto bytes = std::min(std::max(requiredBytes, preferredBytes), remaining);
        m_state->reservedBytes += bytes;
        m_state->peakReservedBytes = std::max(m_state->peakReservedBytes, m_state->reservedBytes);
        return Lease(m_state, bytes, owner);
    }

    [[nodiscard]] CapacitySnapshot Snapshot() const noexcept {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return CapacitySnapshot{
            .limitBytes = m_state->limitBytes,
            .reservedBytes = m_state->reservedBytes,
            .peakReservedBytes = m_state->peakReservedBytes,
        };
    }

    [[nodiscard]] bool Owns(const Lease& lease) const noexcept {
        return lease.m_state == m_state;
    }

private:
    friend class datacodec::bytestore::MemoryStore;
    [[nodiscard]] AllocatedArray Allocate(const Lease& lease) {
        if (!Owns(lease) || lease.Bytes() == 0u ||
            lease.Bytes() > std::numeric_limits<std::size_t>::max()) { return {}; }
        std::lock_guard lock(m_state->allocationMutex);
        std::unique_ptr<std::uint8_t[]> array;
        try { array.reset(new (std::nothrow) std::uint8_t[static_cast<std::size_t>(lease.Bytes())]); }
        catch (...) { ++m_state->allocated.failedAllocationCount; throw; }
        if (!array) { ++m_state->allocated.failedAllocationCount; return {}; }
        auto& allocated = m_state->allocated;
        allocated.liveBytes += lease.Bytes();
        allocated.peakLiveBytes = std::max(allocated.peakLiveBytes, allocated.liveBytes);
        ++allocated.liveArrayCount;
        ++allocated.allocationCount;
        return AllocatedArray(m_state, std::move(array), lease.Bytes());
    }

public:
    [[nodiscard]] StorageAllocationSnapshot AllocatedStorage() const noexcept {
        std::lock_guard lock(m_state->allocationMutex);
        auto snapshot = m_state->allocated;
        snapshot.capturedAt = std::chrono::steady_clock::now();
        return snapshot;
    }

private:
    friend class datacodec::DataCodecExecutionResources;

    // 根执行对象依次取得执行锁和容量锁后统一发布目标
    void SetLimitLocked(const std::uint64_t limitBytes) noexcept {
        m_state->limitBytes = limitBytes;
    }

    std::shared_ptr<State> m_state;
};


} // namespace resource
} // namespace datacodec

#endif
