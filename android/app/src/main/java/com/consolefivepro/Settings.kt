// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.content.Context
import android.os.Environment
import androidx.annotation.StringRes
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

enum class OverlayMode(@StringRes val labelRes: Int) {
    Off(R.string.overlay_off), Compact(R.string.overlay_compact), Full(R.string.overlay_full);

    fun label(context: Context) = context.getString(labelRes)
}

enum class TouchControlsMode(@StringRes val labelRes: Int) {
    Auto(R.string.mode_auto), Always(R.string.mode_always), Never(R.string.mode_never);

    fun label(context: Context) = context.getString(labelRes)
}

/** Face buttons of a physical pad. Nintendo-labelled pads (AYN Odin) report A/B and X/Y by label. */
enum class ButtonLayout(@StringRes val labelRes: Int) {
    Standard(R.string.layout_standard), Nintendo(R.string.layout_nintendo);

    fun label(context: Context) = context.getString(labelRes)
}

/** Shape of the game image on screen; anything but Default letterboxes with black bars. */
enum class ScreenAspect(@StringRes val labelRes: Int) {
    Default(R.string.aspect_default), R16_9(R.string.aspect_16_9), R5_4(R.string.aspect_5_4);

    fun label(context: Context) = context.getString(labelRes)
}

/** One on-screen control cluster: its centre as a fraction of the overlay, and its size. */
data class ControlCluster(
    val x: Float,
    val y: Float,
    val scale: Float = 1f,
) {
    fun clamped(): ControlCluster = ControlCluster(
        x = x.coerceIn(0.02f, 0.98f),
        y = y.coerceIn(0.02f, 0.98f),
        scale = scale.coerceIn(0.6f, 1.8f),
    )
}

/**
 * Layout of the on-screen pad (v0.0.10pro phase 3): three clusters (left pad side, right pad
 * side, top bar) the user can move and resize in the controls editor. Defaults reproduce the
 * classic fixed layout on a phone screen; positions are stored as screen fractions so the
 * layout survives rotation and works on any display.
 */
data class ControlsLayout(
    val left: ControlCluster = ControlCluster(0.19f, 0.70f),
    val right: ControlCluster = ControlCluster(0.81f, 0.70f),
    val top: ControlCluster = ControlCluster(0.5f, 0.08f),
) {
    fun cluster(index: Int): ControlCluster = when (index) {
        0 -> left
        1 -> right
        else -> top
    }

    fun withCluster(index: Int, value: ControlCluster): ControlsLayout = when (index) {
        0 -> copy(left = value)
        1 -> copy(right = value)
        else -> copy(top = value)
    }

    /** Drag delta in screen fractions applied to one cluster, clamped on all axes. */
    fun moved(index: Int, dx: Float, dy: Float): ControlsLayout =
        withCluster(index, cluster(index).let { ControlCluster(it.x + dx, it.y + dy, it.scale) }.clamped())

    /** Size step applied to one cluster, clamped to the editor bounds. */
    fun scaled(index: Int, amount: Float): ControlsLayout =
        withCluster(index, cluster(index).let { ControlCluster(it.x, it.y, it.scale + amount) }.clamped())

    fun clamped(): ControlsLayout = ControlsLayout(left.clamped(), right.clamped(), top.clamped())

    fun toJson(): String {
        fun clusterJson(c: ControlCluster) =
            org.json.JSONArray().put(c.x.toDouble()).put(c.y.toDouble()).put(c.scale.toDouble())
        return org.json.JSONObject()
            .put("left", clusterJson(left))
            .put("right", clusterJson(right))
            .put("top", clusterJson(top))
            .toString()
    }

    companion object {
        /** Reads a layout written by [toJson]; null when the text is absent or malformed. */
        fun fromJson(text: String?): ControlsLayout? = runCatching {
            if (text.isNullOrBlank()) return@runCatching null
            val root = org.json.JSONObject(text)
            fun cluster(name: String): ControlCluster {
                val values = root.getJSONArray(name)
                return ControlCluster(values.getDouble(0).toFloat(), values.getDouble(1).toFloat(), values.getDouble(2).toFloat())
            }
            ControlsLayout(cluster("left"), cluster("right"), cluster("top")).clamped()
        }.getOrNull()
    }
}

/**
 * Emulator core option, passed to the core as its debug.capy.* property before a game starts.
 * Each has a global value and an optional per-game override.
 */
enum class CoreOption(
    @StringRes val titleRes: Int,
    @StringRes val descriptionRes: Int,
    val property: String,
    val choices: List<Pair<Long, Int>>,
    val default: Long,
) {
    /**
     * v0.0.9proQ3: presentation backend. Vulkan renders and presents directly (the default
     * and fastest path); OpenGL ES hands the final frame to the screen through EGL - a
     * compatibility path for devices whose Vulkan present path is broken. The core falls
     * back to Vulkan automatically when the device lacks AHardwareBuffer import.
     */
    GraphicsBackend(
        R.string.option_graphics_backend,
        R.string.option_graphics_backend_desc,
        "debug.capy.gl_backend",
        listOf(
            0L to R.string.backend_vulkan,
            1L to R.string.backend_opengl,
        ),
        0L,
    ),
    /**
     * v0.0.10pro phase 3 (Mali only, shown when the backend is Vulkan): the API version of the
     * session's Vulkan instance. 1.3 is the default; 1.1 asks the core to create a 1.1 instance
     * and enable the functionality Vulkan 1.2/1.3 promoted from extensions through those
     * extensions - for Mali drivers whose 1.3 path misbehaves or that implement the features
     * only that way.
     */
    VulkanVersion(
        R.string.option_vulkan_version,
        R.string.option_vulkan_version_desc,
        "debug.capy.vk_api",
        listOf(
            3L to R.string.vulkan_version_1_3,
            1L to R.string.vulkan_version_1_1,
        ),
        3L,
    ),
    ResolutionScale(
        R.string.option_resolution,
        R.string.option_resolution_desc,
        "debug.capy.res_scale",
        listOf(
            1000L to R.string.resolution_native,
            750L to R.string.resolution_75,
            500L to R.string.resolution_50,
            250L to R.string.resolution_25,
        ),
        1000L,
    ),
    AsyncShaders(
        R.string.option_async_shaders,
        R.string.option_async_shaders_desc,
        "debug.capy.async_pipelines",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        1L,
    ),
    StreamingBuffer(
        R.string.option_stream_mb,
        R.string.option_stream_mb_desc,
        "debug.capy.stream_mb",
        listOf(
            64L to R.string.stream_mb_64,
            96L to R.string.stream_mb_96,
            128L to R.string.stream_mb_128,
            192L to R.string.stream_mb_192,
            256L to R.string.stream_mb_256,
        ),
        64L,
    ),
    ReadAhead(
        R.string.option_read_ahead,
        R.string.option_read_ahead_desc,
        "debug.capy.read_ahead_kb",
        listOf(
            64L to R.string.read_ahead_64,
            128L to R.string.read_ahead_128,
            256L to R.string.read_ahead_256,
            512L to R.string.read_ahead_512,
            1024L to R.string.read_ahead_1024,
        ),
        256L,
    ),
    YieldSleep(
        R.string.option_yield_us,
        R.string.option_yield_us_desc,
        "debug.capy.yield_us",
        listOf(
            0L to R.string.yield_us_0,
            100L to R.string.yield_us_100,
            250L to R.string.yield_us_250,
            500L to R.string.yield_us_500,
            1000L to R.string.yield_us_1000,
        ),
        0L,
    ),
    VerboseLogging(
        R.string.option_verbose,
        R.string.option_verbose_desc,
        "debug.capy.verbose",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        0L,
    ),
    FenceFallback(
        R.string.option_fence_fallback,
        R.string.option_fence_fallback_desc,
        "debug.capy.eop_flush",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        0L,
    ),
    GuestReadCache(
        R.string.option_guest_read_cache,
        R.string.option_guest_read_cache_desc,
        "debug.capy.guest_read_cache",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        1L,
    ),
    PerfCores(
        R.string.option_perf_cores,
        R.string.option_perf_cores_desc,
        "debug.capy.perf_cores",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        1L,
    ),
    PresentMode(
        R.string.option_present_mode,
        R.string.option_present_mode_desc,
        "debug.capy.present_mode",
        listOf(
            0L to R.string.present_mode_low,
            1L to R.string.present_mode_vsync,
            2L to R.string.present_mode_uncapped,
        ),
        0L,
    ),
    MaliProfile(
        R.string.option_mali_profile,
        R.string.option_mali_profile_desc,
        "debug.capy.mali_profile",
        listOf(
            2L to R.string.mali_profile_auto,
            1L to R.string.mali_profile_on,
            0L to R.string.mali_profile_off,
        ),
        2L,
    ),
    AdrenoProfile(
        R.string.option_adreno_profile,
        R.string.option_adreno_profile_desc,
        "debug.capy.adreno_profile",
        listOf(
            2L to R.string.adreno_profile_auto,
            1L to R.string.adreno_profile_on,
            0L to R.string.adreno_profile_off,
        ),
        2L,
    ),
    RefreshRate(
        R.string.option_refresh_rate,
        R.string.option_refresh_rate_desc,
        "debug.capy.refresh_hz",
        listOf(
            0L to R.string.refresh_auto,
            60L to R.string.refresh_60,
            90L to R.string.refresh_90,
            120L to R.string.refresh_120,
        ),
        0L,
    ),
    /**
     * v0.0.10pro phase 5: the frame generator. Fills idle display ticks with a generated
     * in-between frame (the newest frame's motion extended between the two real frames),
     * so the presented cadence reaches the display rate without touching the emulation
     * path - real frames still present the moment they finish, input latency is unchanged.
     * Default is GPU-gated (see gpuAwareDefault): on where the GPU has headroom for the
     * extra per-tick composite, off on bandwidth-bound parts. The toggle always wins.
     */
    FrameGeneration(
        R.string.option_frame_generation,
        R.string.option_frame_generation_desc,
        "debug.capy.frame_gen",
        listOf(1L to R.string.enabled, 0L to R.string.disabled),
        1L,
    ),
    ;

    fun title(context: Context) = context.getString(titleRes)

    fun description(context: Context) = context.getString(descriptionRes)

    fun label(context: Context, value: Long) =
        choices.firstOrNull { it.first == value }?.second?.let(context::getString) ?: value.toString()
}

/** User preferences backed by SharedPreferences and exposed as Compose state. */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("capyps5", Context.MODE_PRIVATE)

    var overlayMode by mutableStateOf(
        runCatching { OverlayMode.valueOf(prefs.getString("overlay_mode", null)!!) }.getOrDefault(OverlayMode.Compact)
    )
        private set
    var touchControls by mutableStateOf(
        runCatching { TouchControlsMode.valueOf(prefs.getString("touch_controls", null)!!) }
            .getOrDefault(TouchControlsMode.Auto)
    )
        private set
    var controlsOpacity by mutableFloatStateOf(prefs.getFloat("controls_opacity", 0.6f))
        private set
    /** On-screen touchpad: Auto shows it with the touch controls, Always also with a physical pad. */
    var touchpadOverlay by mutableStateOf(
        runCatching { TouchControlsMode.valueOf(prefs.getString("touchpad_overlay", null)!!) }
            .getOrDefault(TouchControlsMode.Auto)
    )
        private set

    fun updateTouchpadOverlay(value: TouchControlsMode) {
        touchpadOverlay = value
        prefs.edit().putString("touchpad_overlay", value.name).apply()
    }
    var buttonLayout by mutableStateOf(
        runCatching { ButtonLayout.valueOf(prefs.getString("button_layout", null)!!) }
            .getOrDefault(ButtonLayout.Standard)
    )
        private set

    fun updateButtonLayout(value: ButtonLayout) {
        buttonLayout = value
        prefs.edit().putString("button_layout", value.name).apply()
    }
    /** Folders scanned for games (each game a subfolder with eboot.bin); the first one also holds logs. */
    var libraryPaths by mutableStateOf(
        prefs.getString("library_paths", null)?.split('\n')?.filter { it.isNotBlank() }?.takeIf { it.isNotEmpty() }
            ?: listOf(
                prefs.getString("library_path", null)
                    ?: File(Environment.getExternalStorageDirectory(), "ConsoleFivePro").absolutePath
            )
    )
        private set

    /** Main library folder (first in the list). */
    val libraryPath: String get() = libraryPaths.first()

    fun addLibraryPath(value: String) {
        if (value in libraryPaths) return
        storeLibraryPaths(libraryPaths + value)
    }

    fun removeLibraryPath(value: String) {
        // At least one folder stays: the launcher needs somewhere to look and to write logs.
        if (libraryPaths.size <= 1) return
        storeLibraryPaths(libraryPaths - value)
    }

    private fun storeLibraryPaths(values: List<String>) {
        libraryPaths = values
        prefs.edit().putString("library_paths", values.joinToString("\n")).apply()
    }

    // Bumped on every core option change so composables reading the prefs recompose.
    var coreOptionsVersion by mutableIntStateOf(0)
        private set

    /**
     * v0.0.9pro: the Mali performance profile is active — forced on, or Auto on a device whose
     * GPU is a Mali (the core makes the same check from the Vulkan device name). Bandwidth-tuned
     * defaults, identical graphics.
     */
    fun maliProfileActive(): Boolean = when (prefs.getLong("opt_MaliProfile", 2L)) {
        1L -> true
        0L -> false
        else -> GpuInfo.isMali
    }

    /**
     * v0.0.10pro phase 2: the Adreno performance profile is active - forced on, or Auto on a
     * Snapdragon device (the core makes the same check from the Vulkan device name).
     */
    fun adrenoProfileActive(): Boolean = when (prefs.getLong("opt_AdrenoProfile", 2L)) {
        1L -> true
        0L -> false
        else -> GpuInfo.isAdreno
    }

    /** Global value of a core option. */
    fun coreOption(option: CoreOption): Long = prefs.getLong("opt_${option.name}", gpuAwareDefault(option))

    /**
     * v0.0.9pro / v0.0.10pro: with either GPU profile active the streaming buffer defaults to
     * 128 MB and the GPU download read-ahead to 512 KB (heavy titles fault GPU-owned pages in
     * long sequential sweeps - a wider window halves the fault+download count).
     *
     * The frame generator is gated by the GPU itself, not by a profile: every Mali
     * generation and every Adreno below the 7xx series lacks the headroom for the extra
     * per-tick composite, so its DEFAULT is off there (a user choice still wins; the
     * setting just starts disabled instead of enabled).
     */
    private fun gpuAwareDefault(option: CoreOption): Long {
        if (option == CoreOption.FrameGeneration && !GpuInfo.frameGenerationCapable) {
            return 0L
        }
        if (!maliProfileActive() && !adrenoProfileActive()) return option.default
        return when (option) {
            CoreOption.StreamingBuffer -> 128L
            CoreOption.ReadAhead -> 512L
            else -> option.default
        }
    }

    fun updateCoreOption(option: CoreOption, value: Long) {
        prefs.edit().putLong("opt_${option.name}", value).apply()
        coreOptionsVersion++
    }

    /** Per-game override, or null when the game follows the global value. */
    fun gameOption(titleId: String, option: CoreOption): Long? {
        val key = "game_${titleId}_${option.name}"
        return if (prefs.contains(key)) prefs.getLong(key, option.default) else null
    }

    fun updateGameOption(titleId: String, option: CoreOption, value: Long?) {
        val key = "game_${titleId}_${option.name}"
        prefs.edit().apply { if (value == null) remove(key) else putLong(key, value) }.apply()
        coreOptionsVersion++
    }

    /** Global GPU driver id ("" = system driver). */
    fun gpuDriver(): String = prefs.getString("gpu_driver", "").orEmpty()

    fun updateGpuDriver(id: String) {
        prefs.edit().putString("gpu_driver", id).apply()
        coreOptionsVersion++
    }

    /** Per-game GPU driver: null follows the global one, "" forces the system driver. */
    fun gameGpuDriver(titleId: String): String? = prefs.getString("game_${titleId}_gpu_driver", null)

    fun updateGameGpuDriver(titleId: String, id: String?) {
        val key = "game_${titleId}_gpu_driver"
        prefs.edit().apply { if (id == null) remove(key) else putString(key, id) }.apply()
        coreOptionsVersion++
    }

    /** v0.0.6proplus: clears every per-game override (options and GPU driver) for one title. */
    fun clearGameOverrides(titleId: String) {
        val editor = prefs.edit()
        CoreOption.entries.forEach { editor.remove("game_${titleId}_${it.name}") }
        editor.remove("game_${titleId}_gpu_driver")
        editor.apply()
        coreOptionsVersion++
    }

    fun effectiveGpuDriver(titleId: String): String = gameGpuDriver(titleId) ?: gpuDriver()

    /**
     * Resolution scale for Unreal Engine games without their own setting: they render near 4K and
     * are bound by pixel work on mobile GPUs, so a lower default keeps them playable.
     */
    fun ueResolution(): Long = prefs.getLong("ue_resolution", 250L)

    fun updateUeResolution(value: Long) {
        prefs.edit().putLong("ue_resolution", value).apply()
        coreOptionsVersion++
    }

    /** Value a game runs with: its own setting, then the Unreal default, then the global one. */
    fun effectiveOption(titleId: String, option: CoreOption, unreal: Boolean = false): Long =
        gameOption(titleId, option)
            ?: if (unreal && option == CoreOption.ResolutionScale) ueResolution() else coreOption(option)

    /** v0.0.6proplus: directory of the last launched game (pre-selected on the home screen). */
    var lastGameDir: String
        get() = prefs.getString("last_game_dir", "").orEmpty()
        set(value) {
            prefs.edit().putString("last_game_dir", value).apply()
        }

    /** v0.0.6proplus: lower the render scale under sustained frame drops, recover on headroom. */
    var adaptiveResolution by mutableStateOf(prefs.getBoolean("adaptive_resolution", false))
        private set

    fun updateAdaptiveResolution(value: Boolean) {
        adaptiveResolution = value
        prefs.edit().putBoolean("adaptive_resolution", value).apply()
    }

    /** v0.0.8proplus: shape of the game image (letterboxed unless Default). */
    var screenAspect by mutableStateOf(
        runCatching { ScreenAspect.valueOf(prefs.getString("screen_aspect", null)!!) }
            .getOrDefault(ScreenAspect.Default)
    )
        private set

    fun updateScreenAspect(value: ScreenAspect) {
        screenAspect = value
        prefs.edit().putString("screen_aspect", value.name).apply()
    }

    /** Periodic safety copies of the running game's save data (kept under filesDir). */
    var autoSave by mutableStateOf(prefs.getBoolean("auto_save", true))
        private set

    fun updateAutoSave(value: Boolean) {
        autoSave = value
        prefs.edit().putBoolean("auto_save", value).apply()
    }

    /** Whether the launcher looks for a newer release on start. */
    var updateChecks by mutableStateOf(prefs.getBoolean("update_checks", true))
        private set

    fun updateUpdateChecks(value: Boolean) {
        updateChecks = value
        prefs.edit().putBoolean("update_checks", value).apply()
    }

    /** Release the user chose to skip; no notice until a newer one appears. */
    var skippedRelease: String
        get() = prefs.getString("skipped_release", "").orEmpty()
        set(value) = prefs.edit().putString("skipped_release", value).apply()

    /** Version whose "what's new" card was last shown; empty before the first one. */
    var lastSeenVersion: String
        get() = prefs.getString("last_seen_version", "").orEmpty()
        set(value) = prefs.edit().putString("last_seen_version", value).apply()

    /** Counts launcher starts and returns the new count (the support card shows every 10th). */
    fun countLaunch(): Int {
        val count = prefs.getInt("launch_count", 0) + 1
        prefs.edit().putInt("launch_count", count).apply()
        return count
    }

    fun updateOverlayMode(value: OverlayMode) {
        overlayMode = value
        prefs.edit().putString("overlay_mode", value.name).apply()
    }

    fun updateTouchControls(value: TouchControlsMode) {
        touchControls = value
        prefs.edit().putString("touch_controls", value.name).apply()
    }

    fun updateControlsOpacity(value: Float) {
        controlsOpacity = value
        prefs.edit().putFloat("controls_opacity", value).apply()
    }

    /**
     * v0.0.10pro phase 3: the user's custom on-screen pad layout (see [ControlsLayout]); the
     * fixed layout is used until the user saves one in the controls editor.
     */
    var controlsLayout by mutableStateOf(ControlsLayout.fromJson(prefs.getString("controls_layout", null)) ?: ControlsLayout())
        private set

    fun updateControlsLayout(value: ControlsLayout) {
        controlsLayout = value.clamped()
        prefs.edit().putString("controls_layout", value.clamped().toJson()).apply()
    }

    /** Restores the built-in layout (the editor's Reset button). */
    fun resetControlsLayout() {
        controlsLayout = ControlsLayout()
        prefs.edit().remove("controls_layout").apply()
    }
}
