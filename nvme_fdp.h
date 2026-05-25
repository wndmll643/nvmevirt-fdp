// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_NVME_FDP_H
#define _NVMEVIRT_NVME_FDP_H

#include <linux/types.h>

/* Definitions for NVMe Flexible Data Placement (TP 4146a). */

/* Identify Controller: CTRATT bit 19 = Flexible Data Placement Supported */
#define NVME_CTRL_CTRATT_FDPS		(1U << 19)

/* Endurance group used by the single FDP-enabled namespace. */
#define NVMEV_FDP_ENDGID		1

#endif /* _NVMEVIRT_NVME_FDP_H */
