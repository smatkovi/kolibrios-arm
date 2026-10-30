/* KolibriOS for ARM: MMC, eMMC and SD cards (mmc.c) above a host
 * controller (sdhci.c, pl181.c) */
#ifndef MMC_H
#define MMC_H

/* responses */
enum { MMC_RSP_NONE, MMC_RSP_R1, MMC_RSP_R1B, MMC_RSP_R2, MMC_RSP_R3, MMC_RSP_R6 };

struct mmc_host {
	void *ctx;
	int max_width;                  /* 1 or 4 */
	u32 max_blocks;                 /* the most blocks one command moves */
	/* a command: its index, argument, response kind; the response in
	 * r[0..3] (R2: r[0] bits 127:96 ... without the CRC); data: n blocks of
	 * 512 bytes to (write) or from the card; 0, or -1 (a timeout, a CRC
	 * error, the card's error bits) */
	int (*cmd)(void *ctx, u32 idx, u32 arg, u32 rsp, u32 *r, u8 *data, u32 n, bool write);
	void (*set_clock)(void *ctx, u32 hz);
	void (*set_width)(void *ctx, int bits);
	u64 (*now_us)(void);
	void (*open_drain)(void *ctx, bool on);   /* MMC's identification (or NULL) */
	void (*idle)(void *ctx, bool idle);       /* the clock to the card off, on (or NULL) */
};

/* the card started, added as the disk name ("mmc0"); 1, or 0 */
int mmc_start(struct mmc_host *h, const char *name, bool writable);

/* an eMMC's sleep (CMD5) after a second without requests: from the
 * timer's tick (storage_tick, dev.c); the ms until then (NEXT_NONE-like
 * 0xffffffff: asleep, an SD card, or kolibri.mmcsleep=off) */
void mmc_tick(void);
u32 mmc_next_ms(void);
bool storage_mmc_sleep(void);             /* dev.c: not kolibri.mmcsleep=off */

#endif
