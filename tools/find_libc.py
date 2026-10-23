# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import os
import re

path = r"C:\Users\LeYJa\Downloads\Dead.Cells.PS5-DUPLEX\Dead.Cells.PS5-DUPLEX\PPSA15552-app0\eboot.bin"
with open(path, "rb") as f:
    data = f.read()

with open("core/src/libs/libC.cpp", "r", encoding="utf-8") as fp:
    for line in fp:
        m = re.search(r'LIB_FUNC\("([^"]+)",\s*LibcInternal::([^)]+)\)', line)
        if m:
            nid = m.group(1).encode()
            fn = m.group(2)
            if data.find(nid) != -1:
                print(fn, nid.decode())
