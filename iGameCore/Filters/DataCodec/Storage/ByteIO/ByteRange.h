#ifndef DATACODEC_STORAGE_BYTEIO_BYTERANGE_H
#define DATACODEC_STORAGE_BYTEIO_BYTERANGE_H

#include "DataCodec/Storage/ByteIO/ContiguousView.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/API/Output/EncodedBuffer.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
class DataCodecExecutionResources;

enum class ByteRangePrefetchStatus : std::uint8_t {
    Accepted = 0u,
    RejectedByPolicy = 1u,
    Unavailable = 2u,
    Error = 3u,
};

struct ByteRangePrefetchResult {
    ByteRangePrefetchStatus status{ByteRangePrefetchStatus::Unavailable};
    std::string error;

    [[nodiscard]] bool IsAccepted() const noexcept {
        return status == ByteRangePrefetchStatus::Accepted;
    }
    [[nodiscard]] bool IsRejectedByPolicy() const noexcept {
        return status == ByteRangePrefetchStatus::RejectedByPolicy;
    }
    [[nodiscard]] bool IsUnavailable() const noexcept {
        return status == ByteRangePrefetchStatus::Unavailable;
    }
    [[nodiscard]] bool IsError() const noexcept {
        return status == ByteRangePrefetchStatus::Error;
    }
};

class IByteRangeReader {
public:
    virtual ~IByteRangeReader() = default;
    [[nodiscard]] virtual std::uint64_t ByteSize() const noexcept = 0;
    // 可选的连续只读区间能力
    // 返回值的生命周期由 reader 自身保证，空 span 表示当前 reader 不提供连续视图
    [[nodiscard]] virtual std::span<const std::uint8_t> ContiguousRange(
        std::uint64_t offset,
        std::uint64_t byteSize) const noexcept {
        (void)offset;
        (void)byteSize;
        return {};
    }
    virtual ContiguousViewStatus PrepareContiguousRange(
        const std::uint64_t offset,
        const std::uint64_t byteSize,
        std::span<const std::uint8_t>& output,
        std::string* error = nullptr) const {
        output = {};
        if (offset > ByteSize() || byteSize > ByteSize() - offset) {
            validation::AssignError(error, "contiguous byte range is outside the reader");
            return ContiguousViewStatus::Error;
        }
        output = ContiguousRange(offset, byteSize);
        if (byteSize == 0u || output.size() == byteSize) {
            if (error != nullptr) { error->clear(); }
            return ContiguousViewStatus::Ready;
        }
        if (!output.empty()) {
            output = {};
            validation::AssignError(error, "contiguous byte range has an invalid size");
            return ContiguousViewStatus::Error;
        }
        if (error != nullptr) { error->clear(); }
        return ContiguousViewStatus::Unavailable;
    }
    // 可选的完整输入共享所有权能力
    // 返回非空值时调用方可以直接复用既有字节，不需要重新复制整份输入
    [[nodiscard]] virtual std::shared_ptr<const std::vector<std::uint8_t>> RetainAllBytes() const noexcept {
        return {};
    }
    // 可选的输入预取能力
    // 默认实现不改变读取语义，文件桥接可据此提前准备映射页面
    [[nodiscard]] virtual ByteRangePrefetchResult PrefetchRange(
        std::uint64_t offset,
        std::uint64_t byteSize) const {
        (void)offset;
        (void)byteSize;
        return {.status = ByteRangePrefetchStatus::Unavailable};
    }
    virtual bool ReadAt(
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error = nullptr) = 0;

    // 一次只交付一个固定窗口，取消在已有 I/O 返回后和下一窗口开始前生效
    bool ReadAtCancellable(const std::uint64_t offset, const std::span<std::uint8_t> output,
                           const std::stop_token stop, std::string* error = nullptr) {
        if (offset > ByteSize() || output.size() > ByteSize() - offset) {
            return validation::AssignError(error, "cancellable byte range is outside the source");
        }
        std::size_t consumed = 0u;
        while (consumed < output.size()) {
            if (stop.stop_requested()) { return validation::AssignError(error, "byte range read cancelled"); }
            const auto count = std::min(output.size() - consumed, kIoWindowBytes);
            if (!ReadAt(offset + consumed, output.subspan(consumed, count), error)) { return false; }
            consumed += count;
        }
        if (stop.stop_requested()) { return validation::AssignError(error, "byte range read cancelled"); }
        return true;
    }
};

class IByteRangeOutput {
public:
    virtual ~IByteRangeOutput() = default;
    virtual bool WriteAt(
        std::uint64_t offset,
        std::span<const std::uint8_t> bytes,
        std::string* error = nullptr) = 0;
    virtual bool Finalize(std::uint64_t logicalSize, std::string* error = nullptr) = 0;
};

class MemoryByteRangeReader final : public IByteRangeReader {
public:
    explicit MemoryByteRangeReader(std::shared_ptr<const std::vector<std::uint8_t>> bytes)
        : m_bytes(std::move(bytes)) {}

    explicit MemoryByteRangeReader(EncodedBuffer bytes)
        : m_encoded(std::make_shared<const EncodedBuffer>(std::move(bytes))) {}

    explicit MemoryByteRangeReader(std::shared_ptr<const EncodedBuffer> bytes)
        : m_encoded(std::move(bytes)) {}

    // 连续视图只借用传入 owner 的字节，容量归原 owner 管理
    MemoryByteRangeReader(std::shared_ptr<const void> owner, std::span<const std::uint8_t> bytes)
        : m_retainedOwner(std::move(owner)), m_retainedBytes(bytes) {
        if (!m_retainedOwner && !bytes.empty()) {
            throw std::invalid_argument("memory byte range requires a retained input owner");
        }
    }

    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept {
        if (m_retainedOwner) { return m_retainedBytes; }
        if (m_encoded) { return m_encoded->span(); }
        return m_bytes ? std::span<const std::uint8_t>(*m_bytes) : std::span<const std::uint8_t>{};
    }

    [[nodiscard]] std::uint64_t ByteSize() const noexcept override { return Bytes().size(); }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> RetainAllBytes() const noexcept override {
        return m_bytes;
    }

    [[nodiscard]] std::span<const std::uint8_t> ContiguousRange(
        const std::uint64_t offset,
        const std::uint64_t byteSize) const noexcept override {
        const auto bytes = Bytes();
        if (offset > bytes.size() || byteSize > bytes.size() - offset) { return {}; }
        return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(byteSize));
    }

    bool ReadAt(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) override {
        const auto bytes = Bytes();
        if (offset > bytes.size() || output.size() > bytes.size() - offset) {
            return validation::AssignError(error, "memory byte range reader read range is outside the buffer");
        }
        if (!output.empty()) {
            std::memcpy(output.data(), bytes.data() + static_cast<std::size_t>(offset), output.size());
        }
        return true;
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
    std::shared_ptr<const EncodedBuffer> m_encoded;
    std::shared_ptr<const void> m_retainedOwner;
    std::span<const std::uint8_t> m_retainedBytes;
};

class SubrangeByteRangeReader final : public IByteRangeReader {
public:
    SubrangeByteRangeReader(
        std::shared_ptr<IByteRangeReader> source,
        const std::uint64_t offset,
        const std::uint64_t byteSize)
        : m_source(std::move(source)), m_offset(offset), m_byteSize(byteSize) {}

    [[nodiscard]] std::uint64_t ByteSize() const noexcept override { return m_byteSize; }

    [[nodiscard]] std::span<const std::uint8_t> ContiguousRange(
        const std::uint64_t offset,
        const std::uint64_t byteSize) const noexcept override {
        if (m_source == nullptr || offset > m_byteSize || byteSize > m_byteSize - offset) {
            return {};
        }
        std::uint64_t sourceOffset = 0u;
        if (!validation::CanAddU64(m_offset, offset)) {
            return {};
        }
        sourceOffset = m_offset + offset;
        return m_source->ContiguousRange(sourceOffset, byteSize);
    }

    ContiguousViewStatus PrepareContiguousRange(
        const std::uint64_t offset,
        const std::uint64_t byteSize,
        std::span<const std::uint8_t>& output,
        std::string* error = nullptr) const override {
        output = {};
        if (m_source == nullptr || offset > m_byteSize || byteSize > m_byteSize - offset) {
            validation::AssignError(error, "subrange contiguous view is outside the selected range");
            return ContiguousViewStatus::Error;
        }
        std::uint64_t sourceOffset = 0u;
        if (!validation::CheckedAddU64(
                m_offset,
                offset,
                sourceOffset,
                "subrange contiguous view offset",
                error)) {
            return ContiguousViewStatus::Error;
        }
        return m_source->PrepareContiguousRange(sourceOffset, byteSize, output, error);
    }

    [[nodiscard]] ByteRangePrefetchResult PrefetchRange(
        const std::uint64_t offset,
        const std::uint64_t byteSize) const override {
        if (m_source == nullptr || offset > m_byteSize || byteSize > m_byteSize - offset ||
            !validation::CanAddU64(m_offset, offset)) {
            return {
                .status = ByteRangePrefetchStatus::Error,
                .error = "subrange prefetch range is invalid",
            };
        }
        return m_source->PrefetchRange(m_offset + offset, byteSize);
    }

    bool ReadAt(
        const std::uint64_t offset,
        const std::span<std::uint8_t> output,
        std::string* error = nullptr) override {
        if (m_source == nullptr || offset > m_byteSize || output.size() > m_byteSize - offset) {
            return validation::AssignError(error, "subrange byte reader read is outside the selected range");
        }
        std::uint64_t sourceOffset = 0u;
        if (!validation::CheckedAddU64(m_offset, offset, sourceOffset, "subrange byte reader offset", error)) {
            return false;
        }
        return m_source->ReadAt(sourceOffset, output, error);
    }

private:
    std::shared_ptr<IByteRangeReader> m_source;
    std::uint64_t m_offset{0u};
    std::uint64_t m_byteSize{0u};
};

class MemoryByteRangeOutput final : public IByteRangeOutput {
public:
    explicit MemoryByteRangeOutput(DataCodecExecutionResources& run);
    MemoryByteRangeOutput(const MemoryByteRangeOutput&) = delete;
    MemoryByteRangeOutput& operator=(const MemoryByteRangeOutput&) = delete;
    bool PrepareExactSize(std::uint64_t size, std::string* error = nullptr,
                          std::span<const resource::StorageOwnerDescription> coexist = {});
    bool WriteAt(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error = nullptr) override;
    bool Finalize(std::uint64_t size, std::string* error = nullptr) override;
    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept;
    [[nodiscard]] EncodedBuffer TakeBytes() noexcept;

private:
    bool ZeroRange(std::uint64_t offset, std::uint64_t length, std::string* error);
    DataCodecExecutionResources& m_run;
    std::shared_ptr<bytestore::MemoryStore> m_store;
    std::optional<std::uint64_t> m_preparedSize;
    bool m_finalized{false};
    bool m_failed{false};
};

inline bool CopyByteRangeReaderToOutput(
    IByteRangeReader& source,
    IByteRangeOutput& sink,
    std::string* error = nullptr) {
    const auto byteSize = source.ByteSize();
    std::vector<std::uint8_t> buffer(kIoWindowBytes, 0u);
    std::uint64_t offset = 0u;
    while (offset < byteSize) {
        const auto currentBytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(byteSize - offset, static_cast<std::uint64_t>(kIoWindowBytes)));
        auto window = std::span<std::uint8_t>(buffer.data(), currentBytes);
        if (!source.ReadAt(offset, window, error) ||
            !sink.WriteAt(offset, std::span<const std::uint8_t>(window.data(), window.size()), error)) {
            return false;
        }
        offset += static_cast<std::uint64_t>(currentBytes);
    }
    return sink.Finalize(byteSize, error);
}

} // namespace datacodec

#endif
