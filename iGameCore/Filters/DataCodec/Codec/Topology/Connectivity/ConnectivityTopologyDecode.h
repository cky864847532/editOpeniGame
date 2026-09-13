#ifndef DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYDECODE_H
#define DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYDECODE_H

#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyCodec.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyTypes.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace datacodec::topocodec {

struct ConnectivityTopologyDecodeTimingEvent {
    std::string name;
    double elapsedMs{0.0};
    std::string scope;
};

using ConnectivityTopologyDecodeTimingCallback =
    std::function<void(const ConnectivityTopologyDecodeTimingEvent&)>;

inline void RecordConnectivityTopologyDecodeTiming(
    const ConnectivityTopologyDecodeTimingCallback& timingCallback,
    std::string name,
    const callback::PhaseTimePoint startTime,
    std::string scope = {}) {
    callback::InvokeTimingEvent(timingCallback, ConnectivityTopologyDecodeTimingEvent{
        std::move(name),
        callback::ElapsedMilliseconds(startTime),
        std::move(scope),
    });
}

struct ConnectivityEncodedStreamView {
    std::vector<std::uint8_t> ownedBytes;
    std::span<const std::uint8_t> bytes;
};

inline bool LoadConnectivityEncodedStream(
    const IConnectivityTopologyEncodedStreamReader& encoded,
    const ConnectivityTopologyStreamKind kind,
    const std::uint64_t byteCount,
    ConnectivityEncodedStreamView& output,
    std::string* error = nullptr) {
    output = {};
    if (encoded.StreamSize(kind) != byteCount) {
        return validation::AssignError(error, "topology encoded stream size does not match metadata");
    }
    if (byteCount == 0u) {
        return true;
    }
    if (byteCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return validation::AssignError(error, "topology encoded stream exceeds local size capacity");
    }
    const auto localByteCount = static_cast<std::size_t>(byteCount);
    const auto contiguousStatus = encoded.PrepareContiguousStreamRange(
        kind,
        0u,
        byteCount,
        output.bytes,
        error);
    if (contiguousStatus == ContiguousViewStatus::Error) {
        return false;
    }
    if (contiguousStatus == ContiguousViewStatus::Ready) {
        if (output.bytes.size() != localByteCount) {
            output = {};
            return validation::AssignError(
                error,
                "topology contiguous encoded stream size does not match metadata");
        }
        return true;
    }
    output.ownedBytes.resize(localByteCount);
    if (!encoded.ReadStreamRange(kind, 0u, output.ownedBytes, error)) {
        output = {};
        return false;
    }
    output.bytes = output.ownedBytes;
    return true;
}

inline bool DecodeConnectivityTopologyBlock(
    const IConnectivityTopologyEncodedStreamReader& encoded,
    const std::size_t pointCount,
    const std::size_t cellCount,
    const std::size_t connectivityCount,
    const int fixedCellSize,
    const bool hasCellTypes,
    auto& output,
    std::string* error = nullptr,
    const ConnectivityTopologyDecodeTimingCallback& timingCallback = {},
    TopologyBlockCapacitySamples* capacitySamples = nullptr, ArrayWorkspace* workspace = nullptr) {
    const auto& metadata = encoded.Metadata();
    const auto inputStart = callback::StartTiming(timingCallback);
    ConnectivityEncodedStreamView connectivityStream;
    ConnectivityEncodedStreamView cellSizeStream;
    ConnectivityEncodedStreamView cellPolynomialOrderStream;
    ConnectivityEncodedStreamView cellTypeStream;
    if (!LoadConnectivityEncodedStream(
            encoded,
            ConnectivityTopologyStreamKind::Connectivity,
            metadata.connectivityByteCount,
            connectivityStream,
            error) ||
        !LoadConnectivityEncodedStream(
            encoded,
            ConnectivityTopologyStreamKind::CellSize,
            metadata.cellSizeByteCount,
            cellSizeStream,
            error) ||
        !LoadConnectivityEncodedStream(
            encoded,
            ConnectivityTopologyStreamKind::CellPolynomialOrder,
            metadata.cellPolynomialOrderByteCount,
            cellPolynomialOrderStream,
            error) ||
        !LoadConnectivityEncodedStream(
            encoded,
            ConnectivityTopologyStreamKind::CellType,
            metadata.cellTypeByteCount,
            cellTypeStream,
            error)) {
        return false;
    }
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(TopologyBufferSample::ConnectivityReadCopy, connectivityStream.ownedBytes);
        capacitySamples->Observe(TopologyBufferSample::CellSizeReadCopy, cellSizeStream.ownedBytes);
        capacitySamples->Observe(TopologyBufferSample::PolynomialOrderReadCopy, cellPolynomialOrderStream.ownedBytes);
        capacitySamples->Observe(TopologyBufferSample::CellTypeReadCopy, cellTypeStream.ownedBytes);
    }
    RecordConnectivityTopologyDecodeTiming(
        timingCallback,
        "connectivity.input",
        inputStart,
        "bytes=" + std::to_string(
            metadata.connectivityByteCount +
            metadata.cellSizeByteCount +
            metadata.cellPolynomialOrderByteCount +
            metadata.cellTypeByteCount));

    const auto auxiliaryStart = callback::StartTiming(timingCallback);
    const bool hasOffsets = fixedCellSize <= 0;
    const bool hasCellPolynomialOrders = metadata.cellPolynomialOrderByteCount != 0u;
    output.connectivity.clear(); output.offsets.clear(); output.cellTypes.clear(); output.polynomialOrders.clear();
    ArrayWorkspace::Scope blockScope(workspace);
    WorkingArray<IndexType> cellSizes(workspace, hasOffsets ? cellCount : 0u);
    auto& offsets = output.offsets;
    if (hasOffsets) {
        if (!blockcodec::DecodeUnsignedSequence<IndexType>(
                cellSizeStream.bytes,
                cellCount,
                cellSizes,
                error)) {
            return false;
        }
        offsets.resize(cellCount + 1u, 0u);
        std::size_t offset = 0u;
        for (std::size_t cellIndex = 0u; cellIndex < cellCount; ++cellIndex) {
            if (!validation::CheckedAddSizeT(
                    offset,
                    static_cast<std::size_t>(cellSizes[cellIndex]),
                    offset,
                    "decoded topology connectivity offset",
                    error) ||
                offset > static_cast<std::size_t>(std::numeric_limits<IndexType>::max())) {
                return offset > static_cast<std::size_t>(std::numeric_limits<IndexType>::max())
                    ? validation::AssignError(error, "decoded topology offset exceeds index capacity")
                    : false;
            }
            offsets[cellIndex + 1u] = static_cast<IndexType>(offset);
        }
        if (offset != connectivityCount) {
            return validation::AssignError(
                error,
                "decoded topology cell sizes do not match connectivity count");
        }
    } else {
        if (!cellSizeStream.bytes.empty()) {
            return validation::AssignError(error, "fixed-size topology carries a cell-size stream");
        }
        std::size_t expectedConnectivityCount = 0u;
        if (!validation::CheckedMulSizeT(
                cellCount,
                static_cast<std::size_t>(fixedCellSize),
                expectedConnectivityCount,
                "decoded fixed topology connectivity count",
                error) ||
            expectedConnectivityCount != connectivityCount) {
            return validation::AssignError(
                error,
                "decoded fixed cell size does not match connectivity count");
        }
    }

    const auto connectivityStart = callback::StartTiming(timingCallback);
    auto& connectivity = output.connectivity;
    if (!blockcodec::DecodeConnectivity(
            connectivityStream.bytes,
            cellSizes,
            pointCount,
            cellCount,
            connectivityCount,
            fixedCellSize,
            connectivity,
            error, capacitySamples, workspace)) {
        return false;
    }
    RecordConnectivityTopologyDecodeTiming(
        timingCallback,
        "connectivity.indices",
        connectivityStart,
        "values=" + std::to_string(connectivityCount));

    auto& cellTypes = output.cellTypes;
    if (hasCellTypes) {
        if (!blockcodec::DecodeUnsignedSequence<IndexType>(
                cellTypeStream.bytes,
                cellCount,
                cellTypes,
                error)) {
            return false;
        }
    } else if (!cellTypeStream.bytes.empty()) {
        return validation::AssignError(error, "topology carries cell types without the cell-type flag");
    }

    auto& cellPolynomialOrders = output.polynomialOrders;
    if (hasCellPolynomialOrders &&
        !blockcodec::DecodeUnsignedSequence<std::uint16_t>(
            cellPolynomialOrderStream.bytes,
            cellCount,
            cellPolynomialOrders,
            error)) {
        return false;
    }
    if (capacitySamples != nullptr) {
        capacitySamples->Observe(TopologyBufferSample::Connectivity, connectivity);
        capacitySamples->Observe(TopologyBufferSample::CellSizes, cellSizes);
        capacitySamples->Observe(TopologyBufferSample::Offsets, offsets);
        capacitySamples->Observe(TopologyBufferSample::CellTypes, cellTypes);
        capacitySamples->Observe(TopologyBufferSample::PolynomialOrders, cellPolynomialOrders);
    }
    RecordConnectivityTopologyDecodeTiming(
        timingCallback,
        "connectivity.auxiliary",
        auxiliaryStart,
        "cells=" + std::to_string(cellCount));

    return true;
}

} // namespace datacodec::topocodec

#endif
