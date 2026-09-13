#include "DataCodec/Platform/Wasm/WasmRuntime.h"

#include <algorithm>
#include <limits>
#include "DataCodec/Runtime/Execution/DataCodecResourceController.h"

namespace datacodec::wasm {

WasmResourceDefaults GetWasmResourceDefaults() {
    const auto sample = ProbeResources();
    const auto config = ResolveResourceConfiguration({.mode = CodecResourceMode::Fixed, .threadMode = CodecThreadMode::Fixed}, sample);
    return {*config.initialLimits.ownedStorageLimitBytes,
        sample.hardLimitBytes.value_or(std::numeric_limits<std::uint64_t>::max()), config.computeCeiling};
}

void ValidateWasmResourceParams(const CodecResourceParams& resources) {
    (void)ResolveResourceConfiguration(resources, ProbeResources());
}

WasmRuntimeCapabilities DetectWasmRuntimeCapabilities() noexcept {
    WasmRuntimeCapabilities capabilities;
#if defined(__EMSCRIPTEN__)
    capabilities.emscripten = true;
#endif
#if defined(__EMSCRIPTEN_PTHREADS__)
    capabilities.pthreadsAvailable = true;
#endif
    capabilities.pointerBytes = sizeof(void*);
    capabilities.runtimeProfile = sizeof(void*) > 4u
        ? DataCodecRuntimeProfile::Wasm16GiB
        : DataCodecRuntimeProfile::Wasm4GiB;
    capabilities.maximumDataCodecWorkers = ResourceComputeCapacity(
        ProbeResources(), CodecResourceMode::Adaptive);
    return capabilities;
}

DataCodecDecodeConfigurationParams MakeWasmDecodeConfiguration(
    const bool enableReuseCache) {
    const auto capabilities = DetectWasmRuntimeCapabilities();
    auto controls = MakeDecodeConfigurationParams(
        DataCodecDecodeOptions{
            .enableDecodedResultCache = enableReuseCache,
            .enableEncodedInputCache = enableReuseCache,
        }, capabilities.runtimeProfile);
    controls.decodedFrameCachePolicy.prefetchEnabled = false;
    return controls;
}

DataCodecEncodeConfigurationParams MakeWasmEncodeConfiguration() {
    const auto capabilities = DetectWasmRuntimeCapabilities();
    auto controls = MakeEncodeConfigurationParams(
        DataCodecEncodeOptions{},
        capabilities.runtimeProfile);
    return controls;
}

} // namespace datacodec::wasm
