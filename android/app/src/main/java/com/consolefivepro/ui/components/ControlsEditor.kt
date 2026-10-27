// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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

    Box(Modifier.fillMaxSize().background(Color(0xE6000000))) {
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

        // Header: which controls the user has chosen, with the button layout switchable here.
        Column(
            Modifier
                .align(Alignment.TopStart)
                .fillMaxWidth()
                .background(Capy.Surface)
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(stringResource(R.string.custom_controls), style = MaterialTheme.typography.titleLarge)
            Text(
                stringResource(
                    R.string.controls_editor_type,
                    settings.touchControls.label(context),
                    settings.buttonLayout.label(context),
                ) + " · " + stringResource(R.string.custom_controls_hint),
                style = MaterialTheme.typography.bodySmall,
                color = Capy.TextSecondary,
            )
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Capy.Background, RoundedCornerShape(8.dp))
                    .padding(3.dp),
                horizontalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                ButtonLayout.entries.forEach { layout ->
                    val isSelected = layout == settings.buttonLayout
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .background(
                                if (isSelected) Capy.Accent else Color.Transparent,
                                RoundedCornerShape(6.dp),
                            )
                            .clickable { settings.updateButtonLayout(layout) }
                            .padding(vertical = 7.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            layout.label(context),
                            style = MaterialTheme.typography.labelLarge,
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
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            val clusterLabels = listOf(
                stringResource(R.string.controls_cluster_left),
                stringResource(R.string.controls_cluster_right),
                stringResource(R.string.controls_cluster_top),
            )
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Capy.Background, RoundedCornerShape(8.dp))
                    .padding(3.dp),
                horizontalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                clusterLabels.forEachIndexed { index, label ->
                    val isSelected = index == selected
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .background(
                                if (isSelected) Capy.Accent else Color.Transparent,
                                RoundedCornerShape(6.dp),
                            )
                            .clickable { selected = index }
                            .padding(vertical = 7.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            label,
                            style = MaterialTheme.typography.labelLarge,
                            color = if (isSelected) Color.White else Capy.TextSecondary,
                        )
                    }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.controls_size), style = MaterialTheme.typography.labelLarge)
                Spacer(Modifier.width(12.dp))
                Slider(
                    value = draft.cluster(selected).scale,
                    onValueChange = { value ->
                        draft = draft.withCluster(selected, draft.cluster(selected).copy(scale = value))
                    },
                    valueRange = 0.6f..1.8f,
                    colors = SliderDefaults.colors(thumbColor = Capy.Accent, activeTrackColor = Capy.Accent),
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(12.dp))
                Text(
                    "%.0f%%".format(draft.cluster(selected).scale * 100),
                    style = MaterialTheme.typography.labelLarge,
                    color = Capy.Accent,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .border(1.dp, Capy.TextSecondary, RoundedCornerShape(8.dp))
                        .clickable { draft = ControlsLayout() }
                        .padding(vertical = 9.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        stringResource(R.string.controls_reset),
                        style = MaterialTheme.typography.labelLarge,
                        color = Capy.TextSecondary,
                    )
                }
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .background(Capy.Accent, RoundedCornerShape(8.dp))
                        .clickable { save() }
                        .padding(vertical = 9.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        stringResource(R.string.controls_save),
                        style = MaterialTheme.typography.labelLarge,
                        color = Color.White,
                    )
                }
            }
            Spacer(Modifier.height(2.dp))
        }
    }
}
