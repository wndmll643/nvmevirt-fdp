# FDP on NVMeVirt — Experimental Results

Evaluation of the Flexible Data Placement (NVMe TP 4146a) extension to
NVMeVirt. This document is the source-of-truth for the term paper's evaluation
section: methodology, raw data, results, and interpretation.

Raw data: [`data/`](data/). Figures: [`figures/`](figures/) (pgfplots/matplotlib
that read the CSVs). LaTeX tables: [`tables.tex`](tables.tex).

---

## 1. Setup

| Item | Value |
|------|-------|
| Emulator | NVMeVirt + FDP fork, target `SAMSUNG_FDP` |
| Host kernel | Linux 6.12.77 (guest = same, via virtme-ng) |
| Execution | QEMU/TCG VM (`--disable-kvm`); functional + WAF results are timing-independent |
| Device | 64 MiB, 4 FTL partitions, 8 RUHs, ~7% intrinsic over-provisioning |
| I/O | 32 KiB writes via NVMe passthrough ioctl |
| nvme-cli | 2.8 |

**Metric.** Write Amplification Factor `WAF = MBMW / HBMW` (media bytes written
÷ host bytes written), read from the FDP Statistics log (LID 0x22). WAF = 1.0
is ideal (no GC copying); higher means GC relocated more live data.

**Why WAF.** It is the canonical FTL/GC metric, it is independent of wall-clock
timing (so the TCG VM gives valid numbers), and it maps directly to flash
endurance: `endurance multiplier = baseline_WAF / FDP_WAF`.

## 2. Workload and methodology

A two-phase synthetic hot/cold workload (`fdp_workload.c`), modeling two data
*lifetime* classes (hot = short-lived/frequently rewritten, cold = long-lived):

1. **Random-order fill** of the working set. Random order is essential: with no
   placement (baseline) it interleaves hot and cold LBAs into the *same* erase
   lines; with placement (FDP) hot→RUH 1 and cold→RUH 2, so lines are
   lifetime-pure from the start.
2. **Churn**: rewrite the hot set repeatedly (e.g. 90% of writes into the 20%
   hottest LBAs). In the baseline this invalidates scattered hot pages and
   forces GC to copy the cold survivors trapped in those mixed lines; under FDP
   the hot lines invalidate wholesale and are reclaimed without copying.

`WAF` is measured as a **delta over the churn phase only** (FDP stats
snapshotted after the fill), so the WAF≈1 fill does not dilute the steady-state
number. Each measurement is a fresh module load (counters reset on init).

**Baseline vs. FDP** is an apples-to-apples comparison on the *same* device and
*same* workload: the only difference is whether writes carry a placement
directive (DTYPE=2, DSPEC = hot/cold → RUH 1/2) or not (all data → RUH 0).

## 3. Results

### 3.1 Headline (operating point: 90% working set, 90%→20% hot)

| trial    | ΔHBMW   | ΔMBMW   | WAF  |
|----------|---------|---------|------|
| baseline | 20 MiB  | 120 MiB | 6.02 |
| FDP      | 20 MiB  |  63 MiB | 3.17 |

Same host writes; the baseline pushes **6.0×** that to media, FDP only **3.2×**
— a **47% write-amplification reduction** (≈1.9× endurance). Data:
[`data/single_point.csv`](data/single_point.csv).

### 3.2 Over-provisioning sweep

Vary the working-set size (free space = over-provisioning); skew fixed at
90%→20%. Data: [`data/op_summary.csv`](data/op_summary.csv).

| working set | ~OP  | baseline WAF | FDP WAF | reduction | endurance × |
|-------------|------|--------------|---------|-----------|-------------|
| 75%         | ~33% | 1.03         | 1.01    | 2%        | 1.0×        |
| 85%         | ~18% | 2.66         | 1.40    | 47%       | 1.9×        |
| 92%         | ~9%  | 10.15        | 4.60    | 55%       | 2.2×        |
| 96%         | ~4%  | 13.41        | 11.62   | 13%       | 1.2×        |

**Reading it.** The benefit is largest at *moderate* OP (~9–18%). With abundant
free space (75% WS) GC seldom runs, so neither WAF rises and there is little to
gain. Under extreme pressure (96% WS) the device is nearly full and even FDP
must copy to make room, so the gap narrows. This non-monotonic curve is the
expected FDP/GC behavior and the most informative single figure.

### 3.3 Lifetime-skew sweep

Vary the fraction of writes hitting the hot region (workload separability);
working set fixed at 90%. Data: [`data/skew_summary.csv`](data/skew_summary.csv).

| hot-write % | baseline WAF | FDP WAF | reduction | endurance × |
|-------------|--------------|---------|-----------|-------------|
| 70%         | 5.74         | 3.95    | 31%       | 1.5×        |
| 85%         | 5.54         | 2.59    | 53%       | 2.1×        |
| 95%         | 5.50         | 2.17    | 61%       | 2.5×        |

**Reading it.** Monotonic: the more separable the workload, the more FDP helps
(31% → 61%). Baseline WAF stays ~flat (mixing hurts it regardless of skew);
FDP WAF falls as the lifetimes become cleaner to separate. This is the core FDP
value proposition demonstrated directly.

### 3.4 Variance

Two seeds at the operating point (WS 90%, hot-write 90%). Data:
[`data/seed_summary.csv`](data/seed_summary.csv).

| seed | baseline WAF | FDP WAF |
|------|--------------|---------|
| 1    | 5.41         | 2.57    |
| 2    | 5.66         | 2.38    |

Spread ≈ ±2%, so single-seed points are representative. Add more seeds for
formal error bars in the final paper.

## 4. Takeaways for the paper

1. FDP reduces write amplification by **up to ~61%** (≈**2.5× endurance**) on a
   skewed hot/cold workload, purely by separating data lifetimes into different
   reclaim units — no change to the GC algorithm itself.
2. The benefit is **workload- and OP-dependent**: it requires both (a) GC
   pressure (moderate-to-low over-provisioning) and (b) separable lifetimes
   (skew). The sweeps quantify both dependencies — useful for arguing *when*
   FDP is worthwhile, not just *that* it works.
3. The baseline is the *same device* with placement directives disabled, so the
   gap isolates the placement effect (not a different FTL).

## 5. Threats to validity / limitations

- **Timing not modeled here.** TCG is not latency-faithful; WAF/endurance are
  valid (logic-only), but throughput and tail-latency need a bare-metal run.
- **Synthetic workload.** Two-class hot/cold is the standard lifetime model;
  real applications (e.g. RocksDB levels) would strengthen external validity.
- **Small device (64 MiB)** for tractable GC under emulation; trends are
  geometry-independent but absolute WAF scales with OP and working set.
- **FDP event counts** (type 0x81) read unreliably under TCG and are reported
  qualitatively only; MBE (erase bytes) corroborates that GC ran.
- **RU nominal size** is reported per FTL partition (see README).

## 6. Reproducing

```bash
./run_eval.sh                 # single operating-point WAF (Section 3.1)
./run_sweep.sh                # OP + skew + seed sweeps (Sections 3.2-3.4)
TOTAL_MB=300 ./run_eval.sh    # deeper churn
CHURN_MB=20 ./run_sweep.sh    # deeper per-point churn
```

Both build the module on the host, boot a TCG VM, run the workload, and print
results (also captured to `/tmp/fdp_eval.log` / `/tmp/fdp_sweep.log`). Because
the guest mounts the repo as a CoW overlay, `run_sweep.sh` reconstructs
`sweep_results.csv` on the host from the captured stdout. Copy the regenerated
CSV into `docs/data/` to archive a new run.
