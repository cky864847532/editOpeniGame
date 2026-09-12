#ifndef DATACODEC_TEST_FEATURE_DATACODECFEATURECAPACITYDIAGNOSTICS_H
#define DATACODEC_TEST_FEATURE_DATACODECFEATURECAPACITYDIAGNOSTICS_H

#include "DataCodec/Storage/ByteStore/ByteStore.h"
#include "DataCodec/Test/Common/DataCodecAllocationFailure.h"
#include "DataCodec/Test/Common/DataCodecTestResult.h"
#include "DataCodec/Runtime/Execution/ParallelExecution.h"
#include "DataCodec/Storage/ByteStore/SegmentedBinaryObject.h"
#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include <latch>
#include <thread>

namespace datacodec::test {

inline TestResult RunDataCodecFeatureCapacityDiagnostics() {
    TestResult result;
    static_assert(std::is_trivially_copyable_v<ResourceDebugSnapshot>);
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, false, true});
        CodecRunScope scope(run);
        bytestore::ByteStoreSession session;
        session.BindRun(run);
        run.UpdateLimits({64u, 1u, 1u}, false, ResourceDecisionReason::MechanismCheck);
        resource::CapacityRejection rejection;
        auto early = run.TryAcquireStorage(8u, MemoryDemandKind::RequiredContinuation, &rejection);
        Require(result, !early && run.StorageCapacity()->Snapshot().reservedBytes == 0u && !run.Stopped(),
            "admission.closed-storage", "a closed memory gate must reject a driver reservation even with sufficient byte allowance");
        std::jthread reopen([&] {
            const auto deadline = ResourceClock::now() + std::chrono::seconds(2);
            ResourceDebugSnapshot snapshot;
            while (ResourceClock::now() < deadline) {
                if (run.TryCopyResourceDebugSnapshot(snapshot) && snapshot.waiting == ResourceWaitReason::ByteCapacity) {
                    run.UpdateLimits({64u, 1u, 1u}, true, ResourceDecisionReason::MechanismCheck);
                    return;
                }
                std::this_thread::yield();
            }
            run.RequestStop();
        });
        auto store = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 8u,
            MemoryDemandKind::RequiredContinuation, "gated-stage");
        reopen.join();
        Require(result, store && run.StorageCapacity()->Snapshot().reservedBytes == 8u && !run.Stopped(),
            "admission.driver-resume", "a necessary driver allocation must resume after the memory gate reopens");
        auto memory = std::dynamic_pointer_cast<bytestore::MemoryStore>(store);
        if (memory) {
            const auto bytesBefore = run.StorageCapacity()->Snapshot().reservedBytes;
            Require(result, memory->PrepareCapacity(16u, run, MemoryDemandKind::RequiredContinuation) &&
                run.StorageCapacity()->Snapshot().peakReservedBytes >= bytesBefore + 16u,
                "admission.growth-coexistence", "growth must reserve the complete new array while the old array is live");
        }
    }
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{64u, 1u, 1u}, 64u, 1u, false, true});
        CodecRunScope scope(run);
        run.UpdateLimits({64u, 1u, 1u}, false, ResourceDecisionReason::MechanismCheck);
        std::jthread cancel([&] {
            const auto deadline = ResourceClock::now() + std::chrono::seconds(2);
            ResourceDebugSnapshot snapshot;
            while (ResourceClock::now() < deadline) {
                if (run.TryCopyResourceDebugSnapshot(snapshot) && snapshot.byteWaiting) { break; }
                std::this_thread::yield();
            }
            run.RequestStop();
        });
        const auto begin = ResourceClock::now();
        auto lease = run.WaitForStorage(8u, MemoryDemandKind::RequiredContinuation);
        cancel.join();
        Require(result, !lease && run.Stopped() && ResourceClock::now() - begin < std::chrono::seconds(3) &&
            run.StorageCapacity()->Snapshot().reservedBytes == 0u,
            "admission.cancel-wait", "cancellation must wake a necessary allocation wait and preserve capacity ownership");
    }
    {
        DataCodecExecutionResources run({{12u, 1u, 1u}, 12u, 1u, false, true, false});
        CodecRunScope scope(run);
        bytestore::ByteStoreSession session;
        session.BindRun(run);
        auto input = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 8u, ::datacodec::MemoryDemandKind::RequiredContinuation, "materialize_input");
        bytestore::SegmentedBinaryObject source;
        Require(result, input && input->Seal() && source.AddSegment(input),
            "diagnostics.materialize-setup", "the required source allocation must be ready");
        if (input) {
            bytestore::KnownStorageOwners owners;
            const auto references = input.use_count();
            {
                RejectAllocationsScope reject;
                owners.Add(input.get());
                owners.Add(input.get());
                owners.Add(nullptr);
            }
            Require(result, owners.Entries().size() == 1u && owners.Entries()[0].capacityBytes == 8u &&
                input.use_count() == references && rejectedAllocationCount == 0u,
                "diagnostics.known-owner", "known owners must be deduplicated without allocation or retained references");
            EncodedBuffer output;
            const bool refused = !source.Materialize(run, output);
            ResourceDebugSnapshot snapshot;
            Require(result, refused && output.empty() && source.CanRead() &&
                run.TryCopyResourceDebugSnapshot(snapshot) && snapshot.capacityRejection &&
                snapshot.capacityRejection->requestedBytes == 8u &&
                snapshot.capacityRejection->reservedBytes == 8u &&
                snapshot.capacityRejection->ownerCount == 1u &&
                snapshot.capacityRejection->owners[0].owner.id == owners.Entries()[0].owner.id &&
                snapshot.capacityRejection->owners[0].capacityBytes == 8u,
                "diagnostics.materialize-input", "full output refusal must preserve the input and describe its live exact capacity");
        }
    }
    {
        DataCodecExecutionResources run({{8u, 1u, 1u}, 8u, 1u, false, true, false});
        CodecRunScope scope(run);
        bytestore::ByteStoreSession session;
        session.BindRun(run);
        DecodedTopologyCache output;
        const bool refused = !output.InitializeConnectivity(1u, 2u, true, false, false, session);
        ResourceDebugSnapshot snapshot;
        Require(result, refused && !output.connectivity && !output.offsets &&
            run.TryCopyResourceDebugSnapshot(snapshot) && snapshot.storage.reservedBytes == 0u &&
            snapshot.capacityRejection && snapshot.capacityRejection->reservedBytes == 2u * sizeof(IndexType) &&
            snapshot.capacityRejection->ownerCount == 1u &&
            snapshot.capacityRejection->owners[0].capacityBytes == 2u * sizeof(IndexType),
            "diagnostics.topology-transaction", "later target refusal must describe the prior owner and roll back the unpublished transaction");
    }
    {
        resource::ResidentByteBudget capacity(20u);
        const auto original = capacity.NewOwner(resource::StorageOwnerPurpose::Contiguous, "original");
        auto live = capacity.TryReserve(8u, nullptr, original);
        resource::CapacityRejection rejection;
        const auto next = capacity.NewOwner(resource::StorageOwnerPurpose::Contiguous, "replacement");
        std::array<resource::StorageOwnerDescription, 17u> coexist{};
        for (auto& description : coexist) {
            description = {original, 8u, ResourceClock::now()};
        }
        bool refused = false;
        {
            RejectAllocationsScope reject;
            refused = !capacity.TryReserve(13u, &rejection, next, coexist);
        }
        Require(result, refused && rejectedAllocationCount == 0u &&
            rejection.requestedBytes == 13u && rejection.reservedBytes == 8u && rejection.limitBytes == 20u &&
            rejection.ownerCount == 16u && rejection.ownerListTruncated && rejection.requester.id == next.id &&
            rejection.owners[0].owner.id == original.id && original.id != next.id,
            "diagnostics.exact-rejection", "a refused check must retain exact A/U/M and bounded owner values without allocation");
        auto growth = capacity.TryReserveGrowth(9u, 16u, nullptr, next);
        Require(result, growth && growth->Bytes() == 12u && capacity.Snapshot().reservedBytes == 20u,
            "diagnostics.atomic-growth", "growth must choose its granted capacity using the same locked remaining capacity");
        growth.reset();
        live.reset();
        Require(result, rejection.reservedBytes == 8u && capacity.Snapshot().reservedBytes == 0u,
            "diagnostics.historical-u", "historical refusal values must survive subsequent owner releases");
    }
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{20u, 1u, 1u}, 20u, 1u, false, true});
        Require(result, run.BeginRun(), "diagnostics.begin", "the request must begin");
        bytestore::ByteStoreSession session;
        session.BindRun(run);
        auto owner = session.CreateSizedStore(bytestore::ByteStorePurpose::Contiguous, 8u, ::datacodec::MemoryDemandKind::RequiredContinuation, "payload");
        auto memory = std::dynamic_pointer_cast<bytestore::MemoryStore>(owner);
        Require(result, memory != nullptr, "diagnostics.memory", "the exact owner must be available");
        if (memory) {
            const auto identity = memory->DescribeOwner();
            auto shared = memory;
            const auto references = memory.use_count();
            bool rejected = false;
            ResourceDebugSnapshot snapshot;
            {
                RejectAllocationsScope reject;
                rejected = !memory->PrepareCapacity(13u, run, MemoryDemandKind::RequiredContinuation);
                rejected &= run.TryCopyResourceDebugSnapshot(snapshot);
            }
            Require(result, rejected && rejectedAllocationCount == 0u && snapshot.capacityRejection &&
                snapshot.capacityRejectionFatal && snapshot.failure && snapshot.failure->requestedBytes == 13u &&
                snapshot.capacityRejection->reservedBytes == 8u && snapshot.capacityRejection->ownerCount == 1u &&
                snapshot.capacityRejection->owners[0].owner.id == identity.owner.id &&
                snapshot.capacityRejection->requester.id == identity.owner.id &&
                snapshot.capacityRejection->requester.line != 0u &&
                snapshot.capturedAt >= snapshot.capacityRejection->checkedAt && memory.use_count() == references,
                "diagnostics.growth-owner", "growth refusal must describe the live old allocation without retaining another owner");
            shared.reset();
        }
        owner.reset();
        memory.reset();
        ResourceDebugSnapshot released;
        Require(result, run.TryCopyResourceDebugSnapshot(released) && released.storage.reservedBytes == 0u &&
            released.capacityRejection && released.capacityRejection->reservedBytes == 8u,
            "diagnostics.snapshot-time", "current capacity and historical refusal must use separate observations");
        run.CancelAndWaitRun();
        run.EndRun();
    }
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{0u, 1u, 1u}, 8u, 1u, false, true});
        run.BeginRun();
        auto capacity = run.StorageCapacity();
        resource::CapacityRejection rejection;
        const auto tag = capacity->NewOwner(resource::StorageOwnerPurpose::Optional, "optional-input");
        const auto epoch = run.EventEpoch();
        auto lease = capacity->TryReserve(8u, &rejection, tag);
        if (!lease) { run.RecordCapacityRejection(rejection, false); }
        ResourceDebugSnapshot snapshot;
        Require(result, !lease && run.TryCopyResourceDebugSnapshot(snapshot) && !run.Stopped() &&
            !run.FirstFailure() && run.EventEpoch() == epoch && snapshot.capacityRejection &&
            !snapshot.capacityRejectionFatal && snapshot.limits.ownedStorageLimitBytes == 0u,
            "diagnostics.optional-choice", "an optional memory refusal must not fail the request or publish a control notification");
        run.EndRun();
        run.BeginRun();
        Require(result, run.TryCopyResourceDebugSnapshot(snapshot) && !snapshot.capacityRejection,
            "diagnostics.request-boundary", "a new request must clear the previous refusal context");
        run.EndRun();
    }
    {
        DataCodecExecutionResources run(ResolvedResourceConfiguration{{8u, 1u, 2u}, 8u, 1u, true, true});
        run.BeginRun();
        run.BeginFlow();
        auto slot = run.TryAcquireSlot();
        std::latch started(1);
        std::latch release(1);
        auto work = std::make_shared<TerminalWork>([&](WorkerContext&) {
            started.count_down();
            release.wait();
            return true;
        });
        const bool submitted = slot && run.SubmitTerminal(*slot, work);
        Require(result, submitted, "diagnostics.wait-submit", "the terminal task must be admitted");
        if (submitted) {
            started.wait();
            const auto epoch = run.EventEpoch();
            run.SetWaitReason(ResourceWaitReason::OrderedCommit, &*slot);
            ResourceDebugSnapshot waiting;
            Require(result, run.TryCopyResourceDebugSnapshot(waiting) && waiting.waitContext.block == 0u &&
                waiting.waitContext.expected == ResourceWakeEvent::TerminalCompleted &&
                waiting.waitContext.completion == BlockCompletion::Running &&
                waiting.waitContext.requestId == waiting.requestId && waiting.waitContext.flowId == waiting.flowId &&
                run.EventEpoch() == epoch && !waiting.automaticObservationApplicable,
                "diagnostics.wait-identity", "the driver wait must identify its live block and actual completion event");
            release.count_down();
            while (run.Completion(*work) != BlockCompletion::Succeeded && !run.Stopped()) {
                const auto observed = run.EventEpoch();
                if (run.Completion(*work) != BlockCompletion::Succeeded) { run.WaitForChange(observed); }
            }
            Require(result, run.TryCopyResourceDebugSnapshot(waiting) &&
                waiting.waitContext.completion == BlockCompletion::Succeeded,
                "diagnostics.wait-live-state", "snapshots must refresh the completion of the same admission without dereferencing a waiter");
            run.SetWaitReason(ResourceWaitReason::OutputIO, &*slot);
            Require(result, run.TryCopyResourceDebugSnapshot(waiting) &&
                waiting.waitContext.expected == ResourceWakeEvent::WriterReturned,
                "diagnostics.wait-io", "output I/O must identify the writer return as its progress event");
            run.SetWaitReason(ResourceWaitReason::None);
            run.CommitSlot(*slot);
            slot.reset();
        }
        run.CancelAndWaitRun();
        run.EndRun();
    }
    return result;
}

}

#endif
