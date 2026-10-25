# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import os, re, struct

# Collect registered NIDs in CapyPS5
registered_nids = set()
for root, dirs, files in os.walk('core/src/libs'):
    for f in files:
        if f.endswith('.cpp'):
            with open(os.path.join(root, f), 'r', encoding='utf-8', errors='ignore') as s:
                for line in s:
                    m = re.search(r'LIB_FUNC\("([^"]+)"', line)
                    if m:
                        registered_nids.add(m.group(1))

# Also read libc.prx symbols from Dead Cells
import sys
libc_path = sys.argv[2] if len(sys.argv) > 2 else ""  # optional libc.prx
libc_symbols = set()
if os.path.exists(libc_path):
    with open(libc_path, 'rb') as f:
        lc_data = f.read()
    # Simple search for NID strings (e.g. 11 chars + '#' + ...)
    # But even better: scan strings table
    for m in re.finditer(rb'([A-Za-z0-9\+\-_]{11})#', lc_data):
        libc_symbols.add(m.group(1).decode('latin1'))

print(f"CapyPS5 HLE NIDs: {len(registered_nids)}")
print(f"libc.prx NIDs: {len(libc_symbols)}")

if len(sys.argv) < 2:
    sys.exit("usage: analyze_deadcells.py <eboot.bin> [libc.prx]")
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
symtab_vaddr = None
jmprel_vaddr = None
pltrelsz = 0
rela_vaddr = None
relasz = 0

for tag, val in entries:
    if tag == 5: strtab_vaddr = val
    elif tag == 6: symtab_vaddr = val
    elif tag == 23: jmprel_vaddr = val
    elif tag == 2: pltrelsz = val
    elif tag == 7: rela_vaddr = val
    elif tag == 8: relasz = val

def get_str(str_off):
    fo = vaddr_to_file(strtab_vaddr + str_off)
    if fo is None: return ""
    end = data.find(b'\x00', fo)
    return data[fo:end].decode('latin1', 'replace')

def get_sym_name(sym_idx):
    fo = vaddr_to_file(symtab_vaddr + sym_idx * 24)
    if fo is None: return ""
    st_name = struct.unpack_from('<I', data, fo)[0]
    return get_str(st_name)

unresolved = []
total_imports = 0

def scan_rela(table_vaddr, table_size, name):
    global total_imports
    if not table_vaddr or not table_size: return
    fo = vaddr_to_file(table_vaddr)
    for i in range(table_size // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', data, fo + i*24)
        sym_idx = r_info >> 32
        if sym_idx != 0:
            total_imports += 1
            sym_name = get_sym_name(sym_idx)
            nid = sym_name.split('#')[0] if '#' in sym_name else sym_name
            if nid not in registered_nids and nid not in libc_symbols:
                unresolved.append((name, i, hex(r_offset), nid, sym_name))

scan_rela(jmprel_vaddr, pltrelsz, 'JMPREL')
scan_rela(rela_vaddr, relasz, 'RELA')

print(f"Total imports in Dead Cells: {total_imports}")
print(f"Unresolved imports: {len(unresolved)}")
for u in unresolved[:30]:
    print(f"  {u}")
