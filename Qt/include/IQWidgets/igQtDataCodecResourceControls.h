#pragma once

#include <DataCodec/API/Params/CodecResourceParams.h>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

// 编解码窗口只编辑下次请求的启动参数，运行期调整由 DataCodec 内部执行
class igQtDataCodecResourceControls final : public QWidget {
public:
    explicit igQtDataCodecResourceControls(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QFormLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_mode = new QComboBox(this);
        m_mode->setObjectName(QStringLiteral("DataCodecResourceMode"));
        m_mode->addItem(QStringLiteral("自适应"), static_cast<int>(::datacodec::CodecResourceMode::Adaptive));
        m_mode->addItem(QStringLiteral("固定上限"), static_cast<int>(::datacodec::CodecResourceMode::Fixed));
        m_mode->setToolTip(QStringLiteral("固定上限：本次请求保持启动时的资源限制\n自适应：根据可用内存与压力调整资源，始终受指定上限约束"));
        m_threads = new QSpinBox(this);
        m_threads->setObjectName(QStringLiteral("DataCodecComputeLimit"));
        m_threads->setRange(0, std::numeric_limits<int>::max());
        m_threads->setSpecialValueText(QStringLiteral("设备默认"));
        m_threads->setToolTip(QStringLiteral("计算线程数上限；设备默认值由 DataCodec 在启动时确定"));
        m_memory = new QDoubleSpinBox(this);
        m_memory->setObjectName(QStringLiteral("DataCodecOwnedStorageLimit"));
        m_memory->setDecimals(0);
        m_memory->setRange(-1.0, static_cast<double>(kMaximumMiB));
        m_memory->setSingleStep(64.0);
        m_memory->setSpecialValueText(QStringLiteral("设备默认"));
        m_memory->setSuffix(QStringLiteral(" MiB"));
        m_memory->setToolTip(QStringLiteral("DataCodec 长期自有存储的容量上限，不代表整个程序的物理内存上限\n0 表示禁止新增此类内存存储；设备默认值在启动时根据环境确定"));
        layout->addRow(QStringLiteral("资源模式"), m_mode);
        layout->addRow(QStringLiteral("计算线程上限"), m_threads);
        layout->addRow(QStringLiteral("自有存储上限"), m_memory);
        SetParams({});
        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { Changed(); });
        connect(m_threads, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { Changed(); });
        connect(m_memory, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { Changed(); });
    }

    void SetParams(const ::datacodec::CodecResourceParams& params) {
        const QSignalBlocker modeBlock(m_mode), threadBlock(m_threads), memoryBlock(m_memory);
        m_mode->setCurrentIndex(m_mode->findData(static_cast<int>(params.mode)));
        m_threads->setValue(params.maxComputeThreads
            ? static_cast<int>(std::min<std::size_t>(*params.maxComputeThreads, std::numeric_limits<int>::max())) : 0);
        if (params.ownedStorageLimitBytes) {
            const auto bytes = *params.ownedStorageLimitBytes;
            const auto mib = bytes / kMiB + (bytes % kMiB != 0u);
            m_memory->setValue(static_cast<double>(std::min(mib, kMaximumMiB)));
        } else { m_memory->setValue(-1.0); }
    }

    [[nodiscard]] ::datacodec::CodecResourceParams Params() const {
        ::datacodec::CodecResourceParams params;
        params.mode = static_cast<::datacodec::CodecResourceMode>(m_mode->currentData().toInt());
        if (m_threads->value() > 0) { params.maxComputeThreads = static_cast<std::size_t>(m_threads->value()); }
        if (m_memory->value() >= 0.0) {
            params.ownedStorageLimitBytes = static_cast<std::uint64_t>(m_memory->value()) * kMiB;
        }
        return params;
    }

    void OnChanged(std::function<void()> callback) { m_changed = std::move(callback); }

private:
    void Changed() { if (m_changed) { m_changed(); } }
    static constexpr std::uint64_t kMiB = 1024u * 1024u;
    static constexpr std::uint64_t kMaximumMiB = std::numeric_limits<std::uint64_t>::max() / kMiB;
    QComboBox* m_mode{nullptr};
    QSpinBox* m_threads{nullptr};
    QDoubleSpinBox* m_memory{nullptr};
    std::function<void()> m_changed;
};
