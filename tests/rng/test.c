/* The random number generator on the PC (make -C tests/rng):
 *
 *  - ChaCha20 against RFC 8439's vectors (2.3.2, 2.4.2)
 *  - the generator's rules: no seed below 256 credited bits, a new key
 *    after every request (fast key erasure), a new output after a
 *    reseed, the stir before the seed keeps the pool
 *  - FIPS 140-2's tests on fixed patterns, the continuous test
 *  - the Qualcomm PRNG's driver (kernel/platform/qcom_prng.c) against a
 *    model of its registers and of its clock in the GCC: the vote around
 *    each read, the other votes kept, hardware gating, a stuck clock, a
 *    word of zero, no word, no access to the PRNG while its clock is off,
 *    never its configuration (TrustZone's)
 *
 *   ./test              these checks
 *   ./test dump         the outputs tests/rng/model.py compares with its
 *                       own (the generator, FIPS 140-2's statistics, the
 *                       most common value estimate, log2) */
#include <stdio.h>
#include <stdlib.h>
#include "rng_core.h"
#include "platform/qcom_prng.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
	else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const char *tag, const u8 *p, u32 n)
{
	printf("%s ", tag);
	for (u32 i = 0; i < n; i++) printf("%02x", p[i]);
	printf("\n");
}

static void unhex(const char *s, u8 *out)
{
	for (u32 i = 0; s[2 * i]; i++) {
		unsigned v;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (u8)v;
	}
}

/* --- ChaCha20 ---------------------------------------------------------------- */

static void test_chacha(void)
{
	u8 key[32], nonce[12], out[64], want[64];
	for (int i = 0; i < 32; i++) key[i] = (u8)i;
	unhex("000000090000004a00000000", nonce);
	chacha20_block(key, 1, nonce, out);
	unhex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
	      "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", want);
	CHECK(!memcmp(out, want, 64), "ChaCha20 block: RFC 8439 2.3.2");
	/* 2.4.2: the keystream from counter 1 under nonce 0:0:4a:0 */
	unhex("000000000000004a00000000", nonce);
	chacha20_block(key, 1, nonce, out);
	unhex("224f51f3401bd9e12fde276fb8631ded8c131f823d2c06e27e4fcaec9ef3cf78"
	      "8a3b0aa372600a92b57974cded2b9334794cba40c63e34cdea212c4cf07d41b7", want);
	CHECK(!memcmp(out, want, 64), "ChaCha20 keystream: RFC 8439 2.4.2 (block 1)");
}

/* --- the generator -------------------------------------------------------------- */

static void test_generator(void)
{
	struct rngc s;
	u8 a[100], b[100], k0[32];
	rngc_init(&s);
	rngc_mix(&s, "abc", 3, 255);
	CHECK(!rngc_reseed(&s) && !s.seeded, "no seed with 255 credited bits");
	rngc_mix(&s, "d", 1, 1);
	CHECK(rngc_reseed(&s) && s.seeded && s.pool_bits == 0 && s.reseeds == 1, "seeded with 256");
	memcpy(k0, s.key, 32);
	rngc_generate(&s, a, sizeof(a));
	CHECK(memcmp(k0, s.key, 32) != 0, "a new key after a request (fast key erasure)");
	/* the old key gives the output again, the new one does not */
	struct rngc t = s;
	memcpy(t.key, k0, 32);
	rngc_generate(&t, b, sizeof(b));
	CHECK(!memcmp(a, b, sizeof(a)), "the request's output from the key before it, only");
	rngc_generate(&s, b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0, "the next request differs");
	CHECK(s.generated == 200, "200 bytes counted");
	/* the stir: the key changes, the pool stays */
	struct rngc u;
	rngc_init(&u);
	rngc_mix(&u, "x", 1, 10);
	struct sha256 before = u.pool;
	rngc_stir(&u);
	CHECK(!memcmp(&before, &u.pool, sizeof(before)) && u.pool_bits == 10 && !u.seeded,
	      "the stir before the seed keeps the pool and its credit");
	/* a reseed with the same pool from two keys: different */
	struct rngc v1, v2;
	rngc_init(&v1);
	rngc_init(&v2);
	v2.key[0] = 1;
	rngc_mix(&v1, "same", 4, 256), rngc_mix(&v2, "same", 4, 256);
	rngc_reseed(&v1), rngc_reseed(&v2);
	CHECK(memcmp(v1.key, v2.key, 32) != 0, "the reseed keeps the old key's part");
}

/* --- FIPS 140-2 ------------------------------------------------------------------- */

static void test_fips(void)
{
	static u8 b[FIPS_BLOCK];
	struct fips_stats st;
	memset(b, 0, sizeof(b));
	CHECK(fips140_2(b, &st) == (FIPS_MONOBIT | FIPS_POKER | FIPS_RUNS | FIPS_LONGRUN) && st.longest == 20000,
	      "all zero: every test fails");
	memset(b, 0x55, sizeof(b));
	CHECK(fips140_2(b, &st) == (FIPS_POKER | FIPS_RUNS) && st.ones == 10000 && st.longest == 1,
	      "01010101...: monobit passes, poker and runs fail");
	memset(b, 0xff, sizeof(b));
	CHECK(fips140_2(b, &st) == (FIPS_MONOBIT | FIPS_POKER | FIPS_RUNS | FIPS_LONGRUN), "all one: every test fails");
	/* the generator's output passes */
	struct rngc s;
	rngc_init(&s);
	rngc_mix(&s, "fips", 4, 256);
	rngc_reseed(&s);
	int pass = 0;
	for (int i = 0; i < 20; i++) {
		rngc_generate(&s, b, sizeof(b));
		pass += !fips140_2(b, NULL);
	}
	CHECK(pass >= 19, "the generator's output: %d of 20 blocks pass", pass);
	/* a run of 26 ones in the middle of random data */
	rngc_generate(&s, b, sizeof(b));
	b[1000] = 0x7f, b[1001] = 0xff, b[1002] = 0xff, b[1003] = 0xfe;    /* 0 then 1 x 30 then 0 */
	CHECK(fips140_2(b, &st) & FIPS_LONGRUN, "a run of 30 ones: the long run test fails");
	/* the continuous test */
	u32 last = 0;
	bool has = false;
	u32 w1[3] = { 1, 2, 3 }, w2[2] = { 3, 4 }, w3[2] = { 5, 5 };
	CHECK(rng_continuous_ok(&last, &has, w1, 3), "continuous: 1 2 3");
	CHECK(!rng_continuous_ok(&last, &has, w2, 2), "continuous: 3 after 3 (across reads) refused");
	CHECK(!rng_continuous_ok(&last, &has, w3, 2), "continuous: 5 5 refused");
	/* the most common value estimate */
	static u8 m[4096];
	memset(m, 7, sizeof(m));
	CHECK(mcv_entropy(m, sizeof(m)) == 0, "MCV: a constant has none");
	for (u32 i = 0; i < sizeof(m); i++) m[i] = (u8)i;
	u32 e = mcv_entropy(m, sizeof(m));
	CHECK(e > 6 * 256 && e < 8 * 256, "MCV: all values alike: %u/256 bits", e);
}

/* --- the Qualcomm PRNG against a model of its registers ------------------------------ */

#define PRNG_BASE 0x1b53000ul
#define GCC_BASE 0x1400000ul

static struct model {
	u32 vote, cbcr_extra;           /* the vote register; CBCR bits besides CLK_OFF */
	bool hwctl, clock_stuck;        /* hardware gating; CLK_OFF never clears */
	int on_after;                   /* CBCR reads until the clock is on */
	int cbcr_reads;
	const u32 *words;
	int nwords, next;
	int avail_after, polls;         /* STATUS polls until a word is there */
	bool never_avail;
	u64 us;                         /* the time udelay waited */
	int bad;                        /* accesses the hardware would not take */
	char why[128];
} md;

static bool clock_running(void)
{
	return (md.vote >> GCC_PRNG_VOTE_BIT & 1) && !md.clock_stuck && md.cbcr_reads >= md.on_after;
}

static u32 m_rd(addr_t a)
{
	if (a == GCC_BASE + GCC_APCS_VOTE) return md.vote;
	if (a == GCC_BASE + GCC_PRNG_AHB_CBCR) {
		md.cbcr_reads++;
		return md.cbcr_extra | (md.hwctl ? CBCR_HW_CTL : 0) | (clock_running() ? 0 : CBCR_CLK_OFF);
	}
	if (a >= PRNG_BASE && a < PRNG_BASE + 0x1000) {
		/* with hardware gating the clock runs whenever the bus asks */
		if (!(md.vote >> GCC_PRNG_VOTE_BIT & 1)) {
			md.bad++;
			snprintf(md.why, sizeof(md.why), "PRNG read at +0x%lx without the vote", a - PRNG_BASE);
		}
		if (a - PRNG_BASE >= 0x100) {
			md.bad++;
			snprintf(md.why, sizeof(md.why), "PRNG configuration read (+0x%lx)", a - PRNG_BASE);
		}
		if (a == PRNG_BASE + PRNG_STATUS) {
			if (md.never_avail || md.next >= md.nwords) return 0;
			return ++md.polls > md.avail_after ? PRNG_STATUS_DATA_AVAIL : 0;
		}
		if (a == PRNG_BASE + PRNG_DATA_OUT) {
			md.polls = 0;
			return md.next < md.nwords ? md.words[md.next++] : 0;
		}
	}
	md.bad++;
	snprintf(md.why, sizeof(md.why), "read of 0x%lx", a);
	return 0;
}

static void m_wr(addr_t a, u32 v)
{
	if (a == GCC_BASE + GCC_APCS_VOTE) {
		md.vote = v;
		if (!(v >> GCC_PRNG_VOTE_BIT & 1)) md.cbcr_reads = 0;
		return;
	}
	md.bad++;
	snprintf(md.why, sizeof(md.why), "write of 0x%x to 0x%lx", v, a);
}

static void m_udelay(u32 us) { md.us += us; }

static struct qprng q;

static void model(const u32 *w, int n)
{
	memset(&md, 0, sizeof(md));
	md.vote = 0x00c00011;           /* other branches' votes, to be kept */
	md.words = w;
	md.nwords = n;
	md.on_after = 3;
	md.avail_after = 2;
	q = (struct qprng){ .base = PRNG_BASE, .gcc = GCC_BASE, .rd = m_rd, .wr = m_wr, .udelay = m_udelay };
}

static void test_prng(void)
{
	static const u32 w[] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff01, 0x12345678, 0x9abcdef0 };
	u8 out[24];

	model(w, 6);
	u32 n = qprng_read(&q, out, 24);
	CHECK(n == 24 && !memcmp(out, w, 24) && q.words == 6, "six words read");
	CHECK(md.vote == 0x00c00011, "the vote taken back, the other votes kept (0x%08x)", md.vote);
	CHECK(md.bad == 0, "no access the hardware would refuse%s%s", md.bad ? ": " : "", md.why);

	model(w, 6);
	n = qprng_read(&q, out, 6);
	CHECK(n == 6 && !memcmp(out, w, 6) && md.next == 2, "6 bytes: two words, half of the second");

	model(w, 6);
	md.hwctl = true, md.clock_stuck = true;
	n = qprng_read(&q, out, 8);
	CHECK(n == 8 && q.clock_stuck == 0 && md.bad == 0, "hardware gating: CLK_OFF not waited for (as Linux)");

	model(w, 6);
	md.clock_stuck = true;
	n = qprng_read(&q, out, 8);
	CHECK(n == 0 && q.clock_stuck == 1 && !(md.vote >> GCC_PRNG_VOTE_BIT & 1) && md.bad == 0,
	      "a clock that does not come on: nothing read, the PRNG not touched, the vote back");
	CHECK(md.us >= 200 && md.us <= 300, "... after 200 us (%llu)", (unsigned long long)md.us);

	model(w, 6);
	md.cbcr_extra = (u32)FSM_STATUS_ON << 28;
	md.clock_stuck = true;
	n = qprng_read(&q, out, 4);
	CHECK(n == 4, "the NOC FSM's status on: the clock taken as on (as Linux)");

	static const u32 z[] = { 0x11111111, 0, 0x22222222 };
	model(z, 3);
	memset(out, 0xaa, sizeof(out));
	n = qprng_read(&q, out, 12);
	CHECK(n == 0 && q.zeros == 1 && out[0] == 0 && out[3] == 0, "a word of zero: the read fails, nothing kept");

	model(w, 6);
	md.never_avail = true;
	n = qprng_read(&q, out, 4);
	CHECK(n == 0 && q.timeouts == 1 && md.us >= 10000 && md.us <= 10500 && !(md.vote >> GCC_PRNG_VOTE_BIT & 1),
	      "no word: given up after 10 ms (%llu us), the vote back", (unsigned long long)md.us);

	model(w, 6);
	md.avail_after = 20;
	n = qprng_read(&q, out, 8);
	CHECK(n == 8 && md.bad == 0, "a slow PRNG (20 polls a word) waited for");
}

/* --- the dump for model.py ---------------------------------------------------------- */

static void dump(void)
{
	struct rngc s;
	u8 b[FIPS_BLOCK];
	rngc_init(&s);
	rngc_mix(&s, "the boot loader's seed", 22, 128);
	rngc_mix(&s, "virtio-rng", 10, 128);
	rngc_stir(&s);
	rngc_generate(&s, b, 40);
	hex("stir", b, 40);
	rngc_reseed(&s);
	hex("key1", s.key, 32);
	rngc_generate(&s, b, 1);
	hex("gen1", b, 1);
	rngc_generate(&s, b, 64);
	hex("gen64", b, 64);
	rngc_generate(&s, b, 130);
	hex("gen130", b, 130);
	rngc_mix(&s, "a minute later", 14, 300);
	rngc_reseed(&s);
	hex("key2", s.key, 32);
	rngc_generate(&s, b, 33);
	hex("gen33", b, 33);
	for (int i = 0; i < 3; i++) {
		struct fips_stats st;
		rngc_generate(&s, b, FIPS_BLOCK);
		u32 f = fips140_2(b, &st);
		hex("fipsblock", b, FIPS_BLOCK);
		printf("fips %u %u %u %u", f, st.ones, st.poker, st.longest);
		for (int x = 0; x < 2; x++)
			for (int l = 0; l < 6; l++) printf(" %u", st.runs[x][l]);
		printf("\n");
	}
	/* MCV on shaped samples: a few values more often than the rest */
	for (int shape = 0; shape < 4; shape++) {
		static u8 m[1024];
		rngc_generate(&s, m, sizeof(m));
		for (u32 i = 0; i < sizeof(m); i++)
			if (shape == 1) m[i] &= 3;
			else if (shape == 2) m[i] = m[i] < 200 ? 9 : m[i];
			else if (shape == 3) m[i] &= 0x1f;
		hex("mcvsamples", m, sizeof(m));
		printf("mcv %u\n", mcv_entropy(m, sizeof(m)));
	}
	static const u64 xs[] = { 1, 2, 3, 1000, 0x80000000ull, 0xfffffffful, 0x123456789abcull };
	for (u32 i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
		printf("log2 %llu %u\n", (unsigned long long)xs[i], log2_q8(xs[i]));
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "dump")) {
		dump();
		return 0;
	}
	test_chacha();
	test_generator();
	test_fips();
	test_prng();
	printf("%d of %d checks passed\n", checks - fails, checks);
	return fails != 0;
}
