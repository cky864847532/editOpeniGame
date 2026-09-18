/**
 * @class   igQtCharts
 * @brief   igQtCharts's brief
 */
#pragma once

#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QValueAxis>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include "iGameArrayObject.h"
QT_CHARTS_USE_NAMESPACE

class igQtCharts : public QDialog {
    Q_OBJECT

public:
    igQtCharts(QWidget* parent = nullptr);
    void drawBarChart(iGame::ArrayObject::Pointer m_data);
    void drawLineChart(iGame::ArrayObject::Pointer m_data);
    QChartView* getChartView() const;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // §66：切主题时刷新本窗口 QSS（标题栏/关闭按钮）与 QChart 配色（背景/坐标轴/图例/网格）
    void changeEvent(QEvent* e) override;

private:
    void updateRoundedMask();
    // §66：按当前主题的角色色重设 QChart / QChartView 配色（原来是一整套写死的深色）
    void applyTheme();

    QChart* chart;
    QChartView* chartView;
    QWidget* m_titleBar{nullptr};
    QLabel* m_titleLabel{nullptr};
    QPushButton* m_closeButton{nullptr};
    bool m_dragging{false};
    QPoint m_dragOffset;
    int m_cornerRadius{10};
};


