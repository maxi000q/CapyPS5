// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.content.Context
import android.net.Uri
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** Safety copies of the save data the core keeps under filesDir/_SaveData/<title id>. */
object GameSaves {

    private const val SAVEDATA_DIR = "_SaveData"
    private const val BACKUP_DIR = "saves-backup"
    private const val KEEP_BACKUPS = 3
    private const val MAX_ENTRY_BYTES = 256L * 1024 * 1024
    private const val MAX_TOTAL_BYTES = 2L * 1024 * 1024 * 1024

    fun savedataRoot(context: Context): File = File(context.filesDir, SAVEDATA_DIR)

    /**
     * One safety copy of a single game's saves. Returns the wall-clock label of the copy,
     * or null when there is nothing to back up or the copy failed.
     */
    fun snapshot(context: Context, titleId: String): String? {
        if (titleId.isBlank()) return null
        val source = File(savedataRoot(context), titleId)
        val files = source.walkTopDown().filter { it.isFile }.toList()
        if (files.isEmpty()) return null
        val safeTitle = titleId.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val dir = File(context.filesDir, BACKUP_DIR).apply { mkdirs() }
        val target = File(dir, "$safeTitle-$stamp.zip")
        val ok = runCatching {
            ZipOutputStream(target.outputStream().buffered()).use { zip ->
                files.forEach { file ->
                    zip.putNextEntry(ZipEntry("$titleId/${file.relativeTo(source).invariantSeparatorsPath}"))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
            true
        }.getOrDefault(false)
        if (!ok) {
            target.delete()
            return null
        }
        prune(dir, safeTitle)
        return SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date())
    }

    private fun prune(dir: File, safeTitle: String) {
        val backups = dir.listFiles { f -> f.name.startsWith("$safeTitle-") }?.sortedBy { it.name } ?: return
        backups.dropLast(KEEP_BACKUPS).forEach { it.delete() }
    }

    fun hasSaves(context: Context): Boolean = savedataRoot(context).listFiles()?.isNotEmpty() == true

    /** Writes every game's save data into the SAF document at [uri]. */
    fun exportAll(context: Context, uri: Uri): Boolean = runCatching {
        val root = savedataRoot(context)
        val files = root.walkTopDown().filter { it.isFile }.toList()
        if (files.isEmpty()) return false
        val out = context.contentResolver.openOutputStream(uri) ?: return false
        out.use {
            ZipOutputStream(it.buffered()).use { zip ->
                files.forEach { file ->
                    zip.putNextEntry(ZipEntry("$SAVEDATA_DIR/${file.relativeTo(root).invariantSeparatorsPath}"))
                    file.inputStream().use { input -> input.copyTo(zip) }
                    zip.closeEntry()
                }
            }
        }
        true
    }.getOrDefault(false)

    /**
     * Restores save data from an Export zip, replacing whole game folders.
     * Returns the number of games restored, or -1 when the archive is unusable.
     */
    fun importAll(context: Context, uri: Uri): Int = runCatching {
        val root = savedataRoot(context).apply { mkdirs() }
        val temp = File(context.filesDir, "tmp-saves-import")
        temp.deleteRecursively()
        if (!temp.mkdirs()) return -1
        val input = context.contentResolver.openInputStream(uri) ?: return -1
        var total = 0L
        input.use { stream ->
            ZipInputStream(stream.buffered()).use { zip ->
                var entry = zip.nextEntry
                while (entry != null) {
                    if (!entry.isDirectory) {
                        val parts = entry.name.split('/').filter { it.isNotEmpty() }
                        val rel = if (parts.firstOrNull() == SAVEDATA_DIR) parts.drop(1) else parts
                        if (rel.isNotEmpty() && rel.none { it == ".." }) {
                            val target = File(temp, rel.joinToString("/"))
                            target.parentFile?.mkdirs()
                            var written = 0L
                            target.outputStream().use { out -> written = zip.copyTo(out) }
                            if (written > MAX_ENTRY_BYTES) return -1
                            total += written
                            if (total > MAX_TOTAL_BYTES) return -1
                        }
                    }
                    zip.closeEntry()
                    entry = zip.nextEntry
                }
            }
        }
        val titles = temp.listFiles()?.filter { it.isDirectory } ?: emptyList()
        var restored = 0
        titles.forEach { title ->
            val dest = File(root, title.name)
            if (dest.exists()) dest.deleteRecursively()
            if (title.renameTo(dest) || title.copyRecursively(dest, overwrite = true)) restored++
        }
        temp.deleteRecursively()
        restored
    }.getOrDefault(-1)
}
