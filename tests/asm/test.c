/* The AArch64 kernel's assembler against its C (make -C tests/asm): the
 * drawing loops of arch/arm64/blit.S against kernel/blit.c's, and
 * string.S's memcpy, memmove, memset against byte loops, bit for bit,
 * over many sizes, alignments, owner maps and clippings; every byte of
 * the buffers compared, also around what should be written. A Linux
 * program for AArch64, run with qemu-aarch64 on the PC.
 *
 *   test         the comparisons
 *   test bench   the routines on the screen's sizes, C and assembler one
 *                after the other: count their instructions with QEMU's
 *                plugin (make -C tests/asm bench) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blit.h"

void *k_memcpy(void *d, const void *s, unsigned n);
void *k_memmove(void *d, const void *s, unsigned n);
void *k_memset(void *d, int c, unsigned n);

static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static u32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (u32)(rng >> 16);
}

static int failed, checks;
static void check(int ok, const char *what, int a, int b, int c, int d)
{
	checks++;
	if (ok) return;
	if (failed++ < 20) printf("FAIL %s (%d %d %d %d)\n", what, a, b, c, d);
}

#define GUARD 64
static void fill_rand(void *p, size_t n)
{
	for (size_t i = 0; i < n; i++) ((u8 *)p)[i] = rnd();
}

/* an owner map: all the slot's, none, random, alternating, runs */
static void owners(u8 *o, int n, int slot, int kind)
{
	int s = slot < 0 ? 3 : slot;
	for (int i = 0; i < n; i++) {
		switch (kind) {
		case 0: o[i] = s; break;
		case 1: o[i] = s + 1; break;
		case 2: o[i] = rnd() & 1 ? s : (u8)(s + 1 + (rnd() & 3)); break;
		case 3: o[i] = i & 1 ? s : 0; break;
		default: o[i] = (i / 7) & 1 ? s : (u8)(s ^ 1); break;
		}
	}
}

static void test_fill(void)
{
	static u32 a[2048 + 2 * GUARD], b[2048 + 2 * GUARD];
	static u8 o[2048 + 64];
	const int sizes[] = { 1080, 1081, 1500, 2000 };
	for (int ni = 0; ni < 100 + 4; ni++) {
		int n = ni < 100 ? ni : sizes[ni - 100];
		for (int off = 0; off < 8; off++)
			for (int oo = 0; oo < 3; oo++)
				for (int si = 0; si < 4; si++) {
					int slot = (int[]){ -1, 0, 5, 63 }[si];
					for (int kind = 0; kind < 5; kind++) {
						fill_rand(a, sizeof(a));
						memcpy(b, a, sizeof(a));
						owners(o + oo, n, slot, kind);
						u32 c = rnd() & 0xffffff;
						fill_c(a + GUARD + off, o + oo, slot, c, n);
						fill_neon(b + GUARD + off, o + oo, slot, c, n);
						check(!memcmp(a, b, sizeof(a)), "fill", n, off, slot, kind);
						if (slot < 0) break;
					}
				}
	}
}

static const u8 *glyph_rows(int fh)
{
	static u8 r[32];
	for (int i = 0; i < fh; i++) r[i] = rnd() & 1 ? rnd() : (i & 1 ? 0 : 0xff);
	return r;
}

static void test_glyph(void)
{
	enum { W = 40, H = 20 };
	static u32 a[W * H + 2 * GUARD], b[W * H + 2 * GUARD];
	static u8 o[W * H + 64];
	for (int fw = 1; fw <= 8; fw++)
		for (int fh = 1; fh <= 16; fh += (fh < 10 ? 9 : 6))
			for (int x = 0; x < 9; x++)
				for (int si = 0; si < 3; si++)
					for (int kind = 0; kind < 5; kind++)
						for (int fill = 0; fill < 2; fill++)
							for (int rep = 0; rep < 4; rep++) {
								int slot = (int[]){ -1, 0, 9 }[si];
								fill_rand(a, sizeof(a));
								memcpy(b, a, sizeof(a));
								owners(o, W * H, slot, kind);
								const u8 *g = glyph_rows(fh);
								u32 fg = rnd() & 0xffffff, bg = rnd() & 0xffffff;
								int stride = W - (rep & 1) * 3, y = rep & 2;
								u32 *pa = a + GUARD + y * stride + x, *pb = b + GUARD + y * stride + x;
								glyph_c(pa, stride, o + y * W + x, W, slot, g, fw, fh, fg, bg, fill);
								glyph_neon(pb, stride, o + y * W + x, W, slot, g, fw, fh, fg, bg, fill);
								check(!memcmp(a, b, sizeof(a)), "glyph", fw, fh, x, slot * 10 + fill);
								if (slot < 0) break;
							}
}

static void test_bgr(void)
{
	static u32 a[2048 + 2 * GUARD], b[2048 + 2 * GUARD];
	static u8 o[2048 + 64], img[3 * 2048 + 64], ia[3 * 2048 + 2 * GUARD], ib[3 * 2048 + 2 * GUARD];
	const int sizes[] = { 270, 1080, 1081 };
	for (int ni = 0; ni < 80 + 3; ni++) {
		int n = ni < 80 ? ni : sizes[ni - 80];
		for (int off = 0; off < 5; off++) {
			for (int si = 0; si < 3; si++)
				for (int kind = 0; kind < 5; kind++) {
					int slot = (int[]){ -1, 1, 40 }[si];
					fill_rand(a, sizeof(a));
					memcpy(b, a, sizeof(a));
					fill_rand(img, sizeof(img));
					owners(o + off, n, slot, kind);
					from_bgr_c(a + GUARD + off, o + off, slot, img + off, n);
					from_bgr_neon(b + GUARD + off, o + off, slot, img + off, n);
					check(!memcmp(a, b, sizeof(a)), "from_bgr", n, off, slot, kind);
					if (slot < 0) break;
				}
			fill_rand(ia, sizeof(ia));
			memcpy(ib, ia, sizeof(ia));
			fill_rand(a, sizeof(a));
			to_bgr_c(ia + GUARD + off, a + off, n);
			to_bgr_neon(ib + GUARD + off, a + off, n);
			check(!memcmp(ia, ib, sizeof(ia)), "to_bgr", n, off, 0, 0);
		}
	}
}

static void test_rot2x(void)
{
	/* a panel of up to 64 x 2 * 40 pixels, a screen of 40 x 32 */
	enum { PW = 64, SW = 40, SH = 32 };
	static u32 src[SW * SH + 64];
	static u8 pa[(2 * SW) * (PW * 4 + 16) + 2 * GUARD * 4], pb[sizeof(pa)];
	fill_rand(src, sizeof(src));
	for (int w = 1; w <= SW; w += (w < 13 ? 1 : 9))
		for (int pw = 2 * 8; pw <= PW; pw += 22)
			for (int rep = 0; rep < 30; rep++) {
				int ymax = pw / 2 < SH ? pw / 2 : SH;
				int y1 = 1 + rnd() % ymax, y0 = rnd() % y1;
				int ss = w + (rep & 3), ds = pw * 4 + (rep & 4) * 4;
				if (ss > SW) ss = SW;
				if (ss < w) continue;
				fill_rand(pa, sizeof(pa));
				memcpy(pb, pa, sizeof(pa));
				rot2x_c(pa + GUARD * 4, ds, src, ss, w, y0, y1, pw);
				rot2x_neon(pb + GUARD * 4, ds, src, ss, w, y0, y1, pw);
				check(!memcmp(pa, pb, sizeof(pa)), "rot2x", w, pw, y0, y1);
			}
	/* the Pro1X's: 1080 x 540 to 1080 x 2160, in bands */
	static u32 big[1080 * 540];
	static u8 *qa, *qb;
	qa = malloc(1080 * 4 * 2160);
	qb = malloc(1080 * 4 * 2160);
	fill_rand(big, sizeof(big));
	const int bands[][2] = { { 0, 540 }, { 0, 1 }, { 539, 540 }, { 13, 270 }, { 100, 107 } };
	for (int i = 0; i < 5; i++) {
		memset(qa, 0x5a, 1080 * 4 * 2160);
		memset(qb, 0x5a, 1080 * 4 * 2160);
		rot2x_c(qa, 1080 * 4, big, 1080, 1080, bands[i][0], bands[i][1], 1080);
		rot2x_neon(qb, 1080 * 4, big, 1080, 1080, bands[i][0], bands[i][1], 1080);
		check(!memcmp(qa, qb, 1080 * 4 * 2160), "rot2x pro1x", bands[i][0], bands[i][1], 0, 0);
	}
	/* and against the turn itself: (X, Y) at (1080 - 2 - 2 Y, 2 X) and
	 * its neighbours */
	rot2x_neon(qa, 1080 * 4, big, 1080, 1080, 0, 540, 1080);
	int bad = 0;
	for (int y = 0; y < 540; y += 7)
		for (int x = 0; x < 1080; x += 5) {
			u32 want = big[y * 1080 + x] | 0xff000000u;
			u32 *r0 = (u32 *)(qa + (size_t)(2 * x) * 1080 * 4) + (1080 - 2 - 2 * y);
			u32 *r1 = (u32 *)(qa + (size_t)(2 * x + 1) * 1080 * 4) + (1080 - 2 - 2 * y);
			if (r0[0] != want || r0[1] != want || r1[0] != want || r1[1] != want) bad = 1;
		}
	check(!bad, "rot2x geometry", 0, 0, 0, 0);
	free(qa);
	free(qb);
}

static void test_string(void)
{
	static u8 a[8192 + 2 * GUARD], b[8192 + 2 * GUARD], s[8192 + 64];
	int sizes[300], ns = 0;
	for (int i = 0; i < 260; i++) sizes[ns++] = i;
	sizes[ns++] = 1000; sizes[ns++] = 4095; sizes[ns++] = 4096; sizes[ns++] = 8000;
	for (int si = 0; si < ns; si++) {
		int n = sizes[si];
		for (int da = 0; da < 16; da++)
			for (int sa = 0; sa < 16; sa++) {
				if (n > 300 && (da & 3) != (sa & 3) && sa != 1) continue;
				fill_rand(s, sizeof(s));
				fill_rand(a, sizeof(a));
				memcpy(b, a, sizeof(a));
				for (int i = 0; i < n; i++) a[GUARD + da + i] = s[sa + i];
				void *r = k_memcpy(b + GUARD + da, s + sa, n);
				check(!memcmp(a, b, sizeof(a)) && r == b + GUARD + da, "memcpy", n, da, sa, 0);
			}
		for (int da = 0; da < 16; da++) {
			int c = (int[]){ 0, 0xff, 0x80, 0x1234, -1, 0x5a }[da % 6];
			fill_rand(a, sizeof(a));
			memcpy(b, a, sizeof(a));
			for (int i = 0; i < n; i++) a[GUARD + da + i] = (u8)c;
			void *r = k_memset(b + GUARD + da, c, n);
			check(!memcmp(a, b, sizeof(a)) && r == b + GUARD + da, "memset", n, da, c, 0);
		}
		/* overlapping both ways, and apart */
		if (n > 1000) continue;
		for (int d = -20; d <= 20; d++) {
			fill_rand(a, sizeof(a));
			memcpy(b, a, sizeof(a));
			int from = 2 * GUARD + 40, to = from + d;
			memmove(a + to, a + from, n);
			void *r = k_memmove(b + to, b + from, n);
			check(!memcmp(a, b, sizeof(a)) && r == b + to, "memmove", n, d, 0, 0);
		}
	}
	/* the size is 32 bits: the register's upper half must not count */
	fill_rand(a, sizeof(a));
	memcpy(b, a, sizeof(a));
	void *(*volatile cpy)(void *, const void *, unsigned long) = (void *)k_memcpy;
	cpy(b + GUARD, s, 0xffffffff00000010ul);
	memcpy(a + GUARD, s, 16);
	check(!memcmp(a, b, sizeof(a)), "memcpy 32-bit size", 0, 0, 0, 0);
}

/* the routines on the screen's sizes, for the instruction counts: a
 * screen of 1080 x 540 (the Pro1X's), its windows' rows owned by one
 * slot (the common case) and a half covered one; text; the task
 * switcher's pictures; the flush; the strings */
static void bench(const struct blit_ops *b, void *(*cpy)(void *, const void *, unsigned),
		  void *(*set)(void *, int, unsigned))
{
	enum { W = 1080, H = 540 };
	static u32 scr[W * H];
	static u8 own[W * H], img[270 * 136 * 3];
	static u32 panel[1080 * 2160];
	memset(own, 2, sizeof(own));
	for (int i = 0; i < W * H; i++) if (i % W > 700) own[i] = 3;
	for (int y = 0; y < H; y++) b->fill(scr + y * W, own + y * W, -1, 0x123456, W);   /* boot */
	for (int y = 0; y < H; y++) b->fill(scr + y * W, own + y * W, 2, 0x203040, W);    /* a window */
	for (int y = 0; y < H; y++) b->fill(scr + y * W, own + y * W, 3, 0x405060, W);    /* covered */
	extern const unsigned char font8x16[], font6x10[];
	for (int i = 0; i < 2000; i++) {
		int x = (i * 8) % (W - 8), y = (i / 130) * 16 % (H - 16);
		b->glyph(scr + y * W + x, W, own + y * W + x, W, 2, font8x16 + 16 * (32 + i % 90), 8, 16,
			 0xffffff, 0, i & 1);
		b->glyph(scr + y * W + x, W, own + y * W + x, W, 2, font6x10 + 10 * (32 + i % 90), 6, 10,
			 0xffffff, 0, 0);
	}
	for (int r = 0; r < 9; r++)
		for (int y = 0; y < 136; y++) b->from_bgr(scr + y * W, own + y * W, 2, img + y * 270 * 3, 270);
	for (int y = 0; y < H / 2; y++) b->to_bgr(img, scr + y * W, 270);
	b->rot2x(panel, 1080 * 4, scr, W, W, 0, H, 1080);
	cpy(panel, scr, sizeof(scr));
	set(own, 0, sizeof(own));
}

/* lib.c's loops, as the kernel has them with SIMD=c (cstring.c) */
void *c_memcpy(void *d, const void *s, unsigned n);
void *c_memset(void *d, int c, unsigned n);

__attribute__((noinline)) void bench_c(void) { bench(&blit_c, c_memcpy, c_memset); }
__attribute__((noinline)) void bench_neon(void) { bench(&blit_neon, k_memcpy, k_memset); }

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "bench")) {
		bench_c();
		bench_neon();
		printf("bench: done\n");
		return 0;
	}
	test_fill();
	test_glyph();
	test_bgr();
	test_rot2x();
	test_string();
	printf("%d checks, %d failed\n", checks, failed);
	return failed != 0;
}
