// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro.ui.screens

import android.view.InputDevice
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Logout
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.outlined.Layers
import androidx.compose.material.icons.outlined.SystemUpdate
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.Icon
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import com.consolefivepro.CoreOption
import com.consolefivepro.GameSaves
import com.consolefivepro.RefreshPolicy
import com.consolefivepro.InputRouter
import com.consolefivepro.NativeBridge
import com.consolefivepro.OverlayMode
import com.consolefivepro.ScreenAspect
import com.consolefivepro.Settings
import com.consolefivepro.TouchControlsMode
import com.consolefivepro.model.GameItem
import com.consolefivepro.telemetry.PerfMonitor
import com.consolefivepro.telemetry.PerfSnapshot
import com.consolefivepro.ui.NavKey
import com.consolefivepro.ui.OnNavKey
import com.consolefivepro.ui.components.LayersPanel
import com.consolefivepro.ui.components.PerfOverlay
import com.consolefivepro.ui.components.SettingsPanel
import com.consolefivepro.ui.components.TouchpadOverlay
import com.consolefivepro.ui.components.VirtualGamepad
import com.consolefivepro.ui.theme.Capy
import com.consolefivepro.R
import androidx.compose.ui.res.stringResource

private const val AUTO_SAVE_INTERVAL_MS = 5 * 60 * 1000L

private fun hasPhysicalGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
    val device = InputDevice.getDevice(id) ?: return@any false
    !device.isVirtual && (device.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
}

@Composable
fun EmulationScreen(
    game: GameItem,
    settings: Settings,
    versionText: String,
    onExit: () -> Unit,
) {
    val context = LocalContext.current
    var showPauseMenu by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    var showLayers by remember { mutableStateOf(false) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var gamepadConnected by remember { mutableStateOf(hasPhysicalGamepad()) }
    var snapshot by remember { mutableStateOf(PerfSnapshot()) }
    // v0.0.6proplus: live render scale while adaptive resolution is active (0 = hidden).
    var adaptiveScale by remember { mutableLongStateOf(0L) }
    val fpsHistory = remember { mutableStateListOf<Float>() }
    // Probing /sys and /sys/class/thermal costs ~130 file reads (see cpuThermalZones);
    // keep one monitor for the whole screen instead of rebuilding it on every overlay
    // mode change. Constructed lazily on Dispatchers.IO inside the effect.
    var perfMonitor by remember { mutableStateOf<PerfMonitor?>(null) }

    // While any menu is open, controller input drives the UI instead of the game.
    val uiHasInput = showPauseMenu || showSettings || showLayers || loadError != null
    LaunchedEffect(uiHasInput) { InputRouter.setRouteToUi(uiHasInput) }
    DisposableEffect(Unit) {
        InputRouter.onPauseRequested = { showPauseMenu = true }
        onDispose {
            InputRouter.onPauseRequested = null
            InputRouter.setRouteToUi(true)
        }
    }

    LaunchedEffect(Unit) {
        while (isActive) {
            gamepadConnected = hasPhysicalGamepad()
            delay(2000)
        }
    }

    LaunchedEffect(settings.overlayMode) {
        if (settings.overlayMode == OverlayMode.Off) return@LaunchedEffect
        val monitor = perfMonitor ?: withContext(Dispatchers.IO) { PerfMonitor(context) }
            .also { perfMonitor = it }
        while (isActive) {
            val sample = withContext(Dispatchers.IO) { monitor.sample() }
            snapshot = sample
            fpsHistory.add(sample.fps)
            if (fpsHistory.size > 60) fpsHistory.removeAt(0)
            delay(500)
        }
    }

    // Console Five Pro (v0.0.6proplus): adaptive resolution. Sustained frame drops step
    // the render scale down (never below 250 per mille); sustained headroom steps it back
    // up to the scale the game started with. The core picks option changes up within
    // ~250 ms (see Common::DebugFlags::CachedValue), so no restart is needed. The
    // thresholds skip 30-fps-capped games: they sit between 26 and 54 and never trigger.
    val menusOpenForAdaptive = rememberUpdatedState(uiHasInput)
    LaunchedEffect(settings.adaptiveResolution, game.titleId) {
        if (!settings.adaptiveResolution) return@LaunchedEffect
        val monitor = perfMonitor ?: withContext(Dispatchers.IO) { PerfMonitor(context) }
            .also { perfMonitor = it }
        val maxScale = settings.effectiveOption(game.titleId, CoreOption.ResolutionScale, game.isUnreal)
        var scale = maxScale
        adaptiveScale = scale
        var belowFrames = 0
        var aboveFrames = 0
        while (isActive) {
            delay(1000)
            if (menusOpenForAdaptive.value) {
                belowFrames = 0
                aboveFrames = 0
                continue
            }
            val fps = withContext(Dispatchers.IO) { monitor.sample() }.fps
            if (fps <= 0f) continue
            when {
                fps < 26f -> { belowFrames++; aboveFrames = 0 }
                fps > 54f -> { aboveFrames++; belowFrames = 0 }
                else -> { belowFrames = 0; aboveFrames = 0 }
            }
            if (belowFrames >= 4 && scale > 250L) {
                scale = (scale - 125L).coerceAtLeast(250L)
                NativeBridge.nativeSetOption(CoreOption.ResolutionScale.property, scale)
                adaptiveScale = scale
                belowFrames = 0
            } else if (aboveFrames >= 8 && scale < maxScale) {
                scale = (scale + 125L).coerceAtMost(maxScale)
                NativeBridge.nativeSetOption(CoreOption.ResolutionScale.property, scale)
                adaptiveScale = scale
                aboveFrames = 0
            }
        }
    }

    // Console Five Pro (v0.0.8pro): safety copies of the running game's saves. The core
    // writes them under filesDir/_SaveData; a crash mid-write or a corrupted game save
    // would otherwise be unrecoverable, so a fresh copy is kept every few minutes.
    var autoSaveNote by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(settings.autoSave, game.titleId) {
        if (!settings.autoSave) return@LaunchedEffect
        while (isActive) {
            delay(AUTO_SAVE_INTERVAL_MS)
            val note = withContext(Dispatchers.IO) { GameSaves.snapshot(context, game.titleId) }
            if (note != null) autoSaveNote = note
        }
    }

    val showTouch = when (settings.touchControls) {
        TouchControlsMode.Always -> true
        TouchControlsMode.Never -> false
        TouchControlsMode.Auto -> !gamepadConnected
    }

    val exitWithBackup: () -> Unit = {
        if (settings.autoSave) runCatching { GameSaves.snapshot(context, game.titleId) }
        onExit()
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        // v0.0.8proplus: optional letterboxing. aspectRatio with the incoming max constraints
        // picks the largest area with the chosen ratio, centered by align; Default fills the
        // screen. The core re-creates its swapchain when the surface changes size.
        val surfaceModifier = when (settings.screenAspect) {
            ScreenAspect.Default -> Modifier.fillMaxSize()
            ScreenAspect.R16_9 -> Modifier.align(Alignment.Center).aspectRatio(16f / 9f)
            ScreenAspect.R5_4 -> Modifier.align(Alignment.Center).aspectRatio(5f / 4f)
        }
        AndroidView(
            modifier = surfaceModifier,
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        private var started = false

                        override fun surfaceCreated(holder: SurfaceHolder) {
                            // v0.0.10pro phase 2: the surface adds its own vote for the chosen
                            // display rate (some vendors listen to the surface, not the window).
                            if (android.os.Build.VERSION.SDK_INT >= 30) {
                                runCatching {
                                    holder.surface.setFrameRate(
                                        RefreshPolicy.effectiveHz(context, settings, game.titleId).toFloat(),
                                        android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,
                                    )
                                }
                            }
                            NativeBridge.nativeSetSurface(holder.surface)
                            if (started) return
                            started = true
                            if (!java.io.File(game.elfPath).exists()) {
                                loadError = context.getString(R.string.error_elf_missing, game.elfPath)
                                return
                            }
                            if (!NativeBridge.nativeStartGame(game.directoryPath, game.elfPath)) {
                                loadError = context.getString(R.string.error_elf_start)
                            }
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            if (holder.surface.isValid) NativeBridge.nativeSetSurface(holder.surface)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            NativeBridge.nativeDestroySurface()
                        }
                    })
                }
            },
        )

        // Swiping in from the left edge opens the pause menu (as in other Android emulators). The
        // strip is excluded from the system back gesture so Android does not take the swipe.
        val density = androidx.compose.ui.platform.LocalDensity.current
        val view = androidx.compose.ui.platform.LocalView.current
        Box(
            Modifier
                .align(Alignment.CenterStart)
                .width(28.dp)
                .fillMaxHeight()
                .onGloballyPositioned { coordinates ->
                    val bounds = coordinates.boundsInWindow()
                    view.systemGestureExclusionRects = listOf(
                        android.graphics.Rect(
                            bounds.left.toInt(), bounds.top.toInt(), bounds.right.toInt(), bounds.bottom.toInt()
                        )
                    )
                }
                .pointerInput(Unit) {
                    var dragged = 0f
                    detectHorizontalDragGestures(
                        onDragStart = { dragged = 0f },
                        onHorizontalDrag = { change, amount ->
                            dragged += amount
                            if (dragged > with(density) { 72.dp.toPx() }) {
                                change.consume()
                                showPauseMenu = true
                            }
                        },
                    )
                },
        )

        if (showTouch) {
            VirtualGamepad(
                modifier = Modifier.fillMaxSize(),
                alpha = settings.controlsOpacity,
                showTouchpad = settings.touchpadOverlay != TouchControlsMode.Never,
                onMenuClick = { showPauseMenu = true },
            )
        } else if (settings.touchpadOverlay == TouchControlsMode.Always) {
            TouchpadOverlay(Modifier.fillMaxSize(), alpha = settings.controlsOpacity)
        }

        PerfOverlay(
            mode = settings.overlayMode,
            snapshot = snapshot,
            fpsHistory = fpsHistory,
            adaptiveScale = if (settings.adaptiveResolution) adaptiveScale else 0L,
            modifier = Modifier.align(Alignment.TopStart).padding(10.dp),
        )

        AnimatedVisibility(visible = showPauseMenu && !showSettings && !showLayers, enter = fadeIn(), exit = fadeOut()) {
            PauseMenu(
                game = game,
                autoSaveNote = if (settings.autoSave) autoSaveNote else null,
                onResume = { showPauseMenu = false },
                onSettings = { showSettings = true },
                onLayers = {
                    showPauseMenu = false
                    showLayers = true
                },
                onExit = exitWithBackup,
            )
        }

        if (showSettings) {
            Box(Modifier.fillMaxSize().background(Capy.Scrim).clickable { showSettings = false })
            SettingsPanel(
                settings, versionText,
                onClose = { showSettings = false },
                modifier = Modifier.align(Alignment.CenterEnd),
            )
        }

        if (showLayers) {
            LayersPanel(onClose = { showLayers = false }, modifier = Modifier.align(Alignment.CenterEnd))
        }

        loadError?.let { message ->
            ErrorCard(message, exitWithBackup)
        }
    }
}

@Composable
private fun PauseMenu(
    game: GameItem,
    autoSaveNote: String?,
    onResume: () -> Unit,
    onSettings: () -> Unit,
    onLayers: () -> Unit,
    onExit: () -> Unit,
) {
    var logNote by remember { mutableStateOf<String?>(null) }
    var confirmingExit by remember { mutableStateOf(false) }
    val pauseContext = androidx.compose.ui.platform.LocalContext.current
    val entries = listOfNotNull(
        Triple(stringResource(R.string.resume), Icons.Filled.PlayArrow, onResume),
        Triple(stringResource(R.string.settings), Icons.Outlined.Tune, onSettings),
        Triple(stringResource(R.string.layers), Icons.Outlined.Layers, onLayers),
        // Both processes share the app UID, so the game's lines are in the same log.
        Triple(
            stringResource(R.string.save_log), Icons.Outlined.SystemUpdate,
            { logNote = saveLog(pauseContext) },
        ),
        Triple(
            stringResource(R.string.exit), Icons.AutoMirrored.Outlined.Logout,
            { if (confirmingExit) onExit() else confirmingExit = true },
        ),
    )
    var focused by remember { mutableIntStateOf(0) }
    OnNavKey { key ->
        when (key) {
            NavKey.Left -> focused = (focused - 1).mod(entries.size)
            NavKey.Right -> focused = (focused + 1).mod(entries.size)
            NavKey.Confirm -> entries[focused].third()
            NavKey.Back, NavKey.Menu -> if (confirmingExit) confirmingExit = false else onResume()
            else -> Unit
        }
    }
    Box(
        Modifier.fillMaxSize().background(Capy.Scrim).clickable(onClick = onResume),
        contentAlignment = Alignment.BottomStart,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(Capy.Surface)
                .clickable(enabled = false) {}
                .padding(horizontal = 28.dp, vertical = 18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(game.titleName, style = MaterialTheme.typography.titleLarge, maxLines = 1)
                Spacer(Modifier.width(12.dp))
                Text(
                    listOfNotNull(
                        game.titleId,
                        game.version?.let { "v$it" },
                    ).joinToString("  ·  "),
                    style = MaterialTheme.typography.bodySmall,
                    color = Capy.TextSecondary,
                )
            }
            autoSaveNote?.let {
                Text(
                    stringResource(R.string.autosave_last, it),
                    style = MaterialTheme.typography.bodySmall,
                    color = Capy.TextSecondary,
                )
            }
            if (confirmingExit) {
                Text(
                    stringResource(R.string.exit_confirm),
                    style = MaterialTheme.typography.bodyMedium,
                    color = Capy.Bad,
                )
            }
            logNote?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                entries.forEachIndexed { i, (label, icon, action) ->
                    val isFocused = i == focused
                    val danger = i == entries.lastIndex
                    Column(
                        modifier = Modifier
                            .width(112.dp)
                            .background(
                                when {
                                    isFocused && danger -> Capy.Bad
                                    isFocused -> Color.White
                                    else -> Capy.SurfaceHigh
                                },
                                RoundedCornerShape(14.dp),
                            )
                            .clickable { focused = i; action() }
                            .padding(vertical = 14.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        val tint = when {
                            isFocused && danger -> Color.White
                            isFocused -> Color.Black
                            danger -> Capy.Bad
                            else -> Capy.TextPrimary
                        }
                        Icon(icon, null, Modifier.size(24.dp), tint = tint)
                        Text(label, style = MaterialTheme.typography.labelLarge, color = tint)
                    }
                }
            }
            HintBar(
                listOf(
                    "◀ ▶" to stringResource(R.string.hint_choose),
                    "Ⓐ" to stringResource(R.string.hint_accept),
                    "Ⓑ" to stringResource(R.string.hint_back_to_game),
                ),
            )
        }
    }
}

@Composable
private fun ErrorCard(message: String, onExit: () -> Unit) {
    OnNavKey { key -> if (key == NavKey.Confirm || key == NavKey.Back) onExit() }
    Box(Modifier.fillMaxSize().background(Capy.Scrim), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .width(420.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(stringResource(R.string.error_start_title), style = MaterialTheme.typography.titleLarge, color = Capy.Bad)
            Text(message, style = MaterialTheme.typography.bodyMedium, color = Capy.TextSecondary)
            Text(
                stringResource(R.string.back_to_home_a),
                style = MaterialTheme.typography.labelLarge,
                color = Color.White,
                modifier = Modifier
                    .background(Capy.Accent, RoundedCornerShape(50))
                    .clickable(onClick = onExit)
                    .padding(horizontal = 18.dp, vertical = 9.dp),
            )
        }
    }
}

/** Dumps the shared log next to the game library, as the launcher does. */
private fun saveLog(context: android.content.Context): String? = runCatching {
    val dir = java.io.File(Settings(context).libraryPath, "logs").apply { mkdirs() }
    val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
        .format(java.util.Date())
    val file = java.io.File(dir, "consolefivepro-$stamp.txt")
    val process = Runtime.getRuntime().exec(arrayOf("logcat", "-d", "-v", "time"))
    process.inputStream.use { input -> file.outputStream().use { output -> input.copyTo(output) } }
    process.waitFor()
    context.getString(R.string.log_saved_in, file.absolutePath)
}.getOrNull()