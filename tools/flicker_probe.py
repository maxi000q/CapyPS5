# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

"""Measures frame-to-frame flicker on the device screen.

Captures a burst of screenshots over adb and reports how many pixels toggle between
consecutive frames. Flicker shows up as a high "toggle" score: pixels that change in one
pair and change back in the next (A -> B -> A), which normal motion rarely produces.

    python tools/flicker_probe.py [--frames 10] [--device 282f344c] [--out scratch/probe]
"""
import argparse
import io
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image


def capture(device: str) -> np.ndarray:
    png = subprocess.run(["adb", "-s", device, "exec-out", "screencap", "-p"],
                         check=True, capture_output=True).stdout
    return np.asarray(Image.open(io.BytesIO(png)).convert("RGB")).astype(np.int16)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--device", default="282f344c")
    parser.add_argument("--out", default=None, help="directory to save frames and a heatmap")
    parser.add_argument("--threshold", type=int, default=60, help="summed RGB delta per pixel")
    args = parser.parse_args()

    frames = [capture(args.device) for _ in range(args.frames)]
    # Ignore the performance overlay in the top-left corner.
    for f in frames:
        f[:120, :520] = 0

    changed = [np.abs(a - b).sum(2) > args.threshold for a, b in zip(frames, frames[1:])]
    toggles = []
    for i in range(len(frames) - 2):
        back = np.abs(frames[i] - frames[i + 2]).sum(2) <= args.threshold
        toggles.append(changed[i] & changed[i + 1] & back)

    change_pct = [100 * c.mean() for c in changed]
    toggle_pct = [100 * t.mean() for t in toggles]
    print("changed %:", " ".join(f"{v:.2f}" for v in change_pct))
    print("toggle  %:", " ".join(f"{v:.2f}" for v in toggle_pct))
    print(f"FLICKER SCORE (mean toggle %): {np.mean(toggle_pct):.3f}")

    if args.out:
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        heat = np.clip(np.sum(toggles, axis=0) / max(len(toggles), 1) * 255, 0, 255).astype(np.uint8)
        Image.fromarray(heat).resize((960, 540)).save(out / "toggle_heatmap.png")
        for i, f in enumerate(frames):
            Image.fromarray(f.astype(np.uint8)).save(out / f"frame{i:02d}.png")


if __name__ == "__main__":
    main()
