#!/usr/bin/env python3
"""README figures from committed benchmark CSVs (docs/benchmarks), light and dark SVG each.

    python scripts/plot_readme.py [--out docs/figures]

writes <name>-light.svg and <name>-dark.svg for GitHub's <picture> / prefers-color-scheme switch:

    cold-fps          cold fps per public scene, both phones, 864x1872 (readme-cold-2026-10-31.csv)
    supersplat-pose   PocketSplat vs SuperSplat at our camera pose: fps and GPU cycles per frame
                      (pose-ab-s24u-fba401f.csv, pose-ab-s25u-f0f242c.csv)
    stump-history     S24U stump fps across the optimizations that moved it (per-run 5-s CSVs +
                      readme-cold)
    controller-ab     20 min at 60 fps: fixed 0.1 px cull vs the thermal controller, skin, thermal
                      status and the threshold it chose (1d3faec equal-start legs)
    frame-breakdown   share of GPU frame time per pass on the Adreno 750, Adreno 830 and RTX 3060
                      (copy-probe-*.csv control rows, frame-pipeline-rtx3060-183a6cc-2026-11-01.csv)

Every number is read from the CSV at plot time; captions in the README say what each run was.
matplotlib + numpy (CLAUDE.md: allowed for docs figure scripts).
"""
import argparse
import csv
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("svg")
import matplotlib.pyplot as plt  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
BENCH = REPO / "docs" / "benchmarks"

# Reference palette (dataviz skill), validated for these slots on both surfaces: slots 1-3 pass
# every adjacent gate; light aqua is below 3:1, so the segments it fills carry value labels.
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781", grid="#e1e0d9",
                  axis="#c3c2b7", s1="#2a78d6", s2="#eb6834", s3="#1baf7a", other="#c3c2b7"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781", grid="#2c2c2a",
                 axis="#383835", s1="#3987e5", s2="#d95926", s3="#199e70", other="#5f5e5a"),
}
SCENES = [("counter-full", "counter\n0.97 M"), ("truck-full", "truck\n1.65 M"),
          ("drjohnson-full", "drjohnson\n2.67 M"), ("bicycle-4m", "bicycle\n4.00 M"),
          ("stump-full", "stump\n4.05 M")]


def rows(path):
    with open(path, encoding="utf-8") as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def style(t):
    plt.rcParams.update({
        "svg.fonttype": "none", "font.family": ["Segoe UI", "Helvetica", "Arial", "DejaVu Sans"],
        "font.size": 10, "text.color": t["ink"], "axes.labelcolor": t["ink2"],
        "axes.edgecolor": t["axis"], "axes.facecolor": t["surface"], "figure.facecolor": t["surface"],
        "xtick.color": t["muted"], "ytick.color": t["muted"], "xtick.labelcolor": t["ink2"],
        "ytick.labelcolor": t["ink2"], "axes.titlesize": 11, "axes.titleweight": "bold",
        "axes.titlecolor": t["ink"], "axes.titlelocation": "left", "legend.frameon": False,
        "axes.spines.top": False, "axes.spines.right": False, "axes.grid": False,
        "grid.color": t["grid"], "grid.linewidth": 0.8,
    })


def recessive(ax, axis="x"):
    ax.grid(True, axis=axis)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)
    for side in ("left", "bottom"):
        ax.spines[side].set_linewidth(0.8)


# --- figures ------------------------------------------------------------------------------------

def cold_fps(t):
    data = rows(BENCH / "readme-cold-2026-10-31.csv")
    fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.4), sharey=True)
    for ax, device, title in zip(axes, ("s24u", "s25u"), ("Galaxy S24 Ultra (Adreno 750)", "Galaxy S25 Ultra (Adreno 830)")):
        means = [statistics.mean(float(r["fps"]) for r in data
                                 if r["device"] == device and r["scene"] == s and r["resolution"] == "864x1872")
                 for s, _ in SCENES]
        y = range(len(SCENES))
        ax.barh(y, means, height=0.55, color=t["s1"])
        for yi, m in zip(y, means):
            ax.text(m + 3, yi, f"{m:.0f}", va="center", color=t["ink"], fontsize=9)
        ax.set_yticks(list(y), [label for _, label in SCENES])
        ax.set_xlim(0, 240)
        ax.set_title(title)
        ax.set_xlabel("fps, cold (first 30 s)")
        recessive(ax)
    axes[0].invert_yaxis()  # shared y: once flips both panels
    fig.suptitle("PocketSplat frame rate, 864×1872 internal, uncapped orbit, on battery",
                 x=0.01, ha="left", fontsize=10, color=t["ink2"])
    fig.tight_layout()
    return fig


def supersplat_pose(t):
    picked = []
    for device, path in (("S24U", "pose-ab-s24u-fba401f.csv"), ("S25U", "pose-ab-s25u-f0f242c.csv")):
        for r in rows(BENCH / path):
            if r["renderer"].startswith("PocketSplat") or r["renderer"] == "supersplat-orbit-ourpose-touch":
                picked.append((device, r["scene"].split("-")[0], r["renderer"].startswith("PocketSplat"),
                               float(r["fps_steady"]), float(r["mcycles"])))
    groups = [(d, s) for d in ("S24U", "S25U") for s in ("stump", "truck")]
    fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.2))
    for ax, idx, title, label in ((axes[0], 3, "Frame rate (higher is better)", "fps"),
                                  (axes[1], 4, "GPU work per frame (lower is better)", "GPU Mcycles / frame")):
        for k, (ours, colour, name) in enumerate(((True, t["s1"], "PocketSplat"), (False, t["s2"], "SuperSplat (WebGPU)"))):
            vals = [next(p[idx] for p in picked if (p[0], p[1]) == g and p[2] == ours) for g in groups]
            ys = [i + (k - 0.5) * 0.36 for i in range(len(groups))]
            ax.barh(ys, vals, height=0.34, color=colour, label=name, edgecolor=t["surface"], linewidth=1)
            for yv, v in zip(ys, vals):
                ax.text(v * 1.015 + 0.5 * (idx == 3), yv, f"{v:.0f}" if idx == 3 else f"{v:.1f}", va="center",
                        fontsize=8.5, color=t["ink"])
        ax.set_yticks(range(len(groups)), [f"{d} {s}" for d, s in groups])
        ax.invert_yaxis()
        ax.set_title(title)
        ax.set_xlabel(label)
        ax.set_xlim(0, ax.get_xlim()[1] * 1.12)
        recessive(ax)
    fig.suptitle("Same phone, same splats, 540×975, SuperSplat moved to our bench camera",
                 x=0.01, ha="left", fontsize=10, color=t["ink2"])
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.legend(*axes[0].get_legend_handles_labels(), loc="upper right", ncol=2, fontsize=8.5)
    return fig


def cold_from_5s(name):
    """Mean of the 5-s buckets in the first 30 s: the published cold fps of bench_android.py runs."""
    buckets = [float(r["fps"]) for r in rows(BENCH / f"frame-s24u-{name}-5s.csv") if float(r["t_min_s"]) < 0.5]
    return statistics.mean(buckets)


def stump_history(t):
    steps = [
        ("baseline\n32-bit keys", cold_from_5s("86b7789-stump-full.sh0")),
        ("16-bit keys\n(ADR-0008)", cold_from_5s("86b7789-stump-full.sh0-16bit")),
        ("+ 0.1 px\ncontribution cull", cold_from_5s("86b7789-stump-full.sh0-16bit-c0.1")),
        ("sort rework\n(S16)", cold_from_5s("2002c7e-stump-full.sh0-16bit-c0.1")),
        ("one triangle\nper splat (ADR-0010)", statistics.mean(
            float(r["fps"]) for r in rows(BENCH / "readme-cold-2026-10-31.csv")
            if r["device"] == "s24u" and r["scene"] == "stump-full" and r["resolution"] == "864x1872")),
    ]
    fig, ax = plt.subplots(figsize=(8.4, 3.2))
    x = range(len(steps))
    vals = [v for _, v in steps]
    ax.bar(x, vals, width=0.5, color=t["s1"])
    for xi, v in zip(x, vals):
        ax.text(xi, v + 2, f"{v:.1f}", ha="center", color=t["ink"], fontsize=9.5)
    ax.set_xticks(list(x), [label for label, _ in steps], fontsize=9)
    ax.set_ylabel("fps, cold (first 30 s)")
    ax.set_ylim(0, 105)
    ax.set_title("Stump (4.05 M splats) on the Galaxy S24 Ultra, 864×1872: 34 → 92 fps")
    recessive(ax, "y")
    fig.tight_layout()
    return fig


def controller_ab(t):
    legs = {}
    for device in ("s24u", "s25u"):
        for mode in ("c0.1", "ctl"):
            stem = f"{device}-1d3faec-stump-full.sh0-16bit-{mode}-864x1872-cap60-triangle"
            thermal = rows(BENCH / f"thermal-{stem}.csv")
            frames = rows(BENCH / f"frame-{stem}-5s.csv")
            legs[device, mode] = dict(
                t=[float(r["t_s"]) / 60 for r in thermal], skin=[float(r["skin_c"]) for r in thermal],
                status=[int(r["thermal_status"]) for r in thermal],
                tf=[float(r["t_min_s"]) for r in frames], cull=[float(r["importance_threshold"]) for r in frames],
                fps=[float(r["fps"]) for r in frames])
    fig, axes = plt.subplots(3, 2, figsize=(8.4, 6.0), sharex=True, gridspec_kw=dict(height_ratios=[3, 1.4, 1.6]))
    series = (("c0.1", t["muted"], "fixed 0.1 px cull"), ("ctl", t["s1"], "thermal controller"))
    for col, (device, title) in enumerate((("s24u", "Galaxy S24 Ultra"), ("s25u", "Galaxy S25 Ultra"))):
        skin_ax, status_ax, cull_ax = axes[:, col]
        for mode, colour, name in series:
            leg = legs[device, mode]
            skin_ax.plot(leg["t"], leg["skin"], color=colour, linewidth=2, label=name)
            status_ax.step(leg["t"], leg["status"], where="post", color=colour, linewidth=2)
            cull_ax.step(leg["tf"], leg["cull"], where="post", color=colour, linewidth=2)
            skin_ax.text(leg["t"][-1] + 0.3, leg["skin"][-1], f"{leg['skin'][-1]:.1f} °C", va="center",
                         fontsize=8.5, color=t["ink"])
        skin_ax.set_title(title)
        skin_ax.set_ylabel("skin °C")
        status_ax.set_ylabel("thermal\nstatus")
        status_ax.set_yticks([0, 1, 2])
        status_ax.set_ylim(-0.3, 2.4)
        cull_ax.set_ylabel("cull px")
        cull_ax.set_yticks([0.1, 0.2, 0.5])
        cull_ax.set_ylim(0, 0.6)
        cull_ax.set_xlabel("minutes")
        cull_ax.set_xlim(0, 22.5)
        for ax in (skin_ax, status_ax, cull_ax):
            recessive(ax, "y")
    axes[0, 0].legend(loc="upper left", fontsize=8.5)
    summary = [r for r in rows(BENCH / "controller-ab-1d3faec-2026-10-29.csv")
               if r["protocol"] == "20min-equal-start" and r["window_s"] == "600-1200"]
    on_target = min(float(r["on_target_all"]) for r in summary)
    fig.suptitle("Stump 864×1872 capped at 60 fps on battery, equal cool starts (ADR-0009); "
                 f"every leg ≥ {100 * on_target:.1f} % of frames on time",
                 x=0.01, ha="left", fontsize=10, color=t["ink2"])
    fig.tight_layout()
    return fig


def frame_breakdown(t):
    bars = []
    for label, path in (("Adreno 750\n(S24U)", "copy-probe-s24u-2026-11-01.csv"),
                        ("Adreno 830\n(S25U)", "copy-probe-s25u-2026-10-31.csv")):
        ctl = [r for r in rows(BENCH / path) if r["draw_probe"] == "none"]
        med = {k: statistics.median(float(r[k]) for r in ctl) for k in ("gpu_ms", "preprocess_ms", "sort_ms", "draw_ms")}
        bars.append((label, "864×1872", med))
    rtx = next(r for r in rows(BENCH / "frame-pipeline-rtx3060-183a6cc-2026-11-01.csv")
               if r["scene"] == "stump-full" and r["view"] == "farc01" and r["chunk_cull"] == "on")
    bars.append(("RTX 3060\nlaptop", "1600×900", {k: float(rtx[k]) for k in ("gpu_ms", "preprocess_ms", "sort_ms", "draw_ms")}))
    parts = (("preprocess_ms", t["s1"], "preprocess"), ("sort_ms", t["s2"], "sort"), ("draw_ms", t["s3"], "draw"))
    fig, ax = plt.subplots(figsize=(8.4, 2.9))
    for i, (label, res, m) in enumerate(bars):
        left = 0.0
        segments = [(m[k], c, n) for k, c, n in parts] + [(m["gpu_ms"] - sum(m[k] for k, _, _ in parts), t["other"], "other")]
        for value, colour, name in segments:
            share = 100.0 * value / m["gpu_ms"]
            ax.barh(i, share, left=left, height=0.55, color=colour, edgecolor=t["surface"], linewidth=2,
                    label=name if i == 0 else None)
            if share >= 9:
                ink = "#0b0b0b" if colour == t["other"] and t is THEMES["light"] else "#ffffff"
                ax.text(left + share / 2, i, f"{value:.2f}", ha="center", va="center", fontsize=8.5, color=ink)
            left += share
        ax.text(101.5, i, f"{m['gpu_ms']:.2f} ms\n{res}", va="center", fontsize=8.5, color=t["ink"])
    ax.set_yticks(range(len(bars)), [b[0] for b in bars])
    ax.invert_yaxis()
    ax.set_xlim(0, 115)
    ax.set_xticks([0, 25, 50, 75, 100], ["0", "25", "50", "75", "100 %"])
    ax.set_title("Where a stump frame's GPU time goes (labels in ms; defaults: 16-bit keys, 0.1 px cull)")
    ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.45, -0.12), fontsize=8.5)
    recessive(ax)
    fig.tight_layout()
    return fig


FIGURES = {"cold-fps": cold_fps, "supersplat-pose": supersplat_pose, "stump-history": stump_history,
           "controller-ab": controller_ab, "frame-breakdown": frame_breakdown}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="docs/figures")
    parser.add_argument("--only", nargs="*", choices=sorted(FIGURES), help="subset of figures")
    parser.add_argument("--png", action="store_true", help="also write PNG copies (for a quick look)")
    args = parser.parse_args()
    out = REPO / args.out
    out.mkdir(parents=True, exist_ok=True)
    for name in args.only or FIGURES:
        for mode, theme in THEMES.items():
            style(theme)
            fig = FIGURES[name](theme)
            fig.savefig(out / f"{name}-{mode}.svg", metadata={"Date": None})
            if args.png:
                fig.savefig(out / f"{name}-{mode}.png", dpi=110)
            plt.close(fig)
        print(f"wrote {args.out}/{name}-light.svg, -dark.svg")


if __name__ == "__main__":
    main()
