#ifndef DATACODEC_CODEC_REMAP_REMAPPROVIDER_H
#define DATACODEC_CODEC_REMAP_REMAPPROVIDER_H

#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace datacodec {

class IRemapProvider {
public:
    virtual ~IRemapProvider() = default;
    [[nodiscard]] virtual std::size_t Size() const noexcept = 0;
    [[nodiscard]] virtual bool IsIdentity() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t ResidentSizeHint() const noexcept = 0;
    virtual bool ReadAt(std::size_t index, IndexType& value, std::string* error) const = 0;
    virtual bool ReadRange(std::uint64_t offset, std::span<IndexType> output,
                           std::string* error = nullptr) const = 0;
    virtual bool ReadRange(
        std::uint64_t offset,
        std::uint64_t count,
        std::vector<IndexType>& output,
        std::string* error) const = 0;
};

class IWritableRemapProvider : public IRemapProvider {
public:
    virtual bool AppendRange(std::span<const IndexType> order, std::string* error = nullptr) = 0;
    virtual bool WriteAt(std::size_t index, IndexType value, std::string* error = nullptr) = 0;
    virtual bool EndWrite(std::string* error = nullptr) = 0;
    virtual bool EndRandomWrite(std::string* error = nullptr) = 0;
};

using WritableRemapProviderFactory = std::function<std::shared_ptr<IWritableRemapProvider>(
    std::size_t size,
    bool randomWrite,
    std::string_view label,
    std::string* error,
    std::span<const resource::StorageOwnerDescription> coexist)>;

class IdentityRemapProvider final : public IRemapProvider {
public:
    explicit IdentityRemapProvider(const std::size_t size)
        : m_size(size) {}

    [[nodiscard]] std::size_t Size() const noexcept override { return m_size; }
    [[nodiscard]] bool IsIdentity() const noexcept override { return true; }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override { return 0u; }

    bool ReadAt(const std::size_t index, IndexType& value, std::string* error) const override {
        if (index >= m_size) {
            return validation::AssignError(error, "remap read is out of range");
        }
        value = static_cast<IndexType>(index);
        return true;
    }

    bool ReadRange(const std::uint64_t offset, const std::span<IndexType> output,
                   std::string* error = nullptr) const override {
        if (offset > m_size || output.size() > m_size - offset) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        for (std::size_t i = 0u; i < output.size(); ++i) { output[i] = static_cast<IndexType>(offset + i); }
        return true;
    }

    bool ReadRange(
        const std::uint64_t offset,
        const std::uint64_t count,
        std::vector<IndexType>& output,
        std::string* error) const override {
        output.clear();
        if (offset > m_size || count > m_size - offset) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        output.resize(static_cast<std::size_t>(count));
        for (std::size_t local = 0; local < output.size(); ++local) {
            output[local] = static_cast<IndexType>(offset + local);
        }
        return true;
    }

private:
    std::size_t m_size{0};
};

class VectorRemapProvider final : public IRemapProvider {
public:
    explicit VectorRemapProvider(std::vector<IndexType> order)
        : m_order(std::move(order)) {}

    [[nodiscard]] std::size_t Size() const noexcept override { return m_order.size(); }
    [[nodiscard]] bool IsIdentity() const noexcept override { return m_order.empty(); }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return VectorCapacityBytes(m_order);
    }

    bool ReadAt(const std::size_t index, IndexType& value, std::string* error) const override {
        if (m_order.empty()) {
            value = static_cast<IndexType>(index);
            return true;
        }
        if (index >= m_order.size()) {
            return validation::AssignError(error, "remap read is out of range");
        }
        value = m_order[index];
        return true;
    }

    bool ReadRange(const std::uint64_t offset, const std::span<IndexType> output,
                   std::string* error = nullptr) const override {
        if (m_order.empty()) {
            if (!validation::CanAddU64(offset, output.size())) {
                return validation::AssignError(error, "remap identity range overflows");
            }
            for (std::size_t i = 0u; i < output.size(); ++i) { output[i] = static_cast<IndexType>(offset + i); }
            return true;
        }
        if (offset > m_order.size() || output.size() > m_order.size() - offset) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        std::copy_n(m_order.begin() + static_cast<std::ptrdiff_t>(offset), output.size(), output.begin());
        return true;
    }

    bool ReadRange(
        const std::uint64_t offset,
        const std::uint64_t count,
        std::vector<IndexType>& output,
        std::string* error) const override {
        output.clear();
        if (m_order.empty()) {
            output.resize(static_cast<std::size_t>(count));
            for (std::size_t local = 0; local < output.size(); ++local) {
                output[local] = static_cast<IndexType>(offset + local);
            }
            return true;
        }
        if (offset > m_order.size() || count > m_order.size() - offset) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        const auto begin = m_order.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto end = begin + static_cast<std::ptrdiff_t>(count);
        output.assign(begin, end);
        return true;
    }

private:
    std::vector<IndexType> m_order;
};

class RemapStoreProvider final : public IWritableRemapProvider {
public:
    RemapStoreProvider(
        const std::size_t size,
        std::shared_ptr<bytestore::IRandomAccessByteStore> source,
        const bool randomWrite)
        : m_source(std::move(source)), m_size(size),
          m_writing(!randomWrite), m_randomWriting(randomWrite) {}

    bool AppendRange(std::span<const IndexType> order, std::string* error = nullptr) override {
        if (!m_writing || m_source == nullptr) {
            return validation::AssignError(error, "remap provider writer is not open");
        }
        if (order.size() > m_size || m_writtenSize > m_size - order.size()) {
            return validation::AssignError(error, "remap provider write exceeds expected size");
        }
        if (!m_source->WriteBytesAt(static_cast<std::uint64_t>(m_writtenSize) * sizeof(IndexType),
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(order.data()),
                    order.size() * sizeof(IndexType)), error)) {
            return false;
        }
        m_writtenSize += order.size();
        return true;
    }

    bool WriteAt(
        const std::size_t index,
        const IndexType value,
        std::string* error = nullptr) override {
        if (!m_randomWriting || m_source == nullptr) {
            return validation::AssignError(error, "remap provider random writer is not open");
        }
        if (index >= m_size) {
            return validation::AssignError(error, "remap provider random write is out of range");
        }
        const auto byteOffset = static_cast<std::uint64_t>(index) * sizeof(IndexType);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        return m_source->WriteBytesAt(
            byteOffset,
            std::span<const std::uint8_t>(bytes, sizeof(IndexType)),
            error);
    }

    bool EndRandomWrite(std::string* error = nullptr) override {
        if (!m_randomWriting) {
            return validation::AssignError(error, "remap provider random writer is not open");
        }
        if (m_source == nullptr || !m_source->Seal(error)) {
            return false;
        }
        m_randomWriting = false;
        m_complete = true;
        return true;
    }

    bool EndWrite(std::string* error = nullptr) override {
        if (!m_writing) {
            return validation::AssignError(error, "remap provider writer is not open");
        }
        if (m_writtenSize != m_size) {
            return validation::AssignError(error, "remap provider written size mismatch");
        }
        if (m_source == nullptr || !m_source->Seal(error)) {
            return false;
        }
        m_writing = false;
        m_complete = true;
        return true;
    }

    [[nodiscard]] std::size_t Size() const noexcept override { return m_size; }
    [[nodiscard]] bool IsIdentity() const noexcept override { return false; }
    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return m_source != nullptr ? m_source->ResidentSizeHint() : 0u;
    }

    bool ReadAt(const std::size_t index, IndexType& value, std::string* error) const override {
        if (!m_complete) {
            return validation::AssignError(error, "remap provider is not complete");
        }
        if (index >= m_size) {
            return validation::AssignError(error, "remap read is out of range");
        }
        if (m_source == nullptr) {
            return validation::AssignError(error, "remap provider source is missing");
        }
        auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
        return m_source->Read(
            static_cast<std::uint64_t>(index) * sizeof(IndexType),
            std::span<std::uint8_t>(bytes, sizeof(IndexType)),
            error);
    }

    bool ReadRange(const std::uint64_t offset, const std::span<IndexType> output,
                   std::string* error = nullptr) const override {
        if (!m_complete || m_source == nullptr) {
            return validation::AssignError(error, "remap provider is not complete");
        }
        if (offset > m_size || output.size() > m_size - offset ||
            !validation::CanMulU64(offset, sizeof(IndexType))) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        std::size_t bytes = 0u;
        if (!validation::CheckedMulSizeT(output.size(), sizeof(IndexType), bytes, "remap range", error)) {
            return false;
        }
        return m_source->Read(offset * sizeof(IndexType),
            std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(output.data()), bytes), error);
    }

    bool ReadRange(
        const std::uint64_t offset,
        const std::uint64_t count,
        std::vector<IndexType>& output,
        std::string* error) const override {
        output.clear();
        if (!m_complete) {
            return validation::AssignError(error, "remap provider is not complete");
        }
        if (offset > m_size || count > m_size - offset) {
            return validation::AssignError(error, "remap range is out of bounds");
        }
        output.resize(static_cast<std::size_t>(count));
        if (count == 0u) {
            return true;
        }

        if (m_source == nullptr) {
            return validation::AssignError(error, "remap provider source is missing");
        }
        if (!validation::CanMulU64(offset, sizeof(IndexType)) ||
            !validation::CanMulU64(count, sizeof(IndexType))) {
            validation::AssignError(error, "remap provider range exceeds addressable byte size");
            return false;
        }
        const auto byteOffset = offset * sizeof(IndexType);
        const auto byteCount = count * sizeof(IndexType);
        std::size_t localByteCount = 0u;
        if (!validation::CheckedCastSizeT(byteCount, localByteCount, "remap provider range", error)) {
            return false;
        }
        return m_source->Read(
            byteOffset,
            std::span<std::uint8_t>(
                reinterpret_cast<std::uint8_t*>(output.data()),
                localByteCount),
            error);
    }

    [[nodiscard]] const bytestore::IByteSource* ByteSource() const noexcept { return m_source.get(); }

private:
    std::shared_ptr<bytestore::IRandomAccessByteStore> m_source;
    std::size_t m_writtenSize{0};
    std::size_t m_size{0};
    bool m_writing{false};
    bool m_randomWriting{false};
    bool m_complete{false};
};

inline std::shared_ptr<IWritableRemapProvider> MakeStoreBackedWritableRemapProvider(
    const std::size_t size,
    bytestore::ByteStoreSession& session,
    const bool randomWrite,
    const std::string& label,
    std::string* error = nullptr,
    std::span<const resource::StorageOwnerDescription> coexist = {}) {
    std::size_t bytes = 0u;
    if (!validation::CheckedMulSizeT(size, sizeof(IndexType), bytes, "remap provider bytes", error)) {
        return nullptr;
    }
    auto store = session.CreateSizedStore(bytestore::ByteStorePurpose::Ranged, bytes, ::datacodec::MemoryDemandKind::RequiredContinuation, label, error, coexist);
    if (store == nullptr) {
        return nullptr;
    }
    return std::make_shared<RemapStoreProvider>(size, std::move(store), randomWrite);
}

inline WritableRemapProviderFactory MakeStoreBackedWritableRemapProviderFactory(
    bytestore::ByteStoreSession& session,
    std::string prefix) {
    return [&session, prefix = std::move(prefix)](
        const std::size_t size,
        const bool randomWrite,
        const std::string_view label,
        std::string* error,
        std::span<const resource::StorageOwnerDescription> coexist) {
        std::string storeLabel = prefix;
        if (!storeLabel.empty() && !label.empty()) {
            storeLabel.push_back('_');
        }
        storeLabel.append(label.data(), label.size());
        return MakeStoreBackedWritableRemapProvider(
            size,
            session,
            randomWrite,
            storeLabel.empty() ? std::string("remap") : storeLabel,
            error, coexist);
    };
}

inline std::shared_ptr<IRemapProvider> MakeIdentityRemapProvider(const std::size_t size) {
    return std::make_shared<IdentityRemapProvider>(size);
}

inline std::shared_ptr<IRemapProvider> MakeVectorRemapProvider(std::vector<IndexType> order) {
    return std::make_shared<VectorRemapProvider>(std::move(order));
}

inline bool ReadRemapValue(
    const IRemapProvider* provider,
    const std::size_t index,
    IndexType& value,
    std::string* error = nullptr) {
    if (provider == nullptr || provider->IsIdentity()) {
        value = static_cast<IndexType>(index);
        return true;
    }
    return provider->ReadAt(index, value, error);
}

inline std::size_t RemapSizeOrZero(const IRemapProvider* provider) noexcept {
    return provider == nullptr || provider->IsIdentity() ? 0u : provider->Size();
}

inline std::uint64_t RemapResidentBytes(const std::shared_ptr<IRemapProvider>& provider) noexcept {
    return provider == nullptr ? 0u : provider->ResidentSizeHint();
}

} // namespace datacodec

#endif
