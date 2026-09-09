#ifndef DATACODEC_API_PARAMS_DATACODECCONTROLPARAMS_H
#define DATACODEC_API_PARAMS_DATACODECCONTROLPARAMS_H

#include "DataCodec/API/Params/CodecControlParams.h"
#include "DataCodec/API/Params/CodecResourceParams.h"
#include "DataCodec/API/Params/DecodedFrameCacheParams.h"
#include "DataCodec/API/Params/EncodedInputCacheParams.h"
#include "DataCodec/API/Params/EncodePipelineParams.h"
#include "DataCodec/API/Adapter/IDecodeTopologyBlockObserver.h"
#include "DataCodec/Localization/DataCodecLanguage.h"
#include "DataCodec/Validation/Policy/CodecValidationPolicy.h"

#include <memory>
#include <optional>

namespace datacodec {

using EncodeCodecControlParams = CodecControlParams;

struct DecodeControlParams {
    CodecValidationPolicy validation;
    DecodeControlParams& SetDecodeValidationMode(DecodeValidationMode mode) noexcept {
        validation.decodeMode = mode;
        return *this;
    }
};

enum class DataCodecDecodeValidationProfile { Required, Audit };

// 运行平台标识仅用于描述配置来源，硬能力由 ResourceProbe 提供
enum class DataCodecRuntimeProfile { Native, Wasm4GiB, Wasm16GiB };

inline const char* DataCodecRuntimeProfileName(DataCodecRuntimeProfile profile) noexcept {
    switch (profile) {
        case DataCodecRuntimeProfile::Native: return "Native";
        case DataCodecRuntimeProfile::Wasm4GiB: return "Wasm4GiB";
        case DataCodecRuntimeProfile::Wasm16GiB: return "Wasm16GiB";
    }
    return "Unknown";
}

struct DataCodecEncodeOptions {
    bool enableCompressionEnhancement{false};
    std::optional<int> packageZstdLevel;
    std::optional<std::uint32_t> temporalKeyFrameInterval;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
};

struct DataCodecDecodeLogParams {
    bool enableFileLog{false};
    bool enableConsoleLog{true};
};

struct DataCodecDecodeOptions {
    DataCodecDecodeValidationProfile validationProfile{DataCodecDecodeValidationProfile::Required};
    bool enableDecodedResultCache{true};
    bool enableEncodedInputCache{false};
    DataCodecDecodeLogParams logging;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
};

struct DataCodecEncodeConfigurationSource {
    DataCodecRuntimeProfile runtimeProfile{DataCodecRuntimeProfile::Native};
};

struct DataCodecDecodeConfigurationSource {
    DataCodecRuntimeProfile runtimeProfile{DataCodecRuntimeProfile::Native};
};

enum class TopologyDecodeOutputMode : std::uint8_t { CommitToAdapter, ObserverOnly };

struct DecodeExecutionOptions {
    TopologyDecodeOutputMode topologyOutputMode{TopologyDecodeOutputMode::CommitToAdapter};
    std::shared_ptr<IDecodeTopologyBlockObserver> topologyBlockObserver;
};

struct DataCodecEncodeConfigurationParams {
    EncodeCodecControlParams controlParams;
    EncodePipelineControlParams pipelineControl;
    DataCodecEncodeConfigurationSource source;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
};

struct DataCodecDecodePackageConfigurationParams {
    DecodeControlParams controlParams;
    DecodeExecutionOptions execution;
    DataCodecDecodeConfigurationSource source;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};
};

struct DataCodecDecodeConfigurationParams {
    DecodeControlParams controlParams;
    DecodeExecutionOptions execution;
    DecodedFrameCachePolicy decodedFrameCachePolicy;
    EncodedInputCachePolicy encodedInputCachePolicy;
    DataCodecDecodeConfigurationSource source;
    DataCodecDecodeLogParams logging;
    DataCodecLanguage language{DataCodecLanguage::SimplifiedChinese};

    DataCodecDecodePackageConfigurationParams PackageConfiguration() const {
        return {.controlParams = controlParams, .execution = execution, .source = source, .language = language};
    }
};

}

#endif
