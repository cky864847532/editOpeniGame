#pragma once

#include <DataCodec/API/Params/CodecResourceParams.h>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stop_token>
#include <utility>

// 编解码窗口只编辑下次请求的启动参数，运行期调整由 DataCodec 内部执行
class igQtDataCodecResourceControls final : public QWidget {
public:
    struct StorageCheck {
        std::optional<std::uint64_t> minimumBytes;
        // 只有已证明必需的额度才阻止启动，文件存储路径的全内存门槛仅作建议
        bool required{true};
        std::optional<std::uint64_t> requiredMinimumBytes;
        QString detail;
    };
    using StorageAnalysis = std::function<StorageCheck(std::stop_token)>;
    using StorageAnalysisFactory = std::function<StorageAnalysis()>;
    explicit igQtDataCodecResourceControls(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QFormLayout(this);
        m_layout = layout;
        layout->setContentsMargins(0, 0, 0, 0);
        m_mode = new QComboBox(this);
        m_mode->setObjectName(QStringLiteral("DataCodecResourceMode"));
        m_mode->addItem(QStringLiteral("自适应"), static_cast<int>(::datacodec::CodecResourceMode::Adaptive));
        m_mode->addItem(QStringLiteral("固定上限"), static_cast<int>(::datacodec::CodecResourceMode::Fixed));
        m_mode->addItem(QStringLiteral("内存不设上限"), static_cast<int>(::datacodec::CodecResourceMode::Unlimited));
        m_mode->setToolTip(QStringLiteral("固定上限：本次请求保持启动时的内存限制\n自适应：按系统可用物理内存保留比例调整分配许可，压力持续时暂停新增工作\n内存不设上限：由系统提供内存，保留线程控制和有界块处理"));
        m_reserve = new QDoubleSpinBox(this);
        m_reserve->setObjectName(QStringLiteral("DataCodecTargetAvailableMemory"));
        m_reserve->setRange(0.0, 99.9);
        m_reserve->setDecimals(1);
        m_reserve->setSuffix(QStringLiteral(" %"));
        m_reserve->setToolTip(QStringLiteral("期望系统保留的可用物理内存比例，默认 20%；系统观测包含其他程序，允许短时偏离目标"));
        m_threadMode = new QComboBox(this);
        m_threadMode->setObjectName(QStringLiteral("DataCodecThreadMode"));
        m_threadMode->addItem(QStringLiteral("固定线程上限"), static_cast<int>(::datacodec::CodecThreadMode::Fixed));
        m_threadMode->addItem(QStringLiteral("自适应 CPU 空闲量"), static_cast<int>(::datacodec::CodecThreadMode::Adaptive));
        m_idle = new QDoubleSpinBox(this);
        m_idle->setObjectName(QStringLiteral("DataCodecTargetCpuIdle"));
        m_idle->setRange(0.0, 99.0);
        m_idle->setDecimals(1);
        m_idle->setSuffix(QStringLiteral(" %"));
        m_idle->setToolTip(QStringLiteral("目标系统 CPU 空闲比例；系统总负载包含其他程序，响应在工作单元边界生效"));
        m_threads = new QSpinBox(this);
        m_threads->setObjectName(QStringLiteral("DataCodecComputeLimit"));
        m_threads->setRange(0, std::numeric_limits<int>::max());
        m_threads->setSpecialValueText(QStringLiteral("设备默认"));
        m_threads->setToolTip(QStringLiteral("计算线程数上限；设备默认值由 DataCodec 在启动时确定"));
        m_memory = new QDoubleSpinBox(this);
        m_memory->setObjectName(QStringLiteral("DataCodecOwnedStorageLimit"));
        m_memory->setDecimals(0);
        m_memory->setKeyboardTracking(false);
        m_memory->setRange(-1.0, static_cast<double>(kMaximumMiB));
        m_memory->setSingleStep(64.0);
        m_memory->setSpecialValueText(QStringLiteral("设备默认"));
        m_memory->setSuffix(QStringLiteral(" MiB"));
        m_memory->setToolTip(QStringLiteral("DataCodec 自有存储及工作缓冲的容量上限\n0 表示禁止新增此类内存存储；设备默认值在启动时根据环境确定\n内存不设上限模式忽略此输入"));
        layout->addRow(QStringLiteral("内存模式"), m_mode);
        layout->addRow(QStringLiteral("自有存储上限"), m_memory);
        layout->addRow(QStringLiteral("系统可用内存保留比例"), m_reserve);
        layout->addRow(QStringLiteral("线程模式"), m_threadMode);
        layout->addRow(QStringLiteral("计算线程上限"), m_threads);
        layout->addRow(QStringLiteral("目标 CPU 空闲量"), m_idle);
        m_storageStatus = new QLabel(this);
        m_storageStatus->setObjectName(QStringLiteral("DataCodecStorageCheckStatus"));
        m_storageStatus->setWordWrap(true);
        m_storageStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addRow(m_storageStatus);
        SetParams({});
        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { Changed(); CheckStorage(); });
        connect(m_threads, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { Changed(); });
        connect(m_memory, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { InvalidateStorageCheck(); Changed(); });
        connect(m_memory, &QDoubleSpinBox::editingFinished, this, [this] { CheckStorage(); });
        connect(m_reserve, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { Changed(); });
        connect(m_threadMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { Changed(); });
        connect(m_idle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { Changed(); });
    }

    ~igQtDataCodecResourceControls() override { m_checkStop.request_stop(); }

    void SetParams(const ::datacodec::CodecResourceParams& params) {
        const QSignalBlocker modeBlock(m_mode), threadBlock(m_threads), memoryBlock(m_memory);
        const QSignalBlocker threadModeBlock(m_threadMode), idleBlock(m_idle);
        const QSignalBlocker reserveBlock(m_reserve);
        m_reserve->setValue(params.targetAvailableMemoryRatio.value_or(0.20) * 100.0);
        m_threadMode->setCurrentIndex(m_threadMode->findData(static_cast<int>(params.threadMode)));
        m_idle->setValue(params.targetCpuIdleRatio.value_or(0.2) * 100.0);
        m_mode->setCurrentIndex(m_mode->findData(static_cast<int>(params.mode)));
        m_threads->setValue(params.maxComputeThreads
            ? static_cast<int>(std::min<std::size_t>(*params.maxComputeThreads, std::numeric_limits<int>::max())) : 0);
        if (params.ownedStorageLimitBytes) {
            const auto bytes = *params.ownedStorageLimitBytes;
            const auto mib = bytes / kMiB + (bytes % kMiB != 0u);
            m_memory->setValue(static_cast<double>(std::min(mib, kMaximumMiB)));
        } else { m_memory->setValue(-1.0); }
        RefreshMode();
        InvalidateStorageCheck();
    }

    [[nodiscard]] ::datacodec::CodecResourceParams Params() const {
        ::datacodec::CodecResourceParams params;
        params.mode = static_cast<::datacodec::CodecResourceMode>(m_mode->currentData().toInt());
        params.threadMode = static_cast<::datacodec::CodecThreadMode>(m_threadMode->currentData().toInt());
        if (params.threadMode == ::datacodec::CodecThreadMode::Adaptive) { params.targetCpuIdleRatio = m_idle->value() / 100.0; }
        else if (m_threads->value() > 0) { params.maxComputeThreads = static_cast<std::size_t>(m_threads->value()); }
        if (params.mode == ::datacodec::CodecResourceMode::Adaptive) {
            params.targetAvailableMemoryRatio = m_reserve->value() / 100.0;
        }
        if (params.mode == ::datacodec::CodecResourceMode::Fixed && m_memory->value() >= 0.0) {
            params.ownedStorageLimitBytes = static_cast<std::uint64_t>(m_memory->value()) * kMiB;
        }
        return params;
    }

    void OnChanged(std::function<void()> callback) { m_changed = std::move(callback); }
    void SetStorageAnalyzer(StorageAnalysisFactory factory) { m_analyzer = std::move(factory); }
    void OnStorageStatus(std::function<void(const QString&, bool)> callback) { m_statusChanged = std::move(callback); }
    void CheckStorage();
    void InvalidateStorageCheck();
    [[nodiscard]] bool StorageRejected() const noexcept { return m_storageRejected; }
    [[nodiscard]] QLabel* StorageStatusLabel() const noexcept { return m_storageStatus; }

private:
    void UpdateThreadRange();
    void PublishStorageStatus(const QString& text, bool rejected = false, bool warning = false);
    void RefreshMode() {
        const bool adaptive = m_threadMode->currentData().toInt() == static_cast<int>(::datacodec::CodecThreadMode::Adaptive);
        m_threads->setEnabled(!adaptive);
        m_idle->setEnabled(adaptive);
        const auto mode = static_cast<::datacodec::CodecResourceMode>(m_mode->currentData().toInt());
        const auto showRow = [&](QWidget* field, bool visible) {
            field->setVisible(visible);
            if (auto* label = m_layout->labelForField(field)) { label->setVisible(visible); }
        };
        showRow(m_reserve, mode == ::datacodec::CodecResourceMode::Adaptive);
        showRow(m_memory, mode == ::datacodec::CodecResourceMode::Fixed);
        showRow(m_threads, !adaptive);
        showRow(m_idle, adaptive);
        m_reserve->setEnabled(mode == ::datacodec::CodecResourceMode::Adaptive);
        m_memory->setEnabled(mode == ::datacodec::CodecResourceMode::Fixed);
        UpdateThreadRange();
    }
    void Changed() { RefreshMode(); if (m_changed) { m_changed(); } }
    static constexpr std::uint64_t kMiB = 1024u * 1024u;
    static constexpr std::uint64_t kMaximumMiB = std::numeric_limits<std::uint64_t>::max() / kMiB;
    QComboBox* m_mode{nullptr};
    QComboBox* m_threadMode{nullptr};
    QDoubleSpinBox* m_idle{nullptr};
    QSpinBox* m_threads{nullptr};
    QDoubleSpinBox* m_memory{nullptr};
    QDoubleSpinBox* m_reserve{nullptr};
    QFormLayout* m_layout{nullptr};
    QLabel* m_storageStatus{nullptr};
    StorageAnalysisFactory m_analyzer;
    std::function<void(const QString&, bool)> m_statusChanged;
    std::stop_source m_checkStop;
    std::uint64_t m_checkGeneration{0u};
    bool m_checkRunning{false};
    bool m_checkAgain{false};
    bool m_storageRejected{false};
    std::function<void()> m_changed;
};
