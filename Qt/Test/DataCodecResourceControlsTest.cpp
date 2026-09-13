#include "IQWidgets/igQtDataCodecResourceControls.h"
#include "IQWidgets/igQtDataCodecCompressionWidget.h"
#include "IQCore/igQtDataCodecDecodeSettings.h"
#include <IGDC/iGameDataCodecIOSettings.h>
#include <iGamePointSet.h>
#include <iGameAttributeSet.h>
#include <iGameFlatArray.h>
#include <IGDC/iGameIGDCWriter.h>
#include <IGDC/iGameIGDCReader.h>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    bool passed = true;
    const auto require = [&](bool valid, const char* message) {
        if (!valid) { passed = false; std::cerr << message << '\n'; }
    };
    const auto waitUntil = [](const auto& predicate) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        return predicate();
    };
    constexpr std::uint64_t MiB = 1024u * 1024u;
    igQtDataCodecResourceControls controls;
    auto* modes = controls.findChild<QComboBox*>(QStringLiteral("DataCodecResourceMode"));
    auto* threads = controls.findChild<QSpinBox*>(QStringLiteral("DataCodecComputeLimit"));
    auto* storage = controls.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecOwnedStorageLimit"));
    auto* reserve = controls.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecTargetAvailableMemory"));
    if (!modes || !threads || !storage || !reserve) { std::cerr << "resource controls are missing\n"; return 1; }
    require(modes->count() == 3 && !modes->isEditable(), "mode must use a three-option noneditable combo box");
    auto params = controls.Params();
    require(params.mode == datacodec::CodecResourceMode::Adaptive && !params.maxComputeThreads &&
        !params.ownedStorageLimitBytes && params.targetAvailableMemoryRatio == 0.20,
        "Adaptive starts with the 20 percent physical reserve target");
    std::size_t notifications = 0u;
    controls.OnChanged([&] { ++notifications; });
    controls.SetParams({datacodec::CodecResourceMode::Fixed, 4u, 512u * MiB});
    params = controls.Params();
    require(notifications == 0u && params.mode == datacodec::CodecResourceMode::Fixed &&
        params.maxComputeThreads == 4u && params.ownedStorageLimitBytes == 512u * MiB,
        "loading startup parameters must not trigger an edit notification");
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Adaptive)));
    require(notifications == 1u && controls.Params().maxComputeThreads == 4u &&
        !controls.Params().ownedStorageLimitBytes && !reserve->isHidden() && storage->isHidden(),
        "Adaptive exposes only the reserve ratio and preserves the fixed CPU limit");
    reserve->setValue(30.0);
    require(controls.Params().targetAvailableMemoryRatio == 0.30, "percent input must map to the physical reserve ratio");
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Fixed)));
    require(!controls.Params().targetAvailableMemoryRatio && reserve->isHidden() && !storage->isHidden(),
        "Fixed exposes only the byte limit");
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
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Unlimited)));
    require(!storage->isEnabled() && threads->isEnabled() && !controls.Params().ownedStorageLimitBytes &&
        controls.Params().maxComputeThreads == 2u, "Unlimited disables memory input and preserves threads");
    controls.setEnabled(false);
    controls.setEnabled(true);
    require(!storage->isEnabled(), "parent enable must not reenable Unlimited memory input");
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Fixed)));
    require(storage->isEnabled() && controls.Params().ownedStorageLimitBytes.has_value(),
        "switching back restores the entered fixed budget");

    // 配置只写临时文件，不改变用户保存的窗口设置
    auto* threadMode = controls.findChild<QComboBox*>(QStringLiteral("DataCodecThreadMode"));
    auto* idle = controls.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecTargetCpuIdle"));
    if (!threadMode || !idle) { std::cerr << "CPU controls are missing\n"; return 1; }
    require(threadMode->count() == 2 && !threadMode->isEditable() && !idle->isEnabled(), "thread mode defaults to Fixed");
    threadMode->setCurrentIndex(threadMode->findData(static_cast<int>(datacodec::CodecThreadMode::Adaptive)));
    idle->setValue(25.0);
    require(!threads->isEnabled() && idle->isEnabled() && !controls.Params().maxComputeThreads &&
        controls.Params().targetCpuIdleRatio == 0.25, "Adaptive edits the system idle target independently of memory");
    controls.setEnabled(false);
    require(!threadMode->isEnabled() && !idle->isEnabled(), "disabled parent disables CPU controls");
    controls.setEnabled(true);
    require(!threads->isEnabled() && idle->isEnabled(), "parent enable preserves Adaptive input state");

    const auto savedControlParams = controls.Params();
    controls.SetParams({datacodec::CodecResourceMode::Fixed, 1u, 0u});
    bool advisory = false;
    ::datacodec::DataCodecStatusRecord storageStatus;
    controls.OnStorageStatus([&](const ::datacodec::DataCodecStatusRecord& status) { storageStatus = status; });
    controls.SetStorageAnalyzer([&] {
        const auto required = !advisory;
        return [required](std::stop_token) { return igQtDataCodecResourceControls::StorageCheck{
            .minimumBytes = MiB + 1u, .required = required,
            .messageId = iGame::iGameDataCodecHostMessageId::EncodeStorageCheckScope}; };
    });
    controls.show();
    storage->setFocus();
    QApplication::processEvents();
    storage->findChild<QLineEdit*>()->setText(QStringLiteral("0"));
    threads->setFocus();
    require(waitUntil([&] { return controls.StorageRejected(); }), "leaving the memory input must run capacity analysis");
    require(storage->property("storageInsufficient").toBool() && controls.StorageStatusLabel()->text().contains("2 MiB"),
        "proven insufficiency must mark input and round the minimum upward");
    require(storageStatus.severity == datacodec::DataCodecStatusSeverity::Error,
        "a capacity rejection must reach the output callback as Error");
    storage->setValue(2.0);
    QMetaObject::invokeMethod(storage, "editingFinished");
    require(waitUntil([&] { return controls.StorageStatusLabel()->text().contains(QStringLiteral("未低于")); }) &&
        !controls.StorageRejected() && !storage->property("storageInsufficient").toBool(),
        "correcting the limit must clear the red text");
    require(storageStatus.severity == datacodec::DataCodecStatusSeverity::Info,
        "a sufficient capacity check must reach the output callback as Info");
    advisory = true;
    storage->setValue(0.0);
    controls.CheckStorage();
    require(waitUntil([&] { return controls.StorageStatusLabel()->text().contains(QStringLiteral("临时文件")); }) &&
        !controls.StorageRejected(), "spill-capable decode advice must not claim proven failure");
    require(storageStatus.severity == datacodec::DataCodecStatusSeverity::Warning,
        "an advisory spill check must reach the output callback as Warning");
    controls.SetStorageAnalyzer([] {
        return [](std::stop_token) { return igQtDataCodecResourceControls::StorageCheck{
            .minimumBytes = 2u * MiB, .required = false, .requiredMinimumBytes = 32u}; };
    });
    controls.CheckStorage();
    require(waitUntil([&] { return controls.StorageRejected(); }) &&
        controls.StorageStatusLabel()->text().contains(QStringLiteral("32 字节")),
        "spill cannot satisfy mandatory in-memory workspaces");
    storage->setValue(1.0);
    controls.CheckStorage();
    require(waitUntil([&] { return controls.StorageStatusLabel()->text().contains(QStringLiteral("临时文件")); }) &&
        !controls.StorageRejected(), "capacity above mandatory work may use file storage below the all-memory threshold");
    controls.SetLanguage(datacodec::DataCodecLanguage::English);
    require(waitUntil([&] { return storageStatus.text.find("temporary file storage") != std::string::npos; }) &&
        storageStatus.language == datacodec::DataCodecLanguage::English,
        "changing language must replace an existing capacity check with English wording");
    controls.SetStorageAnalyzer([] {
        return [](std::stop_token) -> igQtDataCodecResourceControls::StorageCheck {
            throw std::runtime_error("reader failed at offset 42");
        };
    });
    controls.CheckStorage();
    require(waitUntil([&] { return storageStatus.technicalDetail == "reader failed at offset 42"; }) &&
        storageStatus.text == "The capacity check did not complete" &&
        storageStatus.severity == datacodec::DataCodecStatusSeverity::Warning && !controls.StorageRejected(),
        "analysis exceptions must retain localized text and a separate technical detail without claiming insufficient capacity");
    controls.CheckStorage();
    modes->setCurrentIndex(modes->findData(static_cast<int>(datacodec::CodecResourceMode::Unlimited)));
    QApplication::processEvents();
    require(!controls.StorageRejected() && controls.StorageStatusLabel()->text().isEmpty(),
        "changing mode must invalidate an in-flight result");
    controls.close();
    controls.SetParams(savedControlParams);
    const auto beforeLoading = notifications;
    const auto adaptiveParams = controls.Params();
    controls.SetParams(adaptiveParams);
    require(notifications == beforeLoading, "loading CPU parameters emits no edit notification");
    threadMode->setCurrentIndex(threadMode->findData(static_cast<int>(datacodec::CodecThreadMode::Fixed)));
    require(threads->isEnabled() && !idle->isEnabled() && !controls.Params().targetCpuIdleRatio,
        "Fixed removes the idle target");
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
        auto secondValues = iGame::FloatArray::New();
        secondValues->SetName("resource_control_second");
        secondValues->SetDimension(1);
        secondValues->AddElement(samples);
        secondValues->AddElement(samples + 1);
        attributes->AddAttribute(IG_SCALAR, IG_POINT, secondValues);
        pointSet->SetAttributeSet(attributes);
        auto model = iGame::Model::New();
        model->SetDataObject(pointSet);
        compression.SetModel(model);
        QApplication::processEvents();
        auto* memoryReserve = compression.findChild<QDoubleSpinBox*>(QStringLiteral("DataCodecTargetAvailableMemory"));
        require(mode && compute && capacity && memoryReserve && mode->isEnabled() && compute->isEnabled() && memoryReserve->isEnabled(),
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

            // 使用真实压缩窗口触发预检，拒绝发生在目录创建和工作线程启动之前
            auto* outputPath = compression.findChild<QLineEdit*>(QStringLiteral("DataCodecEncodeOutputPath"));
            auto* status = compression.findChild<QPlainTextEdit*>(QStringLiteral("DataCodecLog"));
            QPushButton* start = nullptr;
            for (auto* button : compression.findChildren<QPushButton*>()) {
                if (button->property("dataCodecAction").toString() == QStringLiteral("startEncode")) { start = button; }
            }
            require(outputPath && status && start && start->isEnabled(), "encoding preflight UI must be accessible");
            if (outputPath && status && start && start->isEnabled()) {
                const auto rejectedDirectory = directory.filePath(QStringLiteral("rejected-output"));
                outputPath->setText(rejectedDirectory + QStringLiteral("/sample.igc"));
                capacity->setValue(0.0);
                start->click();
                require(status->toPlainText().contains(QStringLiteral("fixed owned storage limit=0 bytes")) &&
                    status->toPlainText().contains(QStringLiteral("proven necessary lower bound=32 bytes")) &&
                    start->isEnabled() && !QFileInfo::exists(rejectedDirectory),
                    "small Fixed budget must report in status and start no writer or directory creation");
                auto writer = iGame::IGDCWriter::New();
                writer->SetResourceParams({datacodec::CodecResourceMode::Fixed, 1u, 0u});
                require(writer->AnalyzeStorage(pointSet).provenLowerBoundBytes.value_or(0u) > 0u &&
                    writer->CheckStorageBeforeEncode(pointSet).has_value(), "writer preflight must use actual selected storage");
                writer->SetResourceParams({datacodec::CodecResourceMode::Unlimited, 1u, std::nullopt});
                require(!writer->CheckStorageBeforeEncode(pointSet), "Unlimited bypasses the Fixed byte precheck");
                QMetaObject::invokeMethod(capacity, "editingFinished");
                require(waitUntil([&] { return capacity->property("storageInsufficient").toBool(); }) &&
                    status->toPlainText().contains(QStringLiteral("至少设置 1 MiB")),
                    "the real encoding panel must report its actual bound on edit completion");
                const auto file = directory.filePath(QStringLiteral("decode-check.igc"));
                require(writer->WriteToFile(pointSet, file.toStdString()), "capacity check fixture must encode");
                // 指定导出位置时保留一个同格式小文件，供浏览器交互回归使用
                const auto exportedFixture = qEnvironmentVariable("IGAME_DATACODEC_UI_FIXTURE");
                if (!exportedFixture.isEmpty()) {
                    auto mesh = iGame::UnstructuredMesh::New();
                    auto vertices = iGame::Points::New();
                    vertices->AddPoint(0.f, 0.f, 0.f);
                    vertices->AddPoint(1.f, 0.f, 0.f);
                    vertices->AddPoint(0.f, 1.f, 0.f);
                    mesh->SetPoints(vertices);
                    igIndex triangle[]{0, 1, 2};
                    mesh->AddCell(triangle, 3, iGame::IG_TRIANGLE);
                    require(writer->WriteToFile(mesh, exportedFixture.toStdString()), "browser fixture must encode");
                    auto reader = iGame::IGDCReader::New();
                    reader->SetResourceParams({datacodec::CodecResourceMode::Unlimited, 1u, std::nullopt});
                    require(reader->ReadFile(exportedFixture.toStdString()) != nullptr, "browser fixture must decode natively");
                }
                const auto full = iGame::IGDCReader::AnalyzeFileStorage(file.toStdString(), true);
                const auto deferred = iGame::IGDCReader::AnalyzeFileStorage(file.toStdString(), false);
                require(full.success && deferred.success && full.minimumExecutionLimitBytes &&
                    deferred.minimumExecutionLimitBytes && *full.minimumExecutionLimitBytes >= *deferred.minimumExecutionLimitBytes,
                    "file precheck must support full and on-demand attribute paths");
                require(!iGame::IGDCReader::AnalyzeFileStorage(directory.filePath("missing.igc").toStdString(), true).success,
                    "missing file analysis must not publish a minimum");
            }
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
        igQtDataCodecDecodeSettingsStore::SaveResources(settings,
            {datacodec::CodecResourceMode::Unlimited, 2u, std::nullopt});
        settings.sync();
        QSettings reread(settingsPath, QSettings::IniFormat);
        params = igQtDataCodecDecodeSettingsStore::LoadResources(reread);
        require(params.mode == datacodec::CodecResourceMode::Unlimited && params.maxComputeThreads == 2u &&
            !params.ownedStorageLimitBytes && !reread.contains(QStringLiteral("OwnedStorageLimitBytes")),
            "Unlimited must persist distinctly and remove the prior fixed byte limit");
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, {});
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, adaptiveParams);
        settings.sync();
        QSettings cpuReader(settingsPath, QSettings::IniFormat);
        const auto cpuParams = igQtDataCodecDecodeSettingsStore::LoadResources(cpuReader);
        require(cpuParams.threadMode == datacodec::CodecThreadMode::Adaptive &&
            cpuParams.targetCpuIdleRatio == 0.25 && !cpuParams.maxComputeThreads &&
            !cpuReader.contains(QStringLiteral("MaxComputeThreads")), "Adaptive CPU settings persist without a fixed limit");
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, {});
        params = igQtDataCodecDecodeSettingsStore::LoadResources(settings);
        require(params.mode == datacodec::CodecResourceMode::Adaptive && !params.maxComputeThreads &&
            params.targetAvailableMemoryRatio == 0.20 &&
            !params.ownedStorageLimitBytes && !settings.contains(QStringLiteral("MaxComputeThreads")) &&
            !settings.contains(QStringLiteral("OwnedStorageLimitBytes")) &&
            params.threadMode == datacodec::CodecThreadMode::Fixed && !params.targetCpuIdleRatio &&
            !settings.contains(QStringLiteral("TargetCpuIdleRatio")), "saving defaults must remove explicit resource keys");
        settings.remove(QStringLiteral("ResourceSettingsVersion"));
        settings.setValue(QStringLiteral("OwnedStorageLimitBytes"), 123456u);
        settings.setValue(QStringLiteral("TargetAvailableMemoryRatio"), 0.7);
        params = igQtDataCodecDecodeSettingsStore::LoadResources(settings);
        require(params.targetAvailableMemoryRatio == 0.20 && !params.ownedStorageLimitBytes &&
            !settings.contains(QStringLiteral("OwnedStorageLimitBytes")) &&
            settings.value(QStringLiteral("ResourceSettingsVersion")).toInt() == 2,
            "old Adaptive byte settings migrate to 20 percent and record the version");
        params.targetAvailableMemoryRatio = 0.35;
        igQtDataCodecDecodeSettingsStore::SaveResources(settings, params);
        require(igQtDataCodecDecodeSettingsStore::LoadResources(settings).targetAvailableMemoryRatio == 0.35,
            "new Adaptive settings preserve the reserve ratio");
        settings.setValue(QStringLiteral("TargetAvailableMemoryRatio"), 1.0);
        require(igQtDataCodecDecodeSettingsStore::LoadResources(settings).targetAvailableMemoryRatio == 0.20,
            "invalid persisted reserve ratios use the explicit default");
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
