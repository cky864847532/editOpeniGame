#ifndef DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYBLOCKENCODE_H
#define DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYTOPOLOGYBLOCKENCODE_H

#include "DataCodec/API/Adapter/IEncodeAdapter.h"
#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Codec/Remap/RemapOrderSource.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyCodec.h"
#include "DataCodec/Codec/Topology/Connectivity/ConnectivityTopologyTypes.h"
#include "DataCodec/Common/DataCodecCallback.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace datacodec::topocodec {

struct OrderedTopologyCellSource {
    const IEncodeAdapter* adapter{nullptr};
    const IndexType* indices{nullptr};
    const IndexType* offsets{nullptr};
    const IndexType* cellTypes{nullptr};
    const std::uint16_t* cellPolynomialOrders{nullptr};
    NumericArrayView cellTypesView;
    NumericArrayView cellPolynomialOrdersView;
    std::size_t pointCount{0u};
    std::size_t cellCount{0u};
    std::size_t connectivityCount{0u};
    int fixedCellSize{0};
    const IRemapProvider* inversePointRemap{nullptr};
    std::span<const IndexType> inversePointRemapValues;
    const IRemapProvider* cellOrderProvider{nullptr};
    std::span<const IndexType> cellOrderValues;
    bool hasCellTypes{false};
    bool hasCellPolynomialOrders{false};

    [[nodiscard]] std::size_t CellCount() const noexcept { return cellCount; }
    [[nodiscard]] std::size_t ConnectivityCount() const noexcept { return connectivityCount; }
    [[nodiscard]] bool HasCellTypes() const noexcept { return hasCellTypes; }
    [[nodiscard]] bool HasCellPolynomialOrders() const noexcept { return hasCellPolynomialOrders; }

    [[nodiscard]] std::size_t OldCellIndex(const std::size_t newCellIndex) const {
        if (newCellIndex >= cellCount) {
            throw std::runtime_error("DataCodec topology cell index is out of range");
        }
        IndexType oldCellIndex = static_cast<IndexType>(newCellIndex);
        if (!cellOrderValues.empty()) {
            oldCellIndex = cellOrderValues[newCellIndex];
        } else if (cellOrderProvider != nullptr && !cellOrderProvider->IsIdentity()) {
            std::string error;
            if (!ReadRemapValue(cellOrderProvider, newCellIndex, oldCellIndex, &error)) {
                throw std::runtime_error("DataCodec topology failed to read cell order: " + error);
            }
        }
        if (static_cast<std::size_t>(oldCellIndex) >= cellCount) {
            throw std::runtime_error("DataCodec topology cell order is out of range");
        }
        return static_cast<std::size_t>(oldCellIndex);
    }

    [[nodiscard]] IndexType CellTypeFromOld(const std::size_t oldCellIndex) const {
        if (cellTypes != nullptr) {
            return cellTypes[oldCellIndex];
        }
        if (cellTypesView.tupleCount == cellCount && cellTypesView.IsValid()) {
            IndexType value = 0u;
            std::string error;
            if (!ReadNumericArrayTupleBytes(cellTypesView, oldCellIndex, 1u, &value, &error)) {
                throw std::runtime_error("DataCodec topology failed to read cell type: " + error);
            }
            return value;
        }
        IndexType value = 0u;
        if (adapter != nullptr && adapter->ReadCellType(oldCellIndex, value)) {
            return value;
        }
        throw std::runtime_error("DataCodec topology failed to read cell type");
    }

    [[nodiscard]] std::uint16_t CellPolynomialOrderFromOld(const std::size_t oldCellIndex) const {
        if (cellPolynomialOrders != nullptr) {
            return cellPolynomialOrders[oldCellIndex];
        }
        if (cellPolynomialOrdersView.tupleCount == cellCount && cellPolynomialOrdersView.IsValid()) {
            std::uint16_t value = 0u;
            std::string error;
            if (!ReadNumericArrayTupleBytes(
                    cellPolynomialOrdersView,
                    oldCellIndex,
                    1u,
                    &value,
                    &error)) {
                throw std::runtime_error(
                    "DataCodec topology failed to read cell polynomial order: " + error);
            }
            return value;
        }
        std::uint16_t value = 0u;
        if (adapter != nullptr && adapter->ReadCellPolynomialOrder(oldCellIndex, value)) {
            return value;
        }
        throw std::runtime_error("DataCodec topology failed to read cell polynomial order");
    }

    void CellRange(const std::size_t oldCellIndex, std::size_t& begin, std::size_t& end) const {
        if (oldCellIndex >= cellCount) {
            throw std::runtime_error("DataCodec topology cell index is out of range");
        }
        if (fixedCellSize > 0) {
            const auto cellSize = static_cast<std::size_t>(fixedCellSize);
            if (!validation::CheckedMulSizeT(
                    oldCellIndex,
                    cellSize,
                    begin,
                    "topology fixed cell offset",
                    nullptr) ||
                !validation::CheckedAddSizeT(
                    begin,
                    cellSize,
                    end,
                    "topology fixed cell end",
                    nullptr)) {
                throw std::runtime_error("DataCodec topology fixed cell range exceeds local capacity");
            }
        } else {
            if (offsets == nullptr) {
                throw std::runtime_error("DataCodec topology offset stream is missing");
            }
            begin = static_cast<std::size_t>(offsets[oldCellIndex]);
            end = static_cast<std::size_t>(offsets[oldCellIndex + 1u]);
            if (end < begin) {
                throw std::runtime_error("DataCodec topology offsets are not monotonic");
            }
        }
        if (end > connectivityCount) {
            throw std::runtime_error("DataCodec topology cell range exceeds connectivity storage");
        }
    }

    void ReadCell(
        const std::size_t newCellIndex,
        std::vector<IndexType>& output,
        IndexType* cellType,
        std::uint16_t* cellPolynomialOrder) const {
        const auto oldCellIndex = OldCellIndex(newCellIndex);
        std::size_t begin = 0u, end = 0u;
        CellRange(oldCellIndex, begin, end);
        output.resize(end - begin);
        for (std::size_t local = 0u; local < output.size(); ++local) {
            const auto sourcePoint = indices[begin + local];
            if (static_cast<std::size_t>(sourcePoint) >= pointCount) {
                throw std::runtime_error("DataCodec topology point index is out of range");
            }
            IndexType mappedPoint = sourcePoint;
            if (!inversePointRemapValues.empty()) {
                mappedPoint = inversePointRemapValues[static_cast<std::size_t>(sourcePoint)];
            } else if (inversePointRemap != nullptr && !inversePointRemap->IsIdentity()) {
                std::string error;
                if (!ReadRemapValue(
                        inversePointRemap,
                        static_cast<std::size_t>(sourcePoint),
                        mappedPoint,
                        &error)) {
                    throw std::runtime_error("DataCodec topology failed to read point order: " + error);
                }
            }
            if (static_cast<std::size_t>(mappedPoint) >= pointCount) {
                throw std::runtime_error("DataCodec topology mapped point index is out of range");
            }
            output[local] = mappedPoint;
        }
        if (cellType != nullptr) {
            *cellType = CellTypeFromOld(oldCellIndex);
        }
        if (cellPolynomialOrder != nullptr) {
            *cellPolynomialOrder = CellPolynomialOrderFromOld(oldCellIndex);
        }
    }
};

struct OrderedTopologyCellRangeSource {
    const OrderedTopologyCellSource* source{nullptr};
    std::size_t firstCell{0u};
    std::size_t cellCount{0u};

    [[nodiscard]] std::size_t CellCount() const noexcept { return cellCount; }
    [[nodiscard]] bool HasCellTypes() const noexcept { return source->HasCellTypes(); }
    [[nodiscard]] bool HasCellPolynomialOrders() const noexcept {
        return source->HasCellPolynomialOrders();
    }
};

struct TopologyEncodeData {
    const IEncodeAdapter& adapter;
    const RemapOrderSource& pointInverseOrderSource;
    const RemapOrderSource& cellOrderSource;
};

struct TopologyEncodeExecutionParams {
    DataCodecExecutionResources& resources;
    std::uint32_t cellElementCount{numericarray::kSpatialBlockElementCount};
};

struct TopologyEncodeRuntime {
    bytestore::ByteStoreSession& byteStoreSession;
};

struct TopologyEncodeContext {
    std::function<void(double)> progressCallback;
    callback::CapacityCallback recordCapacitySamples;
};

struct TopologyEncodeInput {
    TopologyEncodeData data;
    TopologyEncodeExecutionParams execution;
    TopologyEncodeRuntime runtime;
    TopologyEncodeContext context;
};

struct TopologyEncodeResult {
    TopoStorageParams topo;
    std::array<int, 3> structuredAxisSize{0, 0, 0};
    std::shared_ptr<bytestore::IByteSource> transferCache;
};

inline void InvokeConnectivityProgress(
    const TopologyEncodeInput& input,
    const double normalized) {
    callback::InvokeProgress(input.context.progressCallback, normalized);
}


inline bool CalculateTopologyRemapStorageBytes(
    const std::size_t count, std::size_t& bytes, std::string* error = nullptr) {
    return validation::CheckedMulSizeT(count, sizeof(IndexType), bytes, "topology remap bytes", error);
}

inline bool PrepareTopologyRemapValues(
    const IRemapProvider* provider, const std::size_t count,
    bytestore::ByteStoreSession& session,
    std::shared_ptr<bytestore::MemoryStore>& owner,
    std::span<const IndexType>& values, std::string* error) {
    owner.reset();
    values = {};
    if (provider == nullptr || provider->IsIdentity()) { return true; }
    if (provider->Size() != count) {
        return validation::AssignError(error, "topology remap size does not match its domain");
    }
    std::size_t byteSize = 0u;
    if (!CalculateTopologyRemapStorageBytes(count, byteSize, error)) {
        return false;
    }
    auto prepared = std::static_pointer_cast<bytestore::MemoryStore>(
        session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, byteSize, ::datacodec::MemoryDemandKind::RequiredContinuation, "topology_remap", error));
    if (!prepared) { return false; }
    auto bytes = prepared->WritableBytes();
    if (bytes.size() != byteSize || reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(IndexType) != 0u) {
        return validation::AssignError(error, "topology remap storage is not an aligned complete array");
    }
    const auto writable = std::span<IndexType>(reinterpret_cast<IndexType*>(bytes.data()), count);
    if (!provider->ReadRange(0u, writable, error) || !prepared->Seal(error)) { return false; }
    values = writable;
    owner = std::move(prepared);
    return true;
}

struct TopologyBlockInput {
    std::size_t firstCell{0u};
    std::size_t cellCount{0u};
    std::vector<IndexType> connectivity;
    std::vector<IndexType> cellSizes;
    std::vector<IndexType> cellTypes;
    std::vector<std::uint16_t> cellPolynomialOrders;
    std::optional<TopologyBlockCapacitySamples> capacitySamples;
};

struct TopologyBlockEncodeArtifact {
    std::size_t firstCell{0u};
    std::size_t cellCount{0u};
    std::size_t connectivityCount{0u};
    std::vector<std::uint8_t> connectivityBytes;
    std::vector<std::uint8_t> cellSizeBytes;
    std::vector<std::uint8_t> cellPolynomialOrderBytes;
    std::vector<std::uint8_t> cellTypeBytes;
    std::optional<TopologyBlockCapacitySamples> capacitySamples;
};

inline bool ReadTopologyBlock(
    const OrderedTopologyCellRangeSource& range, const int fixedCellSize,
    TopologyBlockInput& block, std::string* error = nullptr) {
    block.firstCell = range.firstCell;
    block.cellCount = range.cellCount;
    std::size_t connectivityCount = 0u;
    for (std::size_t local = 0u; local < range.cellCount; ++local) {
        std::size_t begin = 0u, end = 0u;
        range.source->CellRange(range.source->OldCellIndex(range.firstCell + local), begin, end);
        if (!validation::CheckedAddSizeT(connectivityCount, end - begin, connectivityCount,
                "topology block connectivity count", error)) { return false; }
    }
    block.connectivity.reserve(connectivityCount);
    if (fixedCellSize <= 0) { block.cellSizes.reserve(range.cellCount); }
    if (range.HasCellTypes()) { block.cellTypes.reserve(range.cellCount); }
    if (range.HasCellPolynomialOrders()) { block.cellPolynomialOrders.reserve(range.cellCount); }

    std::vector<IndexType> cell;
    for (std::size_t local = 0u; local < range.cellCount; ++local) {
        IndexType cellType = 0u;
        std::uint16_t polynomialOrder = 0u;
        range.source->ReadCell(range.firstCell + local, cell,
            range.HasCellTypes() ? &cellType : nullptr,
            range.HasCellPolynomialOrders() ? &polynomialOrder : nullptr);
        if (block.capacitySamples) { block.capacitySamples->Observe(TopologyBufferSample::CellScratch, cell); }
        if (cell.size() > std::numeric_limits<IndexType>::max() ||
            block.connectivity.size() > connectivityCount ||
            cell.size() > connectivityCount - block.connectivity.size()) {
            return validation::AssignError(error, "topology cell size changed after counting");
        }
        block.connectivity.insert(block.connectivity.end(), cell.begin(), cell.end());
        if (fixedCellSize <= 0) { block.cellSizes.push_back(static_cast<IndexType>(cell.size())); }
        if (range.HasCellTypes()) { block.cellTypes.push_back(cellType); }
        if (range.HasCellPolynomialOrders()) { block.cellPolynomialOrders.push_back(polynomialOrder); }
    }
    if (block.connectivity.size() != connectivityCount) {
        return validation::AssignError(error, "topology block connectivity count changed");
    }
    if (block.capacitySamples) {
        auto& samples = *block.capacitySamples;
        samples.Observe(TopologyBufferSample::Connectivity, block.connectivity);
        samples.Observe(TopologyBufferSample::CellSizes, block.cellSizes);
        samples.Observe(TopologyBufferSample::CellTypes, block.cellTypes);
        samples.Observe(TopologyBufferSample::PolynomialOrders, block.cellPolynomialOrders);
    }
    return true;
}

inline bool EncodeTopologyBlock(
    const TopologyBlockInput& block, const std::size_t pointCount, const int fixedCellSize,
    TopologyBlockEncodeArtifact& artifact, std::string* error = nullptr) {
    artifact.firstCell = block.firstCell;
    artifact.cellCount = block.cellCount;
    artifact.connectivityCount = block.connectivity.size();
    artifact.capacitySamples = block.capacitySamples;
    auto* samples = artifact.capacitySamples ? &*artifact.capacitySamples : nullptr;
    const bool encoded = blockcodec::EncodeConnectivity(block.connectivity, block.cellSizes, block.cellCount,
            fixedCellSize, pointCount, artifact.connectivityBytes, error, true, samples) &&
        blockcodec::EncodeUnsignedSequence<IndexType>(block.cellSizes, artifact.cellSizeBytes, error) &&
        blockcodec::EncodeUnsignedSequence<std::uint16_t>(block.cellPolynomialOrders,
            artifact.cellPolynomialOrderBytes, error) &&
        blockcodec::EncodeUnsignedSequence<IndexType>(block.cellTypes, artifact.cellTypeBytes, error);
    if (samples != nullptr) {
        samples->Observe(TopologyBufferSample::ConnectivityBytes, artifact.connectivityBytes);
        samples->Observe(TopologyBufferSample::CellSizeBytes, artifact.cellSizeBytes);
        samples->Observe(TopologyBufferSample::PolynomialOrderBytes, artifact.cellPolynomialOrderBytes);
        samples->Observe(TopologyBufferSample::CellTypeBytes, artifact.cellTypeBytes);
    }
    return encoded;
}

inline bool EncodeTopologyToTransferCache(
    const TopologyEncodeInput& input,
    TopologyEncodeResult& result,
    std::string* error = nullptr) {
    result = {};
    TopologyInputDescriptor descriptor;
    if (!input.data.adapter.DescribeTopology(descriptor, error)) {
        return false;
    }
    auto& topo = result.topo;
    topo.cellCount = descriptor.cellCount;
    topo.isStructured = false;
    topo.isPolyhedron = false;
    topo.hasCellTypes = false;
    topo.fixedCellSize = 0;

    int axisSize[3]{0, 0, 0};
    if (descriptor.structured) {
        if (!input.data.adapter.GetStructuredAxisSize(axisSize)) {
            return validation::AssignError(error, "structured topology descriptor is missing axis size");
        }
        topo.isStructured = true;
        result.structuredAxisSize = {axisSize[0], axisSize[1], axisSize[2]};
        result.transferCache = std::make_shared<bytestore::VectorByteSource>(std::vector<std::uint8_t>{});
        return true;
    }
    if (descriptor.cellCount == 0u) {
        result.transferCache = std::make_shared<bytestore::VectorByteSource>(std::vector<std::uint8_t>{});
        return true;
    }
    if (descriptor.polyhedron) {
        return validation::AssignError(error, "polyhedron topology requires the polyhedron topology encoder");
    }

    const auto cellCount = descriptor.cellCount;
    const auto pointCount = descriptor.pointCount;
    TopologyView topologyView;
    const auto hasTopologyView = input.data.adapter.BuildTopologyView(topologyView);
    const auto* indices =
        hasTopologyView &&
            topologyView.connectivity.indexType == IndexValueType::UInt32 &&
            topologyView.connectivity.IsCompact()
        ? static_cast<const IndexType*>(topologyView.connectivity.data)
        : input.data.adapter.GetCellIdBufferPtr();
    const auto* offsets =
        hasTopologyView &&
            !topologyView.fixedCellSizeEnabled &&
            topologyView.offsets.indexType == IndexValueType::UInt32 &&
            topologyView.offsets.IsCompact()
        ? static_cast<const IndexType*>(topologyView.offsets.data)
        : input.data.adapter.GetCellIdOffsetPtr();
    const auto* cellTypes =
        hasTopologyView &&
            topologyView.cellTypes.scalarType == ScalarType::UInt32 &&
            topologyView.cellTypes.IsCompact()
        ? static_cast<const IndexType*>(topologyView.cellTypes.data)
        : input.data.adapter.GetCellTypesPtr();
    const auto* cellPolynomialOrders =
        hasTopologyView &&
            topologyView.cellPolynomialOrders.scalarType == ScalarType::UInt16 &&
            topologyView.cellPolynomialOrders.IsCompact()
        ? static_cast<const std::uint16_t*>(topologyView.cellPolynomialOrders.data)
        : input.data.adapter.GetCellPolynomialOrdersPtr();
    const auto hasCellTypeView = hasTopologyView &&
        topologyView.cellTypes.tupleCount == cellCount &&
        topologyView.cellTypes.IsValid();
    const auto hasCellPolynomialOrderView = hasTopologyView &&
        topologyView.cellPolynomialOrders.tupleCount == cellCount &&
        topologyView.cellPolynomialOrders.IsValid();
    const auto hasCellTypes = TopologyValueSourceProvidesStream(descriptor.cellTypes);
    const auto hasCellPolynomialOrders = TopologyValueSourceProvidesStream(descriptor.cellPolynomialOrders);
    const auto usesTopologyConnectivity =
        hasTopologyView &&
        topologyView.connectivity.indexType == IndexValueType::UInt32 &&
        topologyView.connectivity.IsCompact();
    const auto connectivityCount = usesTopologyConnectivity
        ? topologyView.connectivity.count
        : descriptor.connectivityCount;
    const auto isFixedCellSize = descriptor.cellSize == TopologyCellSizeSource::FixedCellSize;
    if (indices == nullptr || (!isFixedCellSize && offsets == nullptr)) {
        return validation::AssignError(error, "missing compact connectivity or offset stream");
    }
    if (pointCount > static_cast<std::size_t>(std::numeric_limits<IndexType>::max()) + 1u) {
        return validation::AssignError(error, "topology point count exceeds index capacity");
    }

    topo.fixedCellSize = isFixedCellSize
        ? static_cast<int>(std::max(descriptor.fixedCellSize, 0))
        : 0;
    topo.cellBufferSize = connectivityCount;
    topo.hasCellTypes = hasCellTypes;

    if (topo.fixedCellSize > 0) {
        std::size_t expectedConnectivityCount = 0u;
        if (!validation::CheckedMulSizeT(
                cellCount,
                static_cast<std::size_t>(topo.fixedCellSize),
                expectedConnectivityCount,
                "topology fixed connectivity count",
                error) ||
            expectedConnectivityCount != connectivityCount) {
            return validation::AssignError(
                error,
                "topology fixed cell size does not match connectivity count");
        }
    }

    OrderedTopologyCellSource orderedCells;
    orderedCells.adapter = &input.data.adapter;
    orderedCells.indices = indices;
    orderedCells.offsets = offsets;
    orderedCells.cellTypes = cellTypes;
    orderedCells.cellPolynomialOrders = cellPolynomialOrders;
    orderedCells.cellTypesView = hasCellTypeView ? topologyView.cellTypes : NumericArrayView{};
    orderedCells.cellPolynomialOrdersView = hasCellPolynomialOrderView
        ? topologyView.cellPolynomialOrders
        : NumericArrayView{};
    orderedCells.pointCount = pointCount;
    orderedCells.cellCount = cellCount;
    orderedCells.connectivityCount = connectivityCount;
    orderedCells.fixedCellSize = topo.fixedCellSize;
    orderedCells.inversePointRemap = input.data.pointInverseOrderSource.Provider();
    orderedCells.cellOrderProvider = input.data.cellOrderSource.Provider();
    orderedCells.hasCellTypes = hasCellTypes;
    orderedCells.hasCellPolynomialOrders = hasCellPolynomialOrders;

    auto& resources = input.execution.resources;
    auto phase = WaitForHeavyPhase(resources);
    if (!phase) { return false; }
    std::shared_ptr<bytestore::MemoryStore> inversePointRemapOwner, cellOrderOwner;
    if (!PrepareTopologyRemapValues(orderedCells.inversePointRemap, pointCount,
            input.runtime.byteStoreSession, inversePointRemapOwner, orderedCells.inversePointRemapValues, error) ||
        !PrepareTopologyRemapValues(orderedCells.cellOrderProvider, cellCount,
            input.runtime.byteStoreSession, cellOrderOwner, orderedCells.cellOrderValues, error)) {
        return false;
    }
    const auto cellsPerBlock = static_cast<std::size_t>(input.execution.cellElementCount);
    if (cellsPerBlock == 0u) {
        return validation::AssignError(error, "topology cell block size is zero");
    }
    auto transferCache = input.runtime.byteStoreSession.CreateAppendableByteStore("topology_connectivity", error);
    bytestore::AppendableByteStoreWriter transferWriter(transferCache, resources);
    if (!transferCache) { return false; }
    auto& layouts = topo.connectivityLayout.blockLayouts;
    layouts.reserve(1u + (cellCount - 1u) / cellsPerBlock);
    InvokeConnectivityProgress(input, 0.30);
    phase.reset();

    std::size_t nextCell = 0u;
    std::size_t committedCells = 0u;
    std::size_t connectivityOffset = 0u;
    resources.SetWorkType({.path = ResourceWorkPath::ConnectivityEncode,
        .blockElements = cellsPerBlock});
    const bool success = RunOrderedBlocks<TopologyBlockInput, TopologyBlockEncodeArtifact>(
        resources,
        [&] { return nextCell < cellCount; },
        [&](TopologyBlockInput& block) {
            const auto n = std::min(cellsPerBlock, cellCount - nextCell);
            const OrderedTopologyCellRangeSource range{&orderedCells, nextCell, n};
            if (input.context.recordCapacitySamples) { block.capacitySamples.emplace(); }
            if (!ReadTopologyBlock(range, topo.fixedCellSize, block, error)) { return false; }
            nextCell += n;
            return true;
        },
        [&](const TopologyBlockInput& block, TopologyBlockEncodeArtifact& artifact, WorkerContext&) {
            std::string localError;
            if (!EncodeTopologyBlock(block, pointCount, topo.fixedCellSize, artifact, &localError)) {
                resources.RecordFailure(MakeCodecFailureRecord(CodecErrorCode::EncodeFailure,
                    "topology-block-encode", "EncodeTopologyBlock", localError));
                return false;
            }
            return true;
        },
        [&](const TopologyBlockEncodeArtifact& artifact) {
            if (artifact.firstCell != committedCells || artifact.cellCount > cellCount - committedCells ||
                artifact.connectivityCount > connectivityCount - connectivityOffset) {
                return validation::AssignError(error, "topology block does not match the next commit range");
            }
            TopologyConnectivityBlockLayoutParams layout{
                .cellOffset = artifact.firstCell, .cellCount = artifact.cellCount};
            layout.connectivityOffset = connectivityOffset;
            layout.connectivityCount = artifact.connectivityCount;
            layout.connectivityByteCount = artifact.connectivityBytes.size();
            layout.cellSizeByteCount = artifact.cellSizeBytes.size();
            layout.cellPolynomialOrderByteCount = artifact.cellPolynomialOrderBytes.size();
            layout.cellTypeByteCount = artifact.cellTypeBytes.size();
            // 一个块的四条流依格式顺序消费，提交期间仍持有原槽位
            for (const auto bytes : {std::span<const std::uint8_t>(artifact.connectivityBytes),
                     std::span<const std::uint8_t>(artifact.cellSizeBytes),
                     std::span<const std::uint8_t>(artifact.cellPolynomialOrderBytes),
                     std::span<const std::uint8_t>(artifact.cellTypeBytes)}) {
                for (std::size_t offset = 0u; offset < bytes.size();) {
                    const auto n = std::min<std::size_t>(kIoWindowBytes, bytes.size() - offset);
                    if (!transferWriter.Write(bytes.subspan(offset, n), error)) { return false; }
                    offset += n;
                }
            }
            layouts.push_back(layout);
            connectivityOffset += artifact.connectivityCount;
            committedCells += artifact.cellCount;
            if (committedCells == cellCount) {
                if (connectivityOffset != connectivityCount) {
                    return validation::AssignError(error, "topology block connectivity total does not match the source");
                }
                if (!transferCache->Seal(error)) { return false; }
            }
            if (artifact.capacitySamples && input.context.recordCapacitySamples) {
                try { input.context.recordCapacitySamples(artifact.capacitySamples->values); }
                catch (...) { resources.RecordDiagnosticExportFailure(); }
            }
            InvokeConnectivityProgress(input, 0.30 + 0.68 * static_cast<double>(committedCells) / cellCount);
            return true;
        });
    if (!success) { return false; }
    result.transferCache = std::move(transferCache);
    topo.binaryCount = result.transferCache->ByteSizeHint();
    return true;
}

} // namespace datacodec::topocodec

#endif
