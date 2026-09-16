#ifndef DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDGEOMETRYCACHE_H
#define DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDGEOMETRYCACHE_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Workflow/Decode/IDecodeAdapter.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedStorageSize.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
namespace datacodec {

struct DecodedGeometryCache {
    DecodedGeometryCache() = default;
    DecodedGeometryCache(const DecodedGeometryCache&) = delete;
    DecodedGeometryCache& operator=(const DecodedGeometryCache&) = delete;

    DecodedGeometryCache(DecodedGeometryCache&& other) noexcept {
        MoveFrom(std::move(other));
    }

    DecodedGeometryCache& operator=(DecodedGeometryCache&& other) noexcept {
        if (this != &other) {
            Release();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~DecodedGeometryCache() { Release(); }

    std::size_t pointCount{0u};
    std::size_t dimension{0u};
    DataType dataType{DataType::Float32};
    std::shared_ptr<bytestore::IRandomAccessByteStore> bytes;
    bool complete{false};
    std::weak_ptr<const void> nativeOutputIdentity;

    bool Initialize(
        const std::size_t count,
        const std::size_t pointDimension,
        const DataType type,
        bytestore::ByteStoreSession& byteStoreSession,
        std::string* error = nullptr,
        IDecodeAdapter* destination = nullptr) {
        Release();
        std::uint64_t byteCount = 0u;
        if (!CalculateGeometryCacheBytes(count, pointDimension, type, byteCount, error)) {
            return false;
        }
        if (destination && destination->SupportsGeometryDecodeStore(type)) {
            bytes = destination->CreateGeometryDecodeStore(count, pointDimension, error);
            nativeOutputIdentity = destination->DecodeStorageIdentity();
            if (nativeOutputIdentity.expired() || !bytes || bytes->ByteSizeHint() != byteCount) {
                return validation::AssignError(error, "native geometry store does not match output shape");
            }
        } else {
            bytes = byteStoreSession.CreateSizedStore(bytestore::ByteStorePurpose::Ranged,
                byteCount, MemoryDemandKind::RequiredContinuation, "decoded_geometry", error);
        }
        if (bytes == nullptr) {
            return false;
        }
        pointCount = count;
        dimension = pointDimension;
        dataType = type;
        return true;
    }

    void Release() noexcept {
        bytes.reset();
        nativeOutputIdentity.reset();
        pointCount = 0u;
        dimension = 0u;
        complete = false;
    }

private:
    void MoveFrom(DecodedGeometryCache&& other) noexcept {
        pointCount = other.pointCount;
        dimension = other.dimension;
        dataType = other.dataType;
        bytes = std::move(other.bytes);
        nativeOutputIdentity = std::move(other.nativeOutputIdentity);
        complete = other.complete;
        other.pointCount = 0u;
        other.dimension = 0u;
        other.complete = false;
    }
};

inline std::size_t DecodeGeometryTupleBytes(const GeometryStorageParams& meta) {
    std::size_t localValueSize = 0u;
    std::size_t tupleBytes = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize) ||
        !validation::CheckedMulSizeT(static_cast<std::size_t>(std::max(meta.dimension, 0)),
            localValueSize, tupleBytes, "geometry tuple bytes")) {
        return 0u;
    }
    return tupleBytes;
}

inline bool ValidateDecodedGeometryRange(
    const GeometryStorageParams& meta,
    const std::size_t offset,
    const std::size_t count,
    const std::size_t byteSize,
    std::string* error = nullptr) {
    const auto tupleBytes = DecodeGeometryTupleBytes(meta);
    if (tupleBytes == 0u) {
        if (count == 0u && byteSize == 0u) {
            return true;
        }
        return validation::AssignError(error, "decoded geometry tuple size is invalid");
    }
    std::size_t localElementCount = 0u;
    if (!TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
        return validation::AssignError(error, "decoded geometry metadata exceeds this platform size limit");
    }
    if (offset > localElementCount || count > localElementCount - offset) {
        return validation::AssignError(error, "decoded geometry range is out of bounds");
    }
    std::size_t expectedBytes = 0u;
    if (!validation::CheckedMulSizeT(
            count,
            tupleBytes,
            expectedBytes,
            "decoded geometry byte size",
            error)) {
        return false;
    }
    if (byteSize != expectedBytes) {
        return validation::AssignError(error, "decoded geometry byte size does not match range");
    }
    return true;
}

class DecodedGeometryReferenceCache final {
public:
    ~DecodedGeometryReferenceCache() { Reset(); }

    bool BeginGeometry(
        const GeometryStorageParams& storageParams,
        bytestore::ByteStoreSession& byteStoreSession,
        std::string* error = nullptr) {
        Reset();
        std::size_t localElementCount = 0u;
        if (!TryParamSizeToSizeT(storageParams.elementCount, localElementCount)) {
            return validation::AssignError(error, "decoded geometry reference metadata exceeds this platform size limit");
        }
        const auto tupleBytes = DecodeGeometryTupleBytes(storageParams);
        if (tupleBytes == 0u && localElementCount != 0u) {
            return validation::AssignError(error, "decoded geometry reference tuple size is invalid");
        }
        std::uint64_t totalBytes = 0u;
        if (!CalculateDecodedNumericStorageBytes(storageParams, totalBytes, error)) {
            return false;
        }
        auto bytes = byteStoreSession.CreateSizedStore(bytestore::ByteStorePurpose::Ranged,
            totalBytes, ::datacodec::MemoryDemandKind::RequiredContinuation, "decoded_geometry_reference", error);
        if (bytes == nullptr) {
            return false;
        }
        m_bytes = std::move(bytes);
        m_storageParams = storageParams;
        m_tupleBytes = tupleBytes;
        m_initialized = true;
        return true;
    }

    void Reset() noexcept {
        m_bytes.reset();
        m_storageParams = {};
        m_tupleBytes = 0u;
        m_initialized = false;
        m_complete = false;
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] bool IsComplete() const noexcept { return m_initialized && m_complete; }
    [[nodiscard]] const GeometryStorageParams& StorageParams() const noexcept { return m_storageParams; }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept {
        return m_bytes != nullptr ? m_bytes->ResidentSizeHint() : 0u;
    }

    bool WriteRange(
        const std::size_t offset,
        const std::size_t count,
        const void* data,
        const std::size_t byteSize,
        std::string* error = nullptr) {
        if (!m_initialized) {
            return validation::AssignError(error, "decoded geometry reference cache is not initialized");
        }
        if (!ValidateDecodedGeometryRange(m_storageParams, offset, count, byteSize, error)) {
            return false;
        }
        if (byteSize == 0u) {
            return true;
        }
        if (data == nullptr) {
            return validation::AssignError(error, "decoded geometry reference write source is null");
        }
        return m_bytes != nullptr &&
            m_bytes->WriteBytesAt(
                static_cast<std::uint64_t>(offset) * static_cast<std::uint64_t>(m_tupleBytes),
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), byteSize),
                error);
    }

    bool EndGeometry(std::string* error = nullptr) {
        if (!m_initialized || m_bytes == nullptr) {
            return validation::AssignError(
                error,
                "decoded geometry reference cache is not initialized");
        }
        if (!m_bytes->Seal(error)) {
            return false;
        }
        m_complete = true;
        return true;
    }

    bool ReadRange(
        const std::size_t offset,
        const std::size_t count,
        void* data,
        const std::size_t byteSize,
        std::string* error = nullptr) const {
        if (!IsComplete()) {
            return validation::AssignError(error, "decoded geometry reference cache is not complete");
        }
        if (!ValidateDecodedGeometryRange(m_storageParams, offset, count, byteSize, error)) {
            return false;
        }
        if (byteSize == 0u) {
            return true;
        }
        if (data == nullptr) {
            return validation::AssignError(error, "decoded geometry reference read target is null");
        }
        return m_bytes != nullptr &&
            m_bytes->Read(
                static_cast<std::uint64_t>(offset) * static_cast<std::uint64_t>(m_tupleBytes),
                std::span<std::uint8_t>(static_cast<std::uint8_t*>(data), byteSize),
                error);
    }

    [[nodiscard]] std::shared_ptr<bytestore::IRandomAccessByteStore> Bytes() const noexcept {
        return m_bytes;
    }

private:
    GeometryStorageParams m_storageParams;
    std::size_t m_tupleBytes{0u};
    std::shared_ptr<bytestore::IRandomAccessByteStore> m_bytes;
    bool m_initialized{false};
    bool m_complete{false};
};

} // namespace datacodec

#endif
