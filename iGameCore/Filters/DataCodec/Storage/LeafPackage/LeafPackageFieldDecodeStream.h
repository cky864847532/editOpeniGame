#ifndef DATACODEC_STORAGE_LEAFPACKAGE_LEAFPACKAGEFIELDDECODESTREAM_H
#define DATACODEC_STORAGE_LEAFPACKAGE_LEAFPACKAGEFIELDDECODESTREAM_H

#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Storage/ByteIO/ByteSource.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DecodeStageMemory.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Codec/SubCodec/ZstdCodec.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Storage/LeafPackage/LeafPackage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
namespace decodefield {

struct FieldInputSegment {
    std::uint64_t encodedOffset{0};
    std::span<const std::uint8_t> bytes;
};

struct FieldOutputSegment {
    std::uint64_t rawOffset{0};
    std::span<const std::uint8_t> bytes;
};

class FieldDecodeStreamReader {
public:
    FieldDecodeStreamReader() = default;
    FieldDecodeStreamReader(const FieldDecodeStreamReader&) = delete;
    FieldDecodeStreamReader& operator=(const FieldDecodeStreamReader&) = delete;

    FieldDecodeStreamReader(FieldDecodeStreamReader&& other) noexcept { MoveFrom(std::move(other)); }
    FieldDecodeStreamReader& operator=(FieldDecodeStreamReader&& other) noexcept {
        if (this != &other) {
            Release();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~FieldDecodeStreamReader() { Release(); }

    bool Open(
        std::shared_ptr<bytestore::IByteSource> source,
        const std::uint64_t rawSize,
        const CacheResources& runtime,
        std::string* error = nullptr, bool controlled = true) {
        Release();
        if (source == nullptr) {
            validation::AssignError(error, "zstd decode segment reader received a null byte source");
            return false;
        }
        const auto sourceByteSize = source->ByteSizeHint();
        if (sourceByteSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            validation::AssignError(error, "zstd source bytes exceed local address space");
            return false;
        }
        if (rawSize > 0u && sourceByteSize == 0u) {
            validation::AssignError(error, "zstd source bytes is empty while raw size is non-zero");
            return false;
        }
        if (!source->CanRead()) {
            validation::AssignError(error, "zstd decode byte source must support ranged reads");
            return false;
        }
        m_source = std::move(source);
        m_sourceByteSize = static_cast<std::size_t>(sourceByteSize);
        m_rawSize = rawSize;
        m_stop = runtime.Run().StopToken();
        m_rawMode = false;
        if (m_rawSize == 0u && sourceByteSize == 0u) {
            m_finished = true;
            return true;
        }

        if (!PrepareWindows(runtime, controlled, error) || !m_zstdDecoder.Initialize(error)) {
            Release();
            return false;
        }
        return true;
    }

    bool OpenRaw(
        std::shared_ptr<bytestore::IByteSource> source,
        const std::uint64_t rawSize,
        const CacheResources& runtime,
        std::string* error = nullptr, bool controlled = true) {
        Release();
        if (source == nullptr) {
            validation::AssignError(error, "raw decode segment reader received a null byte source");
            return false;
        }
        const auto sourceByteSize = source->ByteSizeHint();
        if (sourceByteSize != rawSize) {
            validation::AssignError(error, "raw decode source size does not match recorded raw size");
            return false;
        }
        if (sourceByteSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            validation::AssignError(error, "raw decode source bytes exceed local address space");
            return false;
        }
        if (!source->CanRead()) {
            validation::AssignError(error, "raw decode byte source must support ranged reads");
            return false;
        }
        m_source = std::move(source);
        m_sourceByteSize = static_cast<std::size_t>(sourceByteSize);
        m_rawSize = rawSize;
        m_stop = runtime.Run().StopToken();
        m_rawMode = true;
        if (!PrepareWindows(runtime, controlled, error)) { Release(); return false; }
        if (m_rawSize == 0u) {
            m_finished = true;
        }
        return true;
    }

    bool ReadNext(
        FieldOutputSegment& segment,
        bool& hasSegment,
        std::string* error = nullptr) {
        segment = {};
        hasSegment = false;
        if (m_stop.stop_requested()) { return validation::AssignError(error, "field stream read cancelled"); }
        if (m_source == nullptr) {
            validation::AssignError(error, "zstd decode segment reader is not open");
            return false;
        }
        if (m_rawMode) {
            return ReadRawNext(segment, hasSegment, error);
        }
        if (m_finished) {
            return true;
        }

        while (!m_finished) {
            if (m_stop.stop_requested()) { return validation::AssignError(error, "field stream read cancelled"); }
            if (m_inputCursor == m_inputLimit && !FillInput(error)) {
                return false;
            }
            if (m_inputCursor == m_inputLimit && m_inputOffset >= m_sourceByteSize) {
                validation::AssignError(error, "zstd stream ended before the frame was complete");
                return false;
            }

            if (m_source == nullptr) {
                validation::AssignError(error, "zstd decode window resources are missing");
                return false;
            }
            auto outputBuffer = m_outputScratchBuffer.Span();

            std::size_t consumedBytes = 0u;
            std::size_t producedBytes = 0u;
            bool frameComplete = false;
            if (!m_zstdDecoder.Decompress(
                    std::span<const std::uint8_t>(
                        m_currentInputSegment.bytes.data() + m_inputCursor,
                        m_inputLimit - m_inputCursor),
                    outputBuffer,
                    consumedBytes,
                    producedBytes,
                    frameComplete,
                    error)) {
                return false;
            }
            m_inputCursor += consumedBytes;

            if (producedBytes > 0u) {
                // 用减法比较，避免累计输出尺寸溢出
                if (m_outputOffset > m_rawSize ||
                    producedBytes > m_rawSize - m_outputOffset) {
                    validation::AssignError(error, "zstd stream decompressed beyond recorded raw size");
                    return false;
                }
                segment.rawOffset = m_outputOffset;
                segment.bytes = std::span<const std::uint8_t>(outputBuffer.data(), producedBytes);
                m_outputOffset += producedBytes;
                if (frameComplete) {
                    if (!ValidateZstdFrameConsumed(error)) {
                        return false;
                    }
                    m_finished = true;
                    if (m_outputOffset != m_rawSize) {
                        validation::AssignError(error, "zstd stream raw size does not match decoded bytes");
                        return false;
                    }
                }
                hasSegment = true;
                return true;
            }

            if (frameComplete) {
                if (!ValidateZstdFrameConsumed(error)) {
                    return false;
                }
                m_finished = true;
                if (m_outputOffset != m_rawSize) {
                    validation::AssignError(error, "zstd stream raw size does not match decoded bytes");
                    return false;
                }
                return true;
            }

            if (m_inputCursor == m_inputLimit && m_inputOffset >= m_sourceByteSize) {
                validation::AssignError(error, "zstd stream ended before the frame was complete");
                return false;
            }
        }

        return true;
    }

private:
    bool PrepareWindows(const CacheResources& runtime, bool controlled, std::string* error) {
        const auto bytes = DecodeFieldWindowBytes(m_sourceByteSize, m_rawSize, !m_rawMode);
        const auto input = m_rawMode ? 0u : std::min<std::size_t>(m_sourceByteSize, kIoWindowBytes);
        std::span<std::uint8_t> memory;
        if (controlled) {
            if (!PrepareDecodeStageMemory(runtime.Run(), bytes, m_backing, error)) { return false; }
            memory = m_backing.Span();
        } else {
            // Params 属于明确豁免的有界元数据
            m_metadataWindow.resize(bytes);
            memory = m_metadataWindow;
        }
        m_inputScratchBuffer = FixedScratchBuffer(FixedArrayView<std::uint8_t>(memory.first(input)));
        m_outputScratchBuffer = FixedScratchBuffer(FixedArrayView<std::uint8_t>(memory.subspan(input)));
        return true;
    }

    bool ValidateZstdFrameConsumed(std::string* error) const {
        // field 内只接受一个完整 ZSTD frame，拒绝 frame 后的尾随字节
        if (m_inputCursor != m_inputLimit || m_inputOffset < m_sourceByteSize) {
            validation::AssignError(error, "zstd stream contains trailing encoded bytes");
            return false;
        }
        return true;
    }

    bool ReadRawNext(
        FieldOutputSegment& segment,
        bool& hasSegment,
        std::string* error) {
        if (m_finished) {
            return true;
        }
        if (m_source == nullptr) {
            validation::AssignError(error, "raw decode window resources are missing");
            return false;
        }
        const auto remainingBytes = m_rawSize - m_outputOffset;
        const auto currentBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            remainingBytes,
            static_cast<std::uint64_t>(kIoWindowBytes)));
        if (currentBytes == 0u) {
            m_finished = true;
            return true;
        }

        m_outputScratchBuffer.Bytes().resize(currentBytes);
        auto outputBuffer = m_outputScratchBuffer.Span();
        if (!m_source->ReadCancellable(m_outputOffset, outputBuffer, m_stop, error)) {
            return false;
        }
        segment.rawOffset = m_outputOffset;
        segment.bytes = std::span<const std::uint8_t>(outputBuffer.data(), currentBytes);
        m_outputOffset += currentBytes;
        if (m_outputOffset == m_rawSize) {
            m_finished = true;
        }
        hasSegment = true;
        return true;
    }

    bool FillInput(std::string* error) {
        if (m_inputOffset >= m_sourceByteSize) {
            m_currentInputSegment = {};
            m_inputCursor = 0u;
            m_inputLimit = 0u;
            return true;
        }

        const auto currentBytes = std::min<std::size_t>(
            kIoWindowBytes,
            m_sourceByteSize - m_inputOffset);
        if (currentBytes == 0u) {
            m_currentInputSegment = {};
            m_inputCursor = 0u;
            m_inputLimit = 0u;
            return true;
        }

        if (m_source == nullptr) {
            validation::AssignError(error, "zstd decode window resources are missing");
            return false;
        }
        m_inputScratchBuffer.Bytes().resize(currentBytes);
        auto inputBuffer = m_inputScratchBuffer.Span();
        if (!m_source->ReadCancellable(
                static_cast<std::uint64_t>(m_inputOffset),
                inputBuffer,
                m_stop,
                error)) {
            return false;
        }

        m_currentInputSegment.encodedOffset = static_cast<std::uint64_t>(m_inputOffset);
        m_currentInputSegment.bytes = std::span<const std::uint8_t>(inputBuffer.data(), currentBytes);
        m_inputOffset += currentBytes;
        m_inputCursor = 0u;
        m_inputLimit = currentBytes;
        return true;
    }

    void Release() noexcept {
        m_zstdDecoder.Release();
        m_source.reset();
        m_sourceByteSize = 0u;
        m_rawSize = 0u;
        m_inputOffset = 0u;
        m_inputCursor = 0u;
        m_inputLimit = 0u;
        m_currentInputSegment = {};
        m_outputOffset = 0u;
        m_finished = false;
        m_inputScratchBuffer.Release();
        m_outputScratchBuffer.Release();
        m_backing = {};
        std::vector<std::uint8_t>().swap(m_metadataWindow);
        m_stop = {};
        m_rawMode = false;
    }

    void MoveFrom(FieldDecodeStreamReader&& other) noexcept {
        m_source = std::move(other.m_source);
        m_stop = std::exchange(other.m_stop, {});
        m_zstdDecoder = std::move(other.m_zstdDecoder);
        m_inputScratchBuffer = std::move(other.m_inputScratchBuffer);
        m_outputScratchBuffer = std::move(other.m_outputScratchBuffer);
        const auto currentInputBytes = other.m_currentInputSegment.bytes.size();
        m_sourceByteSize = other.m_sourceByteSize;
        m_rawSize = other.m_rawSize;
        m_inputOffset = other.m_inputOffset;
        m_inputCursor = other.m_inputCursor;
        m_inputLimit = other.m_inputLimit;
        m_currentInputSegment = FieldInputSegment{
            other.m_currentInputSegment.encodedOffset,
            std::span<const std::uint8_t>(m_inputScratchBuffer.Bytes().data(), currentInputBytes),
        };
        m_outputOffset = other.m_outputOffset;
        m_finished = other.m_finished;
        m_backing = std::move(other.m_backing);
        m_metadataWindow = std::move(other.m_metadataWindow);
        m_rawMode = other.m_rawMode;
        other.m_source.reset();
        other.m_sourceByteSize = 0u;
        other.m_rawSize = 0u;
        other.m_inputOffset = 0u;
        other.m_inputCursor = 0u;
        other.m_inputLimit = 0u;
        other.m_currentInputSegment = {};
        other.m_outputOffset = 0u;
        other.m_finished = false;
        other.m_rawMode = false;
    }

    std::shared_ptr<bytestore::IByteSource> m_source;
    codec::ZstdStreamingDecoder m_zstdDecoder;
    FixedByteBacking m_backing;
    std::vector<std::uint8_t> m_metadataWindow;
    FixedScratchBuffer m_inputScratchBuffer;
    FixedScratchBuffer m_outputScratchBuffer;
    std::size_t m_sourceByteSize{0u};
    std::uint64_t m_rawSize{0u};
    std::size_t m_inputOffset{0};
    std::size_t m_inputCursor{0u};
    std::size_t m_inputLimit{0u};
    FieldInputSegment m_currentInputSegment;
    std::uint64_t m_outputOffset{0};
    bool m_finished{false};
    bool m_rawMode{false};
    std::stop_token m_stop;
};

inline bool OpenLeafPackageFieldDecodeStream(
    const LeafPackageField& field,
    const CacheResources& runtime,
    FieldDecodeStreamReader& reader,
    std::string* error = nullptr) {
    if (field.source == nullptr) {
        validation::AssignError(error, "leaf package field is missing its byte source");
        return false;
    }
    if (field.compressionType == EncodedFieldCompressionType::ZSTD) {
        return reader.Open(field.source, field.rawSize, runtime, error, field.type != FieldType::Params);
    }
    if (field.compressionType == EncodedFieldCompressionType::None) {
        return reader.OpenRaw(field.source, field.rawSize, runtime, error, field.type != FieldType::Params);
    }
    validation::AssignError(error, "leaf package field uses an unsupported compression type");
    return false;
}

inline bool PrepareLeafPackageFieldPayload(
    const LeafPackageField& field, const CacheResources& runtime, bytestore::ByteStoreSession& session,
    std::shared_ptr<bytestore::IByteSource>& payload, std::string* error = nullptr) {
    payload.reset();
    auto& root = runtime.Run();
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return false; }
    if (!field.source || !field.source->CanRead()) {
        return validation::AssignError(error, "field payload requires a readable source");
    }
    if (field.compressionType == EncodedFieldCompressionType::None) {
        if (field.source->ByteSizeHint() != field.rawSize) {
            return validation::AssignError(error, "raw field payload size does not match metadata");
        }
        payload = field.source;
        return true;
    }
    if (field.compressionType != EncodedFieldCompressionType::ZSTD) {
        return validation::AssignError(error, "field payload compression type is unsupported");
    }
    auto store = session.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, field.rawSize, ::datacodec::MemoryDemandKind::RequiredContinuation,
        "prepared_field_payload", error);
    if (!store) { return false; }
    FieldDecodeStreamReader reader;
    if (!OpenLeafPackageFieldDecodeStream(field, runtime, reader, error)) { return false; }
    const bool success = RunTerminalWork(root, *phase, [&](WorkerContext& worker) {
        std::uint64_t copied = 0u;
        for (;;) {
            if (worker.StopToken().stop_requested()) { return false; }
            FieldOutputSegment segment;
            bool hasSegment = false;
            if (!reader.ReadNext(segment, hasSegment, error)) { return false; }
            if (!hasSegment) { break; }
            if (segment.rawOffset != copied || copied > field.rawSize ||
                segment.bytes.size() > field.rawSize - copied) {
                return validation::AssignError(error, "field payload segment has an invalid range");
            }
            for (std::size_t offset = 0u; offset < segment.bytes.size();) {
                const auto count = std::min(kIoWindowBytes, segment.bytes.size() - offset);
                if (!store->WriteBytesAt(copied + offset, segment.bytes.subspan(offset, count), error)) { return false; }
                offset += count;
            }
            copied += segment.bytes.size();
        }
        if (copied != field.rawSize) {
            return validation::AssignError(error, "field decoded payload does not match the full recorded size");
        }
        return store->Seal(error);
    });
    if (!success) { return false; }
    payload = std::move(store);
    return true;
}

class FieldDecodeByteStream {
public:
    explicit FieldDecodeByteStream(FieldDecodeStreamReader& reader) : m_reader(reader) {}

    [[nodiscard]] std::uint64_t Position() const noexcept { return m_position; }

    bool ReadBytes(void* target, const std::size_t byteCount, std::string* error = nullptr) {
        auto* output = static_cast<std::uint8_t*>(target);
        std::size_t copied = 0u;
        while (copied < byteCount) {
            if (!EnsureSegment(error)) {
                return false;
            }
            if (m_available.empty()) {
                validation::AssignError(error, "unexpected end of decoded field stream");
                return false;
            }

            const auto currentBytes = std::min(byteCount - copied, m_available.size());
            std::memcpy(output + copied, m_available.data(), currentBytes);
            m_available = m_available.subspan(currentBytes);
            copied += currentBytes;
            m_position += currentBytes;
        }
        return true;
    }

    bool ReadVector(std::vector<std::uint8_t>& output, const std::size_t byteCount, std::string* error = nullptr) {
        output.assign(byteCount, 0u);
        if (byteCount == 0u) {
            return true;
        }
        return ReadBytes(output.data(), output.size(), error);
    }

    template<typename T>
    bool ReadScalar(T& value, std::string* error = nullptr) {
        value = {};
        return ReadBytes(&value, sizeof(T), error);
    }

    bool Skip(const std::uint64_t byteCount, std::string* error = nullptr) {
        std::uint64_t skipped = 0u;
        while (skipped < byteCount) {
            if (!EnsureSegment(error)) { return false; }
            if (m_available.empty()) { return validation::AssignError(error, "unexpected end while skipping field"); }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(byteCount - skipped, m_available.size()));
            m_available = m_available.subspan(count);
            skipped += count;
            m_position += count;
        }
        return true;
    }

private:
    bool EnsureSegment(std::string* error) {
        while (m_available.empty() && !m_eof) {
            FieldOutputSegment segment;
            bool hasSegment = false;
            if (!m_reader.ReadNext(segment, hasSegment, error)) {
                return false;
            }
            if (!hasSegment) {
                m_eof = true;
                return true;
            }
            m_available = segment.bytes;
        }
        return true;
    }

    FieldDecodeStreamReader& m_reader;
    std::span<const std::uint8_t> m_available;
    std::uint64_t m_position{0};
    bool m_eof{false};
};

} // namespace decodefield
} // namespace datacodec

#endif
