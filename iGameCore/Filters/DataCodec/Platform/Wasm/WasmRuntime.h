#ifndef DATACODEC_PLATFORM_WASM_WASMRUNTIME_H
#define DATACODEC_PLATFORM_WASM_WASMRUNTIME_H

#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "DataCodec/API/Params/CodecResourceParams.h"

#include <cstddef>
#include <cstdint>

namespace datacodec::wasm {

struct WasmRuntimeCapabilities {
    bool emscripten{false};
    bool pthreadsAvailable{false};
    std::size_t pointerBytes{sizeof(void*)};
    std::size_t maximumDataCodecWorkers{1u};
    DataCodecRuntimeProfile runtimeProfile{DataCodecRuntimeProfile::Wasm4GiB};
};

[[nodiscard]] WasmRuntimeCapabilities DetectWasmRuntimeCapabilities() noexcept;

struct WasmResourceDefaults {
    std::uint64_t fixedMemoryBytes{0u};
    std::uint64_t maximumMemoryBytes{0u};
    std::size_t maximumComputeThreads{1u};
};

// 页面查询和启动参数验证共用核心规则，不访问内部控制器
[[nodiscard]] WasmResourceDefaults GetWasmResourceDefaults();
void ValidateWasmResourceParams(const CodecResourceParams& resources);

[[nodiscard]] DataCodecDecodeConfigurationParams MakeWasmDecodeConfiguration(
    bool enableReuseCache = true);

[[nodiscard]] DataCodecEncodeConfigurationParams MakeWasmEncodeConfiguration();

} // namespace datacodec::wasm

#endif
