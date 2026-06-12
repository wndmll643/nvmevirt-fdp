// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_NVME_FDP_H
#define _NVMEVIRT_NVME_FDP_H

#include <linux/types.h>

/* Definitions for NVMe Flexible Data Placement (TP 4146a). */

/* Identify Controller: CTRATT bit 4 = Endurance Groups supported */
#define NVME_CTRL_CTRATT_ENDGRPS	(1U << 4)
/* Identify Controller: CTRATT bit 19 = Flexible Data Placement Supported */
#define NVME_CTRL_CTRATT_FDPS		(1U << 19)

/* Identify Namespace: NSFEAT bit 4 = NPWG/NPWA/NPDG/NPDA/NOWS are valid */
#define NVME_NS_FEAT_IO_OPT		(1U << 4)

/* Endurance group used by the single FDP-enabled namespace. */
#define NVMEV_FDP_ENDGID		1

/* Log page identifiers (endurance-group scoped) */
#define NVME_LOG_FDP_CONFIGS		0x20
#define NVME_LOG_FDP_RUH_USAGE		0x21
#define NVME_LOG_FDP_STATS		0x22
#define NVME_LOG_FDP_EVENTS		0x23

/* Feature identifiers */
#define NVME_FEAT_FDP			0x1d
#define NVME_FEAT_FDP_EVENTS		0x1e

/* FDP Attributes (FDPA) in the configuration descriptor.
 * Bits 3:0 = RGIF (placement ID bits used for the reclaim group; 0 when
 * there is a single reclaim group), bit 7 = configuration valid. */
#define NVME_FDP_CONFIG_FDPA_VALID	(1U << 7)

/* Reclaim Unit Handle types */
#define NVME_FDP_RUHT_INITIALLY_ISOLATED	1
#define NVME_FDP_RUHT_PERSISTENTLY_ISOLATED	2

/* Reclaim Unit Handle usage attributes (RUH Usage log) */
#define NVME_FDP_RUHA_UNUSED		0
#define NVME_FDP_RUHA_HOST		1
#define NVME_FDP_RUHA_CTRL		2

/* Directive fields in the write command: DTYPE is CDW12 bits 23:20
 * (bits 7:4 of nvme_rw_command.control), DSPEC is CDW13 bits 31:16
 * (upper half of nvme_rw_command.dsmgmt). */
#define NVME_RW_DTYPE_SHIFT		4
#define NVME_RW_DTYPE_MASK		0xF
#define NVME_DIRECTIVE_PLACEMENT	2

/* FDP event types */
#define NVME_FDP_EVT_RU_NOT_FULLY_WRITTEN	0x0
#define NVME_FDP_EVT_RU_TIME_LIMIT		0x1
#define NVME_FDP_EVT_CTRL_RESET_MODIFIED_RU	0x2
#define NVME_FDP_EVT_INVALID_PID		0x3
#define NVME_FDP_EVT_MEDIA_REALLOCATED		0x80
#define NVME_FDP_EVT_IMPLICIT_MODIFIED_RU	0x81

/* FDP event record flags */
#define NVME_FDP_EVENT_F_PIV		(1U << 0)	/* placement ID valid */
#define NVME_FDP_EVENT_F_NSIDV		(1U << 1)	/* nsid valid */
#define NVME_FDP_EVENT_F_LV		(1U << 2)	/* location valid */

/* FDP Configurations log (LID 0x20): 16-byte header followed by one or
 * more configuration descriptors, each trailed by per-RUH descriptors. */
struct nvme_fdp_config_log {
	__le16 n;		/* number of configurations, 0-based */
	__u8 version;
	__u8 rsvd3;
	__le32 size;		/* total log size in bytes */
	__u8 rsvd8[8];
} __packed;

struct nvme_fdp_ruh_desc {
	__u8 ruht;
	__u8 rsvd1[3];
} __packed;

struct nvme_fdp_config_desc {
	__le16 dsze;		/* descriptor size incl. RUH descriptors */
	__u8 fdpa;
	__u8 vss;		/* vendor specific size */
	__le32 nrg;		/* number of reclaim groups */
	__le16 nruh;		/* number of RUHs */
	__le16 maxpids;		/* max placement IDs, 0-based */
	__le32 nns;		/* number of namespaces supported */
	__le64 runs;		/* reclaim unit nominal size (bytes) */
	__le32 erutl;		/* estimated RU time limit (0 = not reported) */
	__u8 rsvd28[36];
	/* struct nvme_fdp_ruh_desc ruhs[nruh] follows */
} __packed;

/* RUH Usage log (LID 0x21): 8-byte header + one descriptor per RUH */
struct nvme_fdp_ruhu_log {
	__le16 nruh;
	__u8 rsvd2[6];
	/* struct nvme_fdp_ruhu_desc ruhus[nruh] follows */
} __packed;

struct nvme_fdp_ruhu_desc {
	__u8 ruha;
	__u8 rsvd1[7];
} __packed;

/* FDP Statistics log (LID 0x22): three 128-bit counters */
struct nvme_fdp_stats_log {
	__u8 hbmw[16];		/* host bytes with metadata written */
	__u8 mbmw[16];		/* media bytes with metadata written */
	__u8 mbe[16];		/* media bytes erased */
	__u8 rsvd48[16];
} __packed;

/* FDP Events log (LID 0x23): 64-byte header; 64-byte event records follow */
struct nvme_fdp_events_log {
	__le32 n;		/* number of event entries returned */
	__u8 rsvd4[60];
} __packed;

struct nvme_fdp_event {
	__u8 type;
	__u8 flags;
	__le16 pid;
	__le64 timestamp;
	__le32 nsid;
	__u8 type_specific[16];
	__u8 rsvd32[32];
} __packed;

/* Descriptor exchanged by Get/Set Features FID 0x1E (FDP events) */
struct nvme_fdp_supported_event_desc {
	__u8 evt;
	__u8 evta;		/* bit 0: event enabled */
} __packed;

/* FDP admin handlers (implemented in fdp_ftl.c; linked on FDP builds only) */
void fdp_get_log_page(uint8_t lid, void *buf, uint32_t len);
void fdp_get_feature_events(uint32_t dw11, void *buf, uint32_t *result);
void fdp_set_feature_events(uint32_t dw11, void *buf, uint32_t *result);
void fdp_init_ctx(uint64_t runs);

#endif /* _NVMEVIRT_NVME_FDP_H */
