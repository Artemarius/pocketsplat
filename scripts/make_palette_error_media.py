#!/usr/bin/env python3
"""Error-map figures for palette write-ups (docs/palette-size.md): where a palette's colour error lands.

For one scene and one camera, renders the direct SH 1 pack (the reference) and each --variant pack
with the desktop host (offscreen, the palette quality runs' settings: 32-bit keys, no contribution
cull), and writes one WebP (lossy, quality 92, unless --quality 0 asks for lossless):

  top     the reference render, then per variant the reference in dimmed grey with the per-pixel
          error |variant - reference| (mean over RGB, 8-bit levels) laid over it in a perceptual
          colour map, one shared scale for every panel; each panel is labelled with the variant's
          PSNR against the reference at this view;
  crops   two windows chosen automatically, with no hand-picking: where the first variant's error
          most exceeds the second's, and the reverse. Each crop row shows the reference pixels and
          every variant's error map, enlarged with nearest-neighbour resampling (each screen pixel
          becomes a block, nothing is smoothed);
  scale   the colour bar in 8-bit levels.

Camera: --home (the app's start view for the portrait phone frame, ADR-0014) with --swing degrees
about the up axis and --pull x the eye distance, as scripts/palette_home_ab.py; or --pose
yaw,pitch,dist as scripts/quality_ab.py (the gate A poses orbit the scene's framing from outside).

    python scripts/make_palette_error_media.py --exe build/desktop/Release/pocketsplat-desktop.exe \\
        --prefix palette-size --name hornedlizard-home --reference bench-out/palette-k/hornedlizard.direct.pspk \\
        --home --variant "k 4,096=bench-out/palette-k/hornedlizard.k4096.emeans.e3.s1.pspk" \\
        --variant "k 16,384=bench-out/palette-k/hornedlizard.k16384.emeans.e3.s1.pspk"

matplotlib + numpy (docs media script), ffmpeg on PATH for the WebP.
"""
import argparse
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib
import numpy as np

matplotlib.use("agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_palette_media import HOME_RE, fmt, read_ppm, render, rotate, vec, write_ppm  # noqa: E402

SURFACE, INK, INK2 = (26, 26, 25), "#f0efec", "#c3c2b7"  # make_palette_media.py's dark strip
CMAP = plt.colormaps["inferno"]


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return math.inf if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def text_strip(lines, width, height):
    """Dark strip with a bold first line and smaller further lines, as an RGB array."""
    fig = plt.figure(figsize=(width / 100, height / 100), dpi=100)
    fig.patch.set_facecolor(np.array(SURFACE) / 255)
    step = 1.0 / (len(lines) + 0.4)
    for i, line in enumerate(lines):
        fig.text(0.02 * 600 / max(width, 300), 1 - step * (i + 0.75), line, color=INK if i == 0 else INK2,
                 fontsize=13 if i == 0 else 11, fontweight="bold" if i == 0 else "normal", va="center",
                 family=["Segoe UI", "DejaVu Sans"])
    fig.canvas.draw()
    img = np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()
    plt.close(fig)
    return img


def colour_bar(width, height, vmax):
    fig = plt.figure(figsize=(width / 100, height / 100), dpi=100)
    fig.patch.set_facecolor(np.array(SURFACE) / 255)
    ax = fig.add_axes([0.25, 0.45, 0.5, 0.22])
    ax.imshow(np.linspace(0, 1, 256)[None, :], aspect="auto", cmap=CMAP, extent=(0, vmax, 0, 1))
    ax.set_yticks([])
    ticks = list(range(0, int(vmax) + 1, max(1, int(vmax) // 6)))
    ax.set_xticks(ticks, [f"{t}" for t in ticks[:-1]] + [f"≥ {ticks[-1]}"])
    ax.tick_params(colors=INK2, labelsize=10, length=3)
    for s in ax.spines.values():
        s.set_visible(False)
    fig.text(0.5, 0.86, "error per pixel: |palette − direct SH 1|, mean over R, G, B, in 8-bit levels",
             color=INK2, fontsize=10.5, ha="center", va="center", family=["Segoe UI", "DejaVu Sans"])
    fig.canvas.draw()
    img = np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()
    plt.close(fig)
    return img


def error_map(ref, img, vmax):
    """The reference in dimmed grey with the error over it: colour and opacity both follow the error."""
    err = np.abs(img.astype(np.float64) - ref.astype(np.float64)).mean(2)
    v = np.clip(err / vmax, 0, 1)
    heat = CMAP(v)[:, :, :3] * 255
    grey = (ref.astype(np.float64).mean(2, keepdims=True) * 0.30).repeat(3, 2)
    a = (v ** 0.5)[..., None]
    return (grey * (1 - a) + heat * a).astype(np.uint8), err


def box_sum(a, h, w):
    """Sum of a over every h x w window (top-left anchored), via a summed-area table."""
    s = np.pad(a, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    return s[h:, w:] - s[:-h, w:] - s[h:, :-w] + s[:-h, :-w]


def best_window(score, h, w):
    sums = box_sum(score, h, w)
    y, x = np.unravel_index(np.argmax(sums), sums.shape)
    return int(y), int(x)


def outline(img, y, x, h, w, colour, t=3):
    out = img.copy()
    out[y:y + t, x:x + w] = colour
    out[y + h - t:y + h, x:x + w] = colour
    out[y:y + h, x:x + t] = colour
    out[y:y + h, x + w - t:x + w] = colour
    return out


def enlarge(img, factor):
    return img.repeat(factor, 0).repeat(factor, 1)


def resize_nearest(img, width):
    h, w = img.shape[:2]
    height = round(h * width / w)
    ys = (np.arange(height) * h / height).astype(int)
    xs = (np.arange(width) * w / width).astype(int)
    return img[ys][:, xs]


def hstack(panels, gap=6):
    h = max(p.shape[0] for p in panels)
    out = []
    for i, p in enumerate(panels):
        if p.shape[0] < h:
            pad = np.empty((h - p.shape[0],) + p.shape[1:], np.uint8)
            pad[:] = SURFACE
            p = np.concatenate([p, pad], 0)
        out.append(p)
        if i + 1 < len(panels):
            g = np.empty((h, gap, 3), np.uint8)
            g[:] = SURFACE
            out.append(g)
    return np.concatenate(out, 1)


def vstack(parts, width, gap=10):
    out = []
    for i, p in enumerate(parts):
        if p.shape[1] < width:
            pad = np.empty((p.shape[0], width - p.shape[1], 3), np.uint8)
            pad[:] = SURFACE
            p = np.concatenate([p, pad], 1)
        out.append(p)
        if i + 1 < len(parts):
            g = np.empty((gap, width, 3), np.uint8)
            g[:] = SURFACE
            out.append(g)
    return np.concatenate(out, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, type=Path)
    ap.add_argument("--name", required=True, help="output: <out-dir>/<prefix>-<name>.webp")
    ap.add_argument("--prefix", default="palette", help="output file name prefix")
    ap.add_argument("--reference", required=True, type=Path, help="the direct SH 1 pack")
    ap.add_argument("--variant", action="append", required=True, help="label=pack; the first two choose the crops")
    ap.add_argument("--title", default="", help="caption line above the figure")
    ap.add_argument("--home", action="store_true", help="camera from the app's home view")
    ap.add_argument("--swing", type=float, default=0.0)
    ap.add_argument("--pull", type=float, default=1.0)
    ap.add_argument("--pose", help="yaw,pitch,dist as quality_ab.py (instead of --home)")
    ap.add_argument("--size", default="864x1872", help="render size WxH (the phone portrait frame)")
    ap.add_argument("--panel-width", type=int, default=560, help="overview panel width in the figure")
    ap.add_argument("--crop", default="", help="crop window WxH in render pixels (default: fills the width)")
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--raw-crops", action="store_true", help="also show each variant's rendered pixels in the crops")
    ap.add_argument("--vmax", type=float, default=6.0, help="error at the top of the colour scale, 8-bit levels")
    ap.add_argument("--quality", type=int, default=92, help="lossy WebP quality; 0 = lossless")
    ap.add_argument("--out-dir", type=Path, default=Path("docs/media"))
    args = ap.parse_args()
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH")
    size = tuple(int(x) for x in args.size.lower().split("x"))
    if args.crop:
        cw, ch = (int(x) for x in args.crop.lower().split("x"))
    else:  # crop cells as wide as the overview panels, 4:3
        cw = args.panel_width // args.zoom
        ch = cw * 3 // 4
    exe = args.exe.resolve()
    variants = [tuple(s.split("=", 1)) for s in args.variant]

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        if args.home:
            match = HOME_RE.search(render(exe, args.reference.resolve(), size, ["--home-view"], tmp / "h.ppm"))
            if not match:
                sys.exit("no 'home view:' line in the host's output")
            focus, eye, up = (vec(g) for g in match.groups())
            offset = rotate([args.pull * (e - f) for e, f in zip(eye, focus)], up, math.radians(args.swing))
            cam = ["--look-from", fmt([f + o for f, o in zip(focus, offset)]), "--look-at", fmt(focus),
                   "--up", fmt(up)]
            where = f"home view, swing {args.swing:g}°, distance × {args.pull:g}"
        else:
            yaw, pitch, dist = (float(x) for x in args.pose.split(","))
            cam = ["--yaw-deg", str(yaw), "--pitch-deg", str(pitch), "--dist-scale", str(dist)]
            where = f"orbit pose yaw {yaw:g}°, pitch {pitch:g}°, distance × {dist:g}"
        render(exe, args.reference.resolve(), size, cam, tmp / "ref.ppm")
        ref = read_ppm(tmp / "ref.ppm")
        shots = []
        for label, pack in variants:
            render(exe, Path(pack).resolve(), size, cam, tmp / "v.ppm")
            shots.append((label, read_ppm(tmp / "v.ppm")))

    maps = [error_map(ref, img, args.vmax) for _, img in shots]
    scores = [psnr(img, ref) for _, img in shots]
    for (label, _), s in zip(shots, scores):
        print(f"{args.name}: {label} {s:.2f} dB vs direct SH 1 ({where})")
    # Crops: where variant 0's error most exceeds variant 1's (variant 1 "helps"), and the reverse.
    e0, e1 = maps[0][1], maps[1][1]
    crops = [("helps", best_window(e0 - e1, ch, cw)), ("hurts", best_window(e1 - e0, ch, cw))]
    box_colours = {"helps": (80, 200, 255), "hurts": (255, 255, 255)}

    pw = args.panel_width
    top_ref = ref.copy()
    for tag, (y, x) in crops:
        top_ref = outline(top_ref, y, x, ch, cw, box_colours[tag], t=max(3, ref.shape[1] // 250))
    panels = [vstack([text_strip(["direct SH 1", "reference, no palette"], pw, 62), resize_nearest(top_ref, pw)], pw, 0)]
    for (label, _), (m, _), s in zip(shots, maps, scores):
        panels.append(vstack([text_strip([label, f"{s:.2f} dB at this view"], pw, 62), resize_nearest(m, pw)], pw, 0))
    top = hstack(panels)
    width = top.shape[1]

    parts = []
    if args.title:
        parts.append(text_strip([args.title, where], width, 70))
    parts.append(top)
    cell = cw * args.zoom
    for tag, (y, x) in crops:
        first, second = variants[0][0], variants[1][0]
        caption = (f"blue box: the window where '{second}' gains most over '{first}' (chosen automatically)"
                   if tag == "helps" else
                   f"white box: the window where '{second}' loses most to '{first}' (chosen automatically)")
        cells = [enlarge(ref[y:y + ch, x:x + cw], args.zoom)]
        cells += [enlarge(m[y:y + ch, x:x + cw], args.zoom) for m, _ in maps]
        local = [psnr(img[y:y + ch, x:x + cw], ref[y:y + ch, x:x + cw]) for _, img in shots]
        heads = [text_strip(["direct SH 1", f"{cw} × {ch} px, × {args.zoom}"], cell, 56)]
        heads += [text_strip([label, f"{v:.1f} dB in this box"], cell, 56) for (label, _), v in zip(shots, local)]
        parts.append(text_strip([caption], width, 40))
        parts.append(hstack([vstack([h, c], cell, 0) for h, c in zip(heads, cells)]))
        if args.raw_crops:
            blank = np.empty((56 + ch * args.zoom, cell, 3), np.uint8)
            blank[:] = SURFACE
            raw = [blank] + [vstack([text_strip([label, "rendered pixels"], cell, 56),
                                     enlarge(img[y:y + ch, x:x + cw], args.zoom)], cell, 0) for label, img in shots]
            parts.append(hstack(raw))
    parts.append(colour_bar(width, 90, args.vmax))
    figure = vstack(parts, width)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        ppm = Path(tmp) / "f.ppm"
        write_ppm(ppm, figure)
        out = args.out_dir / f"{args.prefix}-{args.name}.webp"
        codec = ["-lossless", "1"] if args.quality == 0 else ["-quality", str(args.quality)]
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(ppm), "-c:v", "libwebp", *codec,
                        "-compression_level", "6", str(out)], check=True)
        print(f"{out} ({out.stat().st_size / 1e6:.2f} MB, {figure.shape[1]} x {figure.shape[0]})")


if __name__ == "__main__":
    main()
