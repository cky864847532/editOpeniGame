//
// Created by Sumzeek on 12/9/2024.
//
#include "GLVertexArray.h"
#include "Log/iGameLogger.h"
#include <algorithm>
#include <cstdint>
#include <limits>

IGAME_NAMESPACE_BEGIN

namespace {
constexpr IGsize PrimitiveBatchLimit(IGsize primitiveSize) {
    return primitiveSize ? (IGsize{64} * 1024 * 1024 / primitiveSize) * primitiveSize
                         : static_cast<IGsize>(std::numeric_limits<GLsizei>::max());
}

// Numeric boundary checks require no giant allocation or OpenGL context.
// In particular, crossing signed 32-bit index counts must not drop a triangle
// or wrap a byte offset when the EBO position crosses 4 GiB.
constexpr bool LargeTriangleBatchCoverageIsValid() {
    constexpr IGsize total = 2404073778ULL;
    constexpr IGsize limit = PrimitiveBatchLimit(3);
    IGsize first = 0;
    IGsize batches = 0;
    IGsize lastByteOffset = 0;
    while (first < total) {
        const IGsize count = std::min(limit, total - first);
        if (count == 0 || count % 3 || count > static_cast<IGsize>(std::numeric_limits<GLsizei>::max())) { return false; }
        lastByteOffset = first * IGsize{4};
        first += count;
        ++batches;
    }
    return first == total && batches == 36 && lastByteOffset > std::numeric_limits<uint32_t>::max();
}
static_assert(PrimitiveBatchLimit(3) == 67108863ULL);
static_assert(PrimitiveBatchLimit(2) % 2 == 0);
static_assert(PrimitiveBatchLimit(0) == 2147483647ULL);
static_assert(LargeTriangleBatchCoverageIsValid());
} // namespace

GLVertexAttribute::GLVertexAttribute(unsigned int location) {
    m_Index = location;
}

GLVertexAttribute::~GLVertexAttribute() {}

unsigned int GLVertexAttribute::Index() const { return m_Index; }

GLVertexArray::GLVertexArray() {}

GLVertexArray::~GLVertexArray() {}

void GLVertexArray::Bind() const { glBindVertexArray(m_Handle); }

void GLVertexArray::Release() const { glBindVertexArray(0); }

void GLVertexArray::DrawArrays(GLenum mode, GLint first, GLsizei count) {
    glBindVertexArray(m_Handle);
    glDrawArrays(mode, first, count);
    glBindVertexArray(0);
}

void GLVertexArray::DrawElements(GLenum mode, IGsize elementCount, GLenum type,
                                 const void* indices) {
    DrawIndexed(mode, 0, 0, elementCount, type, indices, false, -1);
}

void GLVertexArray::DrawRangeElements(GLenum mode, GLuint start, GLuint end,
                                       IGsize count, GLenum type,
                                       const void* indices, GLint primitiveOffsetLocation) {
    DrawIndexed(mode, start, end, count, type, indices, true, primitiveOffsetLocation);
}

void GLVertexArray::DrawIndexed(GLenum mode, GLuint start, GLuint end,
                               IGsize count, GLenum type, const void* indices,
                               bool useRange, GLint primitiveOffsetLocation) {
    if (count == 0) { return; }
    const IGsize primitiveSize = mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : mode == GL_POINTS ? 1 : 0;
    const IGsize glCountLimit = static_cast<IGsize>(std::numeric_limits<GLsizei>::max());
    // Independent primitives may be split; strips/fans must preserve their
    // original continuity and stay on the existing single-draw path.
    const IGsize batchLimit = PrimitiveBatchLimit(primitiveSize);
    const IGsize indexBytes = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 : 0;
    const auto initialOffset = reinterpret_cast<std::uintptr_t>(indices);
    if (indexBytes == 0 || count > (std::numeric_limits<std::uintptr_t>::max() - initialOffset) / indexBytes ||
        (primitiveSize == 0 && count > glCountLimit) ||
        (primitiveOffsetLocation >= 0 && primitiveSize && count / primitiveSize > glCountLimit)) {
        if (!m_LoggedDrawError) {
            IGAME_RENDERING_ERROR("Indexed draw rejected: mode={}, count={}, type={}, byte offset={}; count or offset cannot be represented safely.",
                                  mode, count, type, initialOffset);
            m_LoggedDrawError = true;
        }
        return;
    }
    const bool batched = count > batchLimit;
    if (batched && m_LoggedLargeIndexCount != count) {
        IGAME_RENDERING_INFO("Indexed draw batching: VAO={}, indices={}, maximum indices per draw={}, draws={}, 64-bit byte offsets.",
                            m_Handle, count, batchLimit, (count + batchLimit - 1) / batchLimit);
        m_LoggedLargeIndexCount = count;
    }
    glBindVertexArray(m_Handle);
    for (IGsize first = 0; first < count;) {
        const IGsize batchCount = std::min(batchLimit, count - first);
        const auto byteOffset = initialOffset + static_cast<std::uintptr_t>(first * indexBytes);
        if (primitiveOffsetLocation >= 0) {
            glUniform1i(primitiveOffsetLocation, static_cast<GLint>(primitiveSize ? first / primitiveSize : 0));
        }
#ifdef __EMSCRIPTEN__
        (void) start;
        (void) end;
        (void) useRange;
        glDrawElements(mode, static_cast<GLsizei>(batchCount), type, reinterpret_cast<const void*>(byteOffset));
#else
        if (useRange) {
            glDrawRangeElements(mode, start, end, static_cast<GLsizei>(batchCount), type, reinterpret_cast<const void*>(byteOffset));
        } else {
            glDrawElements(mode, static_cast<GLsizei>(batchCount), type, reinterpret_cast<const void*>(byteOffset));
        }
#endif
        if (batched) {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                if (!m_LoggedDrawError) {
                    IGAME_RENDERING_ERROR("Batched indexed draw failed: GL error={}, first index={}, batch indices={}, total indices={}, byte offset={}.",
                                          error, first, batchCount, count, byteOffset);
                    m_LoggedDrawError = true;
                }
                break;
            }
        }
        first += batchCount;
    }
    if (primitiveOffsetLocation >= 0) { glUniform1i(primitiveOffsetLocation, 0); }
    glBindVertexArray(0);
}

void GLVertexArray::VertexBuffer(unsigned int vbo_binding_index,
                                 SmartPointer<GLBuffer> buffer,
                                 ptrdiff_t offset, size_t stride) {
#ifdef IGAME_OPENGL_VERSION_330
    if (offset != 0) {
        IGAME_RENDERING_ERROR("You are trying to offset the VBO in the opengl330 "
                         "version, which is illegal. Please check your code.");
    }
    GLVertexArrayManager& manager = GLVertexArrayManager::Instance();
    manager.RegisterBufferToVertexArray(m_Handle, vbo_binding_index,
                                        buffer->Handle(), stride);
#elif IGAME_OPENGL_VERSION_460
    glVertexArrayVertexBuffer(m_Handle, vbo_binding_index, buffer->Handle(),
                              offset, stride);
#endif
}

void GLVertexArray::ElementBuffer(SmartPointer<GLBuffer> buffer) {
#ifdef IGAME_OPENGL_VERSION_330
    glBindVertexArray(m_Handle);
    buffer->Target(GL_ELEMENT_ARRAY_BUFFER);
    buffer->Bind();
    glBindVertexArray(0);
#elif IGAME_OPENGL_VERSION_460
    glVertexArrayElementBuffer(m_Handle, buffer->Handle());
#endif
}

void GLVertexArray::EnableAttrib(const GLVertexAttribute& attribute) {
#ifdef IGAME_OPENGL_VERSION_330
    glBindVertexArray(m_Handle);
    glEnableVertexAttribArray(attribute.Index());
    glBindVertexArray(0);
#elif IGAME_OPENGL_VERSION_460
    glEnableVertexArrayAttrib(m_Handle, attribute.Index());
#endif
}

void GLVertexArray::AttribBindingFormat(const GLVertexAttribute& attribute,
                                        unsigned int vbo_binding_index,
                                        int size, GLenum type, bool normalized,
                                        unsigned int relative_offset) {
#ifdef IGAME_OPENGL_VERSION_330
    auto buffer = GLVertexArrayManager::Instance().GetBuffer(m_Handle,
                                                             vbo_binding_index);
    auto stride = GLVertexArrayManager::Instance().GetStride(m_Handle,
                                                             vbo_binding_index);
    GLintptr offset = static_cast<uintptr_t>(relative_offset);

    glBindVertexArray(m_Handle);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer(attribute.Index(), size, type, normalized, stride,
                          reinterpret_cast<void*>(offset));
    glBindVertexArray(0);
#elif IGAME_OPENGL_VERSION_460
    glVertexArrayAttribBinding(m_Handle, attribute.Index(), vbo_binding_index);
    glVertexArrayAttribFormat(m_Handle, attribute.Index(), size, type,
                              normalized, relative_offset);
#endif
}

void GLVertexArray::CreateHandle(GLsizei count, GLuint* handles) {
#ifdef IGAME_OPENGL_VERSION_330
    glGenVertexArrays(count, handles);
#elif IGAME_OPENGL_VERSION_460
    glCreateVertexArrays(count, handles);
#endif
}

void GLVertexArray::DestroyHandle(GLsizei count, GLuint* handles) {
#ifdef IGAME_OPENGL_VERSION_330
    GLVertexArrayManager& manager = GLVertexArrayManager::Instance();
    for (GLsizei i = 0; i < count; ++i) {
        manager.UnRegisterVertexArray(handles[i]);
    }
    glDeleteVertexArrays(count, handles);
#elif IGAME_OPENGL_VERSION_460
    glDeleteVertexArrays(count, handles);
#endif
}

void GLSetVertexAttrib(SmartPointer<GLVertexArray> VAO,
                       const GLVertexAttribute& attribute,
                       GLuint vbo_binding_index, int size, GLenum type,
                       GLboolean normalized, unsigned int offset) {
    VAO->EnableAttrib(attribute);
    VAO->AttribBindingFormat(attribute, vbo_binding_index, size, type,
                             normalized, offset);
}

IGAME_NAMESPACE_END
