#if defined(FFMPEG_ENABLE)
#include "IQComponents/Dialog/igQtVideoOptionDialog.h"

#include <IQWidgets/igQtRenderWidget.h>   // §82：角色色（uiRoleCss）
#include <QEvent>                          // §82：changeEvent(QEvent*)
#include <QFormLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegExpValidator>
#include <QVBoxLayout>

namespace {
// §82：本弹窗是独立顶层窗，body 上的控件级 QSS 会压过主窗口主题 QSS，
//      所以配色必须用角色色当场生成，并在主题切换时重新生成
//      （原先写死深色，切到 ✦浅白 后输入框/按钮仍是深色底）。
QString bodyThemeQss() {
    return QStringLiteral(
                   "QWidget { background-color: transparent; color: %1; }"
                   "QLabel { color: %1; }"
                   "QLineEdit { background-color: %2; color: %1; border: 1px solid %3; padding: 4px; border-radius: 3px; }"
                   "QLineEdit:focus { border: 1px solid %4; }"
                   "QPushButton { background-color: %2; color: %1; border: 1px solid %3; padding: 6px 12px; border-radius: 4px; }"
                   "QPushButton:hover { background-color: %5; }"
                   "QPushButton:pressed { background-color: %6; }")
            .arg(igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Text),
                 igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::PanelBg2),
                 igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Border),
                 igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Accent),
                 igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::HoverBg),
                 igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::SelectionBg));
}
}   // namespace

igQtVideoOptionDialog::igQtVideoOptionDialog(QWidget* parent) : igQtChromeFramelessDialog(parent) {
    setDialogTitle(QStringLiteral("保存动画选项"));
    setMinimumSize(460, 300);
    resize(520, 320);
    setMaximizeEnabled(false);

    auto* body = new QWidget(this);
    m_body = body;
    body->setAttribute(Qt::WA_StyledBackground, true);
    body->setStyleSheet(bodyThemeQss());   // §82：角色色，切主题时由 changeEvent 重新生成

    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(14, 10, 14, 14);
    layout->setSpacing(12);

    auto* intValidator = new QIntValidator(1, 9999, body);
    auto* numValidator = new QRegExpValidator(QRegExp(QStringLiteral("^[0-9]*\\.?[0-9]*$")), body);

    m_Width_LineEdit = new QLineEdit(QStringLiteral("1920"), body);
    m_Height_LineEdit = new QLineEdit(QStringLiteral("1080"), body);
    m_frameRate_LineEdit = new QLineEdit(QStringLiteral("1"), body);
    m_bitRate_LineEdit = new QLineEdit(QStringLiteral("4000000"), body);
    m_Width_LineEdit->setValidator(intValidator);
    m_Height_LineEdit->setValidator(intValidator);
    m_frameRate_LineEdit->setValidator(numValidator);
    m_bitRate_LineEdit->setValidator(numValidator);
    m_Width_LineEdit->setMinimumWidth(160);
    m_Height_LineEdit->setMinimumWidth(160);
    m_frameRate_LineEdit->setMinimumWidth(160);
    m_bitRate_LineEdit->setMinimumWidth(160);

    auto* form = new QFormLayout();
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(new QLabel(QStringLiteral("宽度："), body), m_Width_LineEdit);
    form->addRow(new QLabel(QStringLiteral("高度："), body), m_Height_LineEdit);
    form->addRow(new QLabel(QStringLiteral("帧率："), body), m_frameRate_LineEdit);
    form->addRow(new QLabel(QStringLiteral("码率："), body), m_bitRate_LineEdit);

    layout->addLayout(form);

    auto* okButton = new QPushButton(QStringLiteral("确定"), body);
    layout->addWidget(okButton, 0, Qt::AlignRight);

    connect(okButton, &QPushButton::clicked, this, &igQtVideoOptionDialog::accept);

    setContentWidget(body);
}

iGame::VideoInputInfo igQtVideoOptionDialog::getInput() {
    iGame::VideoInputInfo res;
    res.width = m_Width_LineEdit->text().toInt();
    res.height = m_Height_LineEdit->text().toInt();
    res.frame_rate = m_frameRate_LineEdit->text().toInt();
    res.bit_rate = m_bitRate_LineEdit->text().toInt();
    return res;
}

// §82：切主题 → 重新按角色色生成 body 样式（本窗口是独立顶层窗，不继承主窗口 QSS）
void igQtVideoOptionDialog::changeEvent(QEvent* e) {
    if (e && e->type() == QEvent::StyleChange && m_body) {
        m_body->setStyleSheet(bodyThemeQss());
    }
    igQtChromeFramelessDialog::changeEvent(e);
}
#endif
