// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package com.consolefivepro

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.res.Configuration
import android.view.View
import org.json.JSONObject
import java.util.Locale

/**
 * Interface languages (v0.0.10pro): every user-facing string of the frontend comes from a flat
 * JSON file in the engine tree's lang/ folder - one file per language, keys identical to the
 * resource names in values/strings.xml. The build copies the folder into the APK assets
 * (see the syncLangFiles task in app/build.gradle.kts), and [LocalizedContext] answers string
 * lookups from the selected file first, then from the English file, then from the English
 * resources themselves. Editing a translation is a one-file, one-line change with no Android
 * resource folders involved.
 *
 * English is the default; the choice lives in the shared preferences and the picker is the
 * Language row in Settings (System). Switching restarts the activity so every screen rebuilds
 * with the new strings - and, for Arabic and Kurdish, mirrored right-to-left.
 */
object Localization {

    /** One selectable interface language; the label is the language's own name for itself. */
    data class Language(val code: String, val label: String, val rtl: Boolean = false)

    val languages = listOf(
        Language("en", "English"),
        Language("ar", "العربية", rtl = true),
        Language("ckb", "کوردی", rtl = true),
        Language("de", "Deutsch"),
        Language("es", "Español"),
        Language("fr", "Français"),
        Language("it", "Italiano"),
        Language("ja", "日本語"),
        Language("ko", "한국어"),
        Language("pl", "Polski"),
        Language("ru", "Русский"),
        Language("tr", "Türkçe"),
        Language("zh", "中文（简体）"),
    )

    private const val PREFS = "capyps5"
    private const val PREF_KEY = "language"

    fun currentLanguage(context: Context): Language {
        val code = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(PREF_KEY, null)
        return languages.firstOrNull { it.code == code } ?: languages.first()
    }

    /**
     * Persists [language] and restarts [context]'s activity: the rebuild re-wraps the base
     * context (see [wrap]), so every screen comes back with the new strings and layout
     * direction. Strings resolve from the freshly loaded table on the next attach.
     */
    fun setLanguage(context: Context, language: Language) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(PREF_KEY, language.code).apply()
        (context as? Activity)?.recreate()
    }

    // The active tables, written by wrap() on every activity attach and read on every lookup.
    // One language is active per process at a time; activities attach one after another.
    @Volatile private var strings: Map<String, String> = emptyMap()
    @Volatile private var englishStrings: Map<String, String> = emptyMap()
    @Volatile private var arrays: Map<String, List<String>> = emptyMap()
    @Volatile private var englishArrays: Map<String, List<String>> = emptyMap()

    /** A string array from the language files (the English one when this language has none). */
    fun stringArray(key: String): List<String> = arrays[key] ?: englishArrays[key] ?: emptyList()

    /** The selected language's string for a JSON key, then the English one; null when neither has it. */
    fun resolve(key: String): String? = strings[key] ?: englishStrings[key]

    /**
     * Loads the language selected in the preferences and returns a context that carries the
     * language's locale and layout direction, and answers string lookups from its file (see
     * [LocalizedContext]). Called from every activity's attachBaseContext.
     */
    fun wrap(base: Context): Context {
        val language = currentLanguage(base)
        val english = table(base, "en")
        val selected = if (language.code == "en") english else table(base, language.code)
        englishStrings = english.strings
        englishArrays = english.arrays
        strings = selected.strings
        arrays = selected.arrays

        val config = Configuration(base.resources.configuration)
        config.setLocale(Locale.forLanguageTag(language.code))
        // setLocale derives the direction from the locale table; the explicit assignment makes
        // right-to-left independent of whether the system knows this particular tag.
        config.layoutDirection =
            if (language.rtl) View.LAYOUT_DIRECTION_RTL else View.LAYOUT_DIRECTION_LTR
        return LocalizedContext(base.createConfigurationContext(config))
    }

    private class Table(val strings: Map<String, String>, val arrays: Map<String, List<String>>)

    private val cache = HashMap<String, Table>()

    /** Reads lang/&lt;code&gt;.json once per code; a malformed or missing file degrades to empty
     *  (every lookup then falls back to English, then to the English resources). */
    private fun table(context: Context, code: String): Table = synchronized(cache) {
        cache.getOrPut(code) {
            runCatching {
                val text = context.assets.open("lang/$code.json").bufferedReader().use { it.readText() }
                val json = JSONObject(text)
                val strings = HashMap<String, String>()
                val arrays = HashMap<String, List<String>>()
                for (key in json.keys()) {
                    when (val value = json.opt(key)) {
                        is String -> strings[key] = value
                        is org.json.JSONArray -> arrays[key] = List(value.length()) { value.optString(it) }
                    }
                }
                Table(strings, arrays)
            }.getOrDefault(Table(emptyMap(), emptyMap()))
        }
    }
}

/**
 * Delegates everything to the configured base context except string lookups: those answer from
 * the selected language file first, then the English file, then the base resources. Every
 * getString in the app - Compose's stringResource, Context.getString, the settings labels -
 * lands here, so no call site needs to know the language system exists.
 */
private class LocalizedContext(base: Context) : ContextWrapper(base) {

    // Resource id -> entry name (the JSON key). Lookups hit the map; ids outside this
    // package (framework and library strings) fall straight through to the base.
    private val entryNames = HashMap<Int, String?>()

    private fun entryName(resId: Int): String? = entryNames.getOrPut(resId) {
        runCatching { base.resources.getResourceEntryName(resId) }.getOrNull()
    }

    private fun raw(resId: Int): String? {
        val key = entryName(resId) ?: return null
        return Localization.resolve(key)
    }

    override fun getString(resId: Int): String = raw(resId) ?: base.getString(resId)

    override fun getString(resId: Int, vararg formatArgs: Any?): String {
        val pattern = raw(resId) ?: return base.getString(resId, *formatArgs)
        return String.format(pattern, *formatArgs)
    }
}
