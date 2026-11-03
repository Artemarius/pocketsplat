#!/usr/bin/env python3
"""App icon from a handful of 2D Gaussians: one splat list, two outputs.

    python scripts/make_icon.py [--preview out.png]

writes
    desktop/resources/pocketsplat.ico                      Windows viewer (16-256 px, PNG entries)
    android/app/src/main/res/drawable/ic_launcher_fg.xml   adaptive-icon foreground (vector)
    android/app/src/main/res/drawable/ic_launcher_mono.xml themed-icon monochrome layer
    android/app/src/main/res/values/ic_launcher_bg.xml     adaptive-icon background colour
    android/app/src/main/res/mipmap-anydpi/ic_launcher.xml the adaptive icon (minSdk 33: no bitmaps)

Each splat is a 2D Gaussian (centre, sigmas, rotation, colour, peak opacity) composited back to
front with "over", the same model the renderer draws. The vector form is a unit circle with a
radial gradient sampling exp(-r^2/2) out to 3 sigma, scaled and rotated by a <group>. The icon
files are committed; re-run this script after changing the splat list. stdlib + numpy only.
"""
import argparse
import math
import struct
import zlib
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent

# Design space: the 108 x 108 adaptive-icon canvas (Android keeps the central 66 visible in every
# mask shape); the Windows icon crops to the central kCrop square and rounds its corners.
kCanvas = 108.0
kCrop = 76.0
kBackground = (0x12, 0x15, 0x1E)
# (cx, cy, sigma_major, sigma_minor, angle_deg, (r, g, b), peak opacity), back to front. Opacity is
# peak * kGain * gaussian, clamped to 1: saturated cores with soft edges, as high-opacity splats look.
SPLATS = [
    # A pinwheel: three splats 120 degrees apart, each pushed out along its major axis so the three
    # overlap in the centre; the visible edge (~2.5 sigma) stays inside the 33-unit safe radius.
    (54.0 + 10.0 * math.cos(math.radians(t)), 54.0 + 10.0 * math.sin(math.radians(t)), 9.0, 4.4, t, c, 1.0)
    for t, c in ((-90.0, (0x1F, 0xB8, 0xC8)), (30.0, (0xF2, 0xA6, 0x3B)), (150.0, (0xE0, 0x4F, 0x8A)))
]
kGain = 2.2
kCutoff = 3.0  # sigmas: splats end where the renderer's quads end


def rasterize(size, crop=kCrop, rounded=True):
    """RGBA float image of the icon at size x size px (Windows form: cropped, rounded corners)."""
    scale = size / crop
    offset = (kCanvas - crop) / 2.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    px = xs / scale + offset
    py = ys / scale + offset
    rgb = np.zeros((size, size, 3))
    rgb[:] = np.array(kBackground) / 255.0
    for cx, cy, s1, s2, angle, colour, peak in SPLATS:
        a = math.radians(angle)
        dx, dy = px - cx, py - cy
        u = (dx * math.cos(a) + dy * math.sin(a)) / s1
        v = (-dx * math.sin(a) + dy * math.cos(a)) / s2
        r2 = u * u + v * v
        alpha = np.minimum(1.0, peak * kGain * np.exp(-0.5 * r2)) * (r2 <= kCutoff * kCutoff)
        rgb = rgb * (1.0 - alpha[..., None]) + (np.array(colour) / 255.0) * alpha[..., None]
    coverage = np.ones((size, size))
    if rounded:
        radius = 0.22 * size
        qx = np.maximum(np.abs(xs - size / 2.0) - (size / 2.0 - radius), 0.0)
        qy = np.maximum(np.abs(ys - size / 2.0) - (size / 2.0 - radius), 0.0)
        dist = np.sqrt(qx * qx + qy * qy) - radius  # < 0 inside
        coverage = np.clip(0.5 - dist, 0.0, 1.0)
    return np.dstack([rgb, coverage])


def png_bytes(rgba):
    h, w = rgba.shape[:2]
    data = np.clip(np.round(rgba * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico_bytes(sizes):
    images = [png_bytes(rasterize(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, img in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    return header + entries + b"".join(images)


def gradient_stops(colour, peak, white=False):
    stops = []
    for i in range(13):
        t = i / 12.0
        alpha = min(1.0, peak * kGain * math.exp(-0.5 * (t * kCutoff) ** 2)) if i < 12 else 0.0
        r, g, b = (255, 255, 255) if white else colour
        stops.append(f'                        <item android:offset="{t:.3f}" '
                     f'android:color="#{round(alpha * 255):02X}{r:02X}{g:02X}{b:02X}" />')
    return "\n".join(stops)


def vector_xml(white=False):
    groups = []
    for cx, cy, s1, s2, angle, colour, peak in SPLATS:
        groups.append(f"""    <group android:translateX="{cx}" android:translateY="{cy}" android:rotation="{angle}"
        android:scaleX="{s1 * kCutoff:.2f}" android:scaleY="{s2 * kCutoff:.2f}">
        <path android:pathData="M-1,0a1,1 0 1,0 2,0a1,1 0 1,0 -2,0z">
            <aapt:attr name="android:fillColor">
                <gradient android:type="radial" android:centerX="0" android:centerY="0"
                    android:gradientRadius="1">
{gradient_stops(colour, peak, white)}
                </gradient>
            </aapt:attr>
        </path>
    </group>""")
    return ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<!-- Generated by scripts/make_icon.py from its splat list; edit there. -->\n"
            "<vector xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
            "    xmlns:aapt=\"http://schemas.android.com/aapt\"\n"
            f"    android:width=\"108dp\" android:height=\"108dp\"\n"
            f"    android:viewportWidth=\"{kCanvas:g}\" android:viewportHeight=\"{kCanvas:g}\">\n"
            + "\n".join(groups) + "\n</vector>\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preview", help="also write a PNG sheet: 512 px Windows form, the 108 dp canvas, "
                                          "and 16/32/48 px shown 4x")
    args = parser.parse_args()
    ico = REPO / "desktop/resources/pocketsplat.ico"
    ico.parent.mkdir(parents=True, exist_ok=True)
    ico.write_bytes(ico_bytes([16, 20, 24, 32, 40, 48, 64, 128, 256]))
    res = REPO / "android/app/src/main/res"
    for name, white in (("ic_launcher_fg.xml", False), ("ic_launcher_mono.xml", True)):
        (res / "drawable").mkdir(parents=True, exist_ok=True)
        (res / "drawable" / name).write_text(vector_xml(white), encoding="utf-8", newline="\n")
    (res / "values").mkdir(parents=True, exist_ok=True)
    r, g, b = kBackground
    (res / "values/ic_launcher_bg.xml").write_text(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- Generated by scripts/make_icon.py. -->\n"
        f"<resources>\n    <color name=\"ic_launcher_bg\">#{r:02X}{g:02X}{b:02X}</color>\n</resources>\n",
        encoding="utf-8", newline="\n")
    (res / "mipmap-anydpi").mkdir(parents=True, exist_ok=True)
    (res / "mipmap-anydpi/ic_launcher.xml").write_text(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- Generated by scripts/make_icon.py. -->\n"
        "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
        "    <background android:drawable=\"@color/ic_launcher_bg\" />\n"
        "    <foreground android:drawable=\"@drawable/ic_launcher_fg\" />\n"
        "    <monochrome android:drawable=\"@drawable/ic_launcher_mono\" />\n"
        "</adaptive-icon>\n", encoding="utf-8", newline="\n")
    print(f"wrote {ico.relative_to(REPO)} ({ico.stat().st_size} bytes) and the Android icon resources")
    if args.preview:
        tiles = [rasterize(512), rasterize(512, crop=kCanvas, rounded=False)]
        tiles += [np.kron(rasterize(s), np.ones((4, 4, 1))) for s in (16, 32, 48)]
        sheet = np.zeros((512, sum(t.shape[1] for t in tiles) + 16 * (len(tiles) - 1), 4))
        sheet[..., :3] = 0.85  # light page, so the dark icon's edge shows
        sheet[..., 3] = 1.0
        x = 0
        for t in tiles:
            h, w = t.shape[:2]
            a = t[..., 3:4]
            sheet[:h, x:x + w, :3] = t[..., :3] * a + sheet[:h, x:x + w, :3] * (1 - a)
            x += w + 16
        Path(args.preview).write_bytes(png_bytes(sheet))


if __name__ == "__main__":
    main()
