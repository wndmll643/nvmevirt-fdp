// SPDX-License-Identifier: GPL-2.0-only
//
// Synthetic workload generator for the FDP write-amplification evaluation.
// Issues fixed-size writes via the NVMe passthrough ioctl (the path `nvme
// write` uses), choosing each write's target by a hot/cold model and, in FDP
// mode, tagging it with a placement directive so hot and cold data land in
// different reclaim unit handles.
//
//   usage: fdp_workload <dev> <fillbase|fillfdp|base|fdp> <churn_MB> [hot_space_pct] [hot_write_pct] [seed] [ws_pct]
//
//   ws_pct : working-set size as % of the device (default 90). Smaller = more
//            free space = higher effective over-provisioning = lighter GC.
//            This is the knob for the over-provisioning sweep.
//
//   fillbase / fillfdp : one-pass fill of the whole device in RANDOM order.
//       Random order matters: with no placement (fillbase) it scatters hot and
//       cold LBAs into the SAME erase lines, so later GC must copy the cold
//       survivors out of hot-dominated lines. fillfdp places hot LBAs in RUH 1
//       and cold LBAs in RUH 2, so lines are lifetime-pure from the start.
//
//   base / fdp : churn phase -- write <churn_MB> of random hot/cold traffic.
//       base sends no directive (all RUH 0); fdp places hot->RUH 1, cold->RUH 2.
//
//   hot_space_pct : % of the LBA space that is "hot"   (default 20)
//   hot_write_pct : % of churn writes that hit the hot region (default 90)
//
// Typical use (see fdp_eval.sh): fill, snapshot FDP stats, churn, snapshot
// again; WAF = delta-MBMW / delta-HBMW measured over the churn phase only.
//
// Build (in guest): gcc -O2 -o fdp_workload fdp_workload.c

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
		.cdw12 = (LBAS_PER_IO - 1) | (dtype << 20),  /* NLB[15:0], DTYPE[23:20] */
		.cdw13 = (dspec << 16),                       /* DSPEC[31:16] */
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

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <dev> <fillbase|fillfdp|base|fdp> <churn_MB> "
			"[hot_space_pct] [hot_write_pct] [seed]\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	const char *mode = argv[2];
	unsigned long churn_mb = strtoul(argv[3], NULL, 10);
	int hot_space_pct = (argc > 4) ? atoi(argv[4]) : 20;
	int hot_write_pct = (argc > 5) ? atoi(argv[5]) : 90;
	unsigned seed = (argc > 6) ? (unsigned)strtoul(argv[6], NULL, 10) : 1;
	int ws_pct = (argc > 7) ? atoi(argv[7]) : 90;
	if (ws_pct < 1 || ws_pct > 99) ws_pct = 90;

	int is_fill = (strncmp(mode, "fill", 4) == 0);
	int is_fdp = (strstr(mode, "fdp") != NULL);

	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) { perror("open"); return 1; }
	off_t dev_bytes = lseek(g_fd, 0, SEEK_END);
	if (dev_bytes <= 0) { perror("lseek"); return 1; }

	/* working set = ws_pct% of the device; the rest is free space (OP) */
	uint64_t tot_ios = ((uint64_t)(dev_bytes / LBA_BYTES) / LBAS_PER_IO) * ws_pct / 100;
	uint64_t hot_ios = tot_ios * hot_space_pct / 100;
	if (hot_ios == 0) hot_ios = 1;
	uint64_t cold_ios = tot_ios - hot_ios;
	if (cold_ios == 0) cold_ios = 1;

	if (posix_memalign(&g_buf, 4096, IO_BYTES)) { perror("memalign"); return 1; }
	memset(g_buf, 0xab, IO_BYTES);
	srand(seed);

	uint64_t done = 0, failed = 0;

	if (is_fill) {
		/* Fisher-Yates shuffle of [0, tot_ios), then write once each. */
		uint64_t *perm = malloc(tot_ios * sizeof(uint64_t));
		if (!perm) { perror("malloc"); return 1; }
		for (uint64_t i = 0; i < tot_ios; i++) perm[i] = i;
		for (uint64_t i = tot_ios - 1; i > 0; i--) {
			uint64_t j = ((uint64_t)rand() * 32768 + rand()) % (i + 1);
			uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
		}
		printf("dev=%s mode=%s FILL ios=%llu (hot=%llu cold=%llu)\n", dev, mode,
		       (unsigned long long)tot_ios, (unsigned long long)hot_ios,
		       (unsigned long long)cold_ios);
		for (uint64_t i = 0; i < tot_ios; i++) {
			uint64_t idx = perm[i];
			uint32_t dt = 0, ds = 0;
			if (is_fdp) { dt = DTYPE_PLACEMENT; ds = (idx < hot_ios) ? 1 : 2; }
			if (do_write(idx, dt, ds) == 0) done++; else failed++;
			if ((i & 0x3ff) == 0x3ff)
				fprintf(stderr, "\r  fill %llu/%llu", (unsigned long long)(i+1),
					(unsigned long long)tot_ios);
		}
		free(perm);
	} else {
		uint64_t n = (churn_mb * 1024ull * 1024ull) / IO_BYTES;
		printf("dev=%s mode=%s CHURN writes=%llu hot=%d%%->%d%% space\n", dev, mode,
		       (unsigned long long)n, hot_write_pct, hot_space_pct);
		for (uint64_t i = 0; i < n; i++) {
			int hot = (rand() % 100) < hot_write_pct;
			uint64_t idx;
			uint32_t dt = 0, ds = 0;
			if (hot) {
				idx = (uint64_t)rand() % hot_ios;
				if (is_fdp) { dt = DTYPE_PLACEMENT; ds = 1; }
			} else {
				idx = hot_ios + ((uint64_t)rand() % cold_ios);
				if (is_fdp) { dt = DTYPE_PLACEMENT; ds = 2; }
			}
			if (do_write(idx, dt, ds) == 0) done++; else failed++;
			if ((i & 0x3ff) == 0x3ff)
				fprintf(stderr, "\r  churn %llu/%llu", (unsigned long long)(i+1),
					(unsigned long long)n);
		}
	}

	fprintf(stderr, "\r");
	printf("done: ok=%llu fail=%llu\n", (unsigned long long)done,
	       (unsigned long long)failed);
	free(g_buf);
	close(g_fd);
	return failed ? 1 : 0;
}
