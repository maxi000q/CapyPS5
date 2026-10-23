# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import find_callers, struct
data = find_callers.data

def find_callers_to(target_vaddr, name):
    print(f"=== Callers to {name} (0x{target_vaddr:x}) ===")
    callers = []
    for phdr_id, soffset, csize, dsize in find_callers.self_segs:
        ph = find_callers.phdrs[phdr_id]
        if ph['flags'] & 1:
            seg_data = data[soffset:soffset+csize]
            for i in range(len(seg_data) - 5):
                # E8 disp32 (call rel32)
                if seg_data[i] == 0xE8:
                    disp = struct.unpack_from('<i', seg_data, i+1)[0]
                    vaddr = ph['vaddr'] + i
                    dest = vaddr + 5 + disp
                    if dest == target_vaddr:
                        callers.append(vaddr)
                        print(f"Call at 0x{vaddr:x}")
    return callers

get_size_callers = find_callers_to(0x17459c0, "KernelGetDirectMemorySize")
alloc_callers = find_callers_to(0x17459d0, "KernelAllocateDirectMemory")

import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

for c in alloc_callers:
    print(f"\n--- Disassembly around 0x{c:x} ---")
    fo = find_callers.vaddr_to_file(c - 40)
    code = data[fo:fo+80]
    for ins in md.disasm(code, c - 40):
        print(f"0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")

