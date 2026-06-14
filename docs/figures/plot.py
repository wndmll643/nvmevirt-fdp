#!/usr/bin/env python3
"""Generate the OP-sweep and skew-sweep WAF figures as PNGs.

Fallback for when the paper is not in LaTeX (otherwise prefer fdp_plots.tex,
which renders pgfplots directly from the CSVs). Requires matplotlib:

    pip install matplotlib
    python3 docs/figures/plot.py        # writes op_waf.png, skew_waf.png here

Reads ../data/op_summary.csv and ../data/skew_summary.csv.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")


def load(name):
    with open(os.path.join(DATA, name)) as f:
        return list(csv.DictReader(f))


def plot(rows, xkey, xlabel, title, out):
    x = [float(r[xkey]) for r in rows]
    base = [float(r["base_waf"]) for r in rows]
    fdp = [float(r["fdp_waf"]) for r in rows]
    plt.figure(figsize=(5, 3.2))
    plt.plot(x, base, "o-", label="Baseline")
    plt.plot(x, fdp, "s-", label="FDP")
    plt.xlabel(xlabel)
    plt.ylabel("Write amplification (WAF)")
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(HERE, out), dpi=150)
    print("wrote", out)


plot(load("op_summary.csv"), "working_set_pct",
     "Working set (% of device)", "WAF vs over-provisioning", "op_waf.png")
plot(load("skew_summary.csv"), "hot_write_pct",
     "Hot-write fraction (%)", "WAF vs workload skew", "skew_waf.png")
