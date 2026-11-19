#!/usr/bin/env python3
"""SH 1 palette trainer comparison (Phase 8 B, docs/sh1-palette-research.md §3-4).

Drives tools/palettebench over each scene's SH1 vectors and collects one CSV row per training run
(trainer, k, epochs, seed, training cap -> loss, dead entries, used-code entropy, distance pairs,
wall times). Losses, dead entries, entropy and pairs do not depend on the machine and come from the
desktop; wall times that count come from the S24 Ultra only (CLAUDE.md §7: ask the owner before
every phone run).

  dump     scene (PLY/SPZ, prepared as `splatconv pack` does) -> <work>/<name>.sh1
  run      palettebench over every .sh1 locally (--low-priority: idle priority, inherited by
           palettebench) or on a phone (--adb: pushes palettebench and the .sh1 files to
           /data/local/tmp, waits for a cool phone before each scene and k, records thermal status
           and skin temperature before and after)
  summary  per scene and k, seed means against the Lloyd-25 loss: loss ratio, pairs ratio, and
           the fewest e-means epochs at or below the Lloyd-25 loss

Hamerly computes exactly Lloyd's iterations (same labels and centroids, bit for bit), so on the
desktop `hamerly` rows also give Lloyd's loss at that epoch count; Lloyd's own pairs are
epochs_run x train_count x k. Lloyd runs only matter for wall time.

Usage:
  python scripts/palette_trainers.py dump --palettebench build/tools/palettebench/Release/palettebench.exe \\
      --scene museum1=bench-out/palette/src/museum1.sh1.clean.ply --work bench-out/palette/sh1
  python scripts/palette_trainers.py run --palettebench build/tools/palettebench/Release/palettebench.exe \\
      --work bench-out/palette/sh1 --trainer hamerly,emeans --k 4096 --epochs 1,2,3,5,10,25 \\
      --seed 1,2,3,4,5 --cap 0,64 --threads 4 --low-priority --label laptop \\
      --csv docs/benchmarks/palette-trainers-<hash>-<date>.csv
  python scripts/palette_trainers.py summary docs/benchmarks/palette-trainers-<hash>-<date>.csv

stdlib only.
"""
import argparse
import csv
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from palette_ab import build_hash, lower_priority  # noqa: E402

PHONE_DIR = "/data/local/tmp/palettebench"
EXTRA_FIELDS = ["build", "thermal_before", "skin_before_c", "thermal_after", "skin_after_c"]


def dump(args) -> int:
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    for spec in args.scene:
        name, _, src = spec.partition("=")
        out = work / f"{name}.sh1"
        if out.exists() and not args.force:
            print(f"{name}: {out} exists")
            continue
        subprocess.run([args.palettebench, src, "--write-sh1", str(out)], check=True)
    return 0


def scenes_in(work: Path, names: list[str] | None) -> list[Path]:
    files = sorted(work.glob("*.sh1"))
    if names:
        files = [f for f in files if f.stem in names]
    if not files:
        raise SystemExit(f"no .sh1 files in {work}")
    return files


def bench_args(args) -> list[str]:
    out = ["--trainer", args.trainer, "--k", args.k, "--epochs", args.epochs, "--seed", args.seed, "--cap", args.cap,
           "--threads", str(args.threads), "--repeat", str(args.repeat)]
    if args.label:
        out += ["--label", args.label]
    return out


def append_rows(csv_path: Path, rows: list[dict], fields: list[str]) -> None:
    fresh = not csv_path.exists() or csv_path.stat().st_size == 0
    with open(csv_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        if fresh:
            writer.writeheader()
        writer.writerows(rows)


def parse_bench_csv(text: str) -> tuple[list[str], list[dict]]:
    reader = csv.DictReader(text.splitlines())
    return list(reader.fieldnames or []), list(reader)


def run_local(args, build: str) -> int:
    csv_path = Path(args.csv)
    for sh1 in scenes_in(Path(args.work), args.only):
        for k in args.k.split(","):
            local_args = bench_args(args)
            local_args[local_args.index("--k") + 1] = k
            started = time.monotonic()
            proc = subprocess.run([args.palettebench, str(sh1), *local_args], capture_output=True, text=True)
            if proc.returncode != 0:
                print(proc.stderr, file=sys.stderr)
                return 1
            fields, rows = parse_bench_csv(proc.stdout)
            for row in rows:
                row.update({"build": build, "thermal_before": "", "skin_before_c": "", "thermal_after": "",
                            "skin_after_c": ""})
            append_rows(csv_path, rows, fields + EXTRA_FIELDS)
            print(f"{sh1.stem:>16} k {k:>6}: {len(rows)} runs in {time.monotonic() - started:7.1f} s", flush=True)
    return 0


def run_phone(args, build: str) -> int:
    from bench_android import adb, shell, thermal, wait_cool  # adb helpers (CLAUDE.md §5 notes)

    serial = args.serial
    shell(f"mkdir -p {PHONE_DIR}", serial=serial)
    adb("push", args.palettebench, f"{PHONE_DIR}/palettebench", serial=serial, timeout=600)
    shell(f"chmod 755 {PHONE_DIR}/palettebench", serial=serial)
    csv_path = Path(args.csv)
    for sh1 in scenes_in(Path(args.work), args.only):
        remote = f"{PHONE_DIR}/{sh1.name}"
        adb("push", str(sh1), remote, serial=serial, timeout=600)
        for k in args.k.split(","):
            wait_cool(serial, args.skin_max, args.cool_timeout)
            before = thermal(serial)
            phone_args = bench_args(args)
            phone_args[phone_args.index("--k") + 1] = k
            started = time.monotonic()
            out = shell(" ".join([f"{PHONE_DIR}/palettebench", remote, *phone_args, "2>/dev/null"]), serial=serial)
            after = thermal(serial)
            fields, rows = parse_bench_csv(out)
            if not rows:
                print(f"{sh1.stem} k {k}: no rows; palettebench output:\n{out}", file=sys.stderr)
                return 1
            for row in rows:
                row.update({"build": build, "thermal_before": before[0], "skin_before_c": f"{before[1]:.1f}",
                            "thermal_after": after[0], "skin_after_c": f"{after[1]:.1f}"})
            append_rows(csv_path, rows, fields + EXTRA_FIELDS)
            print(f"{sh1.stem:>16} k {k:>6}: {len(rows)} runs in {time.monotonic() - started:7.1f} s, thermal "
                  f"{before[0]} -> {after[0]}, skin {before[1]:.1f} -> {after[1]:.1f} C", flush=True)
        shell(f"rm -f {remote}", serial=serial)
    return 0


def run(args) -> int:
    if args.low_priority:
        lower_priority()
    build = build_hash()
    return run_phone(args, build) if args.adb else run_local(args, build)


def summary(args) -> int:
    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(f))
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for r in rows:
        groups[(r["input"], int(r["k"]), int(r["cap_per_entry"]), r["trainer"], int(r["epochs"]))].append(r)

    def mean(values):
        values = list(values)
        return sum(values) / len(values) if values else float("nan")

    scenes = sorted({(g[0], g[1], g[2]) for g in groups})
    for scene, k, cap in scenes:
        ref_rows = groups.get((scene, k, 0, "hamerly", 25)) or groups.get((scene, k, 0, "lloyd", 25))
        if not ref_rows:
            continue
        ref = {int(r["seed"]): float(r["mse"]) for r in ref_rows}
        lloyd_pairs = mean(int(r["epochs_run"]) * int(r["train_count"]) * k for r in ref_rows)
        print(f"\n{scene} k {k} cap {cap or 'none'}: Lloyd-25 loss {mean(ref.values()):.6g} "
              f"(seeds {sorted(ref)}), Lloyd-25 pairs {lloyd_pairs:.3g}")
        print(f"  {'trainer':>8} {'epochs':>6} {'loss/L25':>9} {'worst':>9} {'pairs/L25':>9} {'dead':>7} "
              f"{'entropy':>8} {'train_ms':>9}")
        best_emeans = None
        for (s, kk, c, trainer, epochs), rs in sorted(groups.items()):
            if (s, kk, c) != (scene, k, cap):
                continue
            ratios = [float(r["mse"]) / ref[int(r["seed"])] for r in rs if int(r["seed"]) in ref]
            pairs = mean(int(r["pairs"]) for r in rs)
            print(f"  {trainer:>8} {epochs:>6} {mean(ratios):9.4f} {max(ratios, default=float('nan')):9.4f} "
                  f"{pairs / lloyd_pairs:9.3f} {mean(int(r['dead']) for r in rs):7.1f} "
                  f"{mean(float(r['used_entropy_bits']) for r in rs):8.3f} {mean(float(r['train_ms']) for r in rs):9.0f}")
            if trainer == "emeans" and ratios and max(ratios) <= 1.0 and (best_emeans is None or epochs < best_emeans):
                best_emeans = epochs
        print(f"  fewest e-means epochs at or below Lloyd-25 on every seed: {best_emeans or 'none tried'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    d = sub.add_parser("dump")
    d.add_argument("--palettebench", required=True)
    d.add_argument("--scene", action="append", required=True, help="name=path of an SH 1 (or higher) PLY/SPZ")
    d.add_argument("--work", default="bench-out/palette/sh1")
    d.add_argument("--force", action="store_true")
    r = sub.add_parser("run")
    r.add_argument("--palettebench", required=True, help="desktop build, or the arm64 build with --adb")
    r.add_argument("--work", default="bench-out/palette/sh1")
    r.add_argument("--only", nargs="+", help="scene names (default: every .sh1 in --work)")
    r.add_argument("--trainer", default="hamerly,emeans")
    r.add_argument("--k", default="4096", help="comma list; one palettebench call per scene and k")
    r.add_argument("--epochs", default="1,2,3,5,10,25")
    r.add_argument("--seed", default="1,2,3,4,5")
    r.add_argument("--cap", default="0", help="points per palette entry, comma list, 0 = no cap")
    r.add_argument("--threads", type=int, default=4)
    r.add_argument("--repeat", type=int, default=1)
    r.add_argument("--label", default="")
    r.add_argument("--low-priority", action="store_true")
    r.add_argument("--adb", action="store_true", help="run on the phone (ask the owner first)")
    r.add_argument("--serial")
    r.add_argument("--skin-max", type=float, default=32.0)
    r.add_argument("--cool-timeout", type=int, default=1800)
    r.add_argument("--csv", required=True)
    s = sub.add_parser("summary")
    s.add_argument("csv")
    args = parser.parse_args()
    if getattr(args, "palettebench", None):
        args.palettebench = str(Path(args.palettebench).resolve())
    return {"dump": dump, "run": run, "summary": summary}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
