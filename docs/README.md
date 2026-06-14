# docs/ — evaluation results for the term paper

Organized experimental results for the FDP-on-NVMeVirt project. Everything
needed to write (and reproduce) the evaluation section.

| File | What it is |
|------|------------|
| [`results.md`](results.md) | **Main writeup** — setup, methodology, all results, interpretation, limitations, reproduction. Start here. |
| [`tables.tex`](tables.tex) | LaTeX (booktabs) tables: OP sweep, skew sweep, headline. Paste-ready. |
| [`figures/fdp_plots.tex`](figures/fdp_plots.tex) | pgfplots figures that render directly from the CSVs (preferred for a LaTeX paper). |
| [`figures/plot.py`](figures/plot.py) | matplotlib fallback → PNGs (`pip install matplotlib` first). |
| [`data/sweep_results.csv`](data/sweep_results.csv) | Raw sweep data, one row per measurement (18 rows). |
| [`data/op_summary.csv`](data/op_summary.csv) | OP sweep, plot-ready (base/FDP WAF, reduction, endurance). |
| [`data/skew_summary.csv`](data/skew_summary.csv) | Skew sweep, plot-ready. |
| [`data/seed_summary.csv`](data/seed_summary.csv) | Seed-variance points. |
| [`data/single_point.csv`](data/single_point.csv) | Headline operating point, with byte-level ΔHBMW/ΔMBMW/MBE. |

## One-line result

FDP cuts write amplification by up to **~61%** (≈**2.5× flash endurance**) on a
skewed hot/cold workload, by separating data lifetimes into distinct reclaim
units — same device, same workload, placement directives the only difference.

## Regenerating the data

From the repo root: `./run_sweep.sh` (sweeps) and `./run_eval.sh` (single
point). Each prints results and captures them under `/tmp/`. Copy the
regenerated `sweep_results.csv` here to archive a fresh run; the summary CSVs
are derived from it (base/FDP pairs → reduction = (base−fdp)/base, endurance =
base/fdp).
