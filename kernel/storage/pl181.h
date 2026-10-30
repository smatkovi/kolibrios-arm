/* KolibriOS for ARM: ARM's PL181 (pl181.c), QEMU realview's card slot */
#ifndef PL181_H
#define PL181_H

#include "mmc.h"

struct pl181 {
	addr_t base;
	u64 (*now_us)(void);
	struct mmc_host host;
};

int pl181_start(struct pl181 *p, const char *name, bool writable);

#endif
