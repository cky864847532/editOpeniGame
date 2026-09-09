#ifndef DATACODEC_STORAGE_BYTEIO_BYTESOURCE_H
#define DATACODEC_STORAGE_BYTEIO_BYTESOURCE_H

#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"

#include "DataCodec/Storage/ByteIO/ContiguousView.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
namespace datacodec::bytestore {

inline constexpr std::uint64_t kUnknownByteSize = std::numeric_limits<std::uint64_t>::max();

inline bool IsUnknownByteSize(const std::uint64_t byteSize) noexcept {
    return byteSize == kUnknownByteSize;
}

class IByteWriter {
public:
    virtual ~IByteWriter() = default;
    virtual bool Write(std::span<const std::uint8_t> bytes, std::string* error = nullptr) = 0;
    [[nodiscard]] virtual std::uint64_t ByteSizeHint() const noexcept { return 0u; }
    [[nodiscard]] virtual std::uint64_t ResidentSizeHint() const noexcept { return 0u; }
};

class IByteSource {
public:
    virtual ~IByteSource() = default;
    [[nodiscard]] virtual std::uint64_t ByteSizeHint() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t ResidentSizeHint() const noexcept { return ByteSizeHint(); }
    [[nodiscard]] virtual std::uint64_t MappedSizeHint() const noexcept { return 0u; }
    [[nodiscard]] virtual std::span<const std::uint8_t> ContiguousBytes() const noexcept { return {}; }
    [[nodiscard]] virtual bool PreferDirectCopy() const noexcept { return false; }
    virtual ContiguousViewStatus PrepareContiguousBytes(
        std::span<const std::uint8_t>& output,
        std::string* error = nullptr) {
        output = ContiguousBytes();
        if (!output.empty() || ByteSizeHint() == 0u) {
            if (error != nullptr) { error->clear(); }
            return ContiguousViewStatus::Ready;
        }
        if (!CanRead()) {
            validation::AssignError(error, "byte source is not readable");
            return ContiguousViewStatus::Error;
        }
        if (error != nullptr) { error->clear(); }
        return ContiguousViewStatus::Unavailable;
    }
    [[nodiscard]] virtual bool CanRead() const noexcept { return false; }
    virtual bool Read(
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error = nullptr) const {
        (void)offset;
        (void)output;
        return validation::AssignError(error, "byte source does not support ranged reads");
    }
    virtual bool CopyTo(IByteWriter& writer, std::string* error = nullptr) = 0;

    bool ReadCancellable(const std::uint64_t offset, const std::span<std::uint8_t> output,
                         const std::stop_token stop, std::string* error = nullptr) const {
        const auto size = ByteSizeHint();
        if (!CanRead() || offset > size || output.size() > size - offset) {
            return validation::AssignError(error, "cancellable byte source range is invalid");
        }
        for (std::size_t consumed = 0u; consumed < output.size();) {
            if (stop.stop_requested()) { return validation::AssignError(error, "byte source read cancelled"); }
            const auto count = std::min(kIoWindowBytes, output.size() - consumed);
            if (!Read(offset + consumed, output.subspan(consumed, count), error)) { return false; }
            consumed += count;
        }
        if (stop.stop_requested()) { return validation::AssignError(error, "byte source read cancelled"); }
        return true;
    }
};

enum class ByteSourceConsumptionMode {
    Replayable,
    OneShot,
};

// 范围仅共享原始 owner，不复制 payload，也不重复计入 owner 容量
class SubrangeByteSource final : public IByteSource {
public:
    SubrangeByteSource(std::shared_ptr<IByteSource> source, std::uint64_t offset, std::uint64_t byteSize)
        : m_source(std::move(source)), m_offset(offset), m_byteSize(byteSize) {}

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override { return m_byteSize; }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override { return 0u; }
    [[nodiscard]] bool CanRead() const noexcept override {
        if (!m_source || !m_source->CanRead()) { return false; }
        const auto size = m_source->ByteSizeHint();
        return !IsUnknownByteSize(size) && m_offset <= size && m_byteSize <= size - m_offset;
    }
    [[nodiscard]] std::span<const std::uint8_t> ContiguousBytes() const noexcept override {
        if (!CanRead()) { return {}; }
        const auto bytes = m_source->ContiguousBytes();
        if (m_offset > bytes.size() || m_byteSize > bytes.size() - m_offset) { return {}; }
        return bytes.subspan(static_cast<std::size_t>(m_offset), static_cast<std::size_t>(m_byteSize));
    }
    ContiguousViewStatus PrepareContiguousBytes(std::span<const std::uint8_t>& output,
                                               std::string* error = nullptr) override {
        output = {};
        if (!CanRead()) {
            validation::AssignError(error, "subrange byte source range is invalid");
            return ContiguousViewStatus::Error;
        }
        output = ContiguousBytes();
        if (error != nullptr) { error->clear(); }
        return !output.empty() || m_byteSize == 0u ? ContiguousViewStatus::Ready : ContiguousViewStatus::Unavailable;
    }
    bool Read(std::uint64_t offset, std::span<std::uint8_t> output,
              std::string* error = nullptr) const override {
        if (!CanRead() || offset > m_byteSize || output.size() > m_byteSize - offset) {
            return validation::AssignError(error, "subrange byte source read is outside the selected range");
        }
        return m_source->Read(m_offset + offset, output, error);
    }
    bool CopyTo(IByteWriter& writer, std::string* error = nullptr) override {
        if (!CanRead()) { return validation::AssignError(error, "subrange byte source range is invalid"); }
        const auto contiguous = ContiguousBytes();
        std::vector<std::uint8_t> buffer;
        if (contiguous.empty()) {
            buffer.resize(static_cast<std::size_t>(std::min<std::uint64_t>(m_byteSize, kIoWindowBytes)));
        }
        for (std::uint64_t offset = 0u; offset < m_byteSize;) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(m_byteSize - offset, kIoWindowBytes));
            std::span<const std::uint8_t> window;
            if (contiguous.empty()) {
                auto writable = std::span<std::uint8_t>(buffer).first(n);
                if (!Read(offset, writable, error)) { return false; }
                window = writable;
            } else {
                window = contiguous.subspan(static_cast<std::size_t>(offset), n);
            }
            if (!writer.Write(window, error)) { return false; }
            offset += n;
        }
        return true;
    }

private:
    std::shared_ptr<IByteSource> m_source;
    std::uint64_t m_offset;
    std::uint64_t m_byteSize;
};

inline bool WaitUntilByteSourceFlagReady(
    const std::atomic<std::uint8_t>& ready,
    const std::stop_token& stopToken) {
    std::stop_callback stopCallback(stopToken, [&ready]() {
        const_cast<std::atomic<std::uint8_t>&>(ready).notify_all();
    });
    for (;;) {
        const auto value = ready.load(std::memory_order_acquire);
        if (value != 0u) {
            return true;
        }
        if (stopToken.stop_requested()) {
            return false;
        }
        ready.wait(0u, std::memory_order_acquire);
    }
}

class VectorByteSource final : public IByteSource {
public:
    VectorByteSource() = default;
    explicit VectorByteSource(std::vector<std::uint8_t> bytes)
        : m_bytes(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return static_cast<std::uint64_t>(m_bytes.size());
    }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return static_cast<std::uint64_t>(m_bytes.capacity());
    }
    [[nodiscard]] std::span<const std::uint8_t> ContiguousBytes() const noexcept override {
        return std::span<const std::uint8_t>(m_bytes.data(), m_bytes.size());
    }
    [[nodiscard]] bool CanRead() const noexcept override { return true; }

    bool Read(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) const override {
        if (offset > m_bytes.size() ||
            output.size() > m_bytes.size() - static_cast<std::size_t>(offset)) {
            return validation::AssignError(error, "vector byte source read is outside the source range");
        }
        if (!output.empty()) {
            std::memcpy(
                output.data(),
                m_bytes.data() + static_cast<std::size_t>(offset),
                output.size());
        }
        return true;
    }

    bool CopyTo(IByteWriter& writer, std::string* error = nullptr) override {
        std::size_t offset = 0u;
        while (offset < m_bytes.size()) {
            const auto remaining = m_bytes.size() - offset;
            const auto currentBytes = remaining < kIoWindowBytes ? remaining : kIoWindowBytes;
            if (!writer.Write(std::span<const std::uint8_t>(m_bytes.data() + offset, currentBytes), error)) {
                return false;
            }
            offset += currentBytes;
        }
        return true;
    }

private:
    std::vector<std::uint8_t> m_bytes;
};

} // namespace datacodec::bytestore

#endif
