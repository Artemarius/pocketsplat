#!/usr/bin/env python3
"""Image-quality price of an SH 1 palette (Phase 8 gate A, docs/sh1-palette-research.md §4-5).

For each scene source (an SH 1 PLY/SPZ, already cleaned), builds with splatconv:
  direct  - the ordinary SH 1 pack (the reference),
  sh0     - the same splats at SH 0 (`pack --sh 0`),
  k<K>    - SH1 snapped to a trained K-entry palette (`pack --sh1-palette K`, Lloyd), still an
            ordinary SH 1 pack, so the unchanged renderer shows the palette's price,
then renders every pack with the desktop host at the quality_ab.py poses (offscreen, 32-bit keys,
no contribution cull) and compares each against `direct`: PSNR min/mean over the poses and the
share of pixels moved by more than 2 LSB. `direct@repeat` re-renders the reference: the
renderer's own run-to-run noise floor (equal-key blending order).

Packs and their splatconv JSON are kept in --work, so an interrupted run resumes without
retraining. Training wall time comes from splatconv's own timer (one process, --threads threads;
the CSV records the count). --low-priority runs this script and everything it starts at idle
priority (Windows: the priority class is inherited; elsewhere: nice 19), so a long sweep yields
to interactive work; wall times then also depend on what else runs.

Usage:
  python scripts/palette_ab.py --exe build/desktop/Release/pocketsplat-desktop.exe \\
      --splatconv build/tools/splatconv/Release/splatconv.exe \\
      --scene museum1=bench-out/palette/src/museum1.sh1.clean.ply \\
      --k 1024 4096 16384 65536 --epochs 25 --seed 1 \\
      --work bench-out/palette --csv docs/benchmarks/palette-quality-<hash>.csv

stdlib + numpy only.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from quality_ab import DEFAULT_POSES, moved_share, psnr, render  # noqa: E402

RENDER_FLAGS = ["--key-bits", "32", "--min-contribution", "0"]


def build_hash() -> str:
    repo = Path(__file__).resolve().parent.parent
    head = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"], capture_output=True,
                          text=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True, check=True).stdout.strip()
    return head + ("-dirty" if dirty else "")


def lower_priority() -> None:
    """Idle priority for this process and, by inheritance, every process it starts."""
    if sys.platform == "win32":
        import ctypes
        idle_priority_class = 0x40
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p  # a 64-bit pseudo-handle
        kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        if not kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), idle_priority_class):
            print("warning: could not lower the process priority", file=sys.stderr)
    else:
        os.nice(19)


def pack(splatconv: str, src: str, out: Path, extra: list[str]) -> dict:
    """Builds `out` unless it and its JSON already exist; returns splatconv's JSON line."""
    meta = out.with_suffix(".json")
    if out.exists() and meta.exists():
        return json.loads(meta.read_text())
    proc = subprocess.run([splatconv, "pack", src, str(out), *extra], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"splatconv pack {src} {' '.join(extra)} failed:\n{proc.stderr}")
    info = json.loads(proc.stdout.strip().splitlines()[-1])
    meta.write_text(json.dumps(info))
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="pocketsplat-desktop")
    parser.add_argument("--splatconv", required=True)
    parser.add_argument("--scene", action="append", required=True, help="name=path of an SH 1 PLY/SPZ")
    parser.add_argument("--k", type=int, nargs="+", default=[1024, 4096, 16384, 65536])
    parser.add_argument("--epochs", type=int, default=25)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--threads", type=int, default=0, help="palette training threads (0 = all)")
    parser.add_argument("--low-priority", action="store_true", help="run everything at idle priority")
    parser.add_argument("--width", type=int, default=864)
    parser.add_argument("--height", type=int, default=1872)
    parser.add_argument("--work", default="bench-out/palette", help="packs, JSON and optional PNGs")
    parser.add_argument("--png", action="store_true", help="also write per-pose PNGs to --work/png")
    parser.add_argument("--csv", required=True)
    args = parser.parse_args()
    if args.low_priority:
        lower_priority()
    exe = str(Path(args.exe).resolve())
    splatconv = str(Path(args.splatconv).resolve())
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    build = build_hash()

    fields = ["build", "scene", "count", "variant", "k", "epochs", "epochs_run", "seed", "threads", "train_wall_s", "mse",
              "dead", "used_entropy_bits", "pack_bytes", "psnr_min_db", "psnr_mean_db", "moved_px_max_pct",
              "resolution", "poses", "render_flags"]
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        shot = Path(tmp) / "shot.ppm"
        for spec in args.scene:
            name, _, src = spec.partition("=")
            variants: list[tuple[str, Path, dict]] = []
            info = pack(splatconv, src, work / f"{name}.direct.pspk", [])
            variants.append(("direct", work / f"{name}.direct.pspk", info))
            info = pack(splatconv, src, work / f"{name}.sh0.pspk", ["--sh", "0"])
            variants.append(("sh0", work / f"{name}.sh0.pspk", info))
            for k in args.k:
                out = work / f"{name}.k{k}.e{args.epochs}.s{args.seed}.pspk"
                info = pack(splatconv, src, out, ["--sh1-palette", str(k), "--palette-epochs", str(args.epochs),
                                                  "--palette-seed", str(args.seed),
                                                  "--palette-threads", str(args.threads)])
                variants.append((f"k{k}", out, info))
                print(f"{name:>16} k {k:>6}: trained in {info['palette']['wall_ms'] / 1000:7.1f} s, "
                      f"mse {info['palette']['mse']:.3g}, dead {info['palette']['dead']}", flush=True)

            def shots(path: Path, tag: str):
                images = []
                for p, pose in enumerate(DEFAULT_POSES):
                    img = render(exe, str(path), RENDER_FLAGS, args.width, args.height, pose, shot)
                    images.append(img)
                    if args.png:
                        from golden_check import write_png
                        (work / "png").mkdir(exist_ok=True)
                        write_png(work / "png" / f"{name}-{tag}-pose{p}.png", img.shape[1], img.shape[0],
                                  img.tobytes())
                return images

            reference = shots(variants[0][1], "direct")
            measured = [("direct@repeat", variants[0][1], variants[0][2], shots(variants[0][1], "repeat"))]
            measured += [(v, path, info, shots(path, v)) for v, path, info in variants[1:]]
            for variant, path, info, images in measured:
                scores = [psnr(a, b) for a, b in zip(images, reference)]
                moved = [moved_share(a, b) for a, b in zip(images, reference)]
                pal = info.get("palette", {})
                row = {"build": build, "scene": name, "count": info["count"], "variant": variant,
                       "k": pal.get("k", ""), "epochs": pal.get("epochs", ""), "epochs_run": pal.get("epochs_run", ""),
                       "seed": pal.get("seed", ""), "threads": pal.get("threads", ""),
                       "train_wall_s": f"{pal['wall_ms'] / 1000:.2f}" if pal else "",
                       "mse": f"{pal['mse']:.6g}" if pal else "", "dead": pal.get("dead", ""),
                       "used_entropy_bits": f"{pal['used_entropy_bits']:.4f}" if pal else "",
                       "pack_bytes": path.stat().st_size,
                       "psnr_min_db": f"{min(scores):.2f}", "psnr_mean_db": f"{sum(scores) / len(scores):.2f}",
                       "moved_px_max_pct": f"{100.0 * max(moved):.3f}",
                       "resolution": f"{args.width}x{args.height}", "poses": len(DEFAULT_POSES),
                       "render_flags": " ".join(RENDER_FLAGS)}
                rows.append(row)
                print(f"{name:>16} {variant:>14}: PSNR min {row['psnr_min_db']:>6} mean {row['psnr_mean_db']:>6} dB, "
                      f"moved <= {row['moved_px_max_pct']} %", flush=True)
            # Rewrite after every scene so a long run leaves usable partial results.
            with open(args.csv, "w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
    print(f"summary csv written: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
