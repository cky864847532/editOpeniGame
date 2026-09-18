#include "Sources/iGameLineTypePointsSourceFilter.h"
#include <IQComponents/igQtModelDialogWidget.h>
#include <IQWidgets/igQtRenderWidget.h>
#include <Plugin/qtpropertybrowser/qtpropertymanager.h>
#include <QApplication>
#include <QMainWindow>
#include <QQueue>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QColor>
#include <QHeaderView>
#include <QPalette>
#include <iGameSceneManager.h>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QRegion>
#include <QEvent>
#include <QTimer>       // §49d：展开后用单次定时器把位置钉死
#include <functional>   // §49：标题栏"收起"按钮的回调

namespace {
// §49：树卡片收起后的小方块尺寸（悬浮时右下角保持不动）
constexpr int kTreeCollapsedSize = 48;

// §50 方案 A′：标题栏布局常量（§50c 按用户意见只保留左侧叠层图标）
constexpr int kDockTitleBarHeight = 40;   // 原 38，略微加高更从容
constexpr int kTitleLeftPad = 14;
constexpr int kIconGap = 10;              // 图标与文字之间的间距
constexpr int kTitleLabelLeft = kTitleLeftPad + 16 + kIconGap;   // = 40（文字起点）

// §49d：把窗口矩形收进"屏幕可用区"（Windows 在贴近屏幕边缘/任务栏时会自己挪窗口，
// 尤其 resize 之后会再调整一次位置 —— 这正是"靠近右下角再展开会错位"的来源）。
// 我们自己先按可用区收敛，再用下一轮事件循环把位置钉死，就不会被系统挪走。
void clampRectToAvailable(QRect& rect, const QWidget* w) {
    QScreen* screen = w ? w->screen() : nullptr;
    if (!screen) screen = QGuiApplication::screenAt(rect.center());
    if (!screen) return;
    const QRect avail = screen->availableGeometry();
    if (rect.right() > avail.right()) rect.moveRight(avail.right());
    if (rect.bottom() > avail.bottom()) rect.moveBottom(avail.bottom());
    if (rect.left() < avail.left()) rect.moveLeft(avail.left());
    if (rect.top() < avail.top()) rect.moveTop(avail.top());
}

// §51：收起态小方块的"面" —— 叠层图标 + 名称，一次性画成位图喂给按钮
// （按钮自身仍负责渐变底/描边/悬停/按下；这样收起态与标题栏是同一套视觉语言）
QPixmap buildCollapsedFacePixmap(int size, qreal dpr) {
    const qreal s = qMax(24, size);
    QPixmap pm(qRound(s * dpr), qRound(s * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    const bool light = igQtRenderWidget::globalLightBackground();
    const QColor ink = light ? QColor("#1A1A1A") : QColor("#FFFFFF");

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // 叠层图标（与标题栏同款：后浅前深两个圆角方块）
    const qreal cx = s / 2.0;
    const qreal top = s * 0.19;
    QColor back = ink;
    back.setAlpha(72);
    QColor front = ink;
    front.setAlpha(160);
    QPainterPath backPath;
    backPath.addRoundedRect(QRectF(cx - s * 0.215, top, s * 0.30, s * 0.225), s * 0.055, s * 0.055);
    p.fillPath(backPath, back);
    QPainterPath frontPath;
    frontPath.addRoundedRect(QRectF(cx - s * 0.125, top + s * 0.125, s * 0.32, s * 0.25), s * 0.065, s * 0.065);
    p.fillPath(frontPath, front);

    // 名称（同一层渲染，避免"图标 + 控件文字"两层各自居中造成的廉价感）
    QFont f = QApplication::font();
    f.setPixelSize(qMax(8, qRound(s * 0.215)));
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
    p.setFont(f);
    QColor text = ink;
    text.setAlpha(200);
    p.setPen(text);
    p.drawText(QRectF(0, s * 0.56, s, s * 0.30), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("模型树"));

    p.end();
    return pm;
}

// §51b：收起态"外圈"（窗口 2px 边）的底色 = 小方块渐变的暗端。
// 这样 1bit 窗口遮罩的 1px 台阶落在"深色对深色"之间，几乎看不出来（原来外圈是卡片底色 #252526，
// 比方块底部亮，反而把那圈台阶衬出来了）。
QString collapsedRingColor() {
    switch (igQtRenderWidget::globalStyleMode()) {
        case 13: return QStringLiteral("#D3DBE6");   // 悬浮·浅色
        case 14: return QStringLiteral("#1A1D22");   // 悬浮·石墨现代
        case 15: return QStringLiteral("#16181C");   // 悬浮·石墨哑光
        default: return QStringLiteral("#151517");   // 12 悬浮卡片 / 11 GitCode 暗色
    }
}

// §49：收起态小方块的样式 —— 纵向渐变 + 顶部亮边 + 1px 描边，做出"层次感"；
// 颜色跟随全局颜色族（深色 12 / 浅色 13 / 石墨现代 14 / 石墨哑光 15）。
QString collapsedBlockQss() {
    // §51b：不再用"一圈亮描边"（小尺寸上方块边上一圈白边会把 1bit 窗口遮罩的台阶放大得很明显）。
    //        现在：外侧只留极淡的深色描边（把边缘"收"住），顶部保留一丝高光做层次；
    //        方块**内缩 2px**，让 QSS 自绘的抗锯齿圆角落在窗口遮罩之内，可见边就是平滑的那条。
    switch (igQtRenderWidget::globalStyleMode()) {
        case 13:   // 悬浮·浅色
            return QStringLiteral(
                    "QPushButton#TreeDockCollapsedButton {"
                    " color: #1F2A3A; font-size: 11px; font-weight: 600;"
                    " border: 1px solid rgba(0, 0, 0, 0.10);"
                    " border-top-color: rgba(255, 255, 255, 0.92);"
                    " border-radius: 10px;"
                    " padding: 0;"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #FBFCFE, stop:1 #E2E8F0);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:hover {"
                    " border-color: rgba(37, 99, 235, 1.0); border-top-color: rgba(37, 99, 235, 1.0);"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #FFFFFF, stop:1 #E9EFF7);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:pressed {"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #E2E8F0, stop:1 #D3DBE6);"
                    "}");
        case 14:   // 悬浮·石墨现代
            return QStringLiteral(
                    "QPushButton#TreeDockCollapsedButton {"
                    " color: #D5DAE1; font-size: 11px; font-weight: 600;"
                    " border: 1px solid rgba(0, 0, 0, 0.30);"
                    " border-top-color: rgba(255, 255, 255, 0.06);"
                    " border-radius: 10px;"
                    " padding: 0;"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #343A42, stop:1 #20242A);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:hover {"
                    " border-color: rgba(108, 142, 174, 1.0); border-top-color: rgba(108, 142, 174, 1.0);"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #3D4550, stop:1 #262B32);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:pressed {"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #20242A, stop:1 #1A1D22);"
                    "}");
        case 15:   // 悬浮·石墨哑光
            return QStringLiteral(
                    "QPushButton#TreeDockCollapsedButton {"
                    " color: #D9DDE3; font-size: 11px; font-weight: 600;"
                    " border: 1px solid rgba(0, 0, 0, 0.30);"
                    " border-top-color: rgba(255, 255, 255, 0.06);"
                    " border-radius: 10px;"
                    " padding: 0;"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #2E343C, stop:1 #1C1F25);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:hover {"
                    " border-color: rgba(106, 112, 121, 1.0); border-top-color: rgba(106, 112, 121, 1.0);"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #363D46, stop:1 #23272E);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:pressed {"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #1C1F25, stop:1 #16181C);"
                    "}");
        default:   // 12 悬浮卡片 / 11 GitCode 暗色：贴合卡片的 #252526 底色
            return QStringLiteral(
                    "QPushButton#TreeDockCollapsedButton {"
                    " color: #C6C6C6; font-size: 11px; font-weight: 600;"
                    " border: 1px solid rgba(0, 0, 0, 0.32);"
                    " border-top-color: rgba(255, 255, 255, 0.06);"
                    " border-radius: 10px;"
                    " padding: 0;"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #34343A, stop:1 #1A1A1D);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:hover {"
                    " border-color: rgba(122, 127, 136, 1.0); border-top-color: rgba(122, 127, 136, 1.0);"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #3E3E45, stop:1 #232327);"
                    "}"
                    "QPushButton#TreeDockCollapsedButton:pressed {"
                    " background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "                             stop:0 #1C1C1F, stop:1 #171719);"
                    "}");
    }
}

void applyRoundedMask(QWidget* w, int radius) {
    if (!w) return;
    const QRect r = w->rect();
    if (r.isEmpty()) return;
    QPainterPath path;
    path.addRoundedRect(QRectF(r), radius, radius);
    w->setMask(QRegion(path.toFillPolygon().toPolygon()));
}
} // namespace
#include <qaction.h>
#include <qdebug.h>
#include <qmenu.h>

namespace
{
// A small custom title bar for frameless floating QDockWidget.
// - Provides drag-to-move behavior
// - Provides a close button
class DockTitleBar final : public QWidget {
public:
    explicit DockTitleBar(QDockWidget* dock, const QString& title, QWidget* parent = nullptr)
        : QWidget(parent), m_dock(dock) {
        setObjectName("DockTitleBar");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // 标题栏保持适度高度：左侧竖条向下延伸，底端横线作为标题/内容分隔
        // §50：38 → 40，给微渐变/高光/装饰图标留出呼吸空间
        setFixedHeight(kDockTitleBarHeight);
        // 自定义 QWidget 必须开启 StyledBackground，QSS 的 background-color/border 才会绘制
        setAttribute(Qt::WA_StyledBackground, true);
        // 标题栏底色/圆角由 paintEvent 用抗锯齿路径绘制；
        // QSS 这里只保留一个不透明底色（注意不能用 background: transparent，
        // 那会让 Qt 把整块控件当透明处理，连 paintEvent 的输出一起丢掉）。
        setStyleSheet(
                "DockTitleBar {"
                "  background-color: #252526;"
                "  border: none;"
                "}"
                "DockTitleLabel {"
                "  color: #FFFFFF !important;"
                "  font-size: 12px !important;"
                "  font-weight: 700 !important;"
                "  background: transparent !important;"
                "}"
                "DockTitleCloseButton {"
                "  background: transparent;"
                "  border: none;"
                "}"
                "DockTitleCloseButton:hover {"
                "  background-color: rgba(255, 255, 255, 0.15);"
                "  border-radius: 6px;"
                "}");

        auto* layout = new QHBoxLayout(this);
        // §50c：左边距留给 paintEvent 画的"叠层图标"，文字从 kTitleLabelLeft 开始
        layout->setContentsMargins(kTitleLabelLeft, 0, 8, 0);
        layout->setSpacing(6);

        m_titleLabel = new QLabel(title, this);
        m_titleLabel->setObjectName("DockTitleLabel");
        m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(m_titleLabel);

        // §49：收起按钮（仅悬浮时显示）——点击后整块树卡片收成一个主题色小方块
        m_collapseBtn = new QPushButton(QStringLiteral("–"), this);
        m_collapseBtn->setObjectName("DockTitleCollapseButton");
        m_collapseBtn->setFixedSize(22, 22);
        m_collapseBtn->setFlat(true);
        m_collapseBtn->setFocusPolicy(Qt::NoFocus);
        m_collapseBtn->setCursor(Qt::PointingHandCursor);
        m_collapseBtn->setToolTip(QStringLiteral("收起模型树"));
        m_collapseBtn->setVisible(false);
        layout->addWidget(m_collapseBtn);
        connect(m_collapseBtn, &QPushButton::clicked, this, [this]() {
            if (onCollapse) onCollapse();
        });

        m_closeBtn = new QPushButton(QStringLiteral("×"), this);
        m_closeBtn->setObjectName("DockTitleCloseButton");
        m_closeBtn->setFixedSize(22, 22);
        m_closeBtn->setFlat(true);
        m_closeBtn->setFocusPolicy(Qt::NoFocus);
        m_closeBtn->setCursor(Qt::PointingHandCursor);
        layout->addWidget(m_closeBtn);

        if (m_dock) {
            connect(m_closeBtn, &QPushButton::clicked, m_dock, &QDockWidget::close);
        }

        // 按当前全局主题初始化标题栏配色（文字/图标/底图）
        applyTheme();
    }

    void setTitle(const QString& t) {
        if (m_titleLabel) m_titleLabel->setText(t);
    }

    // §49：收起回调（由 igQtModelDialogWidget 注入）；收起按钮只在悬浮时显示
    void setCollapseVisible(bool visible) {
        if (m_collapseBtn) m_collapseBtn->setVisible(visible);
    }
    std::function<void()> onCollapse;

    // 按全局风格模式刷新标题栏配色；切换主题时由 igQtMainWindow::applyStyleMode 调用
    void applyTheme() {
        // §45：浅色族判断统一走 globalLightBackground()（含 13 悬浮·浅色），
        //      否则浅色变体上会出现白字压浅底。
        const bool light = igQtRenderWidget::globalLightBackground();

        if (m_titleLabel) {
            QString labelStyle;
            if (light) {
                labelStyle = QStringLiteral("color: #1A1A1A; font-size: 12px; font-weight: 700; background: transparent;");
            } else {
                labelStyle = QStringLiteral("color: #FFFFFF; font-size: 12px; font-weight: 700; background: transparent;");
            }
            m_titleLabel->setStyleSheet(labelStyle);
        }
        // 幽灵 × 按钮：平时透明，hover 才高亮；颜色随主题切换
        if (m_closeBtn) {
            if (light) {
                m_closeBtn->setStyleSheet(
                        "QPushButton#DockTitleCloseButton {"
                        " color: #4A5568; background: transparent; border: none; border-radius: 6px;"
                        " font-size: 15px; padding: 0;"
                        "}"
                        "QPushButton#DockTitleCloseButton:hover { background-color: #E2E8F0; }");
            } else {
                m_closeBtn->setStyleSheet(
                        "QPushButton#DockTitleCloseButton {"
                        " color: #A5ADB8; background: transparent; border: none; border-radius: 6px;"
                        " font-size: 15px; padding: 0;"
                        "}"
                        "QPushButton#DockTitleCloseButton:hover { background-color: rgba(255,255,255,0.10); }");
            }
        }
        // §49：收起按钮与关闭按钮同款幽灵样式
        if (m_collapseBtn) {
            if (light) {
                m_collapseBtn->setStyleSheet(
                        "QPushButton#DockTitleCollapseButton {"
                        " color: #4A5568; background: transparent; border: none; border-radius: 6px;"
                        " font-size: 16px; font-weight: 700; padding: 0 0 3px 0;"
                        "}"
                        "QPushButton#DockTitleCollapseButton:hover { background-color: #E2E8F0; }");
            } else {
                m_collapseBtn->setStyleSheet(
                        "QPushButton#DockTitleCollapseButton {"
                        " color: #A5ADB8; background: transparent; border: none; border-radius: 6px;"
                        " font-size: 16px; font-weight: 700; padding: 0 0 3px 0;"
                        "}"
                        "QPushButton#DockTitleCollapseButton:hover { background-color: rgba(255,255,255,0.10); }");
            }
        }
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (!m_dock) return QWidget::mousePressEvent(e);
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragOffset = e->globalPos() - m_dock->frameGeometry().topLeft();
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dock) return QWidget::mouseMoveEvent(e);
        if (m_dragging && (e->buttons() & Qt::LeftButton)) {
            m_dock->move(e->globalPos() - m_dragOffset);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragging = false;
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect();
        // 按全局风格模式适配：0=原始深色(中性灰) 1=现代深色(青蓝) 2=浅色 3=石墨深色
        const int styleMode = igQtRenderWidget::globalStyleMode();
        // §45：悬浮卡片的换色变体沿用各自颜色族的标题栏配色（13→2 / 14→9 / 15→10），
        //      但外形仍按"卡片布局"处理（styleMode 保留原值）。
        int mode = styleMode;
        if (mode == 13) mode = 2;
        else if (mode == 14) mode = 9;
        else if (mode == 15) mode = 10;

        QColor titleBg, accent, bottomLine;
        switch (mode) {
            case 0:  // 原始深色：中性灰，避免偏蓝
                titleBg    = QColor("#2D2D30");
                accent     = QColor("#3F3F46");
                bottomLine = QColor(255, 255, 255, 40);
                break;
            case 1:  // 现代深色：青蓝
                titleBg    = QColor("#2A3648");
                accent     = QColor("#38BDF8");
                bottomLine = QColor(56, 189, 248, 115);
                break;
            case 2:  // 浅色：浅灰蓝 + 蓝色强调
                titleBg    = QColor("#E4E9EF");
                accent     = QColor("#3B82F6");
                bottomLine = QColor("#8DA2B8");
                break;
            case 3:  // 石墨深色：扁平灰黑
                titleBg    = QColor("#2B2D30");
                accent     = QColor("#4A4E54");
                bottomLine = QColor(255, 255, 255, 30);
                break;
            case 4:  // 深空青蓝：深灰 + 蓝青
                titleBg    = QColor("#24282E");
                accent     = QColor("#4DD0E1");
                bottomLine = QColor(77, 208, 225, 115);
                break;
            case 6:  // 工作台：深灰 + 蓝青
                titleBg    = QColor("#24282E");
                accent     = QColor("#4DD0E1");
                bottomLine = QColor(77, 208, 225, 115);
                break;
            case 7:  // 石墨·视图栏：扁平灰黑
                titleBg    = QColor("#2B2D30");
                accent     = QColor("#4A4E54");
                bottomLine = QColor(255, 255, 255, 30);
                break;
            case 8:  // 深空·视图栏：深灰 + 蓝青
                titleBg    = QColor("#24282E");
                accent     = QColor("#4DD0E1");
                bottomLine = QColor(77, 208, 225, 115);
                break;
            case 9:  // 石墨·现代：分层深灰 + 低饱和蓝
                titleBg    = QColor("#22262C");
                accent     = QColor("#6C8EAE");
                bottomLine = QColor(108, 142, 174, 90);
                break;
            case 10: // 石墨·哑光：纯哑光深灰 + 暗青灰强调
                titleBg    = QColor("#20242A");
                accent     = QColor("#2A303A");
                bottomLine = QColor(42, 48, 58, 120);
                break;
            case 11: // GitCode 暗色：纯灰阶
                titleBg    = QColor("#252526");
                accent     = QColor("#37373D");
                bottomLine = QColor(55, 55, 61, 120);
                break;
            case 12: // 悬浮卡片：标题栏整行保持之前的灰色 + 底部可见淡细线
                titleBg    = QColor("#252526");
                accent     = QColor("#37373D");
                bottomLine = QColor("#4A4D52");
                break;
            default:
                titleBg    = QColor("#2A3648");
                accent     = QColor("#38BDF8");
                bottomLine = QColor(56, 189, 248, 115);
                break;
        }

        // 圆角标题栏：直接填充“圆角路径”（fillPath 走抗锯齿），
        // 不能用 setClipPath()——裁剪路径是 1bit 的，Antialiasing 对裁剪无效，弧线必然呈阶梯。
        // 悬浮卡片(12) 统一 8px，与中央视口 / 悬浮卡片一致；其余原本圆角的主题沿用原半径。
        // §45：13/14/15 也是卡片布局，圆角同样按 8px（判断用原始 styleMode）。
        const bool rounded = (mode == 9 || mode == 10 || mode == 11 || mode == 12 || styleMode >= 12);
        const qreal radius = (styleMode >= 12) ? 8.0 : ((mode == 9) ? 6.0 : 4.0);
        QPainterPath titlePath;
        if (rounded) {
            titlePath.addRoundedRect(QRectF(r), radius, radius);
        } else {
            titlePath.addRect(QRectF(r));
        }

        // §50 方案 A′：不再用单色平铺，改成"同一底色的浅→深"纵向微渐变（不引入新颜色）。
        const bool light = igQtRenderWidget::globalLightBackground();
        const QColor bgTop = light ? titleBg.lighter(104) : titleBg.lighter(112);
        QLinearGradient bgGrad(0, 0, 0, r.height());
        bgGrad.setColorAt(0.0, bgTop);
        bgGrad.setColorAt(1.0, titleBg);
        p.fillPath(titlePath, bgGrad);

        // 顶部 1px 高光：深色族用低透明白，浅色族用低透明黑（都是"该主题自己的明暗方向"）
        const QColor highlight = light ? QColor(0, 0, 0, 16) : QColor(255, 255, 255, 15);
        QPainterPath hiPath;
        hiPath.addRect(QRectF(r.left() + 6, r.top(), r.width() - 12, 1));
        p.fillPath(hiPath.intersected(titlePath), highlight);

        // ---- 左侧只保留叠层图标（§50c：按用户意见去掉胶囊强调与抓握点）----
        // accent 原本只给左侧胶囊用，去掉后这里显式标记未使用；颜色表保留以便将来复用。
        Q_UNUSED(accent);
        const int cy = r.height() / 2;
        {
            const int x = kTitleLeftPad;
            // 叠层图标：两个圆角方块（后浅前深），用标题文字色的两档透明度（不引入新颜色）
            const QColor iconBase = light ? QColor("#1A1A1A") : QColor("#FFFFFF");
            QColor back = iconBase;
            back.setAlpha(55);
            QColor front = iconBase;
            front.setAlpha(130);
            QPainterPath backPath;
            backPath.addRoundedRect(QRectF(x, cy - 7, 12, 9), 2.0, 2.0);
            p.fillPath(backPath.intersected(titlePath), back);
            QPainterPath frontPath;
            frontPath.addRoundedRect(QRectF(x + 3, cy - 2, 12, 9), 2.0, 2.0);
            p.fillPath(frontPath.intersected(titlePath), front);
        }

        // 底部分隔线：1px、中间实两端淡出（颜色仍是各主题原有的 bottomLine）
        {
            QColor line = bottomLine;
            line.setAlpha(qMin(255, int(line.alpha() * 0.95)));
            QColor fade = line;
            fade.setAlpha(0);
            QLinearGradient lineGrad(r.left(), 0, r.right(), 0);
            lineGrad.setColorAt(0.00, fade);
            lineGrad.setColorAt(0.14, line);
            lineGrad.setColorAt(0.86, line);
            lineGrad.setColorAt(1.00, fade);
            QPainterPath bottomPath;
            bottomPath.addRect(QRectF(r.left(), r.bottom() - 1.0, r.width(), 1.0));
            p.fillPath(bottomPath.intersected(titlePath), QBrush(lineGrad));
        }
    }

private:
    QDockWidget* m_dock = nullptr;
    QLabel* m_titleLabel = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QPushButton* m_collapseBtn = nullptr;   // §49：收起按钮
    bool m_dragging = false;
    QPoint m_dragOffset;
};

constexpr int SubObjectLoadedRole = Qt::UserRole + 1;

static bool HasSubObjectTreeChildren(iGame::DataObject::Pointer obj) {
    if (!obj) return false;
    if (obj->HasSubDataObject()) return true;

    auto attrSet = obj->GetAttributeSet();
    if (!attrSet) return false;

    auto all = attrSet->GetAllAttributes();
    for (int i = 0; i < all->GetNumberOfElements(); ++i) {
        if (!all->GetElement(i).isDeleted) return true;
    }
    return false;
}

// Build only the immediate sub-object rows. Their contents are populated on expansion.
static void BuildSubObjectTreeSkeleton(
        QTreeWidgetItem* parentItem, iGame::DataObject::Pointer obj) {
    if (!obj || !obj->HasSubDataObject()) return;

    for (auto it = obj->SubDataObjectIteratorBegin(); it != obj->SubDataObjectIteratorEnd(); ++it) {
        auto sub = it->second;
        auto* childItem = new SubObjectTreeWidgetItem(parentItem);
        childItem->setDataObject(sub);
        // default name fallback if empty
        std::string subName = sub->GetName();
        if (subName.empty()) { subName = std::string("Block_") + std::to_string(sub->GetDataObjectId()); }
        childItem->setName(QString::fromStdString(subName));
        childItem->SyncIconWithVisibility(false);
        childItem->setData(0, SubObjectLoadedRole, false);
        childItem->setChildIndicatorPolicy(HasSubObjectTreeChildren(sub)
                                                   ? QTreeWidgetItem::ShowIndicator
                                                   : QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }
}

static void PopulateSubObjectTreeItem(
        QTreeWidget* tree, SubObjectTreeWidgetItem* item) {
    if (!item || item->data(0, SubObjectLoadedRole).toBool()) return;
    item->setData(0, SubObjectLoadedRole, true);

    auto obj = item->getDataObject();
    if (!obj) return;

    if (auto attrSet = obj->GetAttributeSet()) {
        auto all = attrSet->GetAllAttributes();
        for (int i = 0; i < all->GetNumberOfElements(); ++i) {
            auto& attr = all->GetElement(i);
            if (attr.isDeleted) continue;

            auto* attrItem = new SubAttribTreeWidgetItem(i, tree, item);
            const QString attrName = QString::fromStdString(attr.pointer->GetName());
            attrItem->setText(0, attrName);
            attrItem->setToolTip(0, attrName);
            if (attr.attachmentType == IG_POINT) {
                attrItem->setIcon(0, igQtModelTreeIcons::Point());
            } else if (attr.attachmentType == IG_CELL) {
                attrItem->setIcon(0, igQtModelTreeIcons::Cell());
            }
            attrItem->setDimension(attr.pointer->GetDimension());
        }
    }

    BuildSubObjectTreeSkeleton(item, obj);
    item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
}
} // namespace

igQtModelDialogWidget::igQtModelDialogWidget(QWidget* parent) : QObject(parent), ui(new Ui::LayerDialog) {
    // 用臨時 QDockWidget 載入 UI，以取得 modelTreeWidget 與 tabWidget
    QDockWidget dummy;
    ui->setupUi(&dummy);

    tabWidget = ui->tabWidget;
    modelTreeWidget = ui->modelTreeWidget;
    propertyWidget = ui->propertyWidget;

    // 紧凑布局：给树/属性面板一个较窄的默认宽度（200~240），避免左侧占用过多显示空间
    int totalWidth = parent ? qBound(200, parent->width() / 10, 240) : 220;

    // 上半部分：圖層/模型樹 Dock（可單獨拖出懸浮）
    m_treeDock = new QDockWidget(QStringLiteral("模型树"), parent);
    m_treeDock->setObjectName("LayerTreeDock");
    m_treeDock->setWidget(modelTreeWidget);
    m_treeDock->setMinimumWidth(totalWidth);
    // LayerDialog 允许悬浮 + 可拖动（可关闭、可移动、可浮动）
    m_treeDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable);
    m_treeDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::TopDockWidgetArea);

    // 避免透明背景造成 Dock 穿透
    m_treeDock->setAttribute(Qt::WA_TranslucentBackground, false);
    // 自定义标题栏（用于无边框 floating 时提供可拖拽移动）
    auto* treeTitle = new DockTitleBar(m_treeDock, m_treeDock->windowTitle(), m_treeDock);
    m_treeDock->setTitleBarWidget(treeTitle);
    // §49：收起/展开接线（收起按钮只在悬浮时显示）
    m_treeTitleBar = treeTitle;
    treeTitle->onCollapse = [this]() { setTreeDockCollapsed(true); };
    m_setCollapseVisible = [treeTitle](bool visible) { treeTitle->setCollapseVisible(visible); };

    //  Properties Dock（也可懸浮）
    m_propertiesDock = new QDockWidget(QStringLiteral("属性"), parent);
    m_propertiesDock->setObjectName("LayerPropertiesDock");
    m_propertiesDock->setWidget(tabWidget);
    m_propertiesDock->setMinimumWidth(totalWidth);
    // Properties 不允许悬浮/拖动（只保留可关闭）
    m_propertiesDock->setFeatures(QDockWidget::DockWidgetClosable);
    m_propertiesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::TopDockWidgetArea);

    m_propertiesDock->setAttribute(Qt::WA_TranslucentBackground, false);
    // Properties 使用 Qt 默认 dock 标题栏（与普通 dock 一致）
    m_propertiesDock->setTitleBarWidget(nullptr);

    // floating 时强制无系统边框（但仍可通过自定义 title bar 拖拽移动）
    connect(m_treeDock, &QDockWidget::topLevelChanged, m_treeDock, [this](bool floating) {
        if (!m_treeDock) return;
        if (floating) {
            m_treeDock->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
            // 關閉透明背景：使用樣式表來控制外觀與邊框
            m_treeDock->setAttribute(Qt::WA_TranslucentBackground, false);
            m_treeDock->show();
            // 棱角分明：清除圆角蒙版
            m_treeDock->setMask(QRegion());
        } else {
            // 回到 docked：让 Qt 恢复正常 DockWidget 行为
            // §49：停靠态不支持收起，先还原（否则会留一个小方块在停靠区）
            setTreeDockCollapsed(false);
            m_treeDock->setWindowFlags(Qt::Widget);
            m_treeDock->show();
            m_treeDock->setMask(QRegion());
        }
        // §49：收起按钮只在悬浮（卡片态）时出现
        if (m_setCollapseVisible) m_setCollapseVisible(floating && !m_treeCollapsed);
    });
    m_treeDock->installEventFilter(this);
    connect(m_propertiesDock, &QDockWidget::topLevelChanged, m_propertiesDock, [this](bool floating) {
        if (!m_propertiesDock) return;
        if (floating) {
            m_propertiesDock->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
            m_propertiesDock->setAttribute(Qt::WA_TranslucentBackground, false);
            m_propertiesDock->show();
        } else {
            m_propertiesDock->setWindowFlags(Qt::Widget);
            m_propertiesDock->show();
        }
    });

    tabWidget->addTab(ui->ModelInformationWidget, QStringLiteral("模型信息"));
    tabWidget->addTab(ui->propertyWidget, QStringLiteral("模型属性"));

    // 根据总宽度调整列宽
    int col1Width = totalWidth * 0.4;
    int col2Width = totalWidth * 0.6;


    modelTreeWidget->setColumnCount(2);
    modelTreeWidget->header()->hide();
    // 两列随窗口宽度自适应：名字列占 36%（原 42%，收窄一点），右侧属性列占剩余（更大）。
    modelTreeWidget->setLeftColumnPercent(36);
    // 底部横向滚动条不好看，且列宽已按视口宽度自适应，直接隐藏（不会因此看不全内容）
    modelTreeWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 减小缩进，让模型和 attribute 文本更靠近左侧
    modelTreeWidget->setIndentation(8);
    modelTreeWidget->setAlternatingRowColors(true);
    modelTreeWidget->setUniformRowHeights(true);
    // 紧凑行高与图标：20px 按钮 + 24px 行高即可完整显示
    modelTreeWidget->setIconSize(QSize(16, 16));
    modelTreeWidget->setStyleSheet(modelTreeWidget->styleSheet() +
                                   QStringLiteral("QTreeView::item{height:24px;}"
                                                  "QTreeView{font-size:12px;}"));

    connect(modelTreeWidget, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* treeItem) {
        auto* subItem = dynamic_cast<SubObjectTreeWidgetItem*>(treeItem);
        if (!subItem || subItem->data(0, SubObjectLoadedRole).toBool()) return;

        modelTreeWidget->setUpdatesEnabled(false);
        PopulateSubObjectTreeItem(modelTreeWidget, subItem);
        modelTreeWidget->setUpdatesEnabled(true);
        modelTreeWidget->viewport()->update();
    });


    propertyWidget->setHeaderVisible(false);
    // 属性浏览器紧凑化：12px 字体，避免名称/值两列被大字号撑得过宽
    propertyWidget->setStyleSheet(QStringLiteral(
            "QTreeView { font-size: 12px; }"
            "QLabel { font-size: 12px; }"
            "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { font-size: 12px; }"));
    propertyManager = new QtVariantPropertyManager(propertyWidget);
    editFactory = new QtVariantEditorFactory(propertyWidget);
    propertyWidget->setFactoryForManager(propertyManager, editFactory);

    propertyWidget->removeProperty(objectGroup);
    objectGroup =
            propertyManager->addProperty(QtVariantPropertyManager::groupTypeId(), QStringLiteral("对象属性"));
    propertyWidget->addProperty(objectGroup);

    // QtTreePropertyBrowser 内部用 QItemDelegate 绘制选中行，会使用 QPalette::Highlight（Windows 上常为蓝色）。
    // 与主窗口 QSS 中银色选中行一致，改为银色 + 深色文字。
    for (QTreeWidget* tw : propertyWidget->findChildren<QTreeWidget*>()) {
        QPalette pal = tw->palette();
        pal.setColor(QPalette::Active, QPalette::Highlight, QColor(0xC0, 0xC0, 0xC0));
        pal.setColor(QPalette::Inactive, QPalette::Highlight, QColor(0xA8, 0xA8, 0xAC));
        pal.setColor(QPalette::Active, QPalette::HighlightedText, QColor(0x25, 0x25, 0x26));
        pal.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor(0x25, 0x25, 0x26));
        tw->setPalette(pal);
    }

    prop_PointSize = propertyManager->addProperty(QVariant::Int, QStringLiteral("点大小"));
    prop_PointSize->setEnabled(false);
    prop_PointSize->setValue(0);
    objectGroup->addSubProperty(prop_PointSize);
    propertyManager->setAttribute(prop_PointSize, "minimum", 1);
    propertyManager->setAttribute(prop_PointSize, "maximum", 99);
    propertyManager->setAttribute(prop_PointSize, "singleStep", 1);

    pror_LineWidth = propertyManager->addProperty(QVariant::Int, QStringLiteral("线宽"));
    pror_LineWidth->setEnabled(false);
    pror_LineWidth->setValue(0);
    objectGroup->addSubProperty(pror_LineWidth);
    propertyManager->setAttribute(pror_LineWidth, "minimum", 1);
    propertyManager->setAttribute(pror_LineWidth, "maximum", 10);
    propertyManager->setAttribute(pror_LineWidth, "singleStep", 1);

    prop_Transparency = propertyManager->addProperty(QVariant::Double, QStringLiteral("透明度"));
    prop_Transparency->setEnabled(false);
    prop_Transparency->setValue(0);
    objectGroup->addSubProperty(prop_Transparency);
    propertyManager->setAttribute(prop_Transparency, "minimum", 0.0);
    propertyManager->setAttribute(prop_Transparency, "maximum", 1.0);
    propertyManager->setAttribute(prop_Transparency, "singleStep", 0.1);




    connect(propertyManager, &QtVariantPropertyManager::valueChanged, this, &igQtModelDialogWidget::onPropertyChanged);

    ui->ModelInformationWidget->hide();
    //connect(modelTreeWidget, &igQtModelTreeWidget::ChangeCurrentModel, this, &igQtModelDialogWidget::UpdateCurrentModel);
    connect(modelTreeWidget, &igQtModelTreeWidget::ChangeCurrentModel, this,
            static_cast<void (igQtModelDialogWidget::*)(iGame::Model*)>(
                    &igQtModelDialogWidget::updateCurrentModelProperty));

    connect(modelTreeWidget, &igQtModelTreeWidget::ChangeCurrentModel, this,
            &igQtModelDialogWidget::updateCurrentModelInfo);
    //connect(modelTreeWidget, &igQtModelTreeWidget::ChangeCurrentModel, this, &igQtModelDialogWidget::updateCloudPicture);
    connect(modelTreeWidget, &igQtModelTreeWidget::ViewCloudPicture, this, &igQtModelDialogWidget::updateCloudPicture);
}

void igQtModelDialogWidget::refreshStyle() {
    if (m_treeDock) {
        if (auto* bar = dynamic_cast<DockTitleBar*>(m_treeDock->titleBarWidget())) {
            bar->applyTheme();
        }
    }
    // §49：收起态小方块的配色也要跟着主题走
    refreshCollapsedBlockStyle();
    // 模型信息页文字是动态创建并按当时主题上色的，切主题时要重建，避免浅色/深色文字残留
    if (ui && ui->ModelInformationWidget) {
        ui->ModelInformationWidget->updateInformationFrame();
    }
}

// §49：收起态小方块的样式刷新（按当前颜色族）
// §51：同时把"面"（叠层图标 + 名称）重画成图标 —— 收起态与标题栏用同一套视觉语言
void igQtModelDialogWidget::refreshCollapsedBlockStyle() {
    if (!m_collapsedBlock) return;
    m_collapsedBlock->setStyleSheet(collapsedBlockQss());
    if (auto* btn = qobject_cast<QPushButton*>(m_collapsedBlock)) {
        qreal dpr = 1.0;
        if (m_treeDock) {
            if (QScreen* scr = m_treeDock->screen()) dpr = scr->devicePixelRatio();
        }
        const int faceSize = kTreeCollapsedSize - 4;   // §51b：方块内缩 2px，面按内层尺寸画
        btn->setIcon(QIcon(buildCollapsedFacePixmap(faceSize, dpr)));
        btn->setIconSize(QSize(faceSize, faceSize));
    }
    // §51b：收起态外圈底色跟着颜色族（同时让遮罩台阶"藏"在深色里）
    if (m_treeDock && m_treeCollapsed) {
        m_treeDock->setStyleSheet(
                QStringLiteral("QDockWidget#LayerTreeDock { background-color: %1; border: none; }")
                        .arg(collapsedRingColor()));
    }
}

// §49：模型树卡片收起 / 展开。
//   收起：内容与标题栏隐藏（不重 parent、不切换 titleBarWidget，避免打断树内 itemWidget 状态/
//         旧标题栏被销毁），用一个内缩 2px 的圆角渐变小方块做"收起态"，右下角保持不动；
//   展开：还原内容/标题栏/最小尺寸与原几何（尺寸和位置都回到收起前）。
void igQtModelDialogWidget::setTreeDockCollapsed(bool collapsed) {
    if (!m_treeDock) return;
    if (collapsed == m_treeCollapsed) return;
    // 停靠态不做收起（会变成停靠区里的一个小方块，很怪）
    if (collapsed && !m_treeDock->isFloating()) return;

    if (collapsed) {
        m_treeGeomBeforeCollapse = m_treeDock->geometry();
        m_treeMinBeforeCollapse = m_treeDock->minimumSize();
        m_treeDockSavedStyleSheet = m_treeDock->styleSheet();   // §51b：收起时改外圈底色，展开时还原
        m_treeCollapsed = true;

        // 内容与标题栏都藏起来（标题栏**不切换**：setTitleBarWidget 会销毁旧标题栏，风险大）
        if (QWidget* content = m_treeDock->widget()) content->hide();
        if (m_treeTitleBar) m_treeTitleBar->hide();

        if (!m_collapsedBlock) {
            auto* block = new QPushButton(m_treeDock);
            block->setObjectName(QStringLiteral("TreeDockCollapsedButton"));
            block->setCursor(Qt::SizeAllCursor);   // §49b：可拖动
            block->setFocusPolicy(Qt::NoFocus);
            // §51：不再用控件文字；"叠层图标 + 名称"由图标位图一次性渲染（同一层，居中对齐更稳）
            block->setText(QString());
            block->setToolTip(QStringLiteral("点击展开模型树；按住可拖动"));
            connect(block, &QPushButton::clicked, this, [this]() { setTreeDockCollapsed(false); });
            block->installEventFilter(this);        // §49b：拖动/点击判定
            m_collapsedBlock = block;
        }
        refreshCollapsedBlockStyle();

        m_treeDock->setMinimumSize(kTreeCollapsedSize, kTreeCollapsedSize);
        m_treeDock->setMaximumSize(kTreeCollapsedSize, kTreeCollapsedSize);
        m_treeDock->resize(kTreeCollapsedSize, kTreeCollapsedSize);
        // §49c：锚点必须用**实际**尺寸 —— Qt 可能因为最小尺寸/布局把窗口撑得比请求值大，
        //        用请求值算锚点会让"收起→展开"每循环一次漂移几像素。这里改成 resize 后回读 size()。
        const QSize actual = m_treeDock->size();
        const QRect g = m_treeGeomBeforeCollapse;
        m_treeDock->move(g.right() - actual.width() + 1, g.bottom() - actual.height() + 1);

        // §51b：方块**内缩 2px**，让 QSS 自绘的抗锯齿圆角落在 1bit 窗口遮罩之内 ——
        //        可见的那条边就是平滑的 AA 边，遮罩的 1px 台阶被留在外圈，且外圈不再有亮描边。
        m_collapsedBlock->setParent(m_treeDock);
        m_collapsedBlock->setGeometry(2, 2, qMax(8, actual.width() - 4), qMax(8, actual.height() - 4));
        m_collapsedBlock->raise();
        m_collapsedBlock->show();
        m_treeDock->show();
        // §51c：记下"方块此刻所在矩形"——展开时用它当基准算用户挪动的位移量（不影响下一轮的基准）
        m_blockRectAtCollapse = m_treeDock->geometry();
    } else {
        m_treeCollapsed = false;
        // §51b：还原 dock 自身的样式表（收起时为了压暗外圈临时改过）
        m_treeDock->setStyleSheet(m_treeDockSavedStyleSheet);
        if (m_collapsedBlock) m_collapsedBlock->hide();
        if (m_treeTitleBar) m_treeTitleBar->show();
        m_treeDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_treeDock->setMinimumSize(m_treeMinBeforeCollapse.isValid() ? m_treeMinBeforeCollapse
                                                                    : QSize(0, 0));
        if (QWidget* content = m_treeDock->widget()) content->show();
        const QRect g = m_treeGeomBeforeCollapse;
        if (g.isValid() && !g.isEmpty()) {
            // §51c：展开位置 = 保存的"展开态矩形" + 用户把方块挪动的位移量。
            //   位移量以**收起那一刻方块所在位置**为基准计算（m_blockRectAtCollapse），
            //   所以连续"收起→展开"不会累积任何漂移：没挪动时 delta == 0，就是精确回原位；
            //   真拖动过则平移相同距离出现在你放下的位置。
            const QPoint delta = m_treeDock->geometry().topLeft() - m_blockRectAtCollapse.topLeft();
            QRect target = g.translated(delta);
            clampRectToAvailable(target, m_treeDock);
            m_treeDock->resize(target.size());
            const QSize actual = m_treeDock->size();          // 布局可能让实际尺寸 ≠ 请求尺寸
            QRect actualRect(target.topLeft(), actual);
            clampRectToAvailable(actualRect, m_treeDock);
            m_treeDock->setGeometry(actualRect);
            QTimer::singleShot(0, this, [this, actualRect]() {
                if (m_treeDock && !m_treeCollapsed) { m_treeDock->setGeometry(actualRect); }
            });
        }
        m_treeDock->show();
    }
    if (m_setCollapseVisible) m_setCollapseVisible(m_treeDock->isFloating() && !m_treeCollapsed);
}

ModelTreeWidgetItem* igQtModelDialogWidget::getItemFromObject(iGame::DataObject::Pointer obj) {
    // 遍历子项
    for (int i = 0; i < modelTreeWidget->topLevelItemCount(); ++i) {
        ModelTreeWidgetItem* item = dynamic_cast<ModelTreeWidgetItem*>(modelTreeWidget->topLevelItem(i));
        if (item->getModel()->GetDataObject() == obj) { return item; }
    }
    return nullptr;
}
void igQtModelDialogWidget::updateItemName(iGame::DataObject::Pointer obj) {
    auto item = getItemFromObject(obj);
    if (!item) return;
    item->setName(QString::fromStdString(obj->GetName()));
    return;
}
void igQtModelDialogWidget::updateAllAttriubute(iGame::DataObject::Pointer obj) {
    auto item = getItemFromObject(obj);
    if (!item) return;
    item->setCurrentChild(nullptr);

    while (item->childCount() > 0) { delete item->takeChild(0); }
    auto attrSet = obj->GetAttributeSet()->GetAllAttributes();
    for (int i = 0; i < attrSet->GetNumberOfElements(); i++) {
        auto& attr = attrSet->GetElement(i);
        if (attr.isDeleted) continue;
        if (attr.type == IG_BLOCK_MAPPING) continue;
        AttribTreeWidgetItem* child = new AttribTreeWidgetItem(i, modelTreeWidget, item);
        //if (obj->GetAttributeIndex() == i) {
        //    item->setCurrentChild(child);
        //    child->setSelected(true);
        //}
        const QString attrName = QString::fromStdString(attr.pointer->GetName());
        child->setText(0, attrName);
        child->setToolTip(0, attrName);
        if (attr.attachmentType == IG_POINT)
            child->setIcon(0, igQtModelTreeIcons::Point());
        else if (attr.attachmentType == IG_CELL)
            child->setIcon(0, igQtModelTreeIcons::Cell());
        child->setDimension(attr.pointer->GetDimension());
        // std::cout << i << " " << attr.pointer->GetName() << std::endl;
    }

    if (obj->HasBlockMapping()) {
        AttribTreeWidgetItem* child = new AttribTreeWidgetItem(
            obj->GetBlockMappingAttrIndex(), modelTreeWidget, item);
        child->setText(0, QString::fromStdString(obj->GetBlockMapping()->GetName()));
        child->setToolTip(0, child->text(0));
        child->setIcon(0, igQtModelTreeIcons::Cell());
        child->setDimension(1);
    }

    item->viewAttribute(-1);
    iGame::DynamicCast<iGame::DrawObject>(obj)->ForceReConvertToDrawableData();
}

QString igQtModelDialogWidget::renameModelRow(iGame::DataObject::Pointer obj, const QString& newName) {
    if (obj == nullptr || modelTreeWidget == nullptr || newName.isEmpty()) { return newName; }
    auto* item = getItemFromObject(obj);
    if (item == nullptr) { return newName; }

    // 重名检查时排除本行自己：这样“把 A 改成 A”不会变成 A_2
    QStringList existing;
    for (int i = 0; i < modelTreeWidget->topLevelItemCount(); ++i) {
        auto* row = modelTreeWidget->topLevelItem(i);
        if (row == nullptr || row == item) { continue; }
        existing << row->text(0);
    }
    QString finalName = newName;
    if (existing.contains(finalName)) {
        for (int n = 2; n < 10000; ++n) {
            const QString candidate = QStringLiteral("%1_%2").arg(newName).arg(n);
            if (!existing.contains(candidate)) {
                finalName = candidate;
                break;
            }
        }
    }

    // 同步对象自己的名字与树上那一行。
    // 注意：这里**不能**调 updateCurrentModelInfo()——它会发 CurrendModelChanged →
    // AnimationWidget::initAnimationComponents()，而那条链路里（interpolate 树的
    // updateComponentsKeyframeSum → VcrController::setKeyframe_sum）会把当前帧硬重置到 0，
    // 用户看到的就是“转换后自动跳回第一帧”。改名只影响文字，模型信息面板直接刷新即可。
    obj->SetName(finalName.toStdString());
    item->setName(finalName);
    ui->ModelInformationWidget->updateInformationFrame();
    return finalName;
}

int igQtModelDialogWidget::addDataObjectToModelTree(iGame::DataObject::Pointer obj, ItemSource source) {
    ModelTreeWidgetItem* item = new ModelTreeWidgetItem(modelTreeWidget);
    //modelTreeWidget->setCurrentModelItem(item);
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();
    unsigned int id = scene->AddModel(obj);
    iGame::Model* model = scene->GetModelById(id).get();

    //currentModel = model;
    scene->SetCurrentModel(model);

    item->setModelId(id);
    item->setName(QString::fromStdString(obj->GetName()));
    item->setModel(model);

    // build attribute children
    auto attrSet = obj->GetAttributeSet()->GetAllAttributes();
    for (int i = 0; i < attrSet->GetNumberOfElements(); i++) {
        auto& attr = attrSet->GetElement(i);
        if (attr.isDeleted) continue;
        AttribTreeWidgetItem* child = new AttribTreeWidgetItem(i, modelTreeWidget, item);
        const QString attrName = QString::fromStdString(attr.pointer->GetName());
        child->setText(0, attrName);
        child->setToolTip(0, attrName);
        if (attr.attachmentType == IG_POINT) child->setIcon(0, igQtModelTreeIcons::Point());
        else if (attr.attachmentType == IG_CELL)
            child->setIcon(0, igQtModelTreeIcons::Cell());
        child->setDimension(attr.pointer->GetDimension());
    }

    // build sub-data objects hierarchy
    BuildSubObjectTreeSkeleton(item, obj);

    modelTreeWidget->addTopLevelItem(item);
    modelTreeWidget->setCurrentItem(item);

    updateCurrentModelProperty(model);
    updateCurrentModelInfo();
    //QTreeWidgetItem* currentItem = modelTreeWidget->getCurrentModelItem();
    //std::cout << "add current model: " << currentItem << std::endl;
    return id;
}

bool igQtModelDialogWidget::eventFilter(QObject* watched, QEvent* event) {
    // §49b：收起态小方块支持拖动（拖得动就移动整块，几乎没动就当成"点击 → 展开"）。
    // 这里不吞掉 Press：让按钮自己拿到按下事件，Qt 才会建立隐式鼠标抓取，
    // 拖动过程中即使光标移出小方块也仍能收到 Move。
    if (watched == m_collapsedBlock && m_treeCollapsed && m_treeDock) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_blockDragActive = true;
                m_blockDragged = false;
                m_blockDragOffset = me->globalPos() - m_treeDock->frameGeometry().topLeft();
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (m_blockDragActive && (me->buttons() & Qt::LeftButton)) {
                const QPoint target = me->globalPos() - m_blockDragOffset;
                if (!m_blockDragged &&
                    (target - m_treeDock->frameGeometry().topLeft()).manhattanLength() > 8) {
                    m_blockDragged = true;   // 超过 8px 才算拖动（点击抖动不会被当成拖动）
                }
                if (m_blockDragged) {
                    // §49d：小方块也不允许被拖出屏幕可用区
                    QRect r(QPoint(0, 0), m_treeDock->size());
                    r.moveTopLeft(target);
                    clampRectToAvailable(r, m_treeDock);
                    m_treeDock->move(r.topLeft());
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (m_blockDragActive) {
                m_blockDragActive = false;
                if (m_blockDragged) {
                    m_blockDragged = false;
                    return true;   // 拖动结束：吞掉 release，避免再触发 clicked（那会展开）
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void igQtModelDialogWidget::refreshAttributeBadges(iGame::DataObject::Pointer obj) {
    auto item = getItemFromObject(obj);
    if (item == nullptr) { return; }

    auto updateBadge = [](QTreeWidgetItem* row, iGame::AttributeSet::Attribute& attr) {
        if (row == nullptr || attr.isDeleted || attr.pointer == nullptr) { return; }
        if (attr.attachmentType == IG_CELL) {
            row->setIcon(0, igQtModelTreeIcons::Cell());
        } else if (attr.attachmentType == IG_POINT) {
            row->setIcon(0, igQtModelTreeIcons::Point());
        }
        row->setToolTip(0, QString::fromStdString(attr.pointer->GetName()));
    };

    auto refreshRows = [&updateBadge](QTreeWidgetItem* parentRow, iGame::AttributeSet* attrs,
                                      bool subAttribRows) {
        if (parentRow == nullptr || attrs == nullptr) { return; }
        for (int i = 0; i < parentRow->childCount(); ++i) {
            int index = -1;
            if (subAttribRows) {
                auto* row = dynamic_cast<SubAttribTreeWidgetItem*>(parentRow->child(i));
                if (row == nullptr) { continue; }
                index = row->attributeIndex();
            } else {
                auto* row = dynamic_cast<AttribTreeWidgetItem*>(parentRow->child(i));
                if (row == nullptr) { continue; }
                index = row->attributeIndex();
            }
            if (index < 0 || index >= static_cast<int>(attrs->GetNumberOfAttributes())) { continue; }
            updateBadge(parentRow->child(i), attrs->GetAttribute(index));
        }
    };

    // 顶层模型行的属性行
    if (auto attrs = obj->GetAttributeSet()) { refreshRows(item, attrs, false); }

    // 已经展开（属性行已生成）的子块行
    for (int i = 0; i < item->childCount(); ++i) {
        auto* sub = dynamic_cast<SubObjectTreeWidgetItem*>(item->child(i));
        if (sub == nullptr) { continue; }
        auto subObject = sub->getDataObject();
        if (subObject == nullptr) { continue; }
        refreshRows(sub, subObject->GetAttributeSet(), true);
    }
}

int igQtModelDialogWidget::addModelToModelTree(iGame::Model::Pointer model) {
    ModelTreeWidgetItem* item = new ModelTreeWidgetItem(modelTreeWidget);
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();

    auto id = scene->AddModel(model->GetDataObject());

    item->setName(QString::fromStdString(model->GetDataObject()->GetName()));
    item->setModel(model);

    // build sub-data objects hierarchy
    BuildSubObjectTreeSkeleton(item, model->GetDataObject());

    modelTreeWidget->addTopLevelItem(item);
    modelTreeWidget->setCurrentItem(item);
    return id;
}
int igQtModelDialogWidget::updateCurrentModelInfo() {
    //    qDebug() << ui->modelTreeWidget->currentIndex();

    ui->ModelInformationWidget->updateInformationFrame();
    Q_EMIT CurrendModelChanged();


    return 1;
}
void igQtModelDialogWidget::updateCurrentModelProperty() {
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();
    if (!scene) return;
    auto model = scene->GetCurrentModel();
    if (!model) {
        // 没有模型（例如删掉了最后一个）：禁用并清零属性，避免残留已删除模型的值
        prop_PointSize->setEnabled(false);
        prop_PointSize->setValue(0);
        pror_LineWidth->setEnabled(false);
        pror_LineWidth->setValue(0);
        prop_Transparency->setEnabled(false);
        prop_Transparency->setValue(0);
        return;
    }
    //currentModel = model;
    auto obj = DynamicCast<iGame::DrawObject>(model->GetDataObject());
    if (obj) {
        prop_PointSize->setEnabled(true);
        prop_PointSize->setValue(obj->GetPointSize());
        pror_LineWidth->setEnabled(true);
        pror_LineWidth->setValue(obj->GetLineWidth());
        prop_Transparency->setEnabled(true);
        prop_Transparency->setValue(obj->GetTransparency());
    } else {
        prop_PointSize->setEnabled(false);
        prop_PointSize->setValue(0);
        pror_LineWidth->setEnabled(false);
        pror_LineWidth->setValue(0);
        prop_Transparency->setEnabled(false);
        prop_Transparency->setValue(0);
    }
}
void igQtModelDialogWidget::updateCurrentModelProperty(iGame::Model* model) {
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();
    scene->SetCurrentModel(model);

    //currentModel = model;
    auto obj = DynamicCast<iGame::DrawObject>(model->GetDataObject());
    if (obj) {
        prop_PointSize->setEnabled(true);
        prop_PointSize->setValue(obj->GetPointSize());
        pror_LineWidth->setEnabled(true);
        pror_LineWidth->setValue(obj->GetLineWidth());
        prop_Transparency->setEnabled(true);
        prop_Transparency->setValue(obj->GetTransparency());
    } else {
        prop_PointSize->setEnabled(false);
        prop_PointSize->setValue(0);
        pror_LineWidth->setEnabled(false);
        pror_LineWidth->setValue(0);
        prop_Transparency->setEnabled(false);
        prop_Transparency->setValue(0);
    }
}
int igQtModelDialogWidget::updateCloudPicture() {

    Q_EMIT CloudPictureChanged();
    return 1;
}
void igQtModelDialogWidget::deleteCurrentModel() {
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();
    // 获取当前选中的QTreeWidgetItem
    ModelTreeWidgetItem* currentItem = dynamic_cast<ModelTreeWidgetItem*>(modelTreeWidget->currentItem());

    // Fallback: if no UI selection, find item by scene's current model ID (for MCP/programmatic calls)
    if (currentItem == nullptr) {
        unsigned int currentModelId = scene->GetCurrentModelID();
        for (int i = 0; i < modelTreeWidget->topLevelItemCount(); ++i) {
            auto* item = dynamic_cast<ModelTreeWidgetItem*>(modelTreeWidget->topLevelItem(i));
            if (item && static_cast<unsigned int>(item->getModelId()) == currentModelId) {
                currentItem = item;
                break;
            }
        }
    }

    if (currentItem == nullptr) return;

    int id = currentItem->getModelId();
    
    // 在删除之前获取模型名称，避免在 RemoveModel 后持有引用
    std::string modelName;
    {
        auto model = scene->GetModelById(id);
        if (model && model->GetDataObject()) {
            modelName = model->GetDataObject()->GetName();
        }
        // model 智能指针在这里离开作用域并释放
    }

    scene->RemoveModel(id);
    scene->Update();

    if (!modelName.empty()) {
        // Need to emit signal to ScalarViewWidget to clear states
        Q_EMIT ModelDeleted(modelName);
    }

    const int removedIndex = modelTreeWidget->indexOfTopLevelItem(currentItem);
    if (removedIndex != -1) { delete modelTreeWidget->takeTopLevelItem(removedIndex); }

    // QTreeWidget 在移除项时会自己改选中项，但那条路径不会发出 ChangeCurrentModel，
    // 所以这里必须显式选中"留下的那个模型"，并主动刷新属性栏/模型信息/顶栏模型名，
    // 否则它们会继续显示被删除模型的信息。
    ModelTreeWidgetItem* nextItem = nullptr;
    const int count = modelTreeWidget->topLevelItemCount();
    if (count > 0) {
        const int nextIndex = qBound(0, removedIndex < 0 ? 0 : removedIndex, count - 1);
        nextItem = dynamic_cast<ModelTreeWidgetItem*>(modelTreeWidget->topLevelItem(nextIndex));
        if (!nextItem) { nextItem = dynamic_cast<ModelTreeWidgetItem*>(modelTreeWidget->topLevelItem(0)); }
    }

    if (nextItem) {
        scene->SetCurrentModel(nextItem->getModelId());
        modelTreeWidget->setCurrentItem(nextItem);
        nextItem->setSelected(true);
    }

    updateCurrentModelProperty();
    updateCurrentModelInfo();
}

void igQtModelDialogWidget::onPropertyChanged(QtProperty* property, const QVariant& value) {
    auto currentModel = GetCurrentModel();
    if (property == prop_PointSize) {
        //std::cout << value.toInt() << std::endl;
        if (currentModel) {
            auto obj = DynamicCast<iGame::DrawObject>(currentModel->GetDataObject());
            if (obj && obj->GetPointSize() != value.toInt() && value.toInt() > 0) {
                obj->SetPointSize(value.toInt());
                Update();
            }
        }
    } else if (property == pror_LineWidth) {
        //std::cout << value.toDouble() << std::endl;
        if (currentModel) {
            auto obj = DynamicCast<iGame::DrawObject>(currentModel->GetDataObject());
            if (obj && obj->GetLineWidth() != value.toInt() && value.toInt() > 0) {
                obj->SetLineWidth(value.toInt());
                Update();
            }
        }
    } else if (property == prop_Transparency) {
        //std::cout << value.toDouble() << std::endl;
        if (currentModel) {
            auto obj = DynamicCast<iGame::DrawObject>(currentModel->GetDataObject());
            if (obj && obj->GetTransparency() != value.toDouble() && value.toDouble() >= 0 && value.toDouble() <= 1.0) {
                obj->SetTransparency(value.toFloat());
                Update();
            }
        }
    }
}

iGame::Model* igQtModelDialogWidget::GetCurrentModel() {
    auto scene = iGame::SceneManager::Instance()->GetCurrentScene();
    return scene->GetCurrentModel();
}


void igQtModelDialogWidget::positionTreeDockToRendererCorner(QWidget* rendererWidget) {
    if (!rendererWidget || !m_treeDock) return;

    // 如果不允许悬浮，就不要强制 setFloating(true)，否则会变成系统浮动窗
    if (!(m_treeDock->features() & QDockWidget::DockWidgetFloatable)) {
        return;
    }

    // 确保dock widget是悬浮状态，然后设置无边框
    m_treeDock->setFloating(true);
    // 使用无边框 floating（可通过自定义 title bar 拖拽移动）
    m_treeDock->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    // 關閉透明背景，讓樣式表的背景與邊框生效
    m_treeDock->setAttribute(Qt::WA_TranslucentBackground, false);

    // 先设置窗口大小（紧凑：与树列宽 110+140 匹配，约 300 即可）
    // §49：收起态保持小方块尺寸，别在这儿被撑回 300×250
    int dockWidth = m_treeCollapsed ? kTreeCollapsedSize : 300;  // 悬浮窗口宽度
    int dockHeight = m_treeCollapsed ? kTreeCollapsedSize : 250; // 悬浮窗口高度
    m_treeDock->resize(dockWidth, dockHeight);
    m_treeDock->show(); // window flags 变更后需要 show()
    
    // 等待窗口完全显示后再计算位置
    QApplication::processEvents();
    
    // 获取dock窗口的实际大小（窗口显示后可能略有调整）
    QSize actualDockSize = m_treeDock->size();

    // 获取OpenGL渲染窗口在屏幕上的几何信息
    // rendererWidget本身就是centralWidget，直接使用它作为渲染窗口
    QWidget* actualRendererWidget = rendererWidget;
    
    // 获取渲染窗口的几何信息
    // 直接获取窗口的四个角点的全局坐标
    QPoint rendererTopLeft = actualRendererWidget->mapToGlobal(QPoint(0, 0));
    QPoint rendererBottomRight = actualRendererWidget->mapToGlobal(QPoint(actualRendererWidget->width(), actualRendererWidget->height()));

    // 计算悬浮窗口的位置：layerdialog的右下角对应渲染窗口的右下角，留出边距
    int margin = 15; // 边距
    
    // layerdialog的左上角位置（全局坐标）= 渲染窗口右下角 - layerdialog实际大小 - 边距
    QPoint dockTopLeft(
            rendererBottomRight.x() - actualDockSize.width() - margin,
            rendererBottomRight.y() - actualDockSize.height() - margin
    );

    // 获取渲染窗口所在的屏幕，确保窗口完全在屏幕内
    QScreen* screen = nullptr;
    if (QWidget* mainWindow = rendererWidget->window()) {
        screen = mainWindow->screen();
    }
    if (!screen) {
        // 如果无法获取，使用计算位置所在的屏幕
        screen = QApplication::screenAt(dockTopLeft);
    }
    if (!screen) {
        // 如果还是无法获取，使用主屏幕
        screen = QApplication::primaryScreen();
    }
    
    if (screen) {
        QRect screenGeometry = screen->availableGeometry();
        
        // 计算layerdialog的右下角位置
        QPoint dockBottomRight = dockTopLeft + QPoint(actualDockSize.width(), actualDockSize.height());
        
        // 检查并调整位置，确保窗口完全在屏幕内
        // 优先保持layerdialog的右下角对应渲染窗口的右下角
        
        // 如果右下角超出屏幕右边界
        if (dockBottomRight.x() > screenGeometry.right()) {
            // 调整到屏幕右边界内，但确保仍然在渲染窗口右侧
            int newX = screenGeometry.right() - actualDockSize.width() - margin;
            // 计算调整后layerdialog的右下角X坐标
            int newDockRight = newX + actualDockSize.width();
            
            // 只有当调整后的layerdialog右下角仍然在渲染窗口右下角的右侧时才调整
            // 如果调整后会移到渲染窗口左侧，则保持原位置（即使部分超出屏幕）
            if (newDockRight >= rendererBottomRight.x() - margin) {
                // 调整后的位置仍然在渲染窗口右侧，使用新位置
                dockTopLeft.setX(newX);
            }
            // 否则保持原位置，即使部分超出屏幕也比移到左侧好
        }
        
        // 如果右下角超出屏幕下边界
        if (dockBottomRight.y() > screenGeometry.bottom()) {
            dockTopLeft.setY(screenGeometry.bottom() - actualDockSize.height() - margin);
        }
        
        // 确保左上角也在屏幕内（防止窗口完全超出屏幕）
        // 但如果渲染窗口在右侧，layerdialog绝对不应该被移到屏幕左侧
        if (dockTopLeft.x() < screenGeometry.left()) {
            // 检查渲染窗口的位置：如果渲染窗口在屏幕右侧，绝对不调整到左侧
            int rendererRight = rendererBottomRight.x();
            int rendererLeft = rendererTopLeft.x();
            
            // 如果渲染窗口的右边界在屏幕中心右侧，说明渲染窗口在右侧
            // 此时layerdialog不应该被移到屏幕左侧，保持原位置
            // 或者，如果layerdialog的右下角在渲染窗口右侧，也不应该移到左侧
            int dockRight = dockTopLeft.x() + actualDockSize.width();
            bool rendererOnRight = (rendererRight > screenGeometry.center().x() || rendererLeft > screenGeometry.center().x());
            bool dockOnRightOfRenderer = (dockRight >= rendererBottomRight.x() - margin);
            
            if (rendererOnRight || dockOnRightOfRenderer) {
                // 渲染窗口在右侧，或者layerdialog在渲染窗口右侧
                // 绝对不调整到屏幕左侧，保持原位置
                // 完全不进行调整，直接跳过
            } else {
                // 渲染窗口在屏幕左侧或中间，且layerdialog不在渲染窗口右侧
                // 可以调整到屏幕左边界（但这种情况不应该发生）
                dockTopLeft.setX(screenGeometry.left() + margin);
            }
        }
        if (dockTopLeft.y() < screenGeometry.top()) {
            dockTopLeft.setY(screenGeometry.top() + margin);
        }
    }

    // 最终检查：如果渲染窗口在屏幕右侧，确保layerdialog不会出现在屏幕左侧
    if (screen) {
        QRect screenGeometry = screen->availableGeometry();
        int rendererRight = rendererBottomRight.x();
        
        // 如果渲染窗口在屏幕右侧（右边界在屏幕中心右侧）
        if (rendererRight > screenGeometry.center().x()) {
            // 确保layerdialog不会出现在屏幕左侧
            if (dockTopLeft.x() < screenGeometry.left() + 100) {
                // layerdialog出现在屏幕左侧，这是错误的
                // 强制放在渲染窗口右下角
                dockTopLeft.setX(rendererBottomRight.x() - actualDockSize.width() - margin);
                dockTopLeft.setY(rendererBottomRight.y() - actualDockSize.height() - margin);
            }
        }
    }
    

    // 设置悬浮窗口的位置（使用全局坐标）
    m_treeDock->move(dockTopLeft);

    // 确保窗口可见并激活
    m_treeDock->show();
    m_treeDock->raise();
    m_treeDock->activateWindow();
}
