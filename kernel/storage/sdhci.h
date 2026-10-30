/* KolibriOS for ARM: SDHCI-like host controllers (sdhci.c): the N950's
 * OMAP3 HSMMC, QEMU's SDHCI */
#ifndef SDHCI_H
#define SDHCI_H

#include "mmc.h"

struct sdhci {
	addr_t base;                    /* the registers (kernel address) */
	bool omap;                      /* OMAP3 HSMMC: SDHCI's at +0x100 */
	u64 (*now_us)(void);
	u32 sdhci;                      /* (the offset of SDHCI's registers) */
	struct mmc_host host;
};

/* the controller reset and set up, its card started as the disk name */
int sdhci_start(struct sdhci *s, const char *name, bool writable);

#endif
