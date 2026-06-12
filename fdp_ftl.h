// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_FDP_FTL_H
#define _NVMEVIRT_FDP_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

/* The struct definitions are sized by FDP_NR_RUH, which only exists for the
 * FDP target. Other targets just need the namespace entry points below. */
#if (BASE_SSD == SAMSUNG_FDP)

struct fdpparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent;
};

struct fdp_line {
	int id;
	int ipc;
	int vpc;
	struct list_head entry;
	size_t pos;
};

struct fdp_write_pointer {
	struct fdp_line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct fdp_line_mgmt {
	struct fdp_line *lines;

	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct fdp_write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

struct fdp_ftl {
	struct ssd *ssd;

	struct fdpparams cp;
	struct ppa *maptbl;
	uint64_t *rmap;
	/* one user write pointer per reclaim unit handle */
	struct fdp_write_pointer wp_ruh[FDP_NR_RUH];
	struct fdp_write_pointer gc_wp;
	struct fdp_line_mgmt lm;
	struct fdp_write_flow_control wfc;
};

#endif /* BASE_SSD == SAMSUNG_FDP */

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher);

void fdp_remove_namespace(struct nvmev_ns *ns);

bool fdp_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			  struct nvmev_result *ret);

#endif
