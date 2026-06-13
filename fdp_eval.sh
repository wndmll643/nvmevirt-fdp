#!/bin/bash
# Runs INSIDE the virtme-ng guest. FDP WAF evaluation:
#   trial A (baseline): hot/cold workload, no placement directives (all RUH 0)
#   trial B (fdp):      same workload, hot -> RUH 1, cold -> RUH 2
# Each trial loads the module fresh (FDP stats reset on init), drives writes
# past capacity into steady-state GC, then reads WAF = MBMW/HBMW and the erase
# counter (MBE) from the FDP Statistics log. A type-0x81 (implicitly-modified-
# RU) event is also enabled so GC activity is observable.
#
# Override total write volume:  TOTAL_MB=200 ./run_eval.sh

export PATH="/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")" || exit 1

# Write volume: env override, else sidecar file written by run_eval.sh, else default.
TOTAL_MB="${TOTAL_MB:-$(cat eval_total_mb 2>/dev/null || echo 150)}"
CHURN_MB="$TOTAL_MB"   # churn-phase write volume (run_trial reads CHURN_MB)
MEMSIZE="64M"

say() { echo; echo "##### $* #####"; }

say "ENV"
uname -r; nvme version 2>&1 | head -1
sudo modprobe nvme 2>&1 && echo "modprobe nvme ok"
[ -f nvmev.ko ] || { echo "nvmev.ko missing (host build failed?)"; exit 1; }

say "BUILD WORKLOAD"
gcc -O2 -o /tmp/fdp_workload fdp_workload.c && echo "fdp_workload built" || { echo "gcc failed"; exit 1; }

# NOTE: no awk. The guest's awk (9p-mounted, under TCG) intermittently fails
# to load its shared libs ("unexpected PLT reloc type"). grep + bash integer
# math are reliable here. WAF is reported in milli-units then formatted.
read_stat() { sudo nvme fdp stats "$1" --endgrp-id=1 2>/dev/null | grep -E "$2" | grep -oE '[0-9]+' | tail -1; }

# run_trial <kind base|fdp>  -> echoes "dHBMW dMBMW MBE WAF EVENTS" for the
# churn phase (WAF = delta-MBMW / delta-HBMW measured AFTER the fill, so the
# WAF=1 fill does not dilute the steady-state number).
run_trial() {
	local kind="$1" ns ctrl
	sudo insmod ./nvmev.ko memmap_start=4G memmap_size=$MEMSIZE cpus=2,3 2>&1 >/dev/null
	ns=""; for i in $(seq 1 20); do ns=$(ls /dev/nvme*n1 2>/dev/null | head -1); [ -n "$ns" ] && break; sleep 0.5; done
	ctrl=${ns%n1}
	if [ -z "$ns" ]; then echo "NODEV"; return; fi
	sudo nvme fdp set-events "$ns" --enable --event-types=129 >/dev/null 2>&1

	# Phase 1: random-order fill of the whole device (establishes initial
	# data; baseline lines get a hot/cold mix, FDP lines are lifetime-pure).
	/tmp/fdp_workload "$ns" "fill$kind" 0 20 90 1 1>&2
	local h1 m1
	h1=$(read_stat "$ctrl" HBMW); m1=$(read_stat "$ctrl" MBMW)

	# Phase 2: churn -- rewrite the hot set repeatedly, driving GC.
	/tmp/fdp_workload "$ns" "$kind" "$CHURN_MB" 20 90 1 1>&2
	local h2 m2 mbe ev waf
	h2=$(read_stat "$ctrl" HBMW); m2=$(read_stat "$ctrl" MBMW); mbe=$(read_stat "$ctrl" MBE)
	local dh=$((h2-h1)) dm=$((m2-m1)) waf
	if [ "$dh" -gt 0 ]; then
		local milli=$(( dm * 1000 / dh ))
		waf="$((milli/1000)).$(printf '%03d' $((milli%1000)))"
	else
		waf="n/a"
	fi
	ev=$(sudo nvme fdp events "$ctrl" --endgrp-id=1 2>/dev/null | grep -c "Event Type")
	sudo rmmod nvmev 2>/dev/null
	echo "$dh $dm $mbe $waf $ev"
}

say "TRIAL A — baseline (random fill, no placement, churn to RUH 0)"
A=$(run_trial base); echo "churn dHBMW dMBMW MBE WAF events: $A"

say "TRIAL B — FDP (fill places hot->RUH1 cold->RUH2, churn likewise)"
B=$(run_trial fdp); echo "churn dHBMW dMBMW MBE WAF events: $B"

say "RESULTS  (device ${MEMSIZE}, churn ${CHURN_MB} MiB, 90% of writes -> 20% hot space)"
read a_dh a_dm a_mbe a_waf a_ev <<<"$A"
read b_dh b_dm b_mbe b_waf b_ev <<<"$B"
printf "%-10s %14s %14s %14s %8s %8s\n" "trial" "dHBMW(B)" "dMBMW(B)" "MBE(B)" "WAF" "events"
printf "%-10s %14s %14s %14s %8s %8s\n" "baseline" "$a_dh" "$a_dm" "$a_mbe" "$a_waf" "$a_ev"
printf "%-10s %14s %14s %14s %8s %8s\n" "fdp"      "$b_dh" "$b_dm" "$b_mbe" "$b_waf" "$b_ev"
if [ "${a_dh:-0}" -gt 0 ] && [ "${b_dh:-0}" -gt 0 ]; then
	am=$(( a_dm * 1000 / a_dh )); bm=$(( b_dm * 1000 / b_dh ))
	if [ "$am" -gt 0 ]; then
		echo
		echo "WAF: baseline $a_waf, fdp $b_waf  ->  $(( (am-bm)*100/am ))% reduction with FDP"
	fi
fi

say "DONE"
