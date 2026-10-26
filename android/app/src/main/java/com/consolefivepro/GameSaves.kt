// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.content.Context
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Safety copies of the save data the core keeps under filesDir/_SaveData/<title id>. */
object GameSaves {

    private const val SAVEDATA_DIR = "_SaveData"
    private const val BACKUP_DIR = "saves-backup"
    private const val KEEP_BACKUPS = 3

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
}
