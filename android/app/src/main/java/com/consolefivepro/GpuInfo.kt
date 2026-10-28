// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.util.Log

/**
 * The device GPU, from the system OpenGL ES driver (v0.0.9pro). A tiny off-screen context is
 * created once to read GL_RENDERER; the game itself may use a custom Vulkan driver (Turnip,
 * PanVK), but the launcher always talks to the system driver, which names the real hardware
 * (e.g. "Mali-G76 MC4", "Adreno (TM) 660").
 */
object GpuInfo {
    private const val TAG = "CapyPS5-Gpu"

    val renderer: String by lazy { probe() }

    /** Mali GPUs (Mali-G52, G76, G710, ...) get the v0.0.9pro Mali performance profile. */
    val isMali: Boolean by lazy { renderer.contains("mali", ignoreCase = true) }

    /** Adreno GPUs (Snapdragon) get the v0.0.10pro Adreno performance profile. */
    val isAdreno: Boolean by lazy { renderer.contains("adreno", ignoreCase = true) }

    /** The Adreno model number ("Adreno (TM) 740" -> 740), or null on anything else. */
    val adrenoModel: Int? by lazy {
        if (!isAdreno) return@lazy null
        Regex("Adreno[^0-9]*(\\d{3})", RegexOption.IGNORE_CASE).find(renderer)
            ?.groupValues?.getOrNull(1)?.toIntOrNull()
    }

    /**
     * Whether the GPU has headroom for the frame generator's extra per-tick work. The
     * generator composites an extra frame into every idle display tick; on a GPU that is
     * already near saturation that work eats the headroom it was meant to spend. The
     * cut is deliberate: every Mali generation (bandwidth-bound UMA parts) and every
     * Adreno below the 7xx series (730 = the first Snapdragons with real headroom over
     * PS5-class emulation load). Everything unrecognized is treated as NOT capable - a
     * wrong default that quietly keeps the raw cadence is harmless; one that stutters a
     * borderline device is not. The user's own toggle always wins over this default.
     */
    val frameGenerationCapable: Boolean by lazy {
        when {
            isMali -> false
            isAdreno -> (adrenoModel ?: 0) >= 700
            else -> false
        }
    }

    /** Short label for the settings rows; falls back when the probe found nothing. */
    val label: String get() = renderer.ifBlank { "GPU" }

    private fun probe(): String = runCatching {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display === EGL14.EGL_NO_DISPLAY) return@runCatching ""
        if (!EGL14.eglInitialize(display, IntArray(2), 0, IntArray(2), 0)) return@runCatching ""

        val attribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val matched = IntArray(1)
        if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, matched, 0) || matched[0] < 1) {
            return@runCatching ""
        }
        val surfaceAttribs = intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE)
        val surface: EGLSurface = EGL14.eglCreatePbufferSurface(display, configs[0], surfaceAttribs, 0)
        if (surface === EGL14.EGL_NO_SURFACE) return@runCatching ""
        val context: EGLContext = EGL14.eglCreateContext(
            display, configs[0], EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
        )
        if (context === EGL14.EGL_NO_CONTEXT) {
            EGL14.eglDestroySurface(display, surface)
            return@runCatching ""
        }
        try {
            if (!EGL14.eglMakeCurrent(display, surface, surface, context)) return@runCatching ""
            GLES20.glGetString(GLES20.GL_RENDERER)?.trim().orEmpty()
        } finally {
            // Only release what we created. eglTerminate on the shared default display would
            // invalidate the UI's own EGL resources.
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroyContext(display, context)
            EGL14.eglDestroySurface(display, surface)
        }
    }.onFailure { Log.w(TAG, "GPU probe failed; the Mali profile falls back to the core's own detection", it) }
        .getOrDefault("")
}
