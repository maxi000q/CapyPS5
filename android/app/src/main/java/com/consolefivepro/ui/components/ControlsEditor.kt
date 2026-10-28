// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.consolefivepro.ButtonLayout
import com.consolefivepro.ControlCluster
import com.consolefivepro.ControlsLayout
import com.consolefivepro.R
import com.consolefivepro.Settings
import com.consolefivepro.ui.NavKey
import com.consolefivepro.ui.OnNavKey
import com.consolefivepro.ui.theme.Capy

/**
 * Clamps every cluster centre so the whole cluster box fits inside [area] (root px): a layout
 * saved by the old editor (or on another screen size) moves fully inside the pad box, and what
 * the editor shows is what the game gets. Positions stay full-screen fractions, so the saved
 * layout keeps the same relative spot on any screen size.
 */
private fun ControlsLayout.clampedToArea(
    area: Rect, screenW: Float, screenH: Float, density: Density,
): ControlsLayout {
    fun clamp(index: Int, cluster: ControlCluster): ControlCluster {
        val wpx = with(density) { ClusterSizes.width(index, cluster.scale, true).toPx() }
        val hpx = with(density) { ClusterSizes.height(index, cluster.scale).toPx() }
        val x = clampCentreAxis(cluster.x * screenW, area.left, area.right, wpx) / screenW
        val y = clampCentreAxis(cluster.y * screenH, area.top, area.bottom, hpx) / screenH
        return ControlCluster(x, y, cluster.scale)
    }
    return ControlsLayout(clamp(0, left), clamp(1, right), clamp(2, top)).clamped()
}

/**
 * Custom controls editor (v0.0.10pro phase 3, redesigned): the on-screen pad lives in an
 * ISOLATED BOX in the middle of the screen, and every editing tool (button-layout switch,
 * cluster picker, size slider, reset and save) sits OUTSIDE it - in the header above and the
 * tool bar below. The old editor drew the tool panels over the same space the clusters were
 * parked in, so the size slider and the cluster picker ended up stacked on top of the d-pad
 * and the sticks; now a cluster cannot leave its box, so the tools can never cover it and
 * every part of the layout stays grabbable at any screen size.
 *
 * The pad box bounds are handed to the pad preview (VirtualGamepad editBounds) and the draft
 * is clamped into them, so the saved layout - full-screen fractions - shows the clusters at
 * exactly the same relative spot in-game on any screen: positions adapt to the screen while
 * the change ratio the user set is what is stored. Drag a cluster to move it, tap to select
 * it, the slider resizes the selected cluster; Save persists the layout and closes the
 * editor, Back leaves without saving.
 */
@Composable
fun ControlsEditor(
    settings: Settings,
    onDone: () -> Unit,
) {
    val context = LocalContext.current
    // Draft layout: live in the preview, persisted only by Save.
    var draft by remember { mutableStateOf(settings.controlsLayout) }
    var selected by remember { mutableIntStateOf(CLUSTER_LEFT) }

    fun save() {
        settings.updateControlsLayout(draft)
        onDone()
    }

    OnNavKey { key ->
        when (key) {
            NavKey.Up -> selected = (selected - 1).mod(3)
            NavKey.Down -> selected = (selected + 1).mod(3)
            NavKey.Left -> draft = draft.scaled(selected, -0.05f)
            NavKey.Right -> draft = draft.scaled(selected, 0.05f)
            NavKey.Confirm -> save()
            NavKey.Back, NavKey.Menu, NavKey.Secondary -> onDone()
            else -> Unit
        }
    }

    BoxWithConstraints(Modifier.fillMaxSize().background(Color(0xE6000000))) {
        // Compact phones (landscape, so the height is the tight axis): the two panels together
        // could eat half the screen and bury the clusters. Everything they draw - text, paddings,
        // row heights - scales with the available space, so on a big screen the editor looks
        // exactly as before and on a small one the panels shrink until both they and the pad fit.
        val uiScale = (minOf(maxWidth, maxHeight).value / 460f).coerceIn(0.7f, 1f)
        fun Dp.scaled(): Dp = this * uiScale

        val titleStyle = MaterialTheme.typography.titleLarge
            .copy(fontSize = MaterialTheme.typography.titleLarge.fontSize * uiScale)
        val bodyStyle = MaterialTheme.typography.bodySmall
            .copy(fontSize = MaterialTheme.typography.bodySmall.fontSize * uiScale)
        val labelStyle = MaterialTheme.typography.labelLarge
            .copy(fontSize = MaterialTheme.typography.labelLarge.fontSize * uiScale)

        // The isolated pad box: bounds in this root's coordinates, observed after layout.
        var padBounds by remember { mutableStateOf<Rect?>(null) }
        val rootW = constraints.maxWidth.toFloat()
        val rootH = constraints.maxHeight.toFloat()
        val density = LocalDensity.current

        // Whenever the box is (re)laid out or a cluster is resized, pull any cluster that no
        // longer fits back inside it - a layout saved by the old editor or on a bigger screen
        // moves fully into the box, and a grown cluster keeps its whole box visible. Position
        // drags arrive already clamped (VirtualGamepad reports the clamped centre).
        LaunchedEffect(padBounds, draft.left.scale, draft.right.scale, draft.top.scale) {
            val bounds = padBounds ?: return@LaunchedEffect
            draft = draft.clampedToArea(bounds, rootW, rootH, density)
        }

        Column(Modifier.fillMaxSize()) {
            // Header (outside the box): which controls the user has chosen, with the button
            // layout switchable here.
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Capy.Surface)
                    .padding(horizontal = 16.dp.scaled(), vertical = 10.dp.scaled()),
                verticalArrangement = Arrangement.spacedBy(4.dp.scaled()),
            ) {
                Text(stringResource(R.string.custom_controls), style = titleStyle)
                Text(
                    stringResource(
                        R.string.controls_editor_type,
                        settings.touchControls.label(context),
                        settings.buttonLayout.label(context),
                    ) + " · " + stringResource(R.string.custom_controls_hint),
                    style = bodyStyle,
                    color = Capy.TextSecondary,
                )
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Capy.Background, RoundedCornerShape(8.dp.scaled()))
                        .padding(3.dp.scaled()),
                    horizontalArrangement = Arrangement.spacedBy(3.dp.scaled()),
                ) {
                    ButtonLayout.entries.forEach { layout ->
                        val isSelected = layout == settings.buttonLayout
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .background(
                                    if (isSelected) Capy.Accent else Color.Transparent,
                                    RoundedCornerShape(6.dp.scaled()),
                                )
                                .clickable { settings.updateButtonLayout(layout) }
                                .padding(vertical = 7.dp.scaled()),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                layout.label(context),
                                style = labelStyle,
                                color = if (isSelected) Color.White else Capy.TextSecondary,
                            )
                        }
                    }
                }
            }

            // The isolated pad box the clusters live in - visibly bounded, far from the tools.
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = 10.dp.scaled(), vertical = 6.dp.scaled())
                    .onGloballyPositioned { padBounds = it.boundsInRoot() }
                    .background(Color(0x11FFFFFF), RoundedCornerShape(14.dp.scaled()))
                    .border(1.dp.scaled(), Color(0x55FFFFFF), RoundedCornerShape(14.dp.scaled())),
            )

            // Tool bar (outside the box): cluster picker, size slider, reset and save.
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Capy.Surface)
                    .padding(horizontal = 16.dp.scaled(), vertical = 12.dp.scaled()),
                verticalArrangement = Arrangement.spacedBy(8.dp.scaled()),
            ) {
                val clusterLabels = listOf(
                    stringResource(R.string.controls_cluster_left),
                    stringResource(R.string.controls_cluster_right),
                    stringResource(R.string.controls_cluster_top),
                )
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Capy.Background, RoundedCornerShape(8.dp.scaled()))
                        .padding(3.dp.scaled()),
                    horizontalArrangement = Arrangement.spacedBy(3.dp.scaled()),
                ) {
                    clusterLabels.forEachIndexed { index, label ->
                        val isSelected = index == selected
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .background(
                                    if (isSelected) Capy.Accent else Color.Transparent,
                                    RoundedCornerShape(6.dp.scaled()),
                                )
                                .clickable { selected = index }
                                .padding(vertical = 7.dp.scaled()),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                label,
                                style = labelStyle,
                                color = if (isSelected) Color.White else Capy.TextSecondary,
                            )
                        }
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.controls_size), style = labelStyle)
                    Spacer(Modifier.width(12.dp.scaled()))
                    Slider(
                        value = draft.cluster(selected).scale,
                        onValueChange = { value ->
                            draft = draft.withCluster(selected, draft.cluster(selected).copy(scale = value))
                        },
                        valueRange = 0.6f..1.8f,
                        colors = SliderDefaults.colors(thumbColor = Capy.Accent, activeTrackColor = Capy.Accent),
                        modifier = Modifier.weight(1f),
                    )
                    Spacer(Modifier.width(12.dp.scaled()))
                    Text(
                        "%.0f%%".format(draft.cluster(selected).scale * 100),
                        style = labelStyle,
                        color = Capy.Accent,
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp.scaled())) {
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .border(1.dp, Capy.TextSecondary, RoundedCornerShape(8.dp.scaled()))
                            .clickable { draft = ControlsLayout() }
                            .padding(vertical = 9.dp.scaled()),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            stringResource(R.string.controls_reset),
                            style = labelStyle,
                            color = Capy.TextSecondary,
                        )
                    }
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .background(Capy.Accent, RoundedCornerShape(8.dp.scaled()))
                            .clickable { save() }
                            .padding(vertical = 9.dp.scaled()),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            stringResource(R.string.controls_save),
                            style = labelStyle,
                            color = Color.White,
                        )
                    }
                }
                Spacer(Modifier.height(2.dp.scaled()))
            }
        }

        // The pad LAST, over the whole root: its clusters are clamped inside the isolated box
        // (editBounds), so they never sit under the header or the tool bar, and its saved
        // positions are full-screen fractions - the in-game overlay shows the same layout at
        // the same relative spot on any screen size.
        VirtualGamepad(
            modifier = Modifier.fillMaxSize(),
            alpha = 1f,
            showTouchpad = true,
            layout = draft,
            buttonLayout = settings.buttonLayout,
            editMode = true,
            editBounds = padBounds,
            selectedCluster = selected,
            onClusterSelect = { selected = it },
            onClusterMoved = { cluster, x, y ->
                draft = draft.withCluster(cluster, draft.cluster(cluster).copy(x = x, y = y))
            },
        )
    }
}
