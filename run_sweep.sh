#!/bin/bash
# Host-side launcher for the FDP WAF sweeps. Builds nvmev.ko, boots one TCG VM,
# runs fdp_sweep.sh (all measurements in a single guest session), and leaves
# the results in sweep_results.csv (9p-shared from the guest). SLOW: ~18
# measurements x (fill+churn) under emulation -> tens of minutes.
#   CHURN_MB=20 ./run_sweep.sh   # deeper per-point churn, slower

cd "$(dirname "$0")" || exit 1
chmod +x fdp_sweep.sh

echo "### building nvmev.ko on host ###"
make -s 2>&1 | tail -3
[ -f nvmev.ko ] || { echo "HOST BUILD FAILED"; exit 1; }
ls -l nvmev.ko | awk '{print "nvmev.ko:", $5, "bytes"}'

echo "${CHURN_MB:-12}" > sweep_churn_mb   # informational sidecar
LOG=/tmp/fdp_sweep.log
echo "### launching sweep VM (TCG, SLOW: tens of minutes) -> $LOG ###"
vng \
  --disable-kvm \
  --run /boot/vmlinuz-$(uname -r) \
  --cpus 4 \
  --memory 8G \
  --append 'memmap=1G\$4G intremap=off' \
  --exec "$PWD/fdp_sweep.sh" </dev/null >"$LOG" 2>&1
rc=$?
stty sane 2>/dev/null

# The guest wrote its CSV to a CoW overlay that does not reach the host;
# rebuild it from the ROW lines the guest echoed to stdout (captured in $LOG).
echo "exp,ws_pct,hotspace,hotwrite,churn_mb,seed,mode,dHBMW,dMBMW,MBE,WAF" > sweep_results.csv
grep '^ROW,' "$LOG" | sed 's/^ROW,//' | tr -d '\r' >> sweep_results.csv

echo "### vng exited rc=$rc ; sweep_results.csv: ###"
cat sweep_results.csv
