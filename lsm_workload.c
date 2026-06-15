// SPDX-License-Identifier: GPL-2.0-only
//
// RocksDB-like LSM-tree workload generator for the FDP write-amplification
// evaluation. Self-contained: it MODELS the device write stream produced by a
// leveled-compaction LSM (no real RocksDB build or external trace needed), and
// drives it against the emulated device through the same NVMe passthrough path
// that `nvme write` uses.
//
//   usage: lsm_workload <dev> <fillbase|fillfdp|base|fdp> <churn_MB>
//                       [fanout_T] [num_levels_L] [num_ruh_N] [seed] [ws_pct]
//
// Why this models an LSM:
//   In leveled compaction, level i holds ~T^i as much data as level 0, and a
//   byte in level i is rewritten on the order of once per level-i fill, which
//   happens ~T times more often than level i+1. So the DATA LIFETIME tracks the
//   LEVEL: L0 is tiny and churns fast (hot), the deepest level is huge and is
//   rarely rewritten (cold), with adjacent levels separated by ~T. That is the
//   exact "place data of one lifetime in one reclaim unit" premise of FDP, so
//   level == lifetime class == reclaim unit handle.
//
// Model:
//   * Level i in [0,L) owns a contiguous LBA band of size proportional to T^i,
//     normalised so the bands fill ws_pct% of the device. L0 (smallest) is hot,
//     L(L-1) (largest) is cold.
//   * Fill phase: write every LBA of the working set once, in random order,
//     tagged by its band's level.
//   * Churn phase: each write picks a level uniformly (leveled compaction does
//     roughly equal merge BYTES per level in steady state), then a uniform LBA
//     in that band. Because low levels are small regions hit by comparable byte
//     volume, their per-LBA rewrite rate is ~T x higher per level -- the hot/cold
//     separation emerges from the geometry, it is not hand-set.
//   * FDP arm: each write carries a placement directive whose RUH is the level's
//     group (see group_of_level): with N>=L every level gets its own RUH; with
//     N<L the hottest N-1 levels get their own RUH and the colder levels share
//     the last one (hottest separated first). N=1 puts everything in one RUH, so
//     it reproduces the baseline (single reclaim unit) -- a built-in sanity check.
//   * Baseline arm: no directive, all data to the controller default RUH 0.
//
// WAF is read from the FDP Statistics log exactly as in fdp_eval.sh; this binary
// only changes HOW the write stream is shaped. Build (in guest):
//   gcc -O2 -o lsm_workload lsm_workload.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

#define IO_BYTES   (32 * 1024)
#define LBA_BYTES  512
#define LBAS_PER_IO (IO_BYTES / LBA_BYTES)
#define NVME_CMD_WRITE 0x01
#define DTYPE_PLACEMENT 2

#define MAX_LEVELS 8
#define MIN_BAND_IOS 8        /* refuse a geometry that starves a level */

static int g_fd;
static void *g_buf;

/* one 32 KiB write at io slot `idx`, optional placement directive */
static int do_write(uint64_t idx, uint32_t dtype, uint32_t dspec)
{
	uint64_t slba = idx * LBAS_PER_IO;
	struct nvme_passthru_cmd cmd = {
		.opcode = NVME_CMD_WRITE,
		.nsid = 1,
		.addr = (uint64_t)(uintptr_t)g_buf,
		.data_len = IO_BYTES,
		.cdw10 = (uint32_t)(slba & 0xffffffff),
		.cdw11 = (uint32_t)(slba >> 32),
		.cdw12 = (LBAS_PER_IO - 1) | (dtype << 20), /* NLB[15:0], DTYPE[23:20] */
		.cdw13 = (dspec << 16),                     /* DSPEC[31:16] = RUH index */
	};
	int attempt = 0;
	/* Failure is almost always write-buffer backpressure; back off so the
	 * loop self-throttles to the device drain rate instead of erroring. */
	while (ioctl(g_fd, NVME_IOCTL_IO_CMD, &cmd) != 0) {
		if (++attempt > 100000) return -1;
		usleep(100);
	}
	return 0;
}

/* 48-bit-ish random index, like fdp_workload.c */
static uint64_t rnd(uint64_t mod)
{
	return ((uint64_t)rand() * 32768ull + rand()) % mod;
}

/* Map a level to its reclaim unit handle (1..N). Hottest levels separated
 * first: with N>=L each level is its own RUH; with N<L the hottest N-1 levels
 * get private RUHs and the rest share RUH N. */
static uint32_t group_of_level(int level, int L, int N)
{
	if (N >= L)
		return (uint32_t)(level + 1);
	if (level < N - 1)
		return (uint32_t)(level + 1);
	return (uint32_t)N;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <dev> <fillbase|fillfdp|base|fdp> <churn_MB> "
			"[fanout_T] [num_levels_L] [num_ruh_N] [seed] [ws_pct]\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	const char *mode = argv[2];
	unsigned long churn_mb = strtoul(argv[3], NULL, 10);
	int T       = (argc > 4) ? atoi(argv[4]) : 4;
	int L       = (argc > 5) ? atoi(argv[5]) : 4;
	int N       = (argc > 6) ? atoi(argv[6]) : L;
	unsigned seed = (argc > 7) ? (unsigned)strtoul(argv[7], NULL, 10) : 1;
	int ws_pct  = (argc > 8) ? atoi(argv[8]) : 90;

	if (T < 2) T = 2;
	if (L < 1) L = 1;
	if (L > MAX_LEVELS) L = MAX_LEVELS;
	if (N < 1) N = 1;
	if (N > L) N = L;
	if (ws_pct < 1 || ws_pct > 99) ws_pct = 90;

	int is_fill = (strncmp(mode, "fill", 4) == 0);
	int is_fdp  = (strstr(mode, "fdp") != NULL);

	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) { perror("open"); return 1; }
	off_t dev_bytes = lseek(g_fd, 0, SEEK_END);
	if (dev_bytes <= 0) { perror("lseek"); return 1; }

	uint64_t tot_ios = ((uint64_t)(dev_bytes / LBA_BYTES) / LBAS_PER_IO) * ws_pct / 100;

	/* Level band sizes proportional to T^i (L0 smallest = hot). */
	double w[MAX_LEVELS], wsum = 0.0, pw = 1.0;
	for (int i = 0; i < L; i++) { w[i] = pw; wsum += pw; pw *= T; }

	uint64_t band_ios[MAX_LEVELS], band_start[MAX_LEVELS];
	uint64_t acc = 0;
	for (int i = 0; i < L; i++) {
		band_ios[i] = (uint64_t)((double)tot_ios * w[i] / wsum);
		band_start[i] = acc;
		acc += band_ios[i];
	}
	/* give any rounding remainder to the coldest (largest) level */
	band_ios[L - 1] += (tot_ios - acc);

	for (int i = 0; i < L; i++) {
		if (band_ios[i] < MIN_BAND_IOS) {
			fprintf(stderr,
				"lsm: level %d has only %llu IOs (< %d). Use a larger device "
				"(memmap_size), smaller fanout_T, or fewer levels_L.\n",
				i, (unsigned long long)band_ios[i], MIN_BAND_IOS);
			return 3;
		}
	}

	if (posix_memalign(&g_buf, 4096, IO_BYTES)) { perror("memalign"); return 1; }
	memset(g_buf, 0xab, IO_BYTES);
	srand(seed);

	printf("dev=%s mode=%s LSM T=%d L=%d N=%d ws=%d%% tot_ios=%llu\n", dev, mode,
	       T, L, N, ws_pct, (unsigned long long)tot_ios);
	for (int i = 0; i < L; i++)
		printf("  L%d: ios=%llu (%.1f MiB) -> RUH %u%s\n", i,
		       (unsigned long long)band_ios[i],
		       band_ios[i] * (double)IO_BYTES / (1024 * 1024),
		       is_fdp ? group_of_level(i, L, N) : 0u,
		       is_fdp ? "" : " (baseline: no directive)");

	uint64_t done = 0, failed = 0;

	if (is_fill) {
		/* Fisher-Yates shuffle of [0, tot_ios), write each once, tagged by
		 * which level band the LBA falls into. */
		uint64_t *perm = malloc(tot_ios * sizeof(uint64_t));
		if (!perm) { perror("malloc"); return 1; }
		for (uint64_t i = 0; i < tot_ios; i++) perm[i] = i;
		for (uint64_t i = tot_ios - 1; i > 0; i--) {
			uint64_t j = rnd(i + 1);
			uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
		}
		for (uint64_t i = 0; i < tot_ios; i++) {
			uint64_t idx = perm[i];
			int lvl = L - 1;
			for (int k = 0; k < L; k++)
				if (idx < band_start[k] + band_ios[k]) { lvl = k; break; }
			uint32_t dt = 0, ds = 0;
			if (is_fdp) { dt = DTYPE_PLACEMENT; ds = group_of_level(lvl, L, N); }
			if (do_write(idx, dt, ds) == 0) done++; else failed++;
			if ((i & 0x3ff) == 0x3ff)
				fprintf(stderr, "\r  fill %llu/%llu",
					(unsigned long long)(i + 1), (unsigned long long)tot_ios);
		}
		free(perm);
	} else {
		uint64_t n = (churn_mb * 1024ull * 1024ull) / IO_BYTES;
		for (uint64_t i = 0; i < n; i++) {
			int lvl = (int)rnd(L);                       /* ~equal bytes/level */
			uint64_t idx = band_start[lvl] + rnd(band_ios[lvl]);
			uint32_t dt = 0, ds = 0;
			if (is_fdp) { dt = DTYPE_PLACEMENT; ds = group_of_level(lvl, L, N); }
			if (do_write(idx, dt, ds) == 0) done++; else failed++;
			if ((i & 0x3ff) == 0x3ff)
				fprintf(stderr, "\r  churn %llu/%llu",
					(unsigned long long)(i + 1), (unsigned long long)n);
		}
	}

	fprintf(stderr, "\r");
	printf("done: ok=%llu fail=%llu\n", (unsigned long long)done,
	       (unsigned long long)failed);
	free(g_buf);
	close(g_fd);
	return failed ? 1 : 0;
}
