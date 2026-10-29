/* KolibriOS for ARM: the SoC's sleep through the RPM (rpmsleep.h).
 *
 * The RPM keeps two sets of votes per master: the active set while the
 * master runs, the sleep set while it sleeps (its cluster down). A
 * resource that no master needs while all of them sleep may go off: the
 * XO (XO shutdown), CX and MX to retention (vmin). After a kexec the
 * sleep set is what Linux left: its consumers' votes, and its maxima of
 * the boot (rpmpd and clk-smd-rpm clamp to the highest until sync_state).
 * rpm_sleep_votes() sends what Linux sends when nothing uses them; the
 * active set stays as it is. The resources (Linux 7.1):
 *
 *   the XO           "clk0" 0 "Enab"       clk-smd-rpm.c bi_tcxo (MISC_CLK)
 *   its buffers      "clka" 1-3 LN_BB, 4-5 RF "swen"   (CLK_BUF_A, XO_BUFFER)
 *   CX, MX           "rwcx" 0, "rwmx" 0 "vlvl"         rpmpd.c sm6115 (KEY_LEVEL)
 *   the bus clocks   "clk2" 0 bimc, "clk1" 1 cnoc, "clk1" 2 snoc,
 *                    "mmxi" 0 mmnrt, 1 mmrt, "qup" 0 "KHz"
 *                                          clk-smd-rpm.c sm_qnoc_icc_clks
 *
 * Not voted: the interconnect's per-node bandwidth (icc-rpm.c's master
 * and slave requests), the regulators (Linux votes them in the active
 * set only, which the RPM keeps in sleep). */
#include "rpmsleep.h"

#define T_MISC 0x306b6c63u      /* clk0 */
#define T_CLKBUF 0x616b6c63u    /* clka */
#define T_BUS 0x316b6c63u       /* clk1 */
#define T_MEM 0x326b6c63u       /* clk2 */
#define T_MMAXI 0x69786d6du     /* mmxi */
#define T_QUP 0x00707571u       /* qup */
#define T_RWCX 0x78637772u      /* rwcx */
#define T_RWMX 0x786d7772u      /* rwmx */
#define K_ENABLE 0x62616e45u    /* Enab */
#define K_SWEN 0x6e657773u      /* swen */
#define K_RATE 0x007a484bu      /* KHz */
#define K_LEVEL 0x6c766c76u     /* vlvl */

static const struct { u32 type, id, key; const char *name; bool rf; } votes[] = {
	{ T_MISC, 0, K_ENABLE, "XO" },
	{ T_CLKBUF, 1, K_SWEN, "LN_BB 1" },
	{ T_CLKBUF, 2, K_SWEN, "LN_BB 2" },
	{ T_CLKBUF, 3, K_SWEN, "LN_BB 3" },
	{ T_CLKBUF, 4, K_SWEN, "RF 1", true },
	{ T_CLKBUF, 5, K_SWEN, "RF 2", true },
	{ T_RWCX, 0, K_LEVEL, "CX" },
	{ T_RWMX, 0, K_LEVEL, "MX" },
	{ T_MEM, 0, K_RATE, "bimc" },
	{ T_BUS, 1, K_RATE, "cnoc" },
	{ T_BUS, 2, K_RATE, "snoc" },
	{ T_MMAXI, 0, K_RATE, "mmnrt" },
	{ T_MMAXI, 1, K_RATE, "mmrt" },
	{ T_QUP, 0, K_RATE, "qup" },
};

int rpm_sleep_votes(bool with_rf)
{
	if (!rpm_ready()) return 0;
	int ok = 0, n = 0;
	char refused[96];
	u32 rl = 0;
	refused[0] = 0;
	for (u32 i = 0; i < sizeof(votes) / sizeof(votes[0]); i++) {
		if (votes[i].rf && !with_rf) continue;
		u32 zero = 0;
		n++;
		if (rpm_write_set(RPM_SLEEP, votes[i].type, votes[i].id, &votes[i].key, &zero, 1) == 0) {
			ok++;
			continue;
		}
		for (const char *c = votes[i].name; *c && rl + 2 < sizeof(refused); c++) refused[rl++] = *c;
		if (rl + 2 < sizeof(refused)) refused[rl++] = ' ';
		refused[rl] = 0;
	}
	if (rl) refused[rl - 1] = 0;
	mlog("rpm: the sleep set: %d of %d resources at 0%s%s\n", ok, n, rl ? "; not: " : "", refused);
	return ok;
}

static volatile u8 *mpm_reg(volatile u8 *vmpm, u32 pins, u32 reg, u32 bank)
{
	return vmpm + 4 * (reg * ((pins + 31) / 32) + bank + 2);
}

void mpm_prepare(volatile u8 *vmpm, u32 pins, u64 wake, const u32 *pin_mask)
{
	sm_wr32(vmpm, (u32)wake);
	sm_wr32(vmpm + 4, (u32)(wake >> 32));
	for (u32 b = 0; b < (pins + 31) / 32; b++) {
		sm_wr32(mpm_reg(vmpm, pins, MPM_REG_ENABLE, b), pin_mask ? pin_mask[b] : 0);
		sm_wr32(mpm_reg(vmpm, pins, MPM_REG_STATUS, b), 0);
	}
	sm_barrier();
}

void mpm_status(const volatile u8 *vmpm, u32 pins, u32 *out)
{
	for (u32 b = 0; b < (pins + 31) / 32; b++)
		out[b] = sm_rd32(mpm_reg((volatile u8 *)vmpm, pins, MPM_REG_STATUS, b));
}

static u64 rd64(const volatile u8 *p)
{
	return sm_rd32(p) | (u64)sm_rd32(p + 4) << 32;
}

int rpm_stats_read(const volatile u8 *base, u32 size, struct rpm_stat out[2])
{
	if (size < 0x18) return 0;
	u32 off = sm_rd32(base + 0x14) & 0xffff;
	int n = 0;
	for (int i = 0; i < 2; i++, off += 48) {
		if (off + 48 > size) break;
		const volatile u8 *r = base + off;
		u32 t = sm_rd32(r);
		int k = 0;
		for (int j = 0; j < 4; j++) {
			char c = (char)(t >> (8 * j));
			if (c > ' ' && c < 127) out[i].name[k++] = c;
		}
		out[i].name[k] = 0;
		out[i].count = sm_rd32(r + 4);
		out[i].entered = rd64(r + 8);
		out[i].left = rd64(r + 16);
		out[i].accumulated = rd64(r + 24);
		out[i].votes = sm_rd32(r + 32);
		n++;
	}
	return n;
}
