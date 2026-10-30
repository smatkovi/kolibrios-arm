/* KolibriOS for ARM: random numbers (kernel/rng.c, rng_core.c;
 * docs/rng.md). One generator for everything: the Linux personality's
 * getrandom, /dev/urandom and AT_RANDOM, lwIP, the Wi-Fi's nonces and SAE
 * scalars, function 102. Seeded from the hardware (the Pro1X's PRNG,
 * QEMU's virtio-rng, the N950's secure ROM), the boot loader's seed
 * (kexec's /chosen/rng-seed) and, only when those give too little, the
 * timing jitter of the CPU's counters. */
#ifndef RNG_H
#define RNG_H
#include "kernel.h"

/* a hardware source: read() gives up to len bytes, returns how many;
 * quality: the bits credited per 1024 bits read (Linux's hwrng quality),
 * RNG_FULL for a source trusted as fully random */
#define RNG_FULL 1024
void rng_register(const char *name, u32 (*read)(void *buf, u32 len), u32 quality);

/* the boot loader's seed (AArch64: the device tree's /chosen/rng-seed and
 * kaslr-seed, which Linux's kexec_file_load fills from its own seeded
 * generator), taken before the MMU is on; the property is wiped */
void rng_fdt_seed(void *fdt);

void rng_init(void);                    /* after the platform's drivers: seeds */
bool rng_ready(void);                   /* seeded */
bool rng_weak(void);                    /* seeded from timing jitter only */
const char *rng_sources(void);          /* what seeded it, for messages */

/* seeds first if need be (the sources, then jitter for at most a few
 * seconds); false: still not seeded, nothing written */
bool rng_bytes(void *p, u32 len);
bool rng_bytes_nb(void *p, u32 len);    /* false while not seeded, no waiting */
bool rng_seed_step(void);               /* one more try (a batch of jitter) for a waiting caller */
void rng_bytes_insecure(void *p, u32 len);      /* always: unseeded output when asked too early */
u32 rng_u32(void);                      /* insecure, for lwIP's ids and ports */
void rng_event(u32 v);                  /* an input event and its time, credited nothing */

void rng_selftest(void);                /* kolibri.rngtest: FIPS 140-2 on each source */

#endif
