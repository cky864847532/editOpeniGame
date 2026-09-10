#ifndef DATACODEC_RUNTIME_EXECUTION_MORTONREMAPRESOURCERESERVATION_H
#define DATACODEC_RUNTIME_EXECUTION_MORTONREMAPRESOURCERESERVATION_H

#include "DataCodec/Codec/Remap/Common/MortonRemapBuilder.h"
#include "DataCodec/Runtime/Execution/DataCodecResourceTaskRunner.h"
#include "DataCodec/Storage/ByteIO/ByteBudget.h"
#include "DataCodec/Validation/Common/DataCodecValidation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>
#include <string>

namespace datacodec {

struct MortonRemapResourceReservation {
    DataCodecTaskMemoryReservation memoryReservation;
    resource::ActiveByteBudget* legacyScratchBudget{nullptr};
};

inline bool AcquireMortonRemapResourceReservation(
    IParallelTaskRunner* parallelTaskRunner,
    const std::stop_token stopToken,
    const std::size_t elementCount,
    const bool buildInverse,
    const std::size_t leafBudgetBytes,
    const std::size_t runBufferBytes,
    const bool useMemoryStorage,
    resource::ActiveByteBudget& legacyScratchBudget,
    MortonRemapResourceReservation& reservation,
    std::string* error = nullptr) {
    reservation = {};
    auto* resourceTaskRunner = dynamic_cast<IDataCodecResourceTaskRunner*>(
        parallelTaskRunner);
    if (resourceTaskRunner == nullptr) {
        reservation.legacyScratchBudget = &legacyScratchBudget;
        return true;
    }

    const auto reservationBytes = mortonremap::EstimateMortonTaskReservationBytes(
        elementCount,
        buildInverse,
        leafBudgetBytes,
        runBufferBytes,
        useMemoryStorage,
        useMemoryStorage);
    try {
        reservation.memoryReservation = resourceTaskRunner->AcquireMemoryReservation(
            reservationBytes,
            stopToken);
        return true;
    } catch (const std::exception& exception) {
        return validation::AssignError(
            error,
            std::string("Morton remap memory reservation failed: ") + exception.what());
    } catch (...) {
        return validation::AssignError(error, "Morton remap memory reservation failed");
    }
}

} // 命名空间 datacodec

#endif
