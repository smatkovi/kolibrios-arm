/* KolibriOS for ARM: SD host controllers of the SDHCI kind, polled, the
 * data through the buffer port (no DMA):
 *  - the N950's OMAP3 HSMMC (MMC2, its eMMC): SDHCI's registers 0x100 up,
 *    its own reset (SYSCONFIG), clock divider (96 MHz / CLKD), voltage
 *    capabilities to be set (1.8 V for MMC2) and initialisation stream
 *    (CON.INIT), all accessed as 32-bit words;
 *  - SDHCI itself (QEMU's sdhci-pci on virt: the eMMC tests).
 * The cards' protocol: mmc.c. */
#include "storage.h"
#include "mmc.h"
#include "sdhci.h"

/* OMAP's own registers (from the module's start) */
#define SYSCONFIG 0x010
#define SYSSTATUS 0x014
#define CON 0x02c
/* SDHCI's, from its base (OMAP: +0x100) */
#define BLK 0x04
#define ARG 0x08
#define CMD 0x0c
#define RSP 0x10
#define DATA 0x20
#define PSTATE 0x24
#define HCTL 0x28
#define SYSCTL 0x2c
#define STAT 0x30
#define IE 0x34
#define ISE 0x38
#define CAPA 0x40

#define ST_CC (1u << 0)
#define ST_TC (1u << 1)
#define ST_BWR (1u << 4)
#define ST_BRR (1u << 5)
#define ST_ERRI (1u << 15)
#define ST_CTO (1u << 16)
#define ST_DTO (1u << 20)
#define ST_ERRORS 0xffff8000u
#define PS_CMDI (1u << 0)
#define PS_DATI (1u << 1)

static u32 rd(struct sdhci *s, u32 off) { return rd32(s->base + s->sdhci + off); }
static void wr(struct sdhci *s, u32 off, u32 v) { wr32(s->base + s->sdhci + off, v); }

static bool wait_bits(struct sdhci *s, u32 off, u32 mask, u32 want, u32 ms)
{
	u64 t0 = s->now_us();
	for (;;) {
		if ((rd(s, off) & mask) == want) return true;
		if (s->now_us() - t0 > (u64)ms * 1000) return (rd(s, off) & mask) == want;
	}
}

/* the command and data lines reset after an error */
static void reset_lines(struct sdhci *s)
{
	wr(s, SYSCTL, rd(s, SYSCTL) | 3u << 25);
	wait_bits(s, SYSCTL, 3u << 25, 0, 100);
}

static int scmd(void *ctx, u32 idx, u32 arg, u32 rsp, u32 *r, u8 *data, u32 n, bool write)
{
	struct sdhci *s = ctx;
	if (!wait_bits(s, PSTATE, PS_CMDI | (data || rsp == MMC_RSP_R1B ? PS_DATI : 0), 0, 500)) {
		reset_lines(s);
		return -1;
	}
	wr(s, STAT, 0xffffffffu);
	u32 c = idx << 24;
	switch (rsp) {
	case MMC_RSP_R1: case MMC_RSP_R6: c |= 2u << 16 | 1u << 19 | 1u << 20; break;   /* 48 bits, CRC, index */
	case MMC_RSP_R1B: c |= 3u << 16 | 1u << 19 | 1u << 20; break;              /* 48 bits busy */
	case MMC_RSP_R2: c |= 1u << 16 | 1u << 19; break;                          /* 136 bits */
	case MMC_RSP_R3: c |= 2u << 16; break;                                     /* 48 bits, no CRC */
	}
	if (data) {
		c |= 1u << 21 | (write ? 0 : 1u << 4);                     /* data present, direction */
		if (n > 1) c |= 1u << 1 | 1u << 5;                          /* block count, multi */
		wr(s, BLK, n << 16 | 512);
	}
	wr(s, ARG, arg);
	wr(s, CMD, c);
	u64 t0 = s->now_us();
	u32 st;
	while (!((st = rd(s, STAT)) & (ST_CC | ST_ERRI))) {
		if (s->now_us() - t0 > 500000) { st = ST_ERRI; break; }
	}
	if (st & ST_ERRI) {
		if (!(st & ST_CTO) || (idx != 8 && idx != 55 && idx != 41 && idx != 1))
			slog("sdhci: CMD%u: status 0x%x\n", idx, st);
		wr(s, STAT, st);
		reset_lines(s);
		return -1;
	}
	wr(s, STAT, ST_CC);
	if (rsp == MMC_RSP_R2) {
		/* 127:8 in the registers (the CRC dropped): shifted back */
		u32 w[4];
		for (int i = 0; i < 4; i++) w[i] = rd(s, RSP + 4 * i);
		for (int i = 0; i < 4; i++) r[i] = w[3 - i] << 8 | (i < 3 ? w[2 - i] >> 24 : 0);
	} else {
		r[0] = rd(s, RSP);
	}
	if (rsp == MMC_RSP_R1 && (r[0] & 0xfdf90008u) && !data)       /* the card's error bits */
		return -1;
	if (data) {
		for (u32 b = 0; b < n; b++) {
			u32 want = write ? ST_BWR : ST_BRR;
			t0 = s->now_us();
			while (!((st = rd(s, STAT)) & (want | ST_ERRI)))
				if (s->now_us() - t0 > 1000000) { st = ST_ERRI; break; }
			if (st & ST_ERRI) {
				slog("sdhci: CMD%u: data status 0x%x at block %u\n", idx, st, b);
				wr(s, STAT, st);
				reset_lines(s);
				return -1;
			}
			wr(s, STAT, want);
			u8 *p = data + b * 512;
			for (u32 i = 0; i < 512; i += 4) {
				if (write) wr(s, DATA, le32(p + i));
				else put_le32(p + i, rd(s, DATA));
			}
		}
		t0 = s->now_us();
		while (!((st = rd(s, STAT)) & (ST_TC | ST_ERRI)))
			if (s->now_us() - t0 > 2000000) { st = ST_ERRI; break; }
		wr(s, STAT, st);
		if (st & ST_ERRI) {
			slog("sdhci: CMD%u: the end's status 0x%x\n", idx, st);
			reset_lines(s);
			return -1;
		}
	} else if (rsp == MMC_RSP_R1B) {
		t0 = s->now_us();
		while (!((st = rd(s, STAT)) & (ST_TC | ST_ERRI)) && (rd(s, PSTATE) & PS_DATI))
			if (s->now_us() - t0 > 2000000) break;
		wr(s, STAT, st);
	}
	return 0;
}

static void set_clock(void *ctx, u32 hz)
{
	struct sdhci *s = ctx;
	u32 v = rd(s, SYSCTL) & ~0xffffu;
	wr(s, SYSCTL, v);                                          /* clock off */
	if (s->omap) {
		u32 d = (96000000 + hz - 1) / hz;                      /* CLKD: 96 MHz / d */
		if (d > 1023) d = 1023;
		v |= d << 6;
	} else {
		/* SDHCI 3.0's 10-bit divider: base / (2 d) */
		u32 base = (rd(s, CAPA) >> 8 & 0xff) * 1000000;
		u32 d = base ? (base + 2 * hz - 1) / (2 * hz) : 1;
		if (d > 1023) d = 1023;
		v |= (d & 0xff) << 8 | (d >> 8) << 6;
	}
	v |= 0xe << 16 | 1;                                         /* data timeout, internal clock on */
	wr(s, SYSCTL, v);
	wait_bits(s, SYSCTL, 2, 2, 100);                            /* the clock stable */
	wr(s, SYSCTL, v | 4);                                       /* to the card */
}

static void set_width(void *ctx, int bits)
{
	struct sdhci *s = ctx;
	u32 h = rd(s, HCTL) & ~2u;
	wr(s, HCTL, h | (bits == 4 ? 2 : 0));
}

static void open_drain(void *ctx, bool on)
{
	struct sdhci *s = ctx;
	u32 c = rd32(s->base + CON) & ~1u;
	wr32(s->base + CON, c | (on ? 1 : 0));                      /* CON.OD */
}

int sdhci_start(struct sdhci *s, const char *name, bool writable)
{
	s->sdhci = s->omap ? 0x100 : 0;
	if (s->omap) {
		wr32(s->base + SYSCONFIG, 2);                           /* soft reset */
		u64 t0 = s->now_us();
		while (!(rd32(s->base + SYSSTATUS) & 1))
			if (s->now_us() - t0 > 100000) {
				slog("sdhci: %s does not come out of its reset\n", name);
				return 0;
			}
		wr32(s->base + SYSCONFIG, 0x0000308 | 1);               /* clocks kept, smart idle, autoidle */
		wr(s, CAPA, rd(s, CAPA) | 1u << 26);                    /* 1.8 V (MMC2's) */
		wr(s, HCTL, 5u << 9);                                   /* SDVS 1.8 V */
	} else {
		wr(s, SYSCTL, 1u << 24);                                /* reset all */
		wait_bits(s, SYSCTL, 1u << 24, 0, 100);
		u32 cap = rd(s, CAPA);
		u32 vs = cap & (1u << 24) ? 7 : cap & (1u << 25) ? 6 : 5;   /* 3.3, 3.0, 1.8 V */
		wr(s, HCTL, vs << 9);
	}
	wr(s, HCTL, rd(s, HCTL) | 1u << 8);                         /* bus power */
	if (!wait_bits(s, HCTL, 1u << 8, 1u << 8, 100)) {
		slog("sdhci: %s: no bus power\n", name);
		return 0;
	}
	wr(s, IE, 0x317f8033u);                                     /* the status bits latched */
	wr(s, ISE, 0);                                              /* no interrupts: polled */
	set_clock(s, 400000);
	if (s->omap) {
		/* the initialisation stream: 80 clocks with CON.INIT */
		wr32(s->base + CON, rd32(s->base + CON) | 2);
		wr(s, STAT, 0xffffffffu);
		wr(s, CMD, 0);
		wait_bits(s, STAT, ST_CC, ST_CC, 10);
		wr32(s->base + CON, rd32(s->base + CON) & ~2u);
		wr(s, STAT, 0xffffffffu);
	}
	s->host = (struct mmc_host){ s, 4, 128, scmd, set_clock, set_width, s->now_us, s->omap ? open_drain : NULL };
	return mmc_start(&s->host, name, writable);
}
