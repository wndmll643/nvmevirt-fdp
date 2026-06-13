#!/bin/bash
# Host-side launcher. Builds nvmev.ko natively on the host, then boots the
# host kernel in a virtme-ng VM (full TCG emulation, no KVM) with a 1 GiB
# region reserved at 3 GiB for NVMeVirt, and runs fdp_vm_test.sh in the guest.
#
# Why TCG (--disable-kvm): nvmev's controller-enable handshake relies on the
# dispatcher thread seeing the driver's BAR writes through plain RAM. Under
# KVM that cross-vCPU visibility doesn't complete (driver reads CSTS=0 and
# aborts). TCG emulates all CPUs against one coherent host buffer, so the
# handshake works. Slower, but correct for functional testing. KVM is only
# needed if we later want realistic timing (that belongs on bare metal).
#
# No KVM means no /dev/kvm access and no kvm-group dance.

cd "$(dirname "$0")" || exit 1
chmod +x fdp_vm_test.sh

echo "### building nvmev.ko on host (native, fast) ###"
make -s 2>&1 | tail -3
[ -f nvmev.ko ] || { echo "HOST BUILD FAILED -- aborting"; exit 1; }
ls -l nvmev.ko | awk '{print "nvmev.ko:", $5, "bytes"}'

# Reserve memory ABOVE 4 GiB. On x86 the range [3 GiB, 4 GiB) is the PCI MMIO
# hole, not real DRAM (QEMU relocates that RAM above 4 GiB). Reserving the hole
# leaves nvmev's BAR/doorbell area unbacked, so the controller-enable handshake
# never completes (CSTS stays 0, "Device not ready"). An 8 GiB guest lays out
# ~3 GiB low + ~5 GiB high at [4 GiB, 9 GiB), so [4 GiB, 5 GiB) is genuine RAM.
# This mirrors the bare-metal README, which reserves high (memmap=64G$128G).
#
# vng runs its qemu line through check_call(shell=True), one extra /bin/sh; the
# backslash survives bash single-quotes and that sh turns \$ into a literal $,
# so the kernel sees memmap=1G$4G.
#
# Capture guest output to a file rather than the terminal: TCG/qemu takes over
# the tty and leaves it in raw mode on exit (stray ^M, eaten output). Reading
# from a file sidesteps that; we run `stty sane` afterwards to restore the tty.
LOG=/tmp/fdp_vm.log
echo "### launching VM under TCG emulation (slow, ~1-3 min) -> $LOG ###"
vng \
  --disable-kvm \
  --run /boot/vmlinuz-$(uname -r) \
  --cpus 4 \
  --memory 8G \
  --append 'memmap=1G\$4G intremap=off' \
  --exec "$PWD/fdp_vm_test.sh" </dev/null >"$LOG" 2>&1
rc=$?
stty sane 2>/dev/null

echo "### vng exited rc=$rc ; guest output below ###"
cat "$LOG"
