/* KolibriOS for ARM: the SPMI arbiter's channels, its interrupts and the
 * Pro1X's battery readings (kernel/modem/pmic.c) on the PC, against an
 * arbiter in RAM that answers the commands as the hardware does (make -C
 * tests/power host): the map, the observer's reads, the writes and their
 * owners, the fuel gauge's held current, the ADC's conversion; Linux's
 * interrupts quieted, the power key's (PON) set up, a press latched and
 * taken. */
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/modem/pmic.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the regions: core, cnfg small; chnls and obsrvr sparse (8 channels) */
static u32 core[0x1100 / 4], cnfg[0x1000 / 4], intr[0x400 / 4];
static u32 chnl[8][0x110 / 4], obs[8][8];
static u8 pmic[16][0x10000];            /* sid, address */
static u8 en[16][256], latched[16][256]; /* the peripherals' QPNPINT state */
static int held_reads, unheld_reads;

#define CORE 0x10000000ul
#define CHNLS 0x20000000ul
#define OBSRVR 0x30000000ul
#define CNFG 0x40000000ul
#define INTR 0x50000000ul
#define EE 0
#define ACC_ENABLE (0x100 / 4)
#define IRQ_STATUS (0x104 / 4)
#define IRQ_CLEAR (0x108 / 4)

static u32 apid_ppid[8];

static u32 *reg(addr_t a)
{
	if (a >= CORE && a < CORE + sizeof(core)) return &core[(a - CORE) / 4];
	if (a >= CNFG && a < CNFG + sizeof(cnfg)) return &cnfg[(a - CNFG) / 4];
	if (a >= INTR + 0x10000 * EE && a < INTR + 0x10000 * EE + sizeof(intr))
		return &intr[(a - INTR - 0x10000 * EE) / 4];
	if (a >= CHNLS && a < CHNLS + 0x80000 && (a & 0xffff) < 0x110)
		return &chnl[(a - CHNLS) / 0x10000][(a & 0xffff) / 4];
	if (a >= OBSRVR + 0x10000 * EE && a < OBSRVR + 0x10000 * EE + 0x400)
		return &obs[(a - OBSRVR) / 0x80 % 8][(a & 0x1f) / 4];
	printf("FAIL: an access at %#lx\n", a);
	failed++;
	static u32 junk;
	return &junk;
}

static u32 rd(addr_t a) { return *reg(a); }

/* the arbiter's line: a channel's bit in ACC_STATUS while it has an
 * interrupt and is enabled */
static void acc_update(u32 apid)
{
	bool on = chnl[apid][IRQ_STATUS] && (chnl[apid][ACC_ENABLE] & 1);
	intr[apid / 32] = on ? intr[apid / 32] | 1u << apid % 32 : intr[apid / 32] & ~(1u << apid % 32);
}

/* a peripheral's interrupt: latched; to the arbiter when enabled */
static void irq_fire(u32 apid, u32 sid, u32 per, u32 irq)
{
	latched[sid][per] |= 1u << irq;
	if (en[sid][per] & 1u << irq) chnl[apid][IRQ_STATUS] |= 1u << irq;
	acc_update(apid);
}

/* a command: done at once (STATUS), on the PMIC's registers */
static void wr(addr_t a, u32 v)
{
	*reg(a) = v;
	if (a >= CHNLS && a < OBSRVR && (a & 0xffff) >= 0x100) {
		u32 apid = (a - CHNLS) / 0x10000;
		if ((a & 0xffff) / 4 == IRQ_CLEAR) chnl[apid][IRQ_STATUS] &= ~v;
		acc_update(apid);
		return;
	}
	if ((a & 0x1f) != 0) return;
	bool observer = a >= OBSRVR;
	u32 apid = observer ? (a - OBSRVR) / 0x80 % 8 : (a - CHNLS) / 0x10000;
	u32 *ch = observer ? obs[apid] : chnl[apid];
	u32 op = v >> 27, lo = v >> 4 & 0xff, len = (v & 7) + 1;
	u32 ppid = apid_ppid[apid], sid = ppid >> 8, addr = (ppid & 0xff) << 8 | lo;
	if (observer && op == 1) {
		u32 d[2] = { 0, 0 };
		for (u32 i = 0; i < len; i++) {
			u32 r = (addr + i) & 0xff, b = pmic[sid][addr + i];
			if (r == 0x15 || r == 0x16) b = en[sid][addr >> 8 & 0xff];
			if (r == 0x18) b = latched[sid][addr >> 8 & 0xff];
			d[i / 4] |= b << 8 * (i % 4);
		}
		ch[6] = d[0], ch[7] = d[1];
		if (addr == 0x48c6) (pmic[2][0x4842] & 1) ? held_reads++ : unheld_reads++;
	} else if (!observer && op == 0) {
		for (u32 i = 0; i < len; i++) {
			u8 b = (u8)(ch[4 + i / 4] >> 8 * (i % 4));
			u32 r = (addr + i) & 0xff, per = (addr + i) >> 8 & 0xff;
			if (r == 0x14) latched[sid][per] &= ~b;         /* LATCHED_CLR */
			else if (r == 0x15) en[sid][per] |= b;          /* EN_SET */
			else if (r == 0x16) en[sid][per] &= ~b;         /* EN_CLR */
			else pmic[sid][addr + i] = b;
		}
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
	struct { u32 apid, ppid, owner; bool irq; } m[] = {
		{ 1, 0x0008, 0, true }, { 3, 0x248, 0, true }, { 4, 0x231, 0, true }, { 5, 0x210, 1, true } };
	for (u32 i = 0; i < 4; i++) {
		core[0x900 / 4 + m[i].apid] = m[i].ppid << 8 | (m[i].irq ? 1u << 24 : 0);
		cnfg[0x700 / 4 + m[i].apid] = m[i].owner;
		apid_ppid[m[i].apid] = m[i].ppid;
	}
	struct spmi_hw hw = { CORE, CHNLS, OBSRVR, CNFG, INTR, sizeof(core), EE, rd, wr, NULL };
	CHECK(spmi_init(&hw) == 0x50020000, "the version");
	u32 ours, n = spmi_channels(&ours);
	CHECK(n == 8 && ours == 3, "8 channels, 3 ours (%u %u)", n, ours);

	u8 b[8];
	pmic[0][0x0805] = 0x42;
	CHECK(spmi_read(0, 0x0805, b, 1) == 0 && b[0] == 0x42, "a read through the observer");
	CHECK(obs[1][0] == (1u << 27 | 0x05 << 4 | 0), "the read command (%#x)", obs[1][0]);
	for (int i = 0; i < 6; i++) pmic[0][0x0840 + i] = (u8)(0x10 + i);
	CHECK(spmi_read(0, 0x0840, b, 6) == 0 && b[0] == 0x10 && b[5] == 0x15, "six bytes in one read");
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

	/* the interrupts: Linux left the gauge's on (channel 3) with one
	 * pending; quieted, the line drops */
	en[2][0x48] = 0x04;
	chnl[3][ACC_ENABLE] = 1;
	irq_fire(3, 2, 0x48, 2);
	CHECK(intr[0] == 1u << 3, "the gauge's interrupt holds the line (%#x)", intr[0]);
	CHECK(spmi_irq_quiet() == 1, "one channel was on");
	CHECK(chnl[3][ACC_ENABLE] == 0 && chnl[3][IRQ_STATUS] == 0 && intr[0] == 0, "the gauge's quiet, the line down");
	/* the power key: PON (slave 0, 0x800), its interrupt 0, both edges */
	pmic[0][0x0811] = 0x02, pmic[0][0x0812] = 0x00, pmic[0][0x0813] = 0x02;   /* RESIN's as Linux left it */
	latched[0][0x08] = 0x01;
	CHECK(spmi_irq_enable(0, 0x0808, 0, SPMI_IRQ_EDGE_BOTH) == 0, "the power key's interrupt on");
	CHECK(pmic[0][0x0811] == 0x03 && pmic[0][0x0812] == 0x01 && pmic[0][0x0813] == 0x03,
	      "edge, both polarities, RESIN's untouched (%#x %#x %#x)", pmic[0][0x0811], pmic[0][0x0812], pmic[0][0x0813]);
	CHECK(en[0][0x08] == 0x01 && latched[0][0x08] == 0 && chnl[1][ACC_ENABLE] == 1, "enabled, the old latch cleared");
	CHECK(spmi_irq_enable(2, 0x1000, 0, SPMI_IRQ_EDGE_BOTH) == -5, "the modem's channel's interrupts: not ours");
	CHECK(spmi_irq_enable(3, 0x0800, 0, SPMI_IRQ_EDGE_BOTH) == -1, "no channel");
	/* a press */
	struct spmi_irq ev[4];
	CHECK(spmi_irq_take(ev, 4) == 0, "nothing yet");
	pmic[0][0x0810] = 0x01;                 /* RT_STS: KPDPWR_N down */
	irq_fire(1, 0, 0x08, 0);
	CHECK(intr[0] == 1u << 1, "the press on the line (%#x)", intr[0]);
	int got = spmi_irq_take(ev, 4);
	CHECK(got == 1 && ev[0].sid == 0 && ev[0].addr == 0x800 && ev[0].bits == 1, "taken: PON, interrupt 0 (%d)", got);
	CHECK(intr[0] == 0 && chnl[1][IRQ_STATUS] == 0 && latched[0][0x08] == 0, "acknowledged at both ends");
	u8 rt = 0;
	CHECK(spmi_read(0, 0x0810, &rt, 1) == 0 && (rt & 1), "RT_STS: down");
	/* the gauge's again, quiet: cleared, not reported, the line down */
	en[2][0x48] = 0x04;
	irq_fire(3, 2, 0x48, 2);
	chnl[3][IRQ_STATUS] = 0x04;
	intr[0] = 1u << 3;
	CHECK(spmi_irq_take(ev, 4) == 0 && chnl[3][IRQ_STATUS] == 0, "a quieted channel's: cleared, not reported");
	/* the line without a bit in ACC_STATUS: the channels looked at */
	chnl[1][IRQ_STATUS] = 0x01;
	intr[0] = 0;
	CHECK(spmi_irq_take(ev, 4) == 1 && ev[0].addr == 0x800, "without ACC_STATUS: found by its channel");

	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
