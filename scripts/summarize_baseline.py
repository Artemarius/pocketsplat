#!/usr/bin/env python3
"""Cold-window summary of a like-for-like baseline run, as a Markdown table.

Reads PocketSplat's per-frame CSVs (scripts/bench_android.py, `frame-<device>-<hash>-<scene>[-16bit]
[-WxH]-raw.csv` or `-cold.csv`) and the web viewers' per-second CSVs (scripts/bench_web.py,
`web-<device>-<viewer>-<scene>.csv`), and prints one row per scene: median fps over the first 30 s
after load (ours: frames / time; web: median of the 1-s windows) plus our median GPU ms. Optional
`--splatkit` takes `scene=fps` pairs measured from splatkit's own benchmark log.

Usage: python scripts/summarize_baseline.py --ours DIR --web DIR --device s24u
"""
import argparse
import csv
import re
import statistics
from pathlib import Path


def read_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    rows = [r for r in csv.reader(path.open(encoding="utf-8")) if r and not r[0].startswith("#")]
    return rows[0], rows[1:]


def splat_count(path: Path) -> int:
    """Scene splat count from a per-frame CSV's `# scene=..., splats=N` header line."""
    for line in path.open(encoding="utf-8"):
        if m := re.search(r"splats=(\d+)", line):
            return int(m.group(1))
    return 0


def ours_cold(path: Path) -> tuple[float, float]:
    """(fps, median gpu ms) over t_s <= 30 of a per-frame CSV."""
    header, rows = read_rows(path)
    ix = {k: i for i, k in enumerate(header)}
    window = [r for r in rows if float(r[ix["t_s"]]) <= 30.0]
    t = [float(r[ix["t_s"]]) for r in window]
    fps = (len(window) - 1) / (t[-1] - t[0]) if len(window) > 1 and t[-1] > t[0] else float("nan")
    gpu = statistics.median(float(r[ix["gpu_ms"]]) for r in window)
    return fps, gpu


def web_cold(path: Path) -> tuple[float, str]:
    """(median fps over t_s <= 30, 'WxH backend') of a bench_web.py CSV; nan when the load failed."""
    text = path.read_text(encoding="utf-8")
    if "LOAD FAILED" in text:
        return float("nan"), "load failed"
    header, rows = read_rows(path)
    ix = {k: i for i, k in enumerate(header)}
    window = [float(r[ix["fps"]]) for r in rows if float(r[ix["t_s"]]) <= 30.0]
    if not window:
        return float("nan"), "no frames"
    first = rows[0]
    return statistics.median(window), f"{first[ix['canvas_w']]}x{first[ix['canvas_h']]} {first[ix['backend']]}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ours", type=Path, required=True)
    parser.add_argument("--web", type=Path, required=True)
    parser.add_argument("--device", default="s24u")
    parser.add_argument("--canvas", default="540x975", help="the like-for-like resolution tag")
    parser.add_argument("--splatkit", nargs="*", default=[], help="scene=fps pairs")
    args = parser.parse_args()

    ours, splats = {}, {}
    for path in sorted(args.ours.glob(f"frame-{args.device}-*-raw.csv")):
        m = re.match(rf"frame-{args.device}-[0-9a-f]+-(.+?)(-16bit)?(-\d+x\d+)?-raw\.csv$", path.name)
        if not m:
            continue
        scene = m.group(1).removesuffix(".sh0").removesuffix(".sh1")
        key = "16bit" if m.group(2) else (m.group(3)[1:] if m.group(3) else "protocol")
        ours.setdefault(scene, {})[key] = ours_cold(path)
        splats[scene] = splat_count(path)
    web = {}
    for path in sorted(args.web.glob(f"web-{args.device}-*.csv")):
        m = re.match(rf"web-{args.device}-(supersplat-webgl|supersplat-stochastic|supersplat-nogate|supersplat|spark-touch|spark)-(.+)\.csv$",
                     path.name)
        if m:
            scene = m.group(2).removesuffix(".sh0").removesuffix(".sh1")
            web.setdefault(scene, {})[m.group(1)] = web_cold(path)
    splatkit = dict(pair.split("=", 1) for pair in args.splatkit)

    def f(value: float) -> str:
        return "—" if value != value else f"{value:.1f}"

    print(f"| Scene | Splats | Ours 864×1872 fps (GPU ms) | Ours 16-bit keys | Ours {args.canvas} | SuperSplat WebGPU "
          f"| SuperSplat WebGPU, gate off | SuperSplat WebGL2 | SuperSplat sort-free | Spark | Spark, touch-paced "
          f"| splatkit |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for scene in sorted(set(ours) | set(web), key=lambda s: splats.get(s, 1 << 40)):
        o, w = ours.get(scene, {}), web.get(scene, {})
        cell = lambda k: f"{f(o[k][0])} ({o[k][1]:.1f})" if k in o else "—"
        webcell = lambda k: f(w[k][0]) if k in w else "—"
        print(f"| {scene} | {splats.get(scene, 0) / 1e6:.2f} M | {cell('protocol')} | {cell('16bit')} | {cell(args.canvas)} | {webcell('supersplat')} "
              f"| {webcell('supersplat-nogate')} | {webcell('supersplat-webgl')} | {webcell('supersplat-stochastic')} "
              f"| {webcell('spark')} | {webcell('spark-touch')} | {splatkit.get(scene, '—')} |")
    canvases = sorted({v[1] for w in web.values() for v in w.values()})
    print(f"\nWeb canvases / backends seen: {', '.join(canvases)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
