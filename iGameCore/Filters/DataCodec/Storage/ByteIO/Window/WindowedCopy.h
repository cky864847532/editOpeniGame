#ifndef DATACODEC_STORAGE_BYTEIO_WINDOW_WINDOWEDCOPY_H
#define DATACODEC_STORAGE_BYTEIO_WINDOW_WINDOWEDCOPY_H

#include "DataCodec/Storage/ByteIO/ByteSource.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
namespace datacodec {
namespace window {

class WindowedByteSourceReader final {
public:
    WindowedByteSourceReader(
        bytestore::IByteSource& source,
        ScratchByteBufferPool& scratchBytePool)
        : m_source(source),
          m_scratchBytePool(scratchBytePool) {}

    bool Next(std::span<const std::uint8_t>& bytes, bool& hasBytes, std::string* error = nullptr) {
        bytes = {};
        hasBytes = false;
        m_scratchBuffer.Release();

        const auto byteSize = m_source.ByteSizeHint();
        if (!m_source.CanRead() || bytestore::IsUnknownByteSize(byteSize)) {
            return validation::AssignError(
                error,
                "windowed byte source reader requires a ranged readable source with known size");
        }
        if (m_offset >= byteSize) {
            return true;
        }

        const auto remaining = byteSize - m_offset;
        const auto currentBytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(kIoWindowBytes)));
        m_scratchBuffer = m_scratchBytePool.Acquire(currentBytes);
        auto buffer = m_scratchBuffer.Span();
        if (!m_source.Read(m_offset, buffer, error)) {
            return false;
        }
        m_offset += currentBytes;
        bytes = std::span<const std::uint8_t>(buffer.data(), buffer.size());
        hasBytes = true;
        return true;
    }

private:
    bytestore::IByteSource& m_source;
    ScratchByteBufferPool& m_scratchBytePool;
    std::uint64_t m_offset{0u};
    ScratchByteBuffer m_scratchBuffer;
};

template<typename TConsume>
inline bool ForEachByteSourceWindow(
    bytestore::IByteSource& source,
    ScratchByteBufferPool& scratchBytePool,
    TConsume&& consume,
    std::string* error = nullptr) {
    WindowedByteSourceReader reader(
        source,
        scratchBytePool);
    for (;;) {
        std::span<const std::uint8_t> bytes;
        bool hasBytes = false;
        if (!reader.Next(bytes, hasBytes, error)) {
            return false;
        }
        if (!hasBytes) {
            break;
        }
        if (!consume(bytes)) {
            return false;
        }
    }
    return true;
}

inline bool CopyByteSourceByWindow(
    bytestore::IByteSource& source,
    bytestore::IByteWriter& writer,
    ScratchByteBufferPool& scratchBytePool,
    std::string* error = nullptr) {
    const auto byteSize = source.ByteSizeHint();
    if (!source.CanRead() || bytestore::IsUnknownByteSize(byteSize)) {
        return validation::AssignError(
            error,
            "windowed byte source copy requires a ranged readable source with known size");
    }
    if (source.PreferDirectCopy()) {
        return source.CopyTo(writer, error);
    }
    std::span<const std::uint8_t> contiguous;
    const auto contiguousStatus = source.PrepareContiguousBytes(contiguous, error);
    if (contiguousStatus == ContiguousViewStatus::Error) {
        return false;
    }
    if (contiguousStatus == ContiguousViewStatus::Ready) {
        if (static_cast<std::uint64_t>(contiguous.size()) != byteSize) {
            return validation::AssignError(
                error,
                "windowed byte source contiguous size does not match source size");
        }
        std::uint64_t copiedBytes = 0u;
        while (copiedBytes < byteSize) {
            const auto currentBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                byteSize - copiedBytes,
                static_cast<std::uint64_t>(kIoWindowBytes)));
            if (!writer.Write(
                    std::span<const std::uint8_t>(
                        contiguous.data() + static_cast<std::size_t>(copiedBytes),
                        currentBytes),
                    error)) {
                return false;
            }
            if (!validation::CheckedAddU64(
                    copiedBytes,
                    static_cast<std::uint64_t>(currentBytes),
                    copiedBytes,
                    "windowed byte source copied bytes",
                    error)) {
                return false;
            }
        }
        return true;
    }

    return ForEachByteSourceWindow(
        source,
        scratchBytePool,
        [&](const std::span<const std::uint8_t> bytes) {
            return writer.Write(bytes, error);
        },
        error);
}

inline bool CopyByteSourceRangeByWindow(
    bytestore::IByteSource& source,
    const std::uint64_t rangeOffset,
    const std::uint64_t rangeByteCount,
    bytestore::IByteWriter& writer,
    ScratchByteBufferPool& scratchBytePool,
    std::string* error = nullptr) {
    const auto byteSize = source.ByteSizeHint();
    if (!source.CanRead() || bytestore::IsUnknownByteSize(byteSize)) {
        return validation::AssignError(
            error,
            "windowed byte source range copy requires a ranged readable source with known size");
    }
    if (rangeOffset > byteSize || rangeByteCount > byteSize - rangeOffset) {
        return validation::AssignError(error, "windowed byte source range copy is outside the source range");
    }

    std::uint64_t copiedBytes = 0u;
    while (copiedBytes < rangeByteCount) {
        const auto remaining = rangeByteCount - copiedBytes;
        const auto currentBytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(kIoWindowBytes)));
        auto scratchBuffer = scratchBytePool.Acquire(currentBytes);
        auto buffer = scratchBuffer.Span();
        if (!source.Read(rangeOffset + copiedBytes, buffer, error) ||
            !writer.Write(std::span<const std::uint8_t>(buffer.data(), buffer.size()), error)) {
            return false;
        }
        if (!validation::CheckedAddU64(
                copiedBytes,
                static_cast<std::uint64_t>(currentBytes),
                copiedBytes,
                "windowed byte source range copied bytes",
                error)) {
            return false;
        }
    }
    return true;
}

} // namespace window
} // namespace datacodec

#endif
