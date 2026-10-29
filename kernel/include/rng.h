/* KolibriOS for ARM: random numbers from the hardware (AArch64 kernel):
 * QEMU's virtio-rng, the Pro1X's PRNG. The Linux personality's getrandom
 * and lwIP are seeded from it. */
#ifndef RNG_H
#define RNG_H
#include "kernel.h"

void rng_register(const char *name, u32 (*read)(void *buf, u32 len));
u32 rng_hw(void *buf, u32 len);         /* bytes from the hardware; 0: none there */
const char *rng_name(void);             /* NULL: none */

#endif
