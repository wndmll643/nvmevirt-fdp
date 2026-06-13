#!/bin/bash
# Runs INSIDE the virtme-ng guest. Builds, loads, and exercises the FDP
# target through Phases 1-5. Tolerant by design: never aborts, so one run
# surfaces every problem at once. All output is console -> captured by vng.

export PATH="/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")" || exit 1

say() { echo; echo "##### $* #####"; }

say "ENV"
uname -r
nvme version 2>&1 | head -1 || echo "nvme-cli missing"
echo "memmap on cmdline: $(grep -o 'memmap=[^ ]*' /proc/cmdline)"
echo "reserved regions in iomem:"; grep -i reserved /proc/iomem

say "LOAD NVME DRIVER"
# virtme-ng boots a virtio root, so the in-kernel nvme driver is not loaded.
# nvmev creates a virtual PCI device but nothing binds to it without this.
sudo modprobe nvme 2>&1 && echo "modprobe nvme ok"
lsmod | grep -E '^nvme' || echo "(nvme not in lsmod)"

say "MODULE"
# Built natively on the host before launch; same kernel, so it loads here.
ls -l nvmev.ko 2>&1 | awk '{print $5, $NF}'

say "INSMOD"
sudo insmod ./nvmev.ko memmap_start=4G memmap_size=1G cpus=2,3 2>&1
echo "insmod rc=$?"
dmesg | grep -iE "nvmev|samsung-like|fdp ftl|nvme nvme" | tail -10

# Namespace device nodes appear asynchronously after the controller binds.
NS=""
for i in $(seq 1 20); do
	NS=$(ls /dev/nvme*n1 2>/dev/null | head -1)
	[ -n "$NS" ] && break
	sleep 0.5
done
CTRL=${NS%n1}
echo "device: ctrl=$CTRL ns=$NS"
[ -z "$NS" ] && { echo "NO NVME DEVICE -- stopping"; dmesg | tail -25; exit 1; }

say "PHASE 1 -- block round-trip"
dd if=/dev/urandom of=/tmp/x bs=4K count=64 status=none
sudo dd if=/tmp/x of="$NS" bs=4K count=64 oflag=direct status=none
sudo dd if="$NS" of=/tmp/y bs=4K count=64 iflag=direct status=none
cmp /tmp/x /tmp/y && echo "PHASE 1 PASS" || echo "PHASE 1 FAIL"

say "PHASE 2 -- identify advertises FDP"
sudo nvme id-ctrl "$CTRL" -H 2>&1 | grep -iE 'ctratt|flexible|endurance group' | head
echo "--- id-ns ---"
sudo nvme id-ns "$NS" -H 2>&1 | grep -iE 'endgid|npwg|npwa|nows|nsfeat' | head

say "PHASE 3 -- log pages + features"
echo "--- fdp configs ---"; sudo nvme fdp configs "$CTRL" --endgrp-id=1 2>&1 | head -20
echo "--- fdp usage ---";   sudo nvme fdp usage  "$CTRL" --endgrp-id=1 2>&1 | head -12
echo "--- fdp stats (baseline) ---"; sudo nvme fdp stats "$CTRL" --endgrp-id=1 2>&1 | head
echo "--- get-feature 0x1d (FDP) ---"; sudo nvme get-feature "$CTRL" -f 0x1d 2>&1 | head -3

say "PHASE 4 -- placement routing"
dd if=/dev/urandom of=/tmp/p bs=32K count=1 status=none
echo "--- write to RUH 3 (dir-type=2 dir-spec=3) ---"
sudo nvme write "$NS" --start-block=0 --block-count=63 --data-size=32768 \
     --data=/tmp/p --dir-type=2 --dir-spec=3 2>&1
echo "--- usage (RUH 3 should now be host-specified) ---"
sudo nvme fdp usage "$CTRL" --endgrp-id=1 2>&1 | head -12
echo "--- invalid PID 99 should fail ---"
sudo nvme write "$NS" --start-block=0 --block-count=63 --data-size=32768 \
     --data=/tmp/p --dir-type=2 --dir-spec=99 2>&1
echo "--- read-back ---"
sudo nvme read "$NS" --start-block=0 --block-count=63 --data-size=32768 \
     --data=/tmp/q 2>&1 >/dev/null
cmp /tmp/p /tmp/q && echo "PHASE 4 PASS" || echo "PHASE 4 FAIL"

say "PHASE 5 -- events"
echo "--- enable invalid-PID event (type 0x3) ---"
sudo nvme fdp set-events "$NS" --enable --event-types=3 2>&1
echo "--- trigger invalid PID ---"
sudo nvme write "$NS" --start-block=0 --block-count=63 --data-size=32768 \
     --data=/tmp/p --dir-type=2 --dir-spec=99 2>&1
echo "--- events (expect 1 entry, type 0x3, pid 99) ---"
sudo nvme fdp events "$CTRL" --endgrp-id=1 2>&1 | head -15

say "STATS after writes (WAF visible once GC runs)"
sudo nvme fdp stats "$CTRL" --endgrp-id=1 2>&1 | head

say "RMMOD"
sudo rmmod nvmev 2>&1 && echo "unloaded clean"
say "DONE"
