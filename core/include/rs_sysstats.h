#ifndef RS_SYSSTATS_H
#define RS_SYSSTATS_H

// Live host stats for the panel's Server view — CPU, memory, disk, load,
// uptime, OS — one snapshot with per-platform backends behind it. CPU percent
// needs two samples to compute a delta, so a context
// carries the previous sample (and the running peaks) between polls.

#include "rs_common.h"
#include "rs_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_sysstats rs_sysstats;

rs_sysstats *rs_sysstats_create(void);
void rs_sysstats_destroy(rs_sysstats *stats);

// Builds the `system` object the SSE payload carries: cpuPercent, cpuModel,
// coreCount, loadAverage, memoryUsedBytes, totalMemoryBytes, diskTotalBytes,
// diskAvailableBytes, uptimeSeconds, osVersion, and the running peaks
// (peakCPUPercent, peakMemoryBytes). Freed with rs_json_free.
rs_json *rs_sysstats_snapshot(rs_sysstats *stats);

#ifdef __cplusplus
}
#endif

#endif  // RS_SYSSTATS_H
