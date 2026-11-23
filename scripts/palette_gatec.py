#!/usr/bin/env python3
"""Phase 8 Gate C tables from bench_android.py runs and scripted imports (S31, 2026-11-23).

  frames  frame-<device>-<hash>-p8-stump-<size>.<variant>...-raw.csv -> one row per run and window
          (cold 0-30 s, minute 4-5 = 240-300 s): fps, GPU ms, per-pass ms, visible, thermal max
  imports imports-<device>.txt (the app's "import timing" / "import palette" logcat lines) -> one
          row per import: budget, palette, per-stage ms, total ms, palette train / train+assign ms

Usage:
  python scripts/palette_gatec.py frames bench-out/gatec/*/frame-*-raw.csv --csv docs/benchmarks/X.csv
  python scripts/palette_gatec.py imports bench-out/gatec/*/imports-*.txt --csv docs/benchmarks/Y.csv

The phones were on the charger in S31 (owner's setup); the CSVs carry a `power` column for that.
stdlib + numpy (via bench_android.parse_frame_csv).
"""
import argparse
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_android import parse_frame_csv, window_summary  # noqa: E402

WINDOWS = [("cold_0_30s", 0.0, 30.0), ("min_4_5", 240.0, 300.0)]


def run_number(name: str, m: re.Match, paths: list[str]) -> str:
    """'-runN' in the name, else 2 when a '-run1' sibling of the same device and scene exists
    (the S24U 1 M palette was rerun after the direct SH 1 redo), else 1."""
    if m["run"]:
        return m["run"].removeprefix("-run")
    sibling = f"{m['device']}-{m['build']}-p8-{m['scene']}.{m['variant']}-run1-"
    return "2" if any(sibling in Path(p).name for p in paths) else "1"


def frames(paths: list[str], out: str, power: str) -> None:
    fields = ["device", "build", "scene", "variant", "run", "window", "fps", "gpu_ms", "cull_ms", "preprocess_ms",
              "sort_ms", "draw_ms", "visible", "thermal_max", "power"]
    rows = []
    for path in paths:
        name = Path(path).name.removeprefix("frame-").removesuffix("-raw.csv")
        m = re.match(r"(?P<device>[^-]+)-(?P<build>[0-9a-f]+(?:-dirty)?)-p8-(?P<scene>stump-[^.]+)\."
                     r"(?P<variant>[a-z0-9]+)(?P<run>-run\d+)?-", name)
        if not m:
            print(f"skipped {path}: name not recognised", file=sys.stderr)
            continue
        _, data = parse_frame_csv(Path(path).read_text())
        for label, lo, hi in WINDOWS:
            s = window_summary(data, lo, hi)
            if s is None:
                continue
            rows.append({"device": m["device"], "build": m["build"], "scene": m["scene"], "variant": m["variant"],
                         "run": run_number(name, m, paths),
                         "window": label, "fps": f"{s['fps']:.1f}", "gpu_ms": f"{s['gpu']:.3f}",
                         "cull_ms": f"{s['cull']:.3f}", "preprocess_ms": f"{s['pre']:.3f}",
                         "sort_ms": f"{s['sort']:.3f}", "draw_ms": f"{s['draw']:.3f}", "visible": f"{s['visible']:.0f}",
                         "thermal_max": s["status"], "power": power})
    rows.sort(key=lambda r: (r["device"], r["scene"], r["window"], r["variant"], r["run"]))
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} rows -> {out}")


def imports(paths: list[str], out: str, power: str, build: str) -> None:
    fields = ["device", "build", "source", "budget", "palette", "read_ms", "clean_ms", "pack_ms", "palette_ms",
              "write_ms", "total_ms", "palette_train_ms", "palette_train_assign_ms", "palette_mse", "power"]
    rows = []
    for path in paths:
        device = Path(path).stem.removeprefix("imports-")
        lines = Path(path).read_text().splitlines()
        current = None
        for line in lines:
            if "import timing:" in line:
                stages = dict((k, v) for k, v in re.findall(r"(read|clean|sor|pack|palette|write) (\d+) ms", line))
                total = re.search(r"total (\d+) ms", line).group(1)
                current = {"device": device, "build": build, "source": "stump-full.sh1.ply (4,049,567 splats, SH 1)",
                           "read_ms": stages.get("read", ""), "clean_ms": stages.get("clean", ""),
                           "pack_ms": stages.get("pack", ""), "palette_ms": stages.get("palette", ""),
                           "write_ms": stages.get("write", ""), "total_ms": total, "palette": "palette" in stages,
                           "palette_train_ms": "", "palette_train_assign_ms": "", "palette_mse": "", "power": power}
                rows.append(current)
            elif "import done:" in line and current is not None:
                current["budget"] = re.search(r"-> (\d+) splats", line).group(1)
            elif "import palette:" in line and current is not None:
                current["palette_train_ms"] = re.search(r"train (\d+) ms", line).group(1)
                current["palette_train_assign_ms"] = re.search(r"train \+ assign (\d+) ms", line).group(1)
                current["palette_mse"] = re.search(r"mse ([\d.e-]+)", line).group(1)
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} rows -> {out}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["frames", "imports"])
    p.add_argument("files", nargs="+")
    p.add_argument("--csv", required=True)
    p.add_argument("--power", default="charging")
    p.add_argument("--build", default="cd347ef")
    a = p.parse_args()
    if a.mode == "frames":
        frames(a.files, a.csv, a.power)
    else:
        imports(a.files, a.csv, a.power, a.build)
    return 0


if __name__ == "__main__":
    sys.exit(main())
