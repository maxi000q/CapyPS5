// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import com.consolefivepro.ButtonLayout
import com.consolefivepro.ControlsLayout
import com.consolefivepro.NativeBridge
import com.consolefivepro.ui.theme.Capy
import kotlin.math.hypot
import kotlin.math.roundToInt

/** Index of the movable clusters in a [ControlsLayout], as the editor sees them. */
const val CLUSTER_LEFT = 0
const val CLUSTER_RIGHT = 1
const val CLUSTER_TOP = 2

/**
 * On-screen DualSense: shoulders, d-pad, both sticks, face buttons, Create/Options and touchpad.
 *
 * v0.0.10pro phase 3: the three clusters (left pad side, right pad side, top bar) follow the
 * user's [ControlsLayout] - positions and sizes are set in the controls editor and saved. With
 * [editMode] the pad is the editor's live preview: no input reaches the game, clusters can be
 * dragged and selected instead.
 */
@Composable
fun VirtualGamepad(
    modifier: Modifier = Modifier,
    alpha: Float = 0.65f,
    showTouchpad: Boolean = true,
    layout: ControlsLayout = ControlsLayout(),
    buttonLayout: ButtonLayout = ButtonLayout.Standard,
    editMode: Boolean = false,
    selectedCluster: Int = -1,
    onClusterSelect: ((Int) -> Unit)? = null,
    onClusterMove: ((Int, Float, Float) -> Unit)? = null,
    onMenuClick: () -> Unit = {},
) {
    var buttons by remember { mutableIntStateOf(0) }
    val axes = remember { FloatArray(6) } // lx, ly, rx, ry, l2, r2

    fun send() {
        if (editMode) return
        NativeBridge.nativeSendControllerInput(buttons, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5])
    }

    fun button(mask: Int, pressed: Boolean) {
        if (editMode) return
        buttons = if (pressed) buttons or mask else buttons and mask.inv()
        send()
    }

    BoxWithConstraints(modifier.fillMaxSize().alpha(alpha)) {
        val density = LocalDensity.current
        val screenW = constraints.maxWidth.toFloat()
        val screenH = constraints.maxHeight.toFloat()

        // Centres the (w x h) cluster box on its anchor; large clusters stay on screen.
        fun place(index: Int, w: Dp, h: Dp): Modifier {
            val pos = layout.cluster(index)
            val wpx = with(density) { w.toPx() }
            val hpx = with(density) { h.toPx() }
            return Modifier
                .size(w, h)
                .offset {
                    IntOffset(
                        (screenW * pos.x - wpx / 2f).roundToInt()
                            .coerceIn(0, (screenW - wpx).roundToInt().coerceAtLeast(0)),
                        (screenH * pos.y - hpx / 2f).roundToInt()
                            .coerceIn(0, (screenH - hpx).roundToInt().coerceAtLeast(0)),
                    )
                }
        }

        // In the editor: tap selects a cluster, drag moves it (deltas in screen fractions).
        fun editGestures(index: Int): Modifier = if (!editMode) {
            Modifier
        } else {
            Modifier
                .pointerInput(index) {
                    detectTapGestures { onClusterSelect?.invoke(index) }
                }
                .pointerInput(index) {
                    detectDragGestures { change, drag ->
                        onClusterMove?.invoke(index, drag.x / screenW, drag.y / screenH)
                        change.consume()
                    }
                }
        }

        fun clusterBox(index: Int, w: Dp, h: Dp, extra: Modifier = Modifier): Modifier =
            place(index, w, h)
                .then(editGestures(index))
                .then(
                    if (editMode && selectedCluster == index) {
                        Modifier.border(2.dp, Capy.Accent, RoundedCornerShape(14.dp))
                    } else if (editMode) {
                        Modifier.border(1.dp, Color(0x55FFFFFF), RoundedCornerShape(14.dp))
                    } else {
                        Modifier
                    }
                )
                .then(extra)

        // Left: L2/L1, d-pad and left stick.
        val ls = layout.left.scale
        Box(clusterBox(CLUSTER_LEFT, 254.dp * ls, 170.dp * ls)) {
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(10.dp * ls)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp * ls)) {
                    GamepadButton("L2", 56.dp * ls, 36.dp * ls, scale = ls, enabled = !editMode) {
                        axes[4] = if (it) 1f else 0f; button(NativeBridge.BTN_L2, it)
                    }
                    GamepadButton("L1", 56.dp * ls, 36.dp * ls, scale = ls, enabled = !editMode) {
                        button(NativeBridge.BTN_L1, it)
                    }
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(14.dp * ls),
                    verticalAlignment = Alignment.Bottom,
                ) {
                    DPad(scale = ls, enabled = !editMode, onButton = ::button)
                    AnalogStick(size = 116.dp * ls, enabled = !editMode) { x, y ->
                        axes[0] = x; axes[1] = y; send()
                    }
                }
            }
        }

        // Right: R1/R2, right stick and face buttons.
        val rs = layout.right.scale
        Box(clusterBox(CLUSTER_RIGHT, 254.dp * rs, 170.dp * rs)) {
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(10.dp * rs)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp * rs)) {
                    GamepadButton("R1", 56.dp * rs, 36.dp * rs, scale = rs, enabled = !editMode) {
                        button(NativeBridge.BTN_R1, it)
                    }
                    GamepadButton("R2", 56.dp * rs, 36.dp * rs, scale = rs, enabled = !editMode) {
                        axes[5] = if (it) 1f else 0f; button(NativeBridge.BTN_R2, it)
                    }
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(14.dp * rs),
                    verticalAlignment = Alignment.Bottom,
                ) {
                    AnalogStick(size = 116.dp * rs, enabled = !editMode) { x, y ->
                        axes[2] = x; axes[3] = y; send()
                    }
                    FaceButtons(scale = rs, nintendo = buttonLayout == ButtonLayout.Nintendo, enabled = !editMode, onButton = ::button)
                }
            }
        }

        // Top centre: Create, touchpad, Options and the emulator menu.
        val ts = layout.top.scale
        val topWidth = (if (showTouchpad) 432.dp else 200.dp) * ts
        Box(clusterBox(CLUSTER_TOP, topWidth, 100.dp * ts)) {
            Row(
                Modifier.fillMaxSize(),
                horizontalArrangement = Arrangement.spacedBy(12.dp * ts),
                verticalAlignment = Alignment.Top,
            ) {
                GamepadButton("CREATE", 70.dp * ts, 30.dp * ts, scale = ts, textSize = 10, enabled = !editMode) {
                    button(NativeBridge.BTN_SELECT, it)
                }
                if (showTouchpad) {
                    Touchpad(
                        Modifier.size(width = 220.dp * ts, height = 96.dp * ts),
                        enabled = !editMode,
                    )
                }
                GamepadButton("OPTIONS", 70.dp * ts, 30.dp * ts, scale = ts, textSize = 10, enabled = !editMode) {
                    button(NativeBridge.BTN_START, it)
                }
                GamepadButton("☰", 36.dp * ts, 30.dp * ts, scale = ts, textSize = 14, enabled = !editMode) {
                    if (it) onMenuClick()
                }
            }
        }
    }
}

/** Touchpad on its own, for devices with a physical controller but no touchpad. */
@Composable
fun TouchpadOverlay(modifier: Modifier = Modifier, alpha: Float = 0.65f) {
    Box(modifier.fillMaxSize().alpha(alpha).padding(top = 16.dp)) {
        Touchpad(Modifier.align(Alignment.TopCenter).size(width = 240.dp, height = 104.dp))
    }
}

/**
 * DualSense touchpad: up to two fingers are reported to the game with coordinates relative to the
 * pad; a short tap without movement also clicks it. Fingers that were down when the pad left
 * composition (menu opened, editor started) are released on dispose - before, they stayed
 * pressed in the game forever.
 */
@Composable
fun Touchpad(modifier: Modifier = Modifier, enabled: Boolean = true) {
    var touching by remember { mutableStateOf(false) }
    var clicked by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val shape = RoundedCornerShape(16.dp)
    // Pointer id -> (finger slot, down time, start position).
    val fingers = remember { HashMap<Long, Triple<Int, Long, Offset>>() }
    DisposableEffect(Unit) {
        onDispose {
            fingers.values.forEach { NativeBridge.nativeSendTouch(it.first, false, 0f, 0f) }
            fingers.clear()
        }
    }
    val base = modifier
        .clip(shape)
        .background(if (touching) Color(0x664466AA) else Color(0x44222A3A))
        .border(1.dp, if (clicked) Color(0xFF00E5FF) else Color(0x44FFFFFF), shape)
    Box(
        if (enabled) {
            base.pointerInput(Unit) {
                awaitPointerEventScope {
                    var moved = false
                    while (true) {
                        val event = awaitPointerEvent()
                        for (change in event.changes) {
                            val id = change.id.value
                            val x = (change.position.x / size.width).coerceIn(0f, 1f)
                            val y = (change.position.y / size.height).coerceIn(0f, 1f)
                            if (change.pressed && id !in fingers) {
                                val slot = (0..1).firstOrNull { s -> fingers.values.none { it.first == s } } ?: continue
                                fingers[id] = Triple(slot, change.uptimeMillis, change.position)
                                if (fingers.size == 1) moved = false
                                NativeBridge.nativeSendTouch(slot, true, x, y)
                            } else if (change.pressed) {
                                val finger = fingers[id] ?: continue
                                if ((change.position - finger.third).getDistance() > 12f) moved = true
                                NativeBridge.nativeSendTouch(finger.first, true, x, y)
                            } else {
                                val finger = fingers.remove(id) ?: continue
                                NativeBridge.nativeSendTouch(finger.first, false, x, y)
                                // A short tap without movement is a click of the pad.
                                if (!moved && fingers.isEmpty() && change.uptimeMillis - finger.second < 250) {
                                    clicked = true
                                    NativeBridge.nativeSetTouchpadClick(true)
                                    scope.launch {
                                        delay(90)
                                        NativeBridge.nativeSetTouchpadClick(false)
                                        clicked = false
                                    }
                                }
                            }
                            change.consume()
                        }
                        touching = fingers.isNotEmpty()
                        if (event.type == PointerEventType.Exit && fingers.isEmpty()) touching = false
                    }
                }
            }
        } else {
            base
        },
        contentAlignment = Alignment.Center,
    ) {
        Text("TOUCHPAD", color = Color(0x88FFFFFF), fontSize = 10.sp, fontWeight = FontWeight.Bold)
    }
}

/** Analog stick: the knob follows the finger inside the base; release recentres it. The stick
 *  also recentres when it leaves composition mid-touch (menu opened) - before, the guest kept
 *  the last deflection forever. */
@Composable
fun AnalogStick(size: Dp = 116.dp, enabled: Boolean = true, onMove: (Float, Float) -> Unit) {
    var knob by remember { mutableStateOf(Offset.Zero) }
    DisposableEffect(Unit) {
        onDispose { onMove(0f, 0f) }
    }
    val base = Modifier.size(size)
    Box(
        if (enabled) {
            base.pointerInput(Unit) {
                val radius = this.size.width / 2f
                val centre = Offset(radius, radius)
                var finger: PointerId? = null
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        // Follow the finger that grabbed the stick; a second one landing on it
                        // takes over only after the first lifts.
                        val change = event.changes.firstOrNull { it.id == finger }
                            ?: event.changes.firstOrNull { it.pressed }
                            ?: event.changes.firstOrNull()
                            ?: continue
                        finger = if (change.pressed) change.id else null
                        if (change.pressed) {
                            var delta = change.position - centre
                            val length = hypot(delta.x, delta.y)
                            if (length > radius) delta *= radius / length
                            knob = delta
                            onMove(delta.x / radius, delta.y / radius)
                        } else {
                            knob = Offset.Zero
                            onMove(0f, 0f)
                        }
                        event.changes.forEach { it.consume() }
                    }
                }
            }
        } else {
            base
        },
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val radius = this.size.minDimension / 2f
            drawCircle(Color(0x44222A3A), radius)
            drawCircle(Color(0x44FFFFFF), radius, style = Stroke(width = 2f))
            drawCircle(Color(0x99AABBDD), radius * 0.42f, center = center + knob)
        }
    }
}

@Composable
private fun DPad(scale: Float = 1f, enabled: Boolean = true, onButton: (Int, Boolean) -> Unit) {
    Box(Modifier.size(124.dp * scale), contentAlignment = Alignment.Center) {
        Box(Modifier.align(Alignment.TopCenter)) {
            GamepadButton("▲", 42.dp * scale, 42.dp * scale, scale = scale, enabled = enabled) { onButton(NativeBridge.BTN_UP, it) }
        }
        Box(Modifier.align(Alignment.BottomCenter)) {
            GamepadButton("▼", 42.dp * scale, 42.dp * scale, scale = scale, enabled = enabled) { onButton(NativeBridge.BTN_DOWN, it) }
        }
        Box(Modifier.align(Alignment.CenterStart)) {
            GamepadButton("◀", 42.dp * scale, 42.dp * scale, scale = scale, enabled = enabled) { onButton(NativeBridge.BTN_LEFT, it) }
        }
        Box(Modifier.align(Alignment.CenterEnd)) {
            GamepadButton("▶", 42.dp * scale, 42.dp * scale, scale = scale, enabled = enabled) { onButton(NativeBridge.BTN_RIGHT, it) }
        }
    }
}

@Composable
private fun FaceButtons(
    scale: Float = 1f,
    nintendo: Boolean,
    enabled: Boolean = true,
    onButton: (Int, Boolean) -> Unit,
) {
    // Standard (PS positions): △ north, ✕ south, □ west, ○ east - the confirm button (✕)
    // sits south. Nintendo swaps the position pairs (✕↔○, △↔□), mirroring how the app remaps
    // A↔B and X↔Y on Nintendo-labelled physical pads: the primary action lands at east.
    data class Face(val label: String, val mask: Int, val color: Color)

    val cross = Face("✕", NativeBridge.BTN_CROSS, Color(0xFF4169E1))
    val circle = Face("○", NativeBridge.BTN_CIRCLE, Color(0xFFFF4500))
    val triangle = Face("△", NativeBridge.BTN_TRIANGLE, Color(0xFF00FF7F))
    val square = Face("□", NativeBridge.BTN_SQUARE, Color(0xFFFF69B4))
    val (south, east, north, west) = if (nintendo) {
        listOf(circle, cross, square, triangle)
    } else {
        listOf(cross, circle, triangle, square)
    }
    Box(Modifier.size(124.dp * scale), contentAlignment = Alignment.Center) {
        Box(Modifier.align(Alignment.TopCenter)) {
            GamepadButton(north.label, 44.dp * scale, 44.dp * scale, scale = scale, color = north.color, enabled = enabled) {
                onButton(north.mask, it)
            }
        }
        Box(Modifier.align(Alignment.BottomCenter)) {
            GamepadButton(south.label, 44.dp * scale, 44.dp * scale, scale = scale, color = south.color, enabled = enabled) {
                onButton(south.mask, it)
            }
        }
        Box(Modifier.align(Alignment.CenterStart)) {
            GamepadButton(west.label, 44.dp * scale, 44.dp * scale, scale = scale, color = west.color, enabled = enabled) {
                onButton(west.mask, it)
            }
        }
        Box(Modifier.align(Alignment.CenterEnd)) {
            GamepadButton(east.label, 44.dp * scale, 44.dp * scale, scale = scale, color = east.color, enabled = enabled) {
                onButton(east.mask, it)
            }
        }
    }
}

/**
 * Pressed while any finger that went down on it stays down. Compose hands each pointer to the
 * node it landed on, so several buttons (and the sticks) can be held at the same time.
 * [enabled] false (the editor's preview) keeps it a pure visual: no input leaves the pad.
 */
@Composable
fun GamepadButton(
    label: String,
    width: Dp,
    height: Dp,
    color: Color = Color.White,
    textSize: Int = 16,
    scale: Float = 1f,
    enabled: Boolean = true,
    onPressChanged: (Boolean) -> Unit,
) {
    var isPressed by remember { mutableStateOf(false) }
    val onChange by rememberUpdatedState(onPressChanged)
    DisposableEffect(Unit) {
        onDispose { if (isPressed) onChange(false) }
    }

    val base = Modifier
        .size(width = width, height = height)
        .clip(CircleShape)
        .background(if (isPressed) Color(0x994466AA) else Color(0x66222A3A))
        .border(1.dp, if (isPressed) Color(0xFF00E5FF) else Color(0x44FFFFFF), CircleShape)
    Box(
        if (enabled) {
            base.pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        val down = event.changes.any { it.pressed }
                        if (down != isPressed) {
                            isPressed = down
                            onChange(down)
                        }
                        event.changes.forEach { it.consume() }
                    }
                }
            }
        } else {
            base
        },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            color = if (isPressed) Color(0xFF00E5FF) else color,
            fontWeight = FontWeight.Bold,
            fontSize = (textSize * scale).sp,
        )
    }
}
