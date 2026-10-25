# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import os, struct
import capstone

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

import sys
if len(sys.argv) < 2:
    sys.exit("usage: disasm_deadcells.py <eboot.bin>")
path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()

ehdr_pos = 32 + 12 * 32
num_segs = struct.unpack_from('<H', data, 0x18)[0]
ph_offset = struct.unpack_from('<Q', data, ehdr_pos + 0x20)[0]
ph_num = struct.unpack_from('<H', data, ehdr_pos + 0x38)[0]
ph_size = struct.unpack_from('<H', data, ehdr_pos + 0x36)[0]

self_segs = []
for i in range(num_segs):
    stype, soffset, csize, dsize = struct.unpack_from('<QQQQ', data, 32 + i*32)
    if (stype & 0x800) != 0:
        phdr_id = (stype >> 20) & 0xFFF
        self_segs.append((phdr_id, soffset, csize, dsize))
        
phdrs = []
for i in range(ph_num):
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
        '<IIQQQQQQ', data, ehdr_pos + ph_offset + i*ph_size)
    phdrs.append({'type': p_type, 'flags': p_flags, 'vaddr': p_vaddr, 'filesz': p_filesz, 'memsz': p_memsz})

def vaddr_to_file(vaddr):
    for phdr_id, soffset, csize, dsize in self_segs:
        ph = phdrs[phdr_id]
        if ph['vaddr'] <= vaddr < ph['vaddr'] + ph['filesz']:
            return soffset + (vaddr - ph['vaddr'])
    return None

def disasm_at(vaddr, count=25):
    fo = vaddr_to_file(vaddr)
    if fo is None:
        print(f"vaddr {hex(vaddr)} not mapped!")
        return
    code = data[fo:fo+100]
    print(f"--- Disassembly at {hex(vaddr)} ---")
    for ins in md.disasm(code, vaddr):
        print(f"0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")

disasm_at(0x74e9e0)
disasm_at(0x751400)
