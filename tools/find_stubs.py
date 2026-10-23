# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import find_callers, struct
data = find_callers.data

for target_vaddr in [0x1ae08a8, 0x1ae08b0]:
    name = 'KernelGetDirectMemorySize' if target_vaddr == 0x1ae08a8 else 'KernelAllocateDirectMemory'
    print(f'=== Callers to {name} (0x{target_vaddr:x}) ===')
    for phdr_id, soffset, csize, dsize in find_callers.self_segs:
        ph = find_callers.phdrs[phdr_id]
        if ph['flags'] & 1: # executable
            seg_data = data[soffset:soffset+csize]
            for i in range(len(seg_data) - 6):
                if seg_data[i] == 0xFF and seg_data[i+1] in (0x15, 0x25):
                    disp = struct.unpack_from('<i', seg_data, i+2)[0]
                    vaddr = ph['vaddr'] + i
                    dest = vaddr + 6 + disp
                    if dest == target_vaddr:
                        kind = "call" if seg_data[i+1] == 0x15 else "jmp"
                        print(f"PLT stub at 0x{vaddr:x}: {kind} [0x{dest:x}]")
