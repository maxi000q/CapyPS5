#!/usr/bin/env python3
"""Verify RecoverGuestNullRead's decode tables against hand-built A64 encodings.

No cross-assembler is available here, so instructions are constructed field by
field straight from the ARM ARM encoding tables and then run through a Python
re-implementation of the three switch statements (constants copied from the C++
source). Each instruction must land in the intended bucket; every writeback /
store / atomic / exclusive form must NOT be recovered (falls through to the
crash reporter, as designed).
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
cpp = (ROOT / "core/src/cpu/cpuBackend.cpp").read_text(encoding="utf-8")

# Pull the three case tables out of the C++ so the test always checks the
# shipped constants, not a copy.
import re

def parse_switch(section):
    m = re.search(r"switch \(insn & (0x[0-9A-Fa-f]+)u?\) \{", section)
    mask = int(m.group(1), 16)
    # Balance braces from the switch's '{' so nested indentation cannot leak
    # the next switch's cases into this table (switch1 closes at one tab, the
    # others at two).
    i = section.index("{", m.end() - 1)
    depth, j = 0, i
    while j < len(section):
        if section[j] == "{":
            depth += 1
        elif section[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = section[i : j + 1]
    table = {}
    for case, dest, size, pair in re.findall(
        r"case (0x[0-9A-Fa-f]+): dest = k(\w+);?\s*bytes = (\d+);?\s*(pair = true;)?\s*break;",
        body,
    ):
        table[int(case, 16)] = (dest, int(size), bool(pair))
    return mask, table

start = cpp.index("// Every case below loads")
switch1 = parse_switch(cpp[start:])
start = cpp.index("// Unscaled-offset loads")
switch2 = parse_switch(cpp[start:])
start = cpp.index("// Register-offset loads")
switch3 = parse_switch(cpp[start:])

def decode(insn):
    for mask, table in (switch1, switch2, switch3):
        hit = table.get(insn & mask)
        if hit:
            return hit
    return None

# ---- instruction builders (ARM ARM field layouts) ----
def imm_load(size, opc, imm12, rn, rt):
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b01 << 24) | (opc << 22) | (imm12 << 10) | (rn << 5) | rt

def simd_imm_load(size, opc, imm12, rn, rt):
    return (size << 30) | (0b111 << 27) | (1 << 26) | (0b01 << 24) | (opc << 22) | (imm12 << 10) | (rn << 5) | rt

def unscaled(size, opc, imm9, rn, rt):  # LDUR: bits 11:10 = 00
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | (opc << 22) | (imm9 << 12) | (0b00 << 10) | (rn << 5) | rt

def simd_unscaled(size, opc, imm9, rn, rt):
    return (size << 30) | (0b111 << 27) | (1 << 26) | (0b00 << 24) | (opc << 22) | (imm9 << 12) | (0b00 << 10) | (rn << 5) | rt

def reg_load(size, opc, rm, rn, rt, option=0b011, s=0):  # bit21=1, bits 11:10=10
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | (opc << 22) | (1 << 21) | (rm << 16) | (option << 13) | (s << 12) | (0b10 << 10) | (rn << 5) | rt

def simd_reg_load(size, opc, rm, rn, rt, option=0b011, s=0):
    return (size << 30) | (0b111 << 27) | (1 << 26) | (0b00 << 24) | (opc << 22) | (1 << 21) | (rm << 16) | (option << 13) | (s << 12) | (0b10 << 10) | (rn << 5) | rt

def post_index(size, opc, imm9, rn, rt):  # bits 11:10 = 01 (writeback)
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | (opc << 22) | (imm9 << 12) | (0b01 << 10) | (rn << 5) | rt

def pre_index(size, opc, imm9, rn, rt):  # bits 11:10 = 11 (writeback)
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | (opc << 22) | (0b1 << 21) | (imm9 << 12) | (0b11 << 10) | (rn << 5) | rt

def imm_store(size, opc, imm12, rn, rt):
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b01 << 24) | (opc << 22) | (imm12 << 10) | (rn << 5) | rt

ok = fail = 0
def expect(insn, dest, size, pair=False, what=""):
    global ok, fail
    got = decode(insn)
    if got == (dest, size, pair):
        ok += 1
    else:
        fail += 1
        print(f"FAIL {what}: insn=0x{insn:08x} expected {(dest, size, pair)} got {got}")

# --- GPR immediate loads: all widths and extensions
expect(imm_load(0b11, 0b01, 5, 1, 0), "Gpr", 8, False, "LDR X0,[X1,#40]")
expect(imm_load(0b10, 0b01, 0, 3, 5), "Gpr", 4, False, "LDR W5,[X3]")
expect(imm_load(0b00, 0b01, 7, 2, 9), "Gpr", 1, False, "LDRB W9,[X2,#7]")
expect(imm_load(0b01, 0b01, 3, 4, 8), "Gpr", 2, False, "LDRH W8,[X4,#6]")
expect(imm_load(0b10, 0b10, 2, 6, 7), "Gpr", 4, False, "LDRSW X7,[X6,#8]")
expect(imm_load(0b00, 0b10, 1, 7, 2), "Gpr", 1, False, "LDRSB W2,[X7,#1]")
expect(imm_load(0b00, 0b11, 1, 7, 2), "Gpr", 1, False, "LDRSB X2,[X7,#1]")
expect(imm_load(0b01, 0b10, 1, 7, 2), "Gpr", 2, False, "LDRSH W2,[X7,#2]")
expect(imm_load(0b01, 0b11, 1, 7, 2), "Gpr", 2, False, "LDRSH X2,[X7,#2]")
# --- SIMD immediate loads
expect(simd_imm_load(0b00, 0b01, 4, 1, 3), "Simd", 1, False, "LDR B3,[X1,#4]")
expect(simd_imm_load(0b01, 0b01, 4, 1, 3), "Simd", 2, False, "LDR H3,[X1,#8]")
expect(simd_imm_load(0b10, 0b01, 4, 1, 3), "Simd", 4, False, "LDR S3,[X1,#16]")
expect(simd_imm_load(0b11, 0b01, 4, 1, 3), "Simd", 8, False, "LDR D3,[X1,#32]")
expect(simd_imm_load(0b00, 0b11, 4, 1, 3), "Simd", 16, False, "LDR Q3,[X1,#64]")
# --- LDP (signed offset): [31:22] = 1010100101 (x=1) / 0010100101 (x=0)
ldp_x = (0b1010100101 << 22) | (2 << 15) | (1 << 10) | (2 << 5) | 0
assert ldp_x & 0xFFC00000 == 0xA9400000, hex(ldp_x)
expect(ldp_x, "Gpr", 16, True, "LDP X0,X1,[X2,#16]")
ldp_w = (0b0010100101 << 22) | (2 << 15) | (1 << 10) | (2 << 5) | 0
assert ldp_w & 0xFFC00000 == 0x29400000, hex(ldp_w)
expect(ldp_w, "Gpr", 8, True, "LDP W0,W1,[X2,#16]")
# --- LDUR (unscaled, negative offsets)
expect(unscaled(0b11, 0b01, 0x1F8, 1, 0), "Gpr", 8, False, "LDUR X0,[X1,#-8]")
expect(unscaled(0b00, 0b01, 0x1FF, 1, 0), "Gpr", 1, False, "LDURB W0,[X1,#-1]")
expect(unscaled(0b10, 0b10, 0x1F9, 1, 0), "Gpr", 4, False, "LDURSW X0,[X1,#-7]")
expect(simd_unscaled(0b00, 0b11, 0x1F8, 1, 0), "Simd", 16, False, "LDUR Q0,[X1,#-8]")
# --- register-offset
expect(reg_load(0b11, 0b01, 2, 1, 0), "Gpr", 8, False, "LDR X0,[X1,X2]")
expect(reg_load(0b10, 0b01, 2, 1, 0), "Gpr", 4, False, "LDR W0,[X1,X2]")
expect(reg_load(0b00, 0b01, 2, 1, 0), "Gpr", 1, False, "LDRB W0,[X1,X2]")
expect(reg_load(0b01, 0b01, 2, 1, 0), "Gpr", 2, False, "LDRH W0,[X1,X2]")
expect(reg_load(0b10, 0b10, 2, 1, 0), "Gpr", 4, False, "LDRSW X0,[X1,X2]")
expect(reg_load(0b00, 0b10, 4, 1, 6), "Gpr", 1, False, "LDRSB W6,[X1,X4]")
expect(reg_load(0b01, 0b11, 4, 1, 6), "Gpr", 2, False, "LDRSH X6,[X1,X4]")
expect(reg_load(0b11, 0b01, 2, 1, 0, option=0b010, s=0), "Gpr", 8, False, "LDR X0,[X1,X2,UXTW]")
expect(reg_load(0b11, 0b01, 2, 1, 0, option=0b011, s=1), "Gpr", 8, False, "LDR X0,[X1,X2,LSL #3]")
expect(simd_reg_load(0b00, 0b01, 2, 1, 0), "Simd", 1, False, "LDR B0,[X1,X2]")
expect(simd_reg_load(0b11, 0b01, 2, 1, 0), "Simd", 8, False, "LDR D0,[X1,X2]")
expect(simd_reg_load(0b00, 0b11, 2, 1, 0), "Simd", 16, False, "LDR Q0,[X1,X2]")
# --- must NOT recover: stores, writeback forms, atomics, exclusives
def expect_reject(insn, what):
    global ok, fail
    if decode(insn) is None:
        ok += 1
    else:
        fail += 1
        print(f"FAIL (should reject) {what}: insn=0x{insn:08x} -> {decode(insn)}")

expect_reject(imm_store(0b11, 0b00, 5, 1, 0), "STR X0,[X1,#40]")
expect_reject(imm_store(0b00, 0b00, 5, 1, 0), "STRB W0,[X1,#5]")
expect_reject(post_index(0b11, 0b01, 1, 1, 0), "LDR X0,[X1],#1 (post-index writeback)")
expect_reject(pre_index(0b11, 0b01, 1, 1, 0), "LDR X0,[X1,#1]! (pre-index writeback)")
expect_reject((0b10101001 << 24) | (0b11 << 23) | (1 << 15) | (1 << 10) | (2 << 5), "LDP pre-index")
expect_reject((0b10101000 << 24) | (1 << 15) | (1 << 10) | (2 << 5), "LDP post-index")
expect_reject(0xC85F8400, "LDXR X0,[X1] (0xC85F8400?) exclusive")
expect_reject(0x38B07C00, "LDADD-style atomic (0x38B07C00)")
expect_reject(0xF9800000 | (1 << 5), "PRFM (prefetch never faults)")
expect_reject(0x58  << 24 | (1 << 5), "LDR X0,=literal")

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
