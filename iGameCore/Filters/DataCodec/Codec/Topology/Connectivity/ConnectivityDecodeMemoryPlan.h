#ifndef DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYDECODEMEMORYPLAN_H
#define DATACODEC_CODEC_TOPOLOGY_CONNECTIVITY_CONNECTIVITYDECODEMEMORYPLAN_H

#include "DataCodec/API/Params/CodecStorageParams.h"
#include "DataCodec/Runtime/Execution/DecodeBlockWorkspace.h"
#include "DataCodec/Storage/ByteIO/ArrayWorkspace.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"

namespace datacodec::topocodec {

struct ConnectivityDecodeMemoryLayout : DecodeBlockMemoryPlan {
    std::array<DecodeMemoryRange, 4u> input;
    DecodeMemoryRange connectivity, offsets, types, orders, scratch;
};
inline ConnectivityDecodeMemoryLayout MakeConnectivityDecodeMemoryLayout(
    const TopologyConnectivityBlockLayoutParams& block, int fixedCellSize, bool hasTypes) {
    using Region = DecodeMemoryRegion;
    ConnectivityDecodeMemoryLayout result;
    result.block = block.cellOffset;
    const std::array<std::uint64_t, 4u> input{block.connectivityByteCount, block.cellSizeByteCount,
        block.cellPolynomialOrderByteCount, block.cellTypeByteCount};
    const auto local = [](std::uint64_t value) {
        if (value > std::numeric_limits<std::size_t>::max()) { throw std::length_error("topology memory shape exceeds platform"); }
        return static_cast<std::size_t>(value);
    };
    const auto cells = local(block.cellCount);
    for (std::size_t i = 0u; i < input.size(); ++i) { result.input[i] = result.Append<std::uint8_t>(Region::Input, local(input[i])); }
    result.connectivity = result.Append<IndexType>(Region::Output, local(block.connectivityCount));
    result.offsets = result.Append<IndexType>(Region::Output, fixedCellSize <= 0 ? DecodeBlockMemoryPlan::Add(cells, 1u) : 0u);
    result.types = result.Append<IndexType>(Region::Output, hasTypes ? cells : 0u);
    result.orders = result.Append<std::uint16_t>(Region::Output, block.cellPolynomialOrderByteCount ? cells : 0u);
    DecodeBlockMemoryPlan temporary;
    temporary.Append<IndexType>(Region::Temporary, fixedCellSize <= 0 ? cells : 0u);
    temporary.Append<std::size_t>(Region::Temporary, DecodeBlockMemoryPlan::Add(cells, 1u));
    const auto commit = fixedCellSize <= 0 ? DecodeBlockMemoryPlan::Multiply(sizeof(IndexType),
        std::min<std::size_t>(kIoWindowBytes / sizeof(IndexType), DecodeBlockMemoryPlan::Add(cells, 1u))) : 0u;
    result.scratch = result.Append<std::uint8_t>(Region::Temporary, std::max(temporary.regionBytes[2], commit));
    (void)result.TotalBytes();
    return result;
}

struct PreparedConnectivityDecodedBlock {
    FixedArrayView<IndexType> connectivity, offsets, cellTypes;
    FixedArrayView<std::uint16_t> polynomialOrders;
};

}
#endif
