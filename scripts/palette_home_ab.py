#!/usr/bin/env python3
"""Palette packs against direct SH 1 at the app's start view (Phase 10, docs/sh1-palette-research.md).

The gate A poses (`quality_ab.py`) orbit the whole scene from outside its framing. The app opens a
scene at its home view instead (ADR-0014): close to the scene's fine detail. This script measures the
packs that `palette_ab.py` left in --work at that view: the desktop host's home view for the portrait
phone frame, swung about the up axis by each --swing angle, at each --pull (eye distance x pull). Every
pack is rendered at the same poses as the direct SH 1 pack; PSNR per pose and min / mean per pack.

Packs are found by palette_ab.py's names: <scene>.direct.pspk and
<scene>.k<K>.emeans.e<E>.s<seed>.pspk.

Usage:
  python scripts/palette_home_ab.py --exe build/desktop/Release/pocketsplat-desktop.exe \\
      --work bench-out/palette-k --scene beetle --scene museum1 --k 4096 16384 \\
      --seed 1 2 3 --csv docs/benchmarks/palette-size-home-<hash>-<date>.csv

stdlib + numpy only.
"""
import argparse
import csv
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HOME_RE = re.compile(r"home view: focus (\S+) eye (\S+) up (\S+)")
RENDER_FLAGS = ["--key-bits", "32", "--min-contribution", "0"]  # as palette_ab.py


def vec(text):
    return [float(x) for x in text.split(",")]


def fmt(v):
    return ",".join(f"{x:.5f}" for x in v)


def rotate(v, axis, angle):  # Rodrigues
    c, s = math.cos(angle), math.sin(angle)
    d = sum(a * b for a, b in zip(axis, v))
    cross = [axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]]
    return [v[i] * c + cross[i] * s + axis[i] * d * (1 - c) for i in range(3)]


def run(exe, pack, size, extra, out):
    cmd = [str(exe), str(pack), "--offscreen", "--width", str(size[0]), "--height", str(size[1]), "--no-hud",
           "--frames", "2", "--screenshot", str(out)] + RENDER_FLAGS + extra
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"render failed ({result.returncode}): {result.stderr.strip()[-400:]}")
    return result.stdout


def read_ppm(path):
    data = Path(path).read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data[i + 1:i + 1 + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return math.inf if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def home_poses(exe, direct, size, swings, pulls, tmp):
    match = HOME_RE.search(run(exe, direct, size, ["--home-view"], tmp / "home.ppm"))
    if not match:
        sys.exit("no 'home view:' line in the host's output")
    focus, eye, up = (vec(g) for g in match.groups())
    poses = []
    for pull in pulls:
        offset = [pull * (e - f) for e, f in zip(eye, focus)]
        for swing in swings:
            e = [f + o for f, o in zip(focus, rotate(offset, up, math.radians(swing)))]
            poses.append((f"{swing:g}/{pull:g}", ["--look-from", fmt(e), "--look-at", fmt(focus), "--up", fmt(up)]))
    return poses


def build_hash():
    repo = Path(__file__).resolve().parent.parent
    head = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True, check=True).stdout.strip()
    return head + ("-dirty" if dirty else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path, help="palette_ab.py's --work directory")
    ap.add_argument("--scene", action="append", required=True)
    ap.add_argument("--k", type=int, nargs="+", default=[4096])
    ap.add_argument("--epochs", type=int, default=3)
    ap.add_argument("--seed", type=int, nargs="+", default=[1, 2, 3])
    ap.add_argument("--swing", type=float, nargs="+", default=[-25, -12.5, 0, 12.5, 25], help="degrees about up")
    ap.add_argument("--pull", type=float, nargs="+", default=[1.0, 0.6], help="eye distance x this")
    ap.add_argument("--width", type=int, default=864)
    ap.add_argument("--height", type=int, default=1872)
    ap.add_argument("--csv", required=True)
    args = ap.parse_args()
    exe = args.exe.resolve()
    size = (args.width, args.height)
    build = build_hash()
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        shot = tmp / "shot.ppm"
        for scene in args.scene:
            direct = (args.work / f"{scene}.direct.pspk").resolve()
            poses = home_poses(exe, direct, size, args.swing, args.pull, tmp)
            ref = []
            for _, pose in poses:
                run(exe, direct, size, pose, shot)
                ref.append(read_ppm(shot))
            for k in args.k:
                for seed in args.seed:
                    pack = (args.work / f"{scene}.k{k}.emeans.e{args.epochs}.s{seed}.pspk").resolve()
                    if not pack.exists():
                        sys.exit(f"missing pack {pack}")
                    scores = []
                    for _, pose in poses:
                        run(exe, pack, size, pose, shot)
                        scores.append(psnr(read_ppm(shot), ref[len(scores)]))
                    rows.append({"build": build, "scene": scene, "k": k, "seed": seed,
                                 "epochs": args.epochs, "psnr_min_db": f"{min(scores):.2f}",
                                 "psnr_mean_db": f"{sum(scores) / len(scores):.2f}",
                                 "psnr_per_pose_db": " ".join(f"{s:.2f}" for s in scores),
                                 "poses": " ".join(name for name, _ in poses), "resolution": f"{size[0]}x{size[1]}",
                                 "render_flags": " ".join(RENDER_FLAGS)})
                    print(f"{scene:>16} k {k:>6} s{seed}: min {min(scores):6.2f} mean "
                          f"{sum(scores) / len(scores):6.2f} dB", flush=True)
            with open(args.csv, "w", newline="") as f:  # after every scene: partial results survive
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
    print(f"summary csv written: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
