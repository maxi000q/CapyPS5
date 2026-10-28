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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.consolefivepro.ButtonLayout
import com.consolefivepro.ControlsLayout
import com.consolefivepro.R
import com.consolefivepro.Settings
import com.consolefivepro.ui.NavKey
import com.consolefivepro.ui.OnNavKey
import com.consolefivepro.ui.theme.Capy

/**
 * Custom controls editor (v0.0.10pro phase 3): the on-screen pad drawn as a live preview over a
 * dimmed background. Drag a cluster to move it, tap to select it, the slider resizes the selected
 * cluster; Save persists the layout and closes the editor, Back leaves without saving.
 *
 * The header shows the type of controls the user has chosen (touch mode and button layout) and
 * the button layout can be switched right here; the pad preview follows it immediately.
 *
 * The pad is drawn ABOVE the two panels, not beneath them: a cluster the user parked over the
 * header or the bottom bar stays fully visible and stays grabbable - nothing but the user's own
 * cluster ever covers a cluster, so every part of the layout is editable at any screen size.
 * Taps that land on a panel outside a cluster still reach the panel (the pad itself only
 * consumes touches inside its cluster boxes).
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

        // Header: which controls the user has chosen, with the button layout switchable here.
        Column(
            Modifier
                .align(Alignment.TopStart)
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

        // Bottom bar: cluster picker, size slider, reset and save.
        Column(
            Modifier
                .align(Alignment.BottomCenter)
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

        // The pad LAST, on top of both panels: clusters stay visible and grabbable even when
        // the user parks them over the header or the bottom bar (see the class comment).
        VirtualGamepad(
            modifier = Modifier.fillMaxSize(),
            alpha = 1f,
            showTouchpad = true,
            layout = draft,
            buttonLayout = settings.buttonLayout,
            editMode = true,
            selectedCluster = selected,
            onClusterSelect = { selected = it },
            onClusterMove = { cluster, dx, dy -> draft = draft.moved(cluster, dx, dy) },
        )
    }
}
