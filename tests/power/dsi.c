/* KolibriOS for ARM: kernel/platform/dsi_ulps.c on the PC against a
 * model of a Qualcomm DSI controller's lanes (make -C tests/power host).
 *
 * The model's lanes (four data lanes, the clock lane) sit in their stop
 * state (LP-11) when idle; a ULPS request takes a lane in only from its
 * stop state, the clock lane only out of its forced high speed (LANE_CTRL
 * bit 28); an exit takes a lane out once the stop state is forced at
 * least 1 ms after the exit request. Its violations: a request with a
 * FIFO not empty or the clock lane in high speed, an exit shorter than
 * 1 ms, a request for a lane that is not in use, LANE_CTRL left other
 * than it was. The cases: in and out, the FIFO busy, no lane idle, a
 * lane that will not go in, a lane stuck in ULPS, two data lanes. */
#include <stdio.h>
#include <string.h>
#include "dsi_ulps.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct {
	u32 ctrl, lane_ctrl, used;
	int ulps[5], stop[5];
	unsigned long long now, exit_at;
	int fifo_busy, none_idle, refuse, stuck;
	int violations;
} m;

static void violation(const char *what)
{
	m.violations++;
	printf("model: VIOLATION: %s\n", what);
}

static u32 mrd(void *ctx, u32 off)
{
	u32 v = 0;
	switch (off) {
	case 0x00: return m.ctrl;
	case 0x08: return m.fifo_busy ? 0 : 1u << 12 | 1u << 16 | 1u << 20 | 1u << 24 | 1u << 28;
	case 0xa4:
		for (int i = 0; i < 5; i++) {
			if (m.stop[i] && !m.none_idle) v |= 1u << i;
			if (!m.ulps[i]) v |= 1u << (8 + i);
		}
		return v;
	case 0xa8: return m.lane_ctrl;
	}
	return 0;
}

static void mwr(void *ctx, u32 off, u32 v)
{
	if (off != 0xa8) {
		violation("a register other than LANE_CTRL written");
		return;
	}
	m.lane_ctrl = v;
	u32 req = v & 0x1f, ex = v >> 8 & 0x1f, force = v >> 16 & 0x1f;
	if (req) {
		if (m.fifo_busy) violation("ULPS asked for with a FIFO not empty");
		if (v & 1u << 28) violation("ULPS asked for with the clock lane forced to high speed");
		if (req & ~m.used) violation("ULPS asked for a lane not in use");
		for (int i = 0; i < 5; i++)
			if (req & 1u << i && m.stop[i] && !m.none_idle && i != m.refuse) m.ulps[i] = 1, m.stop[i] = 0;
	}
	if (ex) m.exit_at = m.now;
	if (force) {
		if (m.now - m.exit_at < 1000) violation("the stop state forced less than 1 ms after the exit request");
		for (int i = 0; i < 5; i++)
			if (force & 1u << i && i != m.stuck) m.ulps[i] = 0, m.stop[i] = 1;
	}
}

static void delay_us(u32 us) { m.now += us; }

static const struct dsi_regs regs = { NULL, mrd, mwr, delay_us };

static void model(u32 data_lanes, u32 lane_ctrl)
{
	memset(&m, 0, sizeof(m));
	m.ctrl = 1 | 1u << 1 | data_lanes << 4 | 1u << 8;      /* enabled, video mode, the lanes, the clock */
	m.lane_ctrl = lane_ctrl;
	m.used = data_lanes | 1u << 4;
	m.refuse = m.stuck = -1;
	for (int i = 0; i < 5; i++) m.stop[i] = 1;
}

static int in_ulps(void)
{
	int n = 0;
	for (int i = 0; i < 5; i++) n += m.ulps[i];
	return n;
}

int main(void)
{
	struct dsi_ulps s;
	/* in and out, as Linux leaves the controller: the clock lane forced
	 * to high speed (continuous clock), HS_REQ_SEL_PHY */
	model(0xf, 1u << 28 | 1u << 24);
	CHECK(dsi_ulps_enter(&regs, &s) == 0 && s.in && in_ulps() == 5, "all five lanes in ULPS (status %x)", s.status);
	CHECK(s.lanes == 0x1f && !(m.lane_ctrl & 1u << 28), "the request for 0x1f, the clock lane's high speed off (%x)",
	      m.lane_ctrl);
	CHECK(dsi_ulps_exit(&regs, &s) == 0 && !s.in && in_ulps() == 0, "all out again (status %x)", s.status);
	CHECK(m.lane_ctrl == (1u << 28 | 1u << 24), "LANE_CTRL as it was (%x)", m.lane_ctrl);
	CHECK(m.violations == 0, "no violation");

	/* a FIFO not empty, no lane in its stop state: left as they were */
	model(0xf, 1u << 28);
	m.fifo_busy = 1;
	CHECK(dsi_ulps_enter(&regs, &s) == DSI_ULPS_FIFO && !s.in && in_ulps() == 0 && m.lane_ctrl == 1u << 28,
	      "the FIFO busy: nothing (LANE_CTRL %x)", m.lane_ctrl);
	model(0xf, 1u << 28);
	m.none_idle = 1;
	CHECK(dsi_ulps_enter(&regs, &s) == DSI_ULPS_NOT_IDLE && !s.in && in_ulps() == 0 && m.lane_ctrl == 1u << 28,
	      "no lane idle: nothing (LANE_CTRL %x)", m.lane_ctrl);
	CHECK(m.violations == 0, "no violation");

	/* a lane that does not go in: all out again */
	model(0xf, 0);
	m.refuse = 2;
	CHECK(dsi_ulps_enter(&regs, &s) == DSI_ULPS_REFUSED && !s.in && in_ulps() == 0 && m.lane_ctrl == 0,
	      "a lane refused: all out (%d in ULPS)", in_ulps());
	CHECK(m.violations == 0, "no violation");

	/* a lane stuck in ULPS: said */
	model(0xf, 0);
	CHECK(dsi_ulps_enter(&regs, &s) == 0, "in");
	m.stuck = 1;
	CHECK(dsi_ulps_exit(&regs, &s) == DSI_ULPS_STUCK && in_ulps() == 1, "a lane stuck: said");

	/* two data lanes */
	model(0x3, 0);
	CHECK(dsi_ulps_enter(&regs, &s) == 0 && s.lanes == 0x13 && in_ulps() == 3, "two data lanes and the clock lane");
	CHECK(dsi_ulps_exit(&regs, &s) == 0 && in_ulps() == 0, "out");
	CHECK(m.violations == 0, "no violation");

	printf("dsi: %d passed, %d failed\n", passed, failed);
	return failed != 0;
}
