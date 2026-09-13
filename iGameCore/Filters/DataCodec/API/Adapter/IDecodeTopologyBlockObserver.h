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
    // 非零表示稳定索引范围，零表示索引按需增长；同一索引不并发重入
    std::size_t workerCapacity{1u};
};

struct DecodedConnectivityTopologyBlock {
    std::size_t blockIndex{0u};
    std::size_t cellOffset{0u};
    int fixedCellSize{0};
    std::unique_ptr<std::uint8_t[]> owner;
    std::span<const IndexType> connectivity;
    std::span<const IndexType> offsets;
    std::span<const IndexType> cellTypes;
    std::size_t workerIndex{0u};
};

class IDecodeTopologyBlockObserver {
public:
    virtual ~IDecodeTopologyBlockObserver() = default;

    // 并发观察者在块计算额度内消费借用视图，调用返回前必须完成消费
    // 同一 worker 不重入，块间允许乱序，Begin 和 End 由 driver 在任务收束边界调用
    virtual bool SupportsConcurrentBlocks() const noexcept { return false; }

    virtual bool BeginConnectivityTopology(
        const ConnectivityTopologyDecodeInfo& info,
        std::string* error = nullptr) = 0;
    // 默认由 driver 顺序消费并移交 owner，并发模式借用当前槽位存储且 owner 为空
    // 当前槽位保持到消费完成，并发观察者不得保存视图或创建后台子任务
    virtual bool ObserveConnectivityBlock(
        DecodedConnectivityTopologyBlock block,
        std::string* error = nullptr) = 0;
    virtual bool EndConnectivityTopology(std::string* error = nullptr) = 0;
};

} // namespace datacodec

#endif
