#!/usr/bin/env python3
"""Scores coverage sidecars against a reference sidecar from the real capture cameras (ADR-0015).

For eyes orbiting the home view's focus (26 directions x 0.35 and 0.7 of the full distance, the
range interactive use spends its time in), each sidecar's fade (the preprocess shader's formula at
a given strength) is applied to the splat opacities, and a point depth map of the splats that stay
opaque (alpha >= 0.5) is compared with the unfaded one. A pixel is *revealed* where the front
opaque depth moved back by more than 15 %: the fade removed what was in front.

Per candidate, against the reference's revealed pixels:
  recall  share of the reference's reveals the candidate reproduces
  holes   candidate reveals the reference does not make, per reference reveal (0 = none,
          1 = as many wrong reveals as the cameras make right ones)

Usage:
  python scripts/coverage_eval.py --pack scene.pspk --focus x,y,z --up x,y,z --distance D \
      --reference cams.pscov --candidate room=room.pscov --candidate object=obj.pscov [--csv out.csv]
numpy only.
"""
import argparse
import csv
import struct
import sys

import numpy as np

GRID = 5


def read_pack(path):
    with open(path, "rb") as f:
        header = f.read(64)
        magic, version, count = struct.unpack_from("<III", header, 0)
        if magic != 0x4B505350:
            sys.exit(f"{path}: not a pack")
        pos = np.frombuffer(f.read(count * 12), dtype="<f4").reshape(count, 3).astype(np.float64)
        f.seek(count * 12, 1)  # scale_rot
        color = np.frombuffer(f.read(count * 4), dtype="<u4")
    return pos, (color >> 24).astype(np.float64) / 255.0


def read_sidecar(path, count):
    with open(path, "rb") as f:
        header = f.read(32)
        magic, version, n, grid = struct.unpack_from("<IIII", header, 0)
        if magic != 0x56435350 or n != count or grid != GRID:
            sys.exit(f"{path}: not a coverage file for this pack")
        return np.frombuffer(f.read(n * 4), dtype="<u4").astype(np.int64)


def cell_centres():
    out = []
    for c in range(GRID * GRID):
        x = (c % GRID + 0.5) / GRID * 2 - 1
        y = (c // GRID + 0.5) / GRID * 2 - 1
        z = 1 - abs(x) - abs(y)
        if z < 0:
            x, y = (1 - abs(y)) * (1 if x >= 0 else -1), (1 - abs(x)) * (1 if y >= 0 else -1)
        v = np.array([x, y, z])
        out.append(v / np.linalg.norm(v))
    return np.array(out)


CENTRES = cell_centres()
COS_FULL = np.cos(np.radians(35.0))


def match(mask, view_dir):
    """Best smoothstep(cos 90, cos 35, dot(v, centre)) over observed cells (the shader's coverage_match)."""
    t = np.clip((view_dir @ CENTRES.T) / COS_FULL, 0.0, 1.0)
    s = t * t * (3 - 2 * t)
    bits = (mask[:, None] >> np.arange(GRID * GRID)) & 1
    return (s * bits).max(1)


def eyes(focus, distance):
    out = []
    for x in (-1, 0, 1):
        for y in (-1, 0, 1):
            for z in (-1, 0, 1):
                if (x, y, z) == (0, 0, 0):
                    continue
                d = np.array([x, y, z], float) / np.linalg.norm([x, y, z])
                for k in (0.35, 0.7):
                    out.append(focus + d * k * distance)
    return out


def evaluate(pos, alpha, reference, candidates, focus, up, distance, strength, width=128, height=96):
    r = 1 - (1 - strength) ** 2
    totals = {name: np.zeros(3) for name in candidates}
    ref_reveal_share = []
    for eye in eyes(focus, distance):
        fwd = (focus - eye) / np.linalg.norm(focus - eye)
        hint = up if abs(fwd @ up) < 0.95 else np.array([1.0, 0.0, 0.0])
        right = np.cross(fwd, hint)
        right /= np.linalg.norm(right)
        down = np.cross(fwd, right)
        rel = pos - eye
        z = rel @ fwd
        f = 0.5 * height / np.tan(np.radians(30))
        zz = np.maximum(z, 1e-9)
        u = np.floor(rel @ right / zz * f + width / 2)
        v = np.floor(rel @ down / zz * f + height / 2)
        ok = (z > 1e-3 * distance) & (u >= 0) & (u < width) & (v >= 0) & (v < height)
        idx = np.nonzero(ok & (alpha >= 0.5))[0]  # only splats opaque before the fade can occlude
        pix = (v[idx] * width + u[idx]).astype(np.int64)
        zi = z[idx]
        vdir = eye - pos[idx]
        vdir /= np.linalg.norm(vdir, axis=1, keepdims=True)

        def front(mask):
            a = alpha[idx] if mask is None else alpha[idx] * (1 - r * (1 - match(mask[idx], vdir)))
            zb = np.full(width * height, np.inf)
            keep = a >= 0.5
            np.minimum.at(zb, pix[keep], zi[keep])
            return zb

        base = front(None)
        has = np.isfinite(base)
        ref = has & (front(reference) > base * 1.15)
        ref_reveal_share.append(ref.sum() / max(has.sum(), 1))
        for name, mask in candidates.items():
            cand = has & (front(mask) > base * 1.15)
            totals[name] += [ref.sum(), (ref & cand).sum(), (cand & ~ref).sum()]
    out = {}
    for name, (n_ref, both, extra) in totals.items():
        out[name] = (both / max(n_ref, 1), extra / max(n_ref, 1))
    return out, float(np.mean(ref_reveal_share))


def vec(text):
    return np.array([float(x) for x in text.split(",")])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--focus", required=True, type=vec)
    ap.add_argument("--up", required=True, type=vec)
    ap.add_argument("--distance", required=True, type=float, help="home view full distance")
    ap.add_argument("--reference", required=True, help="sidecar from the real capture cameras")
    ap.add_argument("--candidate", action="append", default=[], help="name=path.pscov")
    ap.add_argument("--strength", type=float, action="append", help="fade strength(s), default 0.5 and 1")
    ap.add_argument("--scene", default="")
    ap.add_argument("--csv", help="append rows to this CSV")
    args = ap.parse_args()
    pos, alpha = read_pack(args.pack)
    reference = read_sidecar(args.reference, len(pos))
    candidates = {}
    for c in args.candidate:
        name, path = c.split("=", 1)
        candidates[name] = read_sidecar(path, len(pos))
    rows = []
    for s in args.strength or [0.5, 1.0]:
        res, ref_share = evaluate(pos, alpha, reference, candidates, args.focus, args.up, args.distance, s)
        for name, (recall, holes) in res.items():
            rows.append({"scene": args.scene, "candidate": name, "strength": s, "reference_reveal_share":
                         round(ref_share, 4), "recall": round(recall, 4), "holes": round(holes, 4)})
            print(f"{args.scene:10s} s={s:.2f} ref reveals {100 * ref_share:5.1f} % of pixels | {name:12s} "
                  f"recall {recall:.2f} holes {holes:.2f}", flush=True)
    if args.csv:
        new = False
        try:
            open(args.csv).close()
        except FileNotFoundError:
            new = True
        with open(args.csv, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            if new:
                w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
