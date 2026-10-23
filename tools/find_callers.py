# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import struct
import capstone

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

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

def file_to_vaddr(offset):
    for phdr_id, soffset, csize, dsize in self_segs:
        ph = phdrs[phdr_id]
        if soffset <= offset < soffset + csize:
            return ph['vaddr'] + (offset - soffset)
    return None

# Find DYNAMIC segment (type 2)
dynamic_ph = next(p for p in phdrs if p['type'] == 2)
dyn_off = vaddr_to_file(dynamic_ph['vaddr'])
tags = {}
for i in range(dynamic_ph['filesz'] // 16):
    tag, val = struct.unpack_from('<QQ', data, dyn_off + i*16)
    tags[tag] = val

DT_STRTAB = tags.get(5)
DT_SYMTAB = tags.get(6)
DT_JMPREL = tags.get(23)
DT_PLTRELSZ = tags.get(2)
DT_RELA = tags.get(7)
DT_RELASZ = tags.get(8)

str_off = vaddr_to_file(DT_STRTAB)
sym_off = vaddr_to_file(DT_SYMTAB)
jmprel_off = vaddr_to_file(DT_JMPREL)

def get_str(idx):
    end = data.find(b'\0', str_off + idx)
    return data[str_off + idx : end].decode('latin1')

print(f"PLT Relocs count: {DT_PLTRELSZ // 24}")
plt_targets = {}
for i in range(DT_PLTRELSZ // 24):
    r_offset, r_info, r_addend = struct.unpack_from('<QQq', data, jmprel_off + i*24)
    r_sym = r_info >> 32
    sym_name_idx = struct.unpack_from('<I', data, sym_off + r_sym*24)[0]
    name = get_str(sym_name_idx)
    plt_targets[r_offset] = name
    if "DirectMemory" in name or "Video" in name or "Pad" in name:
        print(f"PLT slot 0x{r_offset:x} -> {name}")

