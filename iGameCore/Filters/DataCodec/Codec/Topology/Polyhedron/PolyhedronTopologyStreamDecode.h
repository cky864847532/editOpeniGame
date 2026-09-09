#ifndef DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYSTREAMDECODE_H
#define DATACODEC_CODEC_TOPOLOGY_POLYHEDRON_POLYHEDRONTOPOLOGYSTREAMDECODE_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedIndexCache.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Codec/SubCodec/SegmentedBitpackCodec.h"
#include "DataCodec/Codec/Topology/Polyhedron/PolyhedronTopologyStreamFormat.h"
#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"
#include "DataCodec/API/Params/CodecStorageParams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>
namespace datacodec {
namespace polyhedron {

inline bool ValidatePolyhedronTopologyStreamOrder(
    const std::span<const PolyhedronTopologyStreamSchedule> schedules,
    std::string* error = nullptr) {
    if (schedules.size() != kStatefulPolyhedronTopologyStreamOrder.size()) {
        return validation::AssignError(error, "stateful polyhedron stream count does not match the fixed stream order");
    }
    for (std::size_t index = 0; index < kStatefulPolyhedronTopologyStreamOrder.size(); ++index) {
        if (schedules[index].kind != kStatefulPolyhedronTopologyStreamOrder[index]) {
            return validation::AssignError(
                error,
                std::string("unexpected stateful polyhedron stream order at index ") + std::to_string(index));
        }
    }
    return true;
}

template<typename TStream>
class PolyhedronTopologyStreamByteReader final {
public:
    PolyhedronTopologyStreamByteReader(
        TStream& stream,
        const PolyhedronTopologyStreamSchedule& schedule, const CacheResources& runtime)
        : m_stream(stream), m_schedule(schedule), m_runtime(runtime) {
        m_buffer.resize(static_cast<std::size_t>(std::min<std::uint64_t>(schedule.streamByteSize, kIoWindowBytes)));
    }

    bool ValidateCurrentFormat(std::string* error = nullptr) const {
        if (m_schedule.auxiliaryStreamByteSize != 0u ||
            (m_schedule.flags & kPolyhedronTopologyStreamFlagHasAuxBytes) != 0u) {
            return validation::AssignError(
                error,
                "stateful polyhedron stream auxiliary bytes are not supported by decode");
        }
        return true;
    }

    bool ReadByte(std::uint8_t& value, std::string* error = nullptr) {
        if (m_streamByteCursor >= m_schedule.streamByteSize) {
            return validation::AssignError(error, "stateful polyhedron stream bytes are exhausted");
        }
        if (m_bufferCursor == m_bufferSize) {
            if (m_runtime.Run().Stopped()) { return false; }
            m_bufferSize = static_cast<std::size_t>(std::min<std::uint64_t>(
                m_buffer.size(), m_schedule.streamByteSize - m_streamByteCursor));
            if (!m_stream.ReadBytes(m_buffer.data(), m_bufferSize, error)) { return false; }
            m_bufferCursor = 0u;
        }
        value = m_buffer[m_bufferCursor++];
        if (!validation::CheckedAddU64(
                m_streamByteCursor,
                1u,
                m_streamByteCursor,
                "stateful polyhedron stream byte cursor",
                error)) {
            return false;
        }
        return true;
    }

    bool Finish(std::string* error = nullptr) {
        if (m_streamByteCursor != m_schedule.streamByteSize) {
            return validation::AssignError(error, "stateful polyhedron stream bytes were not consumed exactly");
        }
        return true;
    }

    void ObserveCapacity(PolyhedronCapacitySamples& samples) const noexcept {
        samples.Observe(PolyhedronBufferSample::StreamReadWindow, m_buffer);
    }

private:
    TStream& m_stream;
    const PolyhedronTopologyStreamSchedule& m_schedule;
    const CacheResources& m_runtime;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_bufferCursor{0u}, m_bufferSize{0u};
    std::uint64_t m_streamByteCursor{0u};
};

template<typename TStreamReader>
inline bool DecodeVarUInt64FromStream(
    TStreamReader& streamReader,
    std::uint64_t& value,
    std::string* error = nullptr) {
    value = 0u;
    std::uint32_t shift = 0u;
    for (std::uint32_t byteIndex = 0u; byteIndex < 10u; ++byteIndex) {
        std::uint8_t byte = 0u;
        if (!streamReader.ReadByte(byte, error)) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            return true;
        }
        shift += 7u;
    }
    return validation::AssignError(error, "uint64 varint is too long");
}

template<typename TStream>
inline bool DecodeVarintScheduleToIndexCache(
    TStream& stream,
    const PolyhedronTopologyStreamSchedule& schedule,
    DecodedIndexCache& output,
    const CacheResources& runtime,
    std::string* error = nullptr,
    PolyhedronCapacitySamples* capacitySamples = nullptr) {
    if (schedule.codec != PolyhedronTopologyStreamCodec::Varint) {
        return validation::AssignError(error, "stateful polyhedron id stream requires varint codec");
    }
    if (schedule.elementCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return validation::AssignError(
            error,
            "stateful polyhedron id stream element count is too large for this platform");
    }
    PolyhedronTopologyStreamByteReader<TStream> streamReader(stream, schedule, runtime);
    if (!streamReader.ValidateCurrentFormat(error)) {
        return false;
    }
    if (output.Count() != schedule.elementCount || output.WrittenCount() != 0u) {
        return validation::AssignError(error, "polyhedron varint target must be prepared before terminal work");
    }
    const auto kIndexWriteBatch = runtime.ValuesPerWindow<IndexType>();
    std::vector<IndexType> buffer;
    buffer.reserve(kIndexWriteBatch);
    if (capacitySamples != nullptr) {
        streamReader.ObserveCapacity(*capacitySamples);
        capacitySamples->Observe(PolyhedronBufferSample::IndexWriteWindow, buffer);
    }
    for (std::uint64_t index = 0u; index < schedule.elementCount; ++index) {
        std::uint64_t value = 0u;
        if (!DecodeVarUInt64FromStream(streamReader, value, error) ||
            value > static_cast<std::uint64_t>(std::numeric_limits<IndexType>::max())) {
            if (error != nullptr && error->empty()) {
                return validation::AssignError(
                    error,
                    "stateful polyhedron varint value exceeds IndexType range");
            }
            return false;
        }
        buffer.push_back(static_cast<IndexType>(value));
        if (buffer.size() == kIndexWriteBatch) {
            if (runtime.Run().Stopped()) { return false; }
            if (!output.Append(std::span<const IndexType>(buffer.data(), buffer.size()), error)) {
                return false;
            }
            buffer.clear();
        }
    }
    if (!buffer.empty() &&
        !output.Append(std::span<const IndexType>(buffer.data(), buffer.size()), error)) {
        return false;
    }
    if (output.WrittenCount() != schedule.elementCount) {
        return validation::AssignError(error, "stateful polyhedron index stream count mismatch");
    }
    if (!streamReader.Finish(error)) {
        return false;
    }
    return true;
}

template<typename TStreamReader>
class LocalFaceIdBitReader final {
public:
    explicit LocalFaceIdBitReader(TStreamReader& streamReader) : m_streamReader(streamReader) {}

    bool ReadValue(
        const std::uint8_t bitWidth,
        const IndexType exclusiveUpperBound,
        IndexType& value,
        std::string* error = nullptr) {
        value = 0;
        if (bitWidth == 0u) {
            if (exclusiveUpperBound <= 0) {
                return validation::AssignError(error, "polyhedron local face id has no valid local vertex range");
            }
            return true;
        }
        if (bitWidth > 63u) {
            return validation::AssignError(error, "polyhedron local face id bit width is too large");
        }
        std::uint64_t decoded = 0u;
        for (std::uint8_t bit = 0u; bit < bitWidth; ++bit) {
            if (m_bitCursor == 8u) {
                if (!m_streamReader.ReadByte(m_currentByte, error)) {
                    return false;
                }
                m_bitCursor = 0u;
            }
            if (((m_currentByte >> m_bitCursor) & 1u) != 0u) {
                decoded |= std::uint64_t{1u} << bit;
            }
            ++m_bitCursor;
        }
        if (decoded > static_cast<std::uint64_t>(std::numeric_limits<IndexType>::max()) ||
            decoded >= static_cast<std::uint64_t>(exclusiveUpperBound)) {
            return validation::AssignError(error, "polyhedron local face id exceeds unique vertex count");
        }
        value = static_cast<IndexType>(decoded);
        return true;
    }

    bool Finish(std::string* error = nullptr) {
        if (m_bitCursor != 8u) {
            const auto mask = static_cast<std::uint8_t>(0xffu << m_bitCursor);
            if ((m_currentByte & mask) != 0u) {
                return validation::AssignError(error, "polyhedron local face id padding bits are not zero");
            }
        }
        return m_streamReader.Finish(error);
    }

private:
    TStreamReader& m_streamReader;
    std::uint8_t m_currentByte{0u};
    std::uint8_t m_bitCursor{8u};
};



inline bool ValidateGlobalPolyhedronCountStores(
    const PolyhedronTopologyStreamHeader& header,
    const DecodedIndexCache& uniqueVertexCountStore,
    const DecodedIndexCache& cellFaceCountStore,
    const DecodedIndexCache& faceVertexCountStore,
    const CacheResources& runtime,
    std::string* error = nullptr,
    PolyhedronCapacitySamples* capacitySamples = nullptr) {
    if (header.cellCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        header.faceCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return validation::AssignError(
            error,
            "stateful polyhedron topology counts are too large for this platform");
    }
    if (uniqueVertexCountStore.Count() != header.cellCount ||
        cellFaceCountStore.Count() != header.cellCount ||
        faceVertexCountStore.Count() != header.faceCount) {
        return validation::AssignError(
            error,
            "stateful polyhedron count stream sizes do not match the stream header");
    }

    const auto kCountReadBatch = runtime.ValuesPerWindow<IndexType>(2u);
    std::vector<IndexType> uniqueVertexCounts;
    std::vector<IndexType> cellFaceCounts;
    std::uint64_t derivedFaceCount = 0u;
    std::uint64_t derivedUniqueIdCount = 0u;
    const auto cellCount = static_cast<std::size_t>(header.cellCount);
    for (std::size_t cursor = 0u; cursor < cellCount;) {
        if (runtime.Run().Stopped()) { return false; }
        const auto count = std::min<std::size_t>(kCountReadBatch, cellCount - cursor);
        uniqueVertexCounts.assign(count, 0);
        cellFaceCounts.assign(count, 0);
        if (!uniqueVertexCountStore.ReadRangeInto(
                cursor,
                std::span<IndexType>(uniqueVertexCounts.data(), uniqueVertexCounts.size()),
                error) ||
            !cellFaceCountStore.ReadRangeInto(
                cursor,
                std::span<IndexType>(cellFaceCounts.data(), cellFaceCounts.size()),
                error)) {
            return false;
        }
        if (capacitySamples != nullptr) {
            capacitySamples->Observe(PolyhedronBufferSample::UniqueCounts, uniqueVertexCounts);
            capacitySamples->Observe(PolyhedronBufferSample::CellFaceCounts, cellFaceCounts);
        }
        for (std::size_t index = 0u; index < count; ++index) {
            if (uniqueVertexCounts[index] < 0 || cellFaceCounts[index] < 0) {
                return validation::AssignError(
                    error,
                    "stateful polyhedron count stream contains negative values");
            }
            if (!validation::CheckedAddU64(
                    derivedUniqueIdCount,
                    static_cast<std::uint64_t>(uniqueVertexCounts[index]),
                    derivedUniqueIdCount,
                    "stateful polyhedron derived unique id count",
                    error) ||
                !validation::CheckedAddU64(
                    derivedFaceCount,
                    static_cast<std::uint64_t>(cellFaceCounts[index]),
                    derivedFaceCount,
                    "stateful polyhedron derived face count",
                    error)) {
                return false;
            }
        }
        cursor += count;
    }
    if (derivedFaceCount != header.faceCount || derivedUniqueIdCount != header.uniqueVertexIdCount) {
        return validation::AssignError(
            error,
            "stateful polyhedron cell count streams do not match topology totals");
    }

    std::vector<IndexType> faceVertexCounts;
    std::uint64_t derivedLocalFaceIdCount = 0u;
    const auto faceCount = static_cast<std::size_t>(header.faceCount);
    for (std::size_t cursor = 0u; cursor < faceCount;) {
        if (runtime.Run().Stopped()) { return false; }
        const auto count = std::min<std::size_t>(kCountReadBatch, faceCount - cursor);
        faceVertexCounts.assign(count, 0);
        if (!faceVertexCountStore.ReadRangeInto(
                cursor,
                std::span<IndexType>(faceVertexCounts.data(), faceVertexCounts.size()),
                error)) {
            return false;
        }
        if (capacitySamples != nullptr) {
            capacitySamples->Observe(PolyhedronBufferSample::FaceCountWindow, faceVertexCounts);
        }
        for (const auto faceVertexCount : faceVertexCounts) {
            if (faceVertexCount < 0) {
                return validation::AssignError(
                    error,
                    "stateful polyhedron face vertex count stream contains negative values");
            }
            if (!validation::CheckedAddU64(
                    derivedLocalFaceIdCount,
                    static_cast<std::uint64_t>(faceVertexCount),
                    derivedLocalFaceIdCount,
                    "stateful polyhedron derived local face id count",
                    error)) {
                return false;
            }
        }
        cursor += count;
    }
    if (derivedLocalFaceIdCount != header.localFaceVertexIdCount) {
        return validation::AssignError(error, "stateful polyhedron face count stream does not match local id total");
    }
    return true;
}

template<typename TLocalFaceIdReader>
inline bool DecodeLocalFaceIdsToCache(
    const PolyhedronTopologyStreamHeader& header,
    const DecodedIndexCache& uniqueVertexCountStore,
    const DecodedIndexCache& cellFaceCountStore,
    const DecodedIndexCache& faceVertexCountStore,
    TLocalFaceIdReader& localIdReader,
    DecodedIndexCache& localIdStore,
    const CacheResources& runtime,
    std::string* error = nullptr,
    PolyhedronCapacitySamples* capacitySamples = nullptr) {
    if (localIdStore.Count() != header.localFaceVertexIdCount || localIdStore.WrittenCount() != 0u) {
        return validation::AssignError(error, "polyhedron local-id target must be prepared before terminal work");
    }

    const auto kLocalIdWriteBatch = runtime.ValuesPerWindow<IndexType>();
    std::vector<IndexType> output;
    output.reserve(kLocalIdWriteBatch);
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(PolyhedronBufferSample::IndexWriteWindow, output);
    }
    std::uint64_t written = 0u;
    std::uint64_t faceCursor = 0u;
    for (std::uint64_t cell = 0u; cell < header.cellCount; ++cell) {
        if (runtime.Run().Stopped()) { return false; }
        IndexType uniqueCountValue = 0;
        IndexType cellFaceCountValue = 0;
        if (!uniqueVertexCountStore.ReadScalar(cell, uniqueCountValue, error) ||
            !cellFaceCountStore.ReadScalar(cell, cellFaceCountValue, error)) {
            return false;
        }
        if (uniqueCountValue < 0 || cellFaceCountValue < 0) {
            return validation::AssignError(error, "stateful polyhedron count stream contains negative values");
        }
        const auto uniqueCount = static_cast<std::size_t>(uniqueCountValue);
        const auto cellFaceCount = static_cast<std::size_t>(cellFaceCountValue);
        const auto bitWidth = codec::RequiredBitWidthForExclusiveUpperBound(
            static_cast<IndexType>(uniqueCount));
        for (std::size_t localFace = 0u; localFace < cellFaceCount; ++localFace) {
            IndexType faceVertexCountValue = 0;
            if (!faceVertexCountStore.ReadScalar(faceCursor, faceVertexCountValue, error)) {
                return false;
            }
            if (!validation::CheckedAddU64(
                    faceCursor,
                    1u,
                    faceCursor,
                    "stateful polyhedron face cursor",
                    error)) {
                return false;
            }
            if (faceVertexCountValue < 0) {
                return validation::AssignError(
                    error,
                    "stateful polyhedron face vertex count stream contains negative values");
            }
            for (std::size_t local = 0u; local < static_cast<std::size_t>(faceVertexCountValue); ++local) {
                IndexType localId = 0;
                if (!localIdReader.ReadValue(
                        bitWidth,
                        static_cast<IndexType>(uniqueCount),
                        localId,
                        error)) {
                    return false;
                }
                output.push_back(localId);
                if (output.size() == kLocalIdWriteBatch) {
                    if (runtime.Run().Stopped()) { return false; }
                    if (!localIdStore.Append(std::span<const IndexType>(output.data(), output.size()), error)) {
                        return false;
                    }
                    if (!validation::CheckedAddU64(
                            written,
                            static_cast<std::uint64_t>(output.size()),
                            written,
                            "stateful polyhedron written local id count",
                            error)) {
                        return false;
                    }
                    output.clear();
                }
            }
        }
    }
    if (!output.empty()) {
        if (!localIdStore.Append(std::span<const IndexType>(output.data(), output.size()), error)) {
            return false;
        }
        if (!validation::CheckedAddU64(
                written,
                static_cast<std::uint64_t>(output.size()),
                written,
                "stateful polyhedron written local id count",
                error)) {
            return false;
        }
    }
    if (written != header.localFaceVertexIdCount || !localIdReader.Finish(error)) {
        if (error != nullptr && error->empty()) {
            return validation::AssignError(error, "stateful polyhedron local id stream count mismatch");
        }
        return false;
    }
    return true;
}

template<typename TStream>
inline bool DecodePolyhedronTopologyStreamsToCache(
    const CacheResources& runtime,
    bytestore::ByteStoreSession& byteStoreSession,
    DecodedTopologyCache& topology,
    const TopoStorageParams& topo,
    TStream& stream,
    std::string* error = nullptr,
    const callback::CapacityCallback& recordCapacitySamples = {}) {
    PolyhedronTopologyStreamHeader streamHeader;
    streamHeader.streamCount = static_cast<std::uint32_t>(topo.polyhedronStreamLayouts.size());
    streamHeader.cellCount = topo.cellCount;
    streamHeader.faceCount = topo.polyhedronFaceVertexCount;
    streamHeader.uniqueVertexIdCount = topo.polyhedronVertexCount;
    streamHeader.localFaceVertexIdCount = topo.cellBufferSize;
    if (streamHeader.streamCount != kStatefulPolyhedronTopologyStreamOrder.size()) {
        return validation::AssignError(error, "stateful polyhedron stream count does not match the fixed stream order");
    }
    if (streamHeader.cellCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return validation::AssignError(error, "stateful polyhedron cell count is too large for this platform");
    }
    if (topo.polyhedronStreamLayouts.size() != kStatefulPolyhedronTopologyStreamOrder.size()) {
        return validation::AssignError(
            error,
            "stateful polyhedron stream layout count does not match the fixed stream order");
    }
    std::vector<PolyhedronTopologyStreamSchedule> schedules;
    schedules.reserve(topo.polyhedronStreamLayouts.size());
    for (std::size_t index = 0u; index < topo.polyhedronStreamLayouts.size(); ++index) {
        const auto& layout = topo.polyhedronStreamLayouts[index];
        PolyhedronTopologyStreamSchedule schedule;
        schedule.kind = kStatefulPolyhedronTopologyStreamOrder[index];
        schedule.codec = schedule.kind == PolyhedronTopologyStreamKind::LocalFaceVertexIds
            ? PolyhedronTopologyStreamCodec::SegmentedBitpack
            : PolyhedronTopologyStreamCodec::Varint;
        switch (schedule.kind) {
            case PolyhedronTopologyStreamKind::UniqueVertexCounts:
            case PolyhedronTopologyStreamKind::CellFaceCounts:
                schedule.elementCount = topo.cellCount;
                break;
            case PolyhedronTopologyStreamKind::FaceVertexCounts:
                schedule.elementCount = topo.polyhedronFaceVertexCount;
                break;
            case PolyhedronTopologyStreamKind::CellUniqueVertexIds:
                schedule.elementCount = topo.polyhedronVertexCount;
                break;
            case PolyhedronTopologyStreamKind::LocalFaceVertexIds:
                schedule.elementCount = topo.cellBufferSize;
                break;
        }
        schedule.streamByteSize = layout.encodedByteLength;
        schedules.push_back(schedule);
    }
    if (!ValidatePolyhedronTopologyStreamOrder(schedules, error)) {
        return false;
    }
    const auto& uniqueCountsSchedule = schedules[0];
    const auto& cellFaceCountsSchedule = schedules[1];
    const auto& faceVertexCountsSchedule = schedules[2];
    const auto& uniqueIdsSchedule = schedules[3];
    const auto& localIdsSchedule = schedules[4];
    if (uniqueCountsSchedule.codec != PolyhedronTopologyStreamCodec::Varint ||
        cellFaceCountsSchedule.codec != PolyhedronTopologyStreamCodec::Varint ||
        faceVertexCountsSchedule.codec != PolyhedronTopologyStreamCodec::Varint ||
        uniqueIdsSchedule.codec != PolyhedronTopologyStreamCodec::Varint ||
        localIdsSchedule.codec != PolyhedronTopologyStreamCodec::SegmentedBitpack) {
        return validation::AssignError(
            error,
            "stateful polyhedron stream decode requires current varint/segmented-bitpack stream codecs");
    }
    if (uniqueCountsSchedule.elementCount != streamHeader.cellCount ||
        cellFaceCountsSchedule.elementCount != streamHeader.cellCount ||
        faceVertexCountsSchedule.elementCount != streamHeader.faceCount ||
        uniqueIdsSchedule.elementCount != streamHeader.uniqueVertexIdCount ||
        localIdsSchedule.elementCount != streamHeader.localFaceVertexIdCount) {
        return validation::AssignError(error, "stateful polyhedron stream schedules do not match topology totals");
    }

    auto& root = runtime.Run();
    auto phase = WaitForHeavyPhase(root);
    if (!phase) { return false; }
    topology.Release();
    DecodedTopologyCache prepared;
    prepared.kind = DecodedTopologyCache::Kind::Polyhedron;
    auto& polyhedron = prepared.polyhedron;
    polyhedron.cellCount = streamHeader.cellCount;
    polyhedron.faceCount = streamHeader.faceCount;
    polyhedron.uniqueVertexIdCount = streamHeader.uniqueVertexIdCount;
    polyhedron.localFaceVertexIdCount = streamHeader.localFaceVertexIdCount;
    // 五个完整目标先由 driver 准入，终端解码仅填充既有范围
    bytestore::KnownStorageOwners coexist;
    const auto prepare = [&](DecodedIndexCache& target, const std::uint64_t count, const char* label) {
        if (!target.Initialize(count, byteStoreSession, error, coexist.Entries(), label)) { return false; }
        coexist.Add(target.ByteSource());
        return true;
    };
    if (!prepare(polyhedron.uniqueVertexCounts, streamHeader.cellCount, "poly_unique_n") ||
        !prepare(polyhedron.cellFaceCounts, streamHeader.cellCount, "poly_cell_n") ||
        !prepare(polyhedron.faceVertexCounts, streamHeader.faceCount, "poly_face_n") ||
        !prepare(polyhedron.cellUniqueVertexIds, streamHeader.uniqueVertexIdCount, "poly_unique_id") ||
        !prepare(polyhedron.localFaceVertexIds, streamHeader.localFaceVertexIdCount, "poly_local_id")) { return false; }
    std::optional<std::array<PolyhedronCapacitySamples, 6>> samples;
    if (recordCapacitySamples) { samples.emplace(); }
    const auto sample = [&](const std::size_t index) -> PolyhedronCapacitySamples* {
        return samples ? &(*samples)[index] : nullptr;
    };
    if (!RunTerminalWork(root, *phase, [&](WorkerContext&) {
        if (!DecodeVarintScheduleToIndexCache(stream, uniqueCountsSchedule, polyhedron.uniqueVertexCounts, runtime, error, sample(0)) ||
            !DecodeVarintScheduleToIndexCache(stream, cellFaceCountsSchedule, polyhedron.cellFaceCounts, runtime, error, sample(1)) ||
            !DecodeVarintScheduleToIndexCache(stream, faceVertexCountsSchedule, polyhedron.faceVertexCounts, runtime, error, sample(2)) ||
            !DecodeVarintScheduleToIndexCache(stream, uniqueIdsSchedule, polyhedron.cellUniqueVertexIds, runtime, error, sample(3)) ||
            !ValidateGlobalPolyhedronCountStores(streamHeader, polyhedron.uniqueVertexCounts,
                polyhedron.cellFaceCounts, polyhedron.faceVertexCounts, runtime, error, sample(4))) { return false; }
        PolyhedronTopologyStreamByteReader<TStream> localStreamReader(stream, localIdsSchedule, runtime);
        if (!localStreamReader.ValidateCurrentFormat(error)) { return false; }
        if (samples) { localStreamReader.ObserveCapacity(*sample(5)); }
        LocalFaceIdBitReader<decltype(localStreamReader)> localIdReader(localStreamReader);
        return DecodeLocalFaceIdsToCache(streamHeader, polyhedron.uniqueVertexCounts, polyhedron.cellFaceCounts,
            polyhedron.faceVertexCounts, localIdReader, polyhedron.localFaceVertexIds, runtime, error, sample(5));
    })) { return false; }
    if (samples && recordCapacitySamples) {
        for (const auto& entry : *samples) {
            try { recordCapacitySamples(entry.values); }
            catch (...) { root.RecordDiagnosticExportFailure(); }
        }
    }
    if (!prepared.PrepareForRead(error)) { return false; }
    topology = std::move(prepared);
    return true;
}

} // namespace polyhedron
} // namespace datacodec

#endif
