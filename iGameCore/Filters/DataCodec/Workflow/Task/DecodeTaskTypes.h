#ifndef DATACODEC_WORKFLOW_TASK_DECODETASKTYPES_H
#define DATACODEC_WORKFLOW_TASK_DECODETASKTYPES_H

#include "DataCodec/Runtime/Execution/ParallelExecution.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace datacodec
{

enum class DecodeTaskState : std::uint8_t {
    Queued = 0u,
    Running = 1u,
    Succeeded = 2u,
    Failed = 3u,
    Cancelled = 4u,
};

struct DecodeTaskKey {
    std::uint64_t scope{0u};
    std::uint32_t frameIndex{0u};
    std::string variant;

    bool operator==(const DecodeTaskKey&) const = default;
};

[[nodiscard]] inline bool IsTerminalDecodeTaskState(const DecodeTaskState state) noexcept {
    return state == DecodeTaskState::Succeeded || state == DecodeTaskState::Failed ||
           state == DecodeTaskState::Cancelled;
}

} // namespace datacodec

#endif
