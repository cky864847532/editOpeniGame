#include "DataCodec/API/Output/EncodedBuffer.h"
#include "DataCodec/Storage/ByteIO/ByteRange.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"

#include <array>

namespace datacodec {

const std::uint8_t* EncodedBuffer::data() const noexcept { return span().data(); }
std::size_t EncodedBuffer::size() const noexcept { return span().size(); }
std::span<const std::uint8_t> EncodedBuffer::span() const noexcept {
    return {m_bytes.get(), m_size};
}

MemoryByteRangeOutput::MemoryByteRangeOutput(DataCodecExecutionResources& run)
    : m_run(run), m_store(std::make_shared<bytestore::MemoryStore>(run.StorageCapacity(), true)) {}

bool MemoryByteRangeOutput::ZeroRange(std::uint64_t offset, std::uint64_t length, std::string* error) {
    static constexpr std::array<std::uint8_t, 4096u> zeros{};
    while (length != 0u) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, zeros.size()));
        if (!m_store->WriteAt(offset, std::span(zeros).first(count), error)) { return false; }
        offset += count;
        length -= count;
    }
    return true;
}

bool MemoryByteRangeOutput::PrepareExactSize(std::uint64_t size, std::string* error,
    std::span<const resource::StorageOwnerDescription> coexist) {
    if (!m_store || m_finalized || m_failed || m_store->ByteSizeHint() != 0u || m_preparedSize) {
        m_failed = true;
        return validation::AssignError(error, "memory output must be prepared before its first write");
    }
    if (!m_store->PrepareCapacity(size, m_run, MemoryDemandKind::RequiredContinuation, error, coexist) ||
        !m_store->Resize(size, error) || !ZeroRange(0u, size, error)) {
        m_failed = true;
        return false;
    }
    m_preparedSize = size;
    return true;
}

bool MemoryByteRangeOutput::WriteAt(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error) {
    if (!m_store || m_finalized || m_failed) {
        return validation::AssignError(error, "memory output is not writable");
    }
    std::uint64_t required = 0u;
    if (!validation::CheckedAddU64(offset, bytes.size(), required, "memory output range", error)) {
        m_failed = true;
        return false;
    }
    if (m_preparedSize && required > *m_preparedSize) {
        m_failed = true;
        return validation::AssignError(error, "memory output write exceeds its prepared layout");
    }
    const auto previousSize = m_store->ByteSizeHint();
    if ((required > previousSize && (!m_store->PrepareCapacity(required, m_run, MemoryDemandKind::RequiredContinuation, error) ||
                                    !m_store->Resize(required, error))) ||
        (offset > previousSize && !ZeroRange(previousSize, offset - previousSize, error)) ||
        !m_store->WriteAt(offset, bytes, error)) {
        m_failed = true;
        return false;
    }
    return true;
}

bool MemoryByteRangeOutput::Finalize(std::uint64_t size, std::string* error) {
    if (!m_store || m_finalized || m_failed || (m_preparedSize && size != *m_preparedSize)) {
        m_failed = true;
        return validation::AssignError(error, "memory output cannot finalize an invalid layout or failed write");
    }
    const auto previousSize = m_store->ByteSizeHint();
    if (!m_store->PrepareCapacity(size, m_run, MemoryDemandKind::RequiredContinuation, error) || !m_store->Resize(size, error) ||
        (size > previousSize && !ZeroRange(previousSize, size - previousSize, error)) ||
        !m_store->Seal(error)) {
        m_failed = true;
        return false;
    }
    m_finalized = true;
    return true;
}

std::span<const std::uint8_t> MemoryByteRangeOutput::Bytes() const noexcept {
    return m_store && m_finalized && !m_failed ? m_store->ContiguousBytes() : std::span<const std::uint8_t>{};
}

EncodedBuffer MemoryByteRangeOutput::TakeBytes() noexcept {
    if (!m_finalized || m_failed) { return {}; }
    if (!m_store) { return {}; }
    const auto size = static_cast<std::size_t>(m_store->ByteSizeHint());
    const auto capacity = static_cast<std::size_t>(m_store->ResidentSizeHint());
    auto bytes = m_store->TakeOwnedBytes();
    m_store.reset();
    return EncodedBuffer(std::move(bytes), size, capacity);
}

}
