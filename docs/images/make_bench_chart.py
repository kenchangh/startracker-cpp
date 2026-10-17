#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Draws docs/images/bench_vs_tetra3.png from bench_vs_tetra3.json (xkcd style).

  python3 docs/images/make_bench_chart.py [--font xkcd-script.ttf]
Needs matplotlib. The xkcd look uses any installed "xkcd Script" / "Humor Sans" font.
"""
import argparse
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=os.path.join(HERE, "bench_vs_tetra3.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "bench_vs_tetra3.png"))
    ap.add_argument("--font")
    a = ap.parse_args()
    d = json.load(open(a.json))
    if a.font:
        font_manager.fontManager.addfont(a.font)
        plt.rcParams["font.family"] = font_manager.FontProperties(fname=a.font).get_name()

    s, u = d["solved"], d["unsolved"]
    keys = ["tetra3", "bindings", "direct"]
    names = ["ESA tetra3\n(Python)", "startracker-cpp\n(Python bindings)", "startracker-cpp\n(C++ direct)"]
    dark = ["#9a9a9a", "#2a9d8f", "#e76f51"]
    light = ["#cfcfcf", "#8fd1c7", "#f4b29f"]
    with plt.xkcd(scale=1, length=120, randomness=2):
        if a.font:
            plt.rcParams["font.family"] = font_manager.FontProperties(fname=a.font).get_name()
        fig, axes = plt.subplots(1, 2, figsize=(12.5, 5.2), gridspec_kw={"width_ratios": [1.2, 1]})

        # Left: a full lost-in-space frame (find stars + solve) on photos that solve.
        ax = axes[0]
        cen = [s[f"{k}_centroid_ms"] for k in keys]
        sol = [s[f"{k}_solve_ms"] for k in keys]
        tot = [s[f"{k}_total_ms"] for k in keys]
        y = range(3)
        ax.barh(y, cen, color=dark, edgecolor="black")
        ax.barh(y, sol, left=cen, color=light, edgecolor="black")
        for i in y:
            label = f"{tot[i]:.1f} ms" + ("" if i == 0 else f"  ({tot[0] / tot[i]:.1f}x faster)")
            ax.text(cen[i] + sol[i] + 1, i, label, va="center", fontsize=12)
        ax.set_yticks(list(y))
        ax.set_yticklabels(names, fontsize=11)
        ax.set_xlim(0, tot[0] * 1.55)
        ax.set_xlabel("milliseconds per photo (less is better)")
        ax.set_title(f"one photo, start to finish\n({s['n']} real night-sky photos that solve)", fontsize=13)
        ax.text(tot[0] * 1.12, 1.55, "dark = finding stars\nlight = solving", fontsize=10)
        ax.invert_yaxis()

        # Right: the solver alone, for photos that solve and photos with no match.
        ax = axes[1]
        groups = ["solves", "no match\n(searches\neverything)"]
        h = 0.26
        for j, k in enumerate(keys):
            vals = [s[f"{k}_solve_ms"], u[f"{k}_solve_ms"]]
            pos = [g + (j - 1) * h for g in range(2)]
            ax.barh(pos, vals, height=h * 0.95, color=dark[j], edgecolor="black", label=names[j].replace("\n", " "))
            for g in range(2):
                ref = [s, u][g]["tetra3_solve_ms"]
                txt = f"{vals[g]:.1f} ms" if j == 0 else f"{vals[g]:.2f} ms ({ref / vals[g]:.0f}x)"
                ax.text(vals[g] + 1.5, pos[g], txt, va="center", fontsize=10)
        ax.set_yticks([0, 1])
        ax.set_yticklabels(groups)
        ax.set_xlim(0, u["tetra3_solve_ms"] * 1.5)
        ax.set_xlabel("milliseconds per solve")
        ax.set_title("just the solver", fontsize=13)
        ax.legend(loc="upper right", fontsize=9)
        ax.invert_yaxis()

        fig.suptitle("same answers as ESA's tetra3 on every photo, just faster", fontsize=16)
        fig.text(0.5, 0.005, f"median of {d['reps']} runs per photo, one thread each, same database; "
                 f"{d['host']}", ha="center", fontsize=9)
        fig.tight_layout(rect=(0, 0.03, 1, 0.95))
        fig.savefig(a.out, dpi=110)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
