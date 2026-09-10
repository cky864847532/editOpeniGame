#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCETASKRUNNER_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURERESOURCETASKRUNNER_H

#include "DataCodec/Runtime/Execution/DataCodecResourceTaskRunner.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace datacodec::test {

namespace resource_task_runner_test {

struct BlockingTaskState {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t startedTaskCount{0u};
    bool release{false};
};

inline bool WaitForStartedTasks(
    BlockingTaskState& state,
    const std::size_t expected) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.changed.wait_for(
        lock,
        std::chrono::seconds(2),
        [&state, expected]() {
            return state.startedTaskCount >= expected;
        });
}

inline void ReleaseTasks(BlockingTaskState& state) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.changed.notify_all();
}

inline std::function<void()> MakeBlockingTask(BlockingTaskState& state) {
    return [&state]() {
        std::unique_lock<std::mutex> lock(state.mutex);
        ++state.startedTaskCount;
        state.changed.notify_all();
        state.changed.wait(lock, [&state]() {
            return state.release;
        });
    };
}

inline void TestMemoryLimitAndFill(TestResult& result) {
    DataCodecResourceTaskRunner runner(3u, 80u);
    auto group = runner.CreateGroup();
    BlockingTaskState state;
    for (std::size_t index = 0u; index < 4u; ++index) {
        group->Submit(
            MakeBlockingTask(state),
            DataCodecTaskResourceClaim{.memoryBytes = 40u});
    }
    const auto started = WaitForStartedTasks(state, 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto runningStats = runner.SnapshotStats();
    std::size_t startedTaskCount = 0u;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        startedTaskCount = state.startedTaskCount;
    }
    Require(
        result,
        started,
        "resourceRunner.memory.started",
        "memory-limited tasks did not start");
    Require(
        result,
        startedTaskCount == 2u,
        "resourceRunner.memory.concurrent",
        "memory limit did not hold concurrent tasks at two");
    Require(
        result,
        runningStats.reservedMemoryBytes == 80u &&
            runningStats.runningTaskCount == 2u,
        "resourceRunner.memory.fill",
        "memory-limited schedule did not fill the available budget");
    ReleaseTasks(state);
    group->Wait();
    const auto completedStats = runner.SnapshotStats();
    Require(
        result,
        completedStats.peakReservedMemoryBytes == 80u &&
            completedStats.reservedMemoryBytes == 0u,
        "resourceRunner.memory.peak",
        "memory accounting exceeded the limit or was not released");
}

inline void TestWorkerLimitAndFill(TestResult& result) {
    DataCodecResourceTaskRunner runner(2u, 1024u);
    auto group = runner.CreateGroup();
    BlockingTaskState state;
    for (std::size_t index = 0u; index < 4u; ++index) {
        group->Submit(
            MakeBlockingTask(state),
            DataCodecTaskResourceClaim{.memoryBytes = 1u});
    }
    const auto started = WaitForStartedTasks(state, 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto runningStats = runner.SnapshotStats();
    std::size_t startedTaskCount = 0u;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        startedTaskCount = state.startedTaskCount;
    }
    Require(
        result,
        started,
        "resourceRunner.workers.started",
        "worker-limited tasks did not start");
    Require(
        result,
        startedTaskCount == 2u && runningStats.runningTaskCount == 2u,
        "resourceRunner.workers.concurrent",
        "worker limit did not hold and fill two worker slots");
    ReleaseTasks(state);
    group->Wait();
    const auto completedStats = runner.SnapshotStats();
    Require(
        result,
        completedStats.peakRunningTaskCount == 2u,
        "resourceRunner.workers.peak",
        "worker peak did not match the configured limit");
}

inline void TestMixedClaimsPreferRunnablePair(TestResult& result) {
    DataCodecResourceTaskRunner runner(2u, 80u);
    auto submissionGate = runner.AcquireMemoryReservation(80u);
    auto group = runner.CreateGroup();
    BlockingTaskState state;
    group->Submit(
        MakeBlockingTask(state),
        DataCodecTaskResourceClaim{.memoryBytes = 50u});
    group->Submit(
        MakeBlockingTask(state),
        DataCodecTaskResourceClaim{.memoryBytes = 40u});
    group->Submit(
        MakeBlockingTask(state),
        DataCodecTaskResourceClaim{.memoryBytes = 40u});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    submissionGate.Release();
    const auto started = WaitForStartedTasks(state, 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto stats = runner.SnapshotStats();
    Require(
        result,
        started && stats.runningTaskCount == 2u &&
            stats.reservedMemoryBytes == 80u,
        "resourceRunner.mixedClaims.fill",
        "mixed task claims did not select the runnable pair that fills both workers");
    ReleaseTasks(state);
    group->Wait();
}

inline void TestOversizedClaimRejected(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    auto group = runner.CreateGroup();
    bool rejected = false;
    try {
        group->Submit(
            []() {},
            DataCodecTaskResourceClaim{.memoryBytes = 65u});
    } catch (const DataCodecTaskClaimExceedsLimit&) {
        rejected = true;
    }
    Require(
        result,
        rejected,
        "resourceRunner.memory.oversized",
        "oversized task memory claim was accepted");
    group->Wait();
}

inline void TestNestedWaitMakesProgress(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    auto outerGroup = runner.CreateGroup();
    std::atomic_bool innerExecuted{false};
    outerGroup->Submit([&]() {
        auto innerGroup = runner.CreateGroup();
        innerGroup->Submit(
            [&innerExecuted]() {
                innerExecuted.store(true, std::memory_order_release);
            },
            DataCodecTaskResourceClaim{.memoryBytes = 64u});
        innerGroup->Wait();
    }, DataCodecTaskResourceClaim{.memoryBytes = 64u});
    outerGroup->Wait();
    Require(
        result,
        innerExecuted.load(std::memory_order_acquire) &&
            runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.nested.progress",
        "nested task did not borrow and release its waiting parent claim");
}

inline void TestNestedRetainedReservationTransfersToParentClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    std::optional<DataCodecTaskMemoryReservation> retainedReservation;
    auto outerGroup = runner.CreateGroup();
    outerGroup->Submit(
        [&]() {
            auto innerGroup = runner.CreateGroup();
            innerGroup->Submit(
                [&]() {
                    retainedReservation.emplace(
                        RetainCurrentDataCodecTaskMemoryReservation(16u));
                },
                DataCodecTaskResourceClaim{.memoryBytes = 64u});
            innerGroup->Wait();
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    outerGroup->Wait();
    Require(
        result,
        retainedReservation.has_value() &&
            retainedReservation->Bytes() == 16u &&
            runner.SnapshotStats().reservedMemoryBytes == 16u,
        "resourceRunner.nested.retained",
        "nested retained bytes were not transferred to the parent claim");
    retainedReservation.reset();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.nested.retainedRelease",
        "nested retained bytes were not released with their result");
}

inline void TestMultiLevelRetainedReservationTransfersToRootClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    std::optional<DataCodecTaskMemoryReservation> retainedReservation;
    auto outerGroup = runner.CreateGroup();
    outerGroup->Submit(
        [&]() {
            auto middleGroup = runner.CreateGroup();
            middleGroup->Submit(
                [&]() {
                    auto innerGroup = runner.CreateGroup();
                    innerGroup->Submit(
                        [&]() {
                            retainedReservation.emplace(
                                RetainCurrentDataCodecTaskMemoryReservation(16u));
                        },
                        DataCodecTaskResourceClaim{.memoryBytes = 64u});
                    innerGroup->Wait();
                },
                DataCodecTaskResourceClaim{.memoryBytes = 64u});
            middleGroup->Wait();
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    outerGroup->Wait();
    Require(
        result,
        retainedReservation.has_value() &&
            retainedReservation->Bytes() == 16u &&
            runner.SnapshotStats().reservedMemoryBytes == 16u,
        "resourceRunner.nested.multiLevelRetained",
        "multi-level retained bytes were not transferred to the root claim");
    retainedReservation.reset();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.nested.multiLevelRelease",
        "multi-level retained bytes were not released through the parent chain");
}

inline void TestCancelledTaskBypassesUnavailableMemory(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    auto occupiedMemory = runner.AcquireMemoryReservation(64u);
    std::stop_source stopSource;
    auto group = runner.CreateGroup(stopSource.get_token());
    std::atomic_bool executed{false};
    group->Submit(
        [&]() {
            executed.store(true, std::memory_order_release);
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    stopSource.request_stop();
    group->Wait();
    const auto stats = runner.SnapshotStats();
    Require(
        result,
        !executed.load(std::memory_order_acquire) &&
            stats.queuedTaskCount == 0u &&
            stats.reservedMemoryBytes == 64u,
        "resourceRunner.cancelled.memoryBypass",
        "cancelled task remained blocked behind unavailable memory");
    occupiedMemory.Release();
}

inline void TestTaskExceptionReleasesClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    auto group = runner.CreateGroup();
    group->Submit(
        []() {
            throw std::runtime_error("expected resource task failure");
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    bool exceptionObserved = false;
    try {
        group->Wait();
    } catch (const std::runtime_error&) {
        exceptionObserved = true;
    }
    const auto stats = runner.SnapshotStats();
    Require(
        result,
        exceptionObserved &&
            stats.runningTaskCount == 0u &&
            stats.queuedTaskCount == 0u &&
            stats.reservedMemoryBytes == 0u,
        "resourceRunner.exception.release",
        "task exception did not release its worker and memory claim");
}

inline void TestShutdownReleasesRejectedTaskCaptures(TestResult& result) {
    auto runner = std::make_unique<DataCodecResourceTaskRunner>(1u, 64u);
    auto activeGroup = runner->CreateGroup();
    BlockingTaskState state;
    activeGroup->Submit(MakeBlockingTask(state));
    const auto activeStarted = WaitForStartedTasks(state, 1u);

    auto queuedGroup = runner->CreateGroup();
    auto capturedReservation = std::make_shared<DataCodecTaskMemoryReservation>(
        runner->AcquireMemoryReservation(64u));
    std::weak_ptr<DataCodecTaskMemoryReservation> capturedReservationWeak =
        capturedReservation;
    queuedGroup->Submit([capturedReservation]() {});
    capturedReservation.reset();

    std::thread shutdownThread([&runner]() {
        runner.reset();
    });
    const auto releaseDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!capturedReservationWeak.expired() &&
           std::chrono::steady_clock::now() < releaseDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto captureReleasedBeforeJoin = capturedReservationWeak.expired();
    ReleaseTasks(state);
    shutdownThread.join();
    activeGroup->Wait();
    bool queuedRejected = false;
    try {
        queuedGroup->Wait();
    } catch (const std::runtime_error&) {
        queuedRejected = true;
    }
    Require(
        result,
        activeStarted && captureReleasedBeforeJoin && queuedRejected,
        "resourceRunner.shutdown.captureRelease",
        "runner shutdown retained a rejected task capture until active workers joined");
}

inline void TestTransferredReservationControlsFollowingTask(TestResult& result) {
    DataCodecResourceTaskRunner runner(2u, 64u);
    std::optional<DataCodecTaskMemoryReservation> retainedReservation;
    auto firstGroup = runner.CreateGroup();
    firstGroup->Submit(
        [&retainedReservation]() {
            retainedReservation.emplace(
                RetainCurrentDataCodecTaskMemoryReservation());
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    firstGroup->Wait();
    Require(
        result,
        retainedReservation.has_value() &&
            retainedReservation->Bytes() == 64u &&
            runner.SnapshotStats().reservedMemoryBytes == 64u,
        "resourceRunner.reservation.retained",
        "task memory reservation was released before its result");

    std::atomic_bool secondStarted{false};
    auto secondGroup = runner.CreateGroup();
    secondGroup->Submit(
        [&secondStarted]() {
            secondStarted.store(true, std::memory_order_release);
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(
        result,
        !secondStarted.load(std::memory_order_acquire),
        "resourceRunner.reservation.blocks",
        "retained task memory did not block a following claim");
    retainedReservation.reset();
    secondGroup->Wait();
    Require(
        result,
        secondStarted.load(std::memory_order_acquire) &&
            runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.reservation.release",
        "released task memory did not unblock the following claim");
}

inline void TestPartialReservationReleasesFinishedWorkspace(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    std::optional<DataCodecTaskMemoryReservation> retainedReservation;
    auto group = runner.CreateGroup();
    group->Submit(
        [&retainedReservation]() {
            retainedReservation.emplace(
                RetainCurrentDataCodecTaskMemoryReservation(16u));
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    group->Wait();
    const auto retainedStats = runner.SnapshotStats();
    Require(
        result,
        retainedReservation.has_value() &&
            retainedReservation->Bytes() == 16u &&
            retainedStats.reservedMemoryBytes == 16u,
        "resourceRunner.reservation.partial",
        "finished task workspace was not released after retaining result bytes");
    retainedReservation.reset();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.reservation.partialRelease",
        "partial retained reservation was not released");
}

inline void TestReservationShrinkReleasesUnusedCapacity(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 96u);
    auto reservation = runner.AcquireMemoryReservation(96u);
    reservation.ShrinkTo(24u);
    const auto retainedStats = runner.SnapshotStats();
    Require(
        result,
        retainedStats.reservedMemoryBytes == 24u &&
            reservation.Bytes() == 24u,
        "resourceRunner.reservation.shrink",
        "shrinking a retained reservation did not release unused capacity");
    reservation.Release();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.reservation.shrinkRelease",
        "shrunk reservation did not release its remaining capacity");
}

inline void TestNestedDirectReservationMakesProgress(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    auto outerGroup = runner.CreateGroup();
    std::atomic_bool secondReservationAcquired{false};
    outerGroup->Submit([&]() {
        auto firstReservation = std::make_shared<DataCodecTaskMemoryReservation>(
            runner.AcquireMemoryReservation(64u));
        auto releaseGroup = runner.CreateGroup();
        releaseGroup->Submit([firstReservation]() {
            firstReservation->Release();
        });
        auto secondReservation = runner.AcquireMemoryReservation(64u);
        secondReservationAcquired.store(true, std::memory_order_release);
        secondReservation.Release();
        releaseGroup->Wait();
    });
    outerGroup->Wait();
    Require(
        result,
        secondReservationAcquired.load(std::memory_order_acquire) &&
            runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.reservation.nestedProgress",
        "single-worker direct memory reservation wait did not execute queued work");
}

inline void TestByteStoreTransfersTaskClaims(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    bytestore::ByteStoreSession session;
    session.ConfigureResourceTaskRunner(&runner);
    auto firstStore = session.CreateMemoryStore(true);
    auto secondStore = session.CreateMemoryStore(true);
    std::array<std::uint8_t, 24u> bytes{};
    std::atomic_bool storesWritten{false};
    auto group = runner.CreateGroup();
    group->Submit(
        [&]() {
            std::string error;
            storesWritten.store(
                firstStore->Append(bytes, &error) &&
                    firstStore->Seal(&error) &&
                    secondStore->Append(bytes, &error) &&
                    secondStore->Seal(&error),
                std::memory_order_release);
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    group->Wait();
    const auto retainedStats = runner.SnapshotStats();
    const auto storeStats = session.SnapshotStats();
    Require(
        result,
        storesWritten.load(std::memory_order_acquire) &&
            retainedStats.reservedMemoryBytes == 48u,
        "resourceRunner.byteStore.retained",
        "memory stores did not retain their capacity from the current task claim");
    Require(
        result,
        storeStats.residentBytes == 48u &&
            storeStats.peakResidentBytes == 48u &&
            storeStats.residentLimitBytes == 64u,
        "resourceRunner.byteStore.telemetry",
        "memory store telemetry did not report unified resident reservations");
    session.ReleaseAll();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.byteStore.release",
        "memory store destruction did not release unified resident reservations");
}

inline void TestByteStoreSplitsDirectReservation(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    bytestore::ByteStoreSession session;
    session.ConfigureResourceTaskRunner(&runner);
    auto firstStore = session.CreateMemoryStore(true);
    auto secondStore = session.CreateMemoryStore(true);
    std::array<std::uint8_t, 24u> bytes{};
    auto reservation = runner.AcquireMemoryReservation(64u);
    bool written = false;
    {
        DataCodecResidentMemoryReservationScope scope(reservation);
        std::string error;
        written = firstStore->Append(bytes, &error) &&
            firstStore->Seal(&error) &&
            secondStore->Append(bytes, &error) &&
            secondStore->Seal(&error);
    }
    Require(
        result,
        written && reservation.Bytes() == 16u &&
            runner.SnapshotStats().reservedMemoryBytes == 64u,
        "resourceRunner.byteStore.split",
        "memory stores did not split capacity from a direct reservation");
    reservation.Release();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 48u,
        "resourceRunner.byteStore.splitRemainder",
        "direct reservation remainder was not released independently");
    session.ReleaseAll();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.byteStore.splitRelease",
        "split memory store reservations were not released");
}

inline void TestByteStoreUsesAvailableGrowthCapacity(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 4096u);
    bytestore::ByteStoreSession session;
    session.ConfigureResourceTaskRunner(&runner);
    auto store = session.CreateMemoryStore();
    std::array<std::uint8_t, 1024u> initialBytes{};
    const std::array<std::uint8_t, 1u> appendedByte{};
    std::string error;
    const auto written = store->Append(initialBytes, &error) &&
        store->Append(appendedByte, &error);
    Require(
        result,
        written && store->ByteSizeHint() == 1025u &&
            store->ResidentSizeHint() == 1536u &&
            runner.SnapshotStats().reservedMemoryBytes == 1536u,
        "resourceRunner.byteStore.growth",
        "memory store did not use available resource capacity for geometric growth");
    session.ReleaseAll();
}

inline void TestByteStoreGrowthFallsBackToRequiredBytes(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 1100u);
    bytestore::ByteStoreSession session;
    session.ConfigureResourceTaskRunner(&runner);
    auto store = session.CreateMemoryStore();
    std::array<std::uint8_t, 1024u> initialBytes{};
    const std::array<std::uint8_t, 1u> appendedByte{};
    std::atomic_bool written{false};
    auto group = runner.CreateGroup();
    group->Submit(
        [&]() {
            std::string error;
            written.store(
                store->Append(initialBytes, &error) &&
                    store->Append(appendedByte, &error),
                std::memory_order_release);
        },
        DataCodecTaskResourceClaim{.memoryBytes = 1100u});
    group->Wait();
    Require(
        result,
        written.load(std::memory_order_acquire) &&
            store->ResidentSizeHint() == 1025u &&
            runner.SnapshotStats().reservedMemoryBytes == 1025u,
        "resourceRunner.byteStore.requiredGrowth",
        "optional geometric growth prevented required memory store progress");
    session.ReleaseAll();
}

inline void TestByteStoreReservationOutlivesRunnerOwner(TestResult& result) {
    bytestore::ByteStoreSession session;
    std::shared_ptr<bytestore::MemoryStore> store;
    {
        auto runner = std::make_unique<DataCodecResourceTaskRunner>(1u, 64u);
        session.ConfigureResourceTaskRunner(runner.get());
        store = session.CreateMemoryStore();
        std::array<std::uint8_t, 24u> bytes{};
        std::string error;
        Require(
            result,
            store->Append(bytes, &error),
            "resourceRunner.byteStore.ownerLifetimeWrite",
            "memory store setup failed before runner owner release");
    }
    store->Release();
    Require(
        result,
        store->ResidentSizeHint() == 0u,
        "resourceRunner.byteStore.ownerLifetimeRelease",
        "memory store could not release reservations after runner owner destruction");
    session.ReleaseAll();
}

inline void TestScratchPoolUsesTaskClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    std::size_t acquiredCapacity = 0u;
    auto group = runner.CreateGroup();
    group->Submit(
        [&]() {
            auto buffer = pool.Acquire(32u);
            acquiredCapacity = buffer.Bytes().capacity();
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    group->Wait();
    const auto stats = runner.SnapshotStats();
    const auto poolStats = pool.SnapshotStats();
    Require(
        result,
        acquiredCapacity >= 32u &&
            stats.reservedMemoryBytes == 0u &&
            poolStats.activeBytes == 0u &&
            poolStats.retainedBytes == 0u,
        "resourceRunner.scratch.claim",
        "scratch allocation did not use and release the current task claim");
}

inline void TestScratchPoolUsesRunnerForZeroClaimTask(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    std::size_t acquiredCapacity = 0u;
    std::uint64_t reservedBytesWhileActive = 0u;
    auto group = runner.CreateGroup();
    group->Submit([&]() {
        auto buffer = pool.Acquire(32u);
        acquiredCapacity = buffer.Bytes().capacity();
        reservedBytesWhileActive = runner.SnapshotStats().reservedMemoryBytes;
    });
    group->Wait();
    const auto stats = runner.SnapshotStats();
    const auto poolStats = pool.SnapshotStats();
    Require(
        result,
        acquiredCapacity >= 32u &&
            reservedBytesWhileActive >= 32u &&
            stats.reservedMemoryBytes == 0u &&
            poolStats.activeBytes == 0u &&
            poolStats.retainedBytes == 0u,
        "resourceRunner.scratch.zeroClaim",
        "zero-claim scratch allocation bypassed the resource runner");
}

inline void TestScratchPoolUsesConfiguredRunnerOnCoordinator(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    pool.ConfigureQuotaAcquire(MakeDataCodecRunnerScratchQuotaAcquire(&runner));
    auto buffer = pool.Acquire(32u);
    const auto reservedBytes = runner.SnapshotStats().reservedMemoryBytes;
    buffer.Release();
    Require(
        result,
        reservedBytes >= 32u &&
            runner.SnapshotStats().reservedMemoryBytes == 0u &&
            pool.SnapshotStats().activeBytes == 0u,
        "resourceRunner.scratch.configuredRunner",
        "coordinator scratch allocation did not use its configured resource runner");
}

inline void TestConfiguredScratchQuotaUsesTaskClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    pool.ConfigureQuotaAcquire(MakeDataCodecRunnerScratchQuotaAcquire(&runner));
    std::uint64_t reservedBytesWhileActive = 0u;
    auto group = runner.CreateGroup();
    group->Submit(
        [&]() {
            auto buffer = pool.Acquire(32u);
            reservedBytesWhileActive = runner.SnapshotStats().reservedMemoryBytes;
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    group->Wait();
    Require(
        result,
        reservedBytesWhileActive == 64u &&
            runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.scratch.configuredTaskClaim",
        "configured scratch quota did not reuse the current task claim");
}

inline void TestConfiguredScratchQuotaDoesNotCrossRunner(TestResult& result) {
    DataCodecResourceTaskRunner firstRunner(1u, 64u);
    DataCodecResourceTaskRunner secondRunner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    pool.ConfigureQuotaAcquire(MakeDataCodecRunnerScratchQuotaAcquire(&secondRunner));
    auto firstReservation = firstRunner.AcquireMemoryReservation(64u);
    ScratchByteBuffer buffer;
    {
        DataCodecResidentMemoryReservationScope scope(firstReservation);
        buffer = pool.Acquire(24u);
    }
    Require(
        result,
        firstRunner.SnapshotStats().reservedMemoryBytes == 64u &&
            secondRunner.SnapshotStats().reservedMemoryBytes == 24u,
        "resourceRunner.scratch.runnerBoundary",
        "configured scratch quota borrowed a reservation from another runner");
    buffer.Release();
    firstReservation.Release();
}

inline void TestScratchPoolSplitsCurrentResidentClaim(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(0u, 0u, 0u);
    auto reservation = runner.AcquireMemoryReservation(64u);
    ScratchByteBuffer buffer;
    {
        DataCodecResidentMemoryReservationScope scope(reservation);
        buffer = pool.Acquire(32u);
    }
    Require(
        result,
        buffer.Bytes().capacity() >= 32u &&
            reservation.Bytes() == 32u &&
            runner.SnapshotStats().reservedMemoryBytes == 64u,
        "resourceRunner.scratch.residentSplit",
        "scratch allocation did not split the current resident claim");
    buffer.Release();
    reservation.Release();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u,
        "resourceRunner.scratch.residentRelease",
        "scratch resident split was not released independently");
}

inline void TestScratchPoolRetainsTaskClaimWithBlock(TestResult& result) {
    DataCodecResourceTaskRunner runner(1u, 64u);
    ScratchByteBufferPool pool(1u, 1024u, 1024u);
    auto group = runner.CreateGroup();
    group->Submit(
        [&]() {
            auto buffer = pool.Acquire(32u);
        },
        DataCodecTaskResourceClaim{.memoryBytes = 64u});
    group->Wait();
    const auto retainedStats = runner.SnapshotStats();
    const auto poolStats = pool.SnapshotStats();
    Require(
        result,
        poolStats.retainedBytes >= 32u &&
            retainedStats.reservedMemoryBytes == poolStats.retainedBytes,
        "resourceRunner.scratch.retained",
        "retained scratch block did not retain its task memory claim");
    pool.Clear();
    Require(
        result,
        runner.SnapshotStats().reservedMemoryBytes == 0u &&
            pool.SnapshotStats().retainedBytes == 0u,
        "resourceRunner.scratch.retainedRelease",
        "clearing retained scratch blocks did not release their claims");
}

inline void TestResidentScopeDoesNotCrossRunnerBoundary(TestResult& result) {
    DataCodecResourceTaskRunner firstRunner(1u, 64u);
    DataCodecResourceTaskRunner secondRunner(1u, 64u);
    auto firstReservation = firstRunner.AcquireMemoryReservation(64u);
    DataCodecTaskMemoryReservation secondReservation;
    {
        DataCodecResidentMemoryReservationScope scope(firstReservation);
        secondReservation = secondRunner.AcquireResidentMemoryReservation(24u);
    }
    Require(
        result,
        firstReservation.Bytes() == 64u &&
            firstRunner.SnapshotStats().reservedMemoryBytes == 64u &&
            secondReservation.Bytes() == 24u &&
            secondRunner.SnapshotStats().reservedMemoryBytes == 24u,
        "resourceRunner.reservation.runnerBoundary",
        "resident reservation scope transferred capacity across runner boundaries");
    firstReservation.Release();
    secondReservation.Release();
}

inline void TestExecutionResourcesHonorExplicitLimits(TestResult& result) {
    auto owner = std::make_shared<DataCodecResourceTaskRunner>(4u, 128u);
    auto unrelatedRunner = std::make_shared<DataCodecResourceTaskRunner>(1u, 32u);
    const auto ownerNormalized = ResolveDataCodecExecutionResources(
        DataCodecExecutionResources{
            .parallelTaskRunner = unrelatedRunner.get(),
            .parallelTaskRunnerOwner = owner,
        });
    Require(
        result,
        ownerNormalized.parallelTaskRunner == owner.get() &&
            ownerNormalized.parallelTaskRunnerOwner == owner,
        "resourceRunner.executionResources.ownerPointer",
        "execution resource owner did not normalize its runner pointer");
    const auto matching = ResolveDataCodecExecutionResources(
        DataCodecExecutionResources{
            .parallelTaskRunnerOwner = owner,
            .workerLimit = 4u,
            .memoryLimitBytes = 128u,
        });
    Require(
        result,
        matching.parallelTaskRunner == owner.get() &&
            matching.parallelTaskRunnerOwner == owner,
        "resourceRunner.executionResources.reuse",
        "matching execution resource limits did not reuse their owner");

    const auto matchingBorrowed = ResolveDataCodecExecutionResources(
        DataCodecExecutionResources{
            .parallelTaskRunner = owner.get(),
            .workerLimit = 4u,
            .memoryLimitBytes = 128u,
        });
    Require(
        result,
        matchingBorrowed.parallelTaskRunner == owner.get() &&
            matchingBorrowed.parallelTaskRunnerOwner == nullptr,
        "resourceRunner.executionResources.reuseBorrowed",
        "matching borrowed execution resources were replaced unnecessarily");

    const auto constrained = ResolveDataCodecExecutionResources(
        DataCodecExecutionResources{
            .parallelTaskRunner = owner.get(),
            .parallelTaskRunnerOwner = owner,
            .workerLimit = 2u,
            .memoryLimitBytes = 64u,
        });
    const auto* constrainedRunner = dynamic_cast<const IDataCodecResourceTaskRunner*>(
        constrained.parallelTaskRunner);
    Require(
        result,
        constrainedRunner != nullptr &&
            constrainedRunner != owner.get() &&
            constrainedRunner->Concurrency() == 2u &&
            constrainedRunner->MemoryLimitBytes() == 64u,
        "resourceRunner.executionResources.constrain",
        "explicit execution resource limits were ignored by an existing owner");
}

} // 资源任务运行器测试命名空间

[[nodiscard]] inline TestResult RunDataCodecFeatureResourceTaskRunner() noexcept {
    TestResult result;
    try {
        resource_task_runner_test::TestMemoryLimitAndFill(result);
        resource_task_runner_test::TestWorkerLimitAndFill(result);
        resource_task_runner_test::TestMixedClaimsPreferRunnablePair(result);
        resource_task_runner_test::TestOversizedClaimRejected(result);
        resource_task_runner_test::TestNestedWaitMakesProgress(result);
        resource_task_runner_test::TestNestedRetainedReservationTransfersToParentClaim(result);
        resource_task_runner_test::TestMultiLevelRetainedReservationTransfersToRootClaim(result);
        resource_task_runner_test::TestCancelledTaskBypassesUnavailableMemory(result);
        resource_task_runner_test::TestTaskExceptionReleasesClaim(result);
        resource_task_runner_test::TestShutdownReleasesRejectedTaskCaptures(result);
        resource_task_runner_test::TestTransferredReservationControlsFollowingTask(result);
        resource_task_runner_test::TestPartialReservationReleasesFinishedWorkspace(result);
        resource_task_runner_test::TestReservationShrinkReleasesUnusedCapacity(result);
        resource_task_runner_test::TestNestedDirectReservationMakesProgress(result);
        resource_task_runner_test::TestByteStoreTransfersTaskClaims(result);
        resource_task_runner_test::TestByteStoreSplitsDirectReservation(result);
        resource_task_runner_test::TestByteStoreUsesAvailableGrowthCapacity(result);
        resource_task_runner_test::TestByteStoreGrowthFallsBackToRequiredBytes(result);
        resource_task_runner_test::TestByteStoreReservationOutlivesRunnerOwner(result);
        resource_task_runner_test::TestScratchPoolUsesTaskClaim(result);
        resource_task_runner_test::TestScratchPoolUsesRunnerForZeroClaimTask(result);
        resource_task_runner_test::TestScratchPoolUsesConfiguredRunnerOnCoordinator(result);
        resource_task_runner_test::TestConfiguredScratchQuotaUsesTaskClaim(result);
        resource_task_runner_test::TestConfiguredScratchQuotaDoesNotCrossRunner(result);
        resource_task_runner_test::TestScratchPoolSplitsCurrentResidentClaim(result);
        resource_task_runner_test::TestScratchPoolRetainsTaskClaimWithBlock(result);
        resource_task_runner_test::TestResidentScopeDoesNotCrossRunnerBoundary(result);
        resource_task_runner_test::TestExecutionResourcesHonorExplicitLimits(result);
    } catch (const std::exception& exception) {
        Require(
            result,
            false,
            "resourceRunner.exception",
            exception.what());
    } catch (...) {
        Require(
            result,
            false,
            "resourceRunner.exception",
            "resource task runner test raised an unknown exception");
    }
    return result;
}

} // 命名空间 datacodec::test

#endif
