#!/usr/bin/env python3
"""Equal-work summary of sustained battery runs: PocketSplat frame-capped vs a paced web viewer.

Reads PocketSplat's per-frame CSVs + host samples (scripts/bench_android.py with --target-fps:
`frame-<device>-<hash>-<scene>...-capN-raw.csv`, `thermal-...-capN.csv`) and the web viewer's
per-second CSVs (scripts/bench_web.py: `web-<device>-<viewer>-<scene>.csv`), and reports per scene
and renderer: frame rate cold (0-30 s) and at minutes 8-10, kgsl GPU busy, peak skin temperature
and thermal status, mean battery power and energy per frame over the steady window (60 s to the
end), and the charge the run drew (fuel-gauge charge counter, ~5 mAh steps on the S24U), plus the mean
GPU clock where the CSVs carry it (at a capped rate the governor holds busy near a target and
moves the clock, so the clock is the work proxy) and GPU cycles per frame = busy x clock / fps
(for ours it matches GPU timestamps x clock within ~1 %, S18; for a browser it includes Chrome's own
GPU work, which is part of what the page costs).

Our rows are labelled by the run's variant (key bits, cull threshold, size, cap from the file name),
so per-rung runs at one cap (ADR-0009) summarize the same way. `--from` moves the start of the
steady window (default 60 s; 10 s for short cold runs on USB, where power is not meaningful).

Power uses `dumpsys battery`'s current now x voltage (bench scripts' current_ma / voltage_mv
columns; a current above 20 A in magnitude is taken as microamps). Both renderers include
whatever else the phone draws at fixed brightness - compare rows of one phone, not across phones.

Usage: python scripts/summarize_equal_work.py --ours DIR --web DIR --device s24u [--csv out.csv]
       [--viewer supersplat-orbit-touch --viewer supersplat-orbit-ourpose-touch] [--from 60] [--all]
"""
import argparse
import csv
import re
import statistics
from pathlib import Path

STEADY_FROM_S = 60.0


def read_table(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    meta = [line for line in lines if line.startswith("#")]
    rows = list(csv.DictReader(line for line in lines if line and not line.startswith("#")))
    return meta, rows


def num(value: str) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def finite(values) -> list[float]:
    return [v for v in values if v == v]


def mean(values) -> float:
    values = finite(values)
    return statistics.fmean(values) if values else float("nan")


def power_w(samples: list[dict[str, str]]) -> float:
    """Mean battery power over host samples (W, positive = drawn from the battery)."""
    watts = []
    for s in samples:
        current, voltage = num(s.get("current_ma", "")), num(s.get("voltage_mv", ""))
        if current != current or voltage != voltage:
            continue
        if abs(current) > 20000:  # microamps
            current /= 1000.0
        watts.append(abs(current) * voltage / 1e6)
    return mean(watts)


def charge_mah(samples: list[dict[str, str]]) -> float:
    counts = finite(num(s.get("charge_uah", "")) for s in samples)
    return (counts[0] - counts[-1]) / 1000.0 if len(counts) > 1 else float("nan")


def cycles(busy: float, mhz: float, fps: float) -> float:
    """Millions of GPU cycles per frame from kgsl busy % and clock (MHz)."""
    return busy / 100.0 * mhz / fps if fps > 0 else float("nan")


def ours(frame_csv: Path, thermal_csv: Path, steady_from: float) -> dict[str, float]:
    meta, rows = read_table(frame_csv)
    t = [num(r["t_s"]) for r in rows]

    def fps(lo: float, hi: float) -> float:
        sel = [x for x in t if lo <= x < hi]
        return (len(sel) - 1) / (sel[-1] - sel[0]) if len(sel) > 1 and sel[-1] > sel[0] else float("nan")

    cap = m.group(1) if (m := re.search(r"target_fps=([\d.]+)", " ".join(meta))) else "0"
    _, samples = read_table(thermal_csv)
    # Host samples start at launch, frame rows at the first rendered frame (after loading). The host's
    # last sample comes from the poll that found the finished CSV, i.e. after the run: drop it, then
    # keep the samples inside the frame rows' span, measured back from the end of the run.
    samples_t = sorted(num(s["t_s"]) for s in samples)
    run_end = samples_t[-2] if len(samples_t) > 1 else samples_t[-1]
    span = t[-1] if t else 0.0
    steady = [s for s in samples if run_end - span + steady_from <= num(s["t_s"]) <= run_end]
    late = [r for r in rows if num(r["t_s"]) >= 480.0]
    busy = mean(num(s.get("gpu_busy_pct", "")) for s in steady)
    mhz = mean(num(s.get("gpu_mhz", "")) for s in steady)
    fps_steady = fps(steady_from, 1e9)
    return {
        "cap": float(cap),
        "fps_cold": fps(0.0, 30.0),
        "fps_late": fps(480.0, 600.0),
        "fps_steady": fps_steady,
        "gpu_ms_late": statistics.median(num(r["gpu_ms"]) for r in late) if late else float("nan"),
        "busy": busy,
        "mhz": mhz,
        "mcycles": cycles(busy, mhz, fps_steady),
        "visible": mean(num(r["visible"]) for r in rows if num(r["t_s"]) >= steady_from),
        "skin_max": max(finite(num(s["skin_c"]) for s in samples), default=float("nan")),
        "status_max": max(int(num(r["thermal_status"])) for r in rows),
        "power_w": power_w(steady),
        "charge_mah": charge_mah(samples),
    }


def web(path: Path, steady_from: float) -> dict[str, float]:
    _, rows = read_table(path)

    def med_fps(lo: float, hi: float) -> float:
        sel = [num(r["fps"]) for r in rows if lo <= num(r["t_s"]) <= hi]
        return statistics.median(sel) if sel else float("nan")

    steady = [r for r in rows if num(r["t_s"]) >= steady_from]
    span = num(steady[-1]["t_s"]) - num(steady[0]["t_s"]) if len(steady) > 1 else 0.0
    # frames over the steady window: per-second fps x window length, summed
    frames = sum(num(r["fps"]) for r in steady)
    return {
        "cap": float("nan"),
        "fps_cold": med_fps(0.0, 30.0),
        "fps_late": med_fps(480.0, 600.0),
        "fps_steady": frames / len(steady) if steady else float("nan"),
        "gpu_ms_late": float("nan"),
        "busy": mean(num(r.get("gpu_busy_pct", "")) for r in steady),
        "mhz": mean(num(r.get("gpu_mhz", "")) for r in steady),
        "mcycles": mean(cycles(num(r.get("gpu_busy_pct", "")), num(r.get("gpu_mhz", "")), num(r["fps"]))
                        for r in steady),
        "visible": float("nan"),
        "skin_max": max(finite(num(r["skin_c"]) for r in rows), default=float("nan")),
        "status_max": max(int(num(r["thermal_status"])) for r in rows),
        "power_w": power_w(steady),
        "charge_mah": charge_mah(rows),
        "span": span,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ours", required=True, type=Path)
    parser.add_argument("--web", required=True, type=Path)
    parser.add_argument("--device", required=True)
    parser.add_argument("--viewer", action="append", default=None,
                        help="web viewer label(s) as in the file names (default supersplat-orbit-touch)")
    parser.add_argument("--from", dest="steady_from", type=float, default=STEADY_FROM_S,
                        help="start of the steady window in seconds (default 60)")
    parser.add_argument("--all", action="store_true", help="our uncapped runs too (default: -cap* runs only)")
    parser.add_argument("--csv", type=Path, default=None, help="also write the summary rows here")
    args = parser.parse_args()
    viewers = args.viewer or ["supersplat-orbit-touch"]

    results = []
    pattern = f"frame-{args.device}-*-raw.csv" if args.all else f"frame-{args.device}-*-cap*-raw.csv"
    scenes = []
    for frame_csv in sorted(args.ours.glob(pattern)):
        m = re.search(r"-([a-z0-9]+-full\.sh0)-(.*)-raw\.csv$", frame_csv.name)
        scene, variant = m.group(1), m.group(2)
        thermal_csv = frame_csv.with_name(frame_csv.name.replace("frame-", "thermal-").replace("-raw.csv", ".csv"))
        results.append((scene, f"PocketSplat {variant}", ours(frame_csv, thermal_csv, args.steady_from)))
        if scene not in scenes:
            scenes.append(scene)
    for scene in scenes:
        for viewer in viewers:
            web_csv = args.web / f"web-{args.device}-{viewer}-{scene}.csv"
            if web_csv.exists():
                results.append((scene, viewer, web(web_csv, args.steady_from)))

    columns = ["fps_cold", "fps_late", "busy", "mhz", "mcycles", "visible", "skin_max", "status_max", "power_w",
               "charge_mah"]
    print(f"| Phone | Scene | Renderer | fps cold -> min 8-10 | fps from {args.steady_from:g} s | GPU busy | "
          f"GPU clock | GPU Mcycles/frame | skin max | status max | power | energy/frame | charge drawn |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    out_rows = [["device", "scene", "renderer", "cap_fps", *columns, "fps_steady", "mj_per_frame", "gpu_ms_late"]]
    for scene, renderer, r in results:
        mj = r["power_w"] / r["fps_steady"] * 1000.0 if r["fps_steady"] > 0 else float("nan")
        print(f"| {args.device} | {scene.split('-')[0]} | {renderer} | {r['fps_cold']:.1f} -> {r['fps_late']:.1f} | "
              f"{r['fps_steady']:.1f} | {r['busy']:.0f} % | {r['mhz']:.0f} MHz | {r['mcycles']:.2f} | "
              f"{r['skin_max']:.1f} C | {r['status_max']} | {r['power_w']:.2f} W | {mj:.1f} mJ | "
              f"{r['charge_mah']:.0f} mAh |")
        out_rows.append([args.device, scene, renderer, f"{r['cap']:g}", *(f"{r[c]:.3f}" for c in columns),
                         f"{r['fps_steady']:.3f}", f"{mj:.2f}", f"{r['gpu_ms_late']:.3f}"])
    if args.csv:
        with args.csv.open("w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(out_rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
