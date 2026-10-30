/* KolibriOS for ARM: random numbers (rng.h, docs/rng.md): the sources,
 * the seeding and the reseeding around the generator of rng_core.c.
 *
 * The first request (or rng_init after the platform's drivers) seeds:
 * the boot loader's seed, 64 bytes from each hardware source, the clocks;
 * the generator is seeded once 256 bits are credited. Should the sources
 * give too little, the CPU's timing jitter is collected until they are
 * there (a few seconds at most), with a warning: that is the block. Once
 * a minute, while a hardware source works, the pool takes 64 new bytes of
 * each and the key is made anew. The kernel runs with the interrupts
 * masked on one CPU: nothing here runs twice at once. */
#include "rng.h"
#include "rng_core.h"
#ifdef __aarch64__
#include "fdt.h"
#endif

#define MAX_SOURCES 4
#define PULL 64                         /* bytes taken from a source at a time */
#define RESEED_TICKS 6000               /* a minute */

struct source {
	const char *name;
	u32 (*read)(void *buf, u32 len);
	u32 quality;
	u32 last;                       /* the last word (the continuous test) */
	bool has_last, off, pulled;
	u32 bytes, repeats, empty;
	u32 seed_bits;                  /* credited to the first seed */
};

static struct rngc st;
static struct source src[MAX_SOURCES];
static int nsrc;
static bool gathered, failed_said, early_said, init_done;
static bool no_jitter;                  /* kolibri.rngjitter=off: tests of the wait for the seed */
const char *boot_arg(const char *name);
static u32 boot_bits, jitter_bits, jitter_samples, jitter_est;
static u8 boot_seed[128], kaslr_seed[8];
static u32 boot_len, kaslr_len;
static u32 last_reseed;
static char sources_text[192];
static bool core_up;

static void core(void)
{
	if (core_up) return;
	rngc_init(&st);
	core_up = true;
}

/* --- the clocks ------------------------------------------------------------- */

#ifdef __aarch64__
/* the generic timer: 19.2 MHz on the Pro1X, 62.5 MHz in QEMU */
static u64 fast_count(void) { return SYSREG_R(cntvct_el0); }
static u64 fast_hz(void) { return SYSREG_R(cntfrq_el0); }
#else
/* the cycle counter (PMCCNTR, the CPU's clock), where it runs; the
 * platform's own counter otherwise: on the N950 the 32 kHz sync timer
 * (from the TWL5031's crystal, independent of the CPU's DPLL), in QEMU's
 * realview the 24 MHz counter of the system registers */
#ifdef PLATFORM_N950
#define SLOW_PA 0x48320010u             /* 32KSYNCNT_CR */
#define SLOW_HZ 32768u
#define CM_ICLKEN_WKUP 0x48004c10u      /* bit 2: EN_32KSYNC */
#else
#define SLOW_PA 0x1000005cu             /* SYS_24MHZ */
#define SLOW_HZ 24000000u
#endif
static bool cycles;

static u32 slow_count(void) { return rd32(mmu_io_va(SLOW_PA)); }

static u32 pmccntr(void)
{
	u32 v;
	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
	return v;
}

static void clocks_on(void)
{
#ifdef PLATFORM_N950
	wr32(mmu_io_va(CM_ICLKEN_WKUP), rd32(mmu_io_va(CM_ICLKEN_WKUP)) | 1u << 2);
#endif
	u32 pmcr;
	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr | 1));     /* E */
	asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31));     /* PMCNTENSET: C */
	u32 a = pmccntr();
	for (volatile int i = 0; i < 1000; i++)
		;
	cycles = pmccntr() != a;
}

static u64 fast_count(void) { return cycles ? pmccntr() : slow_count(); }
#endif

/* --- the sources ------------------------------------------------------------ */

void rng_register(const char *name, u32 (*read)(void *buf, u32 len), u32 quality)
{
	if (nsrc == MAX_SOURCES) return;
	struct source *s = &src[nsrc++];
	s->name = name;
	s->read = read;
	s->quality = MIN(quality, (u32)RNG_FULL);
	kprintf("rng: %s\n", name);
}

/* PULL bytes (whole words) into the pool; the credited bits. A word as
 * the one before (FIPS 140-2's continuous test, 4.9.2) throws the read
 * away; three times, and the source is not used any more */
static u32 pull(struct source *s)
{
	u32 w[PULL / 4];
	if (s->off) return 0;
	u32 got = s->read(w, sizeof(w)) & ~3u;
	if (!got) {
		s->empty++;
		return 0;
	}
	if (!rng_continuous_ok(&s->last, &s->has_last, w, got / 4)) {
		crypto_wipe(w, sizeof(w));
		if (++s->repeats >= 3) {
			s->off = true;
			kprintf("rng: %s gives the same word again and again: not used\n", s->name);
		}
		return 0;
	}
	u32 bits = (u32)((u64)got * 8 * s->quality / RNG_FULL);
	rngc_mix(&st, w, got, bits);
	s->bytes += got;
	crypto_wipe(w, sizeof(w));
	return bits;
}

#ifdef __aarch64__
/* before the MMU is on (arch_platform): the seeds kexec put in /chosen
 * (Linux's of_kexec_alloc_and_setup_fdt: 128 bytes of its generator as
 * rng-seed, 8 as kaslr-seed, only once that was seeded), and QEMU's virt
 * (32 bytes of the host's); copied and wiped in the tree, as Linux does
 * (early_init_dt_scan_chosen) */
static void take(void *fdt, int node, const char *name, u8 *to, u32 size, u32 *got)
{
	int len = 0;
	u8 *p = (u8 *)fdt_prop(fdt, node, name, &len);
	if (!p || len <= 0) return;
	*got = MIN((u32)len, size);
	memcpy(to, p, *got);
	memset(p, 0, len);
}

void rng_fdt_seed(void *fdt)
{
	int ac, sc;
	int ch = fdt_ok(fdt) ? fdt_find_name(fdt, "chosen", &ac, &sc) : -1;
	if (ch < 0) return;
	take(fdt, ch, "rng-seed", boot_seed, sizeof(boot_seed), &boot_len);
	take(fdt, ch, "kaslr-seed", kaslr_seed, sizeof(kaslr_seed), &kaslr_len);
}
#else
void rng_fdt_seed(void *fdt) { (void)fdt; }
#endif

/* --- the jitter --------------------------------------------------------------- */

/* A walk through 64 KiB (beyond the L1 cache) of a length that depends on
 * the last sample, timed by the fast counter; on the N950 it ends at the
 * next edge of the 32 kHz clock, so that the sample also holds the phase
 * between the CPU's clock and that crystal. How much of it is
 * unpredictable is not known: each batch of 1024 samples is estimated
 * (the most common value of their low bytes) and credited 16 bits, 1/64
 * of a bit per sample, only when the estimate is at least 1 bit per
 * sample; that is a guess with a margin, not a bound (docs/rng.md). */
#define JMEM (64u << 10)
#define JBATCH 1024u
static volatile u8 *jmem;
static u32 jpos, jprev;

static u32 jitter_sample(void)
{
	if (!jmem) jmem = kmalloc(JMEM);
	u32 t0 = (u32)fast_count(), steps = 64 + (jprev & 63);
	for (u32 i = 0; i < steps; i++) {
		jpos = (jpos + 4099 + (jprev & 0xff)) & (JMEM - 1);
		jmem[jpos] += (u8)i;
	}
#ifdef PLATFORM_N950
	u32 c = slow_count();
	for (u32 i = 0; i < 100000 && slow_count() == c; i++)
		;
#endif
	jprev = (u32)fast_count() - t0;
	return jprev;
}

static u8 jlow[JBATCH];

/* one batch: its estimate (1/256 bits per sample) */
static u32 jitter_batch(u32 *credited)
{
	for (u32 i = 0; i < JBATCH; i++) {
		u32 v = jitter_sample();
		jlow[i] = (u8)v;
		rngc_mix(&st, &v, sizeof(v), 0);
	}
	u32 est = mcv_entropy(jlow, JBATCH);
	*credited = est >= 256 ? JBATCH / 64 : 0;
	rngc_mix(&st, jlow, JBATCH, *credited);
	jitter_samples += JBATCH;
	jitter_bits += *credited;
	return est;
}

/* --- seeding ---------------------------------------------------------------- */

static void snp(char *b, u32 size, const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	kvsnprintf(b, size, fmt, ap);
	__builtin_va_end(ap);
}

static void add_text(const char *s)
{
	u32 n = strlen(sources_text);
	if (n && n + 2 < sizeof(sources_text)) sources_text[n++] = ',', sources_text[n++] = ' ';
	strncpy0(sources_text + n, s, sizeof(sources_text) - n);
}

static void gather(void)
{
	core();
	gathered = true;
	const char *j = boot_arg("kolibri.rngjitter");
	no_jitter = j && !strncmp(j, "off", 3);
#ifndef __aarch64__
	clocks_on();
#endif
	u64 c = fast_count();
	u32 t = rtc_seconds();
	rngc_mix(&st, &c, sizeof(c), 0);
	rngc_mix(&st, &t, sizeof(t), 0);
	if (boot_len) {
		boot_bits = boot_len * 8;
		rngc_mix(&st, boot_seed, boot_len, boot_bits);
		crypto_wipe(boot_seed, sizeof(boot_seed));
	}
	if (kaslr_len) {
		rngc_mix(&st, kaslr_seed, kaslr_len, 0);
		crypto_wipe(kaslr_seed, sizeof(kaslr_seed));
	}
}

static void say_seeded(void)
{
	char b[112];
	sources_text[0] = 0;
	for (int i = 0; i < nsrc; i++)
		if (src[i].seed_bits) {
			snp(b, sizeof(b), "%s (%u bytes)", src[i].name, src[i].bytes);
			add_text(b);
		}
	if (boot_bits) {
		snp(b, sizeof(b), "the boot loader's seed (%u bytes)", boot_len);
		add_text(b);
	}
	if (jitter_samples) {
		snp(b, sizeof(b), "timing jitter (%u samples, a batch's lowest estimate %u.%02u bits each, %u credited)",
		    jitter_samples, jitter_est / 256, jitter_est % 256 * 100 / 256, jitter_bits);
		add_text(b);
	}
	kprintf("rng: seeded from %s\n", sources_text);
	if (rng_weak())
		kprintf("rng: WEAK: no hardware source and no boot loader's seed, only timing jitter; "
			"not for keys that matter (docs/rng.md)\n");
}

/* seeds if it is not; jitter for at most `batches` batches (the long
 * attempt once, then one batch per call) */
static bool long_tried;

static bool seed(u32 batches)
{
	if (st.seeded) return true;
	if (!gathered) gather();
	for (int i = 0; i < nsrc; i++)
		if (!src[i].pulled) {
			src[i].pulled = true;
			src[i].seed_bits = pull(&src[i]);
		}
	if (long_tried) batches = 1;
	long_tried = true;
	if (no_jitter) batches = 0;
	for (u32 b = 0; b < batches && st.pool_bits < RNG_SEED_BITS; b++) {
		u32 cr, est = jitter_batch(&cr);
		if (!jitter_est || est < jitter_est) jitter_est = est;
	}
	if (!rngc_reseed(&st)) {
		if (!failed_said && no_jitter)
			kprintf("rng: NOT seeded: %u of %u bits, no jitter (kolibri.rngjitter=off)\n",
				st.pool_bits, RNG_SEED_BITS);
		else if (!failed_said)
			kprintf("rng: NOT seeded: %u of %u bits (the jitter's estimate %u/256 bits per sample)\n",
				st.pool_bits, RNG_SEED_BITS, jitter_est);
		failed_said = true;
		return false;
	}
	last_reseed = ticks;
	say_seeded();
	return true;
}

/* a minute after the last reseed: 64 new bytes from each hardware source,
 * the key anew from them */
static void maybe_reseed(void)
{
	if ((u32)(ticks - last_reseed) < RESEED_TICKS) return;
	last_reseed = ticks;
	u64 c = fast_count();
	rngc_mix(&st, &c, sizeof(c), 0);
	for (int i = 0; i < nsrc; i++) pull(&src[i]);
	rngc_reseed(&st);
}

void rng_init(void)
{
	init_done = true;
	seed(64);
}

bool rng_ready(void) { return st.seeded; }

bool rng_weak(void)
{
	u32 bits = boot_bits;
	for (int i = 0; i < nsrc; i++) bits += src[i].seed_bits;
	return st.seeded && bits < RNG_SEED_BITS;
}

const char *rng_sources(void)
{
	return st.seeded ? sources_text : "not seeded";
}

bool rng_bytes(void *p, u32 len)
{
	if (!seed(64)) return false;
	maybe_reseed();
	rngc_generate(&st, p, len);
	return true;
}

bool rng_bytes_nb(void *p, u32 len)
{
	if (!st.seeded) return false;
	maybe_reseed();
	rngc_generate(&st, p, len);
	return true;
}

/* a step towards the seed for a waiting caller (getrandom): one batch */
bool rng_seed_step(void)
{
	return seed(1);
}

/* before rng_init (lwIP's start, before the platform's sources are all
 * there) from the pool so far without seeding; after it, a seed tried */
void rng_bytes_insecure(void *p, u32 len)
{
	if (!st.seeded && init_done) seed(1);
	if (st.seeded) {
		maybe_reseed();
	} else {
		if (!gathered) gather();
		u64 c = fast_count();
		rngc_mix(&st, &c, sizeof(c), 0);
		rngc_stir(&st);
		if (!early_said) kprintf("rng: asked before the seed (lwIP's start): from the pool so far\n");
		early_said = true;
	}
	rngc_generate(&st, p, len);
}

u32 rng_u32(void)
{
	u32 v;
	rng_bytes_insecure(&v, sizeof(v));
	return v;
}

void rng_event(u32 v)
{
	core();
	u32 e[2] = { (u32)fast_count(), v };
	rngc_mix(&st, e, sizeof(e), 0);
}

/* --- kolibri.rngtest ------------------------------------------------------------ */

static u64 elapsed_us(u64 t0)
{
#ifdef __aarch64__
	return (fast_count() - t0) * 1000000 / fast_hz();
#else
	return (u64)(u32)(slow_count() - (u32)t0) * 1000000 / SLOW_HZ;
#endif
}

static u64 now_mark(void)
{
#ifdef __aarch64__
	return fast_count();
#else
	return slow_count();
#endif
}

static u8 tblock[FIPS_BLOCK];

/* FIPS 140-2 on `blocks` blocks of a reader's output; how many passed */
static u32 fips_run(const char *name, u32 (*read)(void *, u32), u32 blocks)
{
	u32 pass = 0, bad = 0, min1 = 0xffffffffu, max1 = 0, longest = 0;
	u64 t0 = now_mark();
	u32 bytes = 0;
	for (u32 b = 0; b < blocks; b++) {
		u32 got = 0, empty = 0;
		while (got < FIPS_BLOCK && empty < 16) {
			u32 n = read(tblock + got, MIN(FIPS_BLOCK - got, (u32)PULL));
			got += n;
			empty = n ? 0 : empty + 1;
			if (!n) bad++;
		}
		bytes += got;
		if (got < FIPS_BLOCK) {
			kprintf("rngtest: %s: block %u: only %u bytes\n", name, b, got);
			break;
		}
		struct fips_stats s;
		u32 f = fips140_2(tblock, &s);
		if (!f) pass++;
		else kprintf("rngtest: %s: block %u fails%s%s%s%s\n", name, b, f & FIPS_MONOBIT ? " monobit" : "",
			     f & FIPS_POKER ? " poker" : "", f & FIPS_RUNS ? " runs" : "", f & FIPS_LONGRUN ? " long-run" : "");
		min1 = MIN(min1, s.ones), max1 = MAX(max1, s.ones), longest = MAX(longest, s.longest);
	}
	u64 us = elapsed_us(t0);
	u32 est = mcv_entropy(tblock, FIPS_BLOCK);
	kprintf("rngtest: %s: %u of %u blocks pass FIPS 140-2 (ones %u-%u of 20000, longest run %u); "
		"%u bytes in %u us; most common byte: %u.%02u bits per byte; %u empty reads\n",
		name, pass, blocks, min1 == 0xffffffffu ? 0 : min1, max1, longest, bytes, (u32)us,
		est / 256, est % 256 * 100 / 256, bad);
	crypto_wipe(tblock, sizeof(tblock));
	return pass;
}

/* a source's raw output, through the continuous test as in use */
static int test_src;
static u32 read_src(void *p, u32 n)
{
	struct source *s = &src[test_src];
	u32 w[PULL / 4], got = s->read(w, MIN(n, (u32)sizeof(w))) & ~3u;
	if (got && !rng_continuous_ok(&s->last, &s->has_last, w, got / 4) && ++s->repeats >= 3 && !s->off) {
		s->off = true;
		kprintf("rng: %s gives the same word again and again: not used\n", s->name);
	}
	memcpy(p, w, got);
	return got;
}
static u32 read_gen(void *p, u32 n) { return rng_bytes(p, n) ? n : 0; }

void rng_selftest(void)
{
	kprintf("rngtest: the generator: %s; %s\n", rng_sources(), st.seeded ? "seeded" : "NOT seeded");
	for (int i = 0; i < nsrc; i++) {
		test_src = i;
		fips_run(src[i].name, read_src, 8);
		kprintf("rngtest: %s: %u repeated words, %u empty reads while in use\n", src[i].name,
			src[i].repeats, src[i].empty);
	}
	/* the jitter: its estimates, the time per sample */
	u64 t0 = now_mark();
	u32 lo = 0xffffffffu, hi = 0;
	for (int b = 0; b < 4; b++) {
		u32 cr, est = jitter_batch(&cr);
		lo = MIN(lo, est), hi = MAX(hi, est);
	}
	u64 us = elapsed_us(t0);
	kprintf("rngtest: timing jitter: 4096 samples in %u us; most common value: %u.%02u-%u.%02u bits per sample "
		"(credited: 1/64 bit per sample from 1 bit on)\n", (u32)us, lo / 256, lo % 256 * 100 / 256,
		hi / 256, hi % 256 * 100 / 256);
	fips_run("the generator", read_gen, 8);
}
