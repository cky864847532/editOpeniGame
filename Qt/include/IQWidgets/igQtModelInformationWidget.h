#pragma once
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include "iGameDataObject.h"
class igQtModelInformationWidget : public QWidget {
public:
	igQtModelInformationWidget(QWidget* parent = nullptr);

public slots:
	void updateInformationFrame();

private:
	QLabel* createLabel(const QString& text);

	void createPropertyLabel(QFormLayout* formLayout, const QString& name, const QString& value);

	QFrame* createSeparator();

	void CreateDataObjectLayoutInfo(iGame::DataObject::Pointer obj, QFormLayout* formLayout);

protected:
	// §82：主题切换时重建信息区 —— 行/标签的颜色是在创建控件那一刻按主题算好的，
	//      不重建的话，已生成的行会一直保留旧主题颜色（浅色主题下仍是深色文字/底色）。
	void changeEvent(QEvent* e) override;

private:
	QScrollArea* scrollArea;
	QFrame* informationFrame;
	QVBoxLayout* frameLayout;
	int m_tableRow{0};
};