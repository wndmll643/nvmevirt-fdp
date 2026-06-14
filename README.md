# NVMeVirt-FDP — Flexible Data Placement for NVMeVirt

> **This is a fork of [NVMeVirt](https://github.com/snu-csl/nvmevirt)** (SNU-CSL),
> a software-defined virtual NVMe device implemented as a Linux kernel module.
> It adds support for **Flexible Data Placement (FDP, NVMe TP 4146a)** on top of
> the upstream conventional-SSD path (`conv_ftl`). The original NVMeVirt design,
> build system, and non-FDP device targets (NVM / conventional SSD / ZNS / KV)
> are unchanged; see the upstream repository and the FAST '23 paper for the base
> emulator. This document covers only the FDP additions.

FDP lets the host hint *where* data should be placed (by lifetime/stream) via
per-write placement directives, so a flash device can co-locate data with
similar lifetimes in the same reclaim units and lower garbage-collection write
amplification. This fork emulates such a device.

## Status

| Phase | What it adds                                              | State |
|-------|-----------------------------------------------------------|-------|
| 1     | Build scaffold and FDP FTL backend (clone of `conv_ftl`)  | done  |
| 2     | FDP capability advertisement in Identify                  | done  |
| 3     | FDP log pages (configs, RUH usage, stats, events) + features | done |
| 4     | Per-RUH placement: writes routed by DTYPE=Placement + DSPEC | done |
| 5     | FDP event ring + statistics lifecycle                     | done |
| 6     | Integration tests + WAF evaluation                        | done (VM): 47% WAF reduction; sweeps/bare-metal pending |

After phases 1–5 the device is a functional FDP SSD: placement-directive
writes land in per-RUH reclaim units, non-directive writes use RUH 0, invalid
placement IDs are rejected (and logged as FDP events), GC reclaims emit
implicitly-modified-RU events, and the logs/features report live state.

### Validation status

Functional path validated end-to-end in a virtme-ng VM (TCG, host kernel
6.12.77, nvme-cli 2.8) via `run_vm_test.sh`:
- Phase 1: 256 KiB write/read round-trip matches.
- Phase 2: `CTRATT=0x80010` (FDPS + ENDGRPS), `nsfeat` bit 4, `npwg=npwa=nows=63`,
  `endgid=1`.
- Phase 3: all four logs + features return correct data; HBMW/MBMW counters
  track host writes live.
- Phase 4: `--dir-type=2 --dir-spec=N` marks RUH N host-specified; non-placement
  writes mark RUH 0 controller-specified; `dspec=99` rejected with Invalid Field.
- Phase 5: invalid-PID (type 0x3) event captured with correct PID and NSID.

Phase 6 evaluation (below) then drove the device into steady-state GC and
confirmed the remaining paths: the `gc_write_page` MBMW bump and `mark_block_free`
MBE bump are exercised (WAF rises to ~6× under churn), and FDP placement cuts
WAF by ~47% vs. the baseline. The type-0x81 event count reads reliably only
intermittently under TCG (see Phase 6 "Further work").

Known cosmetic/semantic items:
- RU Nominal Size is reported as one FTL partition's line size; the
  host-visible reclaim unit striped across `SSD_PARTITIONS` is arguably 4x that.
- Event timestamps use `local_clock()` (ns since boot), so `nvme fdp events`
  shows a nonsensical wall-clock date. Cosmetic.

VM gotchas worth remembering (encoded in `run_vm_test.sh`):
- Reserve memory ABOVE 4 GiB; [3,4) GiB is the PCI MMIO hole, not DRAM, and
  reserving it leaves the BAR unbacked → controller never becomes ready.
- `vng` runs qemu via `shell=True`, so `memmap=1G$4G` needs the `$` backslash-
  escaped to survive that extra shell.
- Use `--disable-kvm`; KVM is only needed for realistic timing (bare metal).

## Phase 1 — build scaffold

- New build target `CONFIG_NVMEVIRT_FDP := y` in `Kbuild`, gated by
  `-DBASE_SSD=SAMSUNG_FDP`.
- New SSD model `SAMSUNG_FDP` (= 5) and namespace type `SSD_TYPE_FDP` (= 4) in
  `ssd_config.h`. The geometry block mirrors `SAMSUNG_970PRO` (8 channels,
  2 LUNs/ch, 32 KiB flash page) and adds three FDP knobs:
  - `FDP_NR_RG = 1`     — reclaim groups
  - `FDP_NR_RUH = 8`    — reclaim unit handles per namespace
  - `FDP_NR_EVENTS = 63`— events log ring depth
- `fdp_ftl.{c,h}` — forked from `conv_ftl.{c,h}` with `fdp_`-prefixed structs
  and functions. Forked rather than refactored because phase 4 diverges
  significantly (per-RUH write pointers) and the original `conv_ftl` should
  stay stable.
- `main.c` and `nvmev.h` wire dispatch branches for `SSD_TYPE_FDP` in
  `NVMEV_NAMESPACE_INIT` / `_FINAL` and add a label in `__print_base_config`.

The struct-name prefixing (`struct fdp_line`, `struct fdp_write_pointer`,
`struct fdp_line_mgmt`, `struct fdp_write_flow_control`, `struct fdp_ftl`,
`struct fdpparams`) is needed because `main.c` includes both `conv_ftl.h` and
`fdp_ftl.h` — they coexist at compile time even though only one FTL is active
at runtime.

## Phase 2 — Identify advertisement

- `nvme_fdp.h` (new) defines `NVME_CTRL_CTRATT_FDPS = 1 << 19` (Flexible Data
  Placement Supported), `NVME_CTRL_CTRATT_ENDGRPS = 1 << 4` (Endurance Groups
  supported — FDP operates on endurance groups, so both are advertised
  together), `NVME_NS_FEAT_IO_OPT = 1 << 4` (NSFEAT bit gating the NPWG/NPWA/
  NOWS fields), and `NVMEV_FDP_ENDGID = 1` (single endurance group).
- `nvme.h`: the legacy `rsvd64[40]` gap in `struct nvme_id_ns` is sliced into
  the NVMe 2.0 fields it actually represents: `npwg`, `npwa`, `npdg`, `npda`,
  `nows`, `mssrl`, `mcl`, `msrc`, `anagrpid`, `nsattr`, `nvmsetid`, `endgid`.
  Byte layout and total struct size unchanged — additive only, so non-FDP
  targets (NVM / CONV / ZNS / KV) keep zero-filling those bytes and see no
  behavior change.
- `admin.c`:
  - Identify Controller (CNS 0x01): sets `ctratt |= ENDGRPS | FDPS` under
    `#if SUPPORTED_SSD_TYPE(FDP)`.
  - Identify Namespace (CNS 0x00): for `SSD_TYPE_FDP` namespaces, sets
    `nsfeat` bit 4 (required for hosts to honor the I/O-optimization fields),
    `endgid = 1`, and `npwg = npwa = nows = (ONESHOT_PAGE_SIZE/LBA_SIZE) - 1`
    (= 63 LBAs, i.e. 32 KiB preferred write granularity).

## Phase 3 — log pages + features

All endurance-group state lives in a private singleton `fdp_ctx` in
`fdp_ftl.c` (admin and I/O paths both run on the dispatcher thread, so it is
lock-free by construction). Wire-format structs are in `nvme_fdp.h`; admin.c
dispatches into `fdp_get_log_page` / `fdp_{get,set}_feature_events` under
`#if SUPPORTED_SSD_TYPE(FDP)`.

- LID 0x20 FDP Configurations: one configuration — `nrg`, `nruh`, `maxpids`,
  `runs` (= one FTL line: `pgs_per_line * pgsz`), RUHs typed Initially
  Isolated, FDPA valid bit + RGIF=0 (placement ID is the RUH index).
- LID 0x21 RUH Usage: per-RUH attribute byte (all "unused" until Phase 4
  starts marking them host-specified).
- LID 0x22 FDP Statistics: HBMW / MBMW / MBE 128-bit counters, fed by live
  hooks — host writes bump HBMW+MBMW in `fdp_write`, GC page copies bump MBMW
  in `gc_write_page`, block erases bump MBE in `mark_block_free`.
  `WAF ≈ MBMW / HBMW` is observable under workload.
- LID 0x23 FDP Events: served from the Phase 5 ring.
- FID 0x1D (FDP): Get returns FDPE=1/FDPCI=0; Set allows an idempotent
  re-enable and otherwise fails with Feature Not Changeable.
- FID 0x1E (FDP Events): Get/Set exchange `{evt, evta}` descriptors for the
  supported host event types.

Caveat: log transfers are single-PRP (≤ 4 KiB), matching the existing
NVMeVirt log-page limitation; our logs all fit in one page.

## Phase 4 — per-RUH placement writes

- `struct fdp_ftl` carries `wp_ruh[FDP_NR_RUH]` — one open line per RUH per
  FTL partition — plus the GC write pointer. With `SSD_PARTITIONS = 4` and
  8 RUHs that is 36 open lines device-wide; `gc_thres_lines` was raised to
  `FDP_NR_RUH + 1` accordingly.
- `fdp_write` parses DTYPE (CDW12 bits 23:20 = `control` bits 7:4) and DSPEC
  (CDW13 bits 31:16 = `dsmgmt >> 16`). DTYPE = 2 (placement) routes the write
  to `wp_ruh[dspec]` and marks that RUH host-specified in the usage log;
  any other DTYPE routes to RUH 0 (marked controller-specified on first use).
  Since RGIF = 0, DSPEC is the bare RUH index.
- DSPEC ≥ `FDP_NR_RUH` fails the write with Invalid Field in Command, per
  TP 4146a, and queues an Invalid-PID event (Phase 5).
- GC is RUH-agnostic: victim lines come from the shared pool regardless of
  which RUH originally filled them, and GC writes use the dedicated GC
  write pointer. LPN striping across the 4 partitions is unchanged — a
  placement write touches the same RUH index in each partition it stripes
  over.

## Phase 5 — FDP events

A 63-record ring of `struct nvme_fdp_event` lives in `fdp_ctx`, overwriting
oldest-first and served most-recent-first through LID 0x23 (64-byte header +
63 × 64-byte records = exactly one page).

Emission points:
- **Invalid Placement Identifier (0x3)** — a placement write whose DSPEC is
  out of range fails with Invalid Field and queues this event with the
  offending PID and the namespace ID.
- **Implicitly Modified RU (0x81)** — `struct fdp_line` tracks which RUH each
  line is open for (`-1` for GC/free lines); when GC selects a victim line
  that the host wrote through an RUH, this event is queued with that PID
  before relocation starts. Data moved by GC lands on RUH-less lines, so a
  later reclaim of those does not re-trigger the event.
- **RU Not Fully Written (0x0)** is advertised as supported but never fires:
  this FTL only rotates a line once it is completely written.

All emissions honor the FID 0x1E enable mask, and all event types default to
disabled per spec — `nvme fdp events` stays empty until the host enables
event types via Set Features 0x1E. Timestamps are `local_clock()` nanoseconds.

## Files

```
modified from upstream:
  Kbuild           # +CONFIG_NVMEVIRT_FDP target
  admin.c          # +FDPS in id-ctrl, +FDP fields in id-ns, +FDP logs/features
  main.c           # +fdp_init/remove_namespace dispatch
  nvme.h           # rsvd64[40] sliced into NVMe 2.0 fields
  nvmev.h          # (local edits)
  ssd_config.h     # +SAMSUNG_FDP model, +SSD_TYPE_FDP, +FDP knobs

added:
  fdp_ftl.c        # FDP FTL backend (fork of conv_ftl) + fdp_ctx, logs, events
  fdp_ftl.h        # types & API
  nvme_fdp.h       # FDP-specific NVMe constants and wire-format structs
  run_vm_test.sh   # host launcher: build + boot virtme-ng VM + functional test
  fdp_vm_test.sh   # in-guest functional test (phases 1-5)
  run_eval.sh      # host launcher for the WAF evaluation
  fdp_eval.sh      # in-guest evaluation: baseline vs FDP, computes WAF
  fdp_workload.c   # synthetic hot/cold workload generator (NVMe passthrough)
  README.md        # this file
```

## Build

The `Kbuild` ships with `CONFIG_NVMEVIRT_FDP := y` active. Just:

```bash
make clean && make
```

Produces `nvmev.ko` linking `fdp_ftl.o`, `ssd.o`, `pqueue/pqueue.o`,
`channel_model.o`.

## Test

Recommended: a virtme-ng VM, so a misbehaving build can only crash the guest.
One-time host setup:

```bash
sudo apt install virtme-ng qemu-system-x86 nvme-cli
sudo chmod a+r /boot/vmlinuz-$(uname -r)
```

Then run the full functional suite (builds on the host, boots a VM under TCG,
runs phases 1–5 in the guest, captures output to `/tmp/fdp_vm.log`):

```bash
./run_vm_test.sh
```

`fdp_vm_test.sh` is the in-guest script; it loads `nvme`, insmods the module
with a memmap above 4 GiB, then walks each phase. Expected results are recorded
under "Validation status" above. The individual `nvme` commands per phase
(id-ctrl, id-ns, `nvme fdp configs/usage/stats/events`, `nvme write
--dir-type/--dir-spec`, `nvme fdp set-events`) are spelled out in that script.

## Phase 6 — evaluation (WAF)

> Organized results for the term paper live in [`docs/`](docs/) — see
> [`docs/results.md`](docs/results.md) for the full writeup, `docs/data/` for raw
> CSVs, `docs/tables.tex` for paste-ready LaTeX tables, and `docs/figures/` for
> pgfplots/matplotlib figures. This section is the summary.

The quantitative result for an FDP device is the **Write Amplification Factor**,
`WAF = MBMW / HBMW`, read straight from the FDP Statistics log. The evaluation
compares a lifetime-separated placement workload against a baseline that mixes
all data in one reclaim unit, under sustained writes that drive the device into
steady-state garbage collection:

- **Baseline:** hot and cold data written without placement directives (all to
  RUH 0) — GC repeatedly copies surviving cold pages out of hot-dominated units.
- **FDP:** hot data → one RUH, cold data → another — units fill with
  same-lifetime data and are reclaimed wholesale, so GC copies less.

### Methodology

Each trial loads the module fresh (FDP stats reset on init) and runs two phases:

1. **Random-order fill** of the whole device. Random order is essential: with
   no placement (baseline) it scatters hot and cold LBAs into the *same* erase
   lines, so lifetimes are mixed; with placement (FDP) the fill already routes
   hot→RUH 1 and cold→RUH 2, so lines are lifetime-pure.
2. **Churn** — rewrite the hot set repeatedly (default: 90% of writes into the
   20% hottest LBA space). In the baseline, invalidating scattered hot pages
   forces GC to copy the cold survivors stuck in those lines; under FDP, hot
   lines invalidate wholesale and are reclaimed without copying.

`WAF = ΔMBMW / ΔHBMW` is measured as a **delta over the churn phase only** (FDP
stats snapshotted after the fill), so the WAF≈1 fill does not dilute the
steady-state number. Secondary metric: media bytes erased (MBE).

### Running it

```bash
./run_eval.sh                 # default churn 150 MiB per trial
TOTAL_MB=300 ./run_eval.sh    # deeper steady state, slower
```

`run_eval.sh` builds the module on the host and boots a TCG VM that runs
`fdp_eval.sh`, which builds `fdp_workload.c`, then for each trial does
fill → snapshot → churn → snapshot and computes WAF. **Slow** — hundreds of MiB
through an emulated device under TCG takes minutes to tens of minutes; output
is captured to `/tmp/fdp_eval.log`. `fdp_workload.c` issues 32 KiB writes via
the NVMe passthrough ioctl; in `fdp` mode it tags hot→PID 1, cold→PID 2
(DTYPE=2), in `base` mode it sends no directive (all RUH 0).

Implementation notes baked into the harness:
- **Device sizing for GC.** A small device (`memmap_size=64M`) with ~7% OP, so
  GC reaches steady state at a tractable write volume. This needs
  `BLKS_PER_PLN` small enough (128 in `ssd_config.h`) that the block size
  scales with capacity instead of being floored at `ONESHOT_PAGE_SIZE` —
  otherwise physical capacity inflates ~4× and GC never runs.
- **No `awk` in the guest.** The 9p-mounted `awk` intermittently fails to load
  under TCG; parsing uses `grep` + bash integer arithmetic instead.

### Results (validation run)

A short run (churn 20 MiB, 64 MiB device, 90% of writes into the 20% hot space,
under TCG) gives:

| trial    | ΔHBMW (B) | ΔMBMW (B)  | MBE (B)    |  WAF  |
|----------|-----------|------------|------------|-------|
| baseline | 20971520  | 126287872  | 123731968  | 6.02  |
| fdp      | 20971520  |  66486272  |  63438848  | 3.17  |

**→ 47% write-amplification reduction with FDP.** Same host write volume in
both trials (ΔHBMW equal); the baseline writes 6.0× that to media because GC
copies cold data co-located with churned hot data, while FDP separates the two
lifetimes and writes only 3.2×.

### Sweep results

`run_sweep.sh` runs three VM-feasible sweeps (64 MiB device, 12 MiB churn per
point; raw data in `sweep_results.csv`). Endurance multiplier = baseline WAF /
FDP WAF (lower media wear → proportionally longer flash life).

**Over-provisioning** (working-set % of device; OP ≈ free space). Skew fixed
(90% of writes → 20% hot space), seed 1:

| working set | ~OP   | baseline WAF | FDP WAF | reduction | endurance × |
|-------------|-------|--------------|---------|-----------|-------------|
| 75%         | ~33%  | 1.03         | 1.01    | 2%        | 1.0×        |
| 85%         | ~18%  | 2.66         | 1.40    | 47%       | 1.9×        |
| 92%         | ~9%   | 10.15        | 4.60    | 55%       | 2.2×        |
| 96%         | ~4%   | 13.41        | 11.62   | 13%       | 1.2×        |

The benefit is largest at *moderate* OP (~9–18%): with lots of free space (75%)
GC barely runs so neither WAF rises; under extreme pressure (96%) even FDP must
copy. This non-monotonic shape is the expected FDP/GC behavior and a good figure.

**Lifetime skew** (% of writes hitting the hot region). Working set 90%, seed 1:

| hot-write % | baseline WAF | FDP WAF | reduction | endurance × |
|-------------|--------------|---------|-----------|-------------|
| 70%         | 5.74         | 3.95    | 31%       | 1.5×        |
| 85%         | 5.54         | 2.59    | 53%       | 2.1×        |
| 95%         | 5.50         | 2.17    | 61%       | 2.5×        |

Monotonic, as expected: the more separable the workload (higher skew), the more
FDP helps. Baseline WAF is ~flat (mixing hurts it regardless); FDP WAF falls as
lifetimes become cleaner to separate.

**Seed variance** (working set 90%, 90% hot-write): baseline 5.41 / 5.66, FDP
2.57 / 2.38 — low variance (~±2%), so single-seed points are representative;
add more seeds for formal error bars.

### Further work

- **Higher volume / more seeds** for publication-grade error bars (deeper churn
  changes absolute WAF but the baseline-vs-FDP gap is the result).
- **Throughput / latency** — needs bare metal; NVMeVirt's timing model is not
  faithful under TCG. There, GiB-scale runs finish in seconds.
- **N lifetime classes** — extend the 2-class (hot/cold) model to 3–4 temperature
  bands over RUHs 1–4 (8 are available) to show the multi-RUH case.
- **FDP events metric** — the type-0x81 count reads unreliably under TCG; treat
  as a qualitative "GC observed" aside or confirm on bare metal.
- **RU-size accuracy** — reclaim-unit nominal size is reported per FTL partition;
  reconcile with the cross-partition striped view if it matters for the writeup.

> Note: virtme mounts the repo as a copy-on-write overlay, so files written by
> the guest (e.g. `sweep_results.csv`) do not propagate to the host. The
> harness therefore also echoes every result to stdout, which `run_sweep.sh`
> captures to `/tmp/fdp_sweep.log` on the host; `sweep_results.csv` is
> reconstructed from that log.

## References

- NVMe Technical Proposal 4146a: Flexible Data Placement.
- NVMe Base Specification 2.0 — Identify Controller (CNS 0x01), Identify
  Namespace (CNS 0x00).
- Upstream NVMeVirt: https://github.com/snu-csl/nvmevirt
- NVMeVirt: A Versatile Software-defined Virtual NVMe Device, USENIX FAST 2023.
