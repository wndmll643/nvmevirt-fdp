#!/bin/bash
# Host-side launcher for the RocksDB-like LSM WAF experiment. Builds nvmev.ko,
# boots one TCG VM, runs fdp_lsm.sh, and leaves the results in lsm_results.csv.
# SLOW: 5 measurements x (fill+churn) under emulation -> tens of minutes.
#   CHURN_MB=50 ./run_lsm.sh        # deeper churn (closer to steady state)
#   LSM_L=5 LSM_MEMSIZE=256M ./run_lsm.sh   # more levels -> longer RUH sweep

cd "$(dirname "$0")" || exit 1
chmod +x fdp_lsm.sh

echo "### building nvmev.ko on host ###"
make -s 2>&1 | tail -3
[ -f nvmev.ko ] || { echo "HOST BUILD FAILED"; exit 1; }
ls -l nvmev.ko | awk '{print "nvmev.ko:", $5, "bytes"}'

echo "${CHURN_MB:-30}" > lsm_churn_mb   # informational sidecar

# Resolve the virtme-ng launcher: env override, else `vng`, else `virtme-ng`.
# (Same dependency as run_sweep.sh -- if you normally activate a venv/module to
#  get `vng`, do that first, or point VNG at the binary: VNG=/path/to/vng ./run_lsm.sh)
VNG="${VNG:-$(command -v vng || command -v virtme-ng)}"
if [ -z "$VNG" ]; then
	echo "ERROR: virtme-ng not found (no 'vng' or 'virtme-ng' on PATH)." >&2
	echo "  Install it (e.g. 'pipx install virtme-ng' or 'pip install --user virtme-ng')," >&2
	echo "  or run in the same environment where ./run_sweep.sh works," >&2
	echo "  or set VNG=/path/to/vng ./run_lsm.sh" >&2
	exit 127
fi

# Guest kernel to boot. Defaults to the host kernel (what the original scripts
# use). IMPORTANT: this project was validated on Linux ~6.12 (see docs/results.md);
# on an older host kernel (e.g. RHEL 8's 4.18) the in-guest `nvme` driver can hard-
# hang on load, so point KERNEL at a 6.x vmlinuz if the host kernel is older:
#   KERNEL=/path/to/vmlinuz-6.12 ./run_lsm.sh
KERNEL="${KERNEL:-/boot/vmlinuz-$(uname -r)}"
# Extra vng flags for non-standard hosts. Example: a RHEL qemu-kvm lacks the
# "microvm" machine type and 9p, so use:  VNG_EXTRA="--disable-microvm" (with a
# Rust virtiofsd + busybox + qemu on PATH). Empty by default to preserve the
# original behavior on the validated environment.
VNG_EXTRA="${VNG_EXTRA:-}"

LOG=/tmp/fdp_lsm.log
echo "### launching LSM VM via $VNG (kernel: $KERNEL) -> $LOG ###"
"$VNG" \
  --disable-kvm \
  --run "$KERNEL" \
  --cpus 4 \
  --memory 8G \
  ${VNG_EXTRA} \
  --append 'memmap=1G\$4G intremap=off' \
  --exec "$PWD/fdp_lsm.sh" </dev/null >"$LOG" 2>&1
rc=$?
stty sane 2>/dev/null

# The guest wrote its CSV to a CoW overlay that does not reach the host;
# rebuild it from the ROW lines the guest echoed to stdout (captured in $LOG).
echo "fanout,levels,num_ruh,seed,mode,dHBMW,dMBMW,MBE,WAF" > lsm_results.csv
grep '^ROW,' "$LOG" | sed 's/^ROW,//' | tr -d '\r' >> lsm_results.csv

echo "### vng exited rc=$rc ; lsm_results.csv: ###"
cat lsm_results.csv
