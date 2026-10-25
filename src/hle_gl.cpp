#include "guest.h"

#include <GLES2/gl2.h>
#include <vector>

#define R(name, ...) hle_register(name, [](Cpu& c) __VA_ARGS__)

// Only touched from the GL thread.
static GLuint s_array_buffer, s_element_buffer;

static const void* client_ptr(uint32_t a, bool buffer_bound) {
    return buffer_bound ? (const void*)(uintptr_t)a : (a ? g2h(a) : nullptr);
}

void hle_gl_register() {
    R("glActiveTexture", { glActiveTexture(c.arg(0)); });
    R("glAttachShader", { glAttachShader(c.arg(0), c.arg(1)); });
    R("glBindAttribLocation", { glBindAttribLocation(c.arg(0), c.arg(1), gstr(c.arg(2))); });
    R("glBindBuffer", {
        GLenum t = c.arg(0);
        if (t == GL_ARRAY_BUFFER) s_array_buffer = c.arg(1);
        else if (t == GL_ELEMENT_ARRAY_BUFFER) s_element_buffer = c.arg(1);
        glBindBuffer(t, c.arg(1));
    });
    R("glBindFramebuffer", { glBindFramebuffer(c.arg(0), c.arg(1)); });
    R("glBindRenderbuffer", { glBindRenderbuffer(c.arg(0), c.arg(1)); });
    R("glBindTexture", { glBindTexture(c.arg(0), c.arg(1)); });
    R("glBlendFunc", { glBlendFunc(c.arg(0), c.arg(1)); });
    R("glBlendEquation", { glBlendEquation(c.arg(0)); });
    R("glBufferData", { glBufferData(c.arg(0), (GLsizeiptr)(int32_t)c.arg(1), c.arg(2) ? g2h(c.arg(2)) : nullptr, c.arg(3)); });
    R("glBufferSubData", { glBufferSubData(c.arg(0), (GLintptr)(int32_t)c.arg(1), (GLsizeiptr)(int32_t)c.arg(2), g2h(c.arg(3))); });
    R("glCheckFramebufferStatus", { c.ret(glCheckFramebufferStatus(c.arg(0))); });
    R("glClear", { glClear(c.arg(0)); });
    R("glClearColor", { glClearColor(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); });
    R("glClearDepthf", { glClearDepthf(c.argf(0)); });
    R("glColorMask", { glColorMask(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    R("glCompileShader", { glCompileShader(c.arg(0)); });
    R("glCreateProgram", { c.ret(glCreateProgram()); });
    R("glCreateShader", { c.ret(glCreateShader(c.arg(0))); });
    R("glCullFace", { glCullFace(c.arg(0)); });
    R("glDeleteBuffers", {
        auto* ids = (const GLuint*)g2h(c.arg(1));
        for (int i = 0; i < (int)c.arg(0); i++) {
            if (ids[i] == s_array_buffer) s_array_buffer = 0;
            if (ids[i] == s_element_buffer) s_element_buffer = 0;
        }
        glDeleteBuffers(c.arg(0), ids);
    });
    R("glDeleteFramebuffers", { glDeleteFramebuffers(c.arg(0), (const GLuint*)g2h(c.arg(1))); });
    R("glDeleteRenderbuffers", { glDeleteRenderbuffers(c.arg(0), (const GLuint*)g2h(c.arg(1))); });
    R("glDeleteProgram", { glDeleteProgram(c.arg(0)); });
    R("glDeleteShader", { glDeleteShader(c.arg(0)); });
    R("glDeleteTextures", { glDeleteTextures(c.arg(0), (const GLuint*)g2h(c.arg(1))); });
    R("glDepthFunc", { glDepthFunc(c.arg(0)); });
    R("glDepthMask", { glDepthMask(c.arg(0)); });
    R("glDisable", { glDisable(c.arg(0)); });
    R("glDisableVertexAttribArray", { glDisableVertexAttribArray(c.arg(0)); });
    R("glDrawArrays", { glDrawArrays(c.arg(0), (GLint)c.arg(1), c.arg(2)); });
    R("glDrawElements", { glDrawElements(c.arg(0), c.arg(1), c.arg(2), client_ptr(c.arg(3), s_element_buffer != 0)); });
    R("glEnable", { glEnable(c.arg(0)); });
    R("glEnableVertexAttribArray", { glEnableVertexAttribArray(c.arg(0)); });
    R("glFinish", { glFinish(); });
    R("glFlush", { glFlush(); });
    R("glFramebufferRenderbuffer", { glFramebufferRenderbuffer(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    R("glFramebufferTexture2D", { glFramebufferTexture2D(c.arg(0), c.arg(1), c.arg(2), c.arg(3), (GLint)c.arg(4)); });
    R("glGenBuffers", { glGenBuffers(c.arg(0), (GLuint*)g2h(c.arg(1))); });
    R("glGenFramebuffers", { glGenFramebuffers(c.arg(0), (GLuint*)g2h(c.arg(1))); });
    R("glGenRenderbuffers", { glGenRenderbuffers(c.arg(0), (GLuint*)g2h(c.arg(1))); });
    R("glGenTextures", { glGenTextures(c.arg(0), (GLuint*)g2h(c.arg(1))); });
    R("glGenerateMipmap", { glGenerateMipmap(c.arg(0)); });
    R("glGetError", { c.ret(glGetError()); });
    R("glGetIntegerv", {
        GLenum p = c.arg(0);
        auto* out = (GLint*)g2h(c.arg(1));
        glGetIntegerv(p, out);
        // Mali reports 65536; engines that store this in 16 bits end up dividing by zero.
        if ((p == GL_MAX_TEXTURE_SIZE || p == GL_MAX_RENDERBUFFER_SIZE) && *out > 4096) *out = 4096;
    });
    R("glGetProgramInfoLog", { glGetProgramInfoLog(c.arg(0), c.arg(1), (GLsizei*)gp<GLsizei>(c.arg(2)), (GLchar*)g2h(c.arg(3))); });
    R("glGetProgramiv", { glGetProgramiv(c.arg(0), c.arg(1), (GLint*)g2h(c.arg(2))); });
    R("glGetShaderInfoLog", { glGetShaderInfoLog(c.arg(0), c.arg(1), (GLsizei*)gp<GLsizei>(c.arg(2)), (GLchar*)g2h(c.arg(3))); });
    R("glGetShaderiv", { glGetShaderiv(c.arg(0), c.arg(1), (GLint*)g2h(c.arg(2))); });
    R("glGetUniformLocation", { c.ret((uint32_t)glGetUniformLocation(c.arg(0), gstr(c.arg(1)))); });
    R("glGetAttribLocation", { c.ret((uint32_t)glGetAttribLocation(c.arg(0), gstr(c.arg(1)))); });
    R("glHint", { glHint(c.arg(0), c.arg(1)); });
    R("glLineWidth", { glLineWidth(c.argf(0)); });
    R("glLinkProgram", { glLinkProgram(c.arg(0)); });
    R("glPixelStorei", { glPixelStorei(c.arg(0), (GLint)c.arg(1)); });
    R("glReadPixels", { glReadPixels((GLint)c.arg(0), (GLint)c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), g2h(c.arg(6))); });
    R("glRenderbufferStorage", { glRenderbufferStorage(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    R("glScissor", { glScissor((GLint)c.arg(0), (GLint)c.arg(1), c.arg(2), c.arg(3)); });
    R("glShaderBinary", { glShaderBinary(c.arg(0), (const GLuint*)g2h(c.arg(1)), c.arg(2), g2h(c.arg(3)), c.arg(4)); });
    R("glShaderSource", {
        GLsizei n = c.arg(1);
        uint32_t strs = c.arg(2), lens = c.arg(3);
        std::vector<const GLchar*> v(n);
        for (GLsizei i = 0; i < n; i++) v[i] = gstr(rd32(strs + 4 * i));
        glShaderSource(c.arg(0), n, v.data(), lens ? (const GLint*)g2h(lens) : nullptr);
    });
    R("glTexImage2D", {
        uint32_t px = c.arg(8);
        glTexImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), c.arg(3), c.arg(4), (GLint)c.arg(5), c.arg(6), c.arg(7), px ? g2h(px) : nullptr);
    });
    R("glTexSubImage2D", {
        glTexSubImage2D(c.arg(0), (GLint)c.arg(1), (GLint)c.arg(2), (GLint)c.arg(3), c.arg(4), c.arg(5), c.arg(6), c.arg(7), g2h(c.arg(8)));
    });
    R("glCompressedTexImage2D", {
        glCompressedTexImage2D(c.arg(0), (GLint)c.arg(1), c.arg(2), c.arg(3), c.arg(4), (GLint)c.arg(5), c.arg(6), g2h(c.arg(7)));
    });
    R("glTexParameterf", { glTexParameterf(c.arg(0), c.arg(1), c.argf(2)); });
    R("glTexParameteri", { glTexParameteri(c.arg(0), c.arg(1), (GLint)c.arg(2)); });
    R("glUniform1f", { glUniform1f((GLint)c.arg(0), c.argf(1)); });
    R("glUniform2f", { glUniform2f((GLint)c.arg(0), c.argf(1), c.argf(2)); });
    R("glUniform3f", { glUniform3f((GLint)c.arg(0), c.argf(1), c.argf(2), c.argf(3)); });
    R("glUniform4f", { glUniform4f((GLint)c.arg(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4)); });
    R("glUniform1i", { glUniform1i((GLint)c.arg(0), (GLint)c.arg(1)); });
    R("glUniform4fv", { glUniform4fv((GLint)c.arg(0), c.arg(1), (const GLfloat*)g2h(c.arg(2))); });
    R("glUniformMatrix4fv", { glUniformMatrix4fv((GLint)c.arg(0), c.arg(1), c.arg(2), (const GLfloat*)g2h(c.arg(3))); });
    R("glUseProgram", { glUseProgram(c.arg(0)); });
    R("glVertexAttribPointer", {
        glVertexAttribPointer(c.arg(0), (GLint)c.arg(1), c.arg(2), c.arg(3), c.arg(4), client_ptr(c.arg(5), s_array_buffer != 0));
    });
    R("glViewport", { glViewport((GLint)c.arg(0), (GLint)c.arg(1), c.arg(2), c.arg(3)); });
}
