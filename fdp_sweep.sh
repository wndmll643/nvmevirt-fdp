#!/bin/bash
# Runs INSIDE the virtme-ng guest. FDP WAF sweeps (all VM-feasible; WAF is
# timing-independent so TCG is fine). Emits CSV (one row per measurement,
# flushed immediately so partial results survive) to sweep_results.csv in the
# repo dir, which is 9p-shared back to the host.
#
# Experiments:
#   op    - over-provisioning sweep: vary working-set % (free space = OP)
#   skew  - lifetime-skew sweep: vary hot-write % (workload separability)
#   seed  - repeats at the operating point for variance / error bars
#
# Each measurement = fresh module load, random-order fill, snapshot, churn,
# snapshot; WAF = delta-MBMW / delta-HBMW over the churn phase.

export PATH="/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")" || exit 1

MEMSIZE="64M"
# churn volume per measurement: env, else sidecar from run_sweep.sh, else 12
CHURN="${CHURN_MB:-$(cat sweep_churn_mb 2>/dev/null || echo 12)}"
CSV="$PWD/sweep_results.csv"

say() { echo; echo "##### $* #####"; }

say "ENV"; uname -r; nvme version 2>&1 | head -1
sudo modprobe nvme 2>&1 | tail -0
[ -f nvmev.ko ] || { echo "nvmev.ko missing"; exit 1; }
gcc -O2 -o /tmp/fdp_workload fdp_workload.c && echo "workload built" || exit 1

read_stat() { sudo nvme fdp stats "$1" --endgrp-id=1 2>/dev/null | grep -E "$2" | grep -oE '[0-9]+' | tail -1; }

echo "exp,ws_pct,hotspace,hotwrite,churn_mb,seed,mode,dHBMW,dMBMW,MBE,WAF" > "$CSV"

# measure <exp> <ws> <hotspace> <hotwrite> <seed> <mode>
measure() {
	local exp=$1 ws=$2 hs=$3 hw=$4 seed=$5 mode=$6 ns ctrl
	sudo insmod ./nvmev.ko memmap_start=4G memmap_size=$MEMSIZE cpus=2,3 2>/dev/null
	ns=""; for i in $(seq 1 20); do ns=$(ls /dev/nvme*n1 2>/dev/null | head -1); [ -n "$ns" ] && break; sleep 0.5; done
	ctrl=${ns%n1}
	if [ -z "$ns" ]; then echo "$exp,$ws,$hs,$hw,$CHURN,$seed,$mode,NODEV,,," >> "$CSV"; return; fi

	/tmp/fdp_workload "$ns" "fill$mode" 0      "$hs" "$hw" "$seed" "$ws" >/dev/null 2>&1
	local h1 m1; h1=$(read_stat "$ctrl" HBMW); m1=$(read_stat "$ctrl" MBMW)
	/tmp/fdp_workload "$ns" "$mode"     "$CHURN" "$hs" "$hw" "$seed" "$ws" >/dev/null 2>&1
	local h2 m2 mbe; h2=$(read_stat "$ctrl" HBMW); m2=$(read_stat "$ctrl" MBMW); mbe=$(read_stat "$ctrl" MBE)
	sudo rmmod nvmev 2>/dev/null

	local dh=$((h2-h1)) dm=$((m2-m1)) waf
	if [ "$dh" -gt 0 ]; then local mi=$((dm*1000/dh)); waf="$((mi/1000)).$(printf '%03d' $((mi%1000)))"; else waf="n/a"; fi
	echo "$exp,$ws,$hs,$hw,$CHURN,$seed,$mode,$dh,$dm,$mbe,$waf" >> "$CSV"
	# Also emit to stdout: the repo is a CoW overlay in the guest, so $CSV does
	# not reach the host; run_sweep.sh rebuilds the CSV from these ROW lines.
	echo "ROW,$exp,$ws,$hs,$hw,$CHURN,$seed,$mode,$dh,$dm,$mbe,$waf"
}

say "OP SWEEP (vary working set; OP = 100-ws%), skew fixed hs=20 hw=90, seed=1"
for ws in 75 85 92 96; do for m in base fdp; do measure op $ws 20 90 1 $m; done; done

say "SKEW SWEEP (vary hot-write %), ws=90 hs=20, seed=1"
for hw in 70 85 95; do for m in base fdp; do measure skew 90 20 $hw 1 $m; done; done

say "SEED REPEATS at operating point ws=90 hs=20 hw=90"
for s in 1 2; do for m in base fdp; do measure seed 90 20 90 $s $m; done; done

say "CSV (sweep_results.csv)"
cat "$CSV"
say "DONE"
