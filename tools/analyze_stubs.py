# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import struct
import os
import re

with open('libc.prx', 'rb') as f:
    data = f.read()

ident, size1, size2, file_size, segs_num, unk, pad = struct.unpack_from('<12sHHQHHI', data, 0)
ehdr_pos = 32 + segs_num * 32
e_phnum = struct.unpack_from('<H', data, ehdr_pos + 56)[0]
e_phoff = struct.unpack_from('<Q', data, ehdr_pos + 32)[0]
ph_start = ehdr_pos + e_phoff

phdrs = []
for i in range(e_phnum):
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, ph_start + i*56)
    phdrs.append({'p_type': p_type, 'p_offset': p_offset, 'p_vaddr': p_vaddr, 'p_filesz': p_filesz})

segs = []
for i in range(segs_num):
    stype, soffset, csize, dsize = struct.unpack_from('<QQQQ', data, 32 + i*32)
    if (stype & 0x800) != 0:
        phdr_id = (stype >> 20) & 0xFFF
        segs.append((phdr_id, soffset, csize, dsize))

def read_prx(vaddr, size):
    for phdr_id, soffset, csize, dsize in segs:
        ph = phdrs[phdr_id]
        if ph['p_vaddr'] <= vaddr < ph['p_vaddr'] + ph['p_filesz']:
            file_off = soffset + (vaddr - ph['p_vaddr'])
            return data[file_off:file_off+size]
    return b''

dyn_ph = next(p for p in phdrs if p['p_type'] == 2)
dyn_data = read_prx(dyn_ph['p_vaddr'], dyn_ph['p_filesz'])

dyn_tags = []
for i in range(len(dyn_data) // 16):
    tag, val = struct.unpack_from('<Qq', dyn_data, i * 16)
    if tag == 0: break
    dyn_tags.append((tag, val))

dyn_map = {tag: val for tag, val in dyn_tags}
strtab_addr = dyn_map.get(0x61000035, dyn_map.get(5))
strsz = dyn_map.get(0x61000037, dyn_map.get(10))
strtab = read_prx(strtab_addr, strsz)

symtab_addr = dyn_map.get(0x61000039, dyn_map.get(6))
symtab_sz = dyn_map.get(0x6100003f, 0)
symtab = read_prx(symtab_addr, symtab_sz)

def get_str(off):
    end = strtab.find(b'\x00', off)
    return strtab[off:end].decode('latin1', 'replace')

num_syms = len(symtab) // 24
print(f'Total symbols in libc.prx: {num_syms}')

prx_symbols = {}
for i in range(num_syms):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', symtab, i * 24)
    if st_name != 0:
        name = get_str(st_name)
        nid = name.split('#')[0]
        prx_symbols[nid] = (name, hex(st_value), hex(st_size), st_info)

with open('celeste_eboot.bin', 'rb') as f:
    eboot_data = f.read()

eb_ehdr_pos = 32 + 12 * 32
eb_ph_start = eb_ehdr_pos + 0x40
eb_phdrs = []
for i in range(14):
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', eboot_data, eb_ph_start + i*56)
    eb_phdrs.append({'p_type': p_type, 'p_offset': p_offset, 'p_vaddr': p_vaddr, 'p_filesz': p_filesz})

eb_segs = []
for i in range(12):
    stype, soffset, csize, dsize = struct.unpack_from('<QQQQ', eboot_data, 32 + i*32)
    if (stype & 0x800) != 0:
        phdr_id = (stype >> 20) & 0xFFF
        eb_segs.append((phdr_id, soffset, csize, dsize))

def read_eb_vaddr(vaddr, size):
    for phdr_id, soffset, csize, dsize in eb_segs:
        ph = eb_phdrs[phdr_id]
        if ph['p_vaddr'] <= vaddr < ph['p_vaddr'] + ph['p_filesz']:
            file_off = soffset + (vaddr - ph['p_vaddr'])
            return eboot_data[file_off:file_off+size]
    return b''

eb_strtab = read_eb_vaddr(0x1cac7c0, 0x16af)
def get_eb_str(off):
    end = eb_strtab.find(b'\x00', off)
    return eb_strtab[off:end].decode('latin1', 'replace')

eb_symtab = read_eb_vaddr(0x1cade70, 0x1cafeb0 - 0x1cade70)
def get_eb_sym_name(sym_idx):
    st_name = struct.unpack_from('<I', eb_symtab, sym_idx * 24)[0]
    return get_eb_str(st_name)

registered_nids = set()
for root, dirs, files in os.walk('core/src/libs'):
    for f in files:
        if f.endswith('.cpp'):
            with open(os.path.join(root, f), 'r', encoding='utf-8', errors='ignore') as s:
                for line in s:
                    m = re.search(r'LIB_FUNC\("([^"]+)"', line)
                    if m: registered_nids.add(m.group(1))

stubbed = []
seen = set()
def process_table(table_vaddr, table_size):
    tb = read_eb_vaddr(table_vaddr, table_size)
    for i in range(len(tb) // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', tb, i * 24)
        sym_idx = r_info >> 32
        if sym_idx != 0:
            sym_name = get_eb_sym_name(sym_idx)
            nid = sym_name.split('#')[0]
            if nid not in registered_nids and r_offset not in seen:
                seen.add(r_offset)
                stubbed.append((len(stubbed), hex(r_offset), nid, sym_name))

process_table(0x1cafeb0, 0xf0)
process_table(0x1caffa0, 0x7dd30)

print(f'Total stubbed imports in eboot: {len(stubbed)}')
matched = 0
for rec_id, vaddr, nid, sname in stubbed:
    if nid in prx_symbols:
        matched += 1
        if rec_id < 45:
            print(f'stub [{rec_id:2d}] vaddr={vaddr} nid={nid} in libc.prx: {prx_symbols[nid][0]} at {prx_symbols[nid][1]}')
    else:
        if rec_id < 45:
            print(f'stub [{rec_id:2d}] vaddr={vaddr} nid={nid} NOT in libc.prx ({sname})')

print(f'Matched with libc.prx: {matched} / {len(stubbed)}')
