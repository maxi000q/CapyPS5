#!/usr/bin/env python3
"""Fix the register-offset case constants in RecoverGuestNullRead.

The register-offset A64 load group encodes bits [11:10] as 10 (the fixed part of
`... option S 10 Rn Rt`), so every case constant under mask 0xFFE00C00 must end
in 0x800. The first version of the decoder used the mask's own base (bits 11:10
== 00), which no real instruction can produce - the branch would simply never
fire (safe, but useless). This adds the missing 0x800 to all fourteen cases.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
cpp = ROOT / "core/src/cpu/cpuBackend.cpp"

text = cpp.read_text(encoding="utf-8")

# (old, new) - all inside the register-offset switch only.
fixes = [
    ("case 0x38600000:", "case 0x38600800:"),
    ("case 0x38A00000:", "case 0x38A00800:"),
    ("case 0x38E00000:", "case 0x38E00800:"),
    ("case 0x78600000:", "case 0x78600800:"),
    ("case 0x78A00000:", "case 0x78A00800:"),
    ("case 0x78E00000:", "case 0x78E00800:"),
    ("case 0xB8600000:", "case 0xB8600800:"),
    ("case 0xB8A00000:", "case 0xB8A00800:"),
    ("case 0xF8600000:", "case 0xF8600800:"),
    ("case 0x3C600000:", "case 0x3C600800:"),
    ("case 0x7C600000:", "case 0x7C600800:"),
    ("case 0xBC600000:", "case 0xBC600800:"),
    ("case 0xFC600000:", "case 0xFC600800:"),
    ("case 0x3CE00000:", "case 0x3CE00800:"),
]

# All of these constants appear exactly once (only in the register switch).
for old, new in fixes:
    n = text.count(old)
    if n != 1:
        print(f"FAIL: {old} found {n}x")
        sys.exit(1)
    text = text.replace(old, new, 1)

cpp.write_text(text, encoding="utf-8", newline="")
print("ok register-offset constants fixed (14 cases +0x800)")
