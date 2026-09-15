#ifndef DATACODEC_TEST_FEATURE_UNLIMITEDTHREADS_H
#define DATACODEC_TEST_FEATURE_UNLIMITEDTHREADS_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include <condition_variable>
#include <mutex>
#include <vector>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureUnlimitedThreads() {
    using namespace std::chrono_literals;
    TestResult result;
    ResourceSample sample;
    sample.threaded = true;
    sample.allowedComputeThreads = 1u;
    sample.runtimeThreadLimit = 3u;
    sample.reservedHostThreads = 1u;
    sample.sampledAt = ResourceClock::now();
    const auto config = ResolveResourceConfiguration({.mode = CodecResourceMode::Unlimited}, sample);
    Require(result, config.threadMode == CodecThreadMode::Unlimited && config.computeCeiling == 0u &&
        config.initialLimits.computeLimit == 0u && config.initialLimits.slotLimit == 0u,
        "threads.unlimited-default", "default threads must not inherit the device or precreated pool ceiling");
    const auto wait = [](DataCodecExecutionResources& run, const TerminalWork& work) {
        while (!run.Stopped()) {
            const auto epoch = run.EventEpoch();
            if (run.Completion(work) == BlockCompletion::Succeeded) { return true; }
            run.WaitForChange(epoch);
        }
        return false;
    };
    {
        DataCodecExecutionResources run(config);
        ResourceDebugSnapshot snapshot;
        run.TryCopyResourceDebugSnapshot(snapshot);
        Require(result, snapshot.createdWorkers == 0u && run.WorkerCapacity() == 0u,
            "threads.lazy", "constructing unlimited resources must not create workers");
        Require(result, run.BeginRun(), "threads.begin", "unlimited request must start");
        {
            auto first = run.Scratch().Acquire(32u);
            auto second = run.Scratch().Acquire(64u);
            first.Release();
            second.Release();
            const auto before = run.Scratch().SnapshotStats();
            auto reused = run.Scratch().Acquire(32u);
            const auto after = run.Scratch().SnapshotStats();
            Require(result, before.retainedBlockCount == 2u &&
                after.allocationCount == before.allocationCount &&
                after.reusedBlockCount == before.reusedBlockCount + 1u,
                "threads.scratch-reuse", "unlimited workers must retain and reuse scratch between blocks");
        }
        constexpr std::size_t count = 24u;
        std::vector<std::thread::id> threads(count);
        std::vector<const void*> compressors(count);
        for (int wave = 0; wave < 3; ++wave) {
            if (wave == 2) {
                Require(result, run.EndRun(), "threads.end", "completed request must drain");
                run.TryCopyResourceDebugSnapshot(snapshot);
                Require(result, snapshot.createdWorkers == count && snapshot.activeComputeUnits == 0u,
                    "threads.retain", "normal request completion must retain idle workers");
                Require(result, run.BeginRun(), "threads.reopen", "unlimited resources must support another request");
            }
            std::mutex mutex;
            std::condition_variable_any changed;
            std::size_t entered = 0u;
            bool released = false;
            bool reused = true;
            std::vector<SlotLease> slots;
            std::vector<std::shared_ptr<TerminalWork>> tasks;
            slots.reserve(count);
            tasks.reserve(count);
            bool submitted = run.BeginFlow();
            for (std::size_t i = 0u; i < count && submitted; ++i) {
                auto slot = run.TryAcquireSlot();
                if (!slot) { submitted = false; break; }
                slots.push_back(std::move(*slot));
                tasks.push_back(std::make_shared<TerminalWork>([&](WorkerContext& worker) {
                    std::unique_lock lock(mutex);
                    const auto index = worker.Index();
                    const auto* compressor = &worker.NumericCompressor();
                    if (index >= count) { reused = false; }
                    else if (wave == 0) {
                        threads[index] = std::this_thread::get_id();
                        compressors[index] = compressor;
                    } else {
                        reused &= threads[index] == std::this_thread::get_id() && compressors[index] == compressor;
                    }
                    ++entered;
                    changed.notify_all();
                    return changed.wait(lock, worker.StopToken(), [&] { return released; });
                }));
                submitted = run.SubmitTerminal(slots.back(), tasks.back());
            }
            bool concurrent = false;
            {
                std::unique_lock lock(mutex);
                concurrent = submitted && changed.wait_for(lock, 3s, [&] { return entered == count; });
                run.TryCopyResourceDebugSnapshot(snapshot);
                released = true;
            }
            changed.notify_all();
            Require(result, concurrent && reused && snapshot.createdWorkers == count && snapshot.activeComputeUnits == count,
                wave == 0 ? "threads.exceed-device" : "threads.reuse",
                "tasks must exceed the device hint and reuse threads and compressors across flows and requests");
            bool completed = submitted;
            for (std::size_t i = 0u; i < tasks.size(); ++i) {
                if (wait(run, *tasks[i])) { completed &= run.CommitSlot(slots[i]); }
                else { completed = false; }
            }
            if (!completed) { run.CancelAndWaitRun(); }
            slots.clear();
            if (!completed) { break; }
        }
        Require(result, run.EndRun(), "threads.final-end", "completed request must drain");
        run.ShutdownAndJoin();
        run.TryCopyResourceDebugSnapshot(snapshot);
        Require(result, snapshot.createdWorkers == 0u && snapshot.activeComputeUnits == 0u,
            "threads.retire", "shutdown must join every retained unlimited worker");
    }
    {
        DataCodecExecutionResources run(config);
        run.BeginRun();
        std::mutex mutex;
        std::condition_variable_any changed;
        bool entered = false;
        auto slot = run.TryAcquireSlot();
        auto task = std::make_shared<TerminalWork>([&](WorkerContext& worker) {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            return changed.wait(lock, worker.StopToken(), [] { return false; });
        });
        const bool submitted = slot && run.SubmitTerminal(*slot, task);
        {
            std::unique_lock lock(mutex);
            Require(result, submitted && changed.wait_for(lock, 3s, [&] { return entered; }),
                "threads.cancel-start", "cancellation test must reach an active worker");
        }
        run.CancelAndWaitRun();
        slot.reset();
        Require(result, run.EndRun(), "threads.cancel-drain", "cancellation must drain and retire active unlimited workers");
        ResourceDebugSnapshot snapshot;
        run.TryCopyResourceDebugSnapshot(snapshot);
        Require(result, snapshot.createdWorkers == 0u && snapshot.admittedBlocks == 0u,
            "threads.cancel-retire", "cancellation must release slots and join workers");
    }
    {
        auto fixedMemory = ResolveResourceConfiguration(
            {.mode = CodecResourceMode::Fixed, .ownedStorageLimitBytes = 64u}, sample);
        DataCodecExecutionResources run(fixedMemory);
        run.BeginRun();
        resource::ResidentByteBudget::Lease firstBytes, secondBytes;
        auto first = run.TryAcquireSlot(64u, firstBytes);
        auto second = run.TryAcquireSlot(1u, secondBytes);
        Require(result, first && !second, "threads.memory-admission", "unlimited threads must still obey the byte budget");
        firstBytes.Reset();
        first.reset();
        second = run.TryAcquireSlot(64u, secondBytes);
        Require(result, second.has_value(), "threads.memory-release", "released byte capacity must permit new admission");
        secondBytes.Reset();
        second.reset();
        run.EndRun();
    }
    {
        DataCodecExecutionResources run(config);
        run.BeginRun();
        auto slot = run.TryAcquireSlot();
        bool ran = false;
        auto task = std::make_shared<TerminalWork>([&](WorkerContext&) { ran = true; return true; });
        bool submitted = false;
        {
            RejectAllocationsScope reject;
            submitted = run.SubmitTerminal(*slot, task);
            run.CancelAndWaitRun();
            slot.reset();
            run.EndRun();
        }
        Require(result, !submitted && !ran && run.FirstFailure().has_value(),
            "threads.creation-failure", "worker storage allocation failure must cancel and drain without running the task");
    }
    {
        auto memorySample = sample;
        memorySample.physicalTotalBytes = 1024ull * 1024u * 1024u;
        memorySample.availableBytes = 768ull * 1024u * 1024u;
        const auto adaptive = ResolveResourceConfiguration({}, memorySample);
        ResourceControllerState controller;
        InitializeResourceController(controller, adaptive, memorySample.sampledAt);
        FlowSnapshot flow;
        flow.runActive = true;
        flow.requestId = 1u;
        flow.limits = adaptive.initialLimits;
        flow.gateOpen = true;
        auto decision = Advance(controller, memorySample.sampledAt, memorySample, flow);
        Require(result, decision.gateOpen && decision.limits.computeLimit == 0u && decision.limits.slotLimit == 0u,
            "threads.adaptive-memory-ready", "healthy adaptive memory must preserve unlimited CPU admission");
        flow.limits = decision.limits;
        memorySample.availableBytes = 1u;
        memorySample.sampledAt += 250ms;
        decision = Advance(controller, memorySample.sampledAt, memorySample, flow);
        Require(result, !decision.gateOpen && decision.limits.computeLimit == 0u,
            "threads.adaptive-memory-pressure", "memory pressure must close admission without creating a CPU ceiling");
    }
    {
        bool rejected = false;
        try { ResolveResourceConfiguration({.mode = CodecResourceMode::Unlimited, .maxComputeThreads = 2u,
            .threadMode = CodecThreadMode::Unlimited}, sample); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(result, rejected, "threads.conflicting-limit", "explicit unlimited mode must reject a thread limit");
        sample.threaded = false;
        const auto single = ResolveResourceConfiguration({.mode = CodecResourceMode::Unlimited}, sample);
        Require(result, single.computeCeiling == 1u && single.initialLimits.slotLimit == 1u,
            "threads.single-runtime", "a runtime without pthread support remains sequential");
    }
    return result;
}

}
#endif
