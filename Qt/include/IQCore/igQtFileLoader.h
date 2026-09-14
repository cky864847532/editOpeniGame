/**
 * @class   igQtFileLoader
 * @brief   igQtFileLoader's brief
 */

#pragma once
#include "iGameSceneManager.h"

#include <QFileDialog>
#include <QString>
#include <IQCore/igQtExportModule.h>

#include <thread>

using namespace iGame;

class IG_QT_MODULE_EXPORT igQtFileLoader : public QObject
{
	Q_OBJECT
public:
	igQtFileLoader(QObject* parent = nullptr);
	~igQtFileLoader() override;

public:
	void LoadFile();
    void LoadOnlineS();
    void LoadOnlineC();
	// Core file paths are UTF-8 encoded. File-system boundaries perform the
	// native conversion (UTF-16 on Windows).
	void OpenFile(const std::string& fileName);
    void OpenFiles(const QStringList& fileNames);
    void OpenSplineFile(const std::string& fileName);
    void OpenODBFile(const std::string& fileName);
    void OpenNastranFile(const QStringList& fileNames);
	void SaveFile();
	void SaveFileAs();
	void SaveCurrentFileToRecentFile(QString file_name);
	void AddCurrentFileToRecentFilePath(QString lastPath);
	void InitRecentFilePaths();
	void InitRecentFileActions(std::vector<QString>);
	void UpdateRecentActionList();
	void UpdateIniFileInfo();
	QList<QAction*> GetRecentActionList() { return this->recentFileActionList; };



signals:
	void NewModel(DataObject::Pointer obj, ItemSource source);

	void FinishReading();
	void EmitMakeCurrent();
	void EmitDoneCurrent();

	void AddFileToModelList(QString file_name);

	void LoadAnimationFile(std::vector<float>& timeValues);

protected:

	QList<QAction*> recentFileActionList;
	int maxFileNr = 10;
    SceneManager::Pointer m_SceneManager;

    // 异步加载：文件读取放到工作线程执行。
    // 原因：加载是"同步读盘 + 解析"，如果放在 GUI 线程里，事件循环会被堵死，
    // 进度条（以及窗口重绘）只能在加载结束后一次性回放 → 表现为"加载中一直不动，
    // 结束后瞬间冲到 100%"。放到工作线程后 GUI 线程空闲，进度回调（已排进 GUI 线程队列）
    // 才能实时生效。读完后通过 QueuedConnection 回到 GUI 线程挂模型。
    std::thread m_LoadThread;
    bool m_Loading{false};
};
