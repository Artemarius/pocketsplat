#!/usr/bin/env python3
"""Derive the benchmark variants of manifest scenes: cleaned, budget-cut, SH 0/1, PLY + pack.

For each requested scene (data/manifest.json name):
  1. `splatconv clean` at the full count (default parameters per docs/cleaning.md, SOR on, SH 1,
     Morton order) -> <name>-full.sh1.ply, plus the cleaning report in
     docs/benchmarks/cleaning-<name>.md (the same report clean_all.py writes).
  2. Budget cuts of that cleaned cloud for every --budgets value below its count, with every
     other op neutralized (alpha 0, no scale cap, no needle clamp, no SOR), so a cut differs from
     the full cloud by the importance prune alone -> <name>-<n>m.sh1.ply.
  3. SH 0 copies (`splatconv convert --sh 0`) and GPU packs (`splatconv pack`) of each variant.

Outputs land in data/derived/bench/ (gitignored). The PLYs are what the web viewers load
(scripts/bench_web.py) and the packs what the Android app loads (scripts/bench_android.py), so
every renderer draws the identical splats.

Usage: python scripts/prepare_bench_scenes.py counter truck bicycle [--budgets 1 2 4] [--sh 0 1]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clean_all import markdown_report  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "data" / "derived" / "bench"
BUDGET_ONLY = ["--alpha", "0", "--scale-frac", "1e9", "--no-needle-clamp", "--no-sor"]


def splatconv(exe: Path, *args: str) -> str:
    result = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"splatconv {' '.join(args[:3])}: {result.stderr.strip()}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+", help="manifest names")
    parser.add_argument("--budgets", nargs="*", type=float, default=[1, 2, 4], help="millions of splats")
    parser.add_argument("--sh", nargs="*", type=int, default=[0, 1], choices=[0, 1])
    parser.add_argument("--splatconv", type=Path,
                        default=REPO / "build" / "tools" / "splatconv" / "Release" / "splatconv.exe")
    parser.add_argument("--no-packs", action="store_true")
    args = parser.parse_args()

    manifest = {e["name"]: e for e in json.loads((REPO / "data" / "manifest.json").read_text(encoding="utf-8"))}
    OUT.mkdir(parents=True, exist_ok=True)
    for name in args.scenes:
        entry = manifest.get(name)
        if entry is None:
            print(f"{name}: not in data/manifest.json", file=sys.stderr)
            return 1
        src = REPO / "data" / entry["file"]
        full = OUT / f"{name}-full.sh1.ply"
        if not full.exists():
            print(f"{name}: cleaning {entry['splats']:,} splats ...", flush=True)
            report = json.loads(splatconv(args.splatconv, "clean", str(src), str(full), "--sh", "1",
                                          "--budget", "1000000000"))
            (REPO / "docs" / "benchmarks" / f"cleaning-{name}.md").write_text(
                markdown_report(entry, report, 1), encoding="utf-8")
            (OUT / f"{name}-full.report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
        count = json.loads((OUT / f"{name}-full.report.json").read_text(encoding="utf-8"))["output_count"]

        variants = [("full", full)]
        for millions in args.budgets:
            n = int(millions * 1_000_000)
            if n >= count:
                continue
            tag = f"{millions:g}m"
            cut = OUT / f"{name}-{tag}.sh1.ply"
            if not cut.exists():
                splatconv(args.splatconv, "clean", str(full), str(cut), "--sh", "1", "--budget", str(n),
                          *BUDGET_ONLY)
            variants.append((tag, cut))

        for tag, sh1 in variants:
            for sh in args.sh:
                ply = sh1 if sh == 1 else OUT / f"{name}-{tag}.sh0.ply"
                if sh == 0 and not ply.exists():
                    splatconv(args.splatconv, "convert", str(sh1), str(ply), "--sh", "0")
                pack = OUT / f"{name}-{tag}.sh{sh}.pspk"
                if not args.no_packs and not pack.exists():
                    splatconv(args.splatconv, "pack", str(ply), str(pack), "--sh", str(sh))
        print(f"{name}: {entry['splats']:,} -> {count:,} cleaned; variants "
              + ", ".join(tag for tag, _ in variants), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
