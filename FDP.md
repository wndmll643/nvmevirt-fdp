# FDP support for NVMeVirt

This fork adds Flexible Data Placement (NVMe TP 4146a) to NVMeVirt, on top of
the existing conventional-SSD path (`conv_ftl`).

## Status

| Phase | What it adds                                              | State |
|-------|-----------------------------------------------------------|-------|
| 1     | Build scaffold and FDP FTL backend (clone of `conv_ftl`)  | done  |
| 2     | FDP capability advertisement in Identify                  | done  |
| 3     | FDP log pages (configs, RUH usage, stats, events) + features | done |
| 4     | Per-RUH placement: writes routed by DTYPE=Placement + DSPEC | done |
| 5     | FDP event ring + statistics lifecycle                     | done |
| 6     | Integration tests + workload comparison vs. conv_ftl       | functional pass (VM); WAF/GC pending bare metal |

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

NOT yet exercised at runtime (needs GC, i.e. writing past device capacity —
impractical under slow TCG, belongs on bare metal): the MBMW bump in
`gc_write_page`, the MBE bump in `mark_block_free`, the type 0x81
(implicitly-modified-RU) event, and any WAF > 1.0. These are low risk
(one-line counter increments reusing the validated event path) but remain to
be confirmed.

Known cosmetic/semantic items:
- RU Nominal Size is reported as one FTL partition's line (128 KiB); the
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
  2 LUNs/ch, 32 KiB flash page, 256 MiB blocks) and adds three FDP knobs:
  - `FDP_NR_RG = 1`     — reclaim groups
  - `FDP_NR_RUH = 8`    — reclaim unit handles per namespace
  - `FDP_NR_EVENTS = 63`— events log ring depth
- `fdp_ftl.{c,h}` — forked from `conv_ftl.{c,h}` with `fdp_`-prefixed structs
  and functions. Behaviorally identical to `conv_ftl` (single write pointer,
  same GC, same line management). Forked rather than refactored because phase 4
  will diverge significantly (per-RUH write pointers) and the original
  `conv_ftl` should stay stable.
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
- LID 0x22 FDP Statistics: HBMW / MBMW / MBE 128-bit counters, already fed
  by live hooks — host writes bump HBMW+MBMW in `fdp_write`, GC page copies
  bump MBMW in `gc_write_page`, block erases bump MBE in `mark_block_free`.
  `WAF ≈ MBMW / HBMW` is observable under workload now.
- LID 0x23 FDP Events: header only, zero events (ring arrives with Phase 5).
- FID 0x1D (FDP): Get returns FDPE=1/FDPCI=0; Set allows an idempotent
  re-enable and otherwise fails with Feature Not Changeable.
- FID 0x1E (FDP Events): Get/Set exchange `{evt, evta}` descriptors for the
  two supported host event types (RU Not Fully Written, Invalid PID).

Caveats: log transfers are single-PRP (≤ 4 KiB), matching the existing
NVMeVirt log-page limitation; the FID 0x1E descriptor format is best-effort
from TP 4146a and should be validated against `nvme fdp set-events` /
`nvme fdp feature` output during testing.

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
  TP 4146a. (The corresponding FDP event is queued from Phase 5.)
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
  this FTL only rotates a line once it is completely written, so the
  condition cannot occur.

All emissions honor the FID 0x1E enable mask, and all event types default to
disabled per spec — `nvme fdp events` stays empty until the host enables
event types via Set Features 0x1E. Timestamps are `local_clock()` nanoseconds
rather than NVMe timestamp-feature time; fine for emulation, worth knowing
when correlating with host clocks.

## Files

```
modified:
  Kbuild           # +CONFIG_NVMEVIRT_FDP target
  admin.c          # +FDPS in id-ctrl, +FDP fields in id-ns
  main.c           # +fdp_init/remove_namespace dispatch
  nvme.h           # rsvd64[40] sliced into NVMe 2.0 fields
  nvmev.h          # (local edits)
  ssd_config.h     # +SAMSUNG_FDP model, +SSD_TYPE_FDP, +FDP knobs

added:
  fdp_ftl.c        # FDP FTL backend (fork of conv_ftl)
  fdp_ftl.h        # types & API
  nvme_fdp.h       # FDP-specific NVMe constants
  FDP.md           # this file
```

## Build

The `Kbuild` ships with `CONFIG_NVMEVIRT_FDP := y` active. Just:

```bash
make clean && make
```

Produces `nvmev.ko` linking `fdp_ftl.o`, `ssd.o`, `pqueue/pqueue.o`,
`channel_model.o`.

## Test

Recommended path: a virtme-ng VM, so a misbehaving build can only crash the
guest. Requires `virtme-ng`, `qemu-system-x86`, and `nvme-cli` on the host.

```bash
# one-time setup
sudo apt install virtme-ng qemu-system-x86 nvme-cli
sudo chmod a+r /boot/vmlinuz-$(uname -r)

# launch the VM (boots the host kernel, mounts $HOME live)
cd ~/snu/nvmevirt-fdp
vng --run /boot/vmlinuz-$(uname -r) \
    --memory 4G \
    --append 'memmap=1G$2G intremap=off isolcpus=2,3'

# inside the VM:
cd ~/snu/nvmevirt-fdp
make clean && make
sudo insmod ./nvmev.ko memmap_start=2G memmap_size=1G cpus=2,3
dmesg | tail -20
```

### Phase 1 check — round-trip works

```bash
ls /dev/nvme*
sudo dd if=/dev/urandom of=/tmp/x bs=4K count=64
sudo dd if=/tmp/x of=/dev/nvme0n1 bs=4K count=64 oflag=direct
sudo dd if=/dev/nvme0n1 of=/tmp/y bs=4K count=64 iflag=direct
cmp /tmp/x /tmp/y && echo "Phase 1 PASS"
```

### Phase 2 check — FDP advertised

```bash
sudo nvme id-ctrl /dev/nvme0 -H | grep -iE 'ctratt|fdp|flex'
# expect: ctratt : 0x80010  (bit 19 = FDP Supported, bit 4 = Endurance Groups)

sudo nvme id-ns /dev/nvme0n1 -H | grep -iE 'endgid|npwg|npwa|nows'
# expect: endgid=1, npwg=63, npwa=63, nows=63
```

### Phase 3 check — FDP logs and features

```bash
sudo nvme fdp configs /dev/nvme0 --endgrp-id=1
# expect: 1 config: nrg=1, nruh=8, runs=512MiB (pgs_per_line * 4K), 8 RUHs type 1

sudo nvme fdp usage  /dev/nvme0 --endgrp-id=1   # 8 RUHs, all "unused" for now
sudo nvme fdp stats  /dev/nvme0 --endgrp-id=1   # HBMW/MBMW grow with dd traffic
sudo nvme fdp events /dev/nvme0 --endgrp-id=1   # 0 events (Phase 5)

sudo nvme get-feature /dev/nvme0 -f 0x1d        # expect value 0x1 (FDP enabled)
```

### Phase 4 check — placement writes routed per RUH

```bash
# Write 32 KiB with placement ID 3 (dtype 2, dspec 3)
sudo dd if=/dev/urandom of=/tmp/x bs=32K count=1
sudo nvme write /dev/nvme0n1 --start-block=0 --block-count=63 \
     --data-size=32768 --data=/tmp/x --dir-type=2 --dir-spec=3

# RUH 3 flips from "unused" to "host specified"; RUH 0 stays untouched
sudo nvme fdp usage /dev/nvme0 --endgrp-id=1

# Out-of-range placement ID is rejected with Invalid Field
sudo nvme write /dev/nvme0n1 --start-block=0 --block-count=63 \
     --data-size=32768 --data=/tmp/x --dir-type=2 --dir-spec=99
# expect: NVMe status: INVALID_FIELD

# Read-back still works regardless of which RUH the data went to
sudo nvme read /dev/nvme0n1 --start-block=0 --block-count=63 \
     --data-size=32768 --data=/tmp/y && cmp /tmp/x /tmp/y && echo "Phase 4 PASS"
```

### Phase 5 check — FDP events

```bash
# Events default to disabled; enable invalid-PID reporting first
sudo nvme fdp set-events /dev/nvme0n1 --enable --event-types=3

# Trigger one: out-of-range placement ID
sudo nvme write /dev/nvme0n1 --start-block=0 --block-count=63 \
     --data-size=32768 --data=/tmp/x --dir-type=2 --dir-spec=99
# write fails with INVALID_FIELD, and...

sudo nvme fdp events /dev/nvme0 --endgrp-id=1
# expect: 1 event, type 0x3 (Invalid Placement Identifier), pid=99

# Implicitly-modified-RU events (type 0x81) appear once GC kicks in under
# sustained overwrite load with --event-types=129 enabled as well.
```

## Roadmap

### Phase 6 — integration testing

Everything up to Phase 5 is compile-tested only; Phase 6 is the runtime
validation pass. Two environments, in order:

#### Environment A — virtme-ng VM (start here)

The setup from the Test section above. The VM boots the host's own kernel
under QEMU/KVM with `$HOME` live-mounted, so the same checkout is built and
loaded inside the guest. A panic in `nvmev.ko` kills only the guest —
relaunch `vng` and go again. No grub changes, no reboots, no risk to the
host.

What it is good for:
- All functional checks (Phase 1–5 recipes above): identify fields, log
  pages, features, placement routing, invalid-PID rejection, events.
- WAF comparison. Write amplification is a function of FTL logic and
  workload shape, not wall-clock speed, so ΔMBMW/ΔHBMW ratios measured in
  the VM are valid.

What it is not good for:
- Absolute latency/throughput numbers. NVMeVirt's timing emulation depends
  on busy-polling threads pinned to isolated CPUs; inside a VM those are
  vCPUs at the mercy of the host scheduler, so timing fidelity is reduced.
  Functional results carry over; performance numbers do not.

#### Environment B — bare metal (for performance-faithful runs)

Only after the full Phase 6 suite passes in the VM. Requires host
configuration (and survives reboots, so undo it when done):

```bash
# /etc/default/grub — reserve memory, isolate CPUs, sidestep the IOMMU panic
GRUB_CMDLINE_LINUX="memmap=8G\\\$16G isolcpus=2,3 intremap=off"
sudo update-grub && sudo reboot

# after reboot — verify the reservation took
cat /proc/iomem | grep -i reserved

sudo insmod ./nvmev.ko memmap_start=16G memmap_size=8G cpus=2,3
```

Adjust offsets/sizes to this machine's RAM (62 GiB total). The known
failure mode: a panic in `__pci_enable_msix()` / `nvme_hwmon_init()` at
insmod means IOMMU interrupt remapping is still active — check that
`intremap=off` is really on the running command line (`cat /proc/cmdline`).

#### Workload plan

fio ≥ 3.34 drives FDP natively through the io_uring passthru engine
(`--fdp=1` handles the placement directives; no manual dtype/dspec):

```bash
# A: lifetime-separated streams — each job overwrites its own region at its
# own rate, placed via distinct PIDs
sudo fio --name=sep --ioengine=io_uring_cmd --cmd_type=nvme \
     --filename=/dev/ng0n1 --rw=randwrite --bs=32k --iodepth=8 \
     --fdp=1 --fdp_pli=1,2,3 --fdp_pli_select=roundrobin \
     --size=100% --loops=4 --group_reporting

# B: same load, everything collapsed onto the default RUH (no --fdp)
sudo fio --name=mixed --ioengine=io_uring_cmd --cmd_type=nvme \
     --filename=/dev/ng0n1 --rw=randwrite --bs=32k --iodepth=8 \
     --size=100% --loops=4 --group_reporting
```

Measure WAF per run as ΔMBMW / ΔHBMW from `nvme fdp stats` snapshots taken
before and after (reload the module between runs for clean counters — state
is volatile). The namespace must be overwritten well past its capacity
(`--loops` ≥ 2 with `--size=100%`) or GC never runs and both WAFs are 1.0.

Pass criteria:
- Run A WAF noticeably lower than run B WAF (separated lifetimes → lines
  die wholesale → cheaper GC). The gap, not the absolute values, is the
  result.
- `nvme fdp usage` shows the PIDs used by run A as host-specified.
- With event types 0x3/0x81 enabled, `nvme fdp events` accumulates
  implicitly-modified-RU events during GC-heavy stretches.
- The conv_ftl (`SAMSUNG_970PRO`) build under workload B's I/O pattern
  yields a WAF comparable to the FDP build's run B — confirming the FDP
  target didn't regress baseline GC behavior.

## References

- NVMe Technical Proposal 4146a: Flexible Data Placement.
- NVMe Base Specification 2.0 — Identify Controller (CNS 0x01), Identify
  Namespace (CNS 0x00).
- NVMeVirt FAST '23 paper — base device-emulation architecture.
