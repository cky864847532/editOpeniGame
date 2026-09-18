#pragma once

#include <IQComponents/Dialog/igQtChromeFramelessDialog.h>
#include <IQCore/igQtExportModule.h>

class QLineEdit;

class IG_QT_MODULE_EXPORT igQtScreenShotOptionDialog : public igQtChromeFramelessDialog {
    Q_OBJECT
public:
    explicit igQtScreenShotOptionDialog(QWidget* parent = nullptr);

    std::pair<int, int> getInput();

protected:
    // §82：本弹窗是独立顶层窗，body 上的控件级 QSS 会压过主窗口主题 QSS，
    //      故配色由角色色当场生成，并在主题切换时重新生成。
    void changeEvent(QEvent* e) override;

    QLineEdit* m_WidthLineEdit{nullptr};
    QLineEdit* m_HeightLineEdit{nullptr};
    QWidget* m_body{nullptr};
};