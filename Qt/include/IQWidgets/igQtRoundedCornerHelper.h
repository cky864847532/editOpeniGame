/**
 * @file     igQtRoundedCornerHelper.h
 * @brief    抗锯齿圆角工具（子控件圆角统一方案）
 *
 * 背景：Qt 里“把控件裁成圆角”只有两条路，都是 1bit、都没有抗锯齿：
 *   - QWidget::setMask(QRegion / QBitmap)：遮罩只有“要 / 不要”两种状态，圆弧必然被量化成阶梯；
 *   - QPainter::setClipPath()：裁剪路径不做抗锯齿（Antialiasing render hint 只作用于“绘制”，不作用于“裁剪”）。
 *
 * 做法：不给控件裁外形，而是在控件最上层挂一个“鼠标穿透”的覆盖层，
 * 用外围底色把四个角涂掉；弧线由抗锯齿路径决定，因此是平滑的。
 * 覆盖层中间不填充（WA_NoSystemBackground + 不刷背景），下层内容照常显示。
 *
 * 用法：
 *   igQtAttachRoundedCorners(dock, 8, QColor("#1E1E1E"));   // 挂上（幂等）
 *   igQtAttachRoundedCorners(card, 8, 12, 8, 8, color);     // 四角不同半径（tl, tr, br, bl）
 *   igQtRefreshRoundedCorners(target, color);               // 主题变色时刷新覆盖色
 *   igQtDetachRoundedCorners(target);                       // 退出该风格时摘掉
 *
 * 注意：覆盖色必须等于“目标控件外围的底色”，否则涂出来的角会和背景撞色。
 */

#pragma once

#include <IQCore/igQtExportModule.h>
#include <QColor>
#include <QWidget>

class IG_QT_MODULE_EXPORT igQtRoundedCornerOverlay : public QWidget {
    Q_OBJECT
public:
    explicit igQtRoundedCornerOverlay(QWidget* parent = nullptr);

    // 四角统一半径（<= 0 表示不画角）
    void setUniformRadius(qreal radius);
    // 四角分别设置：左上、右上、右下、左下
    void setCornerRadii(qreal topLeft, qreal topRight, qreal bottomRight, qreal bottomLeft);
    void setCoverColor(const QColor& color);

    QColor coverColor() const { return m_coverColor; }
    qreal cornerRadius(int corner) const { return m_radius[qBound(0, corner, 3)]; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // 顺序：0=左上 1=右上 2=右下 3=左下
    qreal m_radius[4];
    QColor m_coverColor;
};

// 挂 / 刷新 / 摘（幂等；覆盖层随目标控件尺寸变化自动铺满并置顶）
IG_QT_MODULE_EXPORT void igQtAttachRoundedCorners(QWidget* target, qreal radius, const QColor& coverColor);
IG_QT_MODULE_EXPORT void igQtAttachRoundedCorners(QWidget* target, qreal topLeft, qreal topRight, qreal bottomRight,
                                                 qreal bottomLeft, const QColor& coverColor);
IG_QT_MODULE_EXPORT void igQtRefreshRoundedCorners(QWidget* target, const QColor& coverColor);
IG_QT_MODULE_EXPORT void igQtDetachRoundedCorners(QWidget* target);
