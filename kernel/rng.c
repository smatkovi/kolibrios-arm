/* KolibriOS for ARM: the hardware's random numbers (rng.h): one source,
 * the platform's */
#include "rng.h"

static const char *name;
static u32 (*source)(void *buf, u32 len);

void rng_register(const char *n, u32 (*read)(void *buf, u32 len))
{
	if (source) return;
	u32 probe;
	if (read(&probe, 4) != 4) {
		kprintf("rng: %s gives nothing\n", n);
		return;
	}
	name = n;
	source = read;
	kprintf("rng: %s\n", n);
}

u32 rng_hw(void *buf, u32 len)
{
	return source ? source(buf, len) : 0;
}

const char *rng_name(void)
{
	return name;
}
