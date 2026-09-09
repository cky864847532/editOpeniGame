#ifndef DATACODEC_API_PARAMS_CODECRESOURCEPARAMS_H
#define DATACODEC_API_PARAMS_CODECRESOURCEPARAMS_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace datacodec {

enum class CodecResourceMode : std::uint8_t { Fixed, Adaptive };

struct CodecResourceParams {
    CodecResourceMode mode{CodecResourceMode::Adaptive};
    std::optional<std::size_t> maxComputeThreads;
    // 清单内自有存储的容量上限，零表示禁止新增正容量
    std::optional<std::uint64_t> ownedStorageLimitBytes;
};

}

#endif
