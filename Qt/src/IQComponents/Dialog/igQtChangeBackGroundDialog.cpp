//
// Created by m_ky on 2025/2/15.
//

/**
 * @class   igQtChangeBackGroundDialog
 * @brief   igQtChangeBackGroundDialog's brief
 */


#include "IQComponents/Dialog/igQtChangeBackGroundDialog.h"
#include "ui_igQtChangeBackGroundDialog.h"
#include <IQWidgets/igQtRenderWidget.h>   // §74：角色色（uiRole）
#include <QEvent>                          // §74：changeEvent(QEvent*)
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QIntValidator>
#include <QListWidget>
#include <QColorDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QBitmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QProxyStyle>

#include <iGameSceneManager.h>

namespace {
class DraggableColorDialog : public QColorDialog {
public:
    using QColorDialog::QColorDialog;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragOffset = event->globalPos() - frameGeometry().topLeft();
            event->accept();
            return;
        }
        QColorDialog::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragging && (event->buttons() & Qt::LeftButton)) {
            move(event->globalPos() - m_dragOffset);
            event->accept();
            return;
        }
        QColorDialog::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            event->accept();
            return;
        }
        QColorDialog::mouseReleaseEvent(event);
    }

private:
    bool m_dragging{false};
    QPoint m_dragOffset;
};

class NoFocusRectStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override {
        if (element == PE_FrameFocusRect) {
            if (!option || !painter) return;
            // 用蓝色实线替代默认虚线焦点框，保留“选中反馈”
            QPen pen(QColor("#0E639C"));
            pen.setWidth(2);
            pen.setStyle(Qt::SolidLine);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option->rect.adjusted(1, 1, -2, -2));
            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};
}

// §75：本弹窗是独立顶层窗，QLineEdit 既不在 .ui 的深色 QSS 里、也拿不到主窗口主题 QSS
//      → 直接落到深色 palette（黑底）。这里用角色色**当场**生成它们的样式表。
static void ApplyInputTheme(QWidget* root) {
    if (!root) return;
    const QString qss = QStringLiteral(
            "QLineEdit { background-color: %1; color: %2; border: 1px solid %3; border-radius: 4px; padding: 4px 6px; }"
            "QLineEdit:focus { border: 1px solid %4; }")
                                .arg(igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::PanelBg2),
                                     igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Text),
                                     igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Border),
                                     igQtRenderWidget::uiRoleCss(igQtRenderWidget::UiRole::Accent));
    const QList<QLineEdit*> edits = root->findChildren<QLineEdit*>();
    for (QLineEdit* e : edits) {
        if (e) e->setStyleSheet(qss);
    }
}

igQtChangeBackGroundDialog::igQtChangeBackGroundDialog(QWidget *parent) : QDialog(parent) {
    ui = new Ui::igQtChangeBackGroundDialog();
    ui->setupUi(this);
    setWindowTitle("更换背景色");
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    // §74：本弹窗 .ui 里有 5 处控件级深色 QSS（对话框自身 + 标签 + 颜色预览框 + 两个按钮）
    //      → 在此统一登记"底"，按当前主题做颜色令牌重映射；切主题由 changeEvent → refreshDeep 刷新。
    igQtPanelTheme::attachDeep(this);
    ApplyInputTheme(this);   // §75
    if (parentWidget()) {
        setWindowIcon(parentWidget()->windowIcon());
    }
    ui->label_WindowTitle->setText(windowTitle());
    if (!windowIcon().isNull()) {
        ui->label_WindowIcon->setPixmap(windowIcon().pixmap(16, 16));
    }

    igm::vec3 RGB = iGame::SceneManager::Instance()->GetCurrentScene()->GetBackGround();

    m_R = (int)(RGB.x * 255), m_G = (int)(RGB.y * 255), m_B = (int)(RGB.z * 255);
    m_Red_LineEdit = ui->lineEdit_R;
    m_Green_LineEdit = ui->lineEdit_G;
    m_Blue_LineEdit = ui->lineEdit_B;
    m_Red_LineEdit->setText(QString::number(m_R));
    m_Green_LineEdit->setText(QString::number(m_G));
    m_Blue_LineEdit->setText(QString::number(m_B));
    if (ui->frame_ColorPreview) {
        // §74：预览色块的颜色是"用户选的数据色"（语义色，必须保留），但其描边随主题；
        //      同时清掉它的底 QSS 登记 —— 否则切主题时 refreshDeep 会用旧底覆盖掉这里的动态颜色。
        ui->frame_ColorPreview->setProperty("igPanelBaseQss", QVariant());
        ui->frame_ColorPreview->setStyleSheet(
            QString("QFrame#frame_ColorPreview { background-color: rgb(%1,%2,%3); border: 1px solid %4; border-radius: 4px; }")
                .arg(m_R)
                .arg(m_G)
                .arg(m_B)
                .arg(igQtRenderWidget::uiRole(igQtRenderWidget::UiRole::Border).name()));
    }

    // 创建一个正则表达式，匹配 0~255 的数字
    QRegExp regExp("^(0|[1-9]\\d?|1\\d{2}|2[0-4]\\d|25[0-5])$");

    // 创建一个 QRegExpValidator，使用正则表达式
    QRegExpValidator *validator = new QRegExpValidator(regExp, this);
    m_Red_LineEdit->setValidator(validator);
    m_Green_LineEdit->setValidator(validator);
    m_Blue_LineEdit->setValidator(validator);

    connect(ui->pushButton_Close, &QPushButton::clicked, this, &igQtChangeBackGroundDialog::reject);
    connect(ui->pushButton_OK, &QPushButton::clicked, this, &igQtChangeBackGroundDialog::accept);

    auto refreshPreview = [&]() {
        bool okR = false, okG = false, okB = false;
        const int r = m_Red_LineEdit->text().toInt(&okR);
        const int g = m_Green_LineEdit->text().toInt(&okG);
        const int b = m_Blue_LineEdit->text().toInt(&okB);
        if (!okR || !okG || !okB || !ui->frame_ColorPreview) return;
        ui->frame_ColorPreview->setStyleSheet(
            QString("QFrame#frame_ColorPreview { background-color: rgb(%1,%2,%3); border: 1px solid #3C3C3C; border-radius: 4px; }")
                .arg(r)
                .arg(g)
                .arg(b));
    };
    connect(m_Red_LineEdit, &QLineEdit::textChanged, this, refreshPreview);
    connect(m_Green_LineEdit, &QLineEdit::textChanged, this, refreshPreview);
    connect(m_Blue_LineEdit, &QLineEdit::textChanged, this, refreshPreview);

    connect(ui->pushButton_Edit, &QPushButton::clicked, this, [&](){
        DraggableColorDialog dlg(QColor(m_R, m_G, m_B), this);
        dlg.setWindowTitle(QStringLiteral("选择颜色"));
        // 无边框自绘窗口 + Windows 原生颜色对话框在部分环境下取消会触发异常，强制用 Qt 自己的实现更稳。
        dlg.setOption(QColorDialog::DontUseNativeDialog, true);
        dlg.setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
        // 这里不能直接开透明背景，否则会出现你看到的“整体发透明”问题。
        // ColorManager 的无边框效果是配合 igQtFramelessWidget 外壳做出来的。
        dlg.setAttribute(Qt::WA_TranslucentBackground, false);
        dlg.setAttribute(Qt::WA_StyledBackground, true);
        // 样式从 .ui 中读取，便于统一维护
        if (ui->label_ColorDialogStyle) {
            const QString css = ui->label_ColorDialogStyle->text();
            if (!css.trimmed().isEmpty()) dlg.setStyleSheet(css);
        }
        // 用代理样式彻底禁掉虚线焦点框（左上/左下色块区域）
        dlg.setStyle(new NoFocusRectStyle(dlg.style()));
        if (dlg.exec() != QDialog::Accepted) return;
        const QColor color = dlg.currentColor();
        if (!color.isValid()) return;

        m_Red_LineEdit->setText(QString::number(color.red()));
        m_Green_LineEdit->setText(QString::number(color.green()));
        m_Blue_LineEdit->setText(QString::number(color.blue()));
        refreshPreview();
    });

    updateRoundedMask();
}

igQtChangeBackGroundDialog::~igQtChangeBackGroundDialog() { delete ui; }

void igQtChangeBackGroundDialog::updateRoundedMask() {
    if (width() <= 0 || height() <= 0) return;
    QBitmap mask(size());
    mask.fill(Qt::color0);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setPen(Qt::NoPen);
        mp.setBrush(Qt::color1);
        mp.drawRoundedRect(mask.rect().adjusted(0, 0, -1, -1), m_cornerRadius, m_cornerRadius);
    }
    setMask(mask);
}

void igQtChangeBackGroundDialog::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = rect().adjusted(0, 0, -1, -1);

    QPainterPath path;
    path.addRoundedRect(r, m_cornerRadius, m_cornerRadius);
    p.fillPath(path, igQtRenderWidget::uiRole(igQtRenderWidget::UiRole::PanelBg2)); // §74：内容背景（按主题）

    QPen pen(igQtRenderWidget::uiRole(igQtRenderWidget::UiRole::Border));           // §74：描边（按主题）
    pen.setWidth(1);
    p.setPen(pen);
    p.drawPath(path);
}

void igQtChangeBackGroundDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updateRoundedMask();
}

// §74：切主题 → 重映射本弹窗 QSS 并重绘外壳（本窗口是独立顶层窗，不继承主窗口 QSS）
void igQtChangeBackGroundDialog::changeEvent(QEvent* e) {
    if (e && e->type() == QEvent::StyleChange) {
        igQtPanelTheme::refreshDeep(this);
        ApplyInputTheme(this);   // §75
        update();
    }
    QDialog::changeEvent(e);
}

void igQtChangeBackGroundDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    updateRoundedMask();
}

void igQtChangeBackGroundDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton &&
        ui->widget_TitleBar->geometry().contains(event->pos()) &&
        !ui->pushButton_Close->geometry().contains(ui->widget_TitleBar->mapFrom(this, event->pos()))) {
        m_dragging = true;
        m_dragOffset = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void igQtChangeBackGroundDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void igQtChangeBackGroundDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QDialog::mouseReleaseEvent(event);
}

std::vector<int> igQtChangeBackGroundDialog::getInput() {
    m_R = m_Red_LineEdit->text().toInt(), m_G = m_Green_LineEdit->text().toInt(), m_B = m_Blue_LineEdit->text().toInt();
    return {m_R, m_G, m_B};
}