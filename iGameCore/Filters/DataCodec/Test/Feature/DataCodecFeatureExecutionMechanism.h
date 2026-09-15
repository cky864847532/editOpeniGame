#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATUREEXECUTIONMECHANISM_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATUREEXECUTIONMECHANISM_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Runtime/Cache/CacheResources.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Test/Feature/DataCodecFeatureResourceController.h"
#include "DataCodec/Test/Feature/DataCodecFeatureWorkerCompressor.h"
#include "DataCodec/Test/Feature/DataCodecFeatureWorkTypeBoundary.h"

#include <atomic>
#include <condition_variable>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

namespace datacodec::test {

inline bool CopyExecutionSnapshot(DataCodecExecutionResources& run, ResourceDebugSnapshot& snapshot) {
    const auto deadline = ResourceClock::now() + std::chrono::seconds(2);
    do {
        if (run.TryCopyResourceDebugSnapshot(snapshot)) { return true; }
        std::this_thread::yield();
    } while (ResourceClock::now() < deadline);
    return false;
}

inline bool WaitForTerminal(DataCodecExecutionResources& run, const TerminalWork& work) {
    for (;;) {
        const auto epoch = run.EventEpoch();
        const auto completion = run.Completion(work);
        if (completion == BlockCompletion::Succeeded) { return true; }
        if (run.Stopped()) { return false; }
        run.WaitForChange(epoch);
    }
}

inline TestResult RunDataCodecFeatureExecutionMechanism() {
    TestResult result = RunDataCodecFeatureResourceController();
    const auto compressorResult = RunDataCodecFeatureWorkerCompressor();
    result.passed &= compressorResult.passed;
    result.failures.insert(result.failures.end(), compressorResult.failures.begin(), compressorResult.failures.end());
    result.AppendDiagnostics(compressorResult.diagnostics);
    const auto workTypes = RunDataCodecFeatureWorkTypeBoundary();
    result.passed &= workTypes.passed;
    result.failures.insert(result.failures.end(), workTypes.failures.begin(), workTypes.failures.end());
    result.AppendDiagnostics(workTypes.diagnostics);
    const ResolvedResourceConfiguration fixed{{64u, 2u, 4u}, 128u, 4u, true, true};
    {
        auto config = ResolveResourceConfiguration(
            {.mode = CodecResourceMode::Adaptive, .maxComputeThreads = 1u},
            ProbeResources());
        DataCodecExecutionResources run(config);
        run.BeginRun();
        run.UpdateLimits({64u, 1u, 2u}, true, ResourceDecisionReason::MechanismCheck);
        run.BeginFlow(false);
        auto first = run.TryAcquireSlot();
        auto second = run.TryAcquireSlot();
        if (first && second) {
            std::latch started(1), release(1);
            auto slow = std::make_shared<TerminalWork>([&](WorkerContext&) {
                started.count_down();
                release.wait();
                return true;
            });
            auto next = std::make_shared<TerminalWork>([](WorkerContext&) { return true; });
            const bool submitted = run.SubmitTerminal(*first, slow) && run.SubmitTerminal(*second, next);
            ResourceDebugSnapshot computing, ready;
            if (submitted) {
                started.wait();
                run.SetWaitReason(ResourceWaitReason::OrderedCommit, &*first);
                Require(result, CopyExecutionSnapshot(run, computing) &&
                    computing.observationWaits[0] > ResourceClock::duration::zero() &&
                    computing.observationWaits[3] == ResourceClock::duration::zero(),
                    "execution.compute-is-not-output", "a queued block behind a computing first block must not count as output congestion");
            }
            release.count_down();
            const bool completed = submitted && WaitForTerminal(run, *slow) && WaitForTerminal(run, *next);
            Require(result, completed && CopyExecutionSnapshot(run, ready) &&
                ready.observationWaits[3] > ResourceClock::duration::zero(),
                "execution.completed-output-wait", "completed unconsumed blocks must contribute to output congestion");
            if (completed) { run.CommitSlot(*first); run.CommitSlot(*second); }
            // 故障分支同样在被终端捕获的同步对象析构前收束
            run.CancelAndWaitRun();
        } else {
            Require(result, false, "execution.wait-classification-admission", "the wait classification fixture must acquire both slots");
        }
        first.reset(); second.reset();
        run.SetWaitReason(ResourceWaitReason::None);
        run.CancelAndWaitRun();
        run.EndRun();
        const bool restarted = run.BeginRun();
        auto phase = restarted ? run.TryAcquireHeavyPhase() : std::nullopt;
        const bool reused = phase && RunTerminalWork(run, *phase, [](WorkerContext&) { return true; });
        phase.reset();
        Require(result, restarted && reused && run.EndRun(),
            "execution.adaptive-controller-restart", "an idle adaptive controller must wake for the next request after cancellation");
    }
    for (const bool threaded : {false, true}) {
        auto config = fixed;
        config.threaded = threaded;
        if (!threaded) { config.initialLimits = {64u, 1u, 1u}; }
        DataCodecExecutionResources run(config);
        run.BeginRun();
        unsigned cursor = 0u, reads = 0u, computes = 0u, commits = 0u, descriptions = 0u;
        const bool success = RunOrderedBlocks<unsigned, unsigned>(run,
            [&] { return cursor < 3u; },
            [&](unsigned& input) { ++reads; input = cursor; return input < 3u; },
            [&](unsigned input, unsigned& output, WorkerContext&) {
                ++computes;
                output = input + 1u;
                return true;
            },
            [&](unsigned output) { ++commits; cursor = output; return true; }, true,
            [&] {
                ++descriptions;
                return ResourceWorkType{.path = ResourceWorkPath::AttributeDecode, .codec = cursor};
            });
        Require(result, success && cursor == 3u && reads == 3u && computes == 3u && commits == 3u &&
            descriptions == 3u && run.EndRun(),
            "execution.commit-driven-cursor", "final commit must stop admission without reading an extra empty block");
    }
    {
        ScratchByteBufferPool pool(1u);
        auto dirty = pool.Acquire(64u);
        std::fill(dirty.Bytes().begin(), dirty.Bytes().end(), std::uint8_t{42u});
        dirty.Release();
        auto overwrite = pool.AcquireForOverwrite(64u);
        Require(result, std::all_of(overwrite.Bytes().begin(), overwrite.Bytes().end(),
                [](auto value) { return value == 42u; }),
            "scratch.overwrite-reuse", "overwrite acquisition must preserve reusable storage without clearing");
        overwrite.Release();
        auto initialized = pool.Acquire(32u);
        Require(result, std::all_of(initialized.Bytes().begin(), initialized.Bytes().end(),
                [](auto value) { return value == 0u; }) && pool.SnapshotStats().allocationCount == 1u,
            "scratch.initialized-reuse", "ordinary acquisition must keep its zero initialization contract");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        DecodeBlockMemoryPlan plan;
        const auto input = plan.Append<std::uint8_t>(DecodeMemoryRegion::Input, 32u);
        const auto flow = [&](unsigned field) {
            bool pending = true;
            return RunOrderedBlocks<unsigned, unsigned>(run,
                [&] { return pending; },
                [&](unsigned& value, const SlotLease&, DecodeBlockWorkspace& workspace) {
                    pending = false;
                    value = field;
                    auto bytes = workspace.View<std::uint8_t>(input);
                    bytes.assign(32u, static_cast<std::uint8_t>(field));
                    return true;
                },
                [&](unsigned value, unsigned& output, WorkerContext&, DecodeBlockWorkspace& workspace) {
                    auto bytes = workspace.View<std::uint8_t>(input);
                    bytes.resize(32u);
                    output = bytes[0];
                    return output == value;
                }, [&](unsigned value) { return value == field; }, true,
                [&] { return ResourceWorkType{.path = ResourceWorkPath::AttributeDecode, .codec = field}; },
                [&] { return plan; });
        };
        const bool first = flow(1u);
        const auto allocated = run.Scratch().SnapshotStats().allocationCount;
        run.UpdateLimits(fixed.initialLimits, true, ResourceDecisionReason::MechanismCheck);
        const bool second = flow(2u);
        Require(result, first && second && allocated == 1u &&
            run.Scratch().SnapshotStats().allocationCount == allocated &&
            run.StorageCapacity()->Snapshot().reservedBytes == 32u,
            "scratch.cross-flow", "field changes and unchanged targets must reuse budgeted workspace across flows");
        run.UpdateLimits({16u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        Require(result, run.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "scratch.shrink-fixed", "a memory target reduction must release idle fixed workspace");
        run.UpdateLimits(fixed.initialLimits, true, ResourceDecisionReason::MechanismCheck);
        Require(result, flow(3u) && run.EndRun() && run.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "scratch.end-fixed", "request completion must release retained workspace reservations");
    }
    {
        ScratchByteBuffer survivor;
        {
            ScratchByteBufferPool pool(4u);
            auto small = pool.Acquire(32u);
            auto large = pool.Acquire(64u);
            small.Release();
            large.Release();
            auto best = pool.Acquire(24u);
            Require(result, best.Bytes().capacity() == 32u,
                "scratch.best-fit", "scratch must reuse the smallest sufficient buffer");
            survivor = pool.Acquire(64u);
            survivor.Bytes()[0] = 42u;
            pool.Close();
            best.Release();
            Require(result, pool.SnapshotStats().retainedBytes == 0u && survivor.Bytes()[0] == 42u,
                "scratch.close-live", "closing the pool must preserve borrowed data and disable late retention");
        }
        bool cleared = false;
        {
            RejectAllocationsScope reject;
            survivor.Release();
            cleared = survivor.Bytes().capacity() == 0u;
        }
        Require(result, cleared && rejectedAllocationCount == 0u,
            "scratch.weak-return", "a buffer must be releasable after its pool dies without allocating");
    }
    {
        DataCodecExecutionResources run(fixed);
        CacheResources encodeView;
        CacheResources decodeView;
        encodeView.BindRun(run);
        decodeView.BindRun(run);
        Require(result, &encodeView.ScratchBytePool() == &decodeView.ScratchBytePool() &&
            run.Scratch().SnapshotStats().maxRetainedBlockCount == 8u,
            "scratch.root-view", "all workspace views must access the root pool with a count-based limit");
        auto first = run.Scratch().Acquire(8u);
        auto second = run.Scratch().Acquire(16u);
        auto third = run.Scratch().Acquire(32u);
        first.Release(); second.Release(); third.Release();
        run.UpdateLimits({64u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        Require(result, run.Scratch().SnapshotStats().retainedBlockCount == 2u,
            "scratch.downsize-trim", "lower C/S must immediately trim excess idle buffers");
        auto late = run.Scratch().Acquire(8u);
        run.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        late.Release();
        Require(result, run.Scratch().SnapshotStats().retainedBytes == 0u &&
            run.Scratch().SnapshotStats().maxRetainedBlockCount == 0u,
            "scratch.zero-late-return", "zero M must discard existing idle buffers and late returns");
        run.UpdateLimits({64u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        Require(result, run.BeginRun(), "scratch.failure-begin", "failure cleanup request must begin");
        auto failedBuffer = run.Scratch().Acquire(32u);
        run.RequestStop();
        run.CancelAndWaitRun();
        run.EndRun();
        Require(result, run.BeginRun(), "scratch.next-request", "a drained request must allow the next request");
        failedBuffer.Release();
        Require(result, run.Scratch().SnapshotStats().retainedBytes == 0u,
            "scratch.failed-generation", "buffers cleared by failure must not repopulate a later request's pool");
        run.Scratch().Acquire(32u).Release();
        Require(result, run.Scratch().SnapshotStats().retainedBytes == 32u,
            "scratch.retention-restored", "the next healthy request may retain its own buffers");
        run.EndRun();
        encodeView.UnbindRun();
        decodeView.UnbindRun();
    }
    {
        DataCodecExecutionResources run(fixed);
        auto capacity = run.StorageCapacity();
        auto lease = capacity->TryReserve(48u);
        const bool shrunk = run.UpdateLimits({16u, 1u, 1u}, false, ResourceDecisionReason::MechanismCheck);
        ResourceDebugSnapshot state;
        const bool copied = CopyExecutionSnapshot(run, state);
        Require(result, shrunk && copied && state.storage.reservedBytes == 48u &&
            state.storage.limitBytes == 16u && !capacity->TryReserve(1u) && lease->Bytes() == 48u,
            "execution.downsize-live-capacity", "capacity reduction must preserve real reservations");
        lease.reset();
        Require(result, run.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            !capacity->TryReserve(1u) && capacity->TryReserve(0u).has_value(),
            "execution.zero-capacity", "zero storage limit must have exact semantics");
        const auto epoch = run.EventEpoch();
        Require(result, run.UpdateLimits({0u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck) &&
            run.EventEpoch() == epoch, "execution.unchanged-target", "identical targets must not generate events");
        Require(result, !run.UpdateLimits({8u, 5u, 2u}, true, ResourceDecisionReason::MechanismCheck) &&
            CopyExecutionSnapshot(run, state) && state.limits.ownedStorageLimitBytes == 0u &&
            state.limits.computeLimit == 1u && state.closing,
            "execution.invalid-target", "invalid updates must retain every previous target and record failure");
    }
    {
        DataCodecExecutionResources run(fixed);
        Require(result, run.BeginRun(), "execution.begin", "request must start");
        std::latch release(1);
        std::atomic_uint started{0u};
        std::vector<SlotLease> slots;
        std::vector<std::shared_ptr<TerminalWork>> tasks;
        for (unsigned i = 0u; i < 4u; ++i) {
            slots.push_back(std::move(*run.TryAcquireSlot()));
            auto task = std::make_shared<TerminalWork>([&](WorkerContext&) {
                started.fetch_add(1u);
                started.notify_all();
                release.wait();
                return true;
            });
            if (!run.SubmitTerminal(slots.back(), task)) { release.count_down(); }
            tasks.push_back(std::move(task));
        }
        for (auto count = started.load(); count < 2u; count = started.load()) { started.wait(count); }
        run.UpdateLimits({16u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        ResourceDebugSnapshot state;
        const auto captured = CopyExecutionSnapshot(run, state);
        const bool noExtraSlot = !run.TryAcquireSlot();
        release.count_down();
        bool completed = true;
        for (std::size_t i = 0u; i < tasks.size(); ++i) {
            completed &= WaitForTerminal(run, *tasks[i]);
            completed &= run.CommitSlot(slots[i]);
            slots[i].Reset();
        }
        Require(result, captured && state.admittedBlocks == 4u && state.activeComputeUnits == 2u &&
            state.limits.slotLimit == 1u && noExtraSlot && completed && run.EndRun(),
            "execution.downsize-inflight", "existing slots and compute units must drain under smaller targets");
    }
    {
        auto config = fixed;
        config.initialLimits.computeLimit = 1u;
        DataCodecExecutionResources run(config);
        run.BeginRun();
        std::latch release(1);
        std::atomic_uint started{0u};
        std::vector<SlotLease> slots;
        std::vector<std::shared_ptr<TerminalWork>> tasks;
        for (unsigned i = 0u; i < 3u; ++i) {
            slots.push_back(std::move(*run.TryAcquireSlot()));
            auto task = std::make_shared<TerminalWork>([&](WorkerContext&) {
                started.fetch_add(1u);
                started.notify_all();
                release.wait();
                return true;
            });
            run.SubmitTerminal(slots.back(), task);
            tasks.push_back(std::move(task));
        }
        while (started.load() < 1u) { started.wait(0u); }
        run.UpdateLimits({64u, 3u, 4u}, true, ResourceDecisionReason::MechanismCheck);
        while (started.load() < 3u) { const auto n = started.load(); if (n < 3u) { started.wait(n); } }
        release.count_down();
        bool completed = true;
        for (std::size_t i = 0u; i < tasks.size(); ++i) {
            completed &= WaitForTerminal(run, *tasks[i]);
            completed &= run.CommitSlot(slots[i]);
            slots[i].Reset();
        }
        ResourceDebugSnapshot state;
        Require(result, completed && CopyExecutionSnapshot(run, state) && state.createdWorkers == 3u && run.EndRun(),
            "execution.grow-queued-work", "raising compute targets must wake queued work and create workers on demand");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        auto slot = run.TryAcquireSlot();
        run.UpdateLimits({64u, 2u, 4u}, false, ResourceDecisionReason::MechanismCheck);
        const bool acceptedWork = RunTerminalWork(run, *slot, [](WorkerContext&) { return true; });
        const bool sealed = run.CommitSlot(*slot);
        slot.reset();
        Require(result, acceptedWork && sealed && !run.TryAcquireSlot() && !run.TryAcquireHeavyPhase() && run.EndRun(),
            "execution.closed-gate", "already admitted work must finish while new admission is closed");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        unsigned next = 0u;
        std::vector<unsigned> committed;
        std::atomic_uint readCount{0u};
        std::latch releaseFirst(1);
        std::jthread release([&] {
            while (readCount.load() < 4u) { const auto n = readCount.load(); if (n < 4u) { readCount.wait(n); } }
            releaseFirst.count_down();
        });
        bool bounded = true;
        const auto success = RunOrderedBlocks<unsigned, unsigned>(run,
            [&] { return next < 8u; },
            [&](unsigned& input) {
                ResourceDebugSnapshot state;
                if (run.TryCopyResourceDebugSnapshot(state)) { bounded &= state.admittedBlocks > 0u && state.admittedBlocks <= 4u; }
                input = next++;
                readCount.fetch_add(1u);
                readCount.notify_all();
                return true;
            },
            [&](unsigned input, unsigned& output, WorkerContext&) {
                if (input == 0u) { releaseFirst.wait(); }
                output = input;
                return true;
            },
            [&](unsigned output) {
                committed.push_back(output);
                if (output == 7u) { run.UpdateLimits({64u, 1u, 1u}, false, ResourceDecisionReason::MechanismCheck); }
                return true;
            });
        release.join();
        Require(result, success && bounded && committed == std::vector<unsigned>({0u,1u,2u,3u,4u,5u,6u,7u}) && run.EndRun(),
            "execution.ordered-bounded-flow", "slow first blocks must preserve bounded reads and ordered final commit");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        auto slot = run.TryAcquireSlot();
        bool ran = false, destroyedOutsideLock = false;
        struct Capture {
            DataCodecExecutionResources& run;
            bool& destroyedOutsideLock;
            ~Capture() {
                ResourceDebugSnapshot snapshot;
                destroyedOutsideLock = run.TryCopyResourceDebugSnapshot(snapshot);
            }
        };
        auto capture = std::make_shared<Capture>(run, destroyedOutsideLock);
        auto task = std::make_shared<TerminalWork>([capture, &ran](WorkerContext&) {
            ran = true;
            return true;
        });
        capture.reset();
        bool submitted = false, ended = false;
        std::size_t rejected = 0u;
        ResourceDebugSnapshot snapshot;
        {
            // 拒绝 worker 创建所需的真实堆申请，并保持拒绝直到队列与线程清理结束
            RejectAllocationsScope reject;
            submitted = run.SubmitTerminal(*slot, task);
            run.CancelAndWaitRun();
            slot.reset();
            ended = run.EndRun();
            run.ShutdownAndJoin();
            rejected = rejectedAllocationCount;
        }
        const auto failure = run.FirstFailure();
        Require(result, !submitted && !ran && destroyedOutsideLock && ended && rejected == 1u &&
            failure && std::string_view(failure->reason.data()) == "allocation-failed" &&
            CopyExecutionSnapshot(run, snapshot) && snapshot.closing && snapshot.createdWorkers == 0u &&
            snapshot.queuedTasks == 0u && snapshot.admittedBlocks == 0u && snapshot.activeComputeUnits == 0u &&
            !run.BeginRun(),
            "execution.worker-creation-allocation-failure",
            "worker creation allocation failure must close the root, destroy queued captures outside its lock and finish without further allocations or replay");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        auto slot = run.TryAcquireSlot();
        const bool success = RunTerminalWork(run, *slot, [](WorkerContext&) -> bool { throw std::bad_alloc{}; });
        slot.reset();
        const auto failure = run.FirstFailure();
        bool zeroAllocCleanup = false;
        {
            RejectAllocationsScope reject;
            run.CancelAndWaitRun();
            zeroAllocCleanup = run.EndRun() && rejectedAllocationCount == 0u;
        }
        const bool restarted = run.BeginRun();
        auto phase = run.TryAcquireHeavyPhase();
        const bool recovered = phase && RunTerminalWork(run, *phase, [](WorkerContext&) { return true; });
        phase.reset();
        Require(result, !success && failure && std::string_view(failure->reason.data()) == "allocation-failed" &&
            zeroAllocCleanup && restarted && recovered && run.EndRun(),
            "execution.failure-and-next-request", "failure must drain without allocation and permit the next independent request");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        bool firstPublishedBeforeStop = false;
        std::stop_callback stopped(run.StopToken(), [&] {
            const auto failure = run.FirstFailure();
            firstPublishedBeforeStop = failure && std::string_view(failure->reason.data()) == "allocation-failed";
        });
        const bool success = RunOrderedBlocks<unsigned, unsigned>(run,
            [] { return true; },
            [](unsigned&) -> bool { throw std::bad_alloc{}; },
            [](unsigned, unsigned&, WorkerContext&) { return true; },
            [](unsigned) { return true; });
        Require(result, !success && firstPublishedBeforeStop && run.EndRun(),
            "execution.reader-first-failure", "read exceptions must publish first failure before cancellation and cleanup");
    }
    {
        auto config = fixed;
        config.initialLimits = {64u, 4u, 4u};
        DataCodecExecutionResources run(config);
        run.BeginRun();
        auto phase = run.TryAcquireHeavyPhase();
        std::latch started(1), release(1);
        std::atomic_size_t acquired{0u};
        auto work = std::make_shared<TerminalWork>([&](WorkerContext& worker) {
            acquired.store(worker.ComputeUnits());
            started.count_down();
            release.wait();
            return true;
        }, TerminalWorkKind::ExclusivePackage);
        run.SubmitTerminal(*phase, work);
        started.wait();
        run.UpdateLimits({64u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
        ResourceDebugSnapshot state;
        const bool captured = CopyExecutionSnapshot(run, state);
        release.count_down();
        const bool completed = WaitForTerminal(run, *work);
        phase.reset();
        Require(result, captured && acquired == 4u && state.exclusiveUnits == 4u &&
            state.activeComputeUnits == 4u && state.limits.computeLimit == 1u && completed && run.EndRun(),
            "execution.exclusive-downsize", "exclusive work must retain its full acquired compute units until completion");
    }
    {
        DataCodecExecutionResources run(fixed);
        run.BeginRun();
        auto slot = run.TryAcquireSlot();
        const bool success = RunTerminalWork(run, *slot, [&](WorkerContext& worker) {
            run.CancelAndWaitRun();
            return worker.StopToken().stop_requested();
        });
        slot.reset();
        ResourceDebugSnapshot state;
        Require(result, !success && CopyExecutionSnapshot(run, state) && state.closing &&
            state.failure && std::string_view(state.failure->reason.data()) == "invalid-drain-driver" && run.EndRun(),
            "execution.reject-self-wait", "worker drain attempts must notify cancellation and close the root without self-waiting");
    }
    {
        auto config = fixed;
        config.threaded = false;
        config.initialLimits = {64u, 1u, 1u};
        DataCodecExecutionResources run(config);
        run.BeginRun();
        auto phase = run.TryAcquireHeavyPhase();
        const auto caller = std::this_thread::get_id();
        const bool success = RunTerminalWork(run, *phase, [&](WorkerContext& worker) {
            return std::this_thread::get_id() == caller && worker.ComputeUnits() == 1u;
        });
        phase.reset();
        ResourceDebugSnapshot state;
        Require(result, success && CopyExecutionSnapshot(run, state) && state.createdWorkers == 0u && run.EndRun(),
            "execution.no-threads", "sequential platform must execute the same terminal path without workers");
    }
    return result;
}

}

#endif
