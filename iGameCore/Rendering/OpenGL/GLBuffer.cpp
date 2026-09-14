//
// Created by Sumzeek on 12/9/2024.
//
#include "GLBuffer.h"

IGAME_NAMESPACE_BEGIN

#ifdef __EMSCRIPTEN__
namespace
{
bool IsSupportedBufferTarget(GLenum target) {
    return target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
}
} // namespace
#endif

GLBuffer::GLBuffer() { m_Target = GL_NONE; }

GLBuffer::~GLBuffer() {
    if (m_Handle) glDeleteBuffers(1, &m_Handle);
}

void GLBuffer::CopySubData(const SmartPointer<GLBuffer> source,
                           const SmartPointer<GLBuffer> destination,
                           size_t read_offset, size_t write_offset,
                           size_t size) {
#ifdef IGAME_OPENGL_VERSION_330
    glBindBuffer(GL_COPY_READ_BUFFER, source->Handle());
    glBindBuffer(GL_COPY_WRITE_BUFFER, destination->Handle());
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, read_offset,
                        write_offset, size);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
#elif IGAME_OPENGL_VERSION_460
    glCopyNamedBufferSubData(source->Handle(), destination->Handle(),
                             read_offset, write_offset, size);
#endif
}

void GLBuffer::Allocate(size_t size, const void* data, GLenum usage) {
    const bool largeAllocation = size >= (size_t{1} << 30);
    if (largeAllocation) {
        IGAME_RENDERING_INFO("[GLBuffer] Allocating handle={} bytes={}", m_Handle, size);
    }
#ifdef IGAME_OPENGL_VERSION_330
    #ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
    #endif
    glBindBuffer(m_Target, m_Handle);
    glBufferData(m_Target, size, data, usage);
    glBindBuffer(m_Target, 0);
#elif IGAME_OPENGL_VERSION_460
    glNamedBufferData(m_Handle, size, data, usage);
#endif
#ifndef __EMSCRIPTEN__
    if (largeAllocation) {
        const GLenum error = glGetError();
        GLint64 allocatedBytes = 0;
#ifdef IGAME_OPENGL_VERSION_460
        glGetNamedBufferParameteri64v(m_Handle, GL_BUFFER_SIZE, &allocatedBytes);
#else
        glBindBuffer(m_Target, m_Handle);
        glGetBufferParameteri64v(m_Target, GL_BUFFER_SIZE, &allocatedBytes);
        glBindBuffer(m_Target, 0);
#endif
        if (error != GL_NO_ERROR || allocatedBytes != static_cast<GLint64>(size)) {
            IGAME_RENDERING_ERROR("[GLBuffer] Allocation failed handle={} requested={} actual={} GLerror={}",
                                  m_Handle, size, allocatedBytes, error);
        } else {
            IGAME_RENDERING_INFO("[GLBuffer] Allocation complete handle={} bytes={}", m_Handle, allocatedBytes);
        }
    }
#endif
}

void GLBuffer::Storage(size_t size, const void* data, GLbitfield flags) {
#ifdef IGAME_OPENGL_VERSION_330
    IGAME_RENDERING_ERROR(
            "[GLBuffer::Storage] OpenGL 3.3 (Version 330) does no support "
            "glNamedBufferStorage. Attempted to call this function with size: "
            "{} and flags: {}. Please consider using a different method.",
            size, flags);
#elif IGAME_OPENGL_VERSION_460
    glNamedBufferStorage(m_Handle, size, data, flags);
#endif
}

void* GLBuffer::MapRange(size_t offset, size_t length,
                         GLbitfield access) const {
#ifdef IGAME_OPENGL_VERSION_330
    #ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return nullptr; }
    #endif
    glBindBuffer(m_Target, m_Handle);
    void* ptr = glMapBufferRange(m_Target, offset, length, access);
    glBindBuffer(m_Target, 0);

    if (ptr == nullptr) {
        IGAME_RENDERING_ERROR(
                "[GLBuffer::MapRange] OpenGL 3.3: Failed to map buffer range. "
                "Target: {}, Offset: {}, Length: {}, Access: {}",
                m_Target, offset, length, access);
    }
    return ptr;
#elif IGAME_OPENGL_VERSION_460
    void* ptr = glMapNamedBufferRange(m_Handle, offset, length, access);

    if (ptr == nullptr) {
        IGAME_RENDERING_ERROR(
                "[GLBuffer::MapRange] OpenGL 4.6: Failed to map buffer range. "
                "Buffer Handle: {}, Offset: {}, Length: {}, Access: {}",
                m_Handle, offset, length, access);
    }
    return ptr;
#endif
}

void GLBuffer::SubData(size_t offset, size_t size, const void* data) const {
#ifdef IGAME_OPENGL_VERSION_330
    #ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
    #endif
    glBindBuffer(m_Target, m_Handle);
    glBufferSubData(m_Target, offset, size, data);
    glBindBuffer(m_Target, 0);
#elif IGAME_OPENGL_VERSION_460
    glNamedBufferSubData(m_Handle, offset, size, data);
#endif
}

void GLBuffer::GetSubData(size_t offset, size_t size, void* data) const {
#ifdef IGAME_OPENGL_VERSION_330
    #ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
    #endif
    glBindBuffer(m_Target, m_Handle);
    glGetBufferSubData(m_Target, offset, size, data);
    glBindBuffer(m_Target, 0);
#elif IGAME_OPENGL_VERSION_460
    glGetNamedBufferSubData(m_Handle, offset, size, data);
#endif
}

void GLBuffer::Unmap() {
#ifdef IGAME_OPENGL_VERSION_330
    #ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
    #endif
    glBindBuffer(m_Target, m_Handle);
    if (!glUnmapBuffer(m_Target)) {
        glBindBuffer(m_Target, 0);
        IGAME_RENDERING_ERROR(
                "[GLBuffer::Unmap] OpenGL 3.3: Failed to unmap buffer. "
                "Buffer Handle: {}, Target: {}, Data corruption detected.",
                m_Handle, m_Target);
    }
    glBindBuffer(m_Target, 0);
#elif IGAME_OPENGL_VERSION_460
    if (!glUnmapNamedBuffer(m_Handle)) {
        IGAME_RENDERING_ERROR(
                "[GLBuffer::Unmap] OpenGL 4.6: Failed to unmap buffer. "
                "Buffer Handle: {}, Data corruption detected.",
                m_Handle);
    }
#endif
}

void GLBuffer::Target(GLenum target) { m_Target = target; }

void GLBuffer::Bind() const {
#ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
#endif
    glBindBuffer(m_Target, m_Handle);
}

void GLBuffer::Release() const {
#ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
#endif
    glBindBuffer(m_Target, 0);
}

void GLBuffer::BindBase(unsigned index) const {
#ifdef __EMSCRIPTEN__
    if (!IsSupportedBufferTarget(m_Target)) { return; }
#endif
    glBindBufferBase(m_Target, index, m_Handle);
}

void GLBuffer::CreateHandle(GLsizei count, GLuint* handles) {
#ifdef IGAME_OPENGL_VERSION_330
    glGenBuffers(count, handles);
#elif IGAME_OPENGL_VERSION_460
    glCreateBuffers(count, handles);
#endif
}

void GLBuffer::DestroyHandle(GLsizei count, GLuint* handles) {
#ifdef IGAME_OPENGL_VERSION_330
    glDeleteBuffers(count, handles);
#elif IGAME_OPENGL_VERSION_460
    glDeleteBuffers(count, handles);
#endif
}

void GLAllocateGLBuffer(SmartPointer<GLBuffer> vbo, size_t size,
                        const void* data) {
    vbo->Allocate(size, data, GL_STATIC_DRAW);
}

IGAME_NAMESPACE_END
