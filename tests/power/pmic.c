/* KolibriOS for ARM: the SPMI arbiter's channels and the Pro1X's battery
 * readings (kernel/modem/pmic.c) on the PC, against an arbiter in RAM
 * that answers the commands as the hardware does (make -C tests/power
 * host): the map, the observer's reads, the writes and their owners, the
 * fuel gauge's held current, the ADC's conversion. */
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/modem/pmic.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the regions: core, cnfg small; chnls and obsrvr sparse (8 channels) */
static u32 core[0x1100 / 4], cnfg[0x1000 / 4];
static u32 chnl[8][8], obs[8][8];
static u8 pmic[16][0x10000];            /* sid, address */
static int held_reads, unheld_reads;

#define CORE 0x10000000ul
#define CHNLS 0x20000000ul
#define OBSRVR 0x30000000ul
#define CNFG 0x40000000ul
#define EE 0

static u32 apid_ppid[8];

static u32 *reg(addr_t a)
{
	if (a >= CORE && a < CORE + sizeof(core)) return &core[(a - CORE) / 4];
	if (a >= CNFG && a < CNFG + sizeof(cnfg)) return &cnfg[(a - CNFG) / 4];
	if (a >= CHNLS && a < CHNLS + 0x80000) return &chnl[(a - CHNLS) / 0x10000][(a & 0x1f) / 4];
	if (a >= OBSRVR + 0x10000 * EE && a < OBSRVR + 0x10000 * EE + 0x400)
		return &obs[(a - OBSRVR) / 0x80 % 8][(a & 0x1f) / 4];
	printf("FAIL: an access at %#lx\n", a);
	failed++;
	static u32 junk;
	return &junk;
}

static u32 rd(addr_t a) { return *reg(a); }

/* a command: done at once (STATUS), on the PMIC's registers */
static void wr(addr_t a, u32 v)
{
	*reg(a) = v;
	if ((a & 0x1f) != 0) return;
	bool observer = a >= OBSRVR;
	u32 apid = observer ? (a - OBSRVR) / 0x80 % 8 : (a - CHNLS) / 0x10000;
	u32 *ch = observer ? obs[apid] : chnl[apid];
	u32 op = v >> 27, lo = v >> 4 & 0xff, len = (v & 7) + 1;
	u32 ppid = apid_ppid[apid], sid = ppid >> 8, addr = (ppid & 0xff) << 8 | lo;
	if (observer && op == 1) {
		u32 d[2] = { 0, 0 };
		for (u32 i = 0; i < len; i++) d[i / 4] |= (u32)pmic[sid][addr + i] << 8 * (i % 4);
		ch[6] = d[0], ch[7] = d[1];
		if (addr == 0x48c6) (pmic[2][0x4842] & 1) ? held_reads++ : unheld_reads++;
	} else if (!observer && op == 0) {
		for (u32 i = 0; i < len; i++) pmic[sid][addr + i] = (u8)(ch[4 + i / 4] >> 8 * (i % 4));
		/* the ADC: a conversion asked for (CONV_REQ, 0x3147): 3.9 V / 3 */
		if (addr <= 0x3147 && addr + len > 0x3147 && (pmic[2][0x3147] & 0x80)) {
			u32 code = 1300u * 0x70e4 / 1875;
			pmic[2][0x3150] = code & 0xff, pmic[2][0x3151] = code >> 8;
			pmic[2][0x3108] = 0x01;
		}
	}
	ch[2] = 1;                              /* STATUS: done */
}

int main(void)
{
	core[0] = 0x50020000;                   /* version 5.2 */
	core[1] = 8;                            /* 8 channels */
	/* the map: QG and ADC on slave 2 ours, the charger on slave 2 the
	 * modem's (EE 1), a PMIC 0 peripheral ours */
	struct { u32 apid, ppid, owner; } m[] = { { 1, 0x0008, 0 }, { 3, 0x248, 0 }, { 4, 0x231, 0 }, { 5, 0x210, 1 } };
	for (u32 i = 0; i < 4; i++) {
		core[0x900 / 4 + m[i].apid] = m[i].ppid << 8;
		cnfg[0x700 / 4 + m[i].apid] = m[i].owner;
		apid_ppid[m[i].apid] = m[i].ppid;
	}
	struct spmi_hw hw = { CORE, CHNLS, OBSRVR, CNFG, sizeof(core), EE, rd, wr, NULL };
	CHECK(spmi_init(&hw) == 0x50020000, "the version");
	u32 ours, n = spmi_channels(&ours);
	CHECK(n == 8 && ours == 3, "8 channels, 3 ours (%u %u)", n, ours);

	u8 b[8];
	pmic[0][0x0805] = 0x42;
	CHECK(spmi_read(0, 0x0805, b, 1) == 0 && b[0] == 0x42, "a read through the observer");
	CHECK(obs[1][0] == (1u << 27 | 0x05 << 4 | 0), "the read command (%#x)", obs[1][0]);
	for (int i = 0; i < 6; i++) pmic[0][0x0810 + i] = (u8)(0x10 + i);
	CHECK(spmi_read(0, 0x0810, b, 6) == 0 && b[0] == 0x10 && b[5] == 0x15, "six bytes in one read");
	CHECK(spmi_read(3, 0x0810, b, 1) == -1, "no channel: -1");
	u8 w[2] = { 0xaa, 0x55 };
	CHECK(spmi_write(2, 0x1040, w, 2) == -2, "the modem's channel: not ours to write");
	CHECK(spmi_write(2, 0x4890, w, 2) == 0 && pmic[2][0x4890] == 0xaa && pmic[2][0x4891] == 0x55, "a write");
	CHECK(chnl[3][0] == (0u << 27 | 0x90 << 4 | 1), "the write command (%#x)", chnl[3][0]);

	/* the battery: a 5 A gauge, 1000 steps (152.6 mA discharging) */
	pmic[2][0x4805] = 0x03;
	pmic[2][0x48c6] = 1000 & 0xff, pmic[2][0x48c7] = 1000 >> 8;
	struct battery bat = { .sid = 2, .qg = 0x4800, .adc = 0x3100, .chan = 0x84, .pre_num = 1, .pre_den = 3 };
	CHECK(battery_init(&bat) == 0 && bat.na_per_lsb == 152588, "the gauge's subtype: 5 A");
	s32 ma = 0;
	CHECK(battery_current(&bat, &ma) == 0 && ma == 152, "the current: 152 mA (%d)", ma);
	CHECK(held_reads == 1 && unheld_reads == 0 && !(pmic[2][0x4842] & 1), "read while held, released after");
	pmic[2][0x48c6] = 0x00, pmic[2][0x48c7] = 0xfe;    /* -512: charging */
	CHECK(battery_current(&bat, &ma) == 0 && ma == -78, "charging: -78 mA (%d)", ma);
	pmic[2][0x48c6] = 0x00, pmic[2][0x48c7] = 0x80;
	CHECK(battery_current(&bat, &ma) == -4, "no sample yet");
	u32 mv = 0;
	CHECK(battery_voltage(&bat, &mv) == 0 && mv >= 3898 && mv <= 3900, "the voltage: 3.9 V (%u)", mv);
	CHECK(pmic[2][0x3144] == 0x84 && (pmic[2][0x3146] & 0x80), "the channel, the ADC enabled");
	CHECK((pmic[2][0x3142] & 0x7c) == (2 << 4 | 2 << 2), "calibration and decimation (%#x)", pmic[2][0x3142]);

	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
