/* KolibriOS for ARM: a Qualcomm DSI controller's lanes to ULPS and back
 * (dsi_ulps.c), with its registers by callbacks (tests/power: a model) */
#ifndef DSI_ULPS_H
#define DSI_ULPS_H

#ifdef DSI_HOST
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
#else
#include "kernel.h"
#endif

struct dsi_regs {
	void *ctx;
	u32 (*rd)(void *ctx, u32 off);          /* from the 6G controller's shifted base */
	void (*wr)(void *ctx, u32 off, u32 v);
	void (*delay_us)(u32 us);
};

struct dsi_ulps {
	bool in;                                /* the lanes in ULPS */
	u32 lanes;                              /* the request's bits: data lanes 0-3, the clock lane 4 */
	u32 lane_ctrl;                          /* LANE_CTRL before, restored after */
	u32 status;                             /* LANE_STATUS (or FIFO_STATUS) last read */
};

enum { DSI_ULPS_FIFO = -1, DSI_ULPS_NOT_IDLE = -2, DSI_ULPS_REFUSED = -3, DSI_ULPS_STUCK = -4 };

/* 0: in ULPS; DSI_ULPS_FIFO, _NOT_IDLE: the lanes busy, left as they
 * were; _REFUSED: not every lane went in, all out again */
int dsi_ulps_enter(const struct dsi_regs *r, struct dsi_ulps *s);
/* 0: out, every lane; DSI_ULPS_STUCK: a lane still in ULPS */
int dsi_ulps_exit(const struct dsi_regs *r, struct dsi_ulps *s);

#endif
