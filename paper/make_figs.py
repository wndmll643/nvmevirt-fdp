#!/usr/bin/env python3
"""Generate vector (PDF) figures for the IEEE-format paper from the eval CSVs.

Outputs into the paper/ directory:
  fig_op.pdf        WAF vs over-provisioning (working set)
  fig_skew.pdf      WAF vs workload skew (hot-write fraction)
  fig_endurance.pdf endurance multiplier (= base_WAF / fdp_WAF) across the skew sweep
Reads ../docs/data/*.csv.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "docs", "data")

plt.rcParams.update({
    "font.size": 7,
    "axes.titlesize": 7,
    "axes.labelsize": 7,
    "legend.fontsize": 6.5,
    "xtick.labelsize": 6.5,
    "ytick.labelsize": 6.5,
    "figure.dpi": 200,
    "savefig.bbox": "tight",
    "pdf.fonttype": 42,
})

BASE_C = "#c0392b"
FDP_C = "#2471a3"


def load(name):
    with open(os.path.join(DATA, name)) as f:
        return list(csv.DictReader(f))


def plot_sweep(rows, xkey, xlabel, out, legend_loc="upper left", ymax_factor=1.18):
    x = [float(r[xkey]) for r in rows]
    base = [float(r["base_waf"]) for r in rows]
    fdp = [float(r["fdp_waf"]) for r in rows]
    fig, ax = plt.subplots(figsize=(3.0, 1.9))
    ax.plot(x, base, "o-", color=BASE_C, label="Baseline (no placement)", lw=1.3, ms=4)
    ax.plot(x, fdp, "s-", color=FDP_C, label="FDP (hot/cold split)", lw=1.3, ms=4)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Write amplification (WAF)")
    ax.grid(True, alpha=0.3)
    ax.axhline(1.0, color="gray", ls=":", lw=0.8)
    # headroom so the legend sits clear of the curves and the WAF=1 reference line
    ax.set_ylim(0, max(base) * ymax_factor)
    ax.legend(frameon=False, loc=legend_loc, handlelength=1.5,
              borderaxespad=0.3, labelspacing=0.25)
    fig.tight_layout(pad=0.3)
    fig.savefig(os.path.join(HERE, out))
    print("wrote", out)


def plot_endurance(out):
    rows = load("skew_summary.csv")
    x = [int(float(r["hot_write_pct"])) for r in rows]
    endur = [float(r["base_waf"]) / float(r["fdp_waf"]) for r in rows]
    fig, ax = plt.subplots(figsize=(3.0, 1.9))
    bars = ax.bar([str(v) for v in x], endur, color=FDP_C, width=0.6)
    ax.axhline(1.0, color="gray", ls=":", lw=0.8)
    ax.set_xlabel("Hot-write fraction (%) -- workload separability")
    ax.set_ylabel(r"Endurance gain ($\mathrm{WAF}_{b}/\mathrm{WAF}_{FDP}$)")
    ax.set_ylim(0, max(endur) * 1.28)
    for b, v in zip(bars, endur):
        ax.text(b.get_x() + b.get_width() / 2, v + 0.04, f"{v:.1f}x",
                ha="center", va="bottom", fontsize=6.5, color=FDP_C)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout(pad=0.3)
    fig.savefig(os.path.join(HERE, out))
    print("wrote", out)


def plot_lsm(out):
    """WAF vs number of RUHs on the RocksDB-like LSM workload (lsm_summary.csv:
    num_ruh,base_waf,fdp_waf,...). Baseline is flat (placement-agnostic); FDP
    falls as more LSM levels get their own reclaim unit."""
    path = os.path.join(DATA, "lsm_summary.csv")
    if not os.path.exists(path):
        print("skip", out, "(no lsm_summary.csv yet)")
        return
    rows = load("lsm_summary.csv")
    x = [int(float(r["num_ruh"])) for r in rows]
    fdp = [float(r["fdp_waf"]) for r in rows]
    base = float(rows[0]["base_waf"])
    fig, ax = plt.subplots(figsize=(3.0, 1.9))
    ax.axhline(base, color=BASE_C, ls="--", lw=1.3, label="Baseline (no placement)")
    ax.plot(x, fdp, "s-", color=FDP_C, lw=1.3, ms=4, label=r"FDP (level$\rightarrow$RUH)")
    ax.set_xlabel("Reclaim unit handles used (= LSM levels separated)")
    ax.set_ylabel("Write amplification (WAF)")
    ax.set_xticks(x)
    ax.set_ylim(0, base * 1.22)
    ax.grid(True, alpha=0.3)
    ax.legend(frameon=False, loc="lower left", handlelength=1.5,
              borderaxespad=0.3, labelspacing=0.25)
    fig.tight_layout(pad=0.3)
    fig.savefig(os.path.join(HERE, out))
    print("wrote", out)


plot_sweep(load("op_summary.csv"), "working_set_pct",
           "Working set (% of device)", "fig_op.pdf",
           legend_loc="upper left", ymax_factor=1.18)
plot_sweep(load("skew_summary.csv"), "hot_write_pct",
           "Hot-write fraction (%)", "fig_skew.pdf",
           legend_loc="upper right", ymax_factor=1.42)
plot_endurance("fig_endurance.pdf")
plot_lsm("fig_lsm_ruh.pdf")
