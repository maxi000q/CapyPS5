# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import struct
import sys

if len(sys.argv) < 2:
    sys.exit("usage: resolve_unresolved.py <eboot.bin> [nid#lib#mod ...]\n"
             "       each symbol is NID#lib-id#mod-id as printed by the loader;")
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
for tag, val in entries:
    if tag == 5: strtab_vaddr = val

def get_str(str_off):
    fo = vaddr_to_file(strtab_vaddr + str_off)
    if fo is None: return ""
    end = data.find(b'\x00', fo)
    return data[fo:end].decode('latin1', 'replace')

b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
def encode_id64(num):
    if num < 0x40:
        return b64[num]
    elif num < 0x1000:
        return b64[(num >> 6) & 0x3f] + b64[num & 0x3f]
    else:
        return b64[(num >> 12) & 0x3f] + b64[(num >> 6) & 0x3f] + b64[num & 0x3f]

import_libs = {}
import_mods = {}

for tag, val in entries:
    if tag in (0x61000019, 0x61000045): # DT_OS_IMPORT_LIB
        id_str = encode_id64((val >> 48) & 0xffff)
        name = get_str(val & 0xffffffff)
        import_libs[id_str] = name
    elif tag in (0x61000049, 0x61000047): # DT_OS_NEEDED_MODULE
        id_str = encode_id64((val >> 48) & 0xffff)
        name = get_str(val & 0xffffffff)
        import_mods[id_str] = name

print("Import Libs:", import_libs)
print("Import Mods:", import_mods)

unresolved_syms = sys.argv[2:]
if not unresolved_syms:
    sys.exit("no symbols given; pass them as arguments")

for sym in unresolved_syms:
    nid, lib_id, mod_id = sym.split('#')
    lib_name = import_libs.get(lib_id, "unknown_lib")
    mod_name = import_mods.get(mod_id, "unknown_mod")
    print(f"NID {nid} -> Lib: {lib_name}, Mod: {mod_name}")
