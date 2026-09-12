#ifndef DATACODEC_RUNTIME_EXECUTION_DECODESTAGEMEMORY_H
#define DATACODEC_RUNTIME_EXECUTION_DECODESTAGEMEMORY_H

#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"
#include "DataCodec/Storage/ByteIO/ScratchByteBuffer.h"
#include "DataCodec/Storage/ByteIO/Window/WindowRuntimeParams.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

namespace datacodec {

inline bool PrepareDecodeStageMemory(DataCodecExecutionResources& run, std::size_t bytes,
    FixedByteBacking& backing, std::string* error) {
    backing = {};
    if (!run.IsDriverThread()) {
        return validation::AssignError(error, "decode stage capacity must be prepared by the driver");
    }
    for (;;) {
        run.ServiceDriverEvents();
        run.ReclaimOptionalStorage(bytes);
        const auto epoch = run.EventEpoch();
        if (run.Stopped()) { return validation::AssignError(error, "decode stage memory preparation cancelled"); }
        auto lease = run.TryAcquireStorage(bytes, MemoryDemandKind::RequiredContinuation);
        if (lease) {
            backing = run.Scratch().AllocateFixed(*run.StorageCapacity(), std::move(*lease));
            run.ClearByteWait();
            run.SetWaitReason(ResourceWaitReason::None);
            run.CompleteMemoryPreparation();
            return true;
        }
        if (!run.CheckNecessaryCapacity(bytes, nullptr, MemoryDemandKind::RequiredContinuation)) {
            return validation::AssignError(error, "decode stage memory capacity unavailable");
        }
        run.SetWaitReason(ResourceWaitReason::ByteCapacity);
        run.WaitForChange(epoch);
    }
}

inline std::size_t DecodeFieldWindowBytes(std::uint64_t encoded, std::uint64_t raw, bool zstd) noexcept {
    const auto input = zstd ? std::min<std::uint64_t>(encoded, kIoWindowBytes) : 0u;
    const auto output = zstd && encoded != 0u ? std::max<std::uint64_t>(1u, std::min<std::uint64_t>(raw, kIoWindowBytes)) :
        std::min<std::uint64_t>(raw, kIoWindowBytes);
    return static_cast<std::size_t>(input + output);
}

}
#endif
