#ifndef DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDATTRIBUTECACHESET_H
#define DATACODEC_RUNTIME_CACHE_DECODECACHE_DECODEDATTRIBUTECACHESET_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedStorageSize.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {

inline std::size_t DecodeAttributeTupleBytes(const AttrStorageParams& meta) {
    std::size_t localValueSize = 0u;
    std::size_t tupleBytes = 0u;
    if (!TryParamSizeToSizeT(NumericArrayValueSize(meta), localValueSize) ||
        !validation::CheckedMulSizeT(static_cast<std::size_t>(std::max(meta.dimension, 0)),
            localValueSize, tupleBytes, "attribute tuple bytes")) {
        return 0u;
    }
    return tupleBytes;
}

inline bool ValidateDecodedAttributeRange(
    const AttrStorageParams& meta,
    const std::size_t offset,
    const std::size_t count,
    const std::size_t byteSize,
    std::string* error = nullptr) {
    const auto tupleBytes = DecodeAttributeTupleBytes(meta);
    if (tupleBytes == 0u) {
        if (count == 0u && byteSize == 0u) {
            return true;
        }
        return validation::AssignError(error, "decoded attribute tuple size is invalid");
    }
    std::size_t localElementCount = 0u;
    if (!TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
        return validation::AssignError(error, "decoded attribute metadata exceeds this platform size limit");
    }
    if (offset > localElementCount || count > localElementCount - offset) {
        return validation::AssignError(error, "decoded attribute range is out of bounds");
    }
    std::size_t expectedBytes = 0u;
    if (!validation::CheckedMulSizeT(
            count,
            tupleBytes,
            expectedBytes,
            "decoded attribute byte size",
            error)) {
        return false;
    }
    if (byteSize != expectedBytes) {
        return validation::AssignError(error, "decoded attribute byte size does not match range");
    }
    return true;
}

class DecodedAttributeCacheSet final {
public:
    DecodedAttributeCacheSet() = default;
    DecodedAttributeCacheSet(const DecodedAttributeCacheSet&) = delete;
    DecodedAttributeCacheSet& operator=(const DecodedAttributeCacheSet&) = delete;

    DecodedAttributeCacheSet(DecodedAttributeCacheSet&& other) noexcept {
        MoveFrom(std::move(other));
    }

    DecodedAttributeCacheSet& operator=(DecodedAttributeCacheSet&& other) noexcept {
        if (this != &other) {
            Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~DecodedAttributeCacheSet() { Reset(); }

    bool Initialize(
        const CodecStorageParams& storageParams,
        bytestore::ByteStoreSession& byteStoreSession,
        std::string* error = nullptr) {
        if (m_initialized || !m_fields.empty()) {
            Reset();
        }
        m_storageParams = storageParams;
        m_byteStoreSession = &byteStoreSession;
        m_fields.clear();
        m_fields.resize(storageParams.attrParams.size());
        for (std::size_t index = 0; index < storageParams.attrParams.size(); ++index) {
            auto& field = m_fields[index];
            field.meta = storageParams.attrParams[index];
            std::size_t localElementCount = 0u;
            std::size_t localValueSize = 0u;
            if (!TryParamSizeToSizeT(field.meta.elementCount, localElementCount) ||
                !TryParamSizeToSizeT(NumericArrayValueSize(field.meta), localValueSize)) {
                validation::AssignError(error, "decoded attribute metadata exceeds this platform size limit");
                Reset();
                return false;
            }
            if (!validation::CheckedMulSizeT(static_cast<std::size_t>(std::max(field.meta.dimension, 0)),
                    localValueSize, field.tupleBytes, "decoded attribute tuple bytes", error)) {
                Reset();
                return false;
            }
        }

        m_initialized = true;
        return true;
    }

    void Reset() noexcept {
        m_fields.clear();
        m_storageParams = {};
        m_byteStoreSession = nullptr;
        m_initialized = false;
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] bool IsComplete() const noexcept {
        return m_initialized &&
            std::all_of(m_fields.begin(), m_fields.end(), [](const Field& field) { return field.complete; });
    }
    [[nodiscard]] const CodecStorageParams& StorageParams() const noexcept { return m_storageParams; }
    [[nodiscard]] std::size_t FieldCount() const noexcept { return m_fields.size(); }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept {
        std::uint64_t bytes = 0u;
        for (const auto& field : m_fields) {
            if (field.bytes != nullptr) {
                bytes = validation::SaturatingAddU64(bytes, field.bytes->ResidentSizeHint());
            }
        }
        return bytes;
    }
    [[nodiscard]] std::uint64_t AdapterBackedResidentSizeHint() const noexcept {
        std::uint64_t bytes = 0u;
        for (const auto& field : m_fields) {
            if (field.adapterBacked && field.bytes != nullptr) {
                bytes = validation::SaturatingAddU64(bytes, field.bytes->ResidentSizeHint());
            }
        }
        return bytes;
    }
    bool BindAttributeStore(
        const std::size_t attrIndex,
        std::shared_ptr<bytestore::IRandomAccessByteStore> bytes,
        const bool adapterBacked,
        std::string* error = nullptr) {
        if (!ValidateAttrIndex(attrIndex, error)) {
            return false;
        }
        if (bytes == nullptr) {
            return validation::AssignError(error, "decoded attribute cache received a null byte store");
        }
        auto& field = m_fields[attrIndex];
        if (field.bytes != nullptr && field.bytes != bytes) {
            return validation::AssignError(error, "decoded attribute cache field already has a byte store");
        }
        field.bytes = std::move(bytes);
        field.adapterBacked = adapterBacked;
        field.complete = false;
        return true;
    }

    [[nodiscard]] bool AdapterBacked(const std::size_t attrIndex) const noexcept {
        return attrIndex < m_fields.size() && m_fields[attrIndex].adapterBacked;
    }

    bool BeginAttribute(
        const std::size_t attrIndex,
        const AttrStorageParams& meta,
        std::string* error = nullptr) {
        if (!ValidateAttrIndex(attrIndex, error)) {
            return false;
        }
        auto& field = m_fields[attrIndex];
        std::size_t localElementCount = 0u;
        if (!TryParamSizeToSizeT(meta.elementCount, localElementCount)) {
            return validation::AssignError(error, "decoded attribute metadata exceeds this platform size limit");
        }
        const auto tupleBytes = DecodeAttributeTupleBytes(meta);
        if (tupleBytes == 0u && localElementCount != 0u) {
            return validation::AssignError(error, "decoded attribute tuple size is invalid");
        }
        std::uint64_t totalBytes = 0u;
        if (!CalculateDecodedNumericStorageBytes(meta, totalBytes, error)) {
            return false;
        }
        if (field.bytes == nullptr) {
            if (m_byteStoreSession == nullptr) {
                return validation::AssignError(error, "decoded attribute cache has no byte store session");
            }
            field.bytes = m_byteStoreSession->CreateSizedStore(bytestore::ByteStorePurpose::Ranged,
                totalBytes, ::datacodec::MemoryDemandKind::RequiredContinuation, "decoded_attribute_" + std::to_string(attrIndex), error);
            if (field.bytes == nullptr) {
                return false;
            }
        } else if (field.adapterBacked) {
            // 宿主存储保持自身的分配协议，不进入 DataCodec 容量账本
            if (!field.bytes->ResizeBytes(totalBytes, error)) {
                return false;
            }
        } else if (field.bytes->ByteSizeHint() != totalBytes) {
            return validation::AssignError(error, "decoded attribute size changed while its owner is retained");
        }
        field.meta = meta;
        field.tupleBytes = tupleBytes;
        field.complete = false;
        return true;
    }

    bool WriteAttributeRange(
        const std::size_t attrIndex,
        const std::size_t offset,
        const std::size_t count,
        const void* data,
        const std::size_t byteSize,
        std::string* error = nullptr) {
        if (!ValidateAttrIndex(attrIndex, error)) {
            return false;
        }
        auto& field = m_fields[attrIndex];
        if (!ValidateDecodedAttributeRange(field.meta, offset, count, byteSize, error)) {
            return false;
        }
        if (byteSize == 0u) {
            return true;
        }
        if (data == nullptr) {
            return validation::AssignError(error, "decoded attribute write source is null");
        }
        return field.bytes != nullptr &&
            field.bytes->WriteBytesAt(
                static_cast<std::uint64_t>(offset) * static_cast<std::uint64_t>(field.tupleBytes),
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), byteSize),
                error);
    }

    bool EndAttribute(const std::size_t attrIndex, std::string* error = nullptr) {
        if (!ValidateAttrIndex(attrIndex, error)) {
            return false;
        }
        auto& field = m_fields[attrIndex];
        if (field.bytes == nullptr || !field.bytes->Seal(error)) {
            return false;
        }
        field.complete = true;
        return true;
    }

    void ReleaseFieldBytes(const std::size_t attrIndex) noexcept {
        if (attrIndex >= m_fields.size()) {
            return;
        }
        auto& field = m_fields[attrIndex];
        field.bytes.reset();
        field.adapterBacked = false;
        field.complete = false;
    }

    bool ReadRange(
        const std::size_t attrIndex,
        const std::size_t offset,
        const std::size_t count,
        void* data,
        const std::size_t byteSize,
        std::string* error = nullptr) const {
        if (!ValidateCompleteAttrIndex(attrIndex, error)) {
            return false;
        }
        const auto& field = m_fields[attrIndex];
        if (!ValidateDecodedAttributeRange(field.meta, offset, count, byteSize, error)) {
            return false;
        }
        if (byteSize == 0u) {
            return true;
        }
        if (data == nullptr) {
            return validation::AssignError(error, "decoded attribute read target is null");
        }
        return field.bytes != nullptr &&
            field.bytes->Read(
                static_cast<std::uint64_t>(offset) * static_cast<std::uint64_t>(field.tupleBytes),
                std::span<std::uint8_t>(static_cast<std::uint8_t*>(data), byteSize),
                error);
    }

    [[nodiscard]] const AttrStorageParams* Meta(const std::size_t attrIndex) const noexcept {
        return attrIndex < m_fields.size() ? &m_fields[attrIndex].meta : nullptr;
    }

    [[nodiscard]] std::shared_ptr<bytestore::IRandomAccessByteStore> Bytes(
        const std::size_t attrIndex) const noexcept {
        return attrIndex < m_fields.size() ? m_fields[attrIndex].bytes : nullptr;
    }

    [[nodiscard]] bool Complete(const std::size_t attrIndex) const noexcept {
        return attrIndex < m_fields.size() && m_fields[attrIndex].complete;
    }

private:
    struct Field {
        AttrStorageParams meta;
        std::size_t tupleBytes{0u};
        std::shared_ptr<bytestore::IRandomAccessByteStore> bytes;
        bool adapterBacked{false};
        bool complete{false};
    };

    bool ValidateAttrIndex(const std::size_t attrIndex, std::string* error) const {
        if (!m_initialized || attrIndex >= m_fields.size()) {
            return validation::AssignError(error, "decoded attribute cache index is out of range");
        }
        return true;
    }

    bool ValidateCompleteAttrIndex(const std::size_t attrIndex, std::string* error) const {
        if (!ValidateAttrIndex(attrIndex, error)) {
            return false;
        }
        if (!m_fields[attrIndex].complete) {
            return validation::AssignError(error, "decoded attribute cache field is not complete");
        }
        return true;
    }

    void MoveFrom(DecodedAttributeCacheSet&& other) noexcept {
        m_storageParams = std::move(other.m_storageParams);
        m_fields = std::move(other.m_fields);
        m_byteStoreSession = other.m_byteStoreSession;
        m_initialized = other.m_initialized;
        other.m_storageParams = {};
        other.m_byteStoreSession = nullptr;
        other.m_fields.clear();
        other.m_initialized = false;
    }

    CodecStorageParams m_storageParams;
    std::vector<Field> m_fields;
    bytestore::ByteStoreSession* m_byteStoreSession{nullptr};
    bool m_initialized{false};
};

} // namespace datacodec

#endif
