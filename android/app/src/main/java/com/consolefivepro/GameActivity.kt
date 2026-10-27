// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Process
import kotlin.math.roundToInt
import androidx.activity.compose.setContent
import com.consolefivepro.model.GameItem
import org.libsdl.app.SDL
import com.consolefivepro.ui.screens.EmulationScreen
import com.consolefivepro.ui.theme.CapyPS5Theme
import java.io.File

/**
 * v0.0.10pro phase 2: display refresh selection shared by the activity and the game surface.
 * The window used to stay at Android's default 60 Hz and the core's emulated vblank at a fixed
 * 60 Hz, so games paced on vblank never exceeded 60 FPS even on 90/120 Hz displays. The chosen
 * rate (per-game RefreshRate option, Auto = the display's highest) now drives both the window
 * mode and the emulated vblank; games that do not pace on vblank are unaffected.
 */
object RefreshPolicy {
    /** Highest refresh the default display supports, rounded, capped at 120 (0 when unknown). */
    @Suppress("DEPRECATION")
    fun displayMaxHz(context: Context): Int = runCatching {
        val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as? android.view.WindowManager
        val display = windowManager?.defaultDisplay ?: return 0
        display.supportedModes.maxOfOrNull { it.refreshRate }?.roundToInt()?.coerceAtMost(120) ?: 0
    }.getOrDefault(0)

    /**
     * The applied refresh: the option's cap (0 = follow the display), never above what the
     * display supports and never below 60.
     */
    fun effectiveHz(context: Context, settings: Settings, titleId: String): Int {
        val max = displayMaxHz(context).coerceAtLeast(60)
        val cap = settings.effectiveOption(titleId, CoreOption.RefreshRate).toInt()
        return if (cap <= 0) max else cap.coerceAtMost(max)
    }

    /** Asks the window to run in the display mode with (at least) that refresh rate. */
    @Suppress("DEPRECATION")
    fun applyToWindow(context: Context, hz: Int): Boolean = runCatching {
        if (hz <= 0) return false
        val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as? android.view.WindowManager
        val display = windowManager?.defaultDisplay ?: return false
        val activity = context as? android.app.Activity ?: return false
        val best = display.supportedModes
            .filter { it.refreshRate >= hz - 0.5f }
            .maxByOrNull { it.refreshRate } ?: return false
        activity.window.attributes = activity.window.attributes.also { it.preferredDisplayModeId = best.modeId }
        true
    }.getOrDefault(false)
}

/**
 * Runs one game in its own process (":game"). The native core cannot be torn down and started
 * again inside one process (guest threads and global state outlive Emulator::Run), so leaving the
 * game ends this process and the launcher, in the main process, stays where it was.
 */
class GameActivity : CapyActivity() {

    companion object {
        private const val EXTRA_TITLE_ID = "title_id"
        private const val EXTRA_TITLE_NAME = "title_name"
        private const val EXTRA_DIR = "game_dir"
        private const val EXTRA_ELF = "game_elf"

        /** Native fatal reports land here (see Log::SetFatalReportPath). */
        fun errorFile(context: Context) = File(context.filesDir, "last_error.txt")

        fun launch(context: Context, game: GameItem) {
            // v0.0.6proplus: remember what was played; the home screen pre-selects it.
            Settings(context).lastGameDir = game.directoryPath
            context.startActivity(
                Intent(context, GameActivity::class.java)
                    .putExtra(EXTRA_TITLE_ID, game.titleId)
                    .putExtra(EXTRA_TITLE_NAME, game.titleName)
                    .putExtra(EXTRA_DIR, game.directoryPath)
                    .putExtra(EXTRA_ELF, game.elfPath)
            )
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val elf = intent.getStringExtra(EXTRA_ELF)
        if (elf == null) {
            finish()
            return
        }
        val game = GameItem(
            titleId = intent.getStringExtra(EXTRA_TITLE_ID) ?: "",
            titleName = intent.getStringExtra(EXTRA_TITLE_NAME) ?: File(elf).name,
            directoryPath = intent.getStringExtra(EXTRA_DIR) ?: (File(elf).parent ?: ""),
            elfPath = elf,
        )
        // A report left by an earlier session must not be shown for this one.
        errorFile(this).delete()
        NativeBridge.nativeInit(filesDir = filesDir.absolutePath, cacheDir = cacheDir.absolutePath)
        // SDL (statically linked in the core) needs its Java side for audio output on Android; the
        // frontend is not an SDLActivity, so set it up by hand before the game opens any device.
        runCatching {
            SDL.setupJNI()
            SDL.initialize()
            SDL.setContext(this)
        }.onFailure { android.util.Log.e("CapyPS5-Audio", "SDL Java setup failed", it) }
        InputRouter.onRouteChanged = { toUi -> if (toUi) releaseEmulatedPad() }

        val settings = Settings(this)
        swapFaceButtons = settings.buttonLayout == ButtonLayout.Nintendo
        // Global settings with this game's overrides, applied before the core reads them.
        CoreOption.entries.forEach { option ->
            NativeBridge.nativeSetOption(option.property, settings.effectiveOption(game.titleId, option, game.isUnreal))
        }
        val driver = GpuDrivers.find(this, settings.effectiveGpuDriver(game.titleId))
        NativeBridge.nativeSetGpuDriver(
            nativeLibraryDir = applicationInfo.nativeLibraryDir,
            driverDir = driver?.dir?.absolutePath.orEmpty(),
            library = driver?.library.orEmpty(),
        )
        // v0.0.10pro phase 2: run the display at the chosen rate and pass it as the emulated
        // vblank - the core's present thread flips at most once per vblank, which was the
        // invisible 60 FPS ceiling. Must be set before nativeStartGame.
        val refreshHz = RefreshPolicy.effectiveHz(this, settings, game.titleId)
        if (RefreshPolicy.applyToWindow(this, refreshHz)) {
            android.util.Log.i("CapyPS5-Refresh", "window display mode set for ${refreshHz} Hz")
        }
        NativeBridge.nativeSetOption("debug.capy.vblank_hz", refreshHz.toLong())
        val versionText = Updater.versionText(this)
        setContent {
            CapyPS5Theme {
                EmulationScreen(
                    game = game,
                    settings = settings,
                    versionText = versionText,
                    onExit = ::exitGame,
                )
            }
        }
    }

    private fun exitGame() {
        runCatching { NativeBridge.nativeStopGame() }
        finish()
        // The launcher activity lives in the main process; ending this one frees everything the
        // emulator held (guest memory, threads, Vulkan device).
        Process.killProcess(Process.myPid())
    }
}
