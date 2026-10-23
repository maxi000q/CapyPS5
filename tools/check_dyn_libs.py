# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import os, re, struct

path = r"C:\Users\LeYJa\Downloads\Dead.Cells.PS5-DUPLEX\Dead.Cells.PS5-DUPLEX\PPSA15552-app0\eboot.bin"
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

# Look at how GetDynLibs and GetDynModules work in runtimeLinker.cpp:
# Let's find GetDynLibs implementation in runtimeLinker.cpp
