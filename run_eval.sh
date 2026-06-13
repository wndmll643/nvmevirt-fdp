#!/bin/bash
# Host-side launcher for the FDP WAF evaluation. Builds nvmev.ko natively,
# boots the host kernel in a virtme-ng VM under TCG, and runs fdp_eval.sh in
# the guest. See run_vm_test.sh for why TCG / memmap-above-4GiB / output-to-file.
#
# This run writes hundreds of MiB through the emulated device under software
# emulation, so it is SLOW -- expect several minutes to tens of minutes.
# Tune the write volume with TOTAL_MB (passed through to the guest):
#   TOTAL_MB=100 ./run_eval.sh

cd "$(dirname "$0")" || exit 1
chmod +x fdp_eval.sh

echo "### building nvmev.ko on host ###"
make -s 2>&1 | tail -3
[ -f nvmev.ko ] || { echo "HOST BUILD FAILED -- aborting"; exit 1; }
ls -l nvmev.ko | awk '{print "nvmev.ko:", $5, "bytes"}'

# Pass TOTAL_MB to the guest via a sidecar file in the (9p-shared) repo dir.
# virtme's --exec wants a bare script path; an "env VAR=.. script" form fails.
echo "${TOTAL_MB:-150}" > eval_total_mb

LOG=/tmp/fdp_eval.log
echo "### launching eval VM (TCG, SLOW: minutes), TOTAL_MB=$(cat eval_total_mb) -> $LOG ###"
vng \
  --disable-kvm \
  --run /boot/vmlinuz-$(uname -r) \
  --cpus 4 \
  --memory 8G \
  --append 'memmap=1G\$4G intremap=off' \
  --exec "$PWD/fdp_eval.sh" </dev/null >"$LOG" 2>&1
rc=$?
stty sane 2>/dev/null

echo "### vng exited rc=$rc ; eval output below ###"
cat "$LOG"
