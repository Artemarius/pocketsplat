#!/usr/bin/env python3
"""Image-quality and visible-count A/B for renderer variants on the desktop host.

For each scene, every variant (a set of pocketsplat-desktop flags) is rendered offscreen at fixed
camera poses and compared against the reference variant (PSNR over the full frame, plus the share
of pixels that moved by more than 2 LSB in any channel), and one bench orbit — the deterministic
600-frame orbit that the phone protocol also uses — records how many splats each variant leaves
for the sort. Device time is then predictable from the visible counts (docs/sorting.md §6 gives
the per-key sort cost on the phones); the desktop GPU times are reported for completeness only.

A variant name ending in "@repeat" re-renders the *same* flags as the named variant and reports
the run-to-run PSNR: the renderer's nondeterminism (splats with equal sort keys blend in atomic
slot order), which bounds how much any variant difference can be trusted.

`--scale name=0.85` renders a variant at that fraction of --width/--height (its bench orbit too)
and upscales it bilinearly to the full size before comparing - the internal-resolution rungs of
the thermal controller (ADR-0009), shown the way the compositor stretches the phone's surface.

Usage:
  python scripts/quality_ab.py --exe build/desktop/Release/pocketsplat-desktop.exe \\
      --scene data/derived/bench/stump-full.sh0.pspk --width 864 --height 1872 \\
      --variant ref="--key-bits 32" --variant c1="--min-contribution 1" \\
      --variant k16="--key-bits 16" --variant k16@repeat --reference ref \\
      --out bench-out/quality-ab --csv out.csv

Poses (yaw, pitch, distance scale) default to four yaws around the protocol start pose at the
protocol distance plus two closer views. Exit code 1 if any render fails. stdlib + numpy only.
"""
import argparse
import csv
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from golden_check import read_ppm, write_png  # noqa: E402

import numpy as np  # noqa: E402

DEFAULT_POSES = [(34.4, 17.2, 1.0), (124.4, 17.2, 1.0), (214.4, 17.2, 1.0), (304.4, 17.2, 1.0),
                 (34.4, 17.2, 0.6), (214.4, 10.0, 0.6)]


def run(cmd: list[str]) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({proc.returncode}):\n{proc.stdout}\n{proc.stderr}")
    return proc.stdout


OFFSCREEN = ["--offscreen"]  # cleared by --windowed (builds that predate the flag)


def render(exe: str, scene: str, flags: list[str], width: int, height: int, pose, out_ppm: Path):
    yaw, pitch, dist = pose
    run([exe, scene, *OFFSCREEN, "--no-hud", "--frames", "3", "--width", str(width), "--height",
         str(height), "--yaw-deg", str(yaw), "--pitch-deg", str(pitch), "--dist-scale", str(dist),
         "--screenshot", str(out_ppm)] + flags)
    w, h, rgb = read_ppm(out_ppm)
    return np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3)


def bench(exe: str, scene: str, flags: list[str], width: int, height: int, csv_path: Path) -> dict:
    run([exe, scene, *OFFSCREEN, "--bench", "--frames", "600", "--width", str(width), "--height",
         str(height), "--csv", str(csv_path)] + flags)
    rows = [r for r in csv.DictReader(line for line in csv_path.open() if not line.startswith("#"))]
    rows = rows[10:]  # warmup, as the host's own summary
    def mean(key):
        return sum(float(r[key]) for r in rows) / len(rows)
    return {"visible": mean("visible"), "gpu_ms": mean("gpu_ms"), "sort_ms": mean("sort_ms"),
            "draw_ms": mean("draw_ms"), "preprocess_ms": mean("preprocess_ms")}


def upscale(img: np.ndarray, width: int, height: int) -> np.ndarray:
    """Bilinear resize with pixel-center alignment (as a GPU or display scaler samples)."""
    h, w, _ = img.shape
    if (w, h) == (width, height):
        return img
    ys = np.clip((np.arange(height) + 0.5) * h / height - 0.5, 0, h - 1)
    xs = np.clip((np.arange(width) + 0.5) * w / width - 0.5, 0, w - 1)
    y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
    y1, x1 = np.minimum(y0 + 1, h - 1), np.minimum(x0 + 1, w - 1)
    fy, fx = (ys - y0)[:, None, None], (xs - x0)[None, :, None]
    src = img.astype(np.float64)
    top = src[y0][:, x0] * (1 - fx) + src[y0][:, x1] * fx
    bottom = src[y1][:, x0] * (1 - fx) + src[y1][:, x1] * fx
    return np.clip(np.rint(top * (1 - fy) + bottom * fy), 0, 255).astype(np.uint8)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean())
    return float("inf") if mse == 0.0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def moved_share(a: np.ndarray, b: np.ndarray) -> float:
    diff = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    return float((diff > 2).mean())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--scene", action="append", required=True)
    parser.add_argument("--variant", action="append", required=True,
                        help='name="flags" or name@repeat (repeat the flags of variant "name")')
    parser.add_argument("--reference", required=True, help="variant every other is compared against")
    parser.add_argument("--width", type=int, default=864)
    parser.add_argument("--height", type=int, default=1872)
    parser.add_argument("--no-bench", action="store_true", help="skip the orbit (images only)")
    parser.add_argument("--windowed", action="store_true",
                        help="render in the window, not offscreen (older builds; size capped by the monitor)")
    parser.add_argument("--exe-for", action="append", default=[],
                        help="name=path: render variant 'name' with another executable (e.g. a baseline build)")
    parser.add_argument("--scale", action="append", default=[],
                        help="name=s: render variant 'name' at s x the size, upscaled bilinearly to compare")
    parser.add_argument("--out", default="", help="directory for per-pose PNGs (optional)")
    parser.add_argument("--csv", default="", help="summary CSV path (optional)")
    args = parser.parse_args()
    args.exe = str(Path(args.exe).resolve())  # CreateProcess does not search relative paths
    if args.windowed:
        OFFSCREEN.clear()
    exe_for = {name: str(Path(path).resolve()) for name, _, path in (s.partition("=") for s in args.exe_for)}
    scale_for = {name: float(s) for name, _, s in (spec.partition("=") for spec in args.scale)}

    def size_of(name: str) -> tuple[int, int]:
        s = scale_for.get(name.split("@")[0], 1.0)
        return round(args.width * s), round(args.height * s)

    variants: list[tuple[str, list[str], str]] = []  # (name, flags, repeat_of)
    flags_by_name: dict[str, list[str]] = {}
    for spec in args.variant:
        if spec.endswith("@repeat"):
            base = spec[: -len("@repeat")]
            variants.append((spec, flags_by_name[base], base))
        else:
            name, _, flags = spec.partition("=")
            flags_by_name[name] = shlex.split(flags)
            variants.append((name, flags_by_name[name], ""))
    if args.reference not in flags_by_name:
        parser.error(f"reference variant {args.reference!r} not defined")
    out_dir = Path(args.out) if args.out else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    results = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for scene in args.scene:
            scene_name = Path(scene).name.split(".")[0]
            images: dict[str, list[np.ndarray]] = {}
            for name, flags, _ in variants:
                images[name] = []
                for p, pose in enumerate(DEFAULT_POSES):
                    exe = exe_for.get(name.split("@")[0], args.exe)
                    img = render(exe, scene, flags, *size_of(name), pose, tmp_dir / "shot.ppm")
                    img = upscale(img, args.width, args.height)
                    images[name].append(img)
                    if out_dir:
                        tag = name.replace("@", "-")
                        write_png(out_dir / f"{scene_name}-{tag}-pose{p}.png", img.shape[1], img.shape[0],
                                  img.tobytes())
            ref_bench = None
            for name, flags, repeat_of in variants:
                against = repeat_of or args.reference
                scores = [psnr(a, b) for a, b in zip(images[name], images[against])]
                moved = [moved_share(a, b) for a, b in zip(images[name], images[against])]
                rw, rh = size_of(name)
                row = {"scene": scene_name, "variant": name, "flags": " ".join(flags), "compared_to": against,
                       "resolution": f"{rw}x{rh}" + (f" -> {args.width}x{args.height}" if rw != args.width else ""),
                       "psnr_min_db": min(scores), "psnr_mean_db": sum(scores) / len(scores),
                       "moved_px_max_pct": 100.0 * max(moved)}
                if not args.no_bench and not repeat_of:
                    b = bench(exe_for.get(name, args.exe), scene, flags, *size_of(name), tmp_dir / "bench.csv")
                    row.update({f"orbit_{k}": v for k, v in b.items()})
                    if name == args.reference:
                        ref_bench = b
                results.append(row)
                print(f"{scene_name:>16} {name:>14} vs {against:<8} PSNR min {row['psnr_min_db']:6.2f} "
                      f"mean {row['psnr_mean_db']:6.2f} dB, moved <= {row['moved_px_max_pct']:5.2f} %"
                      + (f", visible {row['orbit_visible'] / 1e6:5.3f} M, gpu {row['orbit_gpu_ms']:.2f} ms"
                         f" (sort {row['orbit_sort_ms']:.2f})" if "orbit_visible" in row else ""),
                      flush=True)
            if ref_bench:
                for row in results:
                    if row["scene"] == scene_name and "orbit_visible" in row:
                        row["visible_vs_ref"] = row["orbit_visible"] / ref_bench["visible"]

    if args.csv:
        keys: list[str] = []
        for r in results:
            keys += [k for k in r if k not in keys]
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=keys)
            writer.writeheader()
            for r in results:
                writer.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in r.items()})
        print(f"summary csv written: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
