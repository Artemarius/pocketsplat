#!/usr/bin/env python3
"""Image price of smaller position and scale arrays, simulated (Phase 9, ADR-0018 option (e)).

ADR-0018's option (e) shrinks the two 12-byte per-splat arrays to 8 bytes: positions to
16 bits per axis inside each 256-splat chunk's tight position bounds (plus a 16-bit pad that would
carry the palette index), log-scales to 8 bits over the scene's log-scale range, and the quaternion
to smallest-three in 32 bits. This script measures what positions and scales lose without writing
a new format: it quantizes and dequantizes them, writes the result as an ordinary v3 pack, and
renders that with the desktop host against the untouched pack.

* Positions: q = round((p - lo) / (hi - lo) * 65535) per axis, lo/hi = the chunk's min/max centre,
  dequantized in float64 and stored as float32 (the shader would compute lo + q * step in fp32).
* Log-scales: 256 levels over [min, max] of all log-scales in the scene, dequantized and stored
  back as fp16 (fp16's own step, <= 0.004 below |log s| = 8, is far under the 8-bit step).
* Rotation is left as it is. Smallest-three at 10 bits per component (max error 0.707 / 1023) is
  finer than today's 4 x snorm8, which a v3 pack cannot hold, so the simulation would only add
  snorm8 error twice: the measured loss is that of positions and scales alone.
* Chunk cull bounds (centre +- 3 x largest scale) and importance_max are recomputed from the
  new values, so the frustum cull stays conservative; the header AABB likewise.

Per variant and pose: PSNR and the share of pixels moved by more than 2 LSB against the
reference pack (quality_ab.py's measures), at the quality_ab.py poses plus two closer ones
(off with `--no-close`); `ref@repeat` re-renders the reference for the renderer's own noise floor. Per scene
also the size of the position step against the splats it moves, in standard deviations of the
splat's largest axis (step / 2 = the largest per-axis error).

Usage:
  python scripts/quantize_sim.py --exe build/desktop/Release/pocketsplat-desktop.exe \\
      --scene museum1=bench-out/palette/museum1.direct.pspk --work bench-out/quantize-sim \\
      --csv docs/benchmarks/quantize-sim-<hash>.csv --stats-csv docs/benchmarks/quantize-steps-<hash>.csv

stdlib + numpy only.
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from palette_ab import build_hash, lower_priority  # noqa: E402
from quality_ab import DEFAULT_POSES, moved_share, psnr, render  # noqa: E402

import numpy as np  # noqa: E402

RENDER_FLAGS = ["--key-bits", "32", "--min-contribution", "0"]
CLOSE_POSES = [(34.4, 17.2, 0.35), (214.4, 10.0, 0.35)]
HEADER_BYTES = 64
CHUNK = 256
CHUNK_FLOATS = 7


def read_pack(path: Path) -> dict:
    raw = path.read_bytes()
    header = bytearray(raw[:HEADER_BYTES])
    magic, version, count, chunk_size, sh_degree = struct.unpack_from("<4sIIII", header, 0)
    if magic != b"PSPK" or chunk_size != CHUNK:
        raise ValueError(f"{path}: not a pack with 256-splat chunks")
    word28 = struct.unpack_from("<I", header, 28)[0]
    palette_count = word28 & 0xFFFFFF
    chunks = (count + CHUNK - 1) // CHUNK
    sh1_words = 0 if sh_degree == 0 else 3 * (palette_count if palette_count else count)
    off = HEADER_BYTES
    def take(dtype, n):
        nonlocal off
        a = np.frombuffer(raw, dtype=dtype, count=n, offset=off).copy()
        off += a.nbytes
        return a
    pack = {"header": header, "count": count, "version": version,
            "pos": take("<f4", 3 * count).reshape(count, 3),
            "scale_rot": take("<u4", 3 * count).reshape(count, 3),
            "color": take("<u4", count),
            "sh1": take("<u4", sh1_words),
            "chunks": take("<f4", CHUNK_FLOATS * chunks).reshape(chunks, CHUNK_FLOATS)}
    if off != len(raw):
        raise ValueError(f"{path}: {len(raw) - off} trailing bytes")
    return pack


def write_pack(pack: dict, path: Path) -> None:
    with path.open("wb") as f:
        f.write(bytes(pack["header"]))
        for key in ("pos", "scale_rot", "color", "sh1", "chunks"):
            f.write(np.ascontiguousarray(pack[key]).tobytes())


def log_scales(scale_rot: np.ndarray) -> np.ndarray:
    halves = np.stack([scale_rot[:, 0] & 0xFFFF, scale_rot[:, 0] >> 16, scale_rot[:, 1] & 0xFFFF], axis=1)
    return halves.astype(np.uint16).view(np.float16).astype(np.float32)


def set_log_scales(scale_rot: np.ndarray, ls: np.ndarray) -> np.ndarray:
    h = ls.astype(np.float16).view(np.uint16).astype(np.uint32)
    out = scale_rot.copy()
    out[:, 0] = h[:, 0] | (h[:, 1] << 16)
    out[:, 1] = (scale_rot[:, 1] & 0xFFFF0000) | h[:, 2]
    return out


def chunk_view(a: np.ndarray, count: int) -> np.ndarray:
    """(chunks, 256, ...) view, the tail chunk padded by repeating its last row."""
    chunks = (count + CHUNK - 1) // CHUNK
    pad = chunks * CHUNK - count
    if pad:
        a = np.concatenate([a, np.repeat(a[-1:], pad, axis=0)])
    return a.reshape(chunks, CHUNK, *a.shape[1:])


def position_frames(pos: np.ndarray, count: int):
    """Per-chunk tight centre bounds (lo, hi) broadcast to splats, and the per-splat step."""
    c = chunk_view(pos.astype(np.float64), count)
    lo = c.min(axis=1)
    hi = c.max(axis=1)
    lo_s = np.repeat(lo, CHUNK, axis=0)[:count]
    hi_s = np.repeat(hi, CHUNK, axis=0)[:count]
    return lo_s, hi_s, (hi_s - lo_s) / 65535.0


def quantize_positions(pos: np.ndarray, count: int) -> np.ndarray:
    lo, hi, step = position_frames(pos, count)
    safe = np.where(step > 0, step, 1.0)
    q = np.clip(np.rint((pos.astype(np.float64) - lo) / safe), 0, 65535)
    return np.where(step > 0, lo + q * step, pos.astype(np.float64)).astype(np.float32)


def quantize_log_scales(ls: np.ndarray):
    lo, hi = float(ls.min()), float(ls.max())
    step = (hi - lo) / 255.0
    q = np.clip(np.rint((ls.astype(np.float64) - lo) / step), 0, 255)
    return (lo + q * step).astype(np.float32), lo, hi, step


def rebuild_bounds(pack: dict) -> None:
    """Chunk cull AABBs and importance_max as splatcore::build_pack, from the pack's own values."""
    count = pack["count"]
    pos = pack["pos"]
    ls = log_scales(pack["scale_rot"])
    r = 3.0 * np.exp(ls.max(axis=1))
    alpha = ((pack["color"] >> 24) & 0xFF).astype(np.float32) / 255.0
    s = np.exp(ls)
    imp = alpha * (s[:, 0] * s[:, 1] + s[:, 1] * s[:, 2] + s[:, 2] * s[:, 0])
    mn = chunk_view(pos - r[:, None], count).min(axis=1)
    mx = chunk_view(pos + r[:, None], count).max(axis=1)
    imp_max = chunk_view(imp, count).max(axis=1)
    pack["chunks"] = np.concatenate([mn, mx, imp_max[:, None]], axis=1).astype(np.float32)
    struct.pack_into("<3f", pack["header"], 32, *pos.min(axis=0).tolist())
    struct.pack_into("<3f", pack["header"], 44, *pos.max(axis=0).tolist())


def make_variant(src: dict, name: str) -> dict:
    pack = {k: (v.copy() if isinstance(v, np.ndarray) else v) for k, v in src.items()}
    pack["header"] = bytearray(src["header"])
    if "pos16" in name:
        pack["pos"] = quantize_positions(src["pos"], src["count"])
    if "scale8" in name:
        q, _, _, _ = quantize_log_scales(log_scales(src["scale_rot"]))
        pack["scale_rot"] = set_log_scales(src["scale_rot"], q)
    rebuild_bounds(pack)
    return pack


def step_stats(scene: str, pack: dict) -> dict:
    count = pack["count"]
    pos = pack["pos"]
    ls = log_scales(pack["scale_rot"])
    lo, hi, step = position_frames(pos, count)
    sigma = np.exp(ls.max(axis=1)).astype(np.float64)
    err_sigma = (step.max(axis=1) / 2.0) / sigma  # largest per-axis error, in the splat's largest sigma
    extent = (hi - lo).max(axis=1)[::CHUNK]
    diag = float(np.linalg.norm(pos.max(axis=0) - pos.min(axis=0)))
    _, s_lo, s_hi, s_step = quantize_log_scales(ls)
    pct = lambda a, p: float(np.percentile(a, p))
    return {
        "scene": scene, "splats": count,
        "chunk_extent_p50_of_diag": pct(extent, 50) / diag,
        "chunk_extent_p99_of_diag": pct(extent, 99) / diag,
        "chunk_extent_max_of_diag": float(extent.max()) / diag,
        "pos_err_sigma_p50": pct(err_sigma, 50), "pos_err_sigma_p99": pct(err_sigma, 99),
        "pos_err_sigma_p999": pct(err_sigma, 99.9), "pos_err_sigma_max": float(err_sigma.max()),
        "share_pos_err_over_0.1_sigma": float((err_sigma > 0.1).mean()),
        "log_scale_min": s_lo, "log_scale_max": s_hi, "log_scale_step": s_step,
        "scale_err_max_pct": (np.exp(s_step / 2.0) - 1.0) * 100.0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--scene", action="append", required=True, help="name=path/to/pack.pspk")
    ap.add_argument("--variants", nargs="+", default=["pos16", "scale8", "pos16+scale8"])
    ap.add_argument("--width", type=int, default=864)
    ap.add_argument("--height", type=int, default=1872)
    ap.add_argument("--no-close", action="store_true", help="only the quality_ab.py poses")
    ap.add_argument("--work", default="bench-out/quantize-sim")
    ap.add_argument("--csv", required=True)
    ap.add_argument("--stats-csv", required=True)
    ap.add_argument("--low-priority", action="store_true")
    args = ap.parse_args()
    args.exe = str(Path(args.exe).resolve())  # CreateProcess does not search relative forward-slash paths
    if args.low_priority:
        lower_priority()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    poses = DEFAULT_POSES + ([] if args.no_close else CLOSE_POSES)
    build = build_hash()

    rows, stats = [], []
    for spec in args.scene:
        scene, path = spec.split("=", 1)
        src = read_pack(Path(path))
        stats.append(step_stats(scene, src))
        print(f"[{scene}] {stats[-1]}", flush=True)
        packs = {"ref": Path(path)}
        for v in args.variants:
            out = work / f"{scene}.{v.replace('+', '_')}.pspk"
            if not out.exists():
                write_pack(make_variant(src, v), out)
            packs[v] = out
        for pi, pose in enumerate(poses):
            ref = render(args.exe, str(packs["ref"]), RENDER_FLAGS, args.width, args.height, pose,
                         work / f"{scene}.ref.p{pi}.ppm")
            for v in ["ref@repeat"] + args.variants:
                img = render(args.exe, str(packs["ref" if v == "ref@repeat" else v]), RENDER_FLAGS, args.width,
                             args.height, pose, work / f"{scene}.{v.replace('+', '_').replace('@', '_')}.p{pi}.ppm")
                row = {"scene": scene, "splats": src["count"], "variant": v, "pose": pi, "yaw": pose[0],
                       "pitch": pose[1], "dist_scale": pose[2], "psnr_db": round(psnr(ref, img), 2),
                       "moved_gt2lsb": round(moved_share(ref, img), 6), "width": args.width,
                       "height": args.height, "build": build}
                rows.append(row)
                print(f"[{scene}] pose {pi} {v}: {row['psnr_db']} dB, moved {row['moved_gt2lsb']:.4%}", flush=True)

    with open(args.csv, "w", newline="") as f:
        f.write(f"# quantize_sim.py, build {build}, desktop host offscreen, flags {' '.join(RENDER_FLAGS)}\n")
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    with open(args.stats_csv, "w", newline="") as f:
        f.write(f"# quantize_sim.py step statistics, build {build}; pos_err_sigma = (chunk step / 2) / "
                f"exp(largest log-scale), chunk extent = largest axis of the tight centre AABB / scene diagonal\n")
        w = csv.DictWriter(f, fieldnames=list(stats[0].keys()))
        w.writeheader()
        for s in stats:
            w.writerow({k: (round(v, 6) if isinstance(v, float) else v) for k, v in s.items()})
    return 0


if __name__ == "__main__":
    sys.exit(main())
