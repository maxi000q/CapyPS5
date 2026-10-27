#!/usr/bin/env python3
"""v0.0.10pro phase 1: remove real game names from the source code (copyright).

Every mention of a real title in comments, app strings and committed scripts is
replaced with a vague description - same technical meaning, no names. Binary-safe
exact-bytes edits; per-file line endings (CRLF vs LF) are preserved because each
anchor embeds the file's own bytes.

Vague replacements used:
  Stray                       -> one launch title
  God of War Sons of Sparta    -> one heavy first-party action title
  Ratchet & Clank: Size Matters-> one cross-generation platformer
  Sackboy: A Big Adventure     -> one platformer launch title
  My First Gran Turismo        -> one first-party racing demo
  Astro's Playroom             -> the built-in platformer demo
  Celeste PICO-8               -> one 2D indie title
  Monster Boy                  -> one 2D platformer
  Guri                         -> the built-in tech demo
  EDF                          -> one bug-hunting shooter
  UP9000 (Sony content prefix) -> TITLEID
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (file, old, new) - all applied exactly once.
EDITS = [
    # ---------------- renderDraw.cpp (CRLF)
    ("core/src/graphics/host_gpu/renderer/renderDraw.cpp",
     b"serves this draw: Stray\r",
     b"serves this draw: one launch title\r"),
    ("core/src/graphics/host_gpu/renderer/renderDraw.cpp",
     b"(God of War Sons of Sparta and Stray alternate a\r",
     b"(one heavy first-party action title and one launch title alternate a\r"),
    ("core/src/graphics/host_gpu/renderer/renderDraw.cpp",
     b"first (Ratchet & Clank: Size\r\n\t// Matters' per-frame depth clear draw kept",
     b"first (one cross-generation\r\n\t// platformer's per-frame depth clear draw kept"),
    ("core/src/graphics/host_gpu/renderer/renderDraw.cpp",
     b"split Stray's 4K G-buffer pass",
     b"split one launch title's 4K G-buffer pass"),
    ("core/src/graphics/host_gpu/renderer/renderDraw.cpp",
     b"made Stray's layers flicker",
     b"made one launch title's layers flicker"),
    # ---------------- imageView.cpp (CRLF)
    ("core/src/graphics/host_gpu/renderer/image/imageView.cpp",
     b"(Ratchet & Clank: Size Matters' sand looked blue)",
     b"(one cross-generation platformer's sand looked blue)"),
    # ---------------- image.cpp (CRLF)
    ("core/src/graphics/host_gpu/renderer/image/image.cpp",
     b"(Stray split its 4K passes per draw)",
     b"(one launch title split its 4K passes per draw)"),
    # ---------------- renderContext.cpp (CRLF)
    ("core/src/graphics/host_gpu/renderer/renderContext.cpp",
     b"contended by Stray's task threads",
     b"contended by one launch title's task threads"),
    ("core/src/graphics/host_gpu/renderer/renderContext.cpp",
     b"made Monster Boy, which unmaps memory",
     b"made one 2D platformer, which unmaps memory"),
    # ---------------- bufferCache.cpp (CRLF)
    ("core/src/graphics/host_gpu/renderer/cache/bufferCache.cpp",
     b"Stray's render thread reads GPU-written buffers",
     b"one launch title's render thread reads GPU-written buffers"),
    # ---------------- libNet.cpp (CRLF)
    ("core/src/libs/libNet.cpp",
     b"(Sackboy: A Big Adventure runs one while loading",
     b"(one platformer launch title runs one while loading"),
    # ---------------- memory.cpp (CRLF)
    ("core/src/kernel/memory.cpp",
     b"(Guri maps 128 GiB above its base",
     b"(the built-in tech demo maps 128 GiB above its base"),
    ("core/src/kernel/memory.cpp",
     b"(Guri's BatchMap failed with ENOMEM)",
     b"(the tech demo's batch map failed with ENOMEM)"),
    ("core/src/kernel/memory.cpp",
     b"which terminated My\r\n\t\t\t\t// First Gran Turismo with status 321",
     b"which terminated one\r\n\t\t\t\t\t\t// first-party racing demo with status 321"),
    # ---------------- cpuBackend.cpp (LF)
    ("core/src/cpu/cpuBackend.cpp",
     b"(Guri: sha1msg1)",
     b"(the built-in tech demo: sha1msg1)"),
    ("core/src/cpu/cpuBackend.cpp",
     b"// AVX 128-bit vector support for Celeste PICO-8\n",
     b"// AVX 128-bit vector support for one 2D indie title\n"),
    # ---------------- shader recompiler (CRLF)
    ("core/src/graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h",
     b"(Guri's sort: ~6500 unpacks)",
     b"(the tech demo's sort: ~6500 unpacks)"),
    ("core/src/graphics/shader/recompiler/backend/spirv/spirvEmitterAluHelpers.cpp",
     b"(Guri: ~10k selects, 2 FPS)",
     b"(the tech demo: ~10k selects, 2 FPS)"),
    ("core/src/graphics/shader/recompiler/frontend/translate/Integer.cpp",
     b"in large vertex shaders (Astro's Playroom)",
     b"in large vertex shaders (the built-in platformer demo)"),
    # ---------------- renderCompute.cpp (LF)
    ("core/src/graphics/host_gpu/renderer/renderCompute.cpp",
     b"wait for it (Guri, EDF).",
     b"wait for it (the tech demo, one bug-hunting shooter)."),
    # ---------------- app strings (LF, user-visible)
    ("android/app/src/main/res/values/strings.xml",
     b"(Stray\\'s render thread)",
     b"(one launch title\\'s render thread)"),
    ("android/app/src/main/res/values-es/strings.xml",
     b"(hilo de render de Stray)",
     b"(hilo de render de un t\xc3\xadtulo de lanzamiento)"),
    # ---------------- committed script (LF)
    ("scripts/v009q1/patch_app_round2.py",
     b"imported/UP9000-....pkg",
     b"imported/TITLEID-....pkg"),
]


def main() -> int:
    by_file = {}
    for f, old, new in EDITS:
        by_file.setdefault(f, []).append((old, new))
    failed = False
    for rel, edits in by_file.items():
        path = ROOT / rel
        data = path.read_bytes()
        for old, new in edits:
            n = data.count(old)
            if n == 0 and data.count(new) == 1:
                print(f"skip {rel}: already applied")
                continue
            if n != 1:
                print(f"FAIL {rel}: anchor x{n}: {old[:80]!r}")
                failed = True
                continue
            data = data.replace(old, new, 1)
        path.write_bytes(data)
        print(f"ok {rel} ({len(edits)} edit(s))")
    if failed:
        return 1
    print("game-name scrub complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
