#!/usr/bin/env python3
"""A/B summary of the thermal-adaptive quality controller (ADR-0009, Phase 4 gate).

Reads scripts/bench_android.py outputs of capped runs (`frame-<device>-<hash>-<scene>-...-capN-raw.csv`
plus `thermal-...-capN.csv`), fixed rung (`-c0.1`) and controller (`-ctl`) alike, and reports per run
the gate metrics of the ADR's A/B protocol over the late window (default minutes 5-10):

  * p5 fps: 5th percentile of the per-second frame rates
  * p99 frame time: 99th percentile of the frame intervals
  * frames on target: share of frame intervals within 1.1x the cap's interval (the controller's
    own miss tolerance), late window and whole run
  * GPU Mcycles/frame: GPU timestamps x the app's kgsl clock sample, per frame (S18: matches
    busy x clock / fps within ~1 %)
  * peak thermal status (app) and skin temperature (host), energy per frame from the fuel gauge
    over the steady window (60 s to the end, as summarize_equal_work.py)
  * time at each contribution threshold, and the time-weighted image cost: each threshold's
    PSNR (min and mean over the six quality_ab.py poses, from --quality) weighted by the time
    spent at it

and, for controller runs, its decision log.

Usage: python scripts/summarize_controller.py --runs DIR --device s24u \\
           --quality docs/benchmarks/quality-rtx3060-rungs-2026-10-28-864x1872.csv [--csv out.csv]
"""
import argparse
import csv
import re
from pathlib import Path

import numpy as np

from summarize_equal_work import STEADY_FROM_S, ours, read_table

REASONS = ["none", "frame_miss", "thermal_high", "thermal_low", "ladder_exhausted"]


def psnr_table(path: Path | None, scene: str, resolution: str, key_bits: str) -> dict[float, tuple[float, float]]:
    """{threshold: (psnr_min, psnr_mean)} for the scene from a quality_ab.py CSV."""
    if path is None:
        return {}
    table: dict[float, tuple[float, float]] = {}
    with path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["scene"] != scene or row.get("resolution", resolution) != resolution:
                continue
            flags = row["flags"]
            if f"--key-bits {key_bits}" not in flags or "--scale" in flags:
                continue
            m = re.search(r"--min-contribution ([\d.]+)", flags)
            if m:
                table[float(m.group(1))] = (float(row["psnr_min_db"]), float(row["psnr_mean_db"]))
    return table


def summarize(frame_csv: Path, quality: Path | None, lo: float, hi: float) -> dict:
    meta_lines, rows = read_table(frame_csv)
    meta = {}
    for line in meta_lines:
        for part in line[1:].split(", "):
            if "=" in part:
                key, _, value = part.partition("=")
                meta[key.strip()] = value.strip()
    cols = rows[0].keys() if rows else []
    t = np.array([float(r["t_s"]) for r in rows])
    interval = np.array([float(r["cpu_ms"]) for r in rows])
    gpu = np.array([float(r["gpu_ms"]) for r in rows])
    mhz = np.array([float(r["gpu_mhz"]) for r in rows]) if "gpu_mhz" in cols else np.full(len(rows), np.nan)
    if "min_contribution" in cols:
        threshold = np.array([float(r["min_contribution"]) for r in rows])
    else:
        threshold = np.full(len(rows), float(meta.get("min_contribution", "nan")))
    cap = float(meta.get("target_fps", "0"))
    period_ms = 1000.0 / cap if cap > 0 else float("nan")

    late = (t >= lo) & (t < hi)
    seconds = np.floor(t[late]).astype(int)
    per_second = np.bincount(seconds - seconds.min()) if seconds.size else np.array([])
    per_second = per_second[1:-1] if per_second.size > 2 else per_second  # partial edge seconds
    cycles = gpu * mhz / 1000.0  # Mcycles per frame
    dt = np.diff(t, append=t[-1])

    key_bits = meta.get("key_bits", "16")
    scene = meta.get("scene", "").replace(".sh0.pspk", "").replace(".pspk", "")
    table = psnr_table(quality, scene, meta.get("resolution", ""), key_bits)
    at = {float(c): float(dt[threshold == c].sum()) for c in np.unique(threshold)}
    weighted_min = weighted_mean = float("nan")
    if at and all(c in table for c in at):
        total = sum(at.values())
        weighted_min = sum(table[c][0] * s for c, s in at.items()) / total
        weighted_mean = sum(table[c][1] * s for c, s in at.items()) / total

    decisions = []
    if "reason" in cols:
        for r in rows:
            if int(float(r["reason"])) > 0:
                decisions.append((float(r["t_s"]), REASONS[int(float(r["reason"]))], float(r["min_contribution"]),
                                  float(r["thermal_forecast"])))

    thermal_csv = frame_csv.with_name(frame_csv.name.replace("frame-", "thermal-").replace("-raw.csv", ".csv"))
    host = ours(frame_csv, thermal_csv, STEADY_FROM_S) if thermal_csv.exists() else {}
    fps_steady = host.get("fps_steady", float("nan"))
    return {
        "variant": re.sub(r"^frame-[^-]+-", "", frame_csv.name).replace("-raw.csv", ""),
        "controller": meta.get("controller", "off"),
        "cap": cap,
        "fps_cold": host.get("fps_cold", float("nan")),
        "fps_late": host.get("fps_late", float("nan")),
        "p5_fps": float(np.percentile(per_second, 5)) if per_second.size else float("nan"),
        "p99_ms": float(np.percentile(interval[late], 99)) if late.any() else float("nan"),
        "on_target_late": float(np.mean(interval[late] <= 1.1 * period_ms)) if late.any() else float("nan"),
        "on_target_all": float(np.mean(interval[1:] <= 1.1 * period_ms)),
        "mcycles_late": float(np.nanmean(cycles[late])) if late.any() else float("nan"),
        "status_max": int(max(float(r["thermal_status"]) for r in rows)),
        "skin_max": host.get("skin_max", float("nan")),
        "mj": host.get("power_w", float("nan")) / fps_steady * 1000.0 if fps_steady > 0 else float("nan"),
        "time_at": at,
        "psnr_min_w": weighted_min,
        "psnr_mean_w": weighted_mean,
        "decisions": decisions,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", required=True, type=Path, nargs="+", help="bench_android.py output dirs")
    parser.add_argument("--device", required=True)
    parser.add_argument("--quality", type=Path, default=None, help="quality_ab.py CSV with the rungs' PSNR")
    parser.add_argument("--from", dest="lo", type=float, default=300.0, help="late window start, s (default 300)")
    parser.add_argument("--to", dest="hi", type=float, default=600.0, help="late window end, s (default 600)")
    parser.add_argument("--csv", type=Path, default=None, help="also write the summary rows here")
    args = parser.parse_args()

    results = [summarize(f, args.quality, args.lo, args.hi)
               for d in args.runs for f in sorted(d.glob(f"frame-{args.device}-*-cap*-raw.csv"))]
    win = f"{args.lo / 60:g}-{args.hi / 60:g} min"
    print(f"| Run | fps cold -> min 8-10 | p5 fps ({win}) | p99 frame ({win}) | on target ({win} / all) | "
          f"GPU Mcycles/frame ({win}) | status max | skin max | energy/frame | time at threshold | "
          f"PSNR min / mean (time-weighted) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    out = [["device", "run", "controller", "cap_fps", "fps_cold", "fps_late", "p5_fps", "p99_ms", "on_target_late",
            "on_target_all", "mcycles_late", "status_max", "skin_max", "mj_per_frame", "time_at", "psnr_min_w",
            "psnr_mean_w"]]
    for r in results:
        time_at = " / ".join(f"c {c:g}: {s:.0f} s" for c, s in sorted(r["time_at"].items()))
        print(f"| {r['variant']} | {r['fps_cold']:.1f} -> {r['fps_late']:.1f} | {r['p5_fps']:.0f} | "
              f"{r['p99_ms']:.1f} ms | {100 * r['on_target_late']:.1f} % / {100 * r['on_target_all']:.1f} % | "
              f"{r['mcycles_late']:.2f} | {r['status_max']} | {r['skin_max']:.1f} C | {r['mj']:.1f} mJ | {time_at} | "
              f"{r['psnr_min_w']:.1f} / {r['psnr_mean_w']:.1f} dB |")
        out.append([args.device, r["variant"], r["controller"], f"{r['cap']:g}", f"{r['fps_cold']:.2f}",
                    f"{r['fps_late']:.2f}", f"{r['p5_fps']:.1f}", f"{r['p99_ms']:.2f}", f"{r['on_target_late']:.4f}",
                    f"{r['on_target_all']:.4f}", f"{r['mcycles_late']:.3f}", r["status_max"], f"{r['skin_max']:.1f}",
                    f"{r['mj']:.2f}", time_at, f"{r['psnr_min_w']:.2f}", f"{r['psnr_mean_w']:.2f}"])
    for r in results:
        if r["decisions"]:
            print(f"\n{r['variant']} decisions:")
            for t, reason, c, forecast in r["decisions"]:
                print(f"  {t:6.1f} s  {reason:16s} from c {c:g}  (forecast {forecast:.3f})")
    if args.csv:
        with args.csv.open("w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
