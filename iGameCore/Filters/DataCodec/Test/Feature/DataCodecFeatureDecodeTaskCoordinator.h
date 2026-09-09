#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREDECODETASKCOORDINATOR_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREDECODETASKCOORDINATOR_H

#include "DataCodec/Workflow/Task/DecodeTaskCoordinator.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <mutex>
#include <vector>

namespace datacodec::test::feature_decode_task_coordinator {

struct Result {
    bool success{false};
    bool cancelled{false};
    std::optional<CodecFailureRecord> failure;
    unsigned value{0u};
};
using Coordinator = DecodeTaskCoordinator<Result>;

inline bool TestDuplicateTargetUsesSingleTask() {
    DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 2u, 3u}, 64u, 2u, true, true});
    Coordinator coordinator(run);
    std::latch started(1), release(1);
    std::atomic_uint count{0u};
    auto first = coordinator.Submit({.frameIndex = 7u}, [&](std::stop_token) {
        ++count;
        started.count_down();
        release.wait();
        return Result{.success = true, .value = 42u};
    });
    started.wait();
    auto second = coordinator.Submit({.frameIndex = 7u}, [](std::stop_token) {
        return Result{.success = true, .value = 0u};
    });
    first = {};
    release.count_down();
    Result result;
    const bool completed = second.Wait(result);
    coordinator.WaitIdle();
    return completed && result.success && result.value == 42u && count == 1u;
}

inline bool TestReleasedHandleStillJoinsTaskOnDriver() {
    DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 2u}, 64u, 2u, true, true});
    Coordinator coordinator(run);
    std::mutex mutex;
    std::condition_variable_any condition;
    std::latch started(1);
    auto handle = coordinator.Submit({.frameIndex = 12u}, [&](std::stop_token stop) {
        started.count_down();
        std::unique_lock lock(mutex);
        condition.wait(lock, stop, [] { return false; });
        return Result{.cancelled = true};
    });
    started.wait();
    handle = {};
    bool cleanupAllocated = false;
    {
        RejectAllocationsScope reject;
        coordinator.WaitIdle();
        cleanupAllocated = rejectedAllocationCount != 0u;
    }
    auto next = coordinator.Submit({.frameIndex = 13u}, [](std::stop_token) {
        return Result{.success = true, .value = 13u};
    });
    Result result;
    const bool completed = next.Wait(result);
    coordinator.WaitIdle();
    return !cleanupAllocated && completed && result.success && result.value == 13u &&
        coordinator.InFlightTaskCount() == 0u;
}

inline bool TestCommandWindowAndOrder() {
    DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 2u, 3u}, 64u, 2u, true, true});
    Coordinator coordinator(run);
    std::latch started(1), release(1);
    std::vector<unsigned> order;
    auto first = coordinator.Submit({.frameIndex = 1u}, [&](std::stop_token) {
        started.count_down();
        release.wait();
        order.push_back(1u);
        return Result{.success = true, .value = 1u};
    });
    started.wait();
    auto second = coordinator.Submit({.frameIndex = 2u}, [&](std::stop_token) {
        order.push_back(2u);
        return Result{.success = true, .value = 2u};
    });
    auto third = coordinator.Submit({.frameIndex = 3u}, [](std::stop_token) {
        return Result{.success = true, .value = 3u};
    });
    auto prefetch = coordinator.Submit({.frameIndex = 4u}, [&](std::stop_token) {
        order.push_back(4u);
        return Result{.success = true, .value = 4u};
    }, DecodeCommandKind::Prefetch);
    Result rejected;
    const bool rejectedCorrectly = third.Wait(rejected) && rejected.failure &&
        std::string_view(rejected.failure->reason.data()) == "command-window-full";
    const auto inFlight = coordinator.InFlightTaskCount();
    release.count_down();
    Result result;
    const bool complete = first.Wait(result) && second.Wait(result) && prefetch.Wait(result);
    coordinator.WaitIdle();
    return rejectedCorrectly && inFlight == 3u && complete &&
        order == std::vector<unsigned>({1u, 2u, 4u});
}

inline bool TestCancelAllDoesNotAllocate() {
    DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 2u}, 64u, 2u, true, true});
    Coordinator coordinator(run);
    std::mutex mutex;
    std::condition_variable_any condition;
    std::latch started(1);
    auto current = coordinator.Submit({.frameIndex = 1u}, [&](std::stop_token stop) {
        started.count_down();
        std::unique_lock lock(mutex);
        condition.wait(lock, stop, [] { return false; });
        return Result{.cancelled = true};
    });
    started.wait();
    auto queued = coordinator.Submit({.frameIndex = 2u}, [](std::stop_token) {
        return Result{.success = true};
    });
    bool noAllocations = false;
    {
        RejectAllocationsScope reject;
        coordinator.CancelAll();
        coordinator.WaitIdle();
        noAllocations = rejectedAllocationCount == 0u;
    }
    Result first, second;
    return noAllocations && current.Wait(first) && queued.Wait(second) &&
        first.cancelled && second.cancelled;
}

}

namespace datacodec::test {
inline int RunDataCodecFeatureDecodeTaskCoordinator() {
    using namespace feature_decode_task_coordinator;
    if (!TestDuplicateTargetUsesSingleTask() || !TestReleasedHandleStillJoinsTaskOnDriver() ||
        !TestCommandWindowAndOrder() || !TestCancelAllDoesNotAllocate()) {
        std::cerr << "DataCodec decode task coordinator feature test failed\n";
        return 1;
    }
    std::cout << "DataCodec decode task coordinator feature test passed\n";
    return 0;
}
}

#endif
