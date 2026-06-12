// SPDX-License-Identifier: GPL-2.0-only

#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "fdp_ftl.h"
#include "nvme_fdp.h"

/*
 * Endurance-group-scoped FDP state (single endurance group, single
 * configuration). Both the admin path (log pages, features) and the I/O
 * path (stats accounting) run on the dispatcher thread, so no locking.
 */
static struct {
	uint64_t hbmw;	/* host bytes written (FDP statistics log) */
	uint64_t mbmw;	/* media bytes written, incl. GC */
	uint64_t mbe;	/* media bytes erased */
	uint64_t runs;	/* reclaim unit nominal size in bytes */
	uint8_t ruh_attr[FDP_NR_RUH];
	struct {
		uint8_t type;
		bool enabled;
	} events[3];

	/* event ring: latest FDP_NR_EVENTS records, overwriting the oldest */
	struct nvme_fdp_event ring[FDP_NR_EVENTS];
	uint32_t ring_head;	/* next slot to write */
	uint32_t nr_events;	/* valid records, capped at FDP_NR_EVENTS */
} fdp_ctx = {
	.events = {
		{ .type = NVME_FDP_EVT_RU_NOT_FULLY_WRITTEN, .enabled = false },
		{ .type = NVME_FDP_EVT_INVALID_PID, .enabled = false },
		{ .type = NVME_FDP_EVT_IMPLICIT_MODIFIED_RU, .enabled = false },
	},
};

void fdp_init_ctx(uint64_t runs)
{
	fdp_ctx.hbmw = 0;
	fdp_ctx.mbmw = 0;
	fdp_ctx.mbe = 0;
	fdp_ctx.runs = runs;
	memset(fdp_ctx.ruh_attr, NVME_FDP_RUHA_UNUSED, sizeof(fdp_ctx.ruh_attr));
	memset(fdp_ctx.ring, 0, sizeof(fdp_ctx.ring));
	fdp_ctx.ring_head = 0;
	fdp_ctx.nr_events = 0;
}

static void fdp_emit_event(uint8_t type, uint8_t flags, uint16_t pid, uint32_t nsid)
{
	struct nvme_fdp_event *ev;
	bool enabled = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(fdp_ctx.events); i++) {
		if (fdp_ctx.events[i].type == type) {
			enabled = fdp_ctx.events[i].enabled;
			break;
		}
	}
	if (!enabled)
		return;

	ev = &fdp_ctx.ring[fdp_ctx.ring_head];
	memset(ev, 0, sizeof(*ev));
	ev->type = type;
	ev->flags = flags;
	ev->pid = cpu_to_le16(pid);
	ev->timestamp = cpu_to_le64(local_clock());
	ev->nsid = cpu_to_le32(nsid);

	fdp_ctx.ring_head = (fdp_ctx.ring_head + 1) % FDP_NR_EVENTS;
	if (fdp_ctx.nr_events < FDP_NR_EVENTS)
		fdp_ctx.nr_events++;
}

static void __fill_fdp_log_configs(uint8_t *log)
{
	struct nvme_fdp_config_log *hdr = (void *)log;
	struct nvme_fdp_config_desc *desc = (void *)(log + sizeof(*hdr));
	struct nvme_fdp_ruh_desc *ruhs = (void *)(log + sizeof(*hdr) + sizeof(*desc));
	const uint16_t dsze = sizeof(*desc) + FDP_NR_RUH * sizeof(*ruhs);
	int i;

	hdr->n = cpu_to_le16(0); /* one configuration, 0-based */
	hdr->version = 0;
	hdr->size = cpu_to_le32(sizeof(*hdr) + dsze);

	desc->dsze = cpu_to_le16(dsze);
	desc->fdpa = NVME_FDP_CONFIG_FDPA_VALID; /* RGIF = 0: PID is the RUH index */
	desc->vss = 0;
	desc->nrg = cpu_to_le32(FDP_NR_RG);
	desc->nruh = cpu_to_le16(FDP_NR_RUH);
	desc->maxpids = cpu_to_le16(FDP_NR_RUH - 1);
	desc->nns = cpu_to_le32(NR_NAMESPACES);
	desc->runs = cpu_to_le64(fdp_ctx.runs);
	desc->erutl = 0;

	for (i = 0; i < FDP_NR_RUH; i++)
		ruhs[i].ruht = NVME_FDP_RUHT_INITIALLY_ISOLATED;
}

static void __fill_fdp_log_ruh_usage(uint8_t *log)
{
	struct nvme_fdp_ruhu_log *hdr = (void *)log;
	struct nvme_fdp_ruhu_desc *descs = (void *)(log + sizeof(*hdr));
	int i;

	hdr->nruh = cpu_to_le16(FDP_NR_RUH);
	for (i = 0; i < FDP_NR_RUH; i++)
		descs[i].ruha = fdp_ctx.ruh_attr[i];
}

static void __fill_fdp_log_stats(uint8_t *log)
{
	struct nvme_fdp_stats_log *stats = (void *)log;

	/* 128-bit little-endian counters; we only ever fill the low 64 bits */
	*(__le64 *)stats->hbmw = cpu_to_le64(fdp_ctx.hbmw);
	*(__le64 *)stats->mbmw = cpu_to_le64(fdp_ctx.mbmw);
	*(__le64 *)stats->mbe = cpu_to_le64(fdp_ctx.mbe);
}

void fdp_get_log_page(uint8_t lid, void *buf, uint32_t len)
{
	static uint8_t log[PAGE_SIZE];

	memset(log, 0, sizeof(log));

	switch (lid) {
	case NVME_LOG_FDP_CONFIGS:
		__fill_fdp_log_configs(log);
		break;
	case NVME_LOG_FDP_RUH_USAGE:
		__fill_fdp_log_ruh_usage(log);
		break;
	case NVME_LOG_FDP_STATS:
		__fill_fdp_log_stats(log);
		break;
	case NVME_LOG_FDP_EVENTS: {
		struct nvme_fdp_events_log *hdr = (void *)log;
		struct nvme_fdp_event *out = (void *)(log + sizeof(*hdr));
		uint32_t i;

		/* 64-byte header + 63 * 64-byte records == PAGE_SIZE exactly */
		hdr->n = cpu_to_le32(fdp_ctx.nr_events);
		for (i = 0; i < fdp_ctx.nr_events; i++) {
			uint32_t slot = (fdp_ctx.ring_head + FDP_NR_EVENTS - 1 - i) %
					FDP_NR_EVENTS;
			out[i] = fdp_ctx.ring[slot]; /* most recent first */
		}
		break;
	}
	default:
		NVMEV_ASSERT(0);
	}

	/* Single-PRP transfer, same constraint as the other log pages */
	memcpy(buf, log, min_t(uint32_t, len, PAGE_SIZE));
}

void fdp_get_feature_events(uint32_t dw11, void *buf, uint32_t *result)
{
	struct nvme_fdp_supported_event_desc *desc = buf;
	int i;

	*result = ARRAY_SIZE(fdp_ctx.events);

	if (!desc)
		return;

	for (i = 0; i < ARRAY_SIZE(fdp_ctx.events); i++) {
		desc[i].evt = fdp_ctx.events[i].type;
		desc[i].evta = fdp_ctx.events[i].enabled ? 0x1 : 0x0;
	}
}

void fdp_set_feature_events(uint32_t dw11, void *buf, uint32_t *result)
{
	/* CDW11 bits 31:16 carry the number of event descriptors supplied */
	uint32_t noet = (dw11 >> 16) & 0xFF;
	struct nvme_fdp_supported_event_desc *desc = buf;
	int i, j;

	*result = 0;

	if (!desc)
		return;

	for (i = 0; i < noet; i++) {
		for (j = 0; j < ARRAY_SIZE(fdp_ctx.events); j++) {
			if (fdp_ctx.events[j].type == desc[i].evt) {
				fdp_ctx.events[j].enabled = desc[i].evta & 0x1;
				(*result)++;
			}
		}
	}
}

static inline bool last_pg_in_wordline(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct fdp_ftl *fdp_ftl)
{
	return (fdp_ftl->lm.free_line_cnt <= fdp_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct fdp_ftl *fdp_ftl)
{
	return fdp_ftl->lm.free_line_cnt <= fdp_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn)
{
	return fdp_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < fdp_ftl->ssd->sp.tt_pgs);
	fdp_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(fdp_ftl, ppa);

	return fdp_ftl->rmap[pgidx];
}

static inline void set_rmap_ent(struct fdp_ftl *fdp_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(fdp_ftl, ppa);

	fdp_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct fdp_line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct fdp_line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct fdp_line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct fdp_line *)a)->pos = pos;
}

static inline void consume_write_credit(struct fdp_ftl *fdp_ftl)
{
	fdp_ftl->wfc.write_credits--;
}

static void foreground_gc(struct fdp_ftl *fdp_ftl);

static inline void check_and_refill_write_credit(struct fdp_ftl *fdp_ftl)
{
	struct fdp_write_flow_control *wfc = &(fdp_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(fdp_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct fdp_line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct fdp_line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct fdp_line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.ruh = -1,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_lines(struct fdp_ftl *fdp_ftl)
{
	pqueue_free(fdp_ftl->lm.victim_line_pq);
	vfree(fdp_ftl->lm.lines);
}

static void init_write_flow_control(struct fdp_ftl *fdp_ftl)
{
	struct fdp_write_flow_control *wfc = &(fdp_ftl->wfc);
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct fdp_line *get_next_free_line(struct fdp_ftl *fdp_ftl)
{
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct fdp_line *curline =
		list_first_entry_or_null(&lm->free_line_list, struct fdp_line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

static struct fdp_write_pointer *__get_wp(struct fdp_ftl *ftl, uint32_t io_type, uint32_t ruh)
{
	if (io_type == USER_IO) {
		NVMEV_ASSERT(ruh < FDP_NR_RUH);
		return &ftl->wp_ruh[ruh];
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}

static void prepare_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t io_type, uint32_t ruh)
{
	struct fdp_write_pointer *wp = __get_wp(fdp_ftl, io_type, ruh);
	struct fdp_line *curline = get_next_free_line(fdp_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	curline->ruh = (io_type == USER_IO) ? (int)ruh : -1;

	*wp = (struct fdp_write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t io_type, uint32_t ruh)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct fdp_write_pointer *wpp = __get_wp(fdp_ftl, io_type, ruh);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	if (wpp->curline->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(fdp_ftl);
	wpp->curline->ruh = (io_type == USER_IO) ? (int)ruh : -1;
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct fdp_ftl *fdp_ftl, uint32_t io_type, uint32_t ruh)
{
	struct ppa ppa;
	struct fdp_write_pointer *wp = __get_wp(fdp_ftl, io_type, ruh);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->maptbl);
}

static void init_rmap(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->rmap);
}

static void fdp_init_ftl(struct fdp_ftl *fdp_ftl, struct fdpparams *cpp, struct ssd *ssd)
{
	fdp_ftl->cp = *cpp;

	fdp_ftl->ssd = ssd;

	init_maptbl(fdp_ftl);
	init_rmap(fdp_ftl);
	init_lines(fdp_ftl);

	{
		uint32_t ruh;

		for (ruh = 0; ruh < FDP_NR_RUH; ruh++)
			prepare_write_pointer(fdp_ftl, USER_IO, ruh);
	}
	prepare_write_pointer(fdp_ftl, GC_IO, 0);

	init_write_flow_control(fdp_ftl);

	NVMEV_INFO("Init FDP FTL instance with %d channels (%ld pages)\n",
		   fdp_ftl->ssd->sp.nchs, fdp_ftl->ssd->sp.tt_pgs);

	return;
}

static void fdp_remove_ftl(struct fdp_ftl *fdp_ftl)
{
	remove_lines(fdp_ftl);
	remove_rmap(fdp_ftl);
	remove_maptbl(fdp_ftl);
}

static void fdp_init_params(struct fdpparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	/* every RUH keeps an open line, plus one for GC */
	cpp->gc_thres_lines = FDP_NR_RUH + 1;
	cpp->gc_thres_lines_high = FDP_NR_RUH + 1;
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct fdpparams cpp;
	struct fdp_ftl *fdp_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	fdp_init_params(&cpp);

	/* One reclaim unit = one line (superblock) of an FTL partition */
	fdp_init_ctx((uint64_t)spp.pgs_per_line * spp.pgsz);

	fdp_ftls = kmalloc(sizeof(struct fdp_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		fdp_init_ftl(&fdp_ftls[i], &cpp, ssd);
	}

	for (i = 1; i < nr_parts; i++) {
		kfree(fdp_ftls[i].ssd->pcie->perf_model);
		kfree(fdp_ftls[i].ssd->pcie);
		kfree(fdp_ftls[i].ssd->write_buffer);

		fdp_ftls[i].ssd->pcie = fdp_ftls[0].ssd->pcie;
		fdp_ftls[i].ssd->write_buffer = fdp_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)fdp_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	ns->proc_io_cmd = fdp_proc_nvme_io_cmd;

	NVMEV_INFO("FDP FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void fdp_remove_namespace(struct nvmev_ns *ns)
{
	struct fdp_ftl *fdp_ftls = (struct fdp_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	for (i = 1; i < nr_parts; i++) {
		fdp_ftls[i].ssd->pcie = NULL;
		fdp_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		fdp_remove_ftl(&fdp_ftls[i]);
		ssd_remove(fdp_ftls[i].ssd);
		kfree(fdp_ftls[i].ssd);
	}

	kfree(fdp_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct fdp_ftl *fdp_ftl, uint64_t lpn)
{
	return (lpn < fdp_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct fdp_line *get_line(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	return &(fdp_ftl->lm.lines[ppa->g.blk]);
}

static void mark_page_invalid(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct fdp_line *line;

	pg = get_pg(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	blk = get_blk(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	line = get_line(fdp_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	if (line->pos) {
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct fdp_line *line;

	pg = get_pg(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	blk = get_blk(fdp_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	line = get_line(fdp_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_block *blk = get_blk(fdp_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;

	fdp_ctx.mbe += (uint64_t)spp->pgs_per_blk * spp->pgsz;
}

static void gc_read_page(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdpparams *cpp = &fdp_ftl->cp;
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(fdp_ftl->ssd, &gcr);
	}
}

static uint64_t gc_write_page(struct fdp_ftl *fdp_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdpparams *cpp = &fdp_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(fdp_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(fdp_ftl, lpn));
	new_ppa = get_new_page(fdp_ftl, GC_IO, 0);
	set_maptbl_ent(fdp_ftl, lpn, &new_ppa);
	set_rmap_ent(fdp_ftl, lpn, &new_ppa);

	mark_page_valid(fdp_ftl, &new_ppa);

	advance_write_pointer(fdp_ftl, GC_IO, 0);

	fdp_ctx.mbmw += spp->pgsz; /* GC traffic counts as media-written only */

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(fdp_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(fdp_ftl->ssd, &gcw);
	}

	return 0;
}

static struct fdp_line *select_victim_line(struct fdp_ftl *fdp_ftl, bool force)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct fdp_line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	return victim_line;
}

static void clean_one_block(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(fdp_ftl->ssd, ppa);
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(fdp_ftl, ppa);
			gc_write_page(fdp_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(fdp_ftl->ssd, ppa)->vpc == cnt);
}

static void clean_one_flashpg(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct fdpparams *cpp = &fdp_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(fdp_ftl->ssd, &ppa_copy);
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(fdp_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(fdp_ftl->ssd, &ppa_copy);

		if (pg_iter->status == PG_VALID) {
			gc_write_page(fdp_ftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct fdp_ftl *fdp_ftl, struct ppa *ppa)
{
	struct fdp_line_mgmt *lm = &fdp_ftl->lm;
	struct fdp_line *line = get_line(fdp_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	line->ruh = -1;
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct fdp_ftl *fdp_ftl, bool force)
{
	struct fdp_line *victim_line = NULL;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(fdp_ftl, force);
	if (!victim_line) {
		return -1;
	}

	/* GC is about to move data the host placed via this RUH */
	if (victim_line->ruh >= 0)
		fdp_emit_event(NVME_FDP_EVT_IMPLICIT_MODIFIED_RU, NVME_FDP_EVENT_F_PIV,
			       victim_line->ruh, 0);

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, fdp_ftl->lm.victim_line_cnt,
		    fdp_ftl->lm.full_line_cnt, fdp_ftl->lm.free_line_cnt);

	fdp_ftl->wfc.credits_to_refill = victim_line->ipc;

	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(fdp_ftl->ssd, &ppa);
				clean_one_flashpg(fdp_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct fdpparams *cpp = &fdp_ftl->cp;

					mark_block_free(fdp_ftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(fdp_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	mark_line_free(fdp_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct fdp_ftl *fdp_ftl)
{
	if (should_gc_high(fdp_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		do_gc(fdp_ftl, true);
	}
}

static bool is_same_flash_page(struct fdp_ftl *fdp_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool fdp_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct fdp_ftl *fdp_ftls = (struct fdp_ftl *)ns->ftls;
	struct fdp_ftl *fdp_ftl = &fdp_ftls[0];
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(fdp_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		fdp_ftl = &fdp_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(fdp_ftl, start_lpn / nr_parts);

		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(fdp_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(fdp_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(fdp_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool fdp_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct fdp_ftl *fdp_ftls = (struct fdp_ftl *)ns->ftls;
	struct fdp_ftl *fdp_ftl = &fdp_ftls[0];

	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct buffer *wbuf = fdp_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	const uint16_t dtype = (cmd->rw.control >> NVME_RW_DTYPE_SHIFT) & NVME_RW_DTYPE_MASK;
	const uint16_t dspec = cmd->rw.dsmgmt >> 16;
	uint32_t ruh = 0;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	/* Placement directive routing (TP 4146a). With RGIF = 0 the whole
	 * DSPEC field is the reclaim unit handle index. */
	if (dtype == NVME_DIRECTIVE_PLACEMENT) {
		if (dspec >= FDP_NR_RUH) {
			fdp_emit_event(NVME_FDP_EVT_INVALID_PID,
				       NVME_FDP_EVENT_F_PIV | NVME_FDP_EVENT_F_NSIDV,
				       dspec, cmd->rw.nsid);
			ret->status = NVME_SC_INVALID_FIELD;
			ret->nsecs_target = req->nsecs_start;
			return true;
		}
		ruh = dspec;
		fdp_ctx.ruh_attr[ruh] = NVME_FDP_RUHA_HOST;
	} else if (fdp_ctx.ruh_attr[0] == NVME_FDP_RUHA_UNUSED) {
		/* non-placement writes implicitly use RUH 0 */
		fdp_ctx.ruh_attr[0] = NVME_FDP_RUHA_CTRL;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	/* FDP statistics: host write traffic also reaches the media */
	fdp_ctx.hbmw += LBA_TO_BYTE(nr_lba);
	fdp_ctx.mbmw += LBA_TO_BYTE(nr_lba);

	nsecs_latest =
		ssd_advance_write_buffer(fdp_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		fdp_ftl = &fdp_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(fdp_ftl, local_lpn);
		if (mapped_ppa(&ppa)) {
			mark_page_invalid(fdp_ftl, &ppa);
			set_rmap_ent(fdp_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(fdp_ftl, &ppa));
		}

		ppa = get_new_page(fdp_ftl, USER_IO, ruh);
		set_maptbl_ent(fdp_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(fdp_ftl, &ppa));
		set_rmap_ent(fdp_ftl, local_lpn, &ppa);

		mark_page_valid(fdp_ftl, &ppa);

		advance_write_pointer(fdp_ftl, USER_IO, ruh);

		if (last_pg_in_wordline(fdp_ftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit(fdp_ftl);
		check_and_refill_write_credit(fdp_ftl);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		ret->nsecs_target = nsecs_latest;
	} else {
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void fdp_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct fdp_ftl *fdp_ftls = (struct fdp_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(fdp_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool fdp_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!fdp_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!fdp_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		fdp_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
