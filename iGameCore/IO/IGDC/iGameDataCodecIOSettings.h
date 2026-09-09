#ifndef iGameDataCodecIOSettings_h
#define iGameDataCodecIOSettings_h

#include "DataCodec/API/Params/CodecParamDefaults.h"
#include "iGameMacro.h"

IGAME_NAMESPACE_BEGIN

class DataCodecIOSettings {
public:
    [[nodiscard]] static ::datacodec::DataCodecDecodeOptions GetDefaultDecodeOptions();
    static void SetDefaultDecodeOptions(const ::datacodec::DataCodecDecodeOptions& options);
    [[nodiscard]] static ::datacodec::CodecResourceParams GetDefaultDecodeResources();
    static void SetDefaultDecodeResources(const ::datacodec::CodecResourceParams& resources);
    [[nodiscard]] static bool GetDefaultLoadAllAvailableAttributes();
    static void SetDefaultLoadAllAvailableAttributes(bool loadAllAvailableAttributes);
};

IGAME_NAMESPACE_END

#endif
