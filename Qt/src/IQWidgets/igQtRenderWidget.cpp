//
// Created by m_ky on 2024/4/18.
//

/**
 * @class   iGameQtGLFWWindow
 * @brief   iGameQtGLFWWindow's brief
 */
#include "iGameInteractor.h"
#include "iGameSceneManager.h"

#include <IQWidgets/igQtRenderWidget.h>
#include <QMouseEvent>
#include <QOpenGLFunctions> //
#include <QOpenGLDebugLogger>
#include <QStringList>
#include <iGamePointSet.h>
#include <iGameUnstructuredMesh.h>
#include <iGameVolumeMesh.h>
#include <qdebug.h>

igQtRenderWidget::igQtRenderWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, false);

    setMouseTracking(true);
    setMinimumHeight(185);
    setMinimumWidth(320);
    connect(this, &QOpenGLWidget::frameSwapped, this,
            &igQtRenderWidget::OnFrameSwapped);
}

igQtRenderWidget::~igQtRenderWidget() {
    if (context()) { disconnect(context(), nullptr, this, nullptr); }
    makeCurrent();
    emit ContextAboutToBeReleased();
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
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this]() {
        makeCurrent();
        emit ContextAboutToBeReleased();
        doneCurrent();
    }, Qt::DirectConnection);
    // 目前当窗口
    iGame::SceneManager::Pointer sceneManager = iGame::SceneManager::Instance();
    m_Scene = sceneManager->NewScene();
    m_Scene->Initialize();
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

void igQtRenderWidget::RequestCompletedFrame(quint64 requestId) {
    if (m_CompletedFrameRequestPending) {
        emit CompletedFrame(requestId, false,
                            QStringLiteral("A completed-frame request is already pending"));
        return;
    }
    if (!m_Scene || !isValid()) {
        emit CompletedFrame(requestId, false,
                            QStringLiteral("Render scene or OpenGL context is not ready"));
        return;
    }
    m_CompletedFrameRequestPending = true;
    m_CompletedFrameAwaitingSwap = false;
    m_CompletedFrameRequestId = requestId;
    m_RequestedAfterFrameSerial = m_Scene->GetCompletedDrawFrameSerial();
    m_RequestedGpuFrameSerial = 0;
    m_CompletedFrameDetail.clear();
    m_Scene->RequestFullResolutionFrame();
}

void igQtRenderWidget::CancelCompletedFrame(quint64 requestId) {
    if (!m_CompletedFrameRequestPending || m_CompletedFrameRequestId != requestId) {
        return;
    }
    m_CompletedFrameRequestPending = false;
    m_CompletedFrameAwaitingSwap = false;
    m_CompletedFrameDetail.clear();
    if (m_Scene) { m_Scene->CancelFullResolutionFrameRequest(); }
    // The debug logger is local to paintGL, never persistent between frames.
    // In the GUI thread a queued cancellation cannot interrupt glFinish.
}

void igQtRenderWidget::CompleteRequestedFrame(bool success, const QString& detail) {
    const quint64 requestId = m_CompletedFrameRequestId;
    // Clear before emitting: a receiver may schedule the next request.
    m_CompletedFrameRequestPending = false;
    m_CompletedFrameAwaitingSwap = false;
    m_CompletedFrameDetail.clear();
    emit CompletedFrame(requestId, success, detail);
}

void igQtRenderWidget::OnFrameSwapped() {
    if (!m_CompletedFrameRequestPending || !m_CompletedFrameAwaitingSwap) { return; }
    if (!m_Scene ||
        m_Scene->GetCompletedDrawFrameSerial() != m_RequestedGpuFrameSerial) {
        CompleteRequestedFrame(false, QStringLiteral(
                "The GPU-completed frame was replaced before Qt frameSwapped"));
        return;
    }
    const QString detail = m_CompletedFrameDetail +
            QStringLiteral("; Qt frameSwapped completed");
    CompleteRequestedFrame(true, detail);
}

void igQtRenderWidget::paintGL() {
    if (!m_CompletedFrameRequestPending || m_CompletedFrameAwaitingSwap) {
        m_Scene->Draw();
        return;
    }

    const quint64 requestedId = m_CompletedFrameRequestId;
    auto* currentContext = QOpenGLContext::currentContext();
    if (!currentContext || currentContext != context() || !currentContext->isValid()) {
        CompleteRequestedFrame(false, QStringLiteral("Expected OpenGL context is not current for the requested frame"));
        return;
    }
    auto* glFunctions = currentContext->functions();
    QStringList errors;
    QStringList preexistingErrors;
    auto collectErrors = [glFunctions](QStringList& destination, const char* phase) {
        for (GLenum error = glFunctions->glGetError(); error != GL_NO_ERROR; error = glFunctions->glGetError()) {
            destination.append(QStringLiteral("%1: GL error 0x%2")
                                  .arg(QString::fromLatin1(phase))
                                  .arg(static_cast<quint32>(error), 0, 16));
            if (destination.size() >= 32) { break; }
        }
    };
    // Do not attribute old context errors (possibly from earlier UI work) to
    // this measured frame, but preserve them in diagnostics instead of hiding
    // them. Context loss is still rejected below via context()->isValid().
    collectErrors(preexistingErrors, "before requested frame");
    if (!preexistingErrors.isEmpty()) {
        qWarning() << "Completed-frame request" << requestedId
                   << "found preexisting OpenGL errors:" << preexistingErrors;
    }

    // Several renderer helpers consume glGetError themselves. A scoped,
    // synchronous debug logger also observes those errors when KHR_debug is
    // available. Normal frames never create this logger or call glFinish.
    QOpenGLDebugLogger debugLogger;
    connect(&debugLogger, &QOpenGLDebugLogger::messageLogged, this,
            [&errors](const QOpenGLDebugMessage& message) {
                if (message.type() == QOpenGLDebugMessage::ErrorType && errors.size() < 32) {
                    errors.append(message.message());
                }
            }, Qt::DirectConnection);
    const bool debugAvailable = debugLogger.initialize();
    if (debugAvailable) {
        debugLogger.startLogging(QOpenGLDebugLogger::SynchronousLogging);
    }

    const auto serialBeforeDraw = m_Scene->GetCompletedDrawFrameSerial();
    m_Scene->Draw();
    if (!m_CompletedFrameRequestPending || m_CompletedFrameRequestId != requestedId) {
        if (debugAvailable) { debugLogger.stopLogging(); }
        return;
    }
    const auto frameSerial = m_Scene->GetCompletedDrawFrameSerial();
    if (serialBeforeDraw != m_RequestedAfterFrameSerial || frameSerial <= serialBeforeDraw) {
        if (debugAvailable) { debugLogger.stopLogging(); }
        CompleteRequestedFrame(false, QStringLiteral(
                "No new complete DrawFrame was submitted; an old-frame copy is not completion"));
        return;
    }

    // This explicit benchmark barrier includes uploads, every visible block's
    // full-resolution draw, overlays, and the final copy into the Qt FBO.
    glFunctions->glFinish();
    collectErrors(errors, "after requested frame glFinish");
    if (debugAvailable) { debugLogger.stopLogging(); }
    if (!m_CompletedFrameRequestPending || m_CompletedFrameRequestId != requestedId) { return; }
    if (!context() || !context()->isValid()) {
        errors.append(QStringLiteral("OpenGL context became invalid"));
    }
    if (!errors.isEmpty()) {
        CompleteRequestedFrame(false, errors.join(QStringLiteral("; ")));
        return;
    }

    m_RequestedGpuFrameSerial = frameSerial;
    m_CompletedFrameDetail = QStringLiteral(
            "Full-resolution DrawFrame serial=%1; GPU glFinish completed; %2")
                                    .arg(static_cast<qulonglong>(frameSerial))
                                    .arg(debugAvailable
                                                 ? QStringLiteral("synchronous GL debug error capture enabled")
                                                 : QStringLiteral("KHR_debug unavailable: only remaining GL errors checked; consumed errors cannot be excluded"));
    if (!preexistingErrors.isEmpty()) {
        m_CompletedFrameDetail += QStringLiteral("; preexisting errors not attributed to this frame: ") +
                preexistingErrors.join(QStringLiteral(", "));
    }
    m_CompletedFrameAwaitingSwap = true;
}


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
