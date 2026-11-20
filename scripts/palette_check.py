#!/usr/bin/env python3
"""Palette golden (Phase 8, pack v3): a palette pack must render like its snapped direct pack.

A palette pack stores each splat's SH1 as an index into the palette; `splatconv pack
--palette-snap` writes the same palette as an ordinary direct SH 1 pack (every splat's SH1
replaced by its entry). Both decode to the same coefficients bit for bit (pack_test), so the
renders may differ only by the renderer's run-to-run noise (equal-depth blending order). The test
also requires the palette render to differ clearly from SH 0, so a palette mode that silently drops
the view-dependent term cannot pass.

Self-contained apart from the scene (default data/raw/synthetic_sphere.sh1.ply, skips with exit
77 when absent, like golden_check.py): packs with splatconv, renders offscreen with the desktop
host, compares PSNR.

Usage:
  python scripts/palette_check.py --exe <pocketsplat-desktop> --splatconv <splatconv> \\
      --workdir <scratch dir> [--scene data/raw/synthetic_sphere.sh1.ply]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from quality_ab import psnr, render  # noqa: E402

POSES = [(34.4, 17.2, 1.0), (214.4, 17.2, 1.0), (124.4, 40.0, 0.7)]
SAME_DB = 60.0  # palette vs snapped: noise floor is ~90-105 dB; any decode error lands far below
SH0_MAX_DB = 50.0  # palette vs SH 0, at the pose where they differ most: the SH1 term is there


def pack(splatconv: str, scene: str, out: Path, extra: list[str]) -> dict:
    proc = subprocess.run([splatconv, "pack", scene, str(out), *extra], capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"FAIL: splatconv pack {out.name}: {proc.stderr.strip()}")
    return json.loads(proc.stdout.strip().splitlines()[-1])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--splatconv", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--scene", default="data/raw/synthetic_sphere.sh1.ply")
    parser.add_argument("--k", type=int, default=256)
    args = parser.parse_args()
    if not Path(args.scene).exists():
        print(f"SKIP: {args.scene} not present")
        return 77
    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    palette = ["--sh", "1", "--sh1-palette", str(args.k), "--palette-trainer", "emeans", "--palette-epochs", "3",
               "--palette-threads", "2"]
    p_info = pack(args.splatconv, args.scene, work / "palette.pspk", palette)
    s_info = pack(args.splatconv, args.scene, work / "snapped.pspk", palette + ["--palette-snap"])
    pack(args.splatconv, args.scene, work / "sh0.pspk", ["--sh", "0"])
    if p_info.get("palette_count") != min(args.k, p_info["count"]) or s_info.get("palette_count") != 0:
        print(f"FAIL: palette_count {p_info.get('palette_count')} / {s_info.get('palette_count')}")
        return 1
    if p_info["palette"]["mse"] != s_info["palette"]["mse"]:
        print("FAIL: the two packs were trained to different palettes")
        return 1

    exe = str(Path(args.exe).resolve())
    flags = ["--key-bits", "32", "--min-contribution", "0"]
    failed = False
    sh0_min = float("inf")
    for i, pose in enumerate(POSES):
        img_p = render(exe, str(work / "palette.pspk"), flags, 640, 360, pose, work / f"p{i}.ppm")
        img_s = render(exe, str(work / "snapped.pspk"), flags, 640, 360, pose, work / f"s{i}.ppm")
        img_0 = render(exe, str(work / "sh0.pspk"), flags, 640, 360, pose, work / f"z{i}.ppm")
        same, sh0 = psnr(img_p, img_s), psnr(img_p, img_0)
        sh0_min = min(sh0_min, sh0)
        failed |= same < SAME_DB
        print(f"pose {pose}: palette vs snapped {same:.2f} dB (>= {SAME_DB}) "
              f"{'ok' if same >= SAME_DB else 'FAIL'}; vs SH 0 {sh0:.2f} dB")
    if sh0_min >= SH0_MAX_DB:
        failed = True
        print(f"FAIL: palette within {sh0_min:.2f} dB of SH 0 at every pose (needs < {SH0_MAX_DB})")
    print("FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
