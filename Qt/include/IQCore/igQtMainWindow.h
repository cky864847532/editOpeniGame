/**
 * @class   igQtMainWindow
 * @brief   igQtMainWindow's brief
 */

#ifndef IGAMEVIS_IGQTMAINWINDOW_H
#define IGAMEVIS_IGQTMAINWINDOW_H

#define QT_NO_OPENGL
#include <ui_iGameQtMainWindow.h>
#if __linux__
#include <QTabWidget>
#include <QtGui>
#else
#include <QtGUI/QtGui>
#include <QtWidgets/Qtabwidget.h>
#endif
#include <IQCore/igQtExportModule.h>
#include <QtWidgets/QMainWindow>
#include <QResizeEvent>
#include <QShowEvent>
#include <QRect>
#include <QTimer>
#include <array>
#undef QT_NO_OPENGL

class QMenu;
class QHBoxLayout;

class igQtModelDrawWidget;
class igQtFileLoader;
class igQtColorManagerWidget;
class igQtFilterDialogDockWidget;
class igQtSliceWidget;
class igQtProgressBarWidget;
class igQtModelDialogWidget;
class igQtModelClipWidget;
class igQtDeformationWidget;
class igQtAiChatWidget;
class igQtCommandManager;
class QFontMetrics;
class igQtChromeFramelessDialog;
class igQtPartFocusWidget;
class igQtAttributeSelectWidget;

class IG_QT_MODULE_EXPORT igQtMainWindow : public QMainWindow {
    Q_OBJECT
public:
    /// 左侧「工具」Tab 面板项（扩展：追加枚举值 + cpp 中 switch + 菜单/工具栏连接）
    enum class LeftToolPanelId : int {
        Scalar = 0,
        Vector,
        Tensor,
        Flow,
        ContourExtract,
        Slice,
        Deformation,
        Selection,
        VariableDensity,
        DataChange,
        Count
    };

    igQtMainWindow(QWidget* parent = Q_NULLPTR);
    ~igQtMainWindow() override;

public:
    void initAllUnDefinedComponents();
    void initToolbarComponent();
    void initAllComponents();
    void initAllDockWidgetConnectWithAction();
    void initAllMySignalConnections();
    void initAllFilters();
    void initAllSources();
    void initAllInteractor();
    void initArgs(const QStringList& args);
    void updateVortexMetricsLabelPos();

    /** 将已登记的面板迁入左侧 QTabWidget 并显示（已存在则仅切换 Tab） */
    void openLeftToolPanel(LeftToolPanelId id);
    /** 从左侧 Tab 移除并还原到原 QDockWidget（供可勾选动作关闭等） */
    void closeLeftToolPanel(LeftToolPanelId id);

public:
    igQtModelDrawWidget* rendererWidget = nullptr;
    igQtFileLoader* fileLoader = nullptr;
    igQtModelDialogWidget* modelTreeWidget = nullptr;

    igQtColorManagerWidget* ColorManagerWidget = nullptr;
    igQtFilterDialogDockWidget* filterDialogDockWidget = nullptr;
    QDockWidget* SliceDockWidget = nullptr;
    QDockWidget* ContourDockWidget = nullptr;
    igQtModelClipWidget* SliceWidget = nullptr;
    QDockWidget* DeformationDockWidget = nullptr;
    igQtDeformationWidget* DeformationWidget = nullptr;

    igQtProgressBarWidget* progressBarWidget = nullptr;
    QComboBox* viewStyleCombox = nullptr;
    QComboBox* attributeViewIndexCombox = nullptr;
    QComboBox* attributeViewDimCombox = nullptr;
    
    // AI Chat DockWidget
    QDockWidget* aiChatDockWidget = nullptr;
    igQtAiChatWidget* aiChatWidget = nullptr;

    // Command Manager for MCP Server (端口 12345)
    igQtCommandManager* commandManager = nullptr;

    // 零件聚焦弹窗
    igQtChromeFramelessDialog* partFocusDialog{nullptr};
    igQtPartFocusWidget* partFocusWidget{nullptr};

    // 报告生成弹窗
    igQtChromeFramelessDialog* reportGenerateDialog{nullptr};
    igQtAttributeSelectWidget* reportGenerateWidget{nullptr};

private slots:
    void updateRecentFilePaths();
    void updateColorBarShow();

    //void ChangeViewStyle();
    //void ChangeScalarView();
    //void ChangeScalarViewDim();
    //void updateViewStyleAndCloudPicture();
    //void updateCurrentDataObject();
    //void updateCurrentSceneWidget();

    void UpdateRenderingWidget();
    //void changePointSelectionInteractor();
    //void changePointsSelectionInteractor();
    //void changeFaceSelectionInteractor();
    //void changeFacesSelectionInteractor();
    void UpdateIcons();
    QString LoadExternalFonts();


private:
    Ui::MainWindow* ui;
    QLabel* vortexMetricsLabel = nullptr;
    // 自定义标题栏相关
    QWidget* m_titleBar = nullptr;
    QLabel* m_titleLabel = nullptr;
    // logo + iGameVis 文字的圆角框
    QWidget* m_brandBox = nullptr;
    // 顶栏菜单按钮所在的横向布局（原生 QMenuBar 内嵌到这里）
    QHBoxLayout* m_topMenuLayout = nullptr;
    // 顶栏品牌框 + 内嵌 QMenuBar 的配色（切换主题时刷新）
    void applyTopMenuButtonStyle();
    QPushButton* m_btnMinimize = nullptr;
    QPushButton* m_btnMaximize = nullptr;
    QPushButton* m_btnClose = nullptr;
    QPushButton* m_styleToggleButton = nullptr;
    QLabel* m_logoIconLabel = nullptr;
    QLabel* m_projectChip = nullptr;
    QFrame* m_titleAccentLine = nullptr;
    QFrame* m_rightDivider = nullptr;
    bool m_titleBarDragging = false;
    QPoint m_dragOffset;
    bool m_isMinimizing = false;
    bool m_isRestoringFromMaximized = false;
    QRect m_geometryBeforeMinimize;
    QRect m_normalGeometry;

    // 左侧工具 Tab（按需添加；下方 Properties 常驻）
    QDockWidget* m_leftFieldDock = nullptr;
    QTabWidget* m_leftFieldTabs = nullptr;
    std::array<int, static_cast<size_t>(LeftToolPanelId::Count)> m_leftToolTabByPanel{{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};

    void relocateContentToLeftTab(QDockWidget* shell, QWidget* inner, const QString& title, LeftToolPanelId id,
                                  bool centerFlowField);
    QWidget* wrapContentInScrollArea(QWidget* content, QWidget* parent, bool centerFlowField);
    QDockWidget* shellDockForLeftPanel(LeftToolPanelId id) const;
    void onLeftToolTabCloseRequested(int index);
    /** 工具面板与 Properties 垂直比例（需在工具 Dock 已 show 后调用） */
    void applyLeftToolStackVerticalSplit();

    /** 与菜单「算法处理 / 特征提取」等一致：无边框 QMessageBox + 暗色圆角边框。 */
    void showDarkFramelessMessage(const QString& title, const QString& text, bool useInformationIcon = false);

    /**
     * 「数据转换」：**就地**转换当前帧挂载的数据（复合模型=当前挂载的子块，即当前帧；
     * 普通模型=自身），转换完**模型树里仍然只有这一个模型**：不新增行，只把该模型改名成
     * 转换后的名字（`原名[_fN]_PointData/_CellData`），并就地刷新属性行图标与画面。
     * @param toPointData true=单元数据转点数据；false=点数据转单元数据
     * @param reason 失败/未转换时的原因（供提示框显示）
     * @param createdNames 转换后使用的模型名
     * @return 1 = 已转换并改名；0 = 没有转换（见 reason）
     */
    int createConvertedFrameModel(bool toPointData, QString& reason, QStringList& createdNames);

    void rebuildActionsAsTwoRowWidget(QToolBar* toolbar, const QList<QAction*>& targetActions, int columns,
                                      QAction* insertBefore = nullptr);
    void addToolbarTitle(QToolBar* toolbar, const QString& title, int iconSizePx);
    void relayoutToolbarWrappers();
    void initCustomTitleBar();

    // ---- 界面风格切换（原始深色 / 现代深色 / 浅色） ----
    void applyStyleMode(int mode);
    QString styleSheetForMode(int mode) const;
    QString loadModernStyleSheet() const;
    QString loadLightStyleSheet() const;
    QString loadProStyleSheet() const;
    QString loadNebulaStyleSheet() const;
    QString loadWorkspaceStyleSheet() const;
    QString loadGraphiteModernStyleSheet() const;
    QString loadMatteGraphiteStyleSheet() const;
    QString loadGitCodeDarkStyleSheet() const;
    QString loadFloatingDarkStyleSheet() const;
    QString styleToggleButtonQss() const;
    QString styleModeDisplayName(int mode) const;
    void createStyleMenu();
    void updateTitleBarIcons();
    QString toolbarButtonQss(int fontPx) const;
    QString twoRowGridButtonQss() const;
    QString toolbarTitleLabelQss() const;
    QString toolbarCaptionLabelQss(int fontPx) const;
    // 工具栏「图标 + 文字」整块按钮的悬停反馈（颜色与 twoRowGridButtonQss 对齐）
    QString toolbarItemQss() const;
    QString toolbarSeamColor() const;
    QString toolbarAccentColor() const;

    // ---- 工作台布局（第 7 种风格：紧凑命令栏 + 右侧工具组 + 模型树保持悬浮）----
    void applyWorkspaceLayout(bool enabled);
    // ---- 悬浮卡片（第 12 种风格：视口/属性圆角卡片 + 细缝 + XZ 网格质感）----
    void applyFloatingCards(bool enabled);
    // §46b：卡片底色/描边/视口外底色按"颜色族"重设（12 与 13/14/15 互切时必须调用）
    void applyFloatingCardPalette();
    // ---- 视图栏（右悬浮竖向快捷栏）----
    void applyViewRail(bool enabled);
    void updateViewRailPosition();
    QDockWidget* m_viewDock = nullptr;

    // ---- 工具栏单排适配（宽度拟合 + 文字自动换行）----
    /** 按指定 iconSize 重建 3×2 轴网格与 4 组「按钮行 + 标题」容器 */
    void rebuildToolbarRow(int iconSize);
    /** 删除旧的 wrapper_* 工具栏（连带其容器） */
    void removeToolbarWrappers();
    /** 测量当前 4 组 wrapper 的实际总宽度（含组间距） */
    int measureToolbarRowWidth() const;
    /** 文字超宽时断成最多两行，第二行仍超宽则省略号收尾 */
    QString wrapToolbarButtonText(const QString& text, int maxWidth, const QFontMetrics& fm) const;

    int m_currentToolbarIconSize = 40;
    bool m_toolbarRebuilding = false;

    // 界面风格（§44 精简后菜单里只剩这 5 种）：2 = 浅色；9 = 石墨·现代；10 = 石墨·哑光；
    // 11 = GitCode 暗色；12 = 悬浮卡片。
    // 已从菜单删除（QSS 资源仍保留、不再可选）：0 原始深色 / 1 现代深色 v2 / 3 石墨专业深色 /
    // 4 深空青蓝 / 5 Fluent / 6 工作台 / 7 石墨·视图栏 / 8 深空·视图栏；
    // 这些模式号若要落到运行时，都会被 normalizeStyleMode() 回退到 9。
    QString m_originalStyleSheet;
    int m_styleMode = 9;
    // 悬浮卡片模式下的中央视口卡片容器
    QWidget* m_centralCardContainer = nullptr;
    // 悬浮卡片模式下被圆角遮罩的属性内容控件
    QWidget* m_floatingCardWidget = nullptr;
    // 悬浮卡片模式下被圆角遮罩的属性 dock 本身
    QDockWidget* m_floatingCardDock = nullptr;
    // 悬浮卡片模式下被圆角遮罩的模型树 dock
    QDockWidget* m_floatingTreeDock = nullptr;
    // 悬浮卡片模式下模型树内容被包进的外层容器
    QWidget* m_floatingTreeWrapper = nullptr;
    // 悬浮前模型树原始内容控件，退出时恢复
    QWidget* m_floatingTreeOriginalWidget = nullptr;
    // 悬浮前模型树原始标题栏控件，退出时恢复
    QWidget* m_floatingTreeOriginalTitleBar = nullptr;
    // 进入悬浮前属性 dock 的原始内容控件，退出时恢复
    QWidget* m_floatingCardOriginalWidget = nullptr;
    // 进入悬浮卡片前属性 dock 的原始最小宽度，退出时恢复
    int m_propertiesOriginalMinWidth = 0;
    QMenu* m_styleMenu = nullptr;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    int streamTreeIndex = -1;

private:
    // 响应式 toolbar 布局：把耗时的重排合并（避免每 1px 拖动都触发全量刷新）
    QTimer* m_ResizeDebounceTimer{nullptr};
    bool m_ResponsiveHooked{false};
    void hookResponsiveEvents();
    // 依当前窗口宽度/屏幕 DPI 挑选最大能一行装下的 iconSize，然后应用到全部工具栏
    void applyResponsiveToolbarLayout();
private:
    void minimizeWithAnimation();
    void toggleMaximizeRestore();
    void updateMaximizeButtonIcon();
};


#endif //IGAMEVIS_IGQTMAINWINDOW_H
