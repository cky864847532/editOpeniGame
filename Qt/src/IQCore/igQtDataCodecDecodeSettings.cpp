#include "IQCore/igQtDataCodecDecodeSettings.h"

#include <IGDC/iGameDataCodecIOSettings.h>

#include <QSettings>
#include <limits>
#include <cmath>

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
    switch (mode) {
    case static_cast<int>(::datacodec::CodecResourceMode::Fixed):
        resources.mode = ::datacodec::CodecResourceMode::Fixed; break;
    case static_cast<int>(::datacodec::CodecResourceMode::Unlimited):
        resources.mode = ::datacodec::CodecResourceMode::Unlimited; break;
    default: resources.mode = ::datacodec::CodecResourceMode::Adaptive; break;
    }
    bool valid = false;
    if (storage.value(QStringLiteral("ThreadMode")).toInt() == static_cast<int>(::datacodec::CodecThreadMode::Adaptive)) {
        resources.threadMode = ::datacodec::CodecThreadMode::Adaptive;
        const auto idle = storage.value(QStringLiteral("TargetCpuIdleRatio"), 0.2).toDouble(&valid);
        resources.targetCpuIdleRatio = valid && std::isfinite(idle) && idle >= 0.0 && idle < 1.0 ? idle : 0.2;
    }
    const auto threads = storage.value(QStringLiteral("MaxComputeThreads")).toULongLong(&valid);
    if (resources.threadMode == ::datacodec::CodecThreadMode::Fixed && valid && threads > 0u && threads <= std::numeric_limits<std::size_t>::max()) {
        resources.maxComputeThreads = static_cast<std::size_t>(threads);
    }
    const auto bytes = storage.value(QStringLiteral("OwnedStorageLimitBytes")).toULongLong(&valid);
    if (valid && resources.mode == ::datacodec::CodecResourceMode::Fixed) {
        resources.ownedStorageLimitBytes = bytes;
    }
    if (resources.mode == ::datacodec::CodecResourceMode::Adaptive) {
        const bool migrated = storage.value(QStringLiteral("ResourceSettingsVersion"), 0).toInt() >= 2;
        const auto ratio = migrated ? storage.value(QStringLiteral("TargetAvailableMemoryRatio"), 0.20).toDouble(&valid) : 0.20;
        resources.targetAvailableMemoryRatio = (!migrated || (valid && std::isfinite(ratio) && ratio >= 0.0 && ratio < 1.0)) ? ratio : 0.20;
    }
    // 旧 Adaptive 字节额度直接删除，迁移版本与有效参数一起保存
    SaveResources(storage, resources);
    return resources;
}

void igQtDataCodecDecodeSettingsStore::SaveResources(
    QSettings& storage, const ::datacodec::CodecResourceParams& resources) {
    storage.setValue(QStringLiteral("ResourceMode"), static_cast<int>(resources.mode));
    storage.setValue(QStringLiteral("ResourceSettingsVersion"), 2);
    storage.setValue(QStringLiteral("ThreadMode"), static_cast<int>(resources.threadMode));
    if (resources.threadMode == ::datacodec::CodecThreadMode::Adaptive && resources.targetCpuIdleRatio) {
        storage.setValue(QStringLiteral("TargetCpuIdleRatio"), *resources.targetCpuIdleRatio);
    } else { storage.remove(QStringLiteral("TargetCpuIdleRatio")); }
    if (resources.threadMode == ::datacodec::CodecThreadMode::Fixed && resources.maxComputeThreads) {
        storage.setValue(QStringLiteral("MaxComputeThreads"), static_cast<qulonglong>(*resources.maxComputeThreads));
    } else { storage.remove(QStringLiteral("MaxComputeThreads")); }
    if (resources.mode == ::datacodec::CodecResourceMode::Fixed && resources.ownedStorageLimitBytes) {
        storage.setValue(QStringLiteral("OwnedStorageLimitBytes"), static_cast<qulonglong>(*resources.ownedStorageLimitBytes));
    } else { storage.remove(QStringLiteral("OwnedStorageLimitBytes")); }
    if (resources.mode == ::datacodec::CodecResourceMode::Adaptive) {
        storage.setValue(QStringLiteral("TargetAvailableMemoryRatio"), resources.targetAvailableMemoryRatio.value_or(0.20));
    } else { storage.remove(QStringLiteral("TargetAvailableMemoryRatio")); }
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
