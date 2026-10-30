/* KolibriOS for ARM: the random number generator's core (rng_core.h) */
#include "rng_core.h"

/* --- ChaCha20 (RFC 8439, 2.3) ---------------------------------------------- */

static inline u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static inline void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24);
}

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), \
			a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
	u32 s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 }, x[16];
	for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
	s[12] = counter;
	for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
	for (int i = 0; i < 16; i++) x[i] = s[i];
	for (int i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
	}
	for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
	crypto_wipe(x, sizeof(x));
	crypto_wipe(s, sizeof(s));
}

/* --- the pool and the generator -------------------------------------------- */

static const u8 zero_nonce[12];
static const char reseed_label[] = "KolibriOS for ARM rng reseed";

void rngc_init(struct rngc *s)
{
	memset(s, 0, sizeof(*s));
	sha256_init(&s->pool);
}

void rngc_mix(struct rngc *s, const void *p, u32 len, u32 bits)
{
	sha256_update(&s->pool, p, len);
	s->pool_bits = s->pool_bits + bits < s->pool_bits ? 0xffffffffu : s->pool_bits + bits;
}

bool rngc_reseed(struct rngc *s)
{
	if (s->pool_bits < RNG_SEED_BITS) return false;
	u8 h[SHA256_LEN], n[8];
	sha256_final(&s->pool, h);
	for (int i = 0; i < 8; i++) n[i] = (u8)(s->reseeds >> (8 * i));
	const u8 *part[3] = { (const u8 *)reseed_label, h, n };
	const u32 len[3] = { sizeof(reseed_label) - 1, sizeof(h), sizeof(n) };
	hmac_sha256_vec(s->key, sizeof(s->key), 3, part, len, s->key);
	sha256_init(&s->pool);
	sha256_update(&s->pool, h, sizeof(h));
	crypto_wipe(h, sizeof(h));
	s->pool_bits = 0;
	s->reseeds++;
	s->generated = 0;
	s->seeded = true;
	return true;
}

static const char stir_label[] = "KolibriOS for ARM rng before the seed";

void rngc_stir(struct rngc *s)
{
	struct sha256 c = s->pool;
	u8 h[SHA256_LEN];
	sha256_final(&c, h);
	const u8 *part[2] = { (const u8 *)stir_label, h };
	const u32 len[2] = { sizeof(stir_label) - 1, sizeof(h) };
	hmac_sha256_vec(s->key, sizeof(s->key), 2, part, len, s->key);
	crypto_wipe(h, sizeof(h));
	crypto_wipe(&c, sizeof(c));
}

void rngc_generate(struct rngc *s, void *out, u32 len)
{
	u8 b[64], k[32];
	chacha20_block(s->key, 0, zero_nonce, b);
	memcpy(s->key, b, 32);                  /* the next request's key */
	memcpy(k, b + 32, 32);                  /* this one's */
	u8 *d = out;
	for (u32 ctr = 0; len; ctr++) {
		chacha20_block(k, ctr, zero_nonce, b);
		u32 n = MIN(len, 64u);
		memcpy(d, b, n);
		d += n, len -= n;
		s->generated += n;
	}
	crypto_wipe(b, sizeof(b));
	crypto_wipe(k, sizeof(k));
}

bool rng_continuous_ok(u32 *last, bool *has_last, const u32 *w, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		if (*has_last && w[i] == *last) return false;
		*last = w[i];
		*has_last = true;
	}
	return true;
}

/* --- FIPS 140-2's statistical tests (4.9.1, change notice 1) ----------------- */

u32 fips140_2(const u8 *block, struct fips_stats *st)
{
	static const u32 lo[6] = { 2315, 1114, 527, 240, 103, 103 };
	static const u32 hi[6] = { 2685, 1386, 723, 384, 209, 209 };
	struct fips_stats s;
	u32 f[16] = { 0 }, fail = 0;
	memset(&s, 0, sizeof(s));
	int run_bit = -1;
	u32 run = 0;
	for (u32 i = 0; i < FIPS_BLOCK; i++) {
		u8 v = block[i];
		f[v >> 4]++;
		f[v & 15]++;
		for (int j = 7; j >= 0; j--) {
			int b = (v >> j) & 1;
			s.ones += b;
			if (b == run_bit) {
				run++;
				continue;
			}
			if (run_bit >= 0) {
				s.runs[run_bit][MIN(run, 6u) - 1]++;
				if (run > s.longest) s.longest = run;
			}
			run_bit = b;
			run = 1;
		}
	}
	s.runs[run_bit][MIN(run, 6u) - 1]++;
	if (run > s.longest) s.longest = run;
	/* poker: X = 16 / 5000 * sum(f^2) - 5000; 5000 * X = 16 * sum - 25e6 */
	u64 sum = 0;
	for (int i = 0; i < 16; i++) sum += (u64)f[i] * f[i];
	s64 px = (s64)(16 * sum) - 25000000;
	s.poker = px < 0 ? 0 : (u32)px;
	if (!(s.ones > 9725 && s.ones < 10275)) fail |= FIPS_MONOBIT;
	if (!(px > 10800 && px < 230850)) fail |= FIPS_POKER;
	for (int b = 0; b < 2; b++)
		for (int l = 0; l < 6; l++)
			if (s.runs[b][l] < lo[l] || s.runs[b][l] > hi[l]) fail |= FIPS_RUNS;
	if (s.longest >= 26) fail |= FIPS_LONGRUN;
	if (st) *st = s;
	return fail;
}

/* --- SP 800-90B, 6.3.1: the most common value estimate --------------------- */

u32 log2_q8(u64 x)
{
	int ip = 63;
	while (!(x >> ip)) ip--;
	/* the mantissa in [1, 2) as Q31 in [2^31, 2^32), then its square
	 * eight times: each time it reaches 2 a bit of the fraction */
	u64 m = ip >= 31 ? x >> (ip - 31) : x << (31 - ip);
	u32 frac = 0;
	for (int i = 0; i < 8; i++) {
		m = (m * m) >> 31;
		if (m >= 1ull << 32) {
			m >>= 1;
			frac |= 1u << (7 - i);
		}
	}
	return (u32)ip * 256 + frac;
}

static u64 isqrt64(u64 v)
{
	u64 r = 0;
	for (u64 bit = 1ull << 62; bit; bit >>= 2) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
	}
	return r;
}

/* p = the most common value's share, its upper bound at 99 %
 * p_u = min(1, p + 2.576 * sqrt(p (1 - p) / (n - 1))), H = -log2(p_u);
 * in Q32 fixed point (the AArch64 kernel has no floating point) */
u32 mcv_entropy(const u8 *samples, u32 n)
{
	if (n < 2) return 0;
	u32 count[256] = { 0 }, top = 0;
	for (u32 i = 0; i < n; i++)
		if (++count[samples[i]] > top) top = count[samples[i]];
	u64 p = ((u64)top << 32) / n;                   /* Q32 */
	u64 var = p * ((1ull << 32) - p) / (n - 1);     /* Q64 */
	u64 pu = p + isqrt64(var) * 2576 / 1000;        /* Q32 */
	if (pu >= 1ull << 32) return 0;
	return 32 * 256 - log2_q8(pu);
}
