# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import find_callers, os, re

nid_map = {}
for r, d, fs in os.walk('core/src/libs'):
    for f in fs:
        if f.endswith('.cpp'):
            with open(os.path.join(r, f), 'r', errors='ignore') as fp:
                for line in fp:
                    m = re.search(r'LIB_FUNC\("([^"]+)",\s*([^,\)]+)', line)
                    if m:
                        nid, fn = m.groups()
                        nid_map[nid] = (f, fn.strip())

resolved = {}
unresolved = []
for addr, nid in find_callers.plt_targets.items():
    clean_nid = nid.split('#')[0]
    if clean_nid in nid_map:
        resolved[addr] = nid_map[clean_nid]
    elif nid in nid_map:
        resolved[addr] = nid_map[nid]
    else:
        unresolved.append((addr, nid))

print(f"Resolved: {len(resolved)} / {len(find_callers.plt_targets)}")
if unresolved:
    print(f"Unresolved count: {len(unresolved)}")
    for u in unresolved[:10]:
        print("  Unresolved:", hex(u[0]), u[1])

by_lib = {}
for addr, (lib, fn) in resolved.items():
    by_lib.setdefault(lib, []).append(fn)

for lib, fns in sorted(by_lib.items()):
    print(f"=== {lib} ({len(fns)}) ===")
    for fn in fns:
        print("  *", fn)
