# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

import subprocess, glob

res = subprocess.run(['adb', 'shell', "for t in /proc/22956/task/*; do tid=$(basename $t); s=$(cat $t/stat); u=$(echo $s | cut -d' ' -f14); k=$(echo $s | cut -d' ' -f15); c=$(cat $t/comm); echo \"$tid ($c): utime=$u stime=$k\"; done"], capture_output=True, text=True)
print(res.stdout)
