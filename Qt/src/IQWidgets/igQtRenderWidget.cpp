//
// Created by m_ky on 2024/4/18.
//

/**
 * @class   iGameQtGLFWWindow
 * @brief   iGameQtGLFWWindow's brief
 */
#include "iGameInteractor.h"
#include <QRegularExpression>   // §52：面板颜色令牌重映射
#include "iGameSceneManager.h"

#include <IQWidgets/igQtRenderWidget.h>
#include <QMouseEvent>
#include <QOpenGLFunctions> //
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <iGamePointSet.h>
#include <iGameUnstructuredMesh.h>
#include <iGameVolumeMesh.h>
#include <qdebug.h>

int igQtRenderWidget::s_styleMode = 0;

void igQtRenderWidget::setGlobalStyleMode(int mode) { s_styleMode = qBound(0, mode, 15); }
int igQtRenderWidget::globalStyleMode() { return s_styleMode; }
// §45：浅色族（2 浅色 / 13 悬浮·浅色）需要"亮底"处理
bool igQtRenderWidget::globalLightBackground() { return s_styleMode == 2 || s_styleMode == 13; }

// ============================================================================
// §52（4b）：工具面板统一样式方案
//   面板自带的 QSS 都是"深色一套"（#252526/#1E1E1E/#3C3C3C/#CCCCCC/#007ACC…），
//   这里把这些令牌按当前主题重映射成"角色色"，结构/圆角/间距等规则原样保留。
//   角色色取值全部抄自各主题已有 QSS —— 不新造配色。
// ============================================================================
namespace {

// 顺序必须与 igQtRenderWidget::UiRole 一致
enum RoleIdx { R_PanelBg, R_PanelBg2, R_CardBg, R_Border, R_BorderStrong,
               R_Text, R_TextDim, R_TextStrong, R_Accent, R_HoverBg, R_SelectionBg, R_Count };

// 8 套风格的角色色：12 / 13 / 14 / 15 / 2 / 9 / 10 / 11
const char* kRoleColors[8][R_Count] = {
        /* 12 悬浮卡片      */ { "#252526", "#1E1E1E", "#2A2A2A", "#2D2D30", "#3A3A3D", "#CCCCCC", "#858585", "#FFFFFF", "#37373D", "#2F2F33", "#37373D" },
        /* 13 悬浮·浅色     */ { "#F1F3F7", "#FFFFFF", "#E9EDF2", "#CBD2DC", "#D6DBE4", "#1F2A3A", "#5A6577", "#1F2A3A", "#2563EB", "#DFE3EA", "#332563EB" },
        /* 14 悬浮·石墨现代 */ { "#22262C", "#1E2126", "#2A2F36", "#31363D", "#3E4550", "#E9EDF2", "#A9B0B9", "#FFFFFF", "#6C8EAE", "#2F343B", "#386C8EAE" },
        /* 15 悬浮·石墨哑光 */ { "#20242A", "#1C1F25", "#262A31", "#2C3038", "#3A414C", "#E2E4E8", "#8A8F98", "#FFFFFF", "#3A414C", "#2B2F36", "#24FFFFFF" },
        /* 2  浅色         */ { "#F1F3F7", "#FFFFFF", "#E9EDF2", "#CBD2DC", "#D6DBE4", "#1F2A3A", "#5A6577", "#1F2A3A", "#2563EB", "#DFE3EA", "#332563EB" },
        /* 9  石墨·现代     */ { "#22262C", "#1E2126", "#2A2F36", "#31363D", "#3E4550", "#E9EDF2", "#A9B0B9", "#FFFFFF", "#6C8EAE", "#2F343B", "#386C8EAE" },
        /* 10 石墨·哑光     */ { "#20242A", "#1C1F25", "#262A31", "#2C3038", "#3A414C", "#E2E4E8", "#8A8F98", "#FFFFFF", "#3A414C", "#2B2F36", "#24FFFFFF" },
        /* 11 GitCode 暗色  */ { "#1E1E1E", "#121212", "#252526", "#2D2D30", "#3A3A3D", "#CCCCCC", "#858585", "#FFFFFF", "#4C9BFF", "#262626", "#37373D" },
};

int roleRowForMode(int mode) {
    switch (mode) {
        case 13: return 1;
        case 14: return 2;
        case 15: return 3;
        case 2:  return 4;
        case 9:  return 5;
        case 10: return 6;
        case 11: return 7;
        default: return 0;   // 12 悬浮卡片（默认）
    }
}

// 面板 QSS 里的"深色令牌" → 角色（这些面板用的就是同一套深色值，一张表覆盖）
struct TokenRole { const char* token; RoleIdx role; };
const TokenRole kTokenRoles[] = {
        { "#252526", R_PanelBg },   { "#1E1E1E", R_PanelBg2 },  { "#2A2A2A", R_CardBg },
        { "#2B2B2B", R_CardBg },    { "#3C3C3C", R_Border },    { "#2D2D30", R_Border },
        { "#4A4A4A", R_BorderStrong }, { "#3A3A3A", R_BorderStrong },
        { "#3A3A3D", R_BorderStrong }, { "#45454A", R_BorderStrong }, { "#BEBEBE", R_BorderStrong },
        { "#CCCCCC", R_Text },      { "#C8C8C8", R_Text },      { "#D4D4D4", R_Text }, { "#D8D8D8", R_Text },
        { "#E0E0E0", R_TextStrong },{ "#EAEAEA", R_TextStrong },{ "#FFFFFF", R_TextStrong },
        { "#858585", R_TextDim },   { "#808080", R_TextDim },   { "#9E9E9E", R_TextDim },
        { "#007ACC", R_Accent },    { "#0E639C", R_Accent },    { "#1E90FF", R_Accent },
        { "#094771", R_SelectionBg },
        // §52c：按各面板实测的"未覆盖率"补齐的结构性令牌
        // （语义色 —— 错误红 / 代码高亮绿蓝等 —— **故意不映射**，保留原样）
        { "#222222", R_PanelBg2 },   { "#1F1F1F", R_PanelBg2 },
        { "#D0D0D0", R_Text },       { "#A8A8A8", R_Text },
        { "#F2F2F2", R_TextStrong }, { "#ECECEC", R_TextStrong },
        { "#B0B0B0", R_TextDim },    { "#707070", R_TextDim },   { "#5A5A5A", R_TextDim },
        { "#464646", R_BorderStrong }, { "#747C84", R_Border },
        { "#383838", R_CardBg },     { "#5A6066", R_CardBg },
        { "#666D74", R_HoverBg },    { "#4A5056", R_BorderStrong },
        { "#3A4A6A", R_Accent },     // 相关分析面板里的蓝灰强调
        { "#2F2F2F", R_HoverBg },    // §63：报告生成面板按钮/复选框的悬停底
};

const RoleIdx* roleForToken(const QString& tokenUpper) {
    for (const TokenRole& tr : kTokenRoles) {
        if (tokenUpper == QLatin1String(tr.token)) return &tr.role;
    }
    return nullptr;
}

} // namespace

QString igQtRenderWidget::uiRoleCss(UiRole role) {
    const int row = roleRowForMode(globalStyleMode());
    return QString::fromLatin1(kRoleColors[row][static_cast<int>(role)]);
}

QColor igQtRenderWidget::uiRole(UiRole role) { return QColor(uiRoleCss(role)); }

QString igQtRenderWidget::themeRemapQss(const QString& baseQss) {
    if (baseQss.isEmpty()) return baseQss;
    const bool light = globalLightBackground();

    // ① 逐个 #RRGGBB 令牌查表替换（大小写不敏感；表里没有的原样保留）
    static const QRegularExpression hexRe(QStringLiteral("#([0-9A-Fa-f]{6})"));
    QString out;
    out.reserve(baseQss.size() + 128);
    int last = 0;
    QRegularExpressionMatchIterator it = hexRe.globalMatch(baseQss);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString tokenUpper = QStringLiteral("#") + m.captured(1).toUpper();
        const RoleIdx* role = roleForToken(tokenUpper);
        out += baseQss.mid(last, m.capturedStart() - last);
        out += role ? uiRoleCss(static_cast<UiRole>(*role)) : m.captured(0);
        last = m.capturedEnd();
    }
    out += baseQss.mid(last);

    // ② 深色里的"浅色中性色"（纯白或浅灰，含低透明度叠加）在浅色主题下要换成黑色系，
    //    否则浅底上会出现"白雾 / 白字看不见"。
    //    §64：原来只认 rgba(255,255,255,a)，漏掉了无边框工具窗标题用的
    //    rgba(220,222,228,0.88) / rgba(230,230,235,0.92) 这类浅灰文字 ——
    //    浅色主题下标题会看不见。这里统一按"R≈G≈B 且足够亮"判定。
    if (light) {
        static const QRegularExpression neutralRe(QStringLiteral(
                "rgba\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*([0-9.]+)\\s*\\)"));
        QString rebuilt;
        int last = 0;
        QRegularExpressionMatchIterator it = neutralRe.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int r = m.captured(1).toInt();
            const int g = m.captured(2).toInt();
            const int b = m.captured(3).toInt();
            const int mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
            const int mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
            rebuilt += out.mid(last, m.capturedStart() - last);
            if (mn >= 180 && (mx - mn) <= 32) {
                rebuilt += QStringLiteral("rgba(0,0,0,") + m.captured(4) + QStringLiteral(")");
            } else {
                rebuilt += m.captured(0);   // 语义色（红/绿/蓝等）原样保留
            }
            last = m.capturedEnd();
        }
        rebuilt += out.mid(last);
        out = rebuilt;
    }
    return out;
}

// §45：把自带深色样式的面板 QSS 适配到浅色族。
// 只做"主色搬运"（底色/文字/边框/悬停/禁用），强调色（选中蓝 #094771/#4FC3F7、聚焦 #5A7FA8）保持不变，
// 与 §45 生成主题 QSS 时的做法一致。
QString igQtRenderWidget::adaptQssToLightPalette(const QString& qss) {
    if (!globalLightBackground() || qss.isEmpty()) return qss;
    QString out = qss;
    struct Pair { const char* from; const char* to; };
    static const Pair kPairs[] = {
            // 底色 / 容器
            { "#222222", "#F1F3F7" },   // 面板底、禁用按钮底
            { "#1E1E1E", "#FFFFFF" },   // 滚动区 / 属性容器底
            { "#252526", "#D6DBE4" },   // 按下态
            { "#2A2A2A", "#E9EDF2" },   // 按钮 / 输入框 / 勾选框底
            { "#2F2F2F", "#DFE3EA" },   // 悬停底
            // 描边
            { "#3C3C3C", "#CBD2DC" },
            { "#3A3A3A", "#CBD2DC" },   // 输入框/按钮描边（也用于悬停底，浅色下同样合适）
            // 文字
            { "rgba(255,255,255,204)", "#1F2A3A" },
            { "rgba(255, 255, 255, 204)", "#1F2A3A" },
            { "rgba(255,255,255,120)", "rgba(0,0,0,120)" },
            { "rgba(255, 255, 255, 120)", "rgba(0, 0, 0, 120)" },
            { "#EAEAEA", "#1F2A3A" },
            { "#D8D8D8", "#33405B" },
            // 半透明描边/指示器
            { "rgba(255,255,255,80)", "rgba(0,0,0,90)" },
            { "rgba(255, 255, 255, 80)", "rgba(0, 0, 0, 90)" },
            { "rgba(255,255,255,32)", "rgba(0,0,0,35)" },
            { "rgba(255, 255, 255, 32)", "rgba(0, 0, 0, 35)" },
            { "rgba(255,255,255,26)", "rgba(0,0,0,30)" },
            { "rgba(255, 255, 255, 26)", "rgba(0, 0, 0, 30)" },
            { "rgba(255,255,255,20)", "rgba(0,0,0,25)" },
            { "rgba(255, 255, 255, 20)", "rgba(0, 0, 0, 25)" },
            { "rgba(255,255,255,10)", "rgba(0,0,0,12)" },
            { "rgba(255, 255, 255, 10)", "rgba(0, 0, 0, 12)" },
            // 无边框工具窗（igQtChromeFramelessDialog / 消息框）的标题栏与文字
            { "rgba(220, 222, 228, 0.88)", "rgba(31, 42, 58, 0.88)" },
            { "rgba(220, 222, 228, 0.9)", "rgba(31, 42, 58, 0.9)" },
            { "rgba(230, 230, 235, 0.92)", "rgba(31, 42, 58, 0.92)" },
            { "rgba(255, 255, 255, 0.07)", "rgba(0, 0, 0, 0.06)" },
            { "rgba(255, 255, 255, 0.1)", "rgba(0, 0, 0, 0.08)" },
            { "rgba(255, 255, 255, 0.16)", "rgba(0, 0, 0, 0.12)" },
            { "#C9C9C9", "#33405B" },
            { "#ECECEC", "#1F2A3A" },
            { "#5A6066", "#DFE3EA" },
            { "#747C84", "#CBD2DC" },
            { "#666D74", "#CBD2DC" },
            { "#4A5056", "#D6DBE4" },
    };
    for (const Pair& p : kPairs) { out.replace(QLatin1String(p.from), QLatin1String(p.to)); }
    return out;
}

void igQtRenderWidget::applyThemeBackground() {
    if (!m_Scene) return;
    // §45：悬浮卡片的换色变体沿用各自颜色族的视口背景
    //      13 悬浮·浅色 → 2；14 悬浮·石墨现代 → 9；15 悬浮·石墨哑光 → 10
    int mode = s_styleMode;
    if (mode == 13) mode = 2;
    else if (mode == 14) mode = 9;
    else if (mode == 15) mode = 10;
    // 注意：Scene 内部以 OpenGL scissor 绘制，垂直模式下第一个 RGB 为顶部、第二个为底部。
    switch (mode) {
        case 0: // 原始深色：中性灰，贴近原版灰色
            m_Scene->SetBackGroundGradient(43, 43, 43,   // 顶部 #2B2B2B
                                           27, 27, 27,   // 底部 #1B1B1B
                                           1);
            break;
        case 1: // 深灰蓝：带一点蓝，与界面呼应
            m_Scene->SetBackGroundGradient(28, 37, 51,   // 顶部 #1C2533
                                           10, 14, 22,   // 底部 #0A0E16
                                           1);
            break;
        case 2: // 浅色：浅灰蓝
            m_Scene->SetBackGroundGradient(224, 230, 236, // 顶部 #E0E6EC
                                           197, 205, 214, // 底部 #C5CDD6
                                           1);
            break;
        case 4: // 深空青蓝：深灰 + 蓝青
            m_Scene->SetBackGroundGradient(20, 22, 26,   // 顶部 #14161A
                                           26, 30, 34,   // 底部 #1A1E22
                                           1);
            break;
        case 6: // 工作台：深灰 + 蓝青
            m_Scene->SetBackGroundGradient(20, 22, 26,   // 顶部 #14161A
                                           26, 30, 34,   // 底部 #1A1E22
                                           1);
            break;
        case 7: // 石墨·视图栏：扁平灰黑
            m_Scene->SetBackGroundGradient(33, 34, 36,   // 顶部 #212224
                                           10, 10, 12,   // 底部 #0A0A0C
                                           1);
            break;
        case 8: // 深空·视图栏：深灰 + 蓝青
            m_Scene->SetBackGroundGradient(20, 22, 26,   // 顶部 #14161A
                                           26, 30, 34,   // 底部 #1A1E22
                                           1);
            break;
        case 9: // 石墨·现代：炭黑分层灰
            m_Scene->SetBackGroundGradient(31, 32, 34,   // 顶部 #1F2022
                                           12, 13, 15,   // 底部 #0C0D0F
                                           1);
            break;
        case 10: // 石墨·哑光：纯炭黑底层
            m_Scene->SetBackGroundGradient(20, 22, 26,   // 顶部 #14161A
                                           14, 15, 17,   // 底部 #0E0F11
                                           1);
            break;
        case 11: // GitCode 暗色：近纯黑分层
            m_Scene->SetBackGroundGradient(18, 18, 18,   // 顶部 #121212
                                           14, 14, 14,   // 底部 #0E0E0E
                                           1);
            break;
        case 12: // 悬浮卡片：炭黑 + 底部网格质感
            m_Scene->SetBackGroundGradient(18, 18, 18,   // 顶部 #121212
                                           14, 14, 14,   // 底部 #0E0E0E
                                           1);
            break;
        default: // 石墨深色(3)：扁平灰黑
            m_Scene->SetBackGroundGradient(33, 34, 36,   // 顶部 #212224
                                           10, 10, 12,   // 底部 #0A0A0C
                                           1);
            break;
    }
    m_Scene->Update();
}

igQtRenderWidget::igQtRenderWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, false);

    setMouseTracking(true);
    setMinimumHeight(185);
    setMinimumWidth(320);
}

igQtRenderWidget::~igQtRenderWidget() {
    makeCurrent();
    iGame::SceneManager::Pointer sceneManager = iGame::SceneManager::Instance();
    sceneManager->DeleteScene(m_Scene);
    m_Scene = nullptr;
    doneCurrent();
}

iGame::Scene* igQtRenderWidget::GetScene() { return m_Scene; }

void igQtRenderWidget::AddDataObject(iGame::SmartPointer<iGame::DataObject> obj) {
    //m_Scene->AddDataObject(obj);
    //Q_EMIT AddDataObjectToModelList(QString::fromStdString(obj->GetName()));
    //update();
}

void igQtRenderWidget::ChangeInteractor(iGame::SmartPointer<iGame::Interactor> it) {
    m_Interactor = it;
    m_Interactor->Initialize(m_Scene);
    m_Scene->SetInteractor(m_Interactor);
}

void igQtRenderWidget::ChangeInteractorStyle(IGenum style) {
    if (!m_Scene || !m_Scene->GetCurrentModel()) { return; }
    switch (style) {
        case iGame::Interactor::BasicStyle:
            m_Interactor->RequestBasicStyle();
            break;
        case iGame::Interactor::SinglePointSelectionStyle: {
            auto obj = m_Scene->GetCurrentModel()->GetDataObject();
            if (obj == nullptr) return;
            auto s = m_Scene->GetCurrentModel()->GetSelection();
            if (s == nullptr) return;
            if (obj->HasSubDataObject()) {
                s->SetModel(m_Scene->GetCurrentModel());
                m_Interactor->SetDataObject(obj);
                m_Interactor->SetPainter3D(m_Scene->GetCurrentModel()->GetPainter3D());
                m_Interactor->RequestPointSelectionStyle(s);

            } else {
                auto ps = DynamicCast<iGame::PointSet>(m_Scene->GetCurrentModel()->GetDataObject());
                if (ps == nullptr) {
                    m_Interactor->RequestBasicStyle();
                    return;
                }
                s->SetPoints(ps->GetPoints());
                s->SetModel(m_Scene->GetCurrentModel());
                m_Interactor->SetDataObject(ps);
                m_Interactor->SetPainter3D(m_Scene->GetCurrentModel()->GetPainter3D());
                m_Interactor->RequestPointSelectionStyle(s);
            }
        } break;
        case iGame::Interactor::SingleFaceSelectionStyle: {
            auto s = m_Scene->GetCurrentModel()->GetSelection();
            if (s == nullptr) return;
            auto model = m_Scene->GetCurrentModel();
            auto obj = model->GetDataObject();
            iGame::Points::Pointer points;
            iGame::CellArray::Pointer faces;

            if (DynamicCast<iGame::VolumeMesh>(obj)) {
                //auto mesh = DynamicCast<VolumeMesh>(obj)->GetDrawMesh();
                auto mesh = DynamicCast<iGame::VolumeMesh>(obj);
                points = mesh->GetPoints();
                faces = mesh->GetFaces();
            } else if (DynamicCast<iGame::UnstructuredMesh>(obj)) {
                //auto mesh = DynamicCast<UnstructuredMesh>(obj)->GetDrawMesh();
                auto mesh = DynamicCast<iGame::UnstructuredMesh>(obj);
                points = mesh->GetPoints();
                faces = mesh->GetCells();
            } else if (DynamicCast<iGame::SurfaceMesh>(obj)) {
                auto mesh = DynamicCast<iGame::SurfaceMesh>(obj);
                points = mesh->GetPoints();
                faces = mesh->GetFaces();
            }
            if (points == nullptr || faces == nullptr) {
                m_Interactor->RequestBasicStyle();
                return;
            }
            s->SetPoints(points);
            s->SetCells(faces);
            s->SetModel(model);
            m_Interactor->SetDataObject(obj);
            m_Interactor->SetPainter3D(m_Scene->GetCurrentModel()->GetPainter3D());
            m_Interactor->RequestFaceSelectionStyle(s);
        } break;
        case iGame::Interactor::MultiPointSelectionStyle:
            //m_Interactor->RequestPointSelectionStyle(m_Scene->GetCurrentModel()->GetSelection());
            break;
        case iGame::Interactor::MultiFaceSelectionStyle:
            //m_Interactor->RequestPointSelectionStyle(m_Scene->GetCurrentModel()->GetSelection());
            break;
        case iGame::Interactor::DragPointStyle: {
            auto s = m_Scene->GetCurrentModel()->GetSelection();
            auto ps = DynamicCast<iGame::PointSet>(m_Scene->GetCurrentModel()->GetDataObject());
            if (ps == nullptr) {
                m_Interactor->RequestBasicStyle();
                return;
            }
            s->SetPoints(ps->GetPoints());
            s->SetModel(m_Scene->GetCurrentModel());
            m_Interactor->SetDataObject(ps);
            m_Interactor->SetPainter3D(m_Scene->GetCurrentModel()->GetPainter3D());
            m_Interactor->RequestDragPointStyle(s);

        } break;
        //case iGame::Interactor::PickCenterStyle: {
        //    //点选中心
        //    // 标记
        //    this->setProperty("isPickingCenter", true);
        //    setCursor(Qt::CrossCursor);

        //    // 1. 获取当前模型的Selection对象
        //    auto s = m_Scene->GetCurrentModel()->GetSelection();
        //    auto model = m_Scene->GetCurrentModel();
        //    auto obj = model->GetDataObject();
        //    // 2. 提取模型的顶点数据（Points）
        //    iGame::Points::Pointer points;
        //    if (DynamicCast<iGame::VolumeMesh>(obj)) {
        //        points = DynamicCast<iGame::VolumeMesh>(obj)->GetPoints();
        //    } else if (DynamicCast<iGame::UnstructuredMesh>(obj)) {
        //        points = DynamicCast<iGame::UnstructuredMesh>(obj)->GetPoints();
        //    } else if (DynamicCast<iGame::SurfaceMesh>(obj)) {
        //        points = DynamicCast<iGame::SurfaceMesh>(obj)->GetPoints();
        //    }
        //    // 3. 数据有效性检查
        //    if (!points) {
        //        m_Interactor->RequestBasicStyle();
        //        return;
        //    }

        //    s->SetPoints(points);
        //    s->SetModel(model);
        //    m_Interactor->SetDataObject(obj);
        //    m_Interactor->SetPainter3D(model->GetPainter3D());
        //    m_Interactor->RequestPickCenterStyle(s); // 需要实现这个方法
        //
        //} break;
        case iGame::Interactor::DragCenterStyle: {
            this->setProperty("isDragingCenter", true);

            m_Interactor->RequestDragCenterStyle(nullptr);
        } break;
        default:
            break;
    }
}

iGame::Interactor* igQtRenderWidget::getInteractor() { return m_Interactor.get(); }

void igQtRenderWidget::initializeGL() {
    // 目前当窗口
    iGame::SceneManager::Pointer sceneManager = iGame::SceneManager::Instance();
    m_Scene = sceneManager->NewScene();
    m_Scene->Initialize();
    // 默认使用克制的近黑蓝灰垂直渐变（顶部 #131B28 → 底部 #070A0F，越往下越深），
    // 避免单色灰底过于单调；用户通过「更换背景」仍可覆盖为纯色（SetBackGround 会关闭渐变）。
    applyThemeBackground();  // 按当前深浅主题应用背景渐变
    m_Scene->SetUpdateFunctor(&igQtRenderWidget::update, this);
    m_Scene->SetMakeCurrentFunctor(&igQtRenderWidget::makeCurrent, this);
    m_Scene->SetDoneCurrentFunctor(&igQtRenderWidget::doneCurrent, this);

    m_Interactor = iGame::Interactor::New();
    m_Interactor->Initialize(m_Scene);
    m_Scene->SetInteractor(m_Interactor);
}

void igQtRenderWidget::resizeGL(int w, int h) {
    auto ratio = this->devicePixelRatio();
    m_Scene->Resize(width(), height(), ratio);
}

void igQtRenderWidget::setCornerCover(int radius, const QColor& coverColor) {
    // 注意：这里只记录参数，paintGL() 里【不】再用 QPainter 叠加绘制。
    // 原因：在 QOpenGLWidget 的 paintGL 里混用 QPainter 会破坏场景的 GL 状态
    // （深度函数/深度写/混合/裁剪盒/VAO 等，实测会导致视口图像渲染异常），
    // 所以视口圆角改回 1bit 遮罩（applyRoundedMask）。若要真正抗锯齿，
    // 必须在场景自身的 overlay pass 里用 GL 绘制四角，而不是 QPainter。
    m_cornerCoverRadius = qMax(0, radius);
    m_cornerCoverColor = coverColor;
    Q_UNUSED(m_cornerCoverRadius);
    Q_UNUSED(m_cornerCoverColor);
}

void igQtRenderWidget::paintGL() { m_Scene->Draw(); }


void igQtRenderWidget::mousePressEvent(QMouseEvent* event) {
    iGame::IEvent _event;
    switch (event->button()) {
        case Qt::NoButton:
            _event.button = iGame::MouseButton::NoButton;
            break;
        case Qt::LeftButton:
            _event.button = iGame::MouseButton::LeftButton;
            break;
        case Qt::RightButton:
            _event.button = iGame::MouseButton::RightButton;
            break;
        case Qt::MiddleButton:
            _event.button = iGame::MouseButton::MiddleButton;
            break;
        default:
            break;
    }
    _event.type = iGame::IEvent::MousePress;
    _event.pos.x = event->pos().x();
    _event.pos.y = event->pos().y();
    m_Interactor->FilterEvent(_event);
    update();
}

void igQtRenderWidget::mouseMoveEvent(QMouseEvent* event) {
    iGame::IEvent _event;
    _event.type = iGame::IEvent::MouseMove;
    _event.pos.x = event->pos().x();
    _event.pos.y = event->pos().y();
    m_Interactor->FilterEvent(_event);
    update();
}

void igQtRenderWidget::mouseReleaseEvent(QMouseEvent* event) {
    iGame::IEvent _event;
    _event.type = iGame::IEvent::MouseRelease;
    _event.pos.x = event->pos().x();
    _event.pos.y = event->pos().y();
    m_Interactor->FilterEvent(_event);
    update();
}

void igQtRenderWidget::wheelEvent(QWheelEvent* event) {
    iGame::IEvent _event;
    _event.type = iGame::IEvent::Wheel;
    _event.delta = event->delta();
    m_Interactor->FilterEvent(_event);
    update();
}

// 根据深度值获取世界坐标
igm::vec3 igQtRenderWidget::GetWorldPositionFromDepth(const QPoint& screenPos, float depth) {
    // 将屏幕坐标转换为标准化设备坐标
    float x = (2.0f * screenPos.x()) / width() - 1.0f;
    float y = 1.0f - (2.0f * screenPos.y()) / height();

    // 获取投影和视图矩阵
    igm::mat4 projection = m_Scene->GetCamera()->GetProjectionMatrix();
    igm::mat4 view = m_Scene->GetCamera()->GetViewMatrix();

    // 计算逆矩阵
    igm::mat4 invVP = (projection * view).invert();

    // 创建近平面和远平面点
    igm::vec4 nearPoint(x, y, -1.0f, 1.0f);
    igm::vec4 farPoint(x, y, 1.0f, 1.0f);

    // 转换为世界坐标
    igm::vec4 nearResult = invVP * nearPoint;
    igm::vec4 farResult = invVP * farPoint;
    nearResult /= nearResult.w;
    farResult /= farResult.w;

    // 计算射线方向
    igm::vec3 rayDir = igm::vec3(farResult) - igm::vec3(nearResult);
    rayDir = rayDir.normalize();

    // 计算交点（世界坐标）
    igm::vec3 worldPos = igm::vec3(m_Scene->GetCamera()->GetPosition()) + rayDir * depth;

    // 如果启用模型变换，则应用当前模型矩阵的逆变换
    //if (applyModelMatrix) {
    igm::mat4 invModelMatrix = m_Scene->GetModelMatrix().invert();
    igm::vec4 transformedPos = invModelMatrix * igm::vec4(worldPos, 1.0f);
    worldPos = igm::vec3(transformedPos);

    // 根据深度计算交点
    return worldPos;
}
