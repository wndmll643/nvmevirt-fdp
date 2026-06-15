#!/bin/bash
# Runs INSIDE the virtme-ng guest. RocksDB-like LSM workload: write
# amplification vs the number of reclaim unit handles (RUHs) used to separate
# LSM levels. Level == lifetime class; the FDP arm places level groups into N
# RUHs, the baseline arm uses no directive. WAF is timing-independent so TCG is
# fine. Each measurement = fresh module load, random-order fill, snapshot,
# churn, snapshot; WAF = delta-MBMW / delta-HBMW over the churn phase.
#
# Emits one CSV row per measurement to lsm_results.csv (9p-shared) AND a ROW
# line to stdout (the repo is a CoW overlay in the guest, so run_lsm.sh rebuilds
# the CSV on the host from these ROW lines).
#
# Knobs (env):  CHURN_MB (default 30)  LSM_T (4)  LSM_L (4)  LSM_WS (90)
#               LSM_MEMSIZE (64M; bump to 256M for L>=5)

export PATH="/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")" || exit 1

MEMSIZE="${LSM_MEMSIZE:-64M}"
CHURN="${CHURN_MB:-$(cat lsm_churn_mb 2>/dev/null || echo 30)}"
T="${LSM_T:-4}"; L="${LSM_L:-4}"; WS="${LSM_WS:-90}"
CSV="$PWD/lsm_results.csv"

say() { echo; echo "##### $* #####"; }

say "ENV"; uname -r; nvme version 2>&1 | head -1
sudo modprobe nvme 2>&1 | tail -0
[ -f nvmev.ko ] || { echo "nvmev.ko missing"; exit 1; }
gcc -O2 -o /tmp/lsm_workload lsm_workload.c && echo "lsm_workload built" || exit 1

read_stat() { sudo nvme fdp stats "$1" --endgrp-id=1 2>/dev/null | grep -E "$2" | grep -oE '[0-9]+' | tail -1; }

echo "fanout,levels,num_ruh,seed,mode,dHBMW,dMBMW,MBE,WAF" > "$CSV"

# measure_lsm <num_ruh> <seed> <mode>
measure_lsm() {
	local n=$1 seed=$2 mode=$3 ns ctrl
	sudo insmod ./nvmev.ko memmap_start=4G memmap_size=$MEMSIZE cpus=2,3 2>/dev/null
	ns=""; for i in $(seq 1 20); do ns=$(ls /dev/nvme*n1 2>/dev/null | head -1); [ -n "$ns" ] && break; sleep 0.5; done
	ctrl=${ns%n1}
	if [ -z "$ns" ]; then echo "$T,$L,$n,$seed,$mode,NODEV,,," >> "$CSV"; echo "ROW,$T,$L,$n,$seed,$mode,NODEV,,,"; return; fi

	/tmp/lsm_workload "$ns" "fill$mode" 0      "$T" "$L" "$n" "$seed" "$WS" >/dev/null 2>&1
	local h1 m1; h1=$(read_stat "$ctrl" HBMW); m1=$(read_stat "$ctrl" MBMW)
	/tmp/lsm_workload "$ns" "$mode"     "$CHURN" "$T" "$L" "$n" "$seed" "$WS" >/dev/null 2>&1
	local h2 m2 mbe; h2=$(read_stat "$ctrl" HBMW); m2=$(read_stat "$ctrl" MBMW); mbe=$(read_stat "$ctrl" MBE)
	sudo rmmod nvmev 2>/dev/null

	local dh=$((h2-h1)) dm=$((m2-m1)) waf
	if [ "$dh" -gt 0 ]; then local mi=$((dm*1000/dh)); waf="$((mi/1000)).$(printf '%03d' $((mi%1000)))"; else waf="n/a"; fi
	echo "$T,$L,$n,$seed,$mode,$dh,$dm,$mbe,$waf" >> "$CSV"
	echo "ROW,$T,$L,$n,$seed,$mode,$dh,$dm,$mbe,$waf"
}

say "LSM RUH SWEEP (T=$T L=$L ws=$WS%% churn=${CHURN}MiB): baseline once, FDP for N=1..L"
# Baseline ignores N (no directive); run it once as the reference line.
measure_lsm 1 1 base
for n in 1 2 3 4; do measure_lsm "$n" 1 fdp; done

say "CSV (lsm_results.csv)"; cat "$CSV"
say "DONE"
