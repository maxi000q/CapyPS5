// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef GRAPHICS_PRESENTATION_WINDOW_EGL_COMPAT_H_
#define GRAPHICS_PRESENTATION_WINDOW_EGL_COMPAT_H_

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)

#include <cstddef>
#include <cstdint>

struct ANativeWindow;
struct AHardwareBuffer;

namespace Libs::Graphics {

/**
 * v0.0.9proQ3: the OpenGL compatibility presentation backend.
 *
 * The emulator core always renders with Vulkan, but a few devices have a broken
 * Vulkan swapchain path (presenting to the Android surface) while offscreen
 * Vulkan rendering and EGL work fine. With the "Graphics API" frontend setting
 * on OpenGL, the final frame is rendered into an AHardwareBuffer-backed Vulkan
 * image and this class hands it to the screen through OpenGL ES:
 *
 *   core blit (Vulkan) -> sync fd -> [wait] -> EGLImage texture -> GLES quad
 *   -> eglSwapBuffers -> native fence fd -> reused as the next acquire wait
 *
 * Everything is loaded at runtime (dlopen libEGL + eglGetProcAddress), so no
 * build-time EGL/GLES dependency is added and the default Vulkan path is
 * untouched. All calls must come from one thread (the present thread); the
 * context is made current lazily on the first blit.
 */
class GlCompatRenderer {
public:
        GlCompatRenderer() = default;
        ~GlCompatRenderer() { Destroy(); }
        GlCompatRenderer(const GlCompatRenderer&)            = delete;
        GlCompatRenderer& operator=(const GlCompatRenderer&) = delete;

        /**
         * Query the current Android window and create the EGL display, config,
         * context and window surface. Safe to call again after Destroy() (a
         * window resize recreates the surface). Returns false - with everything
         * torn back down - when any step fails, in which case the caller falls
         * back to the Vulkan swapchain.
         */
        [[nodiscard]] bool Initialize(uint32_t width, uint32_t height);

        /**
         * Wait for the Vulkan blit (acquire_fence_fd, -1 = none), sample the
         * AHardwareBuffer as a texture, draw it to the window and swap.
         * Returns a native fence fd that signals once OpenGL has finished
         * reading the buffer (the caller waits it before reusing the image),
         * or -1 when the swap did not happen (caller recreates).
         * The acquire fd is consumed (closed) either way.
         */
        int Blit(AHardwareBuffer* buffer, int acquire_fence_fd);

        /** Current window-surface size, for the resize check. */
        [[nodiscard]] bool QuerySize(uint32_t* width, uint32_t* height) const;

        /** Swap interval: 1 blocks to vblank (FIFO), 0 presents as fast as possible. */
        void SetSwapInterval(int interval);

        /** True between a successful Initialize and Destroy. */
        [[nodiscard]] bool IsReady() const { return m_ready; }

        /**
         * Release the EGL resources. Only the thread that made the context
         * current can do this properly; called from another thread the
         * resources are left for process exit (logged).
         */
        void Destroy();

private:
        bool LoadEgl();
        bool MakeCurrentOnce();
        bool EnsureProgram();
        void DestroyProgram();
        void DestroySurface();

        ANativeWindow* m_window_ref = nullptr; // our strong reference
        void*          m_display    = nullptr; // EGLDisplay
        void*          m_context    = nullptr; // EGLContext
        void*          m_surface    = nullptr; // EGLSurface
        void*          m_config     = nullptr; // EGLConfig
        uint32_t       m_width      = 0;
        uint32_t       m_height     = 0;
        bool           m_ready      = false;
        bool           m_context_current = false;
        long           m_owner_thread    = 0; // hash of std::thread::id of MakeCurrent

        // GLES objects (plain handles; the loader owns the entry points).
        unsigned m_program            = 0;
        int      m_position_location  = -1;
        int      m_texture_location   = -1;
        unsigned m_texture            = 0;
        // One EGLImage per AHardwareBuffer (the ring has three buffers).
        static constexpr size_t kImageCacheMax = 8;
        struct ImageCacheEntry {
                AHardwareBuffer* buffer = nullptr;
                void*            image  = nullptr; // EGLImageKHR
        };
        ImageCacheEntry m_image_cache[kImageCacheMax];
        size_t          m_image_cache_count = 0;
};

} // namespace Libs::Graphics

#endif // Android

#endif // GRAPHICS_PRESENTATION_WINDOW_EGL_COMPAT_H_
