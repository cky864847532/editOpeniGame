//
// Created by m_ky on 2024/5/22.
//

#include <IQComponents/igQtProgressBarWidget.h>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QTimer>

#include <string>

namespace {
// 「合成爬升」开关：
// 有些读取器整个过程只在结束时上报一次 1.0（例如旧版 .vtk：iGameVTKReader.cpp 里只有
// UpdateProgress(1.0)），中间没有任何进度事件，进度条就会一直停在 0%（HEAD 版同样如此）。
// 打开后：在"已经收到进度事件、但还没完成"期间，UI 侧让进度条缓慢爬到 90% 上限；
// 一旦有真实进度上报，就用真实值（只增不减）。想完全回到 HEAD 的原始行为 → 改成 false。
constexpr bool kAllowSyntheticCreep = true;
constexpr int kCreepIntervalMs = 100; // 每 100ms 前进 1%
constexpr int kCreepCeiling = 90;     // 爬升上限（剩下的留给真实进度/完成）
} // namespace

/**
 * @class   igQtProgressBarWidget
 * @brief   igQtProgressBarWidget's brief
 */
igQtProgressBarWidget::igQtProgressBarWidget(QWidget *parent) : QWidget(parent) {
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);

    progressBarLabel = new QLabel(DEFAULT,this);

    progressBarLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progressBarLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    progressBarLabel->setMinimumWidth(220);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->addWidget(progressBarLabel);
    layout->addWidget(progressBar);
    layout->setStretch(0, 2);
    layout->setStretch(1, 3);
    layout->setContentsMargins(0, 0, 0, 0);
    this->setLayout(layout);

    progressObserver = iGame::ProgressObserver::Instance();

    // 【重要】ProgressObserver 是同步派发（iGameProgressObserver.h: UpdateProgress → InvokeEvent），
    // 而文件加载跑在**工作线程**（igQtFileLoader 用 std::thread，iGameCore 用 ThreadPool），
    // 所以这两个回调会在「非 GUI 线程」里被调用：
    //   - 旧版直接 setValue()/setText()：靠 Qt 的队列重绘"碰巧"能看到，但同时会在非 GUI 线程
    //     触发 QAccessible 事件 —— 那正是修改记录里那个 UIA 崩溃的根因；
    //   - 加过 show()/hide()/QTimer::start() 的版本更糟：定时器在非 GUI 线程根本起不来、
    //     控件可见性状态错乱 → 表现为"进度条卡在 0% 一动不动"。
    // 这里统一用 QueuedConnection 切回 GUI 线程再更新界面。
   progressObserver->AddObserver(iGame::Command::ProgressEvent,
        [this](iGame::Object*, unsigned long, void* data)-> void {
            const double value = *static_cast<double*>(data);
            QMetaObject::invokeMethod(this, [this, value]() { this->onProgressEvent(value); },
                                      Qt::QueuedConnection);
        });

    progressObserver->AddObserver(iGame::Command::UpdateEvent,
        [this](iGame::Object*, unsigned long, void* data)-> void {
            const char* text = static_cast<const char*>(data);
            const std::string copy = (text != nullptr) ? std::string(text) : std::string();
            QMetaObject::invokeMethod(
                    this,
                    [this, copy]() {
                        if (copy.empty()) {
                            // 读取器结束时会把进度复位并清空文案（resetProgressUI）：
                            // 这是"本次操作结束"的可靠信号 → 停止合成爬升并把条归零，
                            // 否则会停在爬升到的中间值上。
                            if (m_creepTimer != nullptr) { m_creepTimer->stop(); }
                            if (progressBar != nullptr) { progressBar->setValue(0); }
                            resetTextMode();
                            return;
                        }
                        hasExternalText = true;
                        this->updateProgressBarLabel(copy.c_str());
                    },
                    Qt::QueuedConnection);
        });

    if (kAllowSyntheticCreep) {
        m_creepTimer = new QTimer(this);
        m_creepTimer->setInterval(kCreepIntervalMs);
        connect(m_creepTimer, &QTimer::timeout, this, [this]() {
            if (progressBar->value() < kCreepCeiling) {
                progressBar->setValue(progressBar->value() + 1);
            }
        });
    }
}

void igQtProgressBarWidget::onProgressEvent(double value) {
    const int percent = qBound(0, static_cast<int>(value * 100.0), 100);

    if (percent >= 100) {
        // 完成：走原逻辑（100 → 0），并停止合成爬升
        if (m_creepTimer != nullptr) { m_creepTimer->stop(); }
        updateProgressBar(value);
        return;
    }

    // 0.0 是"复位"事件（各读取器结束/失败时都会发），不参与显示，也不触发爬升
    if (percent <= 0) { return; }

    // 真实进度只增不减：合成爬升可能已经把条推到更高位置，
    // 此时不要再被较小的真实值拉回去（否则会出现"倒退"）。
    if (progressBar->value() < percent) { updateProgressBar(value); }

    if (kAllowSyntheticCreep && m_creepTimer != nullptr && !m_creepTimer->isActive()) {
        m_creepTimer->start();
    }
}

void igQtProgressBarWidget::resetTextMode() {
    hasExternalText = false;
    updateProgressBarLabel(DEFAULT);
}

void igQtProgressBarWidget::updateProgressBar(double value) {
    value = std::max(value, 0.0);
    value = std::min(value, 1.0);

    int progress = value * 100;
    

    if (progress < 100) {
        if (!hasExternalText) {
            updateProgressBarLabel(PROCESSING);
        }
        progressBar->setValue(progress);
    } else {
        resetTextMode();
        progressBar->setValue(100);
        progressBar->setValue(0);
    }
}

void igQtProgressBarWidget::updateProgressBarLabel(const char* info) {
    if (!info || info[0] == '\0') {
        progressBarLabel->setText(DEFAULT);
        return;
    }
    progressBarLabel->setText(QString::fromUtf8(info));
}
