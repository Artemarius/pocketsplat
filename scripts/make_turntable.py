#!/usr/bin/env python3
"""Turntable media for the README: the camera swings +-amplitude degrees about the scene's start view.

    python scripts/make_turntable.py --exe build/desktop/Release/pocketsplat-desktop.exe \
        --scene data/derived/hornedlizard.clean.spz --out docs/media/hornedlizard-turntable.webp

The start view (splatcore::estimate_home_view, the app's) is read from the desktop host's
`--home-view` output. The swing turns the eye about the home view's up (the fitted ground normal)
through the focus, amplitude * sin(2 pi i / frames), so the loop is seamless and stays near the
capture trajectory, where trained scenes have no holes. Each frame is one offscreen render
(`--look-from/--look-at/--up --screenshot`). ffmpeg (on PATH) encodes .webp (animated, the README
hero), .mp4 or .gif by the output's extension. Stdlib only.
"""
import argparse
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HOME_RE = re.compile(r"home view: focus (\S+) eye (\S+) up (\S+)")


def vec(text):
    return [float(x) for x in text.split(",")]


def fmt(v):
    return ",".join(f"{x:.5f}" for x in v)


def rotate(v, axis, angle):  # Rodrigues
    c, s = math.cos(angle), math.sin(angle)
    d = sum(a * b for a, b in zip(axis, v))
    cross = [axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]]
    return [v[i] * c + cross[i] * s + axis[i] * d * (1 - c) for i in range(3)]


def render(exe, scene, size, extra, screenshot):
    cmd = [str(exe), str(scene), "--offscreen", "--width", str(size[0]), "--height", str(size[1]), "--no-hud",
           "--frames", "2", "--screenshot", str(screenshot)] + extra
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"render failed ({result.returncode}): {result.stderr.strip()[-400:]}")
    return result.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True, type=Path, help="pocketsplat-desktop executable")
    ap.add_argument("--scene", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help=".webp, .mp4 or .gif")
    ap.add_argument("--frames", type=int, default=120, help="rendered frames per loop (30 fps source)")
    ap.add_argument("--amplitude", type=float, default=30.0, help="swing, degrees each side")
    ap.add_argument("--size", default="1280x720", help="render size WxH")
    ap.add_argument("--width", type=int, default=800, help="encoded width (webp/gif)")
    ap.add_argument("--fps", type=int, default=20, help="encoded frame rate (webp/gif)")
    args = ap.parse_args()
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH")
    size = tuple(int(x) for x in args.size.lower().split("x"))
    exe, scene = args.exe.resolve(), args.scene.resolve()

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        match = HOME_RE.search(render(exe, scene, size, ["--home-view"], tmp / "home.ppm"))
        if not match:
            sys.exit("no 'home view:' line in the host's output")
        focus, eye, up = (vec(g) for g in match.groups())
        offset = [e - f for e, f in zip(eye, focus)]
        for i in range(args.frames):
            angle = math.radians(args.amplitude * math.sin(2 * math.pi * i / args.frames))
            eye_i = [f + o for f, o in zip(focus, rotate(offset, up, angle))]
            render(exe, scene, size, ["--look-from", fmt(eye_i), "--look-at", fmt(focus), "--up", fmt(up)],
                   tmp / f"f{i:03d}.ppm")
            print(f"\rframe {i + 1}/{args.frames}", end="", flush=True)
        print()

        frames = ["-framerate", "30", "-i", str(tmp / "f%03d.ppm")]
        scale = f"fps={args.fps},scale={args.width}:-1:flags=lanczos"
        ext = args.out.suffix.lower()
        if ext == ".webp":
            codec = ["-vf", scale, "-c:v", "libwebp", "-quality", "70", "-compression_level", "6", "-loop", "0"]
        elif ext == ".mp4":
            codec = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart"]
        elif ext == ".gif":
            codec = ["-vf", scale + ",split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer",
                     "-loop", "0"]
        else:
            sys.exit(f"unsupported output type {ext}")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *frames, *codec, str(args.out)], check=True)
    print(f"{args.out} ({args.out.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
