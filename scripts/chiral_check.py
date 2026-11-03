#!/usr/bin/env python3
"""Chiral orientation gate: catches any y-mirror in the projection/covariance chain.

Image-vs-self goldens cannot catch a mirrored renderer — it is self-consistent — which is how
the EWA Jacobian y-row sign bug (S12, docs/silhouette-rays-research.md) survived every golden
and sweep. This test renders a fully synthetic, deliberately chiral scene: one bright splat at
the origin elongated along world +x+y (RUB), viewed head-on from +z. With y-down pixels the
projected streak must run up-right, i.e. the luma-weighted pixel covariance cov_xy must be
clearly negative. A y-mirrored covariance flips the slope and fails the gate.

Fully self-contained: generates the PLY, packs it with splatconv, renders, measures moments.

Usage:
  python scripts/chiral_check.py --exe <pocketsplat-desktop> --splatconv <splatconv> \
      --workdir <scratch dir>
"""
import argparse
import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from golden_check import read_ppm  # noqa: E402

WIDTH, HEIGHT = 800, 450


def write_chiral_ply(path: Path) -> None:
    # RUB coordinates (pass --in-coords RUB to splatconv so nothing is flipped on import).
    # Central splat: log-scales (major, minor, minor), major axis = local x rotated +45 deg
    # about +z -> world +x+y. Four dim isotropic anchors at (+-1, +-1, 0) define the framing
    # box; they project just off the top/bottom image edges and outside the measured crop.
    half = math.cos(math.radians(22.5)), math.sin(math.radians(22.5))
    rows = []
    # x, y, z, f_dc x3, opacity(logit), log-scales x3, rot (w, x, y, z)
    rows.append([0.0, 0.0, 0.0, 1.6, 1.6, 1.6, 3.0,
                 math.log(0.25), math.log(0.02), math.log(0.02),
                 half[0], 0.0, 0.0, half[1]])
    for ax in (-1.0, 1.0):
        for ay in (-1.0, 1.0):
            rows.append([ax, ay, 0.0, 0.0, 0.0, 0.0, -2.0,
                         math.log(0.02), math.log(0.02), math.log(0.02),
                         1.0, 0.0, 0.0, 0.0])
    header = ("ply\nformat binary_little_endian 1.0\n"
              f"element vertex {len(rows)}\n"
              + "".join(f"property float {p}\n" for p in ("x", "y", "z"))
              + "".join(f"property float f_dc_{i}\n" for i in range(3))
              + "property float opacity\n"
              + "".join(f"property float scale_{i}\n" for i in range(3))
              + "".join(f"property float rot_{i}\n" for i in range(4))
              + "end_header\n")
    with path.open("wb") as f:
        f.write(header.encode("ascii"))
        for row in rows:
            f.write(struct.pack("<14f", *row))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--splatconv", required=True)
    parser.add_argument("--workdir", required=True)
    args = parser.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    ply, pack, shot = work / "chiral.ply", work / "chiral.pspk", work / "chiral.ppm"
    exe = str(Path(args.exe).resolve())  # CreateProcess rejects forward-slash relative paths
    splatconv = str(Path(args.splatconv).resolve())

    write_chiral_ply(ply)
    subprocess.run([splatconv, "pack", str(ply), str(pack), "--sh", "0",
                    "--in-coords", "RUB"], check=True, capture_output=True)
    subprocess.run([exe, str(pack), "--no-hud", "--frames", "20",
                    "--yaw-deg", "0", "--pitch-deg", "0",
                    "--width", str(WIDTH), "--height", str(HEIGHT),
                    "--screenshot", str(shot)], check=True, capture_output=True)

    w, h, buf = read_ppm(shot)
    img = np.frombuffer(buf, np.uint8).reshape(h, w, 3)
    crop = img[h // 4: 3 * h // 4, w // 4: 3 * w // 4]
    luma = crop.astype(np.float64).mean(axis=2)
    mask = luma > 30.0
    count = int(mask.sum())
    if count < 500:
        print(f"FAIL: only {count} bright pixels in the central crop - scene did not render")
        return 1
    ys, xs = np.nonzero(mask)
    wgt = luma[ys, xs]
    mx, my = np.average(xs, weights=wgt), np.average(ys, weights=wgt)
    cxx = np.average((xs - mx) ** 2, weights=wgt)
    cyy = np.average((ys - my) ** 2, weights=wgt)
    cxy = np.average((xs - mx) * (ys - my), weights=wgt)
    corr = cxy / math.sqrt(max(cxx * cyy, 1e-12))
    print(f"bright px {count}, pixel-space orientation correlation {corr:+.3f} "
          f"(expected strongly negative: +x+y world streak runs up-right, pixel y is down)")
    if corr > -0.5:
        print("FAIL: projected major-axis slope has the wrong handedness - "
              "the screen covariance is y-mirrored (see docs/silhouette-rays-research.md)")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
