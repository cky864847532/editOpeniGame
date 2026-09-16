#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURETOPOLOGYOBSERVER_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURETOPOLOGYOBSERVER_H

#include "DataCodec/Codec/Topology/IDecodeTopologyBlockObserver.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

namespace datacodec::test {

inline DecodedConnectivityTopologyBlock MakeTopologyObserverTestBlock(std::size_t index) {
    auto owner = std::make_unique<std::uint8_t[]>(5u * sizeof(IndexType));
    auto* values = reinterpret_cast<IndexType*>(owner.get());
    for (std::size_t i = 0u; i < 4u; ++i) { values[i] = static_cast<IndexType>(i); }
    values[4] = 10u;
    return {.blockIndex = index, .cellOffset = index, .fixedCellSize = 4,
        .owner = std::move(owner), .connectivity = {values, 4u}, .cellTypes = {values + 4u, 1u}};
}

inline TestResult RunDataCodecFeatureTopologyObserver() {
    TestResult result;
    for (const bool failBlock : {false, true}) {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, true, true});
        run.BeginRun();
        std::size_t next = 0u;
        std::size_t consumed = 0u;
        bool slotHeldDuringConsumption = true;
        const bool success = RunOrderedBlocks<std::size_t, DecodedConnectivityTopologyBlock>(run,
            [&] { return next < 3u; },
            [&](std::size_t& index) { index = next++; return true; },
            [](std::size_t index, DecodedConnectivityTopologyBlock& block, WorkerContext&) {
                block = MakeTopologyObserverTestBlock(index);
                return true;
            },
            [&](DecodedConnectivityTopologyBlock& block) {
                // S=1 时观察者消费完毕前不能继续读入下一块
                slotHeldDuringConsumption &= !run.TryAcquireSlot() && next == consumed + 1u;
                if (failBlock) { return false; }
                if (block.blockIndex != consumed || block.connectivity.size() != 4u) { return false; }
                ++consumed;
                return true;
            });
        Require(result, success == !failBlock && slotHeldDuringConsumption &&
            consumed == (failBlock ? 0u : 3u) && run.EndRun(),
            failBlock ? "topologyObserver.failure-drain" : "topologyObserver.consume-before-retire",
            "topology output must remain in its admitted slot until synchronous consumption completes");
        if (failBlock) {
            const auto failure = run.FirstFailure();
            Require(result, failure && std::string_view(failure->reason.data()) == "block-commit-failed" && next == 1u,
                "topologyObserver.first-failure", "observer failure must stop further reads and preserve the first failure");
        }
    }
    return result;
}

}

#endif
