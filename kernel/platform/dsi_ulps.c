/* KolibriOS for ARM: the DSI lanes to the ultra-low power state (ULPS)
 * and back, the Qualcomm DSI controller's sequence (dsi_ulps.h).
 *
 * With the panel asleep and the scan-out stopped the lanes still sit in
 * LP-11 (stop state), their transmitters on. In ULPS the PHY's lanes are
 * off but for their detectors. As Linux's msm DSI driver does it (the
 * MDSS one, mdss_dsi_ulps_config_default and mdss_dsi_wait_for_lane_idle;
 * the later SDE driver's dsi_ctrl_hw_cmn_ulps_request/_exit the same),
 * the register layout of mainline's dsi.xml (offsets from the 6G
 * controller's shifted base, as pro1x.c's DSI_REG):
 *
 *   FIFO_STATUS 0x08   12 DLN0 LP empty, 16 DLN0 HS empty, 20, 24, 28: DLN1-3
 *   LANE_STATUS 0xa4   0-3 DLN0-3 stop state, 4 the clock lane's;
 *                      8-11 DLN0-3 ULPS_ACTIVE_NOT, 12 the clock lane's
 *   LANE_CTRL   0xa8   0-3, 4: ULPS request (data lanes, the clock lane);
 *                      8-12: ULPS exit; 16-20: force the stop state;
 *                      24 HS_REQ_SEL_PHY, 28 CLKLN_HS_FORCE_REQUEST
 *   CTRL        0x00   4-7: the data lanes in use (dsi_ctrl_enable's)
 *
 * In: the clock lane's forced high speed off (mdss_dsi_pre_clkoff_cb),
 * the lanes idle (a FIFO empty, a lane in its stop state, 100 us at
 * most), the request for the data lanes in use and the clock lane, 100
 * us, then their ULPS_ACTIVE_NOT bits must be clear. Out: the exit bits,
 * 1 ms (the hardware's minimum), the stop state forced, the request
 * register cleared, 100 us before data; then LANE_CTRL as it was (the
 * clock lane's high speed again). */
#include "dsi_ulps.h"

#define FIFO_STATUS 0x08
#define LANE_STATUS 0xa4
#define LANE_CTRL 0xa8
#define CTRL 0x00
#define LANE_REQ_BITS 0x001f1f1fu      /* request, exit, force stop */
#define CLKLN_HS_FORCE (1u << 28)

/* the lanes for the request bits: the data lanes in use, the clock lane */
static u32 active_lanes(const struct dsi_regs *r)
{
	u32 data = r->rd(r->ctx, CTRL) >> 4 & 0xf;
	return (data ? data : 0xf) | 1u << 4;
}

/* until (reg & mask) is not 0, at most 100 us in steps of 10 */
static bool poll_any(const struct dsi_regs *r, u32 off, u32 mask, u32 *val)
{
	for (int i = 0; i <= 10; i++) {
		*val = r->rd(r->ctx, off);
		if (*val & mask) return true;
		r->delay_us(10);
	}
	return false;
}

int dsi_ulps_enter(const struct dsi_regs *r, struct dsi_ulps *s)
{
	u32 lanes = active_lanes(r), v;
	s->in = false;
	s->lane_ctrl = r->rd(r->ctx, LANE_CTRL);
	u32 base = s->lane_ctrl & ~(LANE_REQ_BITS | CLKLN_HS_FORCE);
	/* the clock lane out of its forced high speed first */
	if (s->lane_ctrl & CLKLN_HS_FORCE) r->wr(r->ctx, LANE_CTRL, base);
	u32 fifo = 0, stop = lanes & 0xf;
	for (int i = 0; i < 4; i++)
		if (lanes & 1u << i) fifo |= i ? 1u << (16 + 4 * i) : 1u << 12 | 1u << 16;
	if (!poll_any(r, FIFO_STATUS, fifo, &v)) {
		s->status = v;
		r->wr(r->ctx, LANE_CTRL, s->lane_ctrl);
		return DSI_ULPS_FIFO;
	}
	if (!poll_any(r, LANE_STATUS, stop, &v)) {
		s->status = v;
		r->wr(r->ctx, LANE_CTRL, s->lane_ctrl);
		return DSI_ULPS_NOT_IDLE;
	}
	r->wr(r->ctx, LANE_CTRL, base | lanes);
	r->delay_us(100);
	s->status = r->rd(r->ctx, LANE_STATUS);
	s->lanes = lanes;
	if (s->status & lanes << 8) {
		/* not all of them: out again, as they were */
		dsi_ulps_exit(r, s);
		return DSI_ULPS_REFUSED;
	}
	s->in = true;
	return 0;
}

int dsi_ulps_exit(const struct dsi_regs *r, struct dsi_ulps *s)
{
	u32 lanes = s->lanes, base = s->lane_ctrl & ~(LANE_REQ_BITS | CLKLN_HS_FORCE);
	r->wr(r->ctx, LANE_CTRL, base | lanes << 8);
	r->delay_us(1000);
	r->wr(r->ctx, LANE_CTRL, base | lanes << 16);
	r->wr(r->ctx, LANE_CTRL, base);
	r->delay_us(100);
	s->status = r->rd(r->ctx, LANE_STATUS);
	r->wr(r->ctx, LANE_CTRL, s->lane_ctrl & ~LANE_REQ_BITS);
	bool out = (s->status & lanes << 8) == lanes << 8;
	s->in = false;
	return out ? 0 : DSI_ULPS_STUCK;
}
