/* KolibriOS for ARM: ARM's PL181 MultiMedia Card Interface, polled, its
 * FIFO by the processor: QEMU realview's SD/MMC slot (-drive if=sd, or
 * -device emmc,bus=sd-bus), the ARMv7 kernel's storage tests. The cards'
 * protocol: mmc.c. */
#include "storage.h"
#include "mmc.h"
#include "pl181.h"

#define POWER 0x00
#define CLOCK 0x04
#define ARGUMENT 0x08
#define COMMAND 0x0c
#define RESPONSE 0x14
#define DATATIMER 0x24
#define DATALENGTH 0x28
#define DATACTRL 0x2c
#define STATUS 0x34
#define CLEAR 0x38
#define MASK0 0x3c
#define FIFO 0x80

#define S_CMDCRCFAIL (1u << 0)
#define S_DATACRCFAIL (1u << 1)
#define S_CMDTIMEOUT (1u << 2)
#define S_DATATIMEOUT (1u << 3)
#define S_TXUNDERRUN (1u << 4)
#define S_RXOVERRUN (1u << 5)
#define S_CMDRESPEND (1u << 6)
#define S_CMDSENT (1u << 7)
#define S_DATAEND (1u << 8)
#define S_DATABLOCKEND (1u << 10)
#define S_TXFIFOEMPTY (1u << 18)
#define S_TXFIFOHALFEMPTY (1u << 14)
#define S_RXDATAAVLBL (1u << 21)
#define S_DATAERR (S_DATACRCFAIL | S_DATATIMEOUT | S_TXUNDERRUN | S_RXOVERRUN)

static u32 rd(struct pl181 *p, u32 off) { return rd32(p->base + off); }
static void wr(struct pl181 *p, u32 off, u32 v) { wr32(p->base + off, v); }

static int pcmd(void *ctx, u32 idx, u32 arg, u32 rsp, u32 *r, u8 *data, u32 n, bool write)
{
	struct pl181 *p = ctx;
	wr(p, CLEAR, 0x7ff);
	if (data) {
		wr(p, DATATIMER, 0xffffffffu);
		wr(p, DATALENGTH, n * 512);
		if (!write) wr(p, DATACTRL, 1 | 2 | 9 << 4);            /* on, from the card, 512-byte blocks */
	}
	u32 c = idx | 1u << 10;
	if (rsp != MMC_RSP_NONE) c |= 1u << 6;
	if (rsp == MMC_RSP_R2) c |= 1u << 7;
	wr(p, ARGUMENT, arg);
	wr(p, COMMAND, c);
	u32 want = rsp == MMC_RSP_NONE ? S_CMDSENT : S_CMDRESPEND;
	u64 t0 = p->now_us();
	u32 st;
	while (!((st = rd(p, STATUS)) & (want | S_CMDTIMEOUT | S_CMDCRCFAIL)))
		if (p->now_us() - t0 > 500000) { st = S_CMDTIMEOUT; break; }
	/* R3 has no CRC: its CRC failure is expected */
	if ((st & S_CMDTIMEOUT) || ((st & S_CMDCRCFAIL) && rsp != MMC_RSP_R3)) {
		wr(p, CLEAR, 0x7ff);
		wr(p, COMMAND, 0);
		return -1;
	}
	for (int i = 0; i < 4; i++) r[i] = rd(p, RESPONSE + 4 * i);
	wr(p, CLEAR, 0x7ff);
	if (rsp == MMC_RSP_R1 && (r[0] & 0xfdf90008u) && !data) return -1;
	if (!data) return 0;
	if (write) wr(p, DATACTRL, 1 | 9 << 4);                     /* on, to the card */
	u32 words = n * 128, done = 0;
	t0 = p->now_us();
	while (done < words) {
		st = rd(p, STATUS);
		if (st & S_DATAERR) break;
		if (write) {
			if (st & S_TXFIFOHALFEMPTY)
				for (int k = 0; k < 8 && done < words; k++, done++) wr(p, FIFO, le32(data + 4 * done));
		} else {
			while ((rd(p, STATUS) & S_RXDATAAVLBL) && done < words) {
				put_le32(data + 4 * done, rd(p, FIFO));
				done++;
			}
		}
		if (p->now_us() - t0 > 2000000) break;
	}
	while (!((st = rd(p, STATUS)) & (S_DATAEND | S_DATAERR)))
		if (p->now_us() - t0 > 2000000) break;
	wr(p, CLEAR, 0x7ff);
	wr(p, DATACTRL, 0);
	if (done < words || (st & S_DATAERR) || !(st & S_DATAEND)) {
		slog("pl181: CMD%u: data status 0x%x, %u of %u words\n", idx, st, done, words);
		return -1;
	}
	return 0;
}

static void set_clock(void *ctx, u32 hz)
{
	struct pl181 *p = ctx;
	wr(p, CLOCK, 1u << 8 | 0x3f);                             /* on, a divider (QEMU does not care) */
}

static void set_width(void *ctx, int bits)
{
}

int pl181_start(struct pl181 *p, const char *name, bool writable)
{
	wr(p, POWER, 0x83);                                         /* on */
	wr(p, MASK0, 0);
	set_clock(p, 400000);
	p->host = (struct mmc_host){ p, 1, pcmd, set_clock, set_width, p->now_us, NULL };
	return mmc_start(&p->host, name, writable);
}
