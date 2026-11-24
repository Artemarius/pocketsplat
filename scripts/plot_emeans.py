#!/usr/bin/env python3
"""Figures for docs/e-means-vs-lloyd.md from the committed Phase 8 trainer CSVs, light and dark SVG.

    python scripts/plot_emeans.py [--out docs/figures]

writes <name>-light.svg and <name>-dark.svg:

    emeans-convergence  palette loss against training work, e-means vs Lloyd, six scenes, k 4,096,
                        mean of 5 seeds with the seed range (palette-trainers-1cdd4cc-2026-11-19.csv)
    emeans-phone-time   S24 Ultra wall time against loss on hornedlizard, every trainer setting of the
                        S30 phone run (palette-trainers-s24u-e87cf48-2026-11-20.csv)
    emeans-dead         palette entries no splat uses, hornedlizard, k 1,024 / 4,096 / 16,384
                        (palette-trainers-1cdd4cc-2026-11-19.csv)

Loss is the mean squared distance of every splat's SH1 vector to its palette entry, relative to
Lloyd with 25 epochs and the same seed. Lloyd's rows are the Hamerly runs, which take Lloyd's
iterations exactly (same labels, same loss) with fewer distance evaluations; work is counted as
plain Lloyd's n x k x epochs. matplotlib + numpy (CLAUDE.md: allowed for docs figure scripts).
"""
import argparse
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("svg")
import matplotlib.pyplot as plt  # noqa: E402

from plot_readme import BENCH, REPO, THEMES, recessive, rows, style  # noqa: E402

DESKTOP = BENCH / "palette-trainers-1cdd4cc-2026-11-19.csv"
PHONE = BENCH / "palette-trainers-s24u-e87cf48-2026-11-20.csv"
SCENES = [("museum1.sh1", "museum1, 0.31 M"), ("hornedlizard.sh1", "hornedlizard, 0.76 M"),
          ("beetle.sh1", "beetle, 1.68 M"), ("truck.sh1", "truck, 1.65 M"),
          ("giraffes_statue.sh1", "giraffes_statue, 2.88 M"), ("stump.sh1", "stump, 4.05 M")]


def desktop_runs(k, cap="0"):
    """{(scene, trainer, epochs): [(loss vs Lloyd-25, work vs Lloyd-25, dead), ...]} over seeds."""
    data = [r for r in rows(DESKTOP) if r["k"] == str(k) and r["cap_per_entry"] == cap]
    ref = {(r["input"], r["seed"]): float(r["mse"]) for r in rows(DESKTOP)
           if r["trainer"] == "hamerly" and r["epochs"] == "25" and r["k"] == str(k) and r["cap_per_entry"] == "0"}
    out = defaultdict(list)
    for r in data:
        n, epochs = int(r["count"]), int(r["epochs"])
        lloyd25 = n * k * 25
        work = (n * k * epochs if r["trainer"] == "hamerly" else int(r["pairs"])) / lloyd25
        trainer = "lloyd" if r["trainer"] == "hamerly" else r["trainer"]
        out[(r["input"], trainer, epochs)].append((float(r["mse"]) / ref[(r["input"], r["seed"])], work,
                                                   int(r["dead"])))
    return out


def convergence(t):
    runs = desktop_runs(4096)
    fig, axes = plt.subplots(2, 3, figsize=(8.6, 5.2), sharex=True, sharey=True)
    for ax, (scene, title) in zip(axes.flat, SCENES):
        ax.axhline(1.0, color=t["axis"], linewidth=0.8, zorder=1)
        for trainer, epochs, colour, name in (("lloyd", (3, 10, 25), t["s1"], "Lloyd"),
                                              ("emeans", (1, 2, 3, 5), t["s2"], "e-means")):
            xs, ys, lo, hi = [], [], [], []
            for e in epochs:
                v = runs[(scene, trainer, e)]
                losses = [x[0] for x in v]
                xs.append(statistics.mean(x[1] for x in v))
                ys.append(statistics.mean(losses))
                lo.append(ys[-1] - min(losses))
                hi.append(max(losses) - ys[-1])
            ax.errorbar(xs, ys, yerr=[lo, hi], color=colour, linewidth=2, marker="o", markersize=5,
                        markeredgecolor=t["surface"], markeredgewidth=1.5, capsize=0, elinewidth=1,
                        label=name, zorder=3)
            if trainer == "emeans":  # the shipped setting
                i = epochs.index(3)
                ax.annotate("3 epochs", (xs[i], ys[i]), xytext=(4, -12), textcoords="offset points",
                            fontsize=8, color=t["ink2"])
        ax.set_xscale("log")
        ax.set_xticks([0.04, 0.12, 0.4, 1.0], ["4 %", "12 %", "40 %", "100 %"])
        ax.minorticks_off()
        ax.set_ylim(0.85, 1.16)
        ax.set_title(title, fontsize=10)
        recessive(ax, "y")
    for ax in axes[1]:
        ax.set_xlabel("training work, share of Lloyd-25")
    for ax in axes[:, 0]:
        ax.set_ylabel("loss vs Lloyd-25")
    fig.suptitle("Palette loss against training work, k 4,096 (mean of 5 seeds, bars: seed range)",
                 x=0.01, ha="left", fontsize=10, color=t["ink2"])
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.legend(*axes[0, 0].get_legend_handles_labels(), loc="upper right", ncol=2, fontsize=9)
    return fig


def phone_time(t):
    data = [r for r in rows(PHONE) if r["input"] == "hornedlizard.sh1"]
    ref = next(float(r["mse"]) for r in data if r["trainer"] == "lloyd" and r["epochs"] == "25")
    fig, ax = plt.subplots(figsize=(8.0, 3.6))
    ax.axhline(1.0, color=t["axis"], linewidth=0.8, zorder=1)
    for r in data:
        cap = r["cap_per_entry"] != "0"
        family = "e-means" if r["trainer"] == "emeans" else "Lloyd"
        name = {"emeans": "e-means", "lloyd": "Lloyd", "hamerly": "Hamerly"}[r["trainer"]]
        label = f"{name}-{r['epochs']}" + (", cap 64" if cap else "")
        x, y = float(r["wall_ms"]) / 1000.0, float(r["mse"]) / ref
        ax.scatter([x], [y], s=60, color=t["s2"] if family == "e-means" else t["s1"],
                   edgecolors=t["surface"], linewidths=2, zorder=3,
                   label=family if label in ("e-means-3", "Lloyd-25") else None)
        dy = {"e-means-5": 8, "e-means-3": -14, "Hamerly-25": -14}.get(label, 8)
        ax.annotate(f"{label}  {x:.1f} s", (x, y), xytext=(7, dy), textcoords="offset points", fontsize=8.5,
                    color=t["ink"])
    ax.set_xlim(0, 100)
    ax.set_ylim(0.84, 1.12)
    ax.set_xlabel("wall time on the Galaxy S24 Ultra, s (training + final assignment, 8 threads)")
    ax.set_ylabel("loss vs Lloyd-25")
    recessive(ax, "both")
    ax.set_title("hornedlizard (765 K splats), k 4,096, scalar kernel (S30)", fontsize=10)
    ax.legend(loc="upper right", fontsize=9)
    fig.tight_layout()
    return fig


def dead(t):
    groups = [(1024, "k 1,024"), (4096, "k 4,096"), (16384, "k 16,384")]
    vals = {}
    for k, _ in groups:
        runs = desktop_runs(k)
        for trainer, epochs in (("lloyd", 25), ("emeans", 3)):
            vals[(k, trainer)] = statistics.mean(x[2] for x in runs[("hornedlizard.sh1", trainer, epochs)])
    fig, ax = plt.subplots(figsize=(8.0, 2.8))
    for j, (trainer, colour, name) in enumerate((("lloyd", t["s1"], "Lloyd-25"), ("emeans", t["s2"], "e-means-3"))):
        ys = [i + (j - 0.5) * 0.36 for i in range(len(groups))]
        xs = [vals[(k, trainer)] for k, _ in groups]
        ax.barh(ys, xs, height=0.34, color=colour, label=name, edgecolor=t["surface"], linewidth=2)
        for (k, _), yv, v in zip(groups, ys, xs):
            ax.text(v + 30, yv, f"{v:,.0f}  ({100 * v / k:.1f} % of k)" if v >= 0.5 else "0", va="center",
                    fontsize=8.5, color=t["ink"])
    ax.set_yticks(range(len(groups)), [g for _, g in groups])
    ax.invert_yaxis()
    ax.set_xlim(0, 3600)
    ax.set_xlabel("palette entries no splat uses (k 4,096: mean of 5 seeds; others: seed 1)")
    recessive(ax)
    ax.set_title("hornedlizard: an SPZ import with 106 K distinct SH1 vectors among 765 K splats", fontsize=10)
    ax.legend(loc="center right", fontsize=9)
    fig.tight_layout()
    return fig


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "figures")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, make in (("emeans-convergence", convergence), ("emeans-phone-time", phone_time), ("emeans-dead", dead)):
        for theme, t in THEMES.items():
            style(t)
            fig = make(t)
            fig.savefig(args.out / f"{name}-{theme}.svg", metadata={"Date": None})
            plt.close(fig)
            print(args.out / f"{name}-{theme}.svg")


if __name__ == "__main__":
    main()
