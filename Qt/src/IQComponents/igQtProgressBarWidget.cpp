//
// Created by m_ky on 2024/5/22.
//

#include <IQComponents/igQtProgressBarWidget.h>
#include <IQWidgets/igQtRenderWidget.h>
#include <QHBoxLayout>
#include <QColor>
#include <QEvent>
#include <QMetaObject>
#include <QThread>
#include <string>

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

    // 默认隐藏，只有加载/处理时才显示
    this->hide();

    // 无操作自动隐藏：加载进度停止更新后 1.5 秒隐藏
    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, [this]() { this->hide(); });

    // 统一标签与进度条文字颜色
    applyThemeStyle();

    progressObserver = iGame::ProgressObserver::Instance();

    // 事件回调发生在“发事件的那个线程”：PVD 读第一帧、切帧读播放帧都是在
    // ThreadPool 工作线程里读的（iGamePVDReader.cpp / iGameStreamingData.cpp），
    // 而 FileReader::UpdateReadProgress() 会把进度直接发给全局 ProgressObserver。
    // 在工作线程里直接 setValue()/setText() 属于跨线程操作 QWidget（UB），
    // 外部 UIA 客户端查询状态栏无障碍树时会撞在 Qt 内部崩溃。
    // 这里统一编组：GUI 线程直调，其它线程投递回本控件所在线程。
    auto dispatch = [this](auto action) {
        if (QThread::currentThread() == this->thread()) {
            action();
            return;
        }
        QMetaObject::invokeMethod(this, action, Qt::QueuedConnection);
    };

    m_ProgressObserverTag = progressObserver->AddObserver(iGame::Command::ProgressEvent,
        [this, dispatch](iGame::Object*, unsigned long, void* data)-> void {
            if (!data) { return; }
            const double value = *static_cast<double*>(data);
            dispatch([this, value]() { this->updateProgressBar(value); });
        });

    m_TextObserverTag = progressObserver->AddObserver(iGame::Command::UpdateEvent,
        [this, dispatch](iGame::Object*, unsigned long, void* data)-> void {
            const char* text = static_cast<const char*>(data);
            const std::string info = text ? std::string(text) : std::string();
            dispatch([this, info]() {
                if (info.empty()) {
                    this->resetTextMode();
                    return;
                }
                this->hasExternalText = true;
                this->updateProgressBarLabel(info.c_str());
            });
        });
}

igQtProgressBarWidget::~igQtProgressBarWidget() {
    // ProgressObserver 是全局单例：不摘掉观察者，控件析构后它还会回调到悬空的 this。
    if (progressObserver) {
        if (m_ProgressObserverTag != kInvalidObserverTag) {
            progressObserver->RemoveObserver(m_ProgressObserverTag);
        }
        if (m_TextObserverTag != kInvalidObserverTag) {
            progressObserver->RemoveObserver(m_TextObserverTag);
        }
    }
}

void igQtProgressBarWidget::resetTextMode() {
    hasExternalText = false;
    updateProgressBarLabel(DEFAULT);
}

void igQtProgressBarWidget::applyThemeStyle() {
    // §81：防重入。本函数会给子控件 setStyleSheet，而 setStyleSheet 会派发 StyleChange；
    // 若不加护栏，changeEvent → applyThemeStyle → setStyleSheet 可能形成回环（§68b 的栈溢出教训）。
    if (m_applyingTheme) return;
    m_applyingTheme = true;

    const bool light = igQtRenderWidget::globalLightBackground();
    // 与状态栏标签一致的文字颜色
    const QString textColor = light ? QStringLiteral("#4A5568") : QStringLiteral("#A5ADB8");
    const QString bgColor   = light ? QStringLiteral("#FFFFFF") : QStringLiteral("#22262B");
    const QString borderCol = light ? QStringLiteral("#CBD2DC") : QStringLiteral("#343B43");
    const QString chunkCol  = light ? QStringLiteral("#2B7CD3") : QStringLiteral("#4DD0E1");

    if (progressBarLabel) {
        progressBarLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(textColor));
    }
    if (progressBar) {
        progressBar->setStyleSheet(QStringLiteral(
                "QProgressBar {"
                " color: %1;"
                " background-color: %2;"
                " border: 1px solid %3;"
                " border-radius: 4px;"
                " min-height: 8px;"
                " text-align: center;"
                "}"
                "QProgressBar::chunk { background-color: %4; }")
                .arg(textColor, bgColor, borderCol, chunkCol));
    }

    m_applyingTheme = false;
}

// §81：本控件是状态栏常驻件（igQtMainWindow 构造期创建一次，之后不重建），
// 所以主题切换时必须自己重新取色；否则会一直保留启动那一刻的配色，
// 表现为「切到✦浅白后，进度条仍是黑底 + 青色 chunk」。
void igQtProgressBarWidget::changeEvent(QEvent* e) {
    if (e && e->type() == QEvent::StyleChange) {
        applyThemeStyle();
        update();
    }
    QWidget::changeEvent(e);
}

// §81：每次显示前也按当前主题重取一次色，覆盖「启动即浅色、用户从未切过主题」的情形。
void igQtProgressBarWidget::showEvent(QShowEvent* e) {
    applyThemeStyle();
    QWidget::showEvent(e);
}

void igQtProgressBarWidget::showWithAutoHide() {
    this->show();
    if (m_hideTimer) {
        m_hideTimer->start(1500); // 1.5 秒无新进度后自动隐藏
    }
}

void igQtProgressBarWidget::updateProgressBar(double value) {
    value = std::max(value, 0.0);
    value = std::min(value, 1.0);

    int progress = value * 100;

    if (progress < 100) {
        // 加载中：显示进度条并重置自动隐藏计时
        showWithAutoHide();
        if (!hasExternalText) {
            updateProgressBarLabel(PROCESSING);
        }
        progressBar->setValue(progress);
    } else {
        // 加载完成：立即隐藏
        resetTextMode();
        progressBar->setValue(100);
        progressBar->setValue(0);
        this->hide();
        if (m_hideTimer) m_hideTimer->stop();
    }
}

void igQtProgressBarWidget::updateProgressBarLabel(const char* info) {
    if (!info || info[0] == '\0') {
        progressBarLabel->setText(DEFAULT);
        this->hide();
        if (m_hideTimer) m_hideTimer->stop();
        return;
    }
    showWithAutoHide();
    progressBarLabel->setText(QString::fromUtf8(info));
}
