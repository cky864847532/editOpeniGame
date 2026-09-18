/**
 * @class   iGameQtGLFWWindow
 * @brief   Provides Qt window and context support  for external renderers
 */

#pragma once

#ifdef __APPLE__
#define __gl3_h_
#define __glext_h_
#define __glext3_h_
#endif

#include "iGameScene.h"
#include <IQCore/igQtExportModule.h>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>


class IG_QT_MODULE_EXPORT igQtRenderWidget : public QOpenGLWidget {
  Q_OBJECT
public:
  igQtRenderWidget(QWidget *parent = nullptr);
  ~igQtRenderWidget() override;
  static igQtRenderWidget* Instance(){
      static igQtRenderWidget instance;
      return &instance;
  }

    iGame::Scene *GetScene();

  void AddDataObject(iGame::SmartPointer<iGame::DataObject> obj);
  void ChangeInteractor(iGame::SmartPointer<iGame::Interactor> it);
  void ChangeInteractorStyle(IGenum style);
  void update() { QOpenGLWidget::update(); }
  // 主题相关：切换浅色/深色时，让 3D 视口背景一起适配
  static void setGlobalStyleMode(int mode);
  static int globalStyleMode();
  static bool globalLightBackground();
  // §45：把"自带整套深色样式"的面板 QSS（如属性选择 / 零件聚焦）适配到浅色族（浅色 / 悬浮·浅色）；
  //      非浅色族原样返回。只替换主色（底色/文字/边框/悬停），强调色（选中蓝等）保持不变。
  static QString adaptQssToLightPalette(const QString& qss);

  // ---- §52（4b）：工具面板统一样式方案 ----
  // 角色色：8 套风格 → 一组语义色（取值全部来自各主题已有 QSS，不新造配色）
  enum class UiRole {
      PanelBg,       // 面板底（深色 #252526）
      PanelBg2,      // 内容/滚动区底（深色 #1E1E1E）
      CardBg,        // 次级底 / 悬停底（深色 #2A2A2A）
      Border,        // 普通描边（深色 #3C3C3C）
      BorderStrong,  // 强描边（深色 #4A4A4A / #3A3A3A）
      Text,          // 正文（深色 #CCCCCC）
      TextDim,       // 次要文字（深色 #858585）
      TextStrong,    // 强调文字（深色 #FFFFFF）
      Accent,        // 强调/主色（深色 #007ACC）
      HoverBg,       // 悬停底
      SelectionBg    // 选中底
  };
  static QColor uiRole(UiRole role);
  static QString uiRoleCss(UiRole role);
  // 把面板自带的那套"深色 QSS"里的颜色令牌按当前主题重映射（结构/圆角/间距等规则原样保留）
  static QString themeRemapQss(const QString& baseQss);
  void applyThemeBackground();
  // 圆角窗口：用抗锯齿路径把四角涂成 coverColor（即视口外围的底色），得到平滑的弧形边。
  // radius <= 0 表示关闭。注意不要再用 setMask()：1bit 遮罩画不出抗锯齿，曲线必然呈阶梯状。
  void setCornerCover(int radius, const QColor& coverColor);

    iGame::Interactor* getInteractor();

  protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

  igm::vec3 GetWorldPositionFromDepth(const QPoint& screenPos, float depth);   

  iGame::SmartPointer<iGame::Scene> m_Scene;
  iGame::SmartPointer<iGame::Interactor> m_Interactor;
  static int s_styleMode;

  int m_cornerCoverRadius{0};
  QColor m_cornerCoverColor{0x1E, 0x1E, 0x1E};
};

// ---------------------------------------------------------------------------
// §52（4b）：工具面板主题化辅助（零侵入 —— 不给每个面板加成员）
//   用法（面板构造里，setupUi / 自带 QSS 载入之后）：
//       igQtPanelTheme::attach(this);
//   并且重写 changeEvent：
//       if (e->type() == QEvent::StyleChange) igQtPanelTheme::refresh(this);
//   原理：把面板**当前**样式表存成"底"（动态属性），切主题时把"底"里的深色令牌
//         按当前主题重映射再套回去；refresh 幂等（目标串相同就不 set），因此
//         setStyleSheet 触发的 StyleChange 不会递归。
// ---------------------------------------------------------------------------
struct igQtPanelTheme {
    static void attach(QWidget* panel) {
        if (!panel) return;
        // 已 attach 过就不再覆盖"底"（避免把重映射后的结果当底、二次重映射）
        if (!panel->property("igPanelBaseQss").toString().isEmpty()) {
            refresh(panel);
            return;
        }
        panel->setProperty("igPanelBaseQss", panel->styleSheet());
        refresh(panel);
    }
    static void refresh(QWidget* panel) {
        if (!panel) return;
        const QString base = panel->property("igPanelBaseQss").toString();
        if (base.isEmpty()) return;
        const QString mapped = igQtRenderWidget::themeRemapQss(base);
        if (panel->styleSheet() == mapped) return;   // 幂等：防 StyleChange 递归
        panel->setStyleSheet(mapped);
    }

    // §57：有些面板的深色 QSS 不在顶层，而挂在某个子控件上（如 Animation 面板的
    //       treeWidget_snap）。attachDeep/refreshDeep 连带处理所有"自带非空样式表"
    //       的后代；只 attach 到这些控件，不会把主题 QSS 套给普通子控件。
    static void attachDeep(QWidget* root) {
        const QList<QWidget*> all = styledWidgets(root);
        for (QWidget* w : all) { attach(w); }
    }
    static void refreshDeep(QWidget* root) {
        const QList<QWidget*> all = styledWidgets(root);
        for (QWidget* w : all) { refresh(w); }
    }

private:
    // 顶层 + 所有"当前自带样式表 或 已存过底"的后代
    static QList<QWidget*> styledWidgets(QWidget* root) {
        QList<QWidget*> out;
        if (!root) return out;
        out << root;
        const QList<QWidget*> kids = root->findChildren<QWidget*>();
        for (QWidget* w : kids) {
            if (!w->styleSheet().isEmpty() || !w->property("igPanelBaseQss").toString().isEmpty()) {
                out << w;
            }
        }
        return out;
    }
};
