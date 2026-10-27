// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// The OpenGL compatibility presentation backend (see eglCompat.h). Android
// only; other platforms compile this file empty.

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)

#include "graphics/presentation/window/eglCompat.h"

#include "android/capyps5_jni.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/sync.h>
#include <dlfcn.h>
#include <functional>
#include <thread>
#include <unistd.h>

#include <cstring>

namespace Libs::Graphics {

// ---------------------------------------------------------------------------
// Runtime loader: the EGL core from libEGL.so, everything else through
// eglGetProcAddress. No build-time link against EGL or GLES is added.
// ---------------------------------------------------------------------------
namespace {

struct EglFunctions {
        bool loaded = false;

        EGLDisplay (*eglGetDisplay)(NativeDisplayType) = nullptr;
        EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
        EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
        EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, NativeWindowType, const EGLint*) = nullptr;
        EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
        EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
        EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
        EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface) = nullptr;
        EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
        EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint) = nullptr;
        EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = nullptr;
        const char* (*eglQueryString)(EGLDisplay, EGLint) = nullptr;
        EGLint (*eglGetError)() = nullptr;
        void* (*eglGetProcAddress)(const char*) = nullptr;

        EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) = nullptr;
        EGLBoolean (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;
        EGLSyncKHR (*eglCreateSyncKHR)(EGLDisplay, EGLenum, const EGLint*) = nullptr;
        EGLBoolean (*eglDestroySyncKHR)(EGLDisplay, EGLSyncKHR) = nullptr;
        EGLint (*eglDupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR) = nullptr;

        // GLES entry points.
        void (*glActiveTexture)(GLenum) = nullptr;
        void (*glAttachShader)(GLuint, GLuint) = nullptr;
        void (*glBindTexture)(GLenum, GLuint) = nullptr;
        void (*glCompileShader)(GLuint) = nullptr;
        GLuint (*glCreateProgram)() = nullptr;
        GLuint (*glCreateShader)(GLenum) = nullptr;
        void (*glDeleteProgram)(GLuint) = nullptr;
        void (*glDeleteShader)(GLuint) = nullptr;
        void (*glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
        void (*glEnableVertexAttribArray)(GLuint) = nullptr;
        GLint (*glGetAttribLocation)(GLuint, const char*) = nullptr;
        GLenum (*glGetError)() = nullptr;
        void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
        void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
        GLint (*glGetUniformLocation)(GLuint, const char*) = nullptr;
        void (*glLinkProgram)(GLuint) = nullptr;
        void (*glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*) = nullptr;
        void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
        void (*glUniform1i)(GLint, GLint) = nullptr;
        void (*glUseProgram)(GLuint) = nullptr;
        void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
        void (*glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
        void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
        void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
        void (*glEGLImageTargetTexture2DOES)(GLenum, GLeglImageOES) = nullptr;
};

EglFunctions g_egl;

bool HasEglExtension(const char* extensions, const char* name) {
        if (extensions == nullptr) {
                return false;
        }
        const auto len = std::strlen(name);
        const char* pos = extensions;
        while ((pos = std::strstr(pos, name)) != nullptr) {
                const bool left_ok  = pos == extensions || pos[-1] == ' ';
                const bool right_ok = pos[len] == '\0' || pos[len] == ' ';
                if (left_ok && right_ok) {
                        return true;
                }
                pos += len;
        }
        return false;
}

} // namespace

bool GlCompatRenderer::LoadEgl() {
        if (g_egl.loaded) {
                return true;
        }
        auto* lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
                LOGF("eglCompat: cannot dlopen libEGL.so: %s\n", dlerror());
                return false;
        }
        g_egl.eglGetDisplay = reinterpret_cast<EGLDisplay (*)(NativeDisplayType)>(
                dlsym(lib, "eglGetDisplay"));
        g_egl.eglGetProcAddress =
                reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "eglGetProcAddress"));
        if (g_egl.eglGetDisplay == nullptr || g_egl.eglGetProcAddress == nullptr) {
                LOGF("eglCompat: libEGL.so lacks the core entry points\n");
                return false;
        }
        bool ok = true;
        const auto resolve = [&](void** target, const char* name) {
                *target = g_egl.eglGetProcAddress(name);
                if (*target == nullptr) {
                        LOGF("eglCompat: %s is missing\n", name);
                        ok = false;
                }
        };
        resolve(reinterpret_cast<void**>(&g_egl.eglInitialize), "eglInitialize");
        resolve(reinterpret_cast<void**>(&g_egl.eglChooseConfig), "eglChooseConfig");
        resolve(reinterpret_cast<void**>(&g_egl.eglCreateWindowSurface), "eglCreateWindowSurface");
        resolve(reinterpret_cast<void**>(&g_egl.eglMakeCurrent), "eglMakeCurrent");
        resolve(reinterpret_cast<void**>(&g_egl.eglCreateContext), "eglCreateContext");
        resolve(reinterpret_cast<void**>(&g_egl.eglDestroyContext), "eglDestroyContext");
        resolve(reinterpret_cast<void**>(&g_egl.eglDestroySurface), "eglDestroySurface");
        resolve(reinterpret_cast<void**>(&g_egl.eglSwapBuffers), "eglSwapBuffers");
        resolve(reinterpret_cast<void**>(&g_egl.eglSwapInterval), "eglSwapInterval");
        resolve(reinterpret_cast<void**>(&g_egl.eglQuerySurface), "eglQuerySurface");
        resolve(reinterpret_cast<void**>(&g_egl.eglQueryString), "eglQueryString");
        resolve(reinterpret_cast<void**>(&g_egl.eglGetError), "eglGetError");
        resolve(reinterpret_cast<void**>(&g_egl.eglCreateImageKHR), "eglCreateImageKHR");
        resolve(reinterpret_cast<void**>(&g_egl.eglDestroyImageKHR), "eglDestroyImageKHR");
        resolve(reinterpret_cast<void**>(&g_egl.eglCreateSyncKHR), "eglCreateSyncKHR");
        resolve(reinterpret_cast<void**>(&g_egl.eglDestroySyncKHR), "eglDestroySyncKHR");
        resolve(reinterpret_cast<void**>(&g_egl.eglDupNativeFenceFDANDROID), "eglDupNativeFenceFDANDROID");
        resolve(reinterpret_cast<void**>(&g_egl.glActiveTexture), "glActiveTexture");
        resolve(reinterpret_cast<void**>(&g_egl.glAttachShader), "glAttachShader");
        resolve(reinterpret_cast<void**>(&g_egl.glBindTexture), "glBindTexture");
        resolve(reinterpret_cast<void**>(&g_egl.glCompileShader), "glCompileShader");
        resolve(reinterpret_cast<void**>(&g_egl.glCreateProgram), "glCreateProgram");
        resolve(reinterpret_cast<void**>(&g_egl.glCreateShader), "glCreateShader");
        resolve(reinterpret_cast<void**>(&g_egl.glDeleteProgram), "glDeleteProgram");
        resolve(reinterpret_cast<void**>(&g_egl.glDeleteShader), "glDeleteShader");
        resolve(reinterpret_cast<void**>(&g_egl.glDrawArrays), "glDrawArrays");
        resolve(reinterpret_cast<void**>(&g_egl.glEnableVertexAttribArray), "glEnableVertexAttribArray");
        resolve(reinterpret_cast<void**>(&g_egl.glGetAttribLocation), "glGetAttribLocation");
        resolve(reinterpret_cast<void**>(&g_egl.glGetError), "glGetError");
        resolve(reinterpret_cast<void**>(&g_egl.glGetProgramiv), "glGetProgramiv");
        resolve(reinterpret_cast<void**>(&g_egl.glGetShaderiv), "glGetShaderiv");
        resolve(reinterpret_cast<void**>(&g_egl.glGetUniformLocation), "glGetUniformLocation");
        resolve(reinterpret_cast<void**>(&g_egl.glLinkProgram), "glLinkProgram");
        resolve(reinterpret_cast<void**>(&g_egl.glShaderSource), "glShaderSource");
        resolve(reinterpret_cast<void**>(&g_egl.glTexParameteri), "glTexParameteri");
        resolve(reinterpret_cast<void**>(&g_egl.glUniform1i), "glUniform1i");
        resolve(reinterpret_cast<void**>(&g_egl.glUseProgram), "glUseProgram");
        resolve(reinterpret_cast<void**>(&g_egl.glVertexAttribPointer), "glVertexAttribPointer");
        resolve(reinterpret_cast<void**>(&g_egl.glViewport), "glViewport");
        resolve(reinterpret_cast<void**>(&g_egl.glGenTextures), "glGenTextures");
        resolve(reinterpret_cast<void**>(&g_egl.glDeleteTextures), "glDeleteTextures");
        resolve(reinterpret_cast<void**>(&g_egl.glEGLImageTargetTexture2DOES), "glEGLImageTargetTexture2DOES");
        if (!ok) {
                return false;
        }
        g_egl.loaded = true;
        return true;
}

bool GlCompatRenderer::Initialize(uint32_t width, uint32_t height) {
        Destroy();
        if (!LoadEgl()) {
                return false;
        }

        auto* native_window = AndroidGetNativeWindow();
        if (native_window == nullptr) {
                LOGF("eglCompat: no Android native window yet\n");
                return false;
        }
        ANativeWindow_acquire(native_window);
        m_window_ref = native_window;
        m_width      = width;
        m_height     = height;

        auto display = g_egl.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
                LOGF("eglCompat: eglGetDisplay failed\n");
                Destroy();
                return false;
        }
        m_display = display;
        if (!g_egl.eglInitialize(display, nullptr, nullptr)) {
                LOGF("eglCompat: eglInitialize failed 0x%x\n", g_egl.eglGetError());
                Destroy();
                return false;
        }
        const auto* display_extensions = g_egl.eglQueryString(display, EGL_EXTENSIONS);
        const bool  has_image          = HasEglExtension(display_extensions, "EGL_ANDROID_image_native_buffer");
        const bool  has_fence          = HasEglExtension(display_extensions, "EGL_ANDROID_native_fence_sync");
        if (!has_image || !has_fence) {
                LOGF("eglCompat: required EGL extensions missing (image_native_buffer=%d native_fence=%d)\n",
                     has_image ? 1 : 0, has_fence ? 1 : 0);
                Destroy();
                return false;
        }

        const EGLint config_attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                EGL_RED_SIZE,        8,
                EGL_GREEN_SIZE,      8,
                EGL_BLUE_SIZE,       8,
                EGL_ALPHA_SIZE,      8,
                EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint    matched = 0;
        if (!g_egl.eglChooseConfig(display, config_attribs, &config, 1, &matched) || matched < 1 ||
            config == nullptr) {
                LOGF("eglCompat: eglChooseConfig failed 0x%x\n", g_egl.eglGetError());
                Destroy();
                return false;
        }
        m_config = config;

        auto surface = g_egl.eglCreateWindowSurface(display, config,
                                                    static_cast<NativeWindowType>(native_window), nullptr);
        if (surface == EGL_NO_SURFACE) {
                LOGF("eglCompat: eglCreateWindowSurface failed 0x%x\n", g_egl.eglGetError());
                Destroy();
                return false;
        }
        m_surface = surface;

        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        auto context = g_egl.eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
        if (context == EGL_NO_CONTEXT) {
                LOGF("eglCompat: eglCreateContext failed 0x%x\n", g_egl.eglGetError());
                Destroy();
                return false;
        }
        m_context = context;

        // Swap interval follows the present-mode setting: FIFO blocks to vblank,
        // the low-latency/uncapped modes present immediately (the emulated
        // vblank governor still paces the flip thread).
        int interval = 1;
        switch (Config::GetPresentMode()) {
                case Config::PresentMode::Fifo: interval = 1; break;
                case Config::PresentMode::Mailbox:
                case Config::PresentMode::Immediate:
                default: interval = 0; break;
        }
        SetSwapInterval(interval);

        m_ready = true;
        LOGF("eglCompat: OpenGL compatibility present path ready (%ux%u)\n", width, height);
        return true;
}

bool GlCompatRenderer::MakeCurrentOnce() {
        if (m_context_current) {
                return true;
        }
        if (m_display == nullptr || m_surface == nullptr || m_context == nullptr) {
                return false;
        }
        if (!g_egl.eglMakeCurrent(static_cast<EGLDisplay>(m_display), static_cast<EGLSurface>(m_surface),
                                  static_cast<EGLSurface>(m_surface), static_cast<EGLContext>(m_context))) {
                LOGF("eglCompat: eglMakeCurrent failed 0x%x\n", g_egl.eglGetError());
                return false;
        }
        m_context_current = true;
        m_owner_thread    = static_cast<long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return true;
}

bool GlCompatRenderer::EnsureProgram() {
        if (m_program != 0) {
                return true;
        }
        static const char* kVertexSource =
                "attribute vec2 a_position;\n"
                "varying vec2 v_uv;\n"
                "void main() {\n"
                "    v_uv = a_position * 0.5 + 0.5;\n"
                "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                "}\n";
        static const char* kFragmentSource =
                "precision mediump float;\n"
                "varying vec2 v_uv;\n"
                "uniform sampler2D u_texture;\n"
                "void main() {\n"
                "    gl_FragColor = texture2D(u_texture, v_uv);\n"
                "}\n";
        const auto compile = [&](GLenum type, const char* source) -> GLuint {
                const auto shader = g_egl.glCreateShader(type);
                if (shader == 0) {
                        return 0;
                }
                g_egl.glShaderSource(shader, 1, &source, nullptr);
                g_egl.glCompileShader(shader);
                GLint compiled = 0;
                g_egl.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (!compiled) {
                        g_egl.glDeleteShader(shader);
                        return 0;
                }
                return shader;
        };
        const auto vertex   = compile(GL_VERTEX_SHADER, kVertexSource);
        const auto fragment = compile(GL_FRAGMENT_SHADER, kFragmentSource);
        if (vertex == 0 || fragment == 0) {
                if (vertex != 0) {
                        g_egl.glDeleteShader(vertex);
                }
                if (fragment != 0) {
                        g_egl.glDeleteShader(fragment);
                }
                LOGF("eglCompat: blit shader compile failed 0x%x\n", g_egl.glGetError());
                return false;
        }
        m_program = g_egl.glCreateProgram();
        g_egl.glAttachShader(m_program, vertex);
        g_egl.glAttachShader(m_program, fragment);
        g_egl.glLinkProgram(m_program);
        GLint linked = 0;
        g_egl.glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
        g_egl.glDeleteShader(vertex);
        g_egl.glDeleteShader(fragment);
        if (!linked || m_program == 0) {
                LOGF("eglCompat: blit program link failed\n");
                g_egl.glDeleteProgram(m_program);
                m_program = 0;
                return false;
        }
        m_position_location = g_egl.glGetAttribLocation(m_program, "a_position");
        m_texture_location  = g_egl.glGetUniformLocation(m_program, "u_texture");
        g_egl.glGenTextures(1, &m_texture);
        if (m_position_location < 0 || m_texture == 0) {
                return false;
        }
        return true;
}

int GlCompatRenderer::Blit(AHardwareBuffer* buffer, int acquire_fence_fd) {
        if (buffer == nullptr || !m_ready) {
                if (acquire_fence_fd >= 0) {
                        close(acquire_fence_fd);
                }
                return -1;
        }
        // The Vulkan blit must finish before GL reads the buffer.
        if (acquire_fence_fd >= 0) {
                if (sync_wait(acquire_fence_fd, 1000) != 0) {
                        LOGF("eglCompat: acquire fence wait timed out\n");
                }
                close(acquire_fence_fd);
        }
        if (!MakeCurrentOnce() || !EnsureProgram()) {
                return -1;
        }
        const auto display = static_cast<EGLDisplay>(m_display);

        // Find (or create) the EGLImage for this buffer.
        void* image = nullptr;
        for (size_t i = 0; i < m_image_cache_count; i++) {
                if (m_image_cache[i].buffer == buffer) {
                        image = m_image_cache[i].image;
                        break;
                }
        }
        if (image == nullptr) {
                image = g_egl.eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                                static_cast<EGLClientBuffer>(buffer), nullptr);
                if (image == nullptr) {
                        LOGF("eglCompat: eglCreateImageKHR failed 0x%x\n", g_egl.eglGetError());
                        return -1;
                }
                if (m_image_cache_count < kImageCacheMax) {
                        m_image_cache[m_image_cache_count] = {buffer, image};
                        m_image_cache_count++;
                }
        }

        // The frame bytes are already sRGB-encoded; the texture is sampled raw
        // (UNORM) and written raw, so the screen shows the same pixels as the
        // Vulkan swapchain path.
        g_egl.glActiveTexture(GL_TEXTURE0);
        g_egl.glBindTexture(GL_TEXTURE_2D, m_texture);
        g_egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
        g_egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g_egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g_egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        static const float kQuad[] = {
                -1.0f, -1.0f, // bottom-left  -> uv (0, 0)
                 1.0f, -1.0f, // bottom-right -> uv (1, 0)
                -1.0f,  1.0f, // top-left     -> uv (0, 1)
                 1.0f,  1.0f, // top-right    -> uv (1, 1)
        };
        g_egl.glViewport(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height));
        g_egl.glUseProgram(m_program);
        g_egl.glUniform1i(m_texture_location, 0);
        g_egl.glEnableVertexAttribArray(static_cast<GLuint>(m_position_location));
        g_egl.glVertexAttribPointer(static_cast<GLuint>(m_position_location), 2, GL_FLOAT, GL_FALSE, 0,
                                    kQuad);
        g_egl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (!g_egl.eglSwapBuffers(display, static_cast<EGLSurface>(m_surface))) {
                LOGF("eglCompat: eglSwapBuffers failed 0x%x\n", g_egl.eglGetError());
                return -1;
        }

        // A native fence that signals when GL has finished reading the buffer.
        auto* sync = g_egl.eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync == nullptr) {
                return -1;
        }
        const auto fd = g_egl.eglDupNativeFenceFDANDROID(display, sync);
        g_egl.eglDestroySyncKHR(display, sync);
        return fd;
}

bool GlCompatRenderer::QuerySize(uint32_t* width, uint32_t* height) const {
        if (m_display == nullptr || m_surface == nullptr || width == nullptr || height == nullptr) {
                return false;
        }
        EGLint w = 0;
        EGLint h = 0;
        if (!g_egl.eglQuerySurface(static_cast<EGLDisplay>(m_display),
                                   static_cast<EGLSurface>(m_surface), EGL_WIDTH, &w) ||
            !g_egl.eglQuerySurface(static_cast<EGLDisplay>(m_display),
                                   static_cast<EGLSurface>(m_surface), EGL_HEIGHT, &h)) {
                return false;
        }
        *width  = static_cast<uint32_t>(w);
        *height = static_cast<uint32_t>(h);
        return true;
}

void GlCompatRenderer::SetSwapInterval(int interval) {
        if (m_display != nullptr && m_surface != nullptr) {
                g_egl.eglSwapInterval(static_cast<EGLDisplay>(m_display), interval);
        }
}

void GlCompatRenderer::DestroyProgram() {
        if (!m_context_current) {
                m_program = 0;
                m_texture = 0;
                return;
        }
        if (m_texture != 0) {
                g_egl.glDeleteTextures(1, &m_texture);
                m_texture = 0;
        }
        if (m_program != 0) {
                g_egl.glDeleteProgram(m_program);
                m_program = 0;
        }
}

void GlCompatRenderer::DestroySurface() {
        if (!g_egl.loaded || m_display == nullptr) {
                m_surface = nullptr;
                return;
        }
        for (size_t i = 0; i < m_image_cache_count; i++) {
                if (m_image_cache[i].image != nullptr) {
                        g_egl.eglDestroyImageKHR(static_cast<EGLDisplay>(m_display),
                                                 static_cast<EGLImageKHR>(m_image_cache[i].image));
                }
                m_image_cache[i] = {};
        }
        m_image_cache_count = 0;
        if (m_surface == nullptr) {
                return;
        }
        if (!m_context_current) {
                g_egl.eglDestroySurface(static_cast<EGLDisplay>(m_display),
                                        static_cast<EGLSurface>(m_surface));
                m_surface = nullptr;
        } else if (g_egl.eglMakeCurrent(static_cast<EGLDisplay>(m_display), EGL_NO_SURFACE,
                                        EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
                m_context_current = false;
                g_egl.eglDestroySurface(static_cast<EGLDisplay>(m_display),
                                        static_cast<EGLSurface>(m_surface));
                m_surface = nullptr;
        } else {
                LOGF("eglCompat: surface leak: context still current on another thread\n");
        }
}

void GlCompatRenderer::Destroy() {
        if (!m_ready && m_window_ref == nullptr && m_display == nullptr) {
                return;
        }
        if (!g_egl.loaded) {
                m_window_ref = nullptr;
                m_display    = nullptr;
                m_surface    = nullptr;
                m_context    = nullptr;
                m_ready      = false;
                return;
        }
        const auto current_thread =
                static_cast<long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        if (m_context_current && current_thread != m_owner_thread) {
                // Only the owning thread can release the context; the game is
                // shutting down, so the process reclaims everything.
                LOGF("eglCompat: Destroy on a foreign thread; EGL resources left to process exit\n");
                m_ready = false;
                return;
        }
        DestroyProgram();
        DestroySurface();
        if (m_context != nullptr) {
                g_egl.eglDestroyContext(static_cast<EGLDisplay>(m_display),
                                        static_cast<EGLContext>(m_context));
                m_context         = nullptr;
                m_context_current = false;
        }
        // The display is shared with the app UI (the launcher probes it too);
        // never eglTerminate it - destroying our surface and context is enough.
        if (m_window_ref != nullptr) {
                ANativeWindow_release(static_cast<ANativeWindow*>(m_window_ref));
                m_window_ref = nullptr;
        }
        m_ready = false;
}

} // namespace Libs::Graphics

#endif // Android
