// Real OpenGL regression: prepared CPU -> GPU -> release GPU -> re-upload.
// No Scene changes, network, large assets, or simplification-policy changes.
#include "iGameSurfaceMesh.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

static void Require(bool ok, const char* name) {
    if (!ok) throw std::runtime_error(name);
    std::cout << "PASS " << name << std::endl;
}
class Probe final : public iGame::SurfaceMesh {
public:
    using Pointer = iGame::SmartPointer<Probe>;
    static Pointer New() { return new Probe; }
    using iGame::DrawObject::SyncGpuBuffers;
    GLuint Vao() const { return m_TriangleVAO->Handle(); }
    void Prepare() {
        m_AttributeIndex = 0; m_AttributeDimension = 0;
        m_UseColor = true; m_AttributeChanged = true;
        ConvertToDrawableData();
        if (m_RenderableMesh.SimplifiedMesh) m_RenderableMesh.SimplifiedMesh->ConvertToDrawableData();
        ConvertToDrawableData();
    }
    void CheckBuffers() {
        std::array<float, 9> xyz{};
        std::array<unsigned, 3> ids{};
        m_PositionVBO->GetSubData(0, sizeof(xyz), xyz.data());
        m_TriangleEBO->GetSubData(0, sizeof(ids), ids.data());
        Require(xyz[0] == -0.8f && xyz[3] == 0.8f && ids[0] == 0 && ids[1] == 1 && ids[2] == 2,
                "GPU-readback-coordinates-and-connectivity");
        for (GLuint attribute : {2u, 3u}) {
            GLint enabled = 0;
            glGetVertexArrayIndexediv(Vao(), attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
            Require(enabled == GL_FALSE, "empty-normal-and-UV-arrays-are-disabled");
        }
    }
};
static GLuint Shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    Require(ok, "test-shader-compiled"); return shader;
}
int main(int argc, char** argv) {
    const bool compareLegacy = argc > 1 && std::string(argv[1]) == "--compare-legacy-upload";
    QGuiApplication app(argc, argv);
    QSurfaceFormat format; format.setVersion(4, 6); format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface; surface.setFormat(format); surface.create();
    QOpenGLContext context; context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface)) return 77;
    if (!gladLoadGL()) return 77;
    try {
        std::cout << "GPU " << glGetString(GL_RENDERER) << std::endl;
        auto points = iGame::Points::New();
        points->AddPoint(-0.8f,-0.8f,0); points->AddPoint(0.8f,-0.8f,0); points->AddPoint(0,0.8f,0);
        auto faces = iGame::CellArray::New(); faces->AddCellId3(0,1,2);
        auto cp = iGame::FloatArray::New(); cp->SetName("PressureCoefficient");
        cp->AddValue(-1); cp->AddValue(0); cp->AddValue(1);
        auto mesh = Probe::New(); mesh->SetPoints(points); mesh->SetFaces(faces);
        mesh->GetAttributeSet()->AddAttribute(IG_SCALAR, IG_POINT, cp); mesh->Prepare();
        const auto before = mesh->InspectCpuDisplayCache(); Require(before.ready, "prepared-state-ready");
        GLuint vs = Shader(GL_VERTEX_SHADER, "#version 460 core\nlayout(location=0) in vec3 p; layout(location=1) in vec4 c; out vec4 color; void main(){gl_Position=vec4(p,1);color=c;}");
        GLuint fs = Shader(GL_FRAGMENT_SHADER, "#version 460 core\nin vec4 color; out vec4 pixel; void main(){pixel=vec4(color.rgb,1);}");
        GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
        GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked); Require(linked,"test-program-linked");
        GLuint fbo=0, texture=0; glCreateFramebuffers(1,&fbo); glCreateTextures(GL_TEXTURE_2D,1,&texture);
        glTextureStorage2D(texture,1,GL_RGBA8,32,32); glNamedFramebufferTexture(fbo,GL_COLOR_ATTACHMENT0,texture,0);
        Require(glCheckNamedFramebufferStatus(fbo,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"test-framebuffer-ready");
        std::array<unsigned char,32*32*4> reference{};
        for(int round=0;round<4;++round) {
            if(round==0) mesh->SyncGpuBuffers();
            else if (compareLegacy) mesh->SyncGpuBuffers();
            else Require(mesh->UploadPreparedCpuData(),"prepared-upload-only-succeeded");
            mesh->CheckBuffers();
            // The ordinary renderer still calls SyncGpuBuffers each frame.
            mesh->SyncGpuBuffers(); mesh->CheckBuffers();
            glBindFramebuffer(GL_FRAMEBUFFER,fbo); glViewport(0,0,32,32);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(program);
            glBindVertexArray(mesh->Vao()); glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,nullptr);
            std::array<unsigned char,32*32*4> pixels{};
            glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            Require(pixels[(16*32+16)*4+3]==255,"triangle-visible-after-GPU-completion");
            if(round==0)reference=pixels;
            else Require(pixels==reference,"rendered-colors-and-geometry-identical-after-reopen");
            Require(glGetError()==GL_NO_ERROR,"real-GL-cycle-no-errors");
            Require(mesh->InspectCpuDisplayCache().signature==before.signature,"no-CPU-geometry-or-scalar-rebuild");
            glBindVertexArray(0); mesh->ReleaseGpuResourcesKeepCpuData();
            Require(!mesh->HasGpuResources(),"all-model-GPU-handles-released");
        }
        mesh->ReleaseDrawableResources();
        glUseProgram(0); glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
        glBindFramebuffer(GL_FRAMEBUFFER,0); glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&texture);
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
