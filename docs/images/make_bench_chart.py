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

    keys = ["tetra3", "bindings"]
    names = ["ESA tetra3", "startracker-cpp"]
    dark = ["#9a9a9a", "#2a9d8f"]
    light = ["#cfcfcf", "#8fd1c7"]
    panels = [(d["solved"], "photos that solve"), (d["unsolved"], "photos with no match\n(searches everything: the worst case)")]
    with plt.xkcd(scale=1, length=120, randomness=2):
        if a.font:
            plt.rcParams["font.family"] = font_manager.FontProperties(fname=a.font).get_name()
        fig, axes = plt.subplots(1, 2, figsize=(12, 4.4))
        for ax, (s, title) in zip(axes, panels):
            cen = [s[f"{k}_centroid_ms"] for k in keys]
            sol = [s[f"{k}_solve_ms"] for k in keys]
            tot = [s[f"{k}_total_ms"] for k in keys]
            ax.barh([0, 1], cen, color=dark, edgecolor="black", height=0.6)
            ax.barh([0, 1], sol, left=cen, color=light, edgecolor="black", height=0.6)
            for i in range(2):
                label = f"{tot[i]:.1f} ms" + ("" if i == 0 else f"  ({tot[0] / tot[i]:.1f}x faster)")
                ax.text(cen[i] + sol[i] + tot[0] * 0.02, i, label, va="center", fontsize=12)
            ax.set_yticks([0, 1])
            ax.set_yticklabels(names)
            ax.set_xlim(0, tot[0] * 1.35)
            ax.set_xlabel("milliseconds per photo, start to finish")
            ax.set_title(f"{s['n']} {title}", fontsize=13)
            ax.invert_yaxis()
        fig.text(0.5, 0.86, "dark = finding the stars in the image,  light = identifying them (solving)",
                 ha="center", fontsize=11)
        fig.suptitle("same answers as ESA's tetra3 on 23 real night-sky photos, just faster", fontsize=15)
        fig.text(0.5, 0.005, f"median of {d['reps']} runs per photo, one thread each, same database; "
                 f"startracker-cpp called through its Python bindings; {d['host']}", ha="center", fontsize=9)
        fig.tight_layout(rect=(0, 0.03, 1, 0.86))
        fig.savefig(a.out, dpi=110)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
