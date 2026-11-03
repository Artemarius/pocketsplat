#!/usr/bin/env python3
"""Run `splatconv clean` over every PLY in the data manifest(s) and write Markdown reports.

Reads data/manifest.json plus, if present, data/manifest.local.json (gitignored, local-only
scenes). Cleaned SPZ files land in data/derived/, per-scene reports in
docs/benchmarks/cleaning-<name>.md — but reports for local-only scenes are NOT written into
docs/ (they must never be committed); those go to data/derived/ instead.

Usage: python scripts/clean_all.py [--splatconv PATH] [--sh 0|1] [--only NAME]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def run_clean(splatconv: Path, entry: dict, sh: int, derived: Path) -> dict | None:
    src = REPO / "data" / entry["file"]
    if not src.exists() or src.suffix.lower() != ".ply":
        return None
    out = derived / f"{entry['name']}.clean.spz"
    cmd = [str(splatconv), "clean", str(src), str(out), "--sh", str(sh)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAILED: {result.stderr.strip()}", file=sys.stderr)
        return None
    return json.loads(result.stdout)


def markdown_report(entry: dict, report: dict, sh: int) -> str:
    lines = [
        f"# Cleaning report — {entry['name']}",
        "",
        f"Source: {entry['source']} ({entry['license']})",
        "",
        f"Input {report['input_count']:,} splats (SH {entry['sh_degree']}) -> "
        f"output {report['output_count']:,} splats (SH {sh}), "
        f"{report['output_bytes']:,} bytes SPZ. Default parameters per docs/cleaning.md.",
        "",
        "| Op | Input | Removed | Removed % | ms | Params |",
        "|---|---|---|---|---|---|",
    ]
    for op in report["ops"]:
        pct = 100.0 * op["removed"] / op["input_count"] if op["input_count"] else 0.0
        lines.append(
            f"| {op['op']} | {op['input_count']:,} | {op['removed']:,} | {pct:.1f} | "
            f"{op['ms']:.0f} | {op['params']} |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--splatconv", type=Path,
                        default=REPO / "build" / "tools" / "splatconv" / "Release" / "splatconv.exe")
    parser.add_argument("--sh", type=int, default=1, choices=[0, 1])
    parser.add_argument("--only", help="clean only the manifest entry with this name")
    args = parser.parse_args()

    derived = REPO / "data" / "derived"
    derived.mkdir(exist_ok=True)
    bench_dir = REPO / "docs" / "benchmarks"

    entries = []
    for manifest, local in ((REPO / "data" / "manifest.json", False),
                            (REPO / "data" / "manifest.local.json", True)):
        if manifest.exists():
            with open(manifest) as f:
                entries += [(e, local) for e in json.load(f)]

    failures = 0
    for entry, local in entries:
        if args.only and entry["name"] != args.only:
            continue
        if not (REPO / "data" / entry["file"]).suffix == ".ply":
            continue
        print(f"cleaning {entry['name']} ...")
        report = run_clean(args.splatconv, entry, args.sh, derived)
        if report is None:
            failures += 1
            continue
        target_dir = derived if local else bench_dir
        target = target_dir / f"cleaning-{entry['name']}.md"
        target.write_text(markdown_report(entry, report, args.sh), encoding="utf-8")
        print(f"  {report['input_count']:,} -> {report['output_count']:,} splats, "
              f"report {target.relative_to(REPO)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
