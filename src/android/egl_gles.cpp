// EGL and OpenGL ES 2.0 for the guest. GL takes pointers to guest memory
// directly (identity mapping); EGL objects travel as opaque handles.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../game/perf_patches.h"
#include "../hle/hle.h"
#include "../mem.h"
#include "android_glue.h"
#include "handles.h"

namespace zb::android {
namespace {

using hle::gcstr;

PtrMap<void> g_displays(0x21);
PtrMap<void> g_configs(0x22);
PtrMap<void> g_contexts(0x23);
PtrMap<void> g_surfaces(0x24);

EGLDisplay D(u32 h) { return g_displays.to_host(h); }
EGLConfig C(u32 h) { return g_configs.to_host(h); }
EGLContext X(u32 h) { return g_contexts.to_host(h); }
EGLSurface S(u32 h) { return g_surfaces.to_host(h); }

// ------------------------------------------------------------ EGL

u32 e_GetDisplay(u32) { return g_displays.to_guest(eglGetDisplay(EGL_DEFAULT_DISPLAY)); }
u32 e_Initialize(u32 d, gp<i32> major, gp<i32> minor) {
    return eglInitialize(D(d), major.a ? major.get() : nullptr, minor.a ? minor.get() : nullptr);
}
u32 e_Terminate(u32 d) { return eglTerminate(D(d)); }
u32 e_ChooseConfig(u32 d, gaddr attribs, gaddr configs, i32 size, gp<i32> num) {
    std::vector<EGLConfig> tmp(size > 0 ? size : 0);
    EGLint n = 0;
    EGLBoolean r = eglChooseConfig(D(d), hp<const EGLint>(attribs), configs ? tmp.data() : nullptr,
                                   configs ? size : 0, &n);
    if (r && configs) {
        for (EGLint i = 0; i < n && i < size; ++i) wr<u32>(configs + 4 * i, g_configs.to_guest(tmp[i]));
    }
    if (num.a) *num.get() = n;
    return r;
}
u32 e_GetConfigs(u32 d, gaddr configs, i32 size, gp<i32> num) {
    std::vector<EGLConfig> tmp(size > 0 ? size : 0);
    EGLint n = 0;
    EGLBoolean r = eglGetConfigs(D(d), configs ? tmp.data() : nullptr, configs ? size : 0, &n);
    if (r && configs) {
        for (EGLint i = 0; i < n && i < size; ++i) wr<u32>(configs + 4 * i, g_configs.to_guest(tmp[i]));
    }
    if (num.a) *num.get() = n;
    return r;
}
u32 e_GetConfigAttrib(u32 d, u32 c, i32 attr, gp<i32> value) { return eglGetConfigAttrib(D(d), C(c), attr, value); }
u32 e_CreateWindowSurface(u32 d, u32 c, u32 win, gaddr attribs) {
    ANativeWindow* window = host_window(win);
    if (!window) {
        ZLOGW("eglCreateWindowSurface: the window 0x%x no longer exists", win);
        return 0;  // EGL_NO_SURFACE
    }
    EGLSurface s = eglCreateWindowSurface(D(d), C(c), window, hp<const EGLint>(attribs));
    ZLOGI("eglCreateWindowSurface -> %p (error 0x%x)", s, s ? EGL_SUCCESS : eglGetError());
    return g_surfaces.to_guest(s);
}
u32 e_CreatePbufferSurface(u32 d, u32 c, gaddr attribs) {
    return g_surfaces.to_guest(eglCreatePbufferSurface(D(d), C(c), hp<const EGLint>(attribs)));
}
u32 e_CreateContext(u32 d, u32 c, u32 share, gaddr attribs) {
    EGLContext x = eglCreateContext(D(d), C(c), X(share), hp<const EGLint>(attribs));
    ZLOGI("eglCreateContext -> %p", x);
    return g_contexts.to_guest(x);
}
u32 e_DestroyContext(u32 d, u32 x) {
    EGLContext ctx = X(x);
    EGLBoolean r = eglDestroyContext(D(d), ctx);
    g_contexts.remove(ctx);
    return r;
}
u32 e_DestroySurface(u32 d, u32 s) {
    EGLSurface surf = S(s);
    EGLBoolean r = eglDestroySurface(D(d), surf);
    g_surfaces.remove(surf);
    return r;
}
u32 e_MakeCurrent(u32 d, u32 draw, u32 read, u32 x) { return eglMakeCurrent(D(d), S(draw), S(read), X(x)); }
// With debug.acp64.verbose=1, once a second: frames presented, and how long each frame spent waiting inside
// eglSwapBuffers (the display's vsync, i.e. idle time) compared with the time the game and the emulator
// needed to produce it.
int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u32 e_SwapBuffers(u32 d, u32 s) {
    if (!g_debug) return eglSwapBuffers(D(d), S(s));
    static int64_t window_start = 0, in_swap = 0, last_end = 0, busy = 0, worst = 0;
    static int frames = 0, slow = 0;
    // A frame is late when it takes more than 1.5 frame periods of the target rate.
    const int64_t late_us = 1500000 / game::target_fps();
    int64_t t0 = now_us();
    u32 r = eglSwapBuffers(D(d), S(s));
    int64_t t1 = now_us();
    if (last_end) {
        int64_t frame = t1 - last_end;  // whole frame, swap included
        worst = std::max(worst, frame);
        if (frame > 50000) ZLOGI("long frame: %.1f ms (%.1f ms before the swap)", frame / 1000.0, (t0 - last_end) / 1000.0);
        if (frame > late_us) slow++;
        busy += t0 - last_end;
    }
    last_end = t1;
    in_swap += t1 - t0;
    frames++;
    if (!window_start) window_start = t0;
    if (t1 - window_start >= 1000000) {
        double n = frames;
        double secs = (t1 - window_start) / 1e6;
        ZLOGI("swap: %d frames in %.2fs = %.1f fps; per frame: %.1f ms of work, %.1f ms waiting in eglSwapBuffers; "
              "worst %.1f ms, %d frames over %.1f ms",
              frames, secs, n / secs, busy / n / 1000.0, in_swap / n / 1000.0, worst / 1000.0, slow, late_us / 1000.0);
        cpu::log_call_stats(secs);
        window_start = t1;
        in_swap = busy = worst = 0;
        frames = slow = 0;
    }
    return r;
}
u32 e_SwapInterval(u32 d, i32 n) {
    ZLOGI("eglSwapInterval(%d)", n);
    return eglSwapInterval(D(d), n);
}
i32 e_GetError() { return eglGetError(); }
u32 e_QuerySurface(u32 d, u32 s, i32 attr, gp<i32> v) { return eglQuerySurface(D(d), S(s), attr, v); }
u32 e_QueryContext(u32 d, u32 x, i32 attr, gp<i32> v) { return eglQueryContext(D(d), X(x), attr, v); }
gaddr e_QueryString(u32 d, i32 name) {
    const char* s = eglQueryString(D(d), name);
    return s ? hle::static_string(s) : 0;
}
u32 e_GetCurrentContext() { return g_contexts.to_guest(eglGetCurrentContext()); }
u32 e_GetCurrentDisplay() { return g_displays.to_guest(eglGetCurrentDisplay()); }
u32 e_GetCurrentSurface(i32 which) { return g_surfaces.to_guest(eglGetCurrentSurface(which)); }
u32 e_BindAPI(u32 api) { return eglBindAPI(api); }
u32 e_WaitGL() { return eglWaitGL(); }

gaddr e_GetProcAddress(gcstr name) {
    gaddr a = cpu::find_thunk(name.get());
    if (!a) ZLOGW("eglGetProcAddress(%s): not bridged", name.get());
    return a;
}

// ------------------------------------------------------------ GLES 2.0

#define V(p) hp<const void>(p)
void g_ActiveTexture(u32 t) { glActiveTexture(t); }
void g_AttachShader(u32 p, u32 s) { glAttachShader(p, s); }
void g_BindAttribLocation(u32 p, u32 i, gcstr n) { glBindAttribLocation(p, i, n); }
void g_BindBuffer(u32 t, u32 b) { glBindBuffer(t, b); }
void g_BindFramebuffer(u32 t, u32 f) { glBindFramebuffer(t, f); }
void g_BindRenderbuffer(u32 t, u32 r) { glBindRenderbuffer(t, r); }
void g_BindTexture(u32 t, u32 x) { glBindTexture(t, x); }
void g_BlendColor(float r, float g, float b, float a) { glBlendColor(r, g, b, a); }
void g_BlendEquation(u32 m) { glBlendEquation(m); }
void g_BlendEquationSeparate(u32 a, u32 b) { glBlendEquationSeparate(a, b); }
void g_BlendFunc(u32 s, u32 d) { glBlendFunc(s, d); }
void g_BlendFuncSeparate(u32 a, u32 b, u32 c, u32 d) { glBlendFuncSeparate(a, b, c, d); }
void g_BufferData(u32 t, i32 size, gaddr data, u32 usage) { glBufferData(t, size, V(data), usage); }
void g_BufferSubData(u32 t, i32 off, i32 size, gaddr data) { glBufferSubData(t, off, size, V(data)); }
u32 g_CheckFramebufferStatus(u32 t) { return glCheckFramebufferStatus(t); }
void g_Clear(u32 m) { glClear(m); }
void g_ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void g_ClearDepthf(float d) { glClearDepthf(d); }
void g_ClearStencil(i32 s) { glClearStencil(s); }
void g_ColorMask(u8 r, u8 g, u8 b, u8 a) { glColorMask(r, g, b, a); }
void g_CompileShader(u32 s) { glCompileShader(s); }
void g_CompressedTexImage2D(u32 t, i32 l, u32 f, i32 w, i32 h, i32 b, i32 size, gaddr d) {
    glCompressedTexImage2D(t, l, f, w, h, b, size, V(d));
}
void g_CompressedTexSubImage2D(u32 t, i32 l, i32 x, i32 y, i32 w, i32 h, u32 f, i32 size, gaddr d) {
    glCompressedTexSubImage2D(t, l, x, y, w, h, f, size, V(d));
}
void g_CopyTexImage2D(u32 t, i32 l, u32 f, i32 x, i32 y, i32 w, i32 h, i32 b) { glCopyTexImage2D(t, l, f, x, y, w, h, b); }
void g_CopyTexSubImage2D(u32 t, i32 l, i32 xo, i32 yo, i32 x, i32 y, i32 w, i32 h) {
    glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
u32 g_CreateProgram() { return glCreateProgram(); }
u32 g_CreateShader(u32 t) { return glCreateShader(t); }
void g_CullFace(u32 m) { glCullFace(m); }
void g_DeleteBuffers(i32 n, gaddr p) { glDeleteBuffers(n, hp<const GLuint>(p)); }
void g_DeleteFramebuffers(i32 n, gaddr p) { glDeleteFramebuffers(n, hp<const GLuint>(p)); }
void g_DeleteProgram(u32 p) { glDeleteProgram(p); }
void g_DeleteRenderbuffers(i32 n, gaddr p) { glDeleteRenderbuffers(n, hp<const GLuint>(p)); }
void g_DeleteShader(u32 s) { glDeleteShader(s); }
void g_DeleteTextures(i32 n, gaddr p) { glDeleteTextures(n, hp<const GLuint>(p)); }
void g_DepthFunc(u32 f) { glDepthFunc(f); }
void g_DepthMask(u8 m) { glDepthMask(m); }
void g_DepthRangef(float n, float f) { glDepthRangef(n, f); }
void g_DetachShader(u32 p, u32 s) { glDetachShader(p, s); }
void g_Disable(u32 c) { glDisable(c); }
void g_DisableVertexAttribArray(u32 i) { glDisableVertexAttribArray(i); }
void g_DrawArrays(u32 m, i32 f, i32 c) { glDrawArrays(m, f, c); }
void g_DrawElements(u32 m, i32 c, u32 t, gaddr i) { glDrawElements(m, c, t, V(i)); }
void g_Enable(u32 c) { glEnable(c); }
void g_EnableVertexAttribArray(u32 i) { glEnableVertexAttribArray(i); }
void g_Finish() { glFinish(); }
void g_Flush() { glFlush(); }
void g_FramebufferRenderbuffer(u32 t, u32 a, u32 rt, u32 r) { glFramebufferRenderbuffer(t, a, rt, r); }
void g_FramebufferTexture2D(u32 t, u32 a, u32 tt, u32 x, i32 l) { glFramebufferTexture2D(t, a, tt, x, l); }
void g_FrontFace(u32 m) { glFrontFace(m); }
void g_GenBuffers(i32 n, gaddr p) { glGenBuffers(n, hp<GLuint>(p)); }
void g_GenFramebuffers(i32 n, gaddr p) { glGenFramebuffers(n, hp<GLuint>(p)); }
void g_GenRenderbuffers(i32 n, gaddr p) { glGenRenderbuffers(n, hp<GLuint>(p)); }
void g_GenTextures(i32 n, gaddr p) { glGenTextures(n, hp<GLuint>(p)); }
void g_GenerateMipmap(u32 t) { glGenerateMipmap(t); }
void g_GetActiveAttrib(u32 p, u32 i, i32 bs, gaddr len, gaddr size, gaddr type, gaddr name) {
    glGetActiveAttrib(p, i, bs, hp<GLsizei>(len), hp<GLint>(size), hp<GLenum>(type), hp<GLchar>(name));
}
void g_GetActiveUniform(u32 p, u32 i, i32 bs, gaddr len, gaddr size, gaddr type, gaddr name) {
    glGetActiveUniform(p, i, bs, hp<GLsizei>(len), hp<GLint>(size), hp<GLenum>(type), hp<GLchar>(name));
}
i32 g_GetAttribLocation(u32 p, gcstr n) { return glGetAttribLocation(p, n); }
void g_GetBooleanv(u32 n, gaddr v) { glGetBooleanv(n, hp<GLboolean>(v)); }
u32 g_GetError() { return glGetError(); }
void g_GetFloatv(u32 n, gaddr v) { glGetFloatv(n, hp<GLfloat>(v)); }
void g_GetIntegerv(u32 n, gaddr v) { glGetIntegerv(n, hp<GLint>(v)); }
void g_GetProgramInfoLog(u32 p, i32 bs, gaddr len, gaddr log) { glGetProgramInfoLog(p, bs, hp<GLsizei>(len), hp<GLchar>(log)); }
void g_GetProgramiv(u32 p, u32 n, gaddr v) { glGetProgramiv(p, n, hp<GLint>(v)); }
void g_GetShaderInfoLog(u32 s, i32 bs, gaddr len, gaddr log) { glGetShaderInfoLog(s, bs, hp<GLsizei>(len), hp<GLchar>(log)); }
void g_GetShaderiv(u32 s, u32 n, gaddr v) { glGetShaderiv(s, n, hp<GLint>(v)); }
gaddr g_GetString(u32 n) {
    const GLubyte* s = glGetString(n);
    if (s && (n == GL_RENDERER || n == GL_VERSION)) ZLOGI("glGetString(0x%x) = %s", n, s);
    return s ? hle::static_string(reinterpret_cast<const char*>(s)) : 0;
}
i32 g_GetUniformLocation(u32 p, gcstr n) { return glGetUniformLocation(p, n); }
void g_Hint(u32 t, u32 m) { glHint(t, m); }
u8 g_IsEnabled(u32 c) { return glIsEnabled(c); }
u8 g_IsTexture(u32 t) { return glIsTexture(t); }
void g_LineWidth(float w) { glLineWidth(w); }
void g_LinkProgram(u32 p) { glLinkProgram(p); }
void g_PixelStorei(u32 n, i32 v) { glPixelStorei(n, v); }
void g_PolygonOffset(float f, float u) { glPolygonOffset(f, u); }
void g_ReadPixels(i32 x, i32 y, i32 w, i32 h, u32 f, u32 t, gaddr p) { glReadPixels(x, y, w, h, f, t, hp(p)); }
void g_RenderbufferStorage(u32 t, u32 f, i32 w, i32 h) { glRenderbufferStorage(t, f, w, h); }
void g_Scissor(i32 x, i32 y, i32 w, i32 h) { glScissor(x, y, w, h); }
void g_ShaderSource(u32 s, i32 count, gaddr strings, gaddr lengths) {
    std::vector<const GLchar*> v(count > 0 ? count : 0);
    for (i32 i = 0; i < count; ++i) v[i] = hp<const GLchar>(rd<u32>(strings + 4 * i));
    glShaderSource(s, count, v.data(), hp<const GLint>(lengths));
}
void g_StencilFunc(u32 f, i32 r, u32 m) { glStencilFunc(f, r, m); }
void g_StencilFuncSeparate(u32 face, u32 f, i32 r, u32 m) { glStencilFuncSeparate(face, f, r, m); }
void g_StencilMask(u32 m) { glStencilMask(m); }
void g_StencilMaskSeparate(u32 face, u32 m) { glStencilMaskSeparate(face, m); }
void g_StencilOp(u32 a, u32 b, u32 c) { glStencilOp(a, b, c); }
void g_StencilOpSeparate(u32 face, u32 a, u32 b, u32 c) { glStencilOpSeparate(face, a, b, c); }
void g_TexImage2D(u32 t, i32 l, i32 ifmt, i32 w, i32 h, i32 b, u32 f, u32 ty, gaddr p) {
    glTexImage2D(t, l, ifmt, w, h, b, f, ty, V(p));
}
void g_TexParameterf(u32 t, u32 n, float v) { glTexParameterf(t, n, v); }
void g_TexParameteri(u32 t, u32 n, i32 v) { glTexParameteri(t, n, v); }
void g_TexSubImage2D(u32 t, i32 l, i32 x, i32 y, i32 w, i32 h, u32 f, u32 ty, gaddr p) {
    glTexSubImage2D(t, l, x, y, w, h, f, ty, V(p));
}
void g_Uniform1f(i32 l, float a) { glUniform1f(l, a); }
void g_Uniform2f(i32 l, float a, float b) { glUniform2f(l, a, b); }
void g_Uniform3f(i32 l, float a, float b, float c) { glUniform3f(l, a, b, c); }
void g_Uniform4f(i32 l, float a, float b, float c, float d) { glUniform4f(l, a, b, c, d); }
void g_Uniform1i(i32 l, i32 a) { glUniform1i(l, a); }
void g_Uniform1fv(i32 l, i32 c, gaddr v) { glUniform1fv(l, c, hp<const GLfloat>(v)); }
void g_Uniform2fv(i32 l, i32 c, gaddr v) { glUniform2fv(l, c, hp<const GLfloat>(v)); }
void g_Uniform3fv(i32 l, i32 c, gaddr v) { glUniform3fv(l, c, hp<const GLfloat>(v)); }
void g_Uniform4fv(i32 l, i32 c, gaddr v) { glUniform4fv(l, c, hp<const GLfloat>(v)); }
void g_Uniform1iv(i32 l, i32 c, gaddr v) { glUniform1iv(l, c, hp<const GLint>(v)); }
void g_UniformMatrix2fv(i32 l, i32 c, u8 t, gaddr v) { glUniformMatrix2fv(l, c, t, hp<const GLfloat>(v)); }
void g_UniformMatrix3fv(i32 l, i32 c, u8 t, gaddr v) { glUniformMatrix3fv(l, c, t, hp<const GLfloat>(v)); }
void g_UniformMatrix4fv(i32 l, i32 c, u8 t, gaddr v) { glUniformMatrix4fv(l, c, t, hp<const GLfloat>(v)); }
void g_UseProgram(u32 p) { glUseProgram(p); }
void g_ValidateProgram(u32 p) { glValidateProgram(p); }
void g_VertexAttrib4f(u32 i, float a, float b, float c, float d) { glVertexAttrib4f(i, a, b, c, d); }
void g_VertexAttribPointer(u32 i, i32 size, u32 type, u8 norm, i32 stride, gaddr p) {
    glVertexAttribPointer(i, size, type, norm, stride, V(p));
}
void g_Viewport(i32 x, i32 y, i32 w, i32 h) { glViewport(x, y, w, h); }
#undef V

// ------------------------------------------------------------ common extensions (eglGetProcAddress)

template <typename F>
F ext(const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, void*> cache;
    std::lock_guard lk(m);
    auto it = cache.find(name);
    if (it == cache.end()) it = cache.emplace(name, reinterpret_cast<void*>(eglGetProcAddress(name))).first;
    return reinterpret_cast<F>(it->second);
}

void x_GenVertexArraysOES(i32 n, gaddr p) {
    if (auto f = ext<PFNGLGENVERTEXARRAYSOESPROC>("glGenVertexArraysOES")) f(n, hp<GLuint>(p));
}
void x_BindVertexArrayOES(u32 a) {
    if (auto f = ext<PFNGLBINDVERTEXARRAYOESPROC>("glBindVertexArrayOES")) f(a);
}
void x_DeleteVertexArraysOES(i32 n, gaddr p) {
    if (auto f = ext<PFNGLDELETEVERTEXARRAYSOESPROC>("glDeleteVertexArraysOES")) f(n, hp<const GLuint>(p));
}
u8 x_IsVertexArrayOES(u32 a) {
    auto f = ext<PFNGLISVERTEXARRAYOESPROC>("glIsVertexArrayOES");
    return f ? f(a) : 0;
}
void x_DiscardFramebufferEXT(u32 t, i32 n, gaddr att) {
    if (auto f = ext<PFNGLDISCARDFRAMEBUFFEREXTPROC>("glDiscardFramebufferEXT")) f(t, n, hp<const GLenum>(att));
}
void x_GetProgramBinaryOES(u32 p, i32 bs, gaddr len, gaddr fmt, gaddr bin) {
    if (auto f = ext<PFNGLGETPROGRAMBINARYOESPROC>("glGetProgramBinaryOES"))
        f(p, bs, hp<GLsizei>(len), hp<GLenum>(fmt), hp(bin));
}
void x_ProgramBinaryOES(u32 p, u32 fmt, gaddr bin, i32 len) {
    if (auto f = ext<PFNGLPROGRAMBINARYOESPROC>("glProgramBinaryOES")) f(p, fmt, hp<const void>(bin), len);
}
// The game's multisampled render targets (MSAA), created when a level loads: logged.
void x_RenderbufferStorageMultisampleEXT(u32 t, i32 s, u32 f, i32 w, i32 h) {
    auto fn = ext<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC>("glRenderbufferStorageMultisampleEXT");
    if (fn) fn(t, s, f, w, h);
    ZLOGI("MSAA buffer: %d samples, %dx%d, format 0x%x%s", s, w, h, f, fn ? "" : " (no glRenderbufferStorageMultisampleEXT)");
}
void x_FramebufferTexture2DMultisampleEXT(u32 t, u32 a, u32 tt, u32 x, i32 l, i32 s) {
    auto fn = ext<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>("glFramebufferTexture2DMultisampleEXT");
    if (fn) fn(t, a, tt, x, l, s);
    ZLOGI("MSAA render target: texture %u, %d samples, attachment 0x%x%s", x, s, a,
          fn ? "" : " (no glFramebufferTexture2DMultisampleEXT)");
}

}  // namespace

void register_egl_gles() {
    REG(eglGetDisplay, e_GetDisplay);
    REG(eglInitialize, e_Initialize);
    REG(eglTerminate, e_Terminate);
    REG(eglChooseConfig, e_ChooseConfig);
    REG(eglGetConfigs, e_GetConfigs);
    REG(eglGetConfigAttrib, e_GetConfigAttrib);
    REG(eglCreateWindowSurface, e_CreateWindowSurface);
    REG(eglCreatePbufferSurface, e_CreatePbufferSurface);
    REG(eglCreateContext, e_CreateContext);
    REG(eglDestroyContext, e_DestroyContext);
    REG(eglDestroySurface, e_DestroySurface);
    REG(eglMakeCurrent, e_MakeCurrent);
    REG(eglSwapBuffers, e_SwapBuffers);
    REG(eglSwapInterval, e_SwapInterval);
    REG(eglGetError, e_GetError);
    REG(eglQuerySurface, e_QuerySurface);
    REG(eglQueryContext, e_QueryContext);
    REG(eglQueryString, e_QueryString);
    REG(eglGetCurrentContext, e_GetCurrentContext);
    REG(eglGetCurrentDisplay, e_GetCurrentDisplay);
    REG(eglGetCurrentSurface, e_GetCurrentSurface);
    REG(eglBindAPI, e_BindAPI);
    REG(eglWaitGL, e_WaitGL);
    REG(eglGetProcAddress, e_GetProcAddress);

    REG(glActiveTexture, g_ActiveTexture);
    REG(glAttachShader, g_AttachShader);
    REG(glBindAttribLocation, g_BindAttribLocation);
    REG(glBindBuffer, g_BindBuffer);
    REG(glBindFramebuffer, g_BindFramebuffer);
    REG(glBindRenderbuffer, g_BindRenderbuffer);
    REG(glBindTexture, g_BindTexture);
    REG(glBlendColor, g_BlendColor);
    REG(glBlendEquation, g_BlendEquation);
    REG(glBlendEquationSeparate, g_BlendEquationSeparate);
    REG(glBlendFunc, g_BlendFunc);
    REG(glBlendFuncSeparate, g_BlendFuncSeparate);
    REG(glBufferData, g_BufferData);
    REG(glBufferSubData, g_BufferSubData);
    REG(glCheckFramebufferStatus, g_CheckFramebufferStatus);
    REG(glClear, g_Clear);
    REG(glClearColor, g_ClearColor);
    REG(glClearDepthf, g_ClearDepthf);
    REG(glClearStencil, g_ClearStencil);
    REG(glColorMask, g_ColorMask);
    REG(glCompileShader, g_CompileShader);
    REG(glCompressedTexImage2D, g_CompressedTexImage2D);
    REG(glCompressedTexSubImage2D, g_CompressedTexSubImage2D);
    REG(glCopyTexImage2D, g_CopyTexImage2D);
    REG(glCopyTexSubImage2D, g_CopyTexSubImage2D);
    REG(glCreateProgram, g_CreateProgram);
    REG(glCreateShader, g_CreateShader);
    REG(glCullFace, g_CullFace);
    REG(glDeleteBuffers, g_DeleteBuffers);
    REG(glDeleteFramebuffers, g_DeleteFramebuffers);
    REG(glDeleteProgram, g_DeleteProgram);
    REG(glDeleteRenderbuffers, g_DeleteRenderbuffers);
    REG(glDeleteShader, g_DeleteShader);
    REG(glDeleteTextures, g_DeleteTextures);
    REG(glDepthFunc, g_DepthFunc);
    REG(glDepthMask, g_DepthMask);
    REG(glDepthRangef, g_DepthRangef);
    REG(glDetachShader, g_DetachShader);
    REG(glDisable, g_Disable);
    REG(glDisableVertexAttribArray, g_DisableVertexAttribArray);
    REG(glDrawArrays, g_DrawArrays);
    REG(glDrawElements, g_DrawElements);
    REG(glEnable, g_Enable);
    REG(glEnableVertexAttribArray, g_EnableVertexAttribArray);
    REG(glFinish, g_Finish);
    REG(glFlush, g_Flush);
    REG(glFramebufferRenderbuffer, g_FramebufferRenderbuffer);
    REG(glFramebufferTexture2D, g_FramebufferTexture2D);
    REG(glFrontFace, g_FrontFace);
    REG(glGenBuffers, g_GenBuffers);
    REG(glGenFramebuffers, g_GenFramebuffers);
    REG(glGenRenderbuffers, g_GenRenderbuffers);
    REG(glGenTextures, g_GenTextures);
    REG(glGenerateMipmap, g_GenerateMipmap);
    REG(glGetActiveAttrib, g_GetActiveAttrib);
    REG(glGetActiveUniform, g_GetActiveUniform);
    REG(glGetAttribLocation, g_GetAttribLocation);
    REG(glGetBooleanv, g_GetBooleanv);
    REG(glGetError, g_GetError);
    REG(glGetFloatv, g_GetFloatv);
    REG(glGetIntegerv, g_GetIntegerv);
    REG(glGetProgramInfoLog, g_GetProgramInfoLog);
    REG(glGetProgramiv, g_GetProgramiv);
    REG(glGetShaderInfoLog, g_GetShaderInfoLog);
    REG(glGetShaderiv, g_GetShaderiv);
    REG(glGetString, g_GetString);
    REG(glGetUniformLocation, g_GetUniformLocation);
    REG(glHint, g_Hint);
    REG(glIsEnabled, g_IsEnabled);
    REG(glIsTexture, g_IsTexture);
    REG(glLineWidth, g_LineWidth);
    REG(glLinkProgram, g_LinkProgram);
    REG(glPixelStorei, g_PixelStorei);
    REG(glPolygonOffset, g_PolygonOffset);
    REG(glReadPixels, g_ReadPixels);
    REG(glRenderbufferStorage, g_RenderbufferStorage);
    REG(glScissor, g_Scissor);
    REG(glShaderSource, g_ShaderSource);
    REG(glStencilFunc, g_StencilFunc);
    REG(glStencilFuncSeparate, g_StencilFuncSeparate);
    REG(glStencilMask, g_StencilMask);
    REG(glStencilMaskSeparate, g_StencilMaskSeparate);
    REG(glStencilOp, g_StencilOp);
    REG(glStencilOpSeparate, g_StencilOpSeparate);
    REG(glTexImage2D, g_TexImage2D);
    REG(glTexParameterf, g_TexParameterf);
    REG(glTexParameteri, g_TexParameteri);
    REG(glTexSubImage2D, g_TexSubImage2D);
    REG(glUniform1f, g_Uniform1f);
    REG(glUniform2f, g_Uniform2f);
    REG(glUniform3f, g_Uniform3f);
    REG(glUniform4f, g_Uniform4f);
    REG(glUniform1i, g_Uniform1i);
    REG(glUniform1fv, g_Uniform1fv);
    REG(glUniform2fv, g_Uniform2fv);
    REG(glUniform3fv, g_Uniform3fv);
    REG(glUniform4fv, g_Uniform4fv);
    REG(glUniform1iv, g_Uniform1iv);
    REG(glUniformMatrix2fv, g_UniformMatrix2fv);
    REG(glUniformMatrix3fv, g_UniformMatrix3fv);
    REG(glUniformMatrix4fv, g_UniformMatrix4fv);
    REG(glUseProgram, g_UseProgram);
    REG(glValidateProgram, g_ValidateProgram);
    REG(glVertexAttrib4f, g_VertexAttrib4f);
    REG(glVertexAttribPointer, g_VertexAttribPointer);
    REG(glViewport, g_Viewport);

    REG(glGenVertexArraysOES, x_GenVertexArraysOES);
    REG(glBindVertexArrayOES, x_BindVertexArrayOES);
    REG(glDeleteVertexArraysOES, x_DeleteVertexArraysOES);
    REG(glIsVertexArrayOES, x_IsVertexArrayOES);
    REG(glDiscardFramebufferEXT, x_DiscardFramebufferEXT);
    REG(glGetProgramBinaryOES, x_GetProgramBinaryOES);
    REG(glProgramBinaryOES, x_ProgramBinaryOES);
    REG(glRenderbufferStorageMultisampleEXT, x_RenderbufferStorageMultisampleEXT);
    REG(glFramebufferTexture2DMultisampleEXT, x_FramebufferTexture2DMultisampleEXT);
}

}  // namespace zb::android
