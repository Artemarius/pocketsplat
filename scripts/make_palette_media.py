#!/usr/bin/env python3
"""Side-by-side media of one scene's variants on the same camera path (docs/e-means-vs-lloyd.md).

    python scripts/make_palette_media.py --exe build/desktop/Release/pocketsplat-desktop.exe \
        --name beetle --reference "direct SH 1=bench-out/palette/beetle.direct.pspk" \
        --variant "SH 0=bench-out/palette/beetle.sh0.pspk" \
        --variant "Lloyd-25 palette=bench-out/palette/beetle.k4096.e25.s1.pspk" \
        --variant "e-means-3 palette=bench-out/palette/beetle.k4096.emeans.e3.s1.pspk" \
        --out-dir docs/media

The camera swings +-amplitude degrees about the reference pack's start view (the app's home view,
as scripts/make_turntable.py), and every variant is rendered offscreen at exactly the same poses
with the exact renderer settings of the palette quality runs (32-bit keys, no contribution cull).
Writes, for --name N:

    palette-N-swing.webp   animated 2 x 2 grid: the reference and the variants, labelled with each
                           variant's PSNR against the reference over the whole swing (min / mean)
    palette-N-diff.webp    animated row: |variant - reference| x gain per pixel, same poses
    palette-N-still.webp   lossless still at the start view: renders above, differences below

PSNR is over the full frame, 8-bit RGB, like scripts/quality_ab.py. Animations are lossy WebP
(quality --quality); the still is lossless. matplotlib + numpy for the labels and the compositing
(docs media script), ffmpeg on PATH for encoding.
"""
import argparse
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib
import numpy as np

matplotlib.use("agg")
import matplotlib.pyplot as plt  # noqa: E402

HOME_RE = re.compile(r"home view: focus (\S+) eye (\S+) up (\S+)")
RENDER_FLAGS = ["--key-bits", "32", "--min-contribution", "0"]
SURFACE, INK, INK2 = (26, 26, 25), "#f0efec", "#c3c2b7"  # plot_readme.py's dark theme


def vec(text):
    return [float(x) for x in text.split(",")]


def fmt(v):
    return ",".join(f"{x:.5f}" for x in v)


def rotate(v, axis, angle):  # Rodrigues
    c, s = math.cos(angle), math.sin(angle)
    d = sum(a * b for a, b in zip(axis, v))
    cross = [axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]]
    return [v[i] * c + cross[i] * s + axis[i] * d * (1 - c) for i in range(3)]


def render(exe, scene, size, extra, out):
    cmd = [str(exe), str(scene), "--offscreen", "--width", str(size[0]), "--height", str(size[1]), "--no-hud",
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


def label(text, sub, width, height=46):
    """A dark strip with a bold title and a smaller second line, as an RGB array."""
    fig = plt.figure(figsize=(width / 100, height / 100), dpi=100)
    fig.patch.set_facecolor(np.array(SURFACE) / 255)
    fig.text(0.02, 0.62, text, color=INK, fontsize=11, fontweight="bold", va="center",
             family=["Segoe UI", "DejaVu Sans"])
    fig.text(0.02, 0.22, sub, color=INK2, fontsize=9, va="center", family=["Segoe UI", "DejaVu Sans"])
    fig.canvas.draw()
    img = np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()
    plt.close(fig)
    return img


def panel(img, strip):
    return np.concatenate([strip, img], axis=0)


def grid(panels, cols, gap=4):
    h, w = panels[0].shape[:2]
    rows = math.ceil(len(panels) / cols)
    out = np.empty((rows * h + (rows - 1) * gap, cols * w + (cols - 1) * gap, 3), np.uint8)
    out[:] = SURFACE
    for k, p in enumerate(panels):
        r, c = divmod(k, cols)
        out[r * (h + gap):r * (h + gap) + h, c * (w + gap):c * (w + gap) + w] = p
    return out


def write_ppm(path, img):
    h, w = img.shape[:2]
    Path(path).write_bytes(f"P6\n{w} {h}\n255\n".encode() + img.tobytes())


def encode(frames_dir, pattern, out, quality, fps, lossless=False):
    codec = ["-c:v", "libwebp", "-loop", "0", "-compression_level", "6"]
    codec += ["-lossless", "1"] if lossless else ["-quality", str(quality)]
    src = ["-framerate", str(fps), "-i", str(frames_dir / pattern)]
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *src, *codec, str(out)], check=True)
    print(f"{out} ({out.stat().st_size / 1e6:.2f} MB)", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--reference", required=True, help="label=pack, the variant every other is compared to")
    ap.add_argument("--variant", action="append", required=True, help="label=pack (3 for the 2 x 2 grid)")
    ap.add_argument("--out-dir", type=Path, default=Path("docs/media"))
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--amplitude", type=float, default=25.0, help="swing, degrees each side")
    ap.add_argument("--size", default="400x300", help="render size per panel, WxH")
    ap.add_argument("--diff-size", default="320x240", help="panel size of the difference animation, WxH")
    ap.add_argument("--pull", type=float, default=1.0, help="eye distance x this (the start view is for portrait)")
    ap.add_argument("--gain", type=float, default=16.0, help="difference amplification")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--quality", type=int, default=75, help="WebP quality of the swing animation")
    ap.add_argument("--diff-quality", type=int, default=60, help="WebP quality of the difference animation")
    args = ap.parse_args()
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH")
    size = tuple(int(x) for x in args.size.lower().split("x"))
    exe = args.exe.resolve()
    entries = [tuple(s.split("=", 1)) for s in [args.reference] + args.variant]
    args.out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        match = HOME_RE.search(render(exe, Path(entries[0][1]).resolve(), size, ["--home-view"], tmp / "home.ppm"))
        if not match:
            sys.exit("no 'home view:' line in the host's output")
        focus, eye, up = (vec(g) for g in match.groups())
        offset = [args.pull * (e - f) for e, f in zip(eye, focus)]
        poses = []
        for i in range(args.frames):
            angle = math.radians(args.amplitude * math.sin(2 * math.pi * i / args.frames))
            poses.append(["--look-from", fmt([f + o for f, o in zip(focus, rotate(offset, up, angle))]),
                          "--look-at", fmt(focus), "--up", fmt(up)])

        shots = {}  # label -> list of frames
        for name, pack in entries:
            shots[name] = []
            for i, pose in enumerate(poses):
                out = tmp / "shot.ppm"
                render(exe, Path(pack).resolve(), size, pose, out)
                shots[name].append(read_ppm(out))
                print(f"\r{name}: frame {i + 1}/{len(poses)}", end="", flush=True)
            print()

        ref_name = entries[0][0]
        ref = shots[ref_name]
        stats = {}
        for name, _ in entries[1:]:
            values = [psnr(a, b) for a, b in zip(shots[name], ref)]
            stats[name] = (min(values), sum(values) / len(values), values[0])
            print(f"{name}: PSNR vs {ref_name} min {stats[name][0]:.1f} / mean {stats[name][1]:.1f} dB")

        w = size[0]
        strips = {ref_name: label(ref_name, "reference", w)}
        diff_strips = {}
        for name, _ in entries[1:]:
            lo, mean, _ = stats[name]
            strips[name] = label(name, f"{lo:.1f} dB min / {mean:.1f} dB mean vs {ref_name}", w)
            diff_strips[name] = label(f"{name}: difference x {args.gain:g}", f"black = identical to {ref_name}", w)

        def diff(name, i):
            d = np.abs(shots[name][i].astype(np.int16) - ref[i].astype(np.int16)) * args.gain
            return np.clip(d, 0, 255).astype(np.uint8)

        dw, dh = (int(x) for x in args.diff_size.lower().split("x"))

        def small(img):  # box-filter-free nearest resample keeps the noise honest (no smoothing)
            ys = (np.arange(dh) * img.shape[0] / dh).astype(int)
            xs = (np.arange(dw) * img.shape[1] / dw).astype(int)
            return img[ys][:, xs]

        small_strips = {n: label(f"{n}: difference x {args.gain:g}", f"black = identical to {ref_name}", dw)
                        for n, _ in entries[1:]}

        fdir = tmp / "frames"
        fdir.mkdir()
        for i in range(len(poses)):
            swing = grid([panel(shots[n][i], strips[n]) for n, _ in entries], cols=2)
            write_ppm(fdir / f"s{i:03d}.ppm", swing)
            row = grid([panel(small(diff(n, i)), small_strips[n]) for n, _ in entries[1:]], cols=len(entries) - 1)
            write_ppm(fdir / f"d{i:03d}.ppm", row)
        encode(fdir, "s%03d.ppm", args.out_dir / f"palette-{args.name}-swing.webp", args.quality, args.fps)
        encode(fdir, "d%03d.ppm", args.out_dir / f"palette-{args.name}-diff.webp", args.diff_quality, args.fps)

        # Still at the start view (frame 0 is the home pose): renders over differences, one column each.
        cols = [panel(shots[ref_name][0], strips[ref_name])]
        blank = np.empty_like(panel(diff(entries[1][0], 0), diff_strips[entries[1][0]]))
        blank[:] = SURFACE
        lower = [blank]
        for name, _ in entries[1:]:
            cols.append(panel(shots[name][0], label(name, f"{stats[name][2]:.1f} dB at this view", w)))
            lower.append(panel(diff(name, 0), diff_strips[name]))
        still = grid(cols + lower, cols=len(entries))
        write_ppm(fdir / "still.ppm", still)
        out = args.out_dir / f"palette-{args.name}-still.webp"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(fdir / "still.ppm"), "-c:v", "libwebp",
                        "-lossless", "1", str(out)], check=True)
        print(f"{out} ({out.stat().st_size / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
