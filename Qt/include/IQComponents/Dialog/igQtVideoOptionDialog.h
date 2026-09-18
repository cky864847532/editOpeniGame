/**
 * @class   igQtVideoOptionDialog
 * @brief   保存动画时的导出参数（分辨率、帧率、码率）
 */

#if defined(FFMPEG_ENABLE)
#pragma once

#include <IQComponents/Dialog/igQtChromeFramelessDialog.h>
#include <IQCore/igQtExportModule.h>
#include <FFMPEG/iGameFFMPEGVideoWriter.h>

class QLineEdit;

class IG_QT_MODULE_EXPORT igQtVideoOptionDialog : public igQtChromeFramelessDialog {
    Q_OBJECT
public:
    explicit igQtVideoOptionDialog(QWidget* parent = nullptr);

    iGame::VideoInputInfo getInput();

protected:
    // §82：本弹窗是独立顶层窗，body 上的控件级 QSS 会压过主窗口主题 QSS，
    //      故配色由角色色当场生成，并在主题切换时重新生成。
    void changeEvent(QEvent* e) override;

    QLineEdit* m_Width_LineEdit{nullptr};
    QLineEdit* m_Height_LineEdit{nullptr};
    QLineEdit* m_frameRate_LineEdit{nullptr};
    QLineEdit* m_bitRate_LineEdit{nullptr};
    QWidget* m_body{nullptr};
};
#endif
