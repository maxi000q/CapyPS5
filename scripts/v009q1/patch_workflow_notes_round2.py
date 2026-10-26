#!/usr/bin/env python3
"""v0.0.9proQ1 round 2: workflow release notes get this round's fixes on top."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
wf = ROOT / ".github/workflows/android-build.yml"

text = wf.read_text(encoding="utf-8")

old = """          ## v0.0.9proQ1 — storage fix + much deeper GPU optimization (same graphics)

          - **Fixed: large game imports no longer eat the whole disk.**"""

new = """          ## v0.0.9proQ1 — crash fixes, import/storage fixes + much deeper GPU optimization (same graphics)

          - **Fixed: the app could crash right after importing a game.** Importing the same executable twice (typically a retry after the game crashed) added two library entries with the same key, which the game grid cannot render — the app died with "Key ... was already used". Imports now run one at a time (a second pick while a multi-GB copy is still running no longer races the first), a completed import reuses the existing library entry instead of adding a twin, and the grid list itself is deduplicated as a last line of defence.
          - **Fixed: a one-off null-pointer read inside the game no longer ends the session.** The v0.0.9pro crash on Mali phones ("the game itself crashed: null-pointer read") now recovers instead: Android cannot map the null page (mmap_min_addr), so the faulting A64 load is decoded in the signal handler, its destination register receives zero — what a zeroed page would return — and execution resumes past it. Reads only (writes are never faked), capped at 64 recoveries per session so a genuine null-dereference loop still fails; `debug.capy.null_read_recovery=0` restores the previous behaviour.
          - **Faster, more responsive imports.** The copy buffer grows from 64 KB to 1 MB and the progress card redraws once per percent instead of once per chunk (a 7 GB import recomposed it over 100,000 times). Picking a real `.pkg` now explains what to import (the extracted game folder or its eboot.bin) instead of a generic "not a game" rejection.
          - **Fixed: large game imports no longer eat the whole disk.**"""

if text.count(old) != 1:
    print("FAIL: anchor found", text.count(old), "times")
    sys.exit(1)

wf.write_text(text.replace(old, new, 1), encoding="utf-8", newline="")
print("ok workflow release notes updated")
