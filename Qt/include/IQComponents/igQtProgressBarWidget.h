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
#include <IQCore/igQtExportModule.h>

class QTimer;

class IG_QT_MODULE_EXPORT igQtProgressBarWidget : public QWidget{
public:
    static constexpr const char* DEFAULT = "进度条";
    static constexpr const char* PROCESSING = "Processing ...";
    explicit igQtProgressBarWidget(QWidget *parent = nullptr);

    void updateProgressBar(double value);

    void updateProgressBarLabel(const char* info);
private:
    void resetTextMode();
    /** GUI 线程里的进度处理：走原逻辑，并（可选）驱动"合成爬升"动画 */
    void onProgressEvent(double value);

    QProgressBar* progressBar;
    QLabel *progressBarLabel;
    iGame::ProgressObserver* progressObserver;
    bool hasExternalText{false};
    QTimer* m_creepTimer{nullptr};
};