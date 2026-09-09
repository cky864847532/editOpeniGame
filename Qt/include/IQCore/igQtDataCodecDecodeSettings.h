#ifndef igQtDataCodecDecodeSettings_h
#define igQtDataCodecDecodeSettings_h

#include "IQCore/igQtExportModule.h"

#include <DataCodec/API/Params/CodecResourceParams.h>

#include <cstddef>

class QSettings;

struct IG_QT_MODULE_EXPORT igQtDataCodecDecodeSettings {
    ::datacodec::CodecResourceParams resources;
    bool decodeAttributesOnDemand{false};
    bool enableDecodedResultCache{false};
    bool outputDecodeLogFile{false};
};

class IG_QT_MODULE_EXPORT igQtDataCodecDecodeSettingsStore final {
public:
    [[nodiscard]] static igQtDataCodecDecodeSettings Load();
    static void Save(const igQtDataCodecDecodeSettings& settings);
    static void Apply(const igQtDataCodecDecodeSettings& settings);
    [[nodiscard]] static ::datacodec::CodecResourceParams LoadResources(QSettings& storage);
    static void SaveResources(QSettings& storage, const ::datacodec::CodecResourceParams& resources);
};

#endif
