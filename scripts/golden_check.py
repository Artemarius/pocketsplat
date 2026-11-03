#!/usr/bin/env python3
"""Golden-image regression gate for the desktop host (CLAUDE.md §7).

Renders a scene with pinned camera/flags via pocketsplat-desktop, then compares the screenshot
against a stored reference PNG. Passes when PSNR >= --psnr-min (default 40 dB) — high enough to
catch real regressions, loose enough to absorb the renderer's benign nondeterminism (splats at
exactly equal float32 depth blend in unordered slots; measured run-to-run PSNR is ~90 dB on the
reference scenes, see docs/benchmarks/README.md) and small driver-version drift.

Modes:
  check   render and compare against the reference (default)
  record  render and (over)write the reference PNG — only after a reviewed visual change

Exit codes: 0 pass, 1 fail, 77 skip (scene or executable missing — machines without test data).

References live in desktop/tests/golden/ and are rendered at 800x450 with --no-hud. PNG IO is
stdlib-only (zlib); the screenshot format is the host's binary PPM.

Usage:
  python scripts/golden_check.py --exe build/desktop/Release/pocketsplat-desktop.exe \
      --scene data/derived/museum1.pspk --ref desktop/tests/golden/museum1.png [record|check]
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SKIP = 77


def read_ppm(path: Path):
    """Binary P6 PPM -> (width, height, flat RGB bytes)."""
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path} is not a binary PPM")
    fields: list[bytes] = []
    pos = 2
    while len(fields) < 3:  # width, height, maxval; comments start with '#'
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while end < len(data) and not data[end : end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1  # the single whitespace after maxval
    width, height, maxval = int(fields[0]), int(fields[1]), int(fields[2])
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval}")
    return width, height, data[pos : pos + 3 * width * height]


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    """Minimal 8-bit RGB PNG writer (no filtering beyond per-row None)."""
    raw = b"".join(b"\x00" + rgb[3 * width * y : 3 * width * (y + 1)] for y in range(height))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def read_png(path: Path):
    """Minimal reader for PNGs produced by write_png (8-bit RGB, filter 0 or 1 per row)."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"{path} is not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, color = struct.unpack_from(">IIBB", payload)
            if depth != 8 or color != 2:
                raise ValueError(f"{path}: expected 8-bit RGB, got depth {depth} color {color}")
        elif tag == b"IDAT":
            idat += payload
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = 3 * width
    out = bytearray(stride * height)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[(stride + 1) * y]
        row = bytearray(raw[(stride + 1) * y + 1 : (stride + 1) * (y + 1)])
        if f == 1:  # Sub
            for i in range(3, stride):
                row[i] = (row[i] + row[i - 3]) & 0xFF
        elif f == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif f != 0:
            raise ValueError(f"{path}: unsupported PNG filter {f}")
        out[stride * y : stride * (y + 1)] = row
        prev = row
    return width, height, bytes(out)


def psnr(a: bytes, b: bytes) -> float:
    import math

    import numpy as np

    x = np.frombuffer(a, dtype=np.uint8).astype(np.float64)
    y = np.frombuffer(b, dtype=np.uint8).astype(np.float64)
    mse = float(((x - y) ** 2).mean())
    if mse == 0.0:
        return float("inf")
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", default="check", choices=["check", "record"])
    parser.add_argument("--exe", required=True, help="pocketsplat-desktop executable")
    parser.add_argument("--scene", required=True, help="scene file (.pspk/.ply/.spz)")
    parser.add_argument("--ref", required=True, help="reference PNG path")
    parser.add_argument("--psnr-min", type=float, default=40.0)
    parser.add_argument("--width", type=int, default=800)
    parser.add_argument("--height", type=int, default=450)
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--yaw-deg", type=float, default=34.4)  # the defaults, pinned explicitly
    parser.add_argument("--pitch-deg", type=float, default=17.2)
    parser.add_argument("--extra", default="", help="extra host flags, space-separated")
    args = parser.parse_args()

    exe, scene, ref = Path(args.exe), REPO / args.scene, REPO / args.ref
    if not exe.exists() or not scene.exists():
        print(f"SKIP: missing {'executable' if not exe.exists() else 'scene'}")
        return SKIP

    with tempfile.TemporaryDirectory() as tmp:
        shot = Path(tmp) / "shot.ppm"
        cmd = [
            str(exe), str(scene), "--validation", "--no-hud",
            "--frames", str(args.frames), "--screenshot", str(shot),
            "--width", str(args.width), "--height", str(args.height),
            "--yaw-deg", str(args.yaw_deg), "--pitch-deg", str(args.pitch_deg),
        ] + ([a for a in args.extra.split(" ") if a])
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
        if result.returncode != 0 or not shot.exists():
            print(f"FAIL: host exited {result.returncode}\n{result.stdout}{result.stderr}")
            return 1
        width, height, rgb = read_ppm(shot)

    if args.mode == "record":
        ref.parent.mkdir(parents=True, exist_ok=True)
        write_png(ref, width, height, rgb)
        print(f"recorded {ref} ({width}x{height})")
        return 0

    if not ref.exists():
        print(f"SKIP: no reference at {ref} (run mode 'record' first)")
        return SKIP
    ref_w, ref_h, ref_rgb = read_png(ref)
    if (ref_w, ref_h) != (width, height):
        print(f"FAIL: size mismatch {width}x{height} vs reference {ref_w}x{ref_h}")
        return 1
    value = psnr(rgb, ref_rgb)
    verdict = "PASS" if value >= args.psnr_min else "FAIL"
    print(f"{verdict}: PSNR {value:.2f} dB (gate {args.psnr_min:.1f} dB) vs {ref.name}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
