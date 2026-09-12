#include "IQWidgets/igQtDataCodecResourceControls.h"

#include <DataCodec/Runtime/Execution/DataCodecResourceController.h>
#include <QThread>
#include <memory>

void igQtDataCodecResourceControls::UpdateThreadRange() {
    const auto params = Params();
    const auto maximum = ::datacodec::ResourceComputeCapacity(::datacodec::ProbeResources(), params.mode, params.threadMode);
    const QSignalBlocker blocker(m_threads);
    m_threads->setMaximum(static_cast<int>(std::min<std::size_t>(maximum, std::numeric_limits<int>::max())));
    m_threads->setToolTip(QStringLiteral("0 表示设备默认；当前模式最多使用 %1 个计算线程").arg(maximum));
}

void igQtDataCodecResourceControls::PublishStorageStatus(const QString& text, bool rejected, bool warning) {
    const bool changed = m_storageStatus->text() != text || m_storageRejected != rejected;
    m_storageRejected = rejected;
    m_memory->setProperty("storageInsufficient", rejected);
    m_memory->setStyleSheet(rejected
        ? QStringLiteral("QDoubleSpinBox, QDoubleSpinBox QLineEdit { color: #ff6868; }") : QString());
    m_storageStatus->setStyleSheet(rejected ? QStringLiteral("color: #ff6868;")
        : warning ? QStringLiteral("color: #d6a74a;") : QString());
    m_storageStatus->setText(text);
    m_storageStatus->setVisible(!text.isEmpty());
    if (changed && m_statusChanged) { m_statusChanged(text, rejected || warning); }
}

void igQtDataCodecResourceControls::InvalidateStorageCheck() {
    ++m_checkGeneration;
    m_checkStop.request_stop();
    m_checkAgain = false;
    PublishStorageStatus({});
}

void igQtDataCodecResourceControls::CheckStorage() {
    InvalidateStorageCheck();
    if (!m_analyzer || Params().mode != ::datacodec::CodecResourceMode::Fixed) { return; }
    // 同一窗口最多运行一个分析，编辑合并为最新请求
    if (m_checkRunning) {
        m_checkAgain = true;
        PublishStorageStatus(QStringLiteral("正在更新容量检查…"));
        return;
    }
    try {
        const auto limit = ::datacodec::ResolveResourceConfiguration(Params(), ::datacodec::ProbeResources())
            .initialLimits.ownedStorageLimitBytes;
        auto analyze = m_analyzer();
        if (!analyze) { return; }
        const auto generation = m_checkGeneration;
        m_checkStop = std::stop_source{};
        const auto stop = m_checkStop.get_token();
        auto result = std::make_shared<StorageCheck>();
        auto* worker = QThread::create([analyze = std::move(analyze), result, stop] {
            try { *result = analyze(stop); }
            catch (const std::exception& error) { result->detail = QString::fromUtf8(error.what()); }
            catch (...) { result->detail = QStringLiteral("容量分析发生未知错误"); }
        });
        m_checkRunning = true;
        PublishStorageStatus(QStringLiteral("正在检查内存容量…"));
        connect(worker, &QThread::finished, this, [this, generation, result, limit] {
            m_checkRunning = false;
            if (m_checkAgain) { CheckStorage(); return; }
            if (generation != m_checkGeneration) { return; }
            if (!result->minimumBytes || !limit) {
                PublishStorageStatus(result->detail.isEmpty() ? QStringLiteral("尚未取得可用的容量检查结果") : result->detail);
                return;
            }
            const auto requiredMinimum = result->required ? *result->minimumBytes : result->requiredMinimumBytes.value_or(0u);
            const bool rejected = *limit < requiredMinimum;
            const auto minimum = rejected ? requiredMinimum : *result->minimumBytes;
            const auto mib = minimum / kMiB + (minimum % kMiB != 0u);
            const bool small = *limit < minimum;
            const auto text = small
                ? (rejected
                    ? QStringLiteral("内存上限不足：至少设置 %1 MiB（%2 字节），或选择内存不设上限")
                    : QStringLiteral("全内存解码至少需要 %1 MiB（%2 字节）；当前额度将依赖临时文件存储，可继续操作"))
                    .arg(mib).arg(minimum)
                : QStringLiteral("当前额度未低于已知容量门槛：%1 MiB（%2 字节）").arg(mib).arg(minimum);
            PublishStorageStatus(text + QStringLiteral("\n") + result->detail, rejected, small);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    } catch (const std::exception& error) {
        PublishStorageStatus(QStringLiteral("容量检查未完成：%1").arg(QString::fromUtf8(error.what())), false, true);
    }
}
