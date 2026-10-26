// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.content.Intent
import android.os.Bundle
import android.os.Environment
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.consolefivepro.model.GameItem
import com.consolefivepro.ui.NavKey
import com.consolefivepro.ui.OnNavKey
import com.consolefivepro.ui.screens.HomeScreen
import com.consolefivepro.ui.theme.Capy
import com.consolefivepro.ui.theme.CapyPS5Theme
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.consolefivepro.ui.components.SupportDialog
import com.consolefivepro.ui.components.UpdateDialog
import com.consolefivepro.ui.components.WhatsNewDialog

/** Launcher: game library and settings. Each game runs in GameActivity, in its own process. */
class MainActivity : CapyActivity() {

    private lateinit var settings: Settings
    private val libraryDir get() = File(settings.libraryPath)
    // Bumped when all-files access may have just been granted, so the library is scanned again.
    private var storageCheck by mutableIntStateOf(0)
    // Fatal report the last game session left behind, shown once when the launcher returns.
    private var lastError by mutableStateOf<String?>(null)
    // Text of the card shown while a picked executable is being copied into the app (v0.0.8proplus).
    private var importStatus by mutableStateOf<String?>(null)
    // Rejection reason of the last import attempt (a string resource), toasted to the user.
    private var importReject: Int? = null
    // Newer release found at start, and whether this launch shows the support card.
    private var update by mutableStateOf<Release?>(null)
    private var showSupport by mutableStateOf(false)
    // First start after an update: the "what's new" card for the installed version.
    private var showWhatsNew by mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        // Games are read by path, which needs all-files access; a fresh install asks for it once.
        if (!Environment.isExternalStorageManager()) {
            runCatching {
                startActivity(
                    Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                        .setData(android.net.Uri.parse("package:$packageName"))
                )
            }
        } else if (!libraryDir.exists()) {
            runCatching { libraryDir.mkdirs() }
        }

        val versionText = Updater.versionText(this)
        val installed = Updater.installedVersion(this)
        if (savedInstanceState == null) {
            val launches = settings.countLaunch()
            showSupport = launches % SUPPORT_EVERY == 0
            // Shown after an update, not on a fresh install; v0.0.1 did not record a version, so
            // an earlier launch also counts as an update.
            val seen = settings.lastSeenVersion
            showWhatsNew = seen != installed && (seen.isNotEmpty() || launches > 1)
            settings.lastSeenVersion = installed
            if (settings.updateChecks) {
                lifecycleScope.launch {
                    update = Updater.checkForUpdate(installed)?.takeIf { it.tag != settings.skippedRelease }
                }
            }
        }
        // `adb shell am start ... --es launch_elf <path> [--es launch_dir <dir>]` starts a game directly.
        // The launcher activity is exported, so any app on the device could feed an arbitrary
        // path into the native loader (v0.0.8pro): only files inside a user-chosen library
        // folder are accepted now, and a launch_dir outside them is ignored.
        intent?.getStringExtra("launch_elf")?.let { elfPath ->
            val elf = File(elfPath)
            val libraryRoots = settings.libraryPaths.mapNotNull { root ->
                runCatching { File(root).canonicalFile }.getOrNull()
            }
            val canonicalElf = runCatching { elf.canonicalFile }.getOrNull()
            fun insideLibrary(candidate: File?): Boolean =
                candidate != null && libraryRoots.any { root ->
                    candidate == root || candidate.path.startsWith(root.path + File.separator)
                }
            val requestedDir = intent.getStringExtra("launch_dir")?.let(::File)
            val canonicalDir = runCatching { requestedDir?.canonicalFile }.getOrNull()
            if (canonicalElf != null && canonicalElf.isFile && insideLibrary(canonicalElf)) {
                val dir = when {
                    canonicalDir != null && (insideLibrary(canonicalDir) ||
                        canonicalDir == canonicalElf.parentFile) -> canonicalDir
                    else -> canonicalElf.parentFile
                } ?: return@let
                GameActivity.launch(this, GameItem.fromGameDir(dir, canonicalElf))
            }
        }

        setContent {
            CapyPS5Theme {
                val games = remember { mutableStateListOf<GameItem>() }
                LaunchedEffect(settings.libraryPaths, storageCheck) {
                    // Folder walks + param.sfo reads per game stay off the main thread;
                    // swap the list in one go so the grid does not flicker while scanning.
                    val found = withContext(Dispatchers.IO) { scanLibrary() }
                    games.clear()
                    games.addAll(found)
                }
                Box(Modifier.fillMaxSize()) {
                    HomeScreen(
                        games = games,
                        libraryPath = settings.libraryPath,
                        settings = settings,
                        versionText = versionText,
                        onGameSelected = { GameActivity.launch(this@MainActivity, it) },
                        onAddGameFile = { uriString ->
                            lifecycleScope.launch {
                                // A picked eboot can be hundreds of megabytes; the copy stays on
                                // IO, is validated before any byte is written and reports progress
                                // (v0.0.8proplus: package files are rejected up front instead of
                                // being copied for minutes and then failing to boot).
                                val localFile = withContext(Dispatchers.IO) {
                                    importReject = null
                                    if (uriString.startsWith("content://")) {
                                        copyUriToLocal(android.net.Uri.parse(uriString)) { name, percent ->
                                            importStatus = when {
                                                name == null -> null
                                                percent < 0 -> getString(R.string.importing_game, name)
                                                else -> getString(R.string.importing_progress, name, percent)
                                            }
                                        }
                                    } else {
                                        val picked = File(uriString)
                                        if (picked.isFile && hasGameExecutableMagic(picked)) picked
                                        else {
                                            importReject = R.string.import_not_game
                                            null
                                        }
                                    }
                                }
                                importStatus = null
                                if (localFile == null) {
                                    android.widget.Toast.makeText(
                                        this@MainActivity,
                                        importReject ?: R.string.import_failed,
                                        android.widget.Toast.LENGTH_LONG,
                                    ).show()
                                } else {
                                    val newGame = GameItem(
                                        titleId = "LOCAL",
                                        titleName = localFile.name,
                                        directoryPath = localFile.parent ?: filesDir.absolutePath,
                                        elfPath = localFile.absolutePath,
                                    )
                                    games.add(newGame)
                                    GameActivity.launch(this@MainActivity, newGame)
                                }
                            }
                        },
                        onRefresh = {
                            games.clear()
                            games.addAll(scanLibrary())
                        },
                        onLibraryPicked = { settings.addLibraryPath(it) },
                        onClearShaderCache = ::clearShaderCache,
                        onSaveLog = ::saveLog,
                        onCheckUpdate = ::checkUpdateNow,
                        overlayActive = lastError != null || showWhatsNew || update != null || showSupport ||
                            importStatus != null,
                    )
                    // Copy-in-progress card: the picked executable is being imported.
                    importStatus?.let { status ->
                        Box(
                            Modifier.fillMaxSize().background(Capy.Scrim),
                            contentAlignment = Alignment.Center,
                        ) {
                            Column(
                                modifier = Modifier
                                    .background(Capy.Surface, RoundedCornerShape(16.dp))
                                    .padding(horizontal = 32.dp, vertical = 26.dp),
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.spacedBy(12.dp),
                            ) {
                                androidx.compose.material3.CircularProgressIndicator(color = Capy.Accent)
                                Text(status, style = MaterialTheme.typography.titleMedium, color = Capy.TextPrimary)
                            }
                        }
                    }
                    // One notice at a time: the crash report, what's new, an update, the support card.
                    val report = lastError
                    val pending = update
                    when {
                        report != null -> CrashDialog(
                            report = report,
                            onSaveLog = ::saveLog,
                            onClose = { lastError = null },
                        )
                        showWhatsNew -> WhatsNewDialog(version = installed, onClose = { showWhatsNew = false })
                        pending != null -> UpdateDialog(
                            release = pending,
                            installed = installed,
                            onSkip = {
                                settings.skippedRelease = pending.tag
                                update = null
                            },
                            onClose = { update = null },
                        )
                        showSupport -> SupportDialog(onClose = { showSupport = false })
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        InputRouter.setRouteToUi(true)
        // Returning from the all-files access screen: scan again now that the folder is readable.
        if (Environment.isExternalStorageManager()) storageCheck++
        // Returning from a game that closed itself: show why, once.
        val report = GameActivity.errorFile(this)
        if (report.exists()) {
            lastError = runCatching { report.readText() }.getOrNull()?.takeIf { it.isNotBlank() }
            report.delete()
        }
    }

    private companion object {
        const val SUPPORT_EVERY = 10
    }

    /** Deletes the persisted pipeline cache of one game, or of every game when [titleId] is null. */
    private fun clearShaderCache(titleId: String?): String {
        val dir = File(filesDir, "_PipelineCache")
        val files = dir.listFiles()?.filter { titleId == null || it.name == "$titleId.bin" } ?: emptyList()
        val removed = files.count { it.delete() }
        return getString(if (removed == 0) R.string.shader_cache_empty else R.string.shader_cache_cleared)
    }

    /** Games of every library folder, by name; a game found in two folders shows once. */
    private fun scanLibrary(): List<GameItem> =
        settings.libraryPaths.flatMap { GameItem.scan(File(it)) }
            .distinctBy { it.elfPath }
            .sortedBy { it.titleName.lowercase() }

    /**
     * Writes this app's recent log (both processes share the app's UID, so the game's lines are
     * included) next to the game library, where users can find and attach it.
     */
    private fun saveLog(): String? = runCatching {
        val dir = File(libraryDir, "logs").apply { mkdirs() }
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val file = File(dir, "consolefivepro-$stamp.txt")
        val process = Runtime.getRuntime().exec(arrayOf("logcat", "-d", "-v", "time"))
        process.inputStream.use { input -> file.outputStream().use { output -> input.copyTo(output) } }
        process.waitFor()
        file.absolutePath
    }.getOrNull()

    /** Manual check from Settings: shows the update dialog, or a toast when current. */
    private fun checkUpdateNow() {
        lifecycleScope.launch {
            val installed = Updater.installedVersion(this@MainActivity)
            val found = Updater.checkForUpdate(installed)
            if (found != null && found.tag != settings.skippedRelease) {
                update = found
            } else {
                android.widget.Toast.makeText(
                    this@MainActivity, R.string.updates_up_to_date, android.widget.Toast.LENGTH_SHORT
                ).show()
            }
        }
    }

    /** True when the first bytes of [head] are a PlayStation executable image (ELF64 or SELF). */
    private fun isGameExecutableMagic(head: ByteArray, length: Int): Boolean {
        if (length < 4) return false
        val elf = head[0] == 0x7f.toByte() && head[1] == 'E'.code.toByte() &&
            head[2] == 'L'.code.toByte() && head[3] == 'F'.code.toByte()
        if (elf) return length < 5 || head[4] == 2.toByte()
        val selfA = head[0] == 0x4f.toByte() && head[1] == 0x15.toByte() &&
            head[2] == 0x3d.toByte() && head[3] == 0x1d.toByte()
        val selfB = head[0] == 0x54.toByte() && head[1] == 0x14.toByte() &&
            head[2] == 0xf5.toByte() && head[3] == 0xee.toByte()
        return selfA || selfB
    }

    /** True when [file] starts with a game-executable magic (the core's ELF/SELF idents). */
    private fun hasGameExecutableMagic(file: File): Boolean = runCatching {
        java.io.FileInputStream(file).use { input ->
            val head = ByteArray(16)
            var len = 0
            while (len < head.size) {
                val n = input.read(head, len, head.size - len)
                if (n < 0) break
                len += n
            }
            isGameExecutableMagic(head, len)
        }
    }.getOrDefault(false)

    /**
     * Copies a picked document into filesDir/imported. The file must really be a game
     * executable (ELF/SELF magic): a picked .pkg used to be copied for minutes and then
     * failed to boot, so anything else is rejected before a byte is written. Space is
     * checked when the provider reports a size, and [onProgress] receives the display
     * name plus the percent (or -1 when unknown) for the import card.
     */
    private fun copyUriToLocal(uri: android.net.Uri, onProgress: (String?, Int) -> Unit): File? = try {
        var name = "eboot.bin"
        var size = -1L
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME).let { idx ->
                    if (idx != -1) cursor.getString(idx)?.let { name = it }
                }
                cursor.getColumnIndex(android.provider.OpenableColumns.SIZE).let { idx ->
                    if (idx != -1 && !cursor.isNull(idx)) size = cursor.getLong(idx)
                }
            }
        }
        val targetDir = File(filesDir, "imported").apply { mkdirs() }
        val targetFile = File(targetDir, name)
        contentResolver.openInputStream(uri)?.use { input ->
            val head = ByteArray(16)
            var headLen = 0
            while (headLen < head.size) {
                val n = input.read(head, headLen, head.size - headLen)
                if (n < 0) break
                headLen += n
            }
            if (!isGameExecutableMagic(head, headLen)) {
                importReject = R.string.import_not_game
                return null
            }
            if (size > 0) {
                val free = android.os.StatFs(filesDir.absolutePath).availableBytes
                if (size > free - 64L * 1024 * 1024) {
                    importReject = R.string.import_no_space
                    return null
                }
            }
            onProgress(name, if (size > 0) 0 else -1)
            targetFile.outputStream().use { output ->
                if (headLen > 0) output.write(head, 0, headLen)
                if (size > 0) {
                    var done = headLen.toLong()
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        output.write(buffer, 0, n)
                        done += n
                        onProgress(name, ((done * 100) / size).toInt().coerceIn(0, 100))
                    }
                } else {
                    input.copyTo(output, 64 * 1024)
                }
            }
        }
        targetFile
    } catch (_: Exception) {
        importReject = R.string.import_failed
        null
    }
}

@Composable
private fun CrashDialog(report: String, onSaveLog: () -> String?, onClose: () -> Unit) {
    var savedPath by remember { mutableStateOf<String?>(null) }
    var focused by remember { mutableIntStateOf(0) }
    val failedText = stringResource(R.string.log_save_failed)
    val actions = listOf(
        stringResource(R.string.save_log) to { savedPath = onSaveLog() ?: failedText },
        stringResource(R.string.close) to onClose,
    )
    OnNavKey { key ->
        when (key) {
            NavKey.Left -> focused = (focused - 1).mod(actions.size)
            NavKey.Right -> focused = (focused + 1).mod(actions.size)
            NavKey.Confirm -> actions[focused].second()
            NavKey.Back -> onClose()
            else -> Unit
        }
    }
    Box(Modifier.fillMaxSize().background(Capy.Scrim), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .width(560.dp)
                .background(Capy.Surface, RoundedCornerShape(16.dp))
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(stringResource(R.string.crash_title), style = MaterialTheme.typography.titleLarge, color = Capy.Bad)
            Text(
                report.trim(),
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = Capy.TextSecondary,
                modifier = Modifier.heightIn(max = 220.dp).verticalScroll(rememberScrollState()),
            )
            savedPath?.let {
                Text(stringResource(R.string.log_saved_in_multiline, it), style = MaterialTheme.typography.bodySmall, color = Capy.TextPrimary)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                actions.forEachIndexed { i, (label, action) ->
                    Text(
                        label,
                        style = MaterialTheme.typography.labelLarge,
                        color = Color.White,
                        modifier = Modifier
                            .background(if (i == focused) Capy.Accent else Capy.SurfaceHigh, RoundedCornerShape(50))
                            .clickable { focused = i; action() }
                            .padding(horizontal = 18.dp, vertical = 9.dp),
                    )
                }
            }
        }
    }
}
