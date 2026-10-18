#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Draws docs/images/bench_vs_tetra3.png from bench_vs_tetra3.json (xkcd style).

Top row: ESA tetra3 vs startracker-cpp (ESA database). Bottom row: cedar-solve vs
startracker-cpp with variant="cedar" (cedar database). The bottom-right panel shows the photos
where cedar-solve stopped at its 5 s timeout, so its bar is a lower bound.

  python3 docs/images/make_bench_chart.py [--font xkcd-script.ttf]
Needs matplotlib. The xkcd look uses any installed "xkcd Script" / "Humor Sans" font.
"""
import argparse
import json
import os
import statistics as stt

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))


def timeout_summary(rows):
    """Medians over the photos where the reference library hit its solve timeout."""
    out = dict(n=len(rows))
    for p in ("ref", "ours"):
        for k in ("centroid", "solve"):
            out[f"{p}_{k}_ms"] = stt.median(r[f"{p}_{k}_ms"] for r in rows)
        out[f"{p}_total_ms"] = stt.median(r[f"{p}_centroid_ms"] + r[f"{p}_solve_ms"] for r in rows)
    return out


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

    dark = ["#9a9a9a", "#2a9d8f"]
    light = ["#cfcfcf", "#8fd1c7"]
    pairs = [("esa", "ESA tetra3", "ESA database"), ("cedar", "cedar-solve", "cedar-solve database")]
    with plt.xkcd(scale=1, length=120, randomness=2):
        if a.font:
            plt.rcParams["font.family"] = font_manager.FontProperties(fname=a.font).get_name()
        fig, axes = plt.subplots(2, 2, figsize=(12, 8.2))
        for row, (key, ref, dbname) in zip(axes, pairs):
            names = [ref, "startracker-cpp"]
            panels = [(d[key]["solved"], "photos that solve", False)]
            if d[key].get("timeouts"):
                panels.append((timeout_summary(d[key]["timeouts"]),
                               f"photos with no match\n({ref} stopped at its 5 s timeout)", True))
            else:
                panels.append((d[key]["unsolved"], "photos with no match\n(searches everything: the worst case)",
                               False))
            for ax, (s, title, capped) in zip(row, panels):
                cen = [s[f"{k}_centroid_ms"] for k in ("ref", "ours")]
                sol = [s[f"{k}_solve_ms"] for k in ("ref", "ours")]
                tot = [s[f"{k}_total_ms"] for k in ("ref", "ours")]
                ax.barh([0, 1], cen, color=dark, edgecolor="black", height=0.6)
                ax.barh([0, 1], sol, left=cen, color=light, edgecolor="black", height=0.6)
                for i in range(2):
                    if capped:
                        label = (f"{tot[i]:.0f}+ ms (gave up)" if i == 0 else
                                 f"{tot[i]:.1f} ms, finished  (at least {tot[0] / tot[i]:.0f}x faster)")
                    else:
                        label = f"{tot[i]:.1f} ms" + ("" if i == 0 else f"  ({tot[0] / tot[i]:.1f}x faster)")
                    ax.text(cen[i] + sol[i] + tot[0] * 0.02, i, label, va="center", fontsize=12)
                ax.set_yticks([0, 1])
                ax.set_yticklabels(names)
                ax.set_xlim(0, tot[0] * 1.35)
                ax.set_xlabel("milliseconds per photo, start to finish")
                ax.set_title(f"{dbname}: {s['n']} {title}", fontsize=13)
                ax.invert_yaxis()
        fig.text(0.5, 0.925, "dark = finding the stars in the image,  light = identifying them (solving)",
                 ha="center", fontsize=11)
        fig.suptitle("same answers as ESA's tetra3 and cedar-solve on 23 real night-sky photos, just faster",
                     fontsize=15)
        fig.text(0.5, 0.005, f"median of {d['reps']} runs per photo, one thread each, same database per pair; "
                 f"startracker-cpp called through its Python bindings; {d['host']}", ha="center", fontsize=9)
        fig.tight_layout(rect=(0, 0.02, 1, 0.92), h_pad=2.5)
        fig.savefig(a.out, dpi=110)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
