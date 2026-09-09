#ifndef DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_COMMON_TOPOLOGYTRANSFERCACHE_H
#define DATACODEC_RUNTIME_CACHE_TRANSFERCACHE_COMMON_TOPOLOGYTRANSFERCACHE_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Storage/ByteStore/SegmentedBinaryObject.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyTypes.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamEncoder.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamFormat.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace datacodec {
namespace topology {


class PolyhedronTopologyStreamSpooler final : public polyhedron::IPolyhedronTopologyStreamWriter {
public:
    explicit PolyhedronTopologyStreamSpooler(
        std::array<std::shared_ptr<bytestore::IByteWriter>, polyhedron::kStatefulPolyhedronTopologyStreamOrder.size()> streamWriters)
        : m_streams{} {
        for (std::size_t index = 0; index < polyhedron::kStatefulPolyhedronTopologyStreamOrder.size(); ++index) {
            m_streams[index].schedule.kind = polyhedron::kStatefulPolyhedronTopologyStreamOrder[index];
            m_streams[index].writer = std::move(streamWriters[index]);
        }
    }

    bool WriteStreamBytes(
        const polyhedron::PolyhedronTopologyStreamKind kind,
        const std::span<const std::uint8_t> bytes,
        std::string* error) override {
        const auto* stream = MutableStream(kind, error);
        if (stream == nullptr) {
            return false;
        }
        if (stream->completed) {
            return validation::AssignError(error, "polyhedron topology stream was already completed");
        }
        if (stream->writer == nullptr) {
            return validation::AssignError(error, "polyhedron topology stream writer is missing");
        }
        return stream->writer->Write(bytes, error);
    }

    bool CompleteStream(
        const polyhedron::PolyhedronTopologyStreamKind kind,
        const polyhedron::PolyhedronTopologyStreamCodec codec,
        const std::uint64_t elementCount,
        const std::uint32_t meshIndexPadding = 0u,
        std::string* error = nullptr) {
        auto* stream = MutableStream(kind, error);
        if (stream == nullptr) {
            return false;
        }
        if (stream->completed) {
            return validation::AssignError(error, "polyhedron topology stream was completed twice");
        }
        stream->schedule.codec = codec;
        stream->schedule.meshIndexPadding = meshIndexPadding;
        stream->schedule.elementCount = elementCount;
        if (stream->writer == nullptr) {
            return validation::AssignError(error, "polyhedron topology stream writer is missing");
        }
        stream->schedule.streamByteSize = stream->writer->ByteSizeHint();
        stream->schedule.auxiliaryStreamByteSize = 0u;
        stream->completed = true;
        return true;
    }

    [[nodiscard]] bool Complete() const noexcept {
        for (const auto& stream : m_streams) {
            if (!stream.completed) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept {
        std::uint64_t residentBytes = 0u;
        for (const auto& stream : m_streams) {
            if (stream.writer != nullptr) {
                residentBytes = validation::SaturatingAddU64(residentBytes, stream.writer->ResidentSizeHint());
            }
        }
        return residentBytes;
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept {
        std::uint64_t outputBytes = 0u;
        for (const auto& stream : m_streams) {
            if (stream.writer != nullptr) {
                outputBytes = validation::SaturatingAddU64(outputBytes, stream.writer->ByteSizeHint());
            }
        }
        return outputBytes;
    }

    [[nodiscard]] const polyhedron::PolyhedronTopologyStreamSchedule* Schedule(
        const polyhedron::PolyhedronTopologyStreamKind kind) const noexcept {
        const auto index = polyhedron::StatefulPolyhedronTopologyStreamIndex(kind);
        return index < m_streams.size() ? &m_streams[index].schedule : nullptr;
    }

    [[nodiscard]] std::shared_ptr<bytestore::IByteWriter> StreamTransferWriter(
        const polyhedron::PolyhedronTopologyStreamKind kind) const noexcept {
        return m_streams[polyhedron::StatefulPolyhedronTopologyStreamIndex(kind)].writer;
    }

private:
    struct StreamRecord {
        polyhedron::PolyhedronTopologyStreamSchedule schedule;
        std::shared_ptr<bytestore::IByteWriter> writer;
        bool completed{false};
    };

    StreamRecord* MutableStream(const polyhedron::PolyhedronTopologyStreamKind kind, std::string* error) {
        const auto index = polyhedron::StatefulPolyhedronTopologyStreamIndex(kind);
        if (index >= m_streams.size()) {
            validation::AssignError(
                error,
                std::string("unsupported stateful polyhedron topology stream ") +
                    polyhedron::PolyhedronTopologyStreamKindName(kind));
            return nullptr;
        }
        return &m_streams[index];
    }

    std::array<StreamRecord, polyhedron::kStatefulPolyhedronTopologyStreamOrder.size()> m_streams{};
};

class TransferCacheEncodeWriter final : public bytestore::IByteWriter {
public:
    explicit TransferCacheEncodeWriter(
        std::shared_ptr<bytestore::IAppendableByteStore> transferCache, DataCodecExecutionResources& run)
        : m_transferCache(std::move(transferCache)), m_writer(m_transferCache, run) {}

    bool Write(const std::span<const std::uint8_t> bytes, std::string* error = nullptr) override {
        if (m_transferCache == nullptr) {
            return validation::AssignError(error, "transfer cache writer is missing its backing source");
        }
        if (bytes.empty()) {
            return true;
        }
        return m_writer.Write(bytes, error);
    }

    [[nodiscard]] std::uint64_t ByteSizeHint() const noexcept override {
        return m_transferCache != nullptr ? m_transferCache->ByteSizeHint() : 0u;
    }

    [[nodiscard]] std::uint64_t ResidentSizeHint() const noexcept override {
        return m_transferCache != nullptr ? m_transferCache->ResidentSizeHint() : 0u;
    }

    [[nodiscard]] std::shared_ptr<bytestore::IByteSource> TransferCache() const noexcept {
        return m_transferCache;
    }

    bool PrepareForRead(std::string* error = nullptr) {
        if (m_transferCache == nullptr) {
            return validation::AssignError(error, "transfer cache writer is missing its backing source");
        }
        return m_transferCache->Seal(error);
    }

private:
    std::shared_ptr<bytestore::IAppendableByteStore> m_transferCache;
    bytestore::AppendableByteStoreWriter m_writer;
};

template<std::size_t N>
inline std::array<std::shared_ptr<bytestore::IByteWriter>, N> MakeTransferCacheWriterInterfaces(
    const std::array<std::shared_ptr<TransferCacheEncodeWriter>, N>& writers) {
    std::array<std::shared_ptr<bytestore::IByteWriter>, N> interfaces{};
    for (std::size_t index = 0; index < N; ++index) {
        interfaces[index] = writers[index];
    }
    return interfaces;
}

inline std::shared_ptr<bytestore::IByteSource> TransferCacheFromWriter(
    const std::shared_ptr<bytestore::IByteWriter>& writer,
    std::string* error = nullptr) {
    auto transferWriter = std::dynamic_pointer_cast<TransferCacheEncodeWriter>(writer);
    if (transferWriter == nullptr || transferWriter->TransferCache() == nullptr) {
        validation::AssignError(error, "transfer cache writer is not backed by a readable source");
        return nullptr;
    }
    if (!transferWriter->PrepareForRead(error)) {
        return nullptr;
    }
    return transferWriter->TransferCache();
}

inline std::shared_ptr<TransferCacheEncodeWriter> MakeByteStoreTransferCacheEncodeWriter(
    bytestore::ByteStoreSession& session,
    DataCodecExecutionResources& run,
    const std::string& label,
    std::string* error = nullptr) {
    auto cache = bytestore::CreateAppendableByteStore(
        session,
        label,
        error);
    if (cache == nullptr) {
        if (error != nullptr && error->empty()) {
            validation::AssignError(error, "failed to create topology transfer cache");
        }
        return nullptr;
    }
    return std::make_shared<TransferCacheEncodeWriter>(std::move(cache), run);
}


inline std::shared_ptr<PolyhedronTopologyStreamSpooler> MakePolyhedronTopologyStreamSpooler(
    bytestore::ByteStoreSession& session,
    DataCodecExecutionResources& run,
    std::string* error = nullptr) {
    std::array<std::shared_ptr<bytestore::IByteWriter>, polyhedron::kStatefulPolyhedronTopologyStreamOrder.size()> writers{};
    for (std::size_t index = 0; index < polyhedron::kStatefulPolyhedronTopologyStreamOrder.size(); ++index) {
        const auto kind = polyhedron::kStatefulPolyhedronTopologyStreamOrder[index];
        auto writer = MakeByteStoreTransferCacheEncodeWriter(
            session,
            run,
            std::string("topology_polyhedron_") + polyhedron::PolyhedronTopologyStreamKindName(kind),
            error);
        if (writer == nullptr) {
            return nullptr;
        }
        writers[index] = std::move(writer);
    }
    return std::make_shared<PolyhedronTopologyStreamSpooler>(std::move(writers));
}


inline bool ExportPolyhedronTopologyStreamLayouts(
    const PolyhedronTopologyStreamSpooler& streams,
    std::vector<TopologyStreamLayoutParams>& layouts,
    std::string* error = nullptr) {
    layouts.clear();
    layouts.reserve(polyhedron::kStatefulPolyhedronTopologyStreamOrder.size());
    for (const auto streamKind : polyhedron::kStatefulPolyhedronTopologyStreamOrder) {
        const auto* schedule = streams.Schedule(streamKind);
        if (schedule == nullptr) {
            validation::AssignError(
                error,
                std::string("missing stateful polyhedron topology stream ") +
                    polyhedron::PolyhedronTopologyStreamKindName(streamKind));
            return false;
        }
        layouts.push_back(TopologyStreamLayoutParams{
            .encodedByteLength = schedule->streamByteSize,
        });
    }
    return true;
}

inline std::shared_ptr<bytestore::IByteSource> BuildPolyhedronTopologyStreamTransferCache(
    PolyhedronTopologyStreamSpooler& streams,
    std::string* error = nullptr) {
    if (!streams.Complete()) {
        validation::AssignError(error, "polyhedron topology stream transfer set is incomplete");
        return nullptr;
    }

    auto segmentedTransferCache = std::make_shared<bytestore::SegmentedBinaryObject>(
        std::vector<bytestore::SegmentedBinaryObject::Segment>{},
        bytestore::ByteSourceConsumptionMode::OneShot);
    for (std::size_t index = 0; index < polyhedron::kStatefulPolyhedronTopologyStreamOrder.size(); ++index) {
        auto streamTransferCache = TransferCacheFromWriter(
            streams.StreamTransferWriter(polyhedron::kStatefulPolyhedronTopologyStreamOrder[index]),
            error);
        if (streamTransferCache == nullptr ||
            !segmentedTransferCache->AddSegment(std::move(streamTransferCache), error)) {
            return nullptr;
        }
    }
    return segmentedTransferCache;
}

} // namespace topology
} // namespace datacodec

#endif
