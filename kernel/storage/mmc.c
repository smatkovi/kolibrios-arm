/* KolibriOS for ARM: MMC, eMMC and SD cards, the protocol above the host
 * controllers (sdhci.c: the N950's OMAP3 HSMMC and QEMU's SDHCI; pl181.c:
 * QEMU realview's PL181), the card as the disk "mmc0".
 *
 * The card is started from the beginning every time (CMD0): whatever the
 * previous kernel left (ubiboot's Linux 2.6.32 puts the N950's eMMC to
 * sleep after a second without requests and switches its supply off
 * after eight more), it ends in the same state. eMMC: CMD1 (sector
 * addressing), CMD2, CMD3 (RCA 1), CMD9, CMD7, CMD8 (EXT_CSD: the size),
 * CMD6 for the bus width (BUS_WIDTH, a setting that does not outlive the
 * card's power); SD: CMD8, ACMD41, CMD2, CMD3, CMD9 (the size), CMD7,
 * ACMD6. Then reads with CMD17/CMD18 (+ CMD12), writes (only the fence's:
 * blk.c) with CMD24/CMD25 (+ CMD12) and CMD13 until the card is back in
 * its transfer state. Nothing else that changes the card: no erase, no
 * EXT_CSD write but the bus width, no boot partition, no RPMB. */
#include "storage.h"
#include "mmc.h"

#define R1_STATE(r) (((r) >> 9) & 15)
#define ST_TRAN 4
#define RCA 1
#define MAX_BLOCKS 128                  /* 64 KiB a command */

struct card {
	struct blkdev dev;
	struct mmc_host *h;
	bool sd, block_addr;
};

static struct card card;

static int cmd(struct mmc_host *h, u32 idx, u32 arg, u32 rsp, u32 *r)
{
	u32 dummy[4];
	return h->cmd(h->ctx, idx, arg, rsp, r ? r : dummy, NULL, 0, false);
}

static bool wait_tran(struct mmc_host *h, u32 ms)
{
	u64 t0 = h->now_us();
	u32 r[4];
	for (;;) {
		if (!cmd(h, 13, RCA << 16, MMC_RSP_R1, r) && R1_STATE(r[0]) == ST_TRAN) return true;
		if (h->now_us() - t0 > (u64)ms * 1000) return false;
	}
}

static int xfer(struct card *c, u64 lba, u32 n, u8 *buf, bool write)
{
	struct mmc_host *h = c->h;
	u32 arg = c->block_addr ? (u32)lba : (u32)(lba * 512);
	u32 r[4];
	int e = h->cmd(h->ctx, n > 1 ? (write ? 25 : 18) : (write ? 24 : 17), arg, MMC_RSP_R1, r, buf, n, write);
	if (n > 1) {
		u32 s[4];
		int e2 = cmd(h, 12, 0, MMC_RSP_R1B, s);
		if (!e) e = e2;
	}
	if (write && !wait_tran(h, 2000)) e = -1;
	if (e) {
		slog("mmc: %s at %u (+%u) failed\n", write ? "write" : "read", (u32)lba, n);
		wait_tran(h, 500);
	}
	return e;
}

static int cread(struct blkdev *d, u64 lba, u32 n, void *buf)
{
	return xfer(d->priv, lba, n, buf, false);
}

/* only the fence calls it (blk.c), and the fence is asked again */
static int cwrite(struct blkdev *d, u64 lba, u32 n, const void *buf)
{
	if (!blk_fence_allows(d, lba, n)) {
		slog("mmc: write at %u refused (the fence)\n", (u32)lba);
		return -1;
	}
	return xfer(d->priv, lba, n, (u8 *)buf, true);
}

/* eMMC and MMC: CMD1 until the card is ready; true, *hc: sector addressed */
static bool mmc_ident(struct mmc_host *h, bool *hc)
{
	u64 t0 = h->now_us();
	u32 r[4];
	for (;;) {
		if (cmd(h, 1, 0x40ff8080, MMC_RSP_R3, r)) return false;
		if (r[0] >> 31) {
			*hc = (r[0] >> 29 & 3) == 2;
			return true;
		}
		if (h->now_us() - t0 > 1000000) return false;
	}
}

/* SD: CMD8, then ACMD41 until ready */
static bool sd_ident(struct mmc_host *h, bool *hc)
{
	u32 r[4];
	bool v2 = !cmd(h, 8, 0x1aa, MMC_RSP_R1, r) && (r[0] & 0xfff) == 0x1aa;
	u64 t0 = h->now_us();
	for (;;) {
		if (cmd(h, 55, 0, MMC_RSP_R1, r) || cmd(h, 41, (v2 ? 1u << 30 : 0) | 0x00ff8000, MMC_RSP_R3, r))
			return false;
		if (r[0] >> 31) {
			*hc = v2 && (r[0] >> 30 & 1);
			return true;
		}
		if (h->now_us() - t0 > 1000000) return false;
	}
}

int mmc_start(struct mmc_host *h, const char *name, bool writable)
{
	struct card *c = &card;
	u32 r[4];
	if (c->h) return 0;
	h->set_clock(h->ctx, 400000);
	h->set_width(h->ctx, 1);
	cmd(h, 0, 0, MMC_RSP_NONE, NULL);
	bool hc = false;
	/* SD first (an eMMC does not answer CMD55/ACMD41), then MMC */
	c->sd = sd_ident(h, &hc);
	if (!c->sd) {
		/* MMC's identification with the command line open drain, as
		 * Linux does it */
		if (h->open_drain) h->open_drain(h->ctx, true);
		cmd(h, 0, 0, MMC_RSP_NONE, NULL);
		if (!mmc_ident(h, &hc)) {
			if (h->open_drain) h->open_drain(h->ctx, false);
			slog("mmc: %s: no card answers\n", name);
			return 0;
		}
	}
	c->block_addr = hc;
	u32 cid[4], csd[4];
	if (cmd(h, 2, 0, MMC_RSP_R2, cid)) return 0;
	u32 rca = RCA;
	if (c->sd) {
		if (cmd(h, 3, 0, MMC_RSP_R6, r)) return 0;
		rca = r[0] >> 16;
	} else {
		int e = cmd(h, 3, RCA << 16, MMC_RSP_R1, r);
		if (h->open_drain) h->open_drain(h->ctx, false);
		if (e) return 0;
	}
	if (cmd(h, 9, rca << 16, MMC_RSP_R2, csd)) return 0;
	if (cmd(h, 7, rca << 16, MMC_RSP_R1B, r)) return 0;
	/* the size: SD's CSD; eMMC's EXT_CSD SEC_COUNT (above 2 GB), or its CSD */
	u64 sectors = 0;
	static u8 ext[512];
	/* csd[0] holds bits 127:96 */
	u32 structure = csd[0] >> 30;
	if (c->sd && structure == 1) {
		u32 csize = (csd[1] & 0x3f) << 16 | csd[2] >> 16;   /* bits 69:48 */
		sectors = (u64)(csize + 1) << 10;
	} else {
		u32 rbl = csd[1] >> 16 & 0xf;                       /* READ_BL_LEN, bits 83:80 */
		u32 csize = (csd[1] & 0x3ff) << 2 | csd[2] >> 30;   /* C_SIZE, bits 73:62 */
		u32 mult = csd[2] >> 15 & 7;                        /* C_SIZE_MULT, bits 49:47 */
		sectors = ((u64)(csize + 1) << (mult + 2)) << rbl >> 9;
	}
	if (!c->sd && h->cmd(h->ctx, 8, 0, MMC_RSP_R1, r, ext, 1, false) == 0) {
		u32 sec = le32(ext + 212);
		if (sec) sectors = sec;
	}
	h->set_clock(h->ctx, c->sd ? 25000000 : 26000000);
	/* 4 bits wide */
	if (h->max_width >= 4) {
		bool ok = c->sd ? !cmd(h, 55, rca << 16, MMC_RSP_R1, r) && !cmd(h, 6, 2, MMC_RSP_R1, r)
				: !cmd(h, 6, 0x03b70100, MMC_RSP_R1B, r) && wait_tran(h, 500);
		if (ok) h->set_width(h->ctx, 4);
	}
	if (!c->block_addr) cmd(h, 16, 512, MMC_RSP_R1, r);
	if (!wait_tran(h, 500)) {
		slog("mmc: %s: the card is not in its transfer state\n", name);
		return 0;
	}
	c->h = h;
	struct blkdev *d = &c->dev;
	strncpy0(d->name, name, sizeof(d->name));
	char *w = d->what;
	const char *kind = c->sd ? "SD card " : "eMMC ";
	while (*kind) *w++ = *kind++;
	/* the product name: CID bits 103:64 (eMMC 6 bytes from 103, SD 5 from 103) */
	u8 pn[6] = { cid[0] & 0xff, cid[1] >> 24, cid[1] >> 16 & 0xff, cid[1] >> 8 & 0xff, cid[1] & 0xff, cid[2] >> 24 };
	for (int i = 0; i < (c->sd ? 5 : 6); i++) *w++ = pn[i] >= 32 && pn[i] < 127 ? pn[i] : '?';
	*w = 0;
	d->bsize = 512;
	d->nblocks = sectors;
	d->max_blocks = h->max_blocks ? MIN(h->max_blocks, MAX_BLOCKS) : MAX_BLOCKS;
	d->read = cread;
	d->priv = c;
	blk_add(d, writable ? cwrite : NULL);
	return 1;
}
