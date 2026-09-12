#ifndef DATACODEC_API_ADAPTER_IDECODETOPOLOGYBLOCKOBSERVER_H
#define DATACODEC_API_ADAPTER_IDECODETOPOLOGYBLOCKOBSERVER_H

#include "DataCodec/Common/DataCodecTypes.h"

#include <cstddef>
#include <string>
#include <memory>
#include <span>

namespace datacodec {

struct ConnectivityTopologyDecodeInfo {
    std::size_t blockCount{0u};
    std::size_t pointCount{0u};
    std::size_t cellCount{0u};
    int fixedCellSize{0};
    bool hasOffsets{false};
    bool hasCellTypes{false};
};

struct DecodedConnectivityTopologyBlock {
    std::size_t blockIndex{0u};
    std::size_t cellOffset{0u};
    int fixedCellSize{0};
    std::unique_ptr<std::uint8_t[]> owner;
    std::span<const IndexType> connectivity;
    std::span<const IndexType> offsets;
    std::span<const IndexType> cellTypes;
};

class IDecodeTopologyBlockObserver {
public:
    virtual ~IDecodeTopologyBlockObserver() = default;

    virtual bool BeginConnectivityTopology(
        const ConnectivityTopologyDecodeInfo& info,
        std::string* error = nullptr) = 0;
    // 同步消费当前块，按值移出的数组归调用方所有
    // 调用方自行创建的后台队列不属于 DataCodec 的在途资源范围
    virtual bool ObserveConnectivityBlock(
        DecodedConnectivityTopologyBlock block,
        std::string* error = nullptr) = 0;
    virtual bool EndConnectivityTopology(std::string* error = nullptr) = 0;
};

} // namespace datacodec

#endif
