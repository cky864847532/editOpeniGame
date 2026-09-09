#include "IQCore/igQtDataCodecDecodeSettings.h"

#include <IGDC/iGameDataCodecIOSettings.h>

#include <QSettings>
#include <limits>

namespace {

constexpr auto kSettingsOrganization = "iGame";
constexpr auto kSettingsApplication = "iGameVis";
constexpr auto kDecodeSettingsGroup = "DataCodec/Decode";
constexpr auto kDecodeAttributesOnDemandKey = "DecodeAttributesOnDemand";
constexpr auto kEnableDecodedResultCacheKey = "EnableDecodedResultCache";
constexpr auto kOutputDecodeLogFileKey = "OutputDecodeLogFile";

QSettings CreateSettings() {
    return QSettings(
        QSettings::IniFormat,
        QSettings::UserScope,
        QString::fromLatin1(kSettingsOrganization),
        QString::fromLatin1(kSettingsApplication));
}

}

::datacodec::CodecResourceParams igQtDataCodecDecodeSettingsStore::LoadResources(QSettings& storage) {
    ::datacodec::CodecResourceParams resources;
    const auto mode = storage.value(QStringLiteral("ResourceMode"),
        static_cast<int>(::datacodec::CodecResourceMode::Adaptive)).toInt();
    resources.mode = mode == static_cast<int>(::datacodec::CodecResourceMode::Fixed)
        ? ::datacodec::CodecResourceMode::Fixed : ::datacodec::CodecResourceMode::Adaptive;
    bool valid = false;
    const auto threads = storage.value(QStringLiteral("MaxComputeThreads")).toULongLong(&valid);
    if (valid && threads > 0u && threads <= std::numeric_limits<std::size_t>::max()) {
        resources.maxComputeThreads = static_cast<std::size_t>(threads);
    }
    const auto bytes = storage.value(QStringLiteral("OwnedStorageLimitBytes")).toULongLong(&valid);
    if (valid) { resources.ownedStorageLimitBytes = bytes; }
    return resources;
}

void igQtDataCodecDecodeSettingsStore::SaveResources(
    QSettings& storage, const ::datacodec::CodecResourceParams& resources) {
    storage.setValue(QStringLiteral("ResourceMode"), static_cast<int>(resources.mode));
    if (resources.maxComputeThreads) {
        storage.setValue(QStringLiteral("MaxComputeThreads"), static_cast<qulonglong>(*resources.maxComputeThreads));
    } else { storage.remove(QStringLiteral("MaxComputeThreads")); }
    if (resources.ownedStorageLimitBytes) {
        storage.setValue(QStringLiteral("OwnedStorageLimitBytes"), static_cast<qulonglong>(*resources.ownedStorageLimitBytes));
    } else { storage.remove(QStringLiteral("OwnedStorageLimitBytes")); }
}

igQtDataCodecDecodeSettings igQtDataCodecDecodeSettingsStore::Load() {
    auto storage = CreateSettings();
    storage.beginGroup(QString::fromLatin1(kDecodeSettingsGroup));
    const auto resources = LoadResources(storage);
    const auto decodeAttributesOnDemand = storage.value(
        QString::fromLatin1(kDecodeAttributesOnDemandKey),
        false).toBool();
    const auto enableDecodedResultCache = storage.value(
        QString::fromLatin1(kEnableDecodedResultCacheKey),
        false).toBool();
    const auto outputDecodeLogFile = storage.value(
        QString::fromLatin1(kOutputDecodeLogFileKey),
        false).toBool();
    storage.endGroup();
    return igQtDataCodecDecodeSettings{
        .resources = resources,
        .decodeAttributesOnDemand = decodeAttributesOnDemand,
        .enableDecodedResultCache = enableDecodedResultCache,
        .outputDecodeLogFile = outputDecodeLogFile,
    };
}

void igQtDataCodecDecodeSettingsStore::Save(
    const igQtDataCodecDecodeSettings& settings) {
    auto storage = CreateSettings();
    storage.beginGroup(QString::fromLatin1(kDecodeSettingsGroup));
    SaveResources(storage, settings.resources);
    storage.setValue(
        QString::fromLatin1(kDecodeAttributesOnDemandKey),
        settings.decodeAttributesOnDemand);
    storage.setValue(
        QString::fromLatin1(kEnableDecodedResultCacheKey),
        settings.enableDecodedResultCache);
    storage.setValue(
        QString::fromLatin1(kOutputDecodeLogFileKey),
        settings.outputDecodeLogFile);
    storage.endGroup();
    storage.sync();
    Apply(settings);
}

void igQtDataCodecDecodeSettingsStore::Apply(
    const igQtDataCodecDecodeSettings& settings) {
    auto options = iGame::DataCodecIOSettings::GetDefaultDecodeOptions();
    options.enableDecodedResultCache = settings.enableDecodedResultCache;
    options.logging.enableFileLog = settings.outputDecodeLogFile;
    iGame::DataCodecIOSettings::SetDefaultDecodeOptions(options);
    iGame::DataCodecIOSettings::SetDefaultDecodeResources(settings.resources);
    iGame::DataCodecIOSettings::SetDefaultLoadAllAvailableAttributes(
        !settings.decodeAttributesOnDemand);
}
