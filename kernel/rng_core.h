/* KolibriOS for ARM: the random number generator's core (rng_core.c,
 * docs/rng.md): plain C without the kernel's services, so that tests/rng
 * builds it on the PC (MODEM_HOST, as kernel/modem).
 *
 *  - the pool: SHA-256 over everything mixed in since the last reseed,
 *    with the bits credited to it counted
 *  - the reseed: the generator's key = HMAC-SHA256(key, label | the
 *    pool's hash | the reseed's number), only with 256 credited bits in
 *    the pool; the next pool starts with the old one's hash
 *  - the output: ChaCha20 (RFC 8439) with fast key erasure: each request
 *    takes block 0 of the key, its first half the next key, its second
 *    half the key of this request's blocks, wiped afterwards; nothing
 *    left in memory gives earlier output again
 *  - FIPS 140-2's statistical tests (4.9.1) on 20000 bits, and SP
 *    800-90B's most common value estimate (6.3.1), for kolibri.rngtest
 *    and the jitter source's health */
#ifndef RNG_CORE_H
#define RNG_CORE_H
#include "modem/crypto.h"

#define RNG_SEED_BITS 256               /* credited bits for a (re)seed */

struct rngc {
	struct sha256 pool;
	u32 pool_bits;                  /* credited since the last reseed */
	u8 key[32];
	u64 reseeds;
	u64 generated;                  /* bytes since the last reseed */
	bool seeded;
};

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]);

void rngc_init(struct rngc *s);
void rngc_mix(struct rngc *s, const void *p, u32 len, u32 bits);
bool rngc_reseed(struct rngc *s);       /* false: fewer than RNG_SEED_BITS in the pool */
/* before the seed: the key from the pool so far, which keeps it all (for
 * requests that cannot wait: lwIP's ids at its start) */
void rngc_stir(struct rngc *s);
void rngc_generate(struct rngc *s, void *out, u32 len);

/* FIPS 140-2's continuous test (4.9.2) on a source's words: false when a
 * word is the one before it (last: the previous read's last word) */
bool rng_continuous_ok(u32 *last, bool *has_last, const u32 *w, u32 n);

/* FIPS 140-2's tests on one block of 20000 bits (2500 bytes, each byte's
 * most significant bit first): a mask of the failed ones, 0 when all
 * passed; the statistics in st (may be NULL) */
#define FIPS_BLOCK 2500
#define FIPS_MONOBIT 1
#define FIPS_POKER 2
#define FIPS_RUNS 4
#define FIPS_LONGRUN 8
struct fips_stats {
	u32 ones;                       /* monobit: 9725 < ones < 10275 */
	u32 poker;                      /* 5000 * X: 10800 < poker < 230850 */
	u32 runs[2][6];                 /* runs of 0 and 1 of lengths 1-5, 6 and more */
	u32 longest;                    /* < 26 */
};
u32 fips140_2(const u8 *block, struct fips_stats *st);

/* SP 800-90B's most common value estimate for n samples of 8 bits: the
 * min-entropy per sample in 1/256 bits (0-2048) */
u32 mcv_entropy(const u8 *samples, u32 n);
u32 log2_q8(u64 x);                     /* floor(log2(x) * 256), x > 0 */

#endif
