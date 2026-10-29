/* KolibriOS for ARM: Qualcomm's shared memory (SMEM), the items in the
 * private partition the apps processor (host 0) shares with one other
 * processor */
#ifndef SMEM_H
#define SMEM_H
#include "modem.h"

#define SMEM_HOST_APPS 0
#define SMEM_HOST_MODEM 1

struct smem {
	volatile u8 *base;
	u32 size;
	volatile u8 *part;     /* the partition's header */
	u32 part_size;
	u32 cacheline;
};

/* the partition of hosts local and remote; 0, or -1 when SMEM or the
 * partition is not there */
int smem_init(struct smem *s, volatile u8 *base, u32 size, u16 local, u16 remote);
/* an item of that partition, its size (without padding); NULL: not there */
volatile u8 *smem_get(struct smem *s, u16 item, u32 *size);

#endif
