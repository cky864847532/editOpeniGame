#ifndef DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYTYPES_H
#define DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYTYPES_H

#include "DataCodec/Common/DataCodecTypes.h"
#include "DataCodec/Common/Views/BufferCapacitySample.h"
#include "DataCodec/Storage/ByteIO/ContiguousView.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <cstddef>
#include <array>
#include <optional>
#include <cstdint>
#include <span>
#include <string>

namespace datacodec::topocodec {

enum class TopologyBufferSample : std::size_t {
    Connectivity,
    CellSizes,
    CellTypes,
    PolynomialOrders,
    CellScratch,
    Offsets,
    AdjustedOffsets,
    GrammarOffsets,
    MainEvents,
    SeedLeadSymbols,
    SeedOrderBytes,
    ResidualBytes,
    MainSymbols,
    RawSeedAux,
    MainStream,
    SeedLeadStream,
    SeedAuxStream,
    ConnectivityBytes,
    CellSizeBytes,
    PolynomialOrderBytes,
    CellTypeBytes,
    ConnectivityReadCopy,
    CellSizeReadCopy,
    PolynomialOrderReadCopy,
    CellTypeReadCopy,
    Count,
};

// 各数组独立取样，块记录只保存固定字段，容量样本不参与准入
struct TopologyBlockCapacitySamples {
    std::array<BufferCapacitySample, static_cast<std::size_t>(TopologyBufferSample::Count)> values{{
        BufferCapacitySample{"topology.connectivity"},
        BufferCapacitySample{"topology.cell_sizes"},
        BufferCapacitySample{"topology.cell_types"},
        BufferCapacitySample{"topology.polynomial_orders"},
        BufferCapacitySample{"topology.cell_scratch"},
        BufferCapacitySample{"topology.offsets"},
        BufferCapacitySample{"topology.adjusted_offsets"},
        BufferCapacitySample{"topology.grammar.offsets"},
        BufferCapacitySample{"topology.grammar.main_events"},
        BufferCapacitySample{"topology.grammar.seed_lead_symbols"},
        BufferCapacitySample{"topology.grammar.seed_order_bytes"},
        BufferCapacitySample{"topology.grammar.residual_bytes"},
        BufferCapacitySample{"topology.grammar.main_symbols"},
        BufferCapacitySample{"topology.grammar.raw_seed_aux"},
        BufferCapacitySample{"topology.grammar.main_stream"},
        BufferCapacitySample{"topology.grammar.seed_lead_stream"},
        BufferCapacitySample{"topology.grammar.seed_aux_stream"},
        BufferCapacitySample{"topology.encoded.connectivity"},
        BufferCapacitySample{"topology.encoded.cell_sizes"},
        BufferCapacitySample{"topology.encoded.polynomial_orders"},
        BufferCapacitySample{"topology.encoded.cell_types"},
        BufferCapacitySample{"topology.read_copy.connectivity"},
        BufferCapacitySample{"topology.read_copy.cell_sizes"},
        BufferCapacitySample{"topology.read_copy.polynomial_orders"},
        BufferCapacitySample{"topology.read_copy.cell_types"},
    }};

    template<class T>
    void Observe(const TopologyBufferSample kind, const T& storage) noexcept {
        values[static_cast<std::size_t>(kind)].Observe(storage);
    }
};

class IConnectivityTopologyDecodeSink {
public:
    virtual ~IConnectivityTopologyDecodeSink() = default;

    virtual bool BeginConnectivityTopology(
        std::size_t cellCount,
        std::size_t connectivityCount,
        bool hasOffsets,
        bool hasCellTypes,
        bool hasCellPolynomialOrders,
        std::string* error = nullptr) = 0;

    virtual bool WriteConnectivityRange(
        std::size_t offset,
        std::span<const IndexType> values,
        std::string* error = nullptr) = 0;

    virtual bool WriteOffsetsRange(
        std::size_t offset,
        std::span<const IndexType> values,
        std::string* error = nullptr) = 0;

    virtual bool WriteCellTypesRange(
        std::size_t offset,
        std::span<const IndexType> values,
        std::string* error = nullptr) = 0;

    virtual bool WriteCellPolynomialOrdersRange(
        std::size_t offset,
        std::span<const std::uint16_t> values,
        std::string* error = nullptr) = 0;

    virtual bool EndConnectivityTopology(std::string* error = nullptr) = 0;
};

enum class ConnectivityTopologyStreamKind : std::uint8_t {
    Connectivity = 0,
    CellSize = 1,
    CellPolynomialOrder = 2,
    CellType = 3,
};

struct ConnectivityTopologyEncodedMetadata {
    std::uint64_t connectivityByteCount{0u};
    std::uint64_t cellSizeByteCount{0u};
    std::uint64_t cellPolynomialOrderByteCount{0u};
    std::uint64_t cellTypeByteCount{0u};
};


class IConnectivityTopologyEncodedStreamReader {
public:
    virtual ~IConnectivityTopologyEncodedStreamReader() = default;
    [[nodiscard]] virtual const ConnectivityTopologyEncodedMetadata& Metadata() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t StreamSize(ConnectivityTopologyStreamKind kind) const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t ResidentSizeHint() const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::uint8_t> ContiguousStreamRange(
        ConnectivityTopologyStreamKind kind,
        std::uint64_t offset,
        std::uint64_t byteCount) const noexcept {
        (void)kind;
        (void)offset;
        (void)byteCount;
        return {};
    }
    virtual ContiguousViewStatus PrepareContiguousStreamRange(
        const ConnectivityTopologyStreamKind kind,
        const std::uint64_t offset,
        const std::uint64_t byteCount,
        std::span<const std::uint8_t>& output,
        std::string* error = nullptr) const {
        output = {};
        if (offset > StreamSize(kind) || byteCount > StreamSize(kind) - offset) {
            validation::AssignError(error, "connectivity contiguous stream range is out of bounds");
            return ContiguousViewStatus::Error;
        }
        output = ContiguousStreamRange(kind, offset, byteCount);
        if (byteCount == 0u || output.size() == byteCount) {
            if (error != nullptr) { error->clear(); }
            return ContiguousViewStatus::Ready;
        }
        if (!output.empty()) {
            output = {};
            validation::AssignError(error, "connectivity contiguous stream range has an invalid size");
            return ContiguousViewStatus::Error;
        }
        if (error != nullptr) { error->clear(); }
        return ContiguousViewStatus::Unavailable;
    }
    virtual bool ReadStreamRange(
        ConnectivityTopologyStreamKind kind,
        std::uint64_t offset,
        std::span<std::uint8_t> output,
        std::string* error = nullptr) const = 0;
};

} // namespace datacodec::topocodec

#endif
