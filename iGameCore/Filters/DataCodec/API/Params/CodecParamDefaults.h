#ifndef DATACODEC_API_PARAMS_CODECPARAMDEFAULTS_H
#define DATACODEC_API_PARAMS_CODECPARAMDEFAULTS_H

#include "DataCodec/API/Params/DataCodecControlParams.h"

namespace datacodec {

[[nodiscard]] inline CompressorConfig MakeAbsoluteErrorNumericArrayCompressor(
    const double absoluteError) {
    CompressorConfig compressor;
    compressor.options["pressio:abs"] = absoluteError;
    return compressor;
}

[[nodiscard]] inline CompressorConfig MakeRelativeErrorNumericArrayCompressor(
    const double relativeError) {
    CompressorConfig compressor;
    compressor.options["pressio:rel"] = relativeError;
    return compressor;
}

[[nodiscard]] inline CompressorConfig MakeLosslessNumericArrayCompressor() {
    return MakeAbsoluteErrorNumericArrayCompressor(0.0);
}

[[nodiscard]] inline CompressorConfig MakeDefaultGeometryValueCompressor() {
    return MakeLosslessNumericArrayCompressor();
}

[[nodiscard]] inline CompressorConfig MakeDefaultAttributeValueCompressor() {
    return MakeLosslessNumericArrayCompressor();
}

[[nodiscard]] inline CodecControlParams MakeDefaultCodecControlParams() {
    CodecControlParams params;
    params.geomControl.regionControl = MakeSingleRegionPrecisionControl(MakeDefaultGeometryValueCompressor());
    params.defaultAttrControl.regionControl = MakeSingleRegionPrecisionControl(MakeDefaultAttributeValueCompressor());
    return params;
}


class CodecControlParamsFactory {
public:
    static EncodeCodecControlParams MakeDefault() { return MakeDefaultCodecControlParams(); }

    static DataCodecEncodeConfigurationParams MakeEncodeConfiguration(
        const DataCodecEncodeOptions& options,
        DataCodecRuntimeProfile profile = DataCodecRuntimeProfile::Native) {
        DataCodecEncodeConfigurationParams result;
        result.controlParams = MakeDefault();
        result.pipelineControl.cellOrder = EncodeCellOrderMode::Original;
        if (options.enableCompressionEnhancement) {
            auto& attribute = result.controlParams.attrReference.temporalField.predictor;
            auto& geometry = result.controlParams.geometryReference.temporalField.predictor;
            attribute.enableLocalWindowSearch = true;
            geometry.enableLocalWindowSearch = true;
            attribute.searchStrategy = TemporalPredictorSearchStrategy::ExhaustiveEstimatedBytes;
            geometry.searchStrategy = TemporalPredictorSearchStrategy::ExhaustiveEstimatedBytes;
            result.pipelineControl.cellOrder = EncodeCellOrderMode::Morton;
        }
        if (options.packageZstdLevel) { result.pipelineControl.packageFields.zstdLevel = *options.packageZstdLevel; }
        if (options.temporalKeyFrameInterval) {
            auto& attribute = result.controlParams.attrReference.temporalField;
            auto& geometry = result.controlParams.geometryReference.temporalField;
            attribute.keyFrameInterval = *options.temporalKeyFrameInterval;
            geometry.keyFrameInterval = *options.temporalKeyFrameInterval;
            attribute.forcePredFrames = false;
            geometry.forcePredFrames = false;
        }
        result.source.runtimeProfile = profile;
        result.language = options.language;
        return result;
    }

    static DataCodecDecodeConfigurationParams MakeDecodeConfiguration(
        const DataCodecDecodeOptions& options,
        DataCodecRuntimeProfile profile = DataCodecRuntimeProfile::Native) {
        DataCodecDecodeConfigurationParams result;
        if (options.validationProfile == DataCodecDecodeValidationProfile::Audit) {
            result.controlParams.SetDecodeValidationMode(DecodeValidationMode::Strict);
            result.controlParams.validation.validateTopologyReferences = true;
            result.controlParams.validation.validateFloatingPointValues = true;
        }
        result.decodedFrameCachePolicy.enabled = options.enableDecodedResultCache;
        result.encodedInputCachePolicy.enabled = options.enableEncodedInputCache;
        result.source.runtimeProfile = profile;
        result.logging = options.logging;
        result.language = options.language;
        return result;
    }
};

inline DataCodecEncodeConfigurationParams MakeDefaultEncodeConfigurationParams() {
    return CodecControlParamsFactory::MakeEncodeConfiguration({});
}
inline DataCodecDecodeConfigurationParams MakeDefaultDecodeConfigurationParams() {
    return CodecControlParamsFactory::MakeDecodeConfiguration({});
}
inline DataCodecDecodePackageConfigurationParams MakeDefaultDecodePackageConfigurationParams() {
    return MakeDefaultDecodeConfigurationParams().PackageConfiguration();
}
inline DataCodecEncodeConfigurationParams MakeEncodeConfigurationParams(
    const DataCodecEncodeOptions& options,
    DataCodecRuntimeProfile profile = DataCodecRuntimeProfile::Native) {
    return CodecControlParamsFactory::MakeEncodeConfiguration(options, profile);
}
inline DataCodecDecodeConfigurationParams MakeDecodeConfigurationParams(
    const DataCodecDecodeOptions& options,
    DataCodecRuntimeProfile profile = DataCodecRuntimeProfile::Native) {
    return CodecControlParamsFactory::MakeDecodeConfiguration(options, profile);
}
inline EncodeCodecControlParams MakeDefaultEncodeControlParams() {
    return MakeDefaultCodecControlParams();
}
inline DecodeControlParams MakeDefaultDecodeControlParams() { return {}; }

} // namespace datacodec

#endif
