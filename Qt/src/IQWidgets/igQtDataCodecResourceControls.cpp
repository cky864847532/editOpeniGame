#include "IQWidgets/igQtDataCodecResourceControls.h"

#include <DataCodec/Runtime/Execution/DataCodecResourceController.h>
#include <QEvent>
#include <QLineEdit>
#include <QThread>
#include <memory>

void igQtDataCodecResourceControls::InitializeMemoryReserveHint() {
    auto* edit = m_reserve->findChild<QLineEdit*>();
    m_reserveCapacity = new QLabel(edit);
    m_reserveCapacity->setObjectName(QStringLiteral("DataCodecMemoryReserveCapacity"));
    m_reserveCapacity->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_reserveCapacity->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_reserveCapacity->setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; border: none; padding: 0; color: #9b9b9b; }"
        "QLabel:disabled { color: #7b7b7b; }"));
    edit->installEventFilter(this);
}

void igQtDataCodecResourceControls::UpdateMemoryReserveHint(
    const std::optional<std::uint64_t> physicalTotalBytes) {
    if (physicalTotalBytes && *physicalTotalBytes > 0u) {
        const auto bytes = static_cast<long double>(*physicalTotalBytes) *
            (m_reserve->value() / 100.0);
        constexpr auto GiB = 1024u * kMiB;
        m_reserveCapacity->setText(bytes >= GiB
            ? QStringLiteral("≈ %1 GiB").arg(static_cast<double>(bytes / GiB), 0, 'f', 2)
            : QStringLiteral("≈ %1 MiB").arg(static_cast<double>(bytes / kMiB), 0, 'f', 1));
    } else {
        m_reserveCapacity->setText(QStringLiteral("容量未知"));
    }
    LayoutMemoryReserveHint();
}

void igQtDataCodecResourceControls::LayoutMemoryReserveHint() {
    auto* edit = static_cast<QLineEdit*>(m_reserveCapacity->parentWidget());
    const auto width = m_reserveCapacity->sizeHint().width();
    // 容量提示独立绘制并预留编辑区，保持百分比的输入和解析规则
    auto margins = edit->textMargins();
    margins.setRight(width + 8);
    edit->setTextMargins(margins);
    m_reserveCapacity->setGeometry(std::max(0, edit->width() - width - 4), 0, width, edit->height());
    m_reserve->setMinimumWidth(
        m_reserve->fontMetrics().horizontalAdvance(QStringLiteral("99.9 %")) + width + 48);
}

bool igQtDataCodecResourceControls::eventFilter(QObject* watched, QEvent* event) {
    if (m_reserveCapacity && watched == m_reserveCapacity->parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::FontChange ||
            event->type() == QEvent::StyleChange)) {
        LayoutMemoryReserveHint();
    }
    return QWidget::eventFilter(watched, event);
}

void igQtDataCodecResourceControls::UpdateThreadRange() {
    const auto params = Params();
    const auto sample = ::datacodec::ProbeResources();
    UpdateMemoryReserveHint(sample.physicalTotalBytes);
    const auto maximum = ::datacodec::ResourceComputeCapacity(sample, params.mode, params.threadMode);
    const QSignalBlocker blocker(m_threads);
    m_threads->setMaximum(static_cast<int>(std::min<std::size_t>(maximum, std::numeric_limits<int>::max())));
    m_threads->setToolTip(QStringLiteral("0 表示设备默认；当前模式最多使用 %1 个计算线程").arg(maximum));
}

void igQtDataCodecResourceControls::SetLanguage(const ::datacodec::DataCodecLanguage language) {
    if (m_language == language) { return; }
    m_language = language;
    CheckStorage();
}

void igQtDataCodecResourceControls::PublishStorageStatus(
    const ::datacodec::DataCodecStatusRecord& status, const bool rejected) {
    const auto text = QString::fromStdString(::datacodec::FormatDataCodecStatusText(status));
    const bool warning = status.severity == ::datacodec::DataCodecStatusSeverity::Warning;
    const bool changed = m_storageStatus->text() != text || m_storageRejected != rejected ||
        m_storageSeverity != status.severity;
    m_storageRejected = rejected;
    m_storageSeverity = status.severity;
    m_memory->setProperty("storageInsufficient", rejected);
    m_memory->setStyleSheet(rejected
        ? QStringLiteral("QDoubleSpinBox, QDoubleSpinBox QLineEdit { color: #ff6868; }") : QString());
    m_storageStatus->setStyleSheet(rejected ? QStringLiteral("color: #ff6868;")
        : warning ? QStringLiteral("color: #d6a74a;") : QString());
    m_storageStatus->setText(text);
    m_storageStatus->setVisible(!text.isEmpty());
    if (changed && m_statusChanged) { m_statusChanged(status); }
}

void igQtDataCodecResourceControls::InvalidateStorageCheck() {
    ++m_checkGeneration;
    m_checkStop.request_stop();
    m_checkAgain = false;
    PublishStorageStatus({});
}

void igQtDataCodecResourceControls::CheckStorage() {
    using Message = iGame::iGameDataCodecHostMessageId;
    using Severity = ::datacodec::DataCodecStatusSeverity;
    InvalidateStorageCheck();
    if (!m_analyzer || Params().mode != ::datacodec::CodecResourceMode::Fixed) { return; }
    // 同一窗口最多运行一个分析，编辑合并为最新请求
    if (m_checkRunning) {
        m_checkAgain = true;
        PublishStorageStatus(iGame::iGameDataCodecHostStatus(m_language, Message::StorageCheckUpdating));
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
            catch (const std::exception& error) {
                result->messageId = Message::StorageCheckFailed;
                result->technicalDetail = QString::fromUtf8(error.what());
            }
            catch (...) {
                result->messageId = Message::StorageCheckFailed;
                result->technicalDetail = QStringLiteral("unknown exception during storage analysis");
            }
        });
        m_checkRunning = true;
        PublishStorageStatus(iGame::iGameDataCodecHostStatus(m_language, Message::StorageCheckRunning));
        connect(worker, &QThread::finished, this, [this, generation, result, limit] {
            m_checkRunning = false;
            if (m_checkAgain) { CheckStorage(); return; }
            if (generation != m_checkGeneration) { return; }
            if (!result->minimumBytes || !limit) {
                PublishStorageStatus(iGame::iGameDataCodecHostStatus(m_language, result->messageId, {},
                    result->messageId == Message::StorageCheckFailed ? Severity::Warning : Severity::Info,
                    result->technicalDetail.toUtf8().toStdString()));
                return;
            }
            const auto requiredMinimum = result->required ? *result->minimumBytes : result->requiredMinimumBytes.value_or(0u);
            const bool rejected = *limit < requiredMinimum;
            const auto minimum = rejected ? requiredMinimum : *result->minimumBytes;
            const auto mib = minimum / kMiB + (minimum % kMiB != 0u);
            const bool small = *limit < minimum;
            auto status = iGame::iGameDataCodecHostStatus(m_language,
                rejected ? Message::StorageLimitInsufficient : small ? Message::StorageSpillRequired : Message::StorageLimitSufficient,
                {{"mib", std::to_string(mib)}, {"bytes", std::to_string(minimum)}},
                rejected ? Severity::Error : small ? Severity::Warning : Severity::Info,
                result->technicalDetail.toUtf8().toStdString());
            if (result->messageId != Message::StorageCheckUnavailable) {
                status.text += "\n" + iGame::iGameDataCodecHostMessage(m_language, result->messageId);
            }
            PublishStorageStatus(status, rejected);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    } catch (const std::exception& error) {
        PublishStorageStatus(iGame::iGameDataCodecHostStatus(m_language, Message::StorageCheckFailed,
            {}, Severity::Warning, error.what()));
    }
}
