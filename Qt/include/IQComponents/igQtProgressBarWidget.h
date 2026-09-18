//
// Created by m_ky on 2024/5/22.
//

/**
 * @class   igQtProgressBarWidget
 * @brief   igQtProgressBarWidget's brief
 */
#pragma once

#include <iGameProgressObserver.h>

#include <QProgressBar>
#include <QLabel>
#include <QWidget>
#include <QTimer>
#include <IQCore/igQtExportModule.h>

class IG_QT_MODULE_EXPORT igQtProgressBarWidget : public QWidget{
public:
    static constexpr const char* DEFAULT = "进度条";
    static constexpr const char* PROCESSING = "Processing ...";
    explicit igQtProgressBarWidget(QWidget *parent = nullptr);
    ~igQtProgressBarWidget() override;

    void updateProgressBar(double value);

    void updateProgressBarLabel(const char* info);
protected:
    // §81：本控件是状态栏常驻件（MainWindow 构造期创建一次，之后不重建），
    // 所以必须在主题切换 / 每次显示时自己重新取色，否则会一直用启动那一刻的配色。
    void changeEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
private:
    void resetTextMode();
    // 按当前主题统一标签与进度条文字颜色
    void applyThemeStyle();
    // 显示并重置自动隐藏计时器
    void showWithAutoHide();

    // 进度事件可能来自工作线程（PVD / 切帧经 ThreadPool 读子文件），
    // 必须投递回本控件所在线程后再改 widget。
    static constexpr unsigned long kInvalidObserverTag = static_cast<unsigned long>(-1);

    QProgressBar* progressBar;
    QLabel *progressBarLabel;
    iGame::ProgressObserver* progressObserver;
    QTimer* m_hideTimer{nullptr};
    bool hasExternalText{false};
    bool m_applyingTheme{false};   // §81：applyThemeStyle() 重入护栏（防 StyleChange 回环）
    unsigned long m_ProgressObserverTag{kInvalidObserverTag};
    unsigned long m_TextObserverTag{kInvalidObserverTag};
};