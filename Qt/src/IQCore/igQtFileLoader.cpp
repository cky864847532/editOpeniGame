//
// Created by m_ky on 2024/4/22.
//
/**
 * @class   igQtFileLoader
 * @brief   igQtFileLoader's brief
 */
#include <cstring>

#include "iGameFileIO.h"
//#include "CSTest.h"
//#include "iGameMeshCodec/iGameMeshEncoder.h"
//#include "iGameMeshCodec/iGameMeshDecoder.h"
#include "iGamePointSet.h"
#include "iGameScene.h"
#if defined(GPSCUDA_ENABLE)
#include "Spline XML/iGameSplineReaderGPU.h"
#endif
#include "Abaqus/iGameODBReader.h"
#include "Client.h"
#include "Nastran/iGameNastranReader.h"
#include "Sever.h"
#include "Spline XML/iGameSplineReaderCPU.h"

#include <IQComponents/Dialog/igQtBasicListOptionDialog.h>
#include <IQComponents/Dialog/igQtSplineOptionDialog.h>
#include <IQCore/igQtFileLoader.h>
#include <IQCore/igQtFileType.h>
#include <QMetaObject>
#include <iGameType.h>

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <iostream>
#include <qaction.h>
#include <qdebug.h>
#include <qsettings.h>

namespace {
std::string ToUtf8FilePath(const QString& path) {
    const QByteArray utf8 = path.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QString FromUtf8FilePath(const std::string& path) {
    return QString::fromUtf8(path.data(), static_cast<int>(path.size()));
}
}

igQtFileLoader::igQtFileLoader(QObject* parent) : QObject(parent) {
    InitRecentFilePaths();
    m_SceneManager = SceneManager::Instance();
}

igQtFileLoader::~igQtFileLoader() {
    // 退出时等待正在进行的加载线程结束，避免它访问/回调已经销毁的对象。
    if (m_LoadThread.joinable()) { m_LoadThread.join(); }
}
void igQtFileLoader::LoadOnlineS() {
#if defined(_WIN32) || defined(_WIN64)
    std::thread server_thread(serverThread);
    server_thread.join();
#endif
}
void igQtFileLoader::LoadOnlineC() {
#if defined(_WIN32) || defined(_WIN64)
    QStringList filters = {"ALL FIle(*.obj *.off *.stl *.ply *.vtk *.mesh *.pvd *.vts *.vtu "
                           "*.vtm *.cgns *.odb *.igc *.igcm *.cas *.ccm *.rst *.rth)",
                           "VTK file(*.vtk)",
                           "CGNS file(*.cgns)",
                           "ABAQUS file(*.odb)",
                           "Spline file(*.xml)",
                           "Compression file(*.igc)",
                           "Compression Manifest file(*.igcm)",
                           "Fluent file(*.cas)",
                           "STAR-CCM+ file(*.ccm)",
                           "Ansys file(*.rst *.rth)"};
    QString selectedFilter;
    std::string filePath = ToUtf8FilePath(
            QFileDialog::getOpenFileName(nullptr, "Load file", "", filters.join(";;"), &selectedFilter));
    auto selected_idx = static_cast<FileType>(filters.indexOf(selectedFilter));
    std::cout << filePath << std::endl;
    // 关键修改：用packaged_task获取线程返回值
    // 1. 创建任务对象，绑定clientThread和参数
    std::packaged_task<std::string(int, std::string)> task(clientThread);
    // 2. 获取future对象，用于接收线程返回值
    std::future<std::string> fut = task.get_future();
    // 3. 启动线程（将task转移到线程中）
    std::thread client_thread(std::move(task), selected_idx, filePath);
    // 4. 等待线程结束，并获取返回值（localFileName）
    client_thread.join();
    std::string localFileName = fut.get();  // 获取接收后的文件名

    // 5. 传递文件名给OpenFile（判断是否为空，避免传无效值）
    if (!localFileName.empty()) {
        this->OpenFile(localFileName);  // 替换空字符串为实际文件名
    } else {
        std::cerr << "[LoadOnlineC] Failed to receive file, OpenFile skipped\n";
        this->OpenFile("");  // 失败时仍传空，保持原有逻辑
    }

#endif
}
void igQtFileLoader::LoadFile() {
    #if defined(AbqSDK_ENABLE)
        igDebug("ABAQUS SDK is enabled.");
    #else
        igDebug("ABAQUS SDK is not enabled.");
    #endif
    QStringList filters = {
        "ALL File(*.obj *.off *.stl *.ply *.vtk *.mesh *.pvd *.vts *.vtu "
        "*.vtm *.cgns *.igc *.igcm *.cas *.ccm *.rst *.rth *.xml"
#if defined(AbqSDK_ENABLE)
        " *.odb"
#endif
#if defined(NASTRAN_ENABLE)
        " *.bdf *.op2"
#endif
        " d3plot* *.d3plot"
        ")",
        "VTK file(*.vtk)",
        "CGNS file(*.cgns)",
#if defined(AbqSDK_ENABLE)
        "ABAQUS file(*.odb)",
#endif
        "Spline file(*.xml)",
#if defined(NASTRAN_ENABLE)
        "Nastran file(*.bdf *.op2)",
#endif
        "Compression file(*.igc)",
        "Compression Manifest file(*.igcm)",
        "Fluent file(*.cas)",
        "STAR-CCM+ file(*.ccm)",
        "Ansys file(*.rst *.rth)",
        "LS-DYNA file(d3plot* *.d3plot)"
    };
    QString selectedFilter;
    QStringList filePath = QFileDialog::getOpenFileNames(nullptr, "Load file", "", filters.join(";;"), &selectedFilter);

    if (filePath.isEmpty()) { return; }

    auto selected_idx = static_cast<FileType>(filters.indexOf(selectedFilter));
    if(filePath.empty()) return ;
    switch (selected_idx) {
        case FileType::Spline:
            this->OpenSplineFile(ToUtf8FilePath(filePath[0]));
            break;
#if defined(AbqSDK_ENABLE)
        case FileType::ABAQUS:
            this->OpenODBFile(ToUtf8FilePath(filePath[0]));
            break;
#endif
#if defined(NASTRAN_ENABLE)
        case FileType::BDF:
            this->OpenNastranFile(filePath);
            break;
#endif
        default:
            this->OpenFiles(filePath);
            break;
    }
}



//static DataObject::Pointer _obj;
void igQtFileLoader::OpenFile(const std::string& filePath) {
    using namespace iGame;
    if (filePath.empty()) return;
    // d3plot 文件无扩展名（d3plot / d3plot01 / ...），需放行；其余无扩展名文件仍拒绝
    if (strrchr(filePath.data(), '.') == nullptr &&
        FileIO::GetFileType(filePath) != FileIO::D3PLOT) return;

#if defined(AbqSDK_ENABLE)
    // ODB 走专用读取路径（IsRuntimeAvailable 守卫 + step 弹窗），避免通用路径崩溃
    // 覆盖"最近文件/在线客户端"等经 OpenFile 打开的入口
    const char* ext = strrchr(filePath.data(), '.');
    if (ext != nullptr) {
        std::string suffix(ext + 1);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
        if (suffix == "odb") {
            this->OpenODBFile(filePath);
            return;
        }
    }
#endif

    // 异步加载：读盘 + 解析放到工作线程，避免阻塞 GUI 线程。
    // （同步版会让事件循环在加载期间停摆，进度条只能在加载结束后一次性回放。）
    if (m_Loading) {
        igDebug("A file is already being loaded, ignore: " + filePath);
        return;
    }
    if (m_LoadThread.joinable()) { m_LoadThread.join(); } // 回收上一次已经结束的线程
    m_Loading = true;

    const std::string path = filePath;
    m_LoadThread = std::thread([this, path]() {
        // ---- 工作线程：只读数据，不碰 UI ----
        auto obj = iGame::FileIO::ReadFile(path);

        // ---- 回到 GUI 线程：挂模型 / 更新最近文件 / 发信号 ----
        QMetaObject::invokeMethod(
                this,
                [this, obj, path]() {
                    m_Loading = false;
                    if (obj == nullptr) {
                        igDebug("This file read error.");
                        return;
                    }
                    auto filename = path.substr(path.find_last_of('/') + 1);
                    obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
                    obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(path);
                    //Q_EMIT AddFileToModelList(QString(path.substr(path.find_last_of('/') + 1).c_str()));

                    this->SaveCurrentFileToRecentFile(FromUtf8FilePath(path));

                    emit NewModel(obj, ItemSource::File);
                    emit FinishReading();
                },
                Qt::QueuedConnection);
    });
}

void igQtFileLoader::OpenFiles(const QStringList& filePaths) {
    using namespace iGame;
    if (filePaths.empty()) return;

    // IGCM 是“多块清单文件”，不应被当作“多文件帧/子文件”加入模型树；
    // 若用户同时选中了 .igcm 与其子块 .igc，仅打开 .igcm 即可（子块会由清单引用并加载）。
    QStringList igcmFiles;
    for (const auto& p : filePaths) {
        if (p.endsWith(".igcm", Qt::CaseInsensitive)) {
            igcmFiles.append(p);
        }
    }
    if (!igcmFiles.empty()) {
        for (const auto& p : igcmFiles) {
            this->OpenFile(ToUtf8FilePath(p));
        }
        return;
    }

    const std::string first_file_path = ToUtf8FilePath(filePaths[0]);
    // d3plot 文件无扩展名（d3plot / d3plot01 / ...），需放行；其余无扩展名文件仍拒绝
    if (strrchr(first_file_path.data(), '.') == nullptr &&
        FileIO::GetFileType(first_file_path) != FileIO::D3PLOT) return;

    // 检测 XML 后缀，使用 Spline 弹窗处理
    const char* ext = strrchr(first_file_path.data(), '.');
    if (ext != nullptr) {
        std::string suffix(ext + 1);
        // 转换为小写进行比较
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
        if (suffix == "xml") {
            this->OpenSplineFile(first_file_path);
            return;
        }
#if defined(AbqSDK_ENABLE)
        // ODB 走专用读取路径：带 IsRuntimeAvailable 守卫 + step 弹窗，避免通用路径崩溃
        if (suffix == "odb") {
            this->OpenODBFile(first_file_path);
            return;
        }
#endif
    }

    auto obj = iGame::FileIO::ReadFile(first_file_path);
    //_obj = obj;
    if (obj == nullptr) {
        igDebug("This file read error.");
        return;
    }

    //Q_EMIT AddFileToModelList(QString(filePath.substr(filePath.find_last_of('/') + 1).c_str()));

    this->SaveCurrentFileToRecentFile(FromUtf8FilePath(first_file_path));

    /* Add left FilePaths to be as SubDataObject. */
    if(filePaths.size() > 1)
    {
        iGame::DataObject::Pointer outerObj = iGame::DrawObject::New();


        double dataRange_max[64], dataRange_min[64];
        for(int k = 0; k < obj->GetAttributeSet()->GetAllAttributes()->GetNumberOfElements(); k ++){
            auto& attr = obj->GetAttributeSet()->GetAttribute(k);
            int dim = attr.pointer->GetDimension();

            DoubleArray::Pointer array = DoubleArray::New();
            array->SetName(attr.pointer->GetName());
            array->SetDimension(dim);
            attr.UpdateAllDataRange();
            const auto& ScalarDataRange = attr.GetDataRange();

            std::fill(dataRange_min, dataRange_min + 64, DBL_MAX);
            std::fill(dataRange_max, dataRange_max + 64, DBL_MIN);
            for(int j = 0; j < dim + 1; j ++){
                dataRange_min[j] = std::min(dataRange_min[j], ScalarDataRange->GetValue(2 * j + 0));
                dataRange_max[j] = std::max(dataRange_max[j], ScalarDataRange->GetValue(2 * j + 1));
            }

            DoubleArray::Pointer parent_dataRange = DoubleArray::New();
            parent_dataRange->SetDimension(2);
            parent_dataRange->Resize(dim + 1);
            for(int j = 0; j < dim + 1; j ++){
                parent_dataRange->SetElement(j, {dataRange_min[j], dataRange_max[j]});
            }
            switch (attr.type) {
                case IG_SCALAR:
                    outerObj->GetAttributeSet()->AddScalar(attr.attachmentType, array, parent_dataRange);
                    break;
                case IG_VECTOR:
                    outerObj->GetAttributeSet()->AddVector(attr.attachmentType, array, parent_dataRange);
                    break;
                default:
                    break;
            }
        }
        outerObj->AddSubDataObject(obj);
        outerObj->UpdateSubDataObjectDataRange();

        auto filename = first_file_path.substr(first_file_path.find_last_of('/') + 1);
        outerObj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
        outerObj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(first_file_path);

        auto timeFrame = outerObj->GetTimeFrames();
        for(int i = 0; i < filePaths.size(); i ++){
            iGame::StringArray::Pointer subFileNameArray = iGame::StringArray::New();
            subFileNameArray->AddElement(ToUtf8FilePath(filePaths[i]));
            timeFrame->AddTimeStep((float) (i + 1) / filePaths.size(), subFileNameArray, StreamingType::MultiSubFiles);
        }
        //return;
        emit NewModel(outerObj, ItemSource::File);
        emit FinishReading();
        return ;
    }

    auto filename = first_file_path.substr(first_file_path.find_last_of('/') + 1);
    obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
    obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(first_file_path);
    //Q_EMIT AddFileToModelList(QString(filePath.substr(filePath.find_last_of('/') + 1).c_str()));


    //return;
    emit NewModel(obj, ItemSource::File);
    emit FinishReading();
}
void igQtFileLoader::OpenODBFile(const std::string& filePath) {
    igDebug("Open ODB file: {}", filePath);
#if defined(AbqSDK_ENABLE)
    igDebug("ABAQUS SDK is enabled.");
    using namespace iGame;
    if (filePath.empty() || strrchr(filePath.data(), '.') == nullptr) return;
    std::string runtimeError;
    if (!ODBReader::IsRuntimeAvailable(&runtimeError)) {
        igDebug("Abaqus ODB runtime unavailable: {}", runtimeError);
        QMessageBox::warning(nullptr, "Abaqus ODB unavailable",
                             QString::fromStdString(runtimeError));
        return;
    }
    igQtBasicListOptionDialog dialog;
    auto stepNames = ODBReader::ReadOdbAllStep(filePath);
    if (stepNames.empty()) {
        QMessageBox::warning(nullptr, "ODB read failed",
                             "No analysis steps could be read from this ODB file.");
        return;
    }
    dialog.setInfoList(stepNames);
    auto filename = filePath.substr(filePath.find_last_of('/') + 1);
    dialog.setWindowTitle("ODB Reader Info");
    dialog.setLabelName(filename, "More than one Step for \" %s \".Please choose one:");
    if (dialog.exec() == QDialog::Accepted) {
        int stepIdx = -1;
        stepIdx = dialog.getDialogOutput();
        if (~stepIdx) {
            auto reader = iGame::ODBReader::New();
            igDebug("Reading ODB file: {}, Step: {}", filePath, stepNames[stepIdx]);
            DataObject::Pointer obj = reader->ReadOdbFirstFrameMesh(filePath, stepNames[stepIdx]);
            if (obj == nullptr) {
                QMessageBox::warning(nullptr, "ODB read failed",
                                     "The selected ODB file could not be read.");
                return;
            }
            obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
            obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(filePath);
            //Q_EMIT AddFileToModelList(QString(filePath.substr(filePath.find_last_of('/') + 1).c_str()));

            this->SaveCurrentFileToRecentFile(FromUtf8FilePath(filePath));
            emit NewModel(obj, ItemSource::File);
            emit FinishReading();
        }
    }

#endif
}

void igQtFileLoader::OpenSplineFile(const std::string& filePath) {
    using namespace iGame;
    if (filePath.empty() || strrchr(filePath.data(), '.') == nullptr) return;
    igQtSplineOptionDialog dialog;
    dialog.setFileName(FromUtf8FilePath(filePath));

    SplineType readerType;
    if (dialog.exec() == QDialog::Accepted) {
        readerType = dialog.getDialogOutput();
    } else {
        return;
    }

#if defined(GPSCUDA_ENABLE)
    DataObject::Pointer obj = nullptr;
    switch (readerType) {
        case SplineType::BSplineSurfaceCPU: {
            SplineReaderCPU::Pointer reader = SplineReaderCPU::New();
            reader->SetFilePath(filePath);
            reader->SetSurfaceRenderForVolume(true);
            reader->Execute();
            obj = reader->GetOutput();
            break;
        }
        case SplineType::BSplineVolumeCPU: {
            SplineReaderCPU::Pointer reader = SplineReaderCPU::New();
            reader->SetFilePath(filePath);
            reader->SetSurfaceRenderForVolume(false);
            reader->Execute();
            obj = reader->GetOutput();
            break;
        }
        case SplineType::BSplineSurfaceGPU: {
            SplineReaderGPU::Pointer reader = SplineReaderGPU::New();
            reader->SetFilePath(filePath);
            reader->SetSurfaceRenderForVolume(true);
            reader->Execute();
            obj = reader->GetOutput();
            break;
        }
        case SplineType::BSplineVolumeGPU: {
            SplineReaderGPU::Pointer reader = SplineReaderGPU::New();
            reader->SetFilePath(filePath);
            reader->SetSurfaceRenderForVolume(false);
            reader->Execute();
            obj = reader->GetOutput();
            break;
        }
        default:
            igDebug("Spline file type not process.");
    }
    if (obj == nullptr) {
        igDebug("This file read error.");
        return;
    }
    auto filename = filePath.substr(filePath.find_last_of('/') + 1);
    obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
    obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(filePath);
    //Q_EMIT AddFileToModelList(QString(filePath.substr(filePath.find_last_of('/') + 1).c_str()));

    this->SaveCurrentFileToRecentFile(FromUtf8FilePath(filePath));
    emit NewModel(obj, ItemSource::File);
    emit FinishReading();

#else
    DataObject::Pointer obj = nullptr;
    /* Without Cuda version, only support Nurbs Reader.*/
    SplineReaderCPU::Pointer reader = SplineReaderCPU::New();
    //reader->SetNurbsType(readerType);
    reader->SetFilePath(filePath);
    reader->Execute();
    obj = reader->GetOutput();

    if (obj == nullptr) {
        igDebug("This file read error.");
        return;
    }
    auto filename = filePath.substr(filePath.find_last_of('/') + 1);
    obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
    obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(filePath);

    this->SaveCurrentFileToRecentFile(FromUtf8FilePath(filePath));
    emit NewModel(obj, ItemSource::File);
    emit FinishReading();
#endif
}
void igQtFileLoader::OpenNastranFile(const QStringList& fileNames) {
#if defined(NASTRAN_ENABLE)
    // --- 校验阶段 ---

    // 规则：检查是否为空
    if (fileNames.isEmpty()) {
        return; // 没有文件，安静地返回
    }

    // 规则：检查是否多于两个文件
    if (fileNames.size() > 2) {
        // QWidget* parent = this; // 如果 igQtFileLoader 继承自 QWidget，使用 'this' 作为父窗口更好
        QMessageBox::warning(nullptr, "文件过多",
                             "您一次最多只能选择两个文件（一个 .bdf 模型文件和一个 .op2 结果文件）。");
        return;
    }

    QString bdfPath = "";
    QString op2Path = "";

    // 规则：检查只有一个文件的情况
    if (fileNames.size() == 1) {
        const QString& filePath = fileNames.first();
        QFileInfo fileInfo(filePath);
        // 使用 .toLower() 确保后缀名比较时不区分大小写 (例如 .BDF 也能识别)
        QString suffix = fileInfo.suffix().toLower();

        if (suffix == "bdf") {
            // 唯一允许的情况：只有一个文件，且是 bdf
            bdfPath = filePath;
            // op2Path 保持为空
        } else if (suffix == "op2") {
            // 错误：只选了一个 op2 文件
            QMessageBox::warning(nullptr, "文件选择错误",
                                 "您只选择了一个 OP2 (结果) 文件。请至少选择一个 BDF (模型) 文件。");
            return;
        } else {
            // 错误：选了其他类型的文件
            QMessageBox::warning(nullptr, "文件类型错误", "您必须选择一个 .bdf 文件。");
            return;
        }
    }
    // 规则：检查有两个文件的情况
    else if (fileNames.size() == 2) {
        QFileInfo file1(fileNames[0]);
        QFileInfo file2(fileNames[1]);

        QString suffix1 = file1.suffix().toLower();
        QString suffix2 = file2.suffix().toLower();

        // 检查是否为一个 bdf 和一个 op2 的组合
        if (suffix1 == "bdf" && suffix2 == "op2") {
            bdfPath = fileNames[0];
            op2Path = fileNames[1];
        } else if (suffix1 == "op2" && suffix2 == "bdf") {
            bdfPath = fileNames[1];
            op2Path = fileNames[0];
        } else {
            // 错误：不是 bdf + op2 的组合 (例如 两个bdf, 或一个bdf一个txt)
            QMessageBox::warning(nullptr, "文件组合错误",
                                 "您选择了两个文件，但它们必须是一个 .bdf 文件和一个 .op2 文件。");
            return;
        }
    }

    // --- 执行阶段 ---
    iGame::NastranReader::Pointer reader = iGame::NastranReader::New();
    reader->SetBDFFileName(bdfPath.toStdString());
    if (!op2Path.isEmpty()) reader->SetOP2FileName(op2Path.toStdString());
    reader->Execute();
    DataObject::Pointer obj = reader->GetOutput();

    if (obj == nullptr) {
        igDebug("This file read error.");
        return;
    }
    auto filePath = bdfPath.toStdString();
    auto filename = filePath.substr(filePath.find_last_of('/') + 1);
    obj->SetName(filename.substr(0, filename.find_last_of('.')).c_str());
    obj->GetProperties()->AddProperty(Variant::String, "FilePath")->SetValue(filePath);

    this->SaveCurrentFileToRecentFile(QString::fromStdString(filePath));
    emit NewModel(obj, ItemSource::File);
    emit FinishReading();

#endif
}


void igQtFileLoader::SaveFile() {
    //auto currentModel = iGame::iGameManager::Instance()->GetCurrentModel();
    //if (currentModel == nullptr)return;
    //if (!iGame::iGameFileIO::WriteDataSetToFile(currentModel->DataSet, currentModel->filePath)) {
    //	igDebug("Save File Error\n");
    //}
}

void igQtFileLoader::SaveFileAs() {

    auto sceneManager = iGame::SceneManager::Instance();
    auto scene = sceneManager->GetCurrentScene();
    if (!scene) return;
    auto currentModel = scene->GetCurrentModel();
    if (!currentModel) return;
    auto obj = currentModel->GetDataObject();
    if (!obj) return;
    std::string filePath = QFileDialog::getSaveFileName(nullptr, "Save file as ", "",
                                                        "Surface Mesh(*.obj *.off *.stl *.vtk);;Volume Mesh(*.mesh "
                                                        "*.vtk *.ex2 *.e *.pvd *.vts *.vtm)")
                                   .toStdString();
    if (filePath.empty()) {
        igDebug("Could not save file with error file path\n");
        return;
    }
    if (!iGame::FileIO::WriteFile(filePath, obj)) { igDebug("Save File Error\n"); }
}

void igQtFileLoader::SaveCurrentFileToRecentFile(QString path) {
    if (path.isEmpty()) return;
    // 统一用 '/'，避免同一文件同时出现 E:/... 和 E:\... 两条记录，
    // 同时避免反斜杠在 INI 中每读写一次就翻倍。
    const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(path).trimmed());
    if (normalized.isEmpty() || normalized.length() > 4096) return;

    for (int i = 0; i < recentFileActionList.size(); i++) {
        QAction* act = recentFileActionList.at(i);
        if (QDir::cleanPath(QDir::fromNativeSeparators(act->data().toString())) == normalized) {
            // 注意：不能 delete 这个 action，它仍可能挂在“最近文件”菜单里，
            // 删掉会导致后续更新菜单时访问悬空指针并崩溃。
            // 这里只把它移到列表末尾表示“最近打开”。
            recentFileActionList.removeAt(i);
            act->setText(normalized);
            act->setData(normalized);
            recentFileActionList.append(act);
            UpdateRecentActionList();
            UpdateIniFileInfo();
            return;
        }
    }

    AddCurrentFileToRecentFilePath(normalized);
    UpdateIniFileInfo();
}
void igQtFileLoader::AddCurrentFileToRecentFilePath(QString filePath) {
    auto recentFileActions = this->GetRecentActionList();
    QAction* recentFileAction = nullptr;
    recentFileAction = new QAction(this);
    recentFileAction->setText(filePath);
    recentFileAction->setData(filePath);
    recentFileAction->setVisible(true);
    connect(recentFileAction, &QAction::triggered, this, [=, this]() { this->OpenFile(ToUtf8FilePath(filePath)); });
    this->recentFileActionList.append(recentFileAction);
    UpdateRecentActionList();
}
void igQtFileLoader::UpdateIniFileInfo() {
    //为了能记住上次打开的路径
    QSettings setting(QCoreApplication::applicationDirPath() + "/config/savePath.ini", QSettings::IniFormat);
    // 先清掉旧的 LastFilePath*，避免历史脏数据/翻倍转义残留。
    const QStringList oldKeys = setting.allKeys();
    for (const QString& key : oldKeys) {
        if (key.startsWith(QStringLiteral("LastFilePath"))) setting.remove(key);
    }

    int num = this->recentFileActionList.size();
    int idx = 0;
    for (int i = 0; i < num && idx < maxFileNr; i++) {
        if (!recentFileActionList.at(i)->isVisible()) continue;

        const QString p = QDir::cleanPath(QDir::fromNativeSeparators(
                recentFileActionList.at(i)->data().toString()).trimmed());
        // 防护：空路径或异常超长路径不允许写回，避免再次把 savePath.ini 撑爆。
        if (p.isEmpty() || p.length() > 4096) continue;

        idx++;
        const QString name = "LastFilePath" + QString::fromStdString(std::to_string(idx));
        setting.setValue(name, p);
    }
}


void igQtFileLoader::InitRecentFilePaths() {
    const QString path = QCoreApplication::applicationDirPath() + "/config/savePath.ini";

    // 防护：最近文件记录一旦异常膨胀（上次崩溃/脏数据写入超长路径），
    // 直接废弃重建，避免启动时读取超大 INI 文件。
    QFileInfo info(path);
    if (info.exists() && info.size() > 1024 * 1024) {
        QFile::remove(path);
        return;
    }

    // 用 QSettings 读取，交给 Qt 处理 INI 的转义/反转义。
    // 旧实现手动 readLine 且不做反转义，导致路径中的反斜杠每读写一次就翻倍，
    // 最终 savePath.ini 被撑爆并在启动时引发卡死/崩溃。
    QSettings setting(path, QSettings::IniFormat);
    QMap<int, QString> entries;
    QSet<QString> seenPaths;
    const QStringList keys = setting.allKeys();
    for (const QString& key : keys) {
        if (!key.startsWith(QStringLiteral("LastFilePath"))) continue;

        bool ok = false;
        const int idx = key.mid(QStringLiteral("LastFilePath").size()).toInt(&ok);
        if (!ok) continue;

        const QString p = QDir::cleanPath(QDir::fromNativeSeparators(setting.value(key).toString().trimmed()));
        if (p.isEmpty() || p.length() > 4096) continue;
        if (seenPaths.contains(p)) continue;

        seenPaths.insert(p);
        entries.insert(idx, p);
    }

    std::vector<QString> FilePaths;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        FilePaths.emplace_back(it.value());
        if (FilePaths.size() >= static_cast<size_t>(maxFileNr)) break;
    }
    InitRecentFileActions(FilePaths);

    // 启动时立即回写一次规范化后的最近文件列表：
    // 清掉历史脏 key、去掉重复项、避免旧 INI 中的反斜杠残留继续膨胀。
    UpdateIniFileInfo();
}

void igQtFileLoader::InitRecentFileActions(std::vector<QString> FilePaths) {
    QAction* recentFileAction = nullptr;
    for (int i = 0; i < FilePaths.size(); i++) {
        recentFileAction = new QAction(this);
        recentFileAction->setText(FilePaths[i]);
        recentFileAction->setData(FilePaths[i]);
        recentFileAction->setVisible(false);
        connect(recentFileAction, &QAction::triggered, this, [=, this]() { this->OpenFile(ToUtf8FilePath(FilePaths[i])); });
        this->recentFileActionList.append(recentFileAction);
    }
    UpdateRecentActionList();
    return;
}

void igQtFileLoader::UpdateRecentActionList() {
    int st = this->recentFileActionList.size() - 1;
    ;
    int ed = std::max(st - maxFileNr + 1, 0);
    for (int i = st; i >= ed; i--) { this->recentFileActionList.at(i)->setVisible(true); }
    for (int i = ed - 1; i >= 0; i--) { this->recentFileActionList.at(i)->setVisible(false); }
    return;
}
