// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.SaveAlt
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Speed
import androidx.compose.material.icons.outlined.SportsEsports
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.platform.LocalContext
import com.consolefivepro.ButtonLayout
import com.consolefivepro.Localization
import androidx.compose.ui.res.stringResource
import com.consolefivepro.GpuDrivers
import com.consolefivepro.GpuInfo
import com.consolefivepro.R
import com.consolefivepro.CoreOption
import com.consolefivepro.GameSaves
import com.consolefivepro.OverlayMode
import com.consolefivepro.ScreenAspect
import com.consolefivepro.Settings
import com.consolefivepro.TouchControlsMode
import com.consolefivepro.ui.NavKey
import com.consolefivepro.ui.OnNavKey
import com.consolefivepro.ui.components.ControlsEditor
import com.consolefivepro.ui.theme.Capy
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Title of a full-screen page with a back arrow in front of it. */
@Composable
fun ScreenHeader(title: String, onBack: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onBack)
            .padding(start = 14.dp, end = 16.dp, top = 4.dp, bottom = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.hint_back),
            Modifier.padding(end = 10.dp).size(24.dp), tint = Capy.TextPrimary,
        )
        Text(title, style = MaterialTheme.typography.headlineSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** One setting: a value cycled with left/right or confirm, or an action run on confirm. */
class SettingItem(
    val title: String,
    val subtitle: String? = null,
    val value: String? = null,
    val danger: Boolean = false,
    val onStep: ((Int) -> Unit)? = null,
    val onConfirm: (() -> Unit)? = null,
)

private class SettingsCategory(val title: String, val icon: ImageVector, val items: () -> List<SettingItem>)

private fun openLink(context: android.content.Context, url: Int) {
    runCatching {
        context.startActivity(
            android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(context.getString(url)))
        )
    }
}

fun <T> cycle(values: List<T>, current: T, delta: Int): T =
    values[(values.indexOf(current).coerceAtLeast(0) + delta).mod(values.size)]

/** Full-screen settings with categories on the left, usable with touch or a controller. */
@Composable
fun SettingsScreen(
    settings: Settings,
    versionText: String,
    onAddFolder: () -> Unit,
    onImportDriver: () -> Unit,
    driverMessage: String?,
    onClearShaderCache: () -> String,
    onSaveLog: () -> String?,
    onCheckUpdate: (() -> Unit)? = null,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    var message by remember { mutableStateOf<String?>(null) }
    var showDrivers by remember { mutableStateOf(false) }
    // v0.0.10pro phase 3: the on-screen controls layout editor (full screen).
    var showControlsEditor by remember { mutableStateOf(false) }
    var savesMessage by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val exportSaves = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip")
    ) { uri ->
        if (uri != null) {
            scope.launch {
                val ok = withContext(Dispatchers.IO) { GameSaves.exportAll(context, uri) }
                val any = withContext(Dispatchers.IO) { GameSaves.hasSaves(context) }
                savesMessage = context.getString(
                    when {
                        ok -> R.string.saves_exported
                        any -> R.string.saves_export_failed
                        else -> R.string.saves_export_empty
                    }
                )
            }
        }
    }
    val importSaves = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            scope.launch {
                val restored = withContext(Dispatchers.IO) { GameSaves.importAll(context, uri) }
                savesMessage = context.getString(
                    when {
                        restored > 0 -> R.string.saves_imported
                        restored == 0 -> R.string.saves_import_empty
                        else -> R.string.saves_import_failed
                    }
                )
            }
        }
    }
    // Read so the option rows recompose when a value changes.
    val optionsVersion = settings.coreOptionsVersion

    fun coreItem(option: CoreOption): SettingItem {
        val values = option.choices.map { it.first }
        val current = settings.coreOption(option)
        return SettingItem(
            option.title(context), option.description(context), option.label(context, current),
            onStep = { settings.updateCoreOption(option, cycle(values, current, it)) },
        )
    }

    val categories = remember(
        optionsVersion, settings.libraryPaths, message, driverMessage, settings.updateChecks,
        settings.adaptiveResolution, settings.autoSave, savesMessage, settings.screenAspect,
    ) {
        listOf(
            SettingsCategory(context.getString(R.string.category_graphics), Icons.Outlined.Image) {
                val drivers = GpuDrivers.list(context)
                val ids = listOf("") + drivers.map { it.id }
                val current = settings.gpuDriver().takeIf { id -> id in ids }.orEmpty()
                val scales = CoreOption.ResolutionScale.choices.map { it.first }
                val ueScale = settings.ueResolution()
                // v0.0.10pro phase 3: the Vulkan-version option exists only on Mali devices and
                // only while the Vulkan backend is selected (the row recomposes through
                // optionsVersion when the backend changes).
                val showVulkanVersion =
                    GpuInfo.isMali && settings.coreOption(CoreOption.GraphicsBackend) == 0L
                listOfNotNull(
                    coreItem(CoreOption.GraphicsBackend),
                    if (showVulkanVersion) coreItem(CoreOption.VulkanVersion) else null,
                    coreItem(CoreOption.ResolutionScale),
                    SettingItem(
                        context.getString(R.string.ue_resolution),
                        context.getString(R.string.ue_resolution_desc),
                        CoreOption.ResolutionScale.label(context, ueScale),
                        onStep = { settings.updateUeResolution(cycle(scales, ueScale, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.screen_aspect),
                        context.getString(R.string.screen_aspect_desc),
                        settings.screenAspect.label(context),
                        onStep = { settings.updateScreenAspect(cycle(ScreenAspect.entries, settings.screenAspect, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.gpu_driver),
                        context.getString(R.string.gpu_driver_desc),
                        drivers.firstOrNull { it.id == current }?.label ?: context.getString(R.string.gpu_driver_system),
                        onStep = { settings.updateGpuDriver(cycle(ids, current, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.driver_download), context.getString(R.string.driver_download_desc),
                        onConfirm = { showDrivers = true },
                    ),
                    SettingItem(
                        context.getString(R.string.gpu_driver_import),
                        context.getString(R.string.gpu_driver_import_desc),
                        onConfirm = onImportDriver,
                    ),
                ) + listOfNotNull(
                    drivers.firstOrNull { it.id == current }?.let { driver ->
                        SettingItem(
                            context.getString(R.string.gpu_driver_delete, driver.label),
                            context.getString(R.string.gpu_driver_delete_desc), danger = true,
                            onConfirm = {
                                GpuDrivers.delete(context, driver.id)
                                settings.updateGpuDriver("")
                            },
                        )
                    },
                    driverMessage?.let { SettingItem(it) },
                )
            },
            SettingsCategory(context.getString(R.string.category_performance), Icons.Outlined.Speed) {
                // v0.0.9pro: bandwidth-tuned defaults for Mali GPUs; the value names the detected
                // GPU so it is obvious whether Auto applies the profile.
                val maliValues = listOf(2L, 1L, 0L)
                val maliCurrent = settings.coreOption(CoreOption.MaliProfile)
                val maliLabel = (when (maliCurrent) {
                    1L -> context.getString(R.string.mali_profile_on)
                    0L -> context.getString(R.string.mali_profile_off)
                    else -> context.getString(R.string.mali_profile_auto)
                }) + " \u00B7 " + GpuInfo.label
                // v0.0.10pro phase 2: Snapdragon-tuned defaults, same shape as the Mali row.
                val adrenoValues = listOf(2L, 1L, 0L)
                val adrenoCurrent = settings.coreOption(CoreOption.AdrenoProfile)
                val adrenoLabel = (when (adrenoCurrent) {
                    1L -> context.getString(R.string.adreno_profile_on)
                    0L -> context.getString(R.string.adreno_profile_off)
                    else -> context.getString(R.string.adreno_profile_auto)
                }) + " \u00B7 " + GpuInfo.label
                listOf(
                    SettingItem(
                        context.getString(R.string.option_mali_profile),
                        context.getString(R.string.option_mali_profile_desc),
                        maliLabel,
                        onStep = { settings.updateCoreOption(CoreOption.MaliProfile, cycle(maliValues, maliCurrent, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.option_adreno_profile),
                        context.getString(R.string.option_adreno_profile_desc),
                        adrenoLabel,
                        onStep = { settings.updateCoreOption(CoreOption.AdrenoProfile, cycle(adrenoValues, adrenoCurrent, it)) },
                    ),
                    coreItem(CoreOption.RefreshRate),
                    coreItem(CoreOption.FrameGeneration),
                    coreItem(CoreOption.AsyncShaders),
                    coreItem(CoreOption.StreamingBuffer),
                    coreItem(CoreOption.ReadAhead),
                    coreItem(CoreOption.YieldSleep),
                    coreItem(CoreOption.VerboseLogging),
                    coreItem(CoreOption.FenceFallback),
                    coreItem(CoreOption.GuestReadCache),
                    coreItem(CoreOption.PerfCores),
                    coreItem(CoreOption.PresentMode),
                    SettingItem(
                        context.getString(R.string.perf_overlay), context.getString(R.string.perf_overlay_desc),
                        settings.overlayMode.label(context),
                        onStep = { settings.updateOverlayMode(cycle(OverlayMode.entries, settings.overlayMode, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.adaptive_resolution),
                        context.getString(R.string.adaptive_resolution_desc),
                        context.getString(if (settings.adaptiveResolution) R.string.enabled else R.string.disabled),
                        onStep = { settings.updateAdaptiveResolution(!settings.adaptiveResolution) },
                    ),
                )
            },
            SettingsCategory(context.getString(R.string.category_controls), Icons.Outlined.SportsEsports) {
                listOf(
                    SettingItem(
                        context.getString(R.string.touch_controls), context.getString(R.string.touch_controls_desc),
                        settings.touchControls.label(context),
                        onStep = {
                            settings.updateTouchControls(cycle(TouchControlsMode.entries, settings.touchControls, it))
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.touchpad_overlay),
                        context.getString(R.string.touchpad_overlay_desc),
                        settings.touchpadOverlay.label(context),
                        onStep = {
                            settings.updateTouchpadOverlay(cycle(TouchControlsMode.entries, settings.touchpadOverlay, it))
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.button_layout),
                        context.getString(R.string.button_layout_desc),
                        settings.buttonLayout.label(context),
                        onStep = { settings.updateButtonLayout(cycle(ButtonLayout.entries, settings.buttonLayout, it)) },
                    ),
                    SettingItem(
                        context.getString(R.string.controls_opacity), null, "%.0f %%".format(settings.controlsOpacity * 100),
                        onStep = { settings.updateControlsOpacity((settings.controlsOpacity + it * 0.1f).coerceIn(0.1f, 1f)) },
                    ),
                    // v0.0.10pro phase 3: open the on-screen pad layout editor.
                    SettingItem(
                        context.getString(R.string.custom_controls),
                        context.getString(R.string.custom_controls_desc),
                        onConfirm = { showControlsEditor = true },
                    ),
                )
            },
            SettingsCategory(context.getString(R.string.category_saves), Icons.Outlined.SaveAlt) {
                listOf(
                    SettingItem(
                        context.getString(R.string.auto_save),
                        context.getString(R.string.auto_save_desc),
                        context.getString(if (settings.autoSave) R.string.enabled else R.string.disabled),
                        onStep = { settings.updateAutoSave(!settings.autoSave) },
                    ),
                    SettingItem(
                        context.getString(R.string.saves_export),
                        context.getString(R.string.saves_export_desc),
                        onConfirm = {
                            exportSaves.launch(
                                "ConsoleFivePro-saves-" +
                                    SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".zip"
                            )
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.saves_import),
                        context.getString(R.string.saves_import_desc),
                        onConfirm = { importSaves.launch(arrayOf("application/zip", "application/octet-stream")) },
                    ),
                ) + listOfNotNull(savesMessage?.let { SettingItem(it) })
            },
            SettingsCategory(context.getString(R.string.category_folders), Icons.Outlined.Folder) {
                settings.libraryPaths.map { path ->
                    SettingItem(
                        path,
                        context.getString(
                            if (settings.libraryPaths.size > 1) R.string.folder_remove else R.string.folder_main,
                        ),
                        onConfirm = if (settings.libraryPaths.size > 1) ({ settings.removeLibraryPath(path) }) else null,
                    )
                } + SettingItem(
                    context.getString(R.string.folder_add), context.getString(R.string.folder_add_desc),
                    onConfirm = onAddFolder,
                )
            },
            SettingsCategory(context.getString(R.string.category_system), Icons.Outlined.Settings) {
                listOf(
                    // v0.0.10pro: the interface language. Every string comes from the engine
                    // tree's lang/ folder; switching restarts the screens in the new language
                    // (Arabic and Kurdish mirrored right-to-left).
                    SettingItem(
                        context.getString(R.string.language),
                        context.getString(R.string.language_desc),
                        Localization.currentLanguage(context).label,
                        onStep = { step ->
                            val current = Localization.currentLanguage(context)
                            Localization.setLanguage(context, cycle(Localization.languages, current, step))
                        },
                    ),
                    SettingItem(
                        context.getString(R.string.update_checks), context.getString(R.string.update_checks_desc),
                        context.getString(if (settings.updateChecks) R.string.enabled else R.string.disabled),
                        onStep = { settings.updateUpdateChecks(!settings.updateChecks) },
                    ),
                    SettingItem(
                        context.getString(R.string.clear_shader_cache), context.getString(R.string.clear_shader_cache_desc),
                        danger = true,
                        onConfirm = { message = onClearShaderCache() },
                    ),
                    SettingItem(
                        context.getString(R.string.save_log), context.getString(R.string.save_log_desc),
                        onConfirm = {
                            message = onSaveLog()?.let { context.getString(R.string.log_saved_in, it) }
                                ?: context.getString(R.string.log_save_failed)
                        },
                    ),
                ) + listOfNotNull(
                    onCheckUpdate?.let {
                        SettingItem(
                            context.getString(R.string.update_check_now),
                            context.getString(R.string.update_check_now_desc),
                            onConfirm = it,
                        )
                    },
                    message?.let { SettingItem(it) },
                )
            },
            SettingsCategory(context.getString(R.string.category_about), Icons.Outlined.Info) {
                listOf(
                    SettingItem("Console Five Pro", versionText),
                    SettingItem(
                        context.getString(R.string.support_discord), context.getString(R.string.discord_link),
                        onConfirm = { openLink(context, R.string.discord_link) },
                    ),
                    SettingItem(
                        context.getString(R.string.license),
                        context.getString(R.string.license_desc),
                    ),
                    SettingItem(
                        context.getString(R.string.device),
                        "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} · ${android.os.Build.SOC_MODEL} · " +
                            "Android ${android.os.Build.VERSION.RELEASE}",
                    ),
                )
            },
        )
    }

    var category by remember { mutableIntStateOf(0) }
    var inContent by remember { mutableStateOf(false) }
    var row by remember { mutableIntStateOf(0) }
    val items = categories[category].items()
    if (row >= items.size) row = (items.size - 1).coerceAtLeast(0)

    OnNavKey(enabled = !showDrivers) { key ->
        if (!inContent) {
            when (key) {
                NavKey.Up -> { category = (category - 1).mod(categories.size); row = 0 }
                NavKey.Down -> { category = (category + 1).mod(categories.size); row = 0 }
                NavKey.Right, NavKey.Confirm -> inContent = true
                NavKey.Back, NavKey.Menu -> onClose()
                else -> Unit
            }
        } else {
            val item = items.getOrNull(row)
            when (key) {
                NavKey.Up -> row = (row - 1).mod(items.size.coerceAtLeast(1))
                NavKey.Down -> row = (row + 1).mod(items.size.coerceAtLeast(1))
                NavKey.Left -> if (item?.onStep != null) item.onStep.invoke(-1) else inContent = false
                NavKey.Right -> item?.onStep?.invoke(1)
                NavKey.Confirm -> item?.onConfirm?.invoke() ?: item?.onStep?.invoke(1)
                NavKey.Back -> inContent = false
                NavKey.Menu -> onClose()
                else -> Unit
            }
        }
    }

    if (showDrivers) {
        DriverDownloadScreen(settings, onClose = { showDrivers = false })
        return
    }

    if (showControlsEditor) {
        ControlsEditor(settings, onDone = { showControlsEditor = false })
        return
    }
    Row(Modifier.fillMaxSize().background(Capy.Background)) {
        Column(
            // Scrollable: with the system bars hidden and seven categories, small screens cut
            // the list off around About (v0.0.8proplus) — now the column scrolls instead.
            Modifier
                .width(260.dp)
                .fillMaxHeight()
                .background(Capy.Surface)
                .verticalScroll(rememberScrollState())
                .padding(vertical = 22.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            // The system bars are hidden, so touch users need their own way back.
            ScreenHeader(stringResource(R.string.settings), onBack = onClose)
            categories.forEachIndexed { i, entry ->
                val selected = i == category
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(if (selected) Capy.SurfaceHigh else Color.Transparent)
                        .clickable { category = i; row = 0; inContent = false }
                        .padding(horizontal = 24.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(entry.icon, null, Modifier.size(20.dp), tint = if (selected) Capy.TextPrimary else Capy.TextSecondary)
                    Spacer(Modifier.width(14.dp))
                    Text(
                        entry.title,
                        style = MaterialTheme.typography.titleMedium,
                        color = if (selected) Capy.TextPrimary else Capy.TextSecondary,
                    )
                }
            }
        }
        Column(
            Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()).padding(28.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(categories[category].title, style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(6.dp))
            items.forEachIndexed { i, item ->
                SettingRowView(item, focused = inContent && i == row) { row = i; inContent = true }
            }
        }
    }
}

@Composable
fun SettingRowView(item: SettingItem, focused: Boolean, onFocus: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (focused) Capy.SurfaceHigh else Capy.Surface, shape)
            .border(1.dp, if (focused) Capy.Accent else Color.Transparent, shape)
            .clickable {
                onFocus()
                item.onConfirm?.invoke() ?: item.onStep?.invoke(1)
            }
            .padding(horizontal = 18.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(item.title, style = MaterialTheme.typography.titleMedium, color = if (item.danger) Capy.Bad else Capy.TextPrimary)
            item.subtitle?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = Capy.TextSecondary)
            }
        }
        item.value?.let {
            Box(Modifier.padding(start = 12.dp)) {
                Text(
                    if (item.onStep != null && focused) "‹  $it  ›" else it,
                    style = MaterialTheme.typography.titleMedium,
                    color = Capy.Accent,
                )
            }
        }
    }
}
