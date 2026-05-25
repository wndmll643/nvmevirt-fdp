# FDP support for NVMeVirt

This fork adds Flexible Data Placement (NVMe TP 4146a) to NVMeVirt, on top of
the existing conventional-SSD path (`conv_ftl`).

## Status

| Phase | What it adds                                              | State |
|-------|-----------------------------------------------------------|-------|
| 1     | Build scaffold and FDP FTL backend (clone of `conv_ftl`)  | done  |
| 2     | FDP capability advertisement in Identify                  | done  |
| 3     | FDP log pages (configs, RUH usage, stats, events) + features | not started |
| 4     | Per-RUH placement: writes routed by DTYPE=Placement + DSPEC | not started |
| 5     | RU rotation events + statistics lifecycle                 | not started |
| 6     | Integration tests + workload comparison vs. conv_ftl       | not started |

After phases 1 + 2, the device builds, loads, services normal block I/O, and
advertises FDP support — but does not yet make any placement decision based on
host directives. All writes flow through a single shared write pointer, same as
`conv_ftl`. The advertisement is purely informational at this stage.

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

- `nvme_fdp.h` (new) defines `NVME_CTRL_CTRATT_FDPS = 1 << 19` (the
  Flexible Data Placement Supported bit in the controller's CTRATT field) and
  `NVMEV_FDP_ENDGID = 1` (single endurance group).
- `nvme.h`: the legacy `rsvd64[40]` gap in `struct nvme_id_ns` is sliced into
  the NVMe 2.0 fields it actually represents: `npwg`, `npwa`, `npdg`, `npda`,
  `nows`, `mssrl`, `mcl`, `msrc`, `anagrpid`, `nsattr`, `nvmsetid`, `endgid`.
  Byte layout and total struct size unchanged — additive only, so non-FDP
  targets (NVM / CONV / ZNS / KV) keep zero-filling those bytes and see no
  behavior change.
- `admin.c`:
  - Identify Controller (CNS 0x01): sets `ctrl->ctratt = FDPS` under
    `#if SUPPORTED_SSD_TYPE(FDP)`.
  - Identify Namespace (CNS 0x00): for `SSD_TYPE_FDP` namespaces, sets
    `endgid = 1` and `npwg = npwa = nows = (ONESHOT_PAGE_SIZE/LBA_SIZE) - 1`
    (= 63 LBAs, i.e. 32 KiB preferred write granularity).

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
# expect: ctratt : 0x80000  (bit 19 = Flexible Data Placement Supported)

sudo nvme id-ns /dev/nvme0n1 -H | grep -iE 'endgid|npwg|npwa|nows'
# expect: endgid=1, npwg=63, npwa=63, nows=63
```

## Roadmap

### Phase 3 — log pages + features (read-only state)

- LID 0x20 FDP Configurations: enumerate (NR_RG, NR_RUH, RU_SIZE).
- LID 0x21 RUH Usage: per-RUH `lba_written / lba_total`.
- LID 0x22 FDP Statistics: `host_bytes_written`, `media_bytes_written`.
- LID 0x23 FDP Events: ring of `struct fdp_event` (empty initially).
- FID 0x1D FDP enable (per endurance group), FID 0x1E events bitmap.
- Exit: `nvme fdp configs/stats/usage/events /dev/nvme0` return sensible
  output.

### Phase 4 — per-RUH placement writes

- Replace single `wp` with `wp_ruh[NR_RG][NR_RUH]` in `struct fdp_ftl`.
- Parse DTYPE (CDW12 bits 23:20) and DSPEC (CDW13 bits 31:16) in
  `fdp_proc_nvme_io_cmd`. Route placement writes to the addressed RUH; route
  non-placement writes to the spec-default RUH (RUH 0).
- GC unchanged — still draws victim lines from a shared pool.
- Exit: writes with `nvme write --dtype=2 --dspec=N` land in different lines
  for different N's; verifiable via the existing `/proc/nvmev/io_units`.

### Phase 5 — events & statistics

- Emit "RU Not Modified" events on RU rotation.
- Bump `host_bytes_written` on placement writes; `media_bytes_written` on GC
  writes.
- Honor the FID 0x1E event filter.
- Exit: `nvme fdp events` accumulates entries under a stress workload.

### Phase 6 — integration

- `fio --ioengine=io_uring_cmd --cmd_type=nvme --dtype=2 --dspec=…` workloads.
- Verify low WAF (host_bytes ≈ media_bytes) when lifetimes are separable
  across PIDs, vs. high WAF when all writes collapse to one RUH.

## References

- NVMe Technical Proposal 4146a: Flexible Data Placement.
- NVMe Base Specification 2.0 — Identify Controller (CNS 0x01), Identify
  Namespace (CNS 0x00).
- NVMeVirt FAST '23 paper — base device-emulation architecture.
