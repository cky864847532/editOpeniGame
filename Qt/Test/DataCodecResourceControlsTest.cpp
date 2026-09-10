#include "IQWidgets/igQtDataCodecResourceControls.h"
#include "IQWidgets/igQtDataCodecCompressionWidget.h"
#include "IQCore/igQtDataCodecDecodeSettings.h"
#include <IGDC/iGameDataCodecIOSettings.h>
#include <iGamePointSet.h>
#include <iGameAttributeSet.h>
#include <iGameFlatArray.h>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    bool passed = true;
    const auto require = [&](bool valid, const char* message) {
        if (!valid) { passed = false; std::cerr << message << '\n'; }
    };
    constexpr std::uint64_t MiB = 1024u * 1024u;
    igQtDataCodecResourceControls controls;
    auto* modes = controls.findChild<QComboBox*>(QStringLiteral("DataCodecResourceMode"));
    auto* threads = controls.findChild<QSpinBox*>(QStringLiteral("DataCodecComputeLimit"));
    auto* storage = controls.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecOwnedStorageLimit"));
    if (!modes || !threads || !storage) { std::cerr << "resource controls are missing\n"; return 1; }
    require(modes->count() == 2 && !modes->isEditable(), "mode must use a two-option noneditable combo box");
    auto params = controls.Params();
    require(params.mode == datacodec::CodecResourceMode::Adaptive && !params.maxComputeThreads &&
        !params.ownedStorageLimitBytes, "startup defaults must remain unspecified");
    std::size_t notifications = 0u;
    controls.OnChanged([&] { ++notifications; });
    controls.SetParams({datacodec::CodecResourceMode::Fixed, 4u, 512u * MiB});
    params = controls.Params();
    require(notifications == 0u && params.mode == datacodec::CodecResourceMode::Fixed &&
        params.maxComputeThreads == 4u && params.ownedStorageLimitBytes == 512u * MiB,
        "loading startup parameters must not trigger an edit notification");
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Adaptive)));
    require(notifications == 1u && controls.Params().maxComputeThreads == 4u &&
        controls.Params().ownedStorageLimitBytes == 512u * MiB, "changing mode must preserve explicit limits");
    storage->setValue(0.0);
    require(controls.Params().ownedStorageLimitBytes == 0u, "zero capacity must remain an explicit limit");
    storage->setValue(-1.0);
    threads->setValue(0);
    require(!controls.Params().ownedStorageLimitBytes && !controls.Params().maxComputeThreads,
        "device defaults must remove the explicit limits");
    controls.setEnabled(false);
    require(!modes->isEnabled() && !threads->isEnabled() && !storage->isEnabled(),
        "disabling resources with no input must disable all child controls");
    controls.setEnabled(true);
    require(modes->isEnabled() && threads->isEnabled() && storage->isEnabled(),
        "resource controls must become editable when enabled");
    controls.SetParams({datacodec::CodecResourceMode::Fixed, 2u, std::numeric_limits<std::uint64_t>::max()});
    require(controls.Params().ownedStorageLimitBytes ==
        (std::numeric_limits<std::uint64_t>::max() / MiB) * MiB, "maximum MiB conversion must not overflow");

    // 配置只写临时文件，不改变用户保存的窗口设置
    QTemporaryDir directory;
    if (!directory.isValid()) { std::cerr << "temporary settings directory unavailable\n"; return 1; }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    {
        igQtDataCodecCompressionWidget compression;
        compression.SetModel({});
        compression.resize(1000, 800);
        compression.show();
        QApplication::processEvents();
        auto* mode = compression.findChild<QComboBox*>(QStringLiteral("DataCodecResourceMode"));
        auto* compute = compression.findChild<QSpinBox*>(QStringLiteral("DataCodecComputeLimit"));
        auto* capacity = compression.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecOwnedStorageLimit"));
        require(mode && compute && capacity && !mode->isEnabled() && !compute->isEnabled() && !capacity->isEnabled(),
            "the real encoding panel must display disabled resource controls without a model");
        auto points = iGame::Points::New();
        points->AddPoint(0.0f, 1.0f, 2.0f);
        points->AddPoint(3.0f, 4.0f, 5.0f);
        auto pointSet = iGame::PointSet::New();
        pointSet->SetPoints(points);
        auto values = iGame::FloatArray::New();
        values->SetName("resource_control_scalar");
        values->SetDimension(1);
        const float samples[]{1.0f, 2.0f};
        values->AddElement(samples);
        values->AddElement(samples + 1);
        auto attributes = iGame::AttributeSet::New();
        attributes->AddAttribute(IG_SCALAR, IG_POINT, values);
        pointSet->SetAttributeSet(attributes);
        auto model = iGame::Model::New();
        model->SetDataObject(pointSet);
        compression.SetModel(model);
        QApplication::processEvents();
        require(mode && compute && capacity && mode->isEnabled() && compute->isEnabled() && capacity->isEnabled(),
            "loading a real point model must enable the encoding resource controls");
        if (mode && compute && capacity) {
            mode->setCurrentIndex(mode->findData(static_cast<int>(datacodec::CodecResourceMode::Fixed)));
            compute->setValue(3);
            capacity->setValue(64.0);
            QSettings saved(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("iGame"), QStringLiteral("iGameVis"));
            saved.beginGroup(QStringLiteral("DataCodec/Encode"));
            const auto selected = igQtDataCodecDecodeSettingsStore::LoadResources(saved);
            require(selected.mode == datacodec::CodecResourceMode::Fixed && selected.maxComputeThreads == 3u &&
                selected.ownedStorageLimitBytes == 64u * MiB,
                "editing real encoding controls must persist the exact next-request resource parameters");
        }
        compression.SetModel({});
        require(mode && compute && capacity && !mode->isEnabled() && !compute->isEnabled() && !capacity->isEnabled(),
            "removing the model must disable the resource controls again");
        compression.close();
    }
    const auto settingsPath = directory.filePath(QStringLiteral("resource.ini"));
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        const datacodec::CodecResourceParams explicitParams{datacodec::CodecResourceMode::Fixed, 7u, 0u};
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, explicitParams);
        settings.sync();
        require(settings.status() == QSettings::NoError, "resource settings must persist successfully");
    }
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        params = igQtDataCodecDecodeSettingsStore::LoadResources(settings);
        require(params.mode == datacodec::CodecResourceMode::Fixed && params.maxComputeThreads == 7u &&
            params.ownedStorageLimitBytes == 0u, "a fresh settings reader must preserve fixed mode and explicit zero");
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, {});
        params = igQtDataCodecDecodeSettingsStore::LoadResources(settings);
        require(params.mode == datacodec::CodecResourceMode::Adaptive && !params.maxComputeThreads &&
            !params.ownedStorageLimitBytes && !settings.contains(QStringLiteral("MaxComputeThreads")) &&
            !settings.contains(QStringLiteral("OwnedStorageLimitBytes")), "saving defaults must remove explicit resource keys");
    }
    const auto original = iGame::DataCodecIOSettings::GetDefaultDecodeResources();
    igQtDataCodecDecodeSettings selected;
    selected.resources = {datacodec::CodecResourceMode::Fixed, 3u, 128u * MiB};
    igQtDataCodecDecodeSettingsStore::Apply(selected);
    const auto applied = iGame::DataCodecIOSettings::GetDefaultDecodeResources();
    require(applied.mode == selected.resources.mode && applied.maxComputeThreads == 3u &&
        applied.ownedStorageLimitBytes == 128u * MiB, "decode settings must publish the next request resource parameters");
    iGame::DataCodecIOSettings::SetDefaultDecodeResources(original);
    return passed ? 0 : 1;
}
