#!/usr/bin/env python3
"""v0.0.9proQ1 round 2 (app): duplicate-key crash, PKG message, import speed.

Files touched (binary-safe: str round-trip through UTF-8, line endings preserved
because every anchor keeps its own newlines):
  android/.../MainActivity.kt      - one-import-at-a-time guard, dedupe add + defensive
                                     dedupe at the HomeScreen call, real-PKG detection
                                     with a dedicated message, 1 MB copy buffer and
                                     per-percent progress (was: redraw per 64 KB chunk)
  android/.../values/strings.xml    - new strings + whats-new items + aapt2 fix
  android/.../values-es/strings.xml - same, Spanish

The v0.0.9pro crash log: "Key .../imported/TITLEID-....pkg was already used" -
importing the same executable twice (a retry after a game crash) added two grid
entries with the same key and Compose aborted the app. Fixes:
  * a second pick while an import is running is ignored
  * a completed import reuses the existing entry instead of adding a twin
  * the grid list itself is deduped as the last line of defence
The aapt2 build failure (NPE in TableExtractor.flattenXmlSubTree) comes from the
escaped-quote form inside string-array items; the whats-new text now uses
typographic quotes instead.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def patch(path: Path, edits):
    text = path.read_text(encoding="utf-8")
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            print(f"FAIL {path.name}: anchor found {n}x (expected 1):")
            print(old[:200])
            sys.exit(1)
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8", newline="")
    print(f"ok {path} ({len(edits)} edit(s))")


# ------------------------------------------------------------------ MainActivity
kt = ROOT / "android/app/src/main/java/com/consolefivepro/MainActivity.kt"

patch(
    kt,
    [
        # One import at a time: guard + claim the slot.
        (
            """                                // A picked eboot can be hundreds of megabytes; the copy stays on
""",
            """                                // v0.0.9proQ1: one import at a time. A second pick while a
                                // multi-GB copy is still running raced the first for the same
                                // destination and could add a duplicate library entry - the
                                // v0.0.9pro game-grid crash.
                                if (importStatus != null) {
                                    return@launch
                                }
                                importStatus = ""
                                // A picked eboot can be hundreds of megabytes; the copy stays on
""",
        ),
        # File-branch rejection: say what a real .pkg needs.
        (
            """                                        val picked = File(uriString)
                                        if (picked.isFile && hasGameExecutableMagic(picked)) picked
                                        else {
                                            importReject = R.string.import_not_game
                                            null
                                        }
""",
            """                                        val picked = File(uriString)
                                        if (picked.isFile && hasGameExecutableMagic(picked)) picked
                                        else {
                                            importReject =
                                                if (picked.isFile && hasPackageMagic(picked)) {
                                                    R.string.import_pkg_not_supported
                                                } else {
                                                    R.string.import_not_game
                                                }
                                            null
                                        }
""",
        ),
        # A completed import never duplicates an existing entry.
        (
            """                                    games.add(newGame)
                                    GameActivity.launch(this@MainActivity, newGame)
""",
            """                                    // The same executable picked twice (a retry after a
                                    // crash) must not create a second entry with the same grid key.
                                    val toLaunch = games.firstOrNull { it.elfPath == newGame.elfPath }
                                        ?: newGame.also { games.add(it) }
                                    GameActivity.launch(this@MainActivity, toLaunch)
""",
        ),
        # The grid is deduped as the last line of defence.
        (
            """                    HomeScreen(
                        games = games,
""",
            """                    HomeScreen(
                        // v0.0.9proQ1: LazyGrid keys crash the app on duplicates - the
                        // library is deduped here no matter what produced the list.
                        games = games.distinctBy { it.elfPath },
""",
        ),
        # PKG magic helpers next to the executable-magic ones.
        (
            """    /** True when [file] starts with a game-executable magic (the core's ELF/SELF idents). */
""",
            """    /** True when [head] is a PS4/PS5 package header (0x7F "CNT") - the core cannot load those. */
    private fun isPackageMagic(head: ByteArray, length: Int): Boolean =
        length >= 4 && head[0] == 0x7f.toByte() && head[1] == 'C'.code.toByte() &&
            head[2] == 'N'.code.toByte() && head[3] == 'T'.code.toByte()

    /** True when [file] starts with the PS4/PS5 package magic (0x7F "CNT"). */
    private fun hasPackageMagic(file: File): Boolean = runCatching {
        java.io.FileInputStream(file).use { input ->
            val head = ByteArray(4)
            var len = 0
            while (len < head.size) {
                val n = input.read(head, len, head.size - len)
                if (n < 0) break
                len += n
            }
            isPackageMagic(head, len)
        }
    }.getOrDefault(false)

    /** True when [file] starts with a game-executable magic (the core's ELF/SELF idents). */
""",
        ),
        # A real .pkg gets its own message at the pre-copy rejection.
        (
            """                if (!isGameExecutableMagic(head, headLen)) {
                    importReject = R.string.import_not_game
                    return null
                }
""",
            """                if (!isGameExecutableMagic(head, headLen)) {
                    // A real .pkg is the most common wrong pick (multi-GB): say what to
                    // import instead of a generic rejection - the core loads extracted games.
                    importReject = if (isPackageMagic(head, headLen)) {
                        R.string.import_pkg_not_supported
                    } else {
                        R.string.import_not_game
                    }
                    return null
                }
""",
        ),
        # Copy buffer + per-percent progress.
        (
            """                    var done = headLen.toLong()
                    var nextSpaceCheck = 0L
                    val buffer = ByteArray(64 * 1024)
""",
            """                    var done = headLen.toLong()
                    var nextSpaceCheck = 0L
                    var lastPercent = -1
                    // v0.0.9proQ1: a 1 MB buffer keeps the copy fast on flash storage.
                    val buffer = ByteArray(1024 * 1024)
""",
        ),
        (
            """                        if (size > 0) {
                            onProgress(name, ((done * 100) / size).toInt().coerceIn(0, 100))
                        }
""",
            """                        if (size > 0) {
                            // One redraw per percent, not per chunk: a 7 GB import used to
                            // recompose the progress card over a hundred thousand times.
                            val percent = ((done * 100) / size).toInt().coerceIn(0, 100)
                            if (percent != lastPercent) {
                                lastPercent = percent
                                onProgress(name, percent)
                            }
                        }
""",
        ),
    ],
)

# --------------------------------------------------------------- strings (en)
en = ROOT / "android/app/src/main/res/values/strings.xml"

patch(
    en,
    [
        # Dedicated message for real .pkg picks.
        (
            """    <string name="import_no_space">""",
            """    <string name="import_pkg_not_supported">PKG packages are not supported yet. Import the game\\'s extracted folder (the one with eboot.bin inside) or the eboot.bin itself.</string>
    <string name="import_no_space">""",
        ),
        # aapt2 fix: escaped quotes inside a string-array item crash TableExtractor.
        (
            'fails with \\"not enough space\\"',
            "fails with \u201cnot enough space\u201d",
        ),
        (
            "left overs are cleaned automatically on start",
            "leftovers are cleaned automatically on start",
        ),
        # Whats-new: this round's fixes, first in the list.
        (
            """    <string-array name="whats_new_items">
        <item>Fixed: importing a large game""",
            """    <string-array name="whats_new_items">
        <item>Fixed: the app could crash right after an import \u2014 the same file could enter the library twice, which the game grid cannot show, and a second pick while a copy was still running raced the first one. Both are impossible now, and picking a real .pkg says what to import instead of a generic rejection. A one-off null-pointer read inside the game no longer ends the session either: the read returns zero and the game keeps running (limited, reads only \u2014 debug.capy.null_read_recovery).</item>
        <item>Faster: big imports copy with a 1 MB buffer and the progress redraws once per percent instead of once per chunk, so the interface stays responsive while gigabytes are written.</item>
        <item>Fixed: importing a large game""",
        ),
    ],
)

# --------------------------------------------------------------- strings (es)
es = ROOT / "android/app/src/main/res/values-es/strings.xml"

patch(
    es,
    [
        (
            """    <string name="import_no_space">""",
            """    <string name="import_pkg_not_supported">Los paquetes PKG aún no son compatibles. Importa la carpeta extraída del juego (la que contiene eboot.bin) o el propio eboot.bin.</string>
    <string name="import_no_space">""",
        ),
        (
            'falla con \\"no hay espacio\\"',
            "falla con \u201cno hay espacio\u201d",
        ),
        (
            """    <string-array name="whats_new_items">
        <item>Corregido: importar un juego grande""",
            """    <string-array name="whats_new_items">
        <item>Corregido: la app podía cerrarse justo después de importar \u2014 el mismo archivo podía quedar dos veces en la biblioteca, algo que la cuadrícula no puede mostrar, y una segunda selección durante una copia competía con la primera. Ya no puede pasar, y elegir un .pkg real explica qué hay que importar en lugar de un rechazo genérico. Una lectura de puntero nulo puntual dentro del juego tampoco termina la sesión: la lectura devuelve cero y el juego continúa (con límite y solo lecturas \u2014 debug.capy.null_read_recovery).</item>
        <item>Más rápido: las importaciones grandes copian con un búfer de 1 MB y el progreso se redibuja una vez por porcentaje en lugar de por bloque, así que la interfaz responde mientras se escriben gigabytes.</item>
        <item>Corregido: importar un juego grande""",
        ),
    ],
)

print("app round-2 patched")
