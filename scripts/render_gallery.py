#!/usr/bin/env python3
"""Render every scene in data/ with pocketsplat-desktop and build an HTML gallery.

Each .ply/.spz/.pspk under data/raw and data/derived (minus scratch files) is rendered from two
angles (above/below - some captures are y-flipped relative to our RUB convention), converted to
PNG (stdlib zlib encoder, no dependencies) and listed in <out>/index.html.

Usage: python scripts/render_gallery.py [--out DIR] [--exe PATH] [--width W] [--height H]
       [--frames N] [--only NAME]
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SKIP_SUBSTRINGS = ("sortest", "nosor", "roundtrip")
SKIP_NAMES = ("rt.ply", "rt.spz")
ANGLES = [("above", 35.0, 25.0), ("below", 145.0, -25.0)]


def ppm_to_png(ppm_path: Path, png_path: Path) -> None:
    data = ppm_path.read_bytes()
    # P6 header: magic, dimensions, maxval, then binary RGB.
    parts = data.split(b"\n", 3)
    width, height = (int(v) for v in parts[1].split())
    pixels = parts[3]
    stride = 3 * width
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    png_path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                         chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def scene_files() -> list[Path]:
    files = []
    for sub in ("raw", "derived"):
        directory = REPO / "data" / sub
        if not directory.is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if path.suffix.lower() not in (".ply", ".spz", ".pspk"):
                continue
            if path.name in SKIP_NAMES or any(s in path.name for s in SKIP_SUBSTRINGS):
                continue
            files.append(path)
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=REPO / "bench-out" / "renders")
    parser.add_argument("--exe", type=Path,
                        default=REPO / "build" / "desktop" / "Release" / "pocketsplat-desktop.exe")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--frames", type=int, default=45)
    parser.add_argument("--only", help="render only files whose name contains this substring")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    rows = []
    failures = 0
    for path in scene_files():
        if args.only and args.only not in path.name:
            continue
        label = f"{path.parent.name}/{path.name}"
        images = []
        for angle_name, yaw, pitch in ANGLES:
            stem = f"{path.parent.name}.{path.stem}.{angle_name}"
            ppm = args.out / (stem + ".ppm")
            png = args.out / (stem + ".png")
            cmd = [str(args.exe), str(path), "--frames", str(args.frames),
                   "--width", str(args.width), "--height", str(args.height),
                   "--yaw-deg", str(yaw), "--pitch-deg", str(pitch), "--screenshot", str(ppm)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0 or not ppm.exists():
                print(f"FAILED {label} ({angle_name}): {result.stderr.strip()[:200]}", file=sys.stderr)
                failures += 1
                continue
            ppm_to_png(ppm, png)
            ppm.unlink()
            images.append(png.name)
            print(f"rendered {label} ({angle_name})")
        if images:
            size_mb = path.stat().st_size / 1e6
            rows.append((label, size_mb, images))

    html = ["<!doctype html><meta charset='utf-8'><title>PocketSplat renders</title>",
            "<style>body{font-family:sans-serif;background:#111;color:#ddd;margin:20px}"
            "img{width:46vw;margin:4px;border:1px solid #333}h2{margin:24px 0 4px}</style>",
            "<h1>PocketSplat scene renders</h1>"]
    for label, size_mb, images in rows:
        html.append(f"<h2>{label} <small>({size_mb:.1f} MB)</small></h2>")
        for image in images:
            html.append(f"<a href='{image}'><img src='{image}' loading='lazy'></a>")
    (args.out / "index.html").write_text("\n".join(html), encoding="utf-8")
    print(f"\n{len(rows)} scenes -> {args.out / 'index.html'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
