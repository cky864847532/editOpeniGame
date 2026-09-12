#ifndef DATACODEC_STORAGE_BYTESTORE_BYTESTORE_H
#define DATACODEC_STORAGE_BYTESTORE_BYTESTORE_H

#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"

#include "DataCodec/Storage/ByteIO/ByteSource.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
namespace bytestore {

// 定长范围存储允许在准入前选文件，必要连续数组固定使用内存
enum class ByteStorePurpose : std::uint8_t { Ranged, Contiguous };

inline std::uint64_t NextByteStoreSessionId() noexcept {
    static std::atomic<std::uint64_t> nextId{0u};
    return nextId.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

class IAppendableByteStore : public IByteSource {
public:
    virtual bool AppendBytes(std::span<const std::uint8_t> bytes, std::string* error = nullptr) = 0;
    virtual bool Seal(std::string* error = nullptr) = 0;

    bool AppendBytes(std::vector<std::uint8_t> bytes, std::string* error = nullptr) {
        const auto ok = AppendBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()), error);
        std::vector<std::uint8_t>().swap(bytes);
        return ok;
    }
};

class IRandomAccessByteStore : public IAppendableByteStore {
public:
    virtual bool ResizeBytes(std::uint64_t byteSize, std::string* error = nullptr) = 0;
    virtual bool WriteBytesAt(
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) = 0;
};

class AppendableByteStoreWriter final : public IByteWriter {
public:
    AppendableByteStoreWriter(std::shared_ptr<IAppendableByteStore> store, DataCodecExecutionResources& run)
        : m_store(std::move(store)), m_run(run) {}

    bool Write(std::span<const std::uint8_t> bytes, std::string* error = nullptr) override;

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return m_store != nullptr ? m_store->ByteSizeHint() : 0u;
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return m_store != nullptr ? m_store->ResidentSizeHint() : 0u;
    }

private:
    std::shared_ptr<IAppendableByteStore> m_store;
    DataCodecExecutionResources& m_run;
};

class IByteStore : public IRandomAccessByteStore {
public:
    virtual bool Append(std::span<const std::uint8_t> bytes, std::string* error = nullptr) = 0;
    virtual bool Resize(std::uint64_t byteSize, std::string* error = nullptr) = 0;
    virtual bool WriteAt(
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) = 0;
    bool AppendBytes(std::span<const std::uint8_t> bytes, std::string* error = nullptr) override {
        return Append(bytes, error);
    }
    bool ResizeBytes(std::uint64_t byteSize, std::string* error = nullptr) override {
        return Resize(byteSize, error);
    }
    bool WriteBytesAt(
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) override {
        return WriteAt(offset, bytes, error);
    }
};

class MemoryStore final : public IByteStore {
public:
    explicit MemoryStore(
        std::shared_ptr<resource::ResidentByteBudget> residentBudget,
        const bool requireSealBeforeRead = false,
        resource::StorageOwnerTag owner = {})
        : m_residentBudget(std::move(residentBudget)),
          m_requireSealBeforeRead(requireSealBeforeRead) {
        if (m_residentBudget == nullptr) {
            throw std::invalid_argument("memory store requires its capacity state");
        }
        m_owner = owner.id == 0u ? m_residentBudget->NewOwner() : owner;
    }
    ~MemoryStore() override = default;

    [[nodiscard]] resource::StorageOwnerDescription DescribeOwner() const noexcept {
        return {m_owner, m_capacity, std::chrono::steady_clock::now()};
    }

    bool Append(const std::span<const std::uint8_t> bytes, std::string* error = nullptr) override {
        if (m_released) {
            return validation::AssignError(error, "memory store was already released");
        }
        if (m_requireSealBeforeRead && m_sealed) {
            return validation::AssignError(error, "memory store was already sealed");
        }
        if (bytes.empty()) {
            return true;
        }
        const auto oldSize = m_size;
        std::size_t newSize = 0u;
        if (!validation::CheckedAddSizeT(oldSize, bytes.size(), newSize, "memory store", error)) {
            return false;
        }
        std::optional<std::size_t> sourceOffset;
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(bytes.data());
        const auto ownAddress = reinterpret_cast<std::uintptr_t>(m_bytes.get());
        if (m_bytes != nullptr && sourceAddress >= ownAddress && sourceAddress - ownAddress < m_capacity) {
            const auto offset = static_cast<std::size_t>(sourceAddress - ownAddress);
            if (offset > m_size || bytes.size() > m_size - offset) {
                return validation::AssignError(error, "memory store append source exceeds its readable range");
            }
            sourceOffset = offset;
        }
        if (!ReserveCapacity(newSize, error)) {
            return false;
        }
        const auto* source = sourceOffset ? m_bytes.get() + *sourceOffset : bytes.data();
        std::memcpy(m_bytes.get() + oldSize, source, bytes.size());
        m_size = newSize;
        m_sealed = false;
        return true;
    }

    bool Resize(const std::uint64_t byteSize, std::string* error = nullptr) override {
        if (m_released) {
            return validation::AssignError(error, "memory store was already released");
        }
        if (m_requireSealBeforeRead && m_sealed) {
            return validation::AssignError(error, "memory store was already sealed");
        }
        std::size_t localByteSize = 0u;
        if (!validation::CheckedCastSizeT(byteSize, localByteSize, "memory store resize", error)) {
            return false;
        }
        if (localByteSize > m_capacity) {
            if (!ReserveCapacity(localByteSize, error)) {
                return false;
            }
        }
        m_size = localByteSize;
        m_released = false;
        m_sealed = false;
        return true;
    }

    bool WriteAt(
        const std::uint64_t offset,
        const std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) override {
        if (m_released ||
            (m_requireSealBeforeRead && m_sealed) ||
            offset > m_size ||
            bytes.size() > m_size - static_cast<std::size_t>(offset)) {
            return validation::AssignError(error, "memory store write is outside the store range");
        }
        if (!bytes.empty()) {
            std::memcpy(m_bytes.get() + static_cast<std::size_t>(offset), bytes.data(), bytes.size());
        }
        m_sealed = false;
        return true;
    }

    bool Seal(std::string* error = nullptr) override {
        if (m_released) {
            return validation::AssignError(error, "memory store was already released");
        }
        m_sealed = true;
        return true;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return static_cast<std::uint64_t>(m_size);
    }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return static_cast<std::uint64_t>(m_capacity);
    }
    [[nodiscard]] std::span<const std::uint8_t> ContiguousBytes() const noexcept override {
        return CanRead()
            ? std::span<const std::uint8_t>(m_bytes.get(), m_size)
            : std::span<const std::uint8_t>{};
    }
    [[nodiscard]] std::span<std::uint8_t> WritableBytes() noexcept {
        return !m_released && !m_sealed
            ? std::span<std::uint8_t>(m_bytes.get(), m_size) : std::span<std::uint8_t>{};
    }
    [[nodiscard]] bool CanRead() const noexcept override {
        return !m_released && (!m_requireSealBeforeRead || m_sealed);
    }

    bool Read(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) const override {
        if (!CanRead()) {
            return validation::AssignError(error, "memory store is not sealed for reading");
        }
        if (offset > m_size || output.size() > m_size - static_cast<std::size_t>(offset)) {
            return validation::AssignError(error, "memory store read is outside the store range");
        }
        if (!output.empty()) {
            std::memcpy(output.data(), m_bytes.get() + static_cast<std::size_t>(offset), output.size());
        }
        return true;
    }

    bool CopyTo(IByteWriter& writer, std::string* error = nullptr) override {
        if (!CanRead()) {
            return validation::AssignError(error, "memory store is not sealed for copying");
        }
        std::size_t offset = 0u;
        while (offset < m_size) {
            const auto currentBytes = std::min<std::size_t>(m_size - offset, kIoWindowBytes);
            if (!writer.Write(std::span<const std::uint8_t>(m_bytes.get() + offset, currentBytes), error)) {
                return false;
            }
            offset += currentBytes;
        }
        return true;
    }

    bool PrepareCapacity(const std::uint64_t requiredBytes, DataCodecExecutionResources& run, MemoryDemandKind kind,
                         std::string* error = nullptr,
                         std::span<const resource::StorageOwnerDescription> coexist = {}) {
        std::size_t required = 0u;
        if (!validation::CheckedCastSizeT(requiredBytes, required, "memory store capacity", error)) { return false; }
        if (required <= m_capacity) { return true; }
        if (run.StorageCapacity() != m_residentBudget) {
            return validation::AssignError(error, "memory store growth requires its owning root");
        }
        // 只有 driver 能同步解除可选缓存引用，终端 worker 继续单次即时预约
        if (run.IsDriverThread()) { run.ReclaimOptionalStorage(required); }
        return ReserveCapacity(required, error, &run, coexist, kind);
    }

private:
    friend class ByteStoreSession;
    bool InitializeReserved(const std::size_t byteSize,
                            resource::ResidentByteBudget::Lease lease,
                            std::string* error) {
        if (m_bytes != nullptr || m_size != 0u || lease.Bytes() != byteSize) {
            return validation::AssignError(error, "invalid exact memory store initialization");
        }
        resource::ResidentByteBudget::AllocatedArray bytes;
        if (byteSize != 0u) {
            bytes = m_residentBudget->Allocate(lease);
            if (bytes == nullptr) {
                return validation::AssignError(error, "memory store allocation failed because memory is exhausted");
            }
        }
        m_bytes = std::move(bytes);
        m_capacityLease = std::move(lease);
        m_size = byteSize;
        m_capacity = byteSize;
        return true;
    }

    bool ReserveCapacity(const std::size_t requiredBytes, std::string* error,
                         DataCodecExecutionResources* run = nullptr,
                         std::span<const resource::StorageOwnerDescription> coexist = {},
                         MemoryDemandKind kind = MemoryDemandKind::Block) {
        if (requiredBytes <= m_capacity) {
            return true;
        }
        auto targetCapacity = requiredBytes;
        if (m_capacity != 0u) {
            const auto growth = std::max<std::size_t>(m_capacity / 2u, 1u);
            const auto boundedGrowth = std::min(growth, std::numeric_limits<std::size_t>::max() - m_capacity);
            targetCapacity = std::max(requiredBytes, m_capacity + boundedGrowth);
        }
        // 旧数组的凭证保持有效，新数组按完整容量预约一次
        const auto previous = DescribeOwner();
        std::array<resource::StorageOwnerDescription, 17u> owners{};
        std::size_t ownerCount = 0u;
        if (m_capacity != 0u) { owners[ownerCount++] = previous; }
        for (const auto& owner : coexist) {
            if (ownerCount == owners.size()) { break; }
            if (owner.owner.id == previous.owner.id) { continue; }
            owners[ownerCount++] = owner;
        }
        resource::CapacityRejection rejection;
        const bool driver = run != nullptr && run->IsDriverThread();
        auto lease = driver ? run->WaitForStorage(requiredBytes, kind, m_owner,
            std::span(owners).first(ownerCount), targetCapacity) :
            m_residentBudget->TryReserveGrowth(requiredBytes, targetCapacity, &rejection,
                m_owner, std::span(owners).first(ownerCount));
        if (!lease) {
            if (run != nullptr && !run->Stopped()) { run->RecordCapacityRejection(rejection, true); }
            return validation::AssignError(error, "memory store capacity admission was rejected");
        }
        targetCapacity = static_cast<std::size_t>(lease->Bytes());
        auto expanded = m_residentBudget->Allocate(*lease);
        if (expanded == nullptr) {
            return validation::AssignError(error, "memory store allocation failed because memory is exhausted");
        }
        if (m_bytes != nullptr && m_size != 0u) {
            std::memcpy(expanded.get(), m_bytes.get(), m_size);
        }
        m_bytes = std::move(expanded);
        m_capacityLease = std::move(*lease);
        m_capacity = targetCapacity;
        if (driver) { run->CompleteMemoryPreparation(); }
        return true;
    }

    std::shared_ptr<resource::ResidentByteBudget> m_residentBudget;
    resource::ResidentByteBudget::Lease m_capacityLease;
    resource::StorageOwnerTag m_owner;
    resource::ResidentByteBudget::AllocatedArray m_bytes;
    std::size_t m_size{0u};
    std::size_t m_capacity{0u};
    bool m_requireSealBeforeRead{false};
    bool m_sealed{false};
    bool m_released{false};
};

inline bool AppendableByteStoreWriter::Write(std::span<const std::uint8_t> bytes, std::string* error) {
    if (m_store == nullptr) {
        return validation::AssignError(error, "byte store writer is missing its backing store");
    }
    if (m_run.Stopped()) { return validation::AssignError(error, "byte store output was stopped"); }
    if (auto* memory = dynamic_cast<MemoryStore*>(m_store.get())) {
        std::uint64_t required = 0u;
        if (!validation::CheckedAddU64(memory->ByteSizeHint(), bytes.size(), required,
                "appendable store size", error) || !memory->PrepareCapacity(required, m_run,
                    MemoryDemandKind::RequiredContinuation, error)) { return false; }
    }
    return m_store->AppendBytes(bytes, error);
}

// 只在当前 driver 申请点取样已知且仍然存活的完整数组，不持有存储引用
class KnownStorageOwners final {
public:
    void Add(const IByteSource* source) noexcept {
        const auto* memory = dynamic_cast<const MemoryStore*>(source);
        if (memory == nullptr || m_count == m_values.size()) { return; }
        for (std::size_t i = 0u; i < m_count; ++i) {
            if (m_sources[i] == memory) { return; }
        }
        m_sources[m_count] = memory;
        m_values[m_count] = memory->DescribeOwner();
        ++m_count;
    }

    [[nodiscard]] std::span<const resource::StorageOwnerDescription> Entries() const noexcept {
        return {m_values.data(), m_count};
    }

private:
    // 第十七项只用于使既有拒绝记录明确标注截断
    std::array<resource::StorageOwnerDescription, 17u> m_values{};
    std::array<const MemoryStore*, 17u> m_sources{};
    std::size_t m_count{0u};
};

class FileBackedStreamStore final : public IByteStore {
public:
    explicit FileBackedStreamStore(
        std::filesystem::path path,
        const bool requireSealBeforeRead = false)
        : m_path(std::move(path)),
          m_requireSealBeforeRead(requireSealBeforeRead) {}

    ~FileBackedStreamStore() override { DestroyStorage(); }

    bool Create(std::string* error = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fileCreated) {
            return validation::AssignError(error, "file-backed store was already created");
        }
        m_file.open(m_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!m_file) {
            return validation::AssignError(error, "failed to create file-backed store");
        }
        m_fileCreated = true;
        return true;
    }

    bool Append(const std::span<const std::uint8_t> bytes, std::string* error = nullptr) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return validation::AssignError(error, "file-backed stream store was already released");
        }
        if (m_requireSealBeforeRead && m_sealed) {
            return validation::AssignError(error, "file-backed stream store was already sealed");
        }
        std::uint64_t nextSize = 0u;
        if (!validation::CheckedAddU64(
                m_byteSize,
                static_cast<std::uint64_t>(bytes.size()),
                nextSize,
                "file-backed stream store append",
                error)) {
            return false;
        }
        if (!bytes.empty() && !WriteFileRangeUnlocked(m_byteSize, bytes, error)) {
            return false;
        }
        m_byteSize = nextSize;
        m_sealed = false;
        return true;
    }

    bool Resize(const std::uint64_t byteSize, std::string* error = nullptr) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return validation::AssignError(error, "file-backed stream store was already released");
        }
        if (m_requireSealBeforeRead && m_sealed) {
            return validation::AssignError(error, "file-backed stream store was already sealed");
        }
        if (m_file.is_open()) {
            m_file.close();
        }
        m_writeOffsetKnown = false;
        std::error_code resizeError;
        std::filesystem::resize_file(m_path, byteSize, resizeError);
        if (resizeError) {
            return validation::AssignError(
                error,
                "failed to resize file-backed stream store: " + resizeError.message());
        }
        m_byteSize = byteSize;
        m_sealed = false;
        return true;
    }

    bool WriteAt(
        const std::uint64_t offset,
        const std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released ||
            (m_requireSealBeforeRead && m_sealed) ||
            offset > m_byteSize ||
            bytes.size() > m_byteSize - offset) {
            return validation::AssignError(error, "file-backed stream store write is outside the store range");
        }
        if (!bytes.empty() && !WriteFileRangeUnlocked(offset, bytes, error)) {
            return false;
        }
        m_sealed = false;
        return true;
    }

    bool Seal(std::string* error = nullptr) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return validation::AssignError(error, "file-backed stream store was already released");
        }
        if (!EnsureFileOpenUnlocked(error)) {
            return false;
        }
        m_file.flush();
        if (!m_file) {
            return validation::AssignError(error, "failed to flush file-backed stream store");
        }
        CloseFileUnlocked();
        m_sealed = true;
        return true;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override { return m_byteSize; }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override { return 0u; }
    [[nodiscard]] std::uint64_t MappedSizeHint() const noexcept override { return 0u; }
    [[nodiscard]] std::span<const std::uint8_t> ContiguousBytes() const noexcept override { return {}; }
    [[nodiscard]] bool PreferDirectCopy() const noexcept override { return false; }
    [[nodiscard]] bool CanRead() const noexcept override {
        return !m_released && (!m_requireSealBeforeRead || m_sealed);
    }

    bool Read(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!CanRead()) {
            return validation::AssignError(error, "file-backed stream store is not sealed for reading");
        }
        if (m_released || offset > m_byteSize || output.size() > m_byteSize - offset) {
            return validation::AssignError(error, "file-backed stream store read is outside the store range");
        }
        return ReadFileRangeUnlocked(offset, output, error);
    }

    bool CopyTo(IByteWriter& writer, std::string* error = nullptr) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!CanRead()) {
            return validation::AssignError(error, "file-backed stream store is not sealed for copying");
        }
        std::vector<std::uint8_t> window(kIoWindowBytes);
        std::uint64_t offset = 0u;
        while (offset < m_byteSize) {
            const auto currentBytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(m_byteSize - offset, window.size()));
            const auto output = std::span<std::uint8_t>(window.data(), currentBytes);
            if (!ReadFileRangeUnlocked(offset, output, error) ||
                !writer.Write(std::span<const std::uint8_t>(output.data(), output.size()), error)) {
                CloseFileUnlocked();
                return false;
            }
            offset += currentBytes;
        }
        CloseFileUnlocked();
        return true;
    }

private:
    void DestroyStorage() noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            return;
        }
        if (m_file.is_open()) {
            m_file.close();
        }
        if (m_fileCreated) {
            std::error_code removeError;
            std::filesystem::remove(m_path, removeError);
        }
        m_byteSize = 0u;
        m_sealed = false;
        m_released = true;
    }

private:
    void CloseFileUnlocked() const noexcept {
        if (m_file.is_open()) {
            m_file.close();
        }
        m_writeOffsetKnown = false;
    }

    bool EnsureFileOpenUnlocked(std::string* error) const {
        if (m_file.is_open()) {
            return true;
        }
        m_file.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!m_file) {
            return validation::AssignError(error, "failed to open file-backed stream store");
        }
        m_writeOffsetKnown = false;
        return true;
    }

    bool WriteFileRangeUnlocked(
        const std::uint64_t offset,
        const std::span<const std::uint8_t> bytes,
        std::string* error) const {
        if (!EnsureFileOpenUnlocked(error)) {
            return false;
        }
        std::uint64_t nextOffset = 0u;
        if (!validation::CheckedAddU64(
                offset,
                static_cast<std::uint64_t>(bytes.size()),
                nextOffset,
                "file-backed stream store write position",
                error)) {
            return false;
        }
        if (!m_writeOffsetKnown || m_writeOffset != offset) {
            m_file.clear();
            m_file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!m_file) {
                return validation::AssignError(error, "failed to seek file-backed stream store");
            }
        }
        m_file.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!m_file) {
            return validation::AssignError(error, "failed to write file-backed stream store");
        }
        m_writeOffset = nextOffset;
        m_writeOffsetKnown = true;
        return true;
    }

    bool ReadFileRangeUnlocked(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error) const {
        if (output.empty()) {
            return true;
        }
        if (!EnsureFileOpenUnlocked(error)) {
            return false;
        }
        m_file.flush();
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_writeOffsetKnown = false;
        m_file.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
        if (!m_file) {
            return validation::AssignError(error, "failed to read file-backed stream store");
        }
        return true;
    }

    std::filesystem::path m_path;
    mutable std::fstream m_file;
    bool m_fileCreated{false};
    std::uint64_t m_byteSize{0u};
    bool m_requireSealBeforeRead{false};
    bool m_sealed{false};
    bool m_released{false};
    mutable std::uint64_t m_writeOffset{0u};
    mutable bool m_writeOffsetKnown{false};
    mutable std::mutex m_mutex;
};

struct ByteStoreSessionStats {
    std::uint64_t logicalBytes{0u};
    std::uint64_t residentBytes{0u};
    std::uint64_t mappedBytes{0u};
    std::uint64_t managedFileBytes{0u};
    std::uint64_t reservedBytes{0u};
    std::uint64_t peakReservedBytes{0u};
    std::optional<std::uint64_t> capacityLimitBytes{0u};
    std::size_t storeCount{0u};
};

class ByteStoreSession final {
public:
    ByteStoreSession() = default;
    ~ByteStoreSession() { ReleaseAll(); }
    ByteStoreSession(const ByteStoreSession&) = delete;
    ByteStoreSession& operator=(const ByteStoreSession&) = delete;

    ByteStoreSession(ByteStoreSession&& other) : m_residentBudget(nullptr) {
        MoveFrom(std::move(other));
    }

    ByteStoreSession& operator=(ByteStoreSession&& other) {
        if (this != &other) {
            ReleaseAll();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    void Reset() {
        ReleaseAll();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sequence = 0u;
            m_sessionId = NextByteStoreSessionId();
            m_diagnostics.clear();
            m_released = false;
        }
    }

    void BindRun(DataCodecExecutionResources& run) {
        BindStorage(run.StorageCapacity(), run.ExternalSpillAvailable());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_run = &run;
    }

    void UnbindRun() noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_run = nullptr;
    }

    void BindStorage(std::shared_ptr<resource::ResidentByteBudget> capacity,
                     const bool externalSpillAvailable) {
        if (capacity == nullptr) {
            throw std::invalid_argument("byte store session requires root storage capacity");
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_residentBudget == capacity && m_externalSpillAvailable == externalSpillAvailable) {
            return;
        }
        std::erase_if(m_stores, [](const auto& store) { return store.expired(); });
        if (!m_stores.empty()) {
            throw std::logic_error("byte store session with live stores cannot change its root capacity");
        }
        m_residentBudget = std::move(capacity);
        m_externalSpillAvailable = externalSpillAvailable;
        m_run = nullptr;
    }

    [[nodiscard]] std::shared_ptr<MemoryStore> CreateMemoryStore(
        const bool requireSealBeforeRead = false,
        const std::string_view label = "store",
        const std::source_location site = std::source_location::current()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released || m_residentBudget == nullptr) {
            return nullptr;
        }
        auto owner = m_residentBudget->NewOwner(requireSealBeforeRead
            ? resource::StorageOwnerPurpose::Appendable : resource::StorageOwnerPurpose::Contiguous, label, site);
        auto store = std::make_shared<MemoryStore>(m_residentBudget, requireSealBeforeRead, owner);
        std::erase_if(m_stores, [](const auto& entry) { return entry.expired(); });
        m_stores.push_back(store);
        return store;
    }

    [[nodiscard]] std::shared_ptr<MemoryStore> CreateReservedMemoryStore(
        resource::ResidentByteBudget::Lease lease,
        std::string* error = nullptr) {
        // 局部凭证确保失败容量在函数返回前归还
        auto reservation = std::move(lease);
        std::shared_ptr<resource::ResidentByteBudget> capacity;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_released || m_residentBudget == nullptr || !m_residentBudget->Owns(reservation)) {
                validation::AssignError(error, "reserved store requires a lease from its bound capacity");
                return nullptr;
            }
            capacity = m_residentBudget;
        }
        std::size_t bytes = 0u;
        if (!validation::CheckedCastSizeT(reservation.Bytes(), bytes, "reserved memory store", error)) {
            return nullptr;
        }
        auto store = std::make_shared<MemoryStore>(capacity, false, reservation.Owner());
        if (!store->InitializeReserved(bytes, std::move(reservation), error) || !RegisterStore(store, error)) {
            return nullptr;
        }
        return store;
    }

    [[nodiscard]] std::shared_ptr<IByteStore> CreateSizedStore(
        const ByteStorePurpose purpose,
        const std::uint64_t byteSize,
        const MemoryDemandKind demandKind = MemoryDemandKind::Block,
        const std::string& label = "store",
        std::string* error = nullptr,
        std::span<const resource::StorageOwnerDescription> coexist = {},
        const std::source_location site = std::source_location::current()) {
        if (purpose != ByteStorePurpose::Ranged && purpose != ByteStorePurpose::Contiguous) {
            validation::AssignError(error, "unknown byte store purpose");
            return nullptr;
        }
        std::size_t localByteSize = 0u;
        if (!validation::CheckedCastSizeT(byteSize, localByteSize, "sized byte store", error)) {
            return nullptr;
        }
        std::shared_ptr<resource::ResidentByteBudget> capacity;
        bool externalSpillAvailable = false;
        DataCodecExecutionResources* run = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_released || m_residentBudget == nullptr) {
                validation::AssignError(error, "sized store requires an active storage binding");
                return nullptr;
            }
            capacity = m_residentBudget;
            externalSpillAvailable = m_externalSpillAvailable;
            run = m_run;
        }
        // 必要目标由 driver 在正式预约前逐项解除可选引用
        if (run != nullptr) { run->ReclaimOptionalStorage(byteSize); }
        // 一次预约决定后端，实际分配失败不进入文件分支
        const auto owner = capacity->NewOwner(purpose == ByteStorePurpose::Contiguous
            ? resource::StorageOwnerPurpose::Contiguous : resource::StorageOwnerPurpose::Ranged, label, site);
        resource::CapacityRejection rejection;
        const bool fatal = purpose == ByteStorePurpose::Contiguous || !externalSpillAvailable;
        const bool driver = run != nullptr && run->IsDriverThread();
        auto lease = driver ? (fatal ? run->WaitForStorage(byteSize, demandKind, owner, coexist) :
            run->TryAcquireStorage(byteSize, demandKind, &rejection, owner, coexist)) :
            capacity->TryReserve(byteSize, &rejection, owner, coexist);
        if (lease) {
            auto store = CreateReservedMemoryStore(std::move(*lease), error);
            if (driver && store) { run->CompleteMemoryPreparation(); }
            return store;
        }
        if (driver) { run->ClearByteWait(); }
        if (run != nullptr && !run->Stopped()) { run->RecordCapacityRejection(rejection, fatal); }
        if (fatal) {
            validation::AssignError(error, "sized memory store capacity admission was rejected");
            return nullptr;
        }
        auto store = CreateManagedByteStore(label, error);
        if (store == nullptr || !store->ResizeBytes(byteSize, error)) {
            return nullptr;
        }
        return store;
    }

    [[nodiscard]] std::shared_ptr<IByteStore> CreateManagedByteStore(
        const std::string& label = "store",
        std::string* error = nullptr) {
        const auto path = CreateManagedStorePath(label, error);
        if (!path.has_value()) {
            return nullptr;
        }
        auto store = std::make_shared<FileBackedStreamStore>(*path, false);
        if (!store->Create(error) || !RegisterStore(store, error)) {
            return nullptr;
        }
        return store;
    }

    [[nodiscard]] std::shared_ptr<IAppendableByteStore> CreateAppendableByteStore(
        const std::string& label = "store",
        std::string* error = nullptr,
        const std::source_location site = std::source_location::current()) {
        bool externalSpillAvailable = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_released || m_residentBudget == nullptr) {
                validation::AssignError(error, "appendable store requires an active storage binding");
                return nullptr;
            }
            externalSpillAvailable = m_externalSpillAvailable;
        }
        // 未知长度暂存在创建前选定后端，分配或写入失败不重新选择
        if (!externalSpillAvailable) {
            auto store = CreateMemoryStore(true, label, site);
            if (store == nullptr) {
                validation::AssignError(error, "failed to create appendable memory byte store");
            }
            return store;
        }
        const auto path = CreateManagedStorePath(label, error);
        if (!path.has_value()) {
            return nullptr;
        }
        auto store = std::make_shared<FileBackedStreamStore>(*path, true);
        if (!store->Create(error) || !RegisterStore(store, error)) {
            return nullptr;
        }
        return store;
    }

    [[nodiscard]] ByteStoreSessionStats SnapshotStats() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        ByteStoreSessionStats stats;
        if (m_residentBudget != nullptr) {
            const auto residentStats = m_residentBudget->Snapshot();
            stats.reservedBytes = residentStats.reservedBytes;
            stats.peakReservedBytes = residentStats.peakReservedBytes;
            stats.capacityLimitBytes = residentStats.limitBytes;
        }
        for (const auto& weakStore : m_stores) {
            const auto store = weakStore.lock();
            if (store == nullptr) {
                continue;
            }
            ++stats.storeCount;
            stats.logicalBytes = validation::SaturatingAddU64(
                stats.logicalBytes,
                store->ByteSizeHint());
            stats.residentBytes = validation::SaturatingAddU64(
                stats.residentBytes,
                store->ResidentSizeHint());
            stats.mappedBytes = validation::SaturatingAddU64(
                stats.mappedBytes,
                store->MappedSizeHint());
            if (dynamic_cast<const MemoryStore*>(store.get()) == nullptr) {
                stats.managedFileBytes = validation::SaturatingAddU64(
                    stats.managedFileBytes,
                    store->ByteSizeHint());
            }
        }
        return stats;
    }

    void Register(std::shared_ptr<IByteSource> source) {
        if (source != nullptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_released) {
                return;
            }
            RegisterUniqueLocked(source);
        }
    }

    void ReleaseAll() noexcept {
        std::vector<std::weak_ptr<IByteSource>> stores;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
            stores.swap(m_stores);
        }
    }

    [[nodiscard]] std::vector<std::string> TakeDiagnostics() {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto diagnostics = std::move(m_diagnostics);
        m_diagnostics.clear();
        return diagnostics;
    }

private:
    [[nodiscard]] std::optional<std::filesystem::path> CreateManagedStorePath(
        const std::string& label,
        std::string* error) {
        std::uint64_t sequence = 0u;
        std::uint64_t sessionId = 0u;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_released) {
                validation::AssignError(error, "failed to create byte store in a released session");
                return std::nullopt;
            }
            if (m_residentBudget == nullptr || !m_externalSpillAvailable) {
                validation::AssignError(error, "byte store session has no external spill capability");
                return std::nullopt;
            }
            sequence = ++m_sequence;
            sessionId = m_sessionId;
        }
        std::string safeLabel;
        safeLabel.reserve(label.size());
        for (const auto character : label) {
            safeLabel.push_back(
                (character >= '0' && character <= '9') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z') ||
                    character == '_'
                ? character
                : '_');
        }
        if (safeLabel.empty()) {
            safeLabel = "store";
        }
        std::filesystem::path tempDirectory;
        std::error_code pathError;
        tempDirectory = std::filesystem::temp_directory_path(pathError);
        if (pathError) {
            validation::AssignError(error, "failed to locate byte store temporary directory: " + pathError.message());
            return std::nullopt;
        }
        const auto path = tempDirectory /
            ("datacodec_" + std::to_string(sessionId) + "_" +
             std::to_string(sequence) + "_" + safeLabel + ".cache");
        return path;
    }

    template<typename TStore>
    bool RegisterStore(const std::shared_ptr<TStore>& store, std::string* error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_released) {
            validation::AssignError(error, "byte store session was released while creating a store");
            return false;
        }
        RegisterUniqueLocked(store);
        return true;
    }

    void RegisterUniqueLocked(const std::shared_ptr<IByteSource>& source) {
        std::erase_if(m_stores, [](const auto& entry) { return entry.expired(); });
        for (const auto& entry : m_stores) {
            if (!entry.owner_before(source) && !source.owner_before(entry)) {
                return;
            }
        }
        m_stores.push_back(source);
    }

    DataCodecExecutionResources* m_run{nullptr};

    void MoveFrom(ByteStoreSession&& other) {
        std::scoped_lock lock(m_mutex, other.m_mutex);
        m_stores = std::move(other.m_stores);
        m_residentBudget = other.m_residentBudget;
        m_externalSpillAvailable = other.m_externalSpillAvailable;
        m_run = std::exchange(other.m_run, nullptr);
        m_sequence = other.m_sequence;
        m_sessionId = other.m_sessionId;
        m_diagnostics = std::move(other.m_diagnostics);
        m_released = other.m_released;
        other.m_sequence = 0u;
        other.m_sessionId = NextByteStoreSessionId();
        other.m_diagnostics.clear();
        other.m_released = true;
    }

    std::vector<std::weak_ptr<IByteSource>> m_stores;
    std::shared_ptr<resource::ResidentByteBudget> m_residentBudget;
    bool m_externalSpillAvailable{false};
    std::vector<std::string> m_diagnostics;
    std::uint64_t m_sequence{0u};
    std::uint64_t m_sessionId{NextByteStoreSessionId()};
    bool m_released{false};
    mutable std::mutex m_mutex;
};

inline std::shared_ptr<IAppendableByteStore> CreateAppendableByteStore(
    ByteStoreSession& session,
    const std::string& label,
    std::string* error = nullptr) {
    return session.CreateAppendableByteStore(label, error);
}

} // namespace bytestore
} // namespace datacodec

#endif
