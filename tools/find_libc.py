# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import re

parser = argparse.ArgumentParser(description="List libC.cpp functions whose NIDs appear in an eboot")
parser.add_argument("eboot", help="path to the game's eboot.bin")
parser.add_argument("--source", default="core/src/libs/libC.cpp", help="path to libC.cpp")
args = parser.parse_args()

with open(args.eboot, "rb") as f:
    data = f.read()

with open(args.source, "r", encoding="utf-8") as fp:
    for line in fp:
        m = re.search(r'LIB_FUNC\("([^"]+)",\s*LibcInternal::([^)]+)\)', line)
        if m:
            nid = m.group(1).encode()
            fn = m.group(2)
            if data.find(nid) != -1:
                print(fn, nid.decode())
