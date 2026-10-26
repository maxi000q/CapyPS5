# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import re
import struct

parser = argparse.ArgumentParser(description="List the SCE tags of a SELF/ELF eboot")
parser.add_argument("eboot", help="path to the game's eboot.bin")
args = parser.parse_args()

with open(args.eboot, 'rb') as f:
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

dyn_ph = None
for ph in phdrs:
    if ph['type'] == 2: dyn_ph = ph; break

dyn_off = vaddr_to_file(dyn_ph['vaddr'])
entries = []
for i in range(dyn_ph['filesz'] // 16):
    tag, val = struct.unpack_from('<Qq', data, dyn_off + i*16)
    if tag == 0: break
    entries.append((tag, val))

strtab_vaddr = None
for tag, val in entries:
    if tag == 5: strtab_vaddr = val

def get_str(str_off):
    fo = vaddr_to_file(strtab_vaddr + str_off)
    if fo is None: return ""
    end = data.find(b'\x00', fo)
    return data[fo:end].decode('latin1', 'replace')

# Look for SCE library import structures or symbols
# In SCE ELF, DT_SCE_IMPORT_LIB is tag 0x61000015, DT_SCE_LIB_NAME is tag 0x61000017, etc.
for tag, val in entries:
    if hex(tag).startswith('0x6100'):
        # SCE tag
        name = get_str(val) if val < 0x100000 else f"vaddr {hex(val)}"
        print(f"SCE tag {hex(tag)}: {val} -> {name}")
