# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import os, re, struct

registered_nids = set()
for root, dirs, files in os.walk('core/src/libs'):
    for f in files:
        if f.endswith('.cpp'):
            with open(os.path.join(root, f), 'r', encoding='utf-8', errors='ignore') as s:
                for line in s:
                    m = re.search(r'LIB_FUNC\("([^"]+)"', line)
                    if m:
                        registered_nids.add(m.group(1))

with open('celeste_eboot.bin', 'rb') as f:
    data = f.read()

ehdr_pos = 32 + 12 * 32
ph_start = ehdr_pos + 0x40
phdrs = []
for i in range(14):
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, ph_start + i*56)
    phdrs.append({'p_type': p_type, 'p_offset': p_offset, 'p_vaddr': p_vaddr, 'p_filesz': p_filesz})

segs = []
for i in range(12):
    stype, soffset, csize, dsize = struct.unpack_from('<QQQQ', data, 32 + i*32)
    if (stype & 0x800) != 0:
        phdr_id = (stype >> 20) & 0xFFF
        segs.append((phdr_id, soffset, csize, dsize))

def read_vaddr_bytes(vaddr, size):
    for phdr_id, soffset, csize, dsize in segs:
        ph = phdrs[phdr_id]
        if ph['p_vaddr'] <= vaddr < ph['p_vaddr'] + ph['p_filesz']:
            file_off = soffset + (vaddr - ph['p_vaddr'])
            return data[file_off:file_off+size]
    return b''

strtab = read_vaddr_bytes(0x1cac7c0, 0x16af)
def get_str(off):
    end = strtab.find(b'\x00', off)
    return strtab[off:end].decode('latin1', 'replace')

symtab = read_vaddr_bytes(0x1cade70, 0x1cafeb0 - 0x1cade70)
def get_sym_name(sym_idx):
    st_name = struct.unpack_from('<I', symtab, sym_idx * 24)[0]
    return get_str(st_name)

# JMPREL first, then RELA
stubbed_imports = []
seen_vaddrs = set()

def process_table(table_vaddr, table_size, name):
    tb = read_vaddr_bytes(table_vaddr, table_size)
    for i in range(len(tb) // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', tb, i * 24)
        sym_idx = r_info >> 32
        r_type = r_info & 0xffffffff
        if sym_idx != 0:
            sym_name = get_sym_name(sym_idx)
            nid = sym_name.split('#')[0] if '#' in sym_name else sym_name
            if nid not in registered_nids:
                if r_offset not in seen_vaddrs:
                    seen_vaddrs.add(r_offset)
                    stubbed_imports.append((len(stubbed_imports), name, i, hex(r_offset), nid, sym_name))

process_table(0x1cafeb0, 0xf0, 'JMPREL')
process_table(0x1caffa0, 0x7dd30, 'RELA')

print(f"Total stubbed imports: {len(stubbed_imports)}")
for rec_id, tbl, i, vaddr, nid, sname in stubbed_imports[:40]:
    print(f"record_id [{rec_id}]: {tbl}[{i}] vaddr={vaddr} nid={nid} sym={sname}")
