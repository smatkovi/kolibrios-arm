/* KolibriOS for ARM: kolibri.bench, the drawing's inner loops and the
 * string functions timed on the device, C against assembler (blit.h,
 * arch/<arch>/blit.S, string.S), before the first program starts
 * (interrupts are still masked: nothing comes between). The clock:
 * AArch64's generic timer, ARMv7's cycle counter (arch/armv7/cycles.c).
 * Each one runs kolibri.bench=N times (5), the shortest counts. The
 * buffers are the screen's size, in RAM of their own; the last line is
 * the real flush: the Pro1X's panel in device memory, the N950's
 * uncached frame buffer. The lines go to the console (on the Pro1X its
 * RAM copy, which tools/pro1x-read.py reads after the next boot); on
 * the ARMv7 kernel also onto the screen, which keeps them there for
 * kolibri.benchhold=S seconds (20; the N950 has no serial port at hand)
 * before the system starts (docs/asm.md). */
#include <stdarg.h>
#include "gui.h"
#include "blit.h"
#include "power.h"

extern const unsigned char font8x16[], font6x10[];
void *memcpy_c(void *d, const void *s, size_t n);
void *memset_c(void *d, int c, size_t n);
const char *boot_arg(const char *name);

static int reps;
static u64 freq;

struct job {
	const char *name;
	void (*run)(const struct blit_ops *b, bool fast);
	u32 bytes;                     /* written, for the MB/s */
	bool once;                     /* one way only (not C against assembler) */
};

static int W, H;
static u32 *fb, *panel;
static u8 *own, *img;
static u8 *s_src, *s_dst;

static void j_fill_all(const struct blit_ops *b, bool f)
{
	for (int y = 0; y < H; y++) b->fill(fb + y * W, own + y * W, -1, 0x123456, W);
}

static void j_fill_win(const struct blit_ops *b, bool f)
{
	for (int y = 0; y < H; y++) b->fill(fb + y * W, own + y * W, 2, 0x203040, W);
}

static void j_fill_half(const struct blit_ops *b, bool f)
{
	for (int y = 0; y < H; y++) b->fill(fb + y * W, own + y * W, 3, 0x405060, W);
}

/* a screen of text: rows of 8x16 glyphs on their background (fill),
 * then 6x10 ones without */
static void j_text16(const struct blit_ops *b, bool f)
{
	for (int y = 0; y + 16 <= H; y += 16)
		for (int x = 0; x + 8 <= W; x += 8)
			b->glyph(fb + y * W + x, W, own + y * W + x, W, 2, font8x16 + 16 * (33 + (x / 8 + y) % 90),
				 8, 16, 0xffffff, 0x102030, 1);
}

static void j_text10(const struct blit_ops *b, bool f)
{
	for (int y = 0; y + 10 <= H; y += 10)
		for (int x = 0; x + 6 <= W; x += 6)
			b->glyph(fb + y * W + x, W, own + y * W + x, W, 2, font6x10 + 10 * (33 + (x / 6 + y) % 90),
				 6, 10, 0xe0e0e0, 0, 0);
}

/* the task switcher's: nine pictures of 270 x 136 put (function 7), a
 * quarter of the screen read (function 36) */
static void j_from_bgr(const struct blit_ops *b, bool f)
{
	for (int i = 0; i < 9; i++)
		for (int y = 0; y < 136 && y < H; y++)
			b->from_bgr(fb + y * W + (i % 3) * 270, own + y * W, 2, img + y * 270 * 3, 270);
}

static void j_to_bgr(const struct blit_ops *b, bool f)
{
	for (int y = 0; y < H / 2; y++) b->to_bgr(img + (y % 136) * (W / 2) * 3, fb + y * W, W / 2);
}

#ifdef __aarch64__
static void j_rot2x(const struct blit_ops *b, bool f)
{
	b->rot2x(panel, H * 2 * 4, fb, W, W, 0, H, H * 2);
}
#else
/* the N950's turn: the screen's columns as panel lines of H pixels */
static void j_rot90_32(const struct blit_ops *b, bool f)
{
	b->rot90(panel, H * 4, fb, W, W, H, 32);
}

static void j_rot90_16(const struct blit_ops *b, bool f)
{
	b->rot90(panel, H * 2, fb, W, W, H, 16);
}
#endif

static void j_memcpy(const struct blit_ops *b, bool f)
{
	(f ? memcpy : memcpy_c)(s_dst, s_src, 2u << 20);
}

static void j_memcpy4(const struct blit_ops *b, bool f)
{
	(f ? memcpy : memcpy_c)(s_dst + 4, s_src, (2u << 20) - 4);
}

static void j_memset(const struct blit_ops *b, bool f)
{
	(f ? memset : memset_c)(own, 2, W * H);
}

/* the platform's own flush of the whole screen, with the table in use
 * (the N950 sends at most a frame every 3 ticks: past that, as the
 * panic's screen does) */
static void j_flush(const struct blit_ops *b, bool f)
{
	struct blit_ops keep = blit;
	blit = *b;
	ticks += 3;
	plat->display_flush(0, scr.h);
	blit = keep;
}

/* the N950's flush as it was (platform/n950.c), for the way the panel
 * is written: by columns, a pixel a store */
extern void n9_flush_by_columns(void) __attribute__((weak));
static void j_flush_cols(const struct blit_ops *b, bool f)
{
	n9_flush_by_columns();
}

static u64 time_of(const struct job *j, const struct blit_ops *b, bool fast)
{
	u64 best = ~0ull;
	for (int r = 0; r < reps; r++) {
		u64 t0 = timer_count();
		j->run(b, fast);
		u64 t = timer_count() - t0;
		if (t < best) best = t;
	}
	return best;
}

static u32 us(u64 t) { return (u32)(t * 1000000 / freq); }

/* a line of the result: to the console, kept for the screen */
#define LINES 24
static char lines[LINES][112];
static int nlines;
static u32 llen;

static void say(const char *fmt, ...)
{
	char *l = lines[nlines < LINES ? nlines : LINES - 1];
	va_list ap;
	va_start(ap, fmt);
	llen += kvsnprintf(l + llen, sizeof(lines[0]) - llen, fmt, ap);
	va_end(ap);
}

static void say_end(void)
{
	kprintf("%s\n", lines[nlines < LINES ? nlines : LINES - 1]);
	if (nlines < LINES) nlines++;
	llen = 0;
}

/* the lines on the screen, white on black, kept there for S seconds */
static void show(u32 s)
{
	gfx_rect(0, 0, scr.w, scr.h, 0x000000, -1);
	for (int i = 0; i < nlines; i++)
		gfx_text(8, 8 + i * 18, lines[i], -1, i == 0 || i == nlines - 1 ? 0x80e080 : 0xffffff, 1, 1,
			 false, 0, -1, false);
	gfx_dirty(0, scr.h);
	for (int i = 0; i < 1000 && plat->display_flush && !plat->display_flush(0, scr.h); i++)
		ticks += 3;
	u64 t0 = timer_count();
	while (timer_count() - t0 < (u64)s * freq)
		;
	wm_redraw_background(0, 0, scr.w, scr.h);
}

void bench_run(void)
{
	const char *v = boot_arg("kolibri.bench");
	reps = v && *v ? 0 : 5;
	for (; v && *v >= '0' && *v <= '9'; v++) reps = reps * 10 + *v - '0';
	if (reps < 1) reps = 1;
	freq = timer_freq();
	W = scr.w;
	H = scr.h;
	u32 fb_mb = ALIGN_UP((u32)W * H * 4, 1u << 20) >> 20, own_mb = ALIGN_UP((u32)W * H, 1u << 20) >> 20;
	u32 panel_mb = ALIGN_UP((u32)W * H * 16, 1u << 20) >> 20;
	addr_t a_fb = mm_alloc_mb(fb_mb), a_own = mm_alloc_mb(own_mb), a_panel = mm_alloc_mb(panel_mb);
	addr_t a_img = mm_alloc_mb(1), a_s = mm_alloc_mb(4);
	if (!a_fb || !a_own || !a_panel || !a_img || !a_s) {
		kprintf("bench: not enough memory\n");
		return;
	}
	fb = (u32 *)a_fb;
	own = (u8 *)a_own;
	panel = (u32 *)a_panel;
	img = (u8 *)a_img;
	s_src = (u8 *)a_s;
	s_dst = (u8 *)a_s + (2u << 20);
	/* a window over the screen, its right third covered by another */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) own[y * W + x] = x * 3 < W * 2 ? 2 : 3;
	for (u32 i = 0; i < 270 * 136 * 3; i++) img[i] = i * 7;
	for (u32 i = 0; i < (2u << 20); i++) s_src[i] = i;
	static const struct job jobs[] = {
		{ "fill, everywhere", j_fill_all, 0, false },
		{ "fill, a window's 2/3", j_fill_win, 0, false },
		{ "fill, the covered 1/3", j_fill_half, 0, false },
		{ "text 8x16 on its colour", j_text16, 0, false },
		{ "text 6x10, no background", j_text10, 0, false },
		{ "image 9 x 270x136 (fn 7)", j_from_bgr, 9 * 270 * 136 * 4, false },
		{ "read a quarter (fn 36)", j_to_bgr, 0, false },
#ifdef __aarch64__
		{ "turn+double to RAM", j_rot2x, 0, false },
#else
		{ "turn to RAM, 32 bpp", j_rot90_32, 0, false },
		{ "turn to RAM, 16 bpp", j_rot90_16, 0, false },
#endif
		{ "memcpy 2 MiB", j_memcpy, 2u << 20, false },
		{ "memcpy 2 MiB, d+4", j_memcpy4, 2u << 20, false },
		{ "memset the owner map", j_memset, 0, false },
		{ "the flush (display)", j_flush, 0, false },
		{ "the flush, by columns (old)", j_flush_cols, 0, true },
	};
	/* the screen's hold: the ARMv7 kernel's, kolibri.benchhold=S */
	const char *hv = boot_arg("kolibri.benchhold");
#ifdef __aarch64__
	u32 hold = 0;
#else
	u32 hold = 20;
#endif
	if (hv) for (hold = 0; *hv >= '0' && *hv <= '9'; hv++) hold = hold * 10 + *hv - '0';
	say("bench: %d x %d, %u Hz counter, best of %d; C, then assembler (NEON)", W, H, (u32)freq, reps);
	say_end();
	for (u32 i = 0; i < ARRAY_SIZE(jobs); i++) {
		const struct job *j = &jobs[i];
		u32 bytes = j->bytes;
		if (j->run == j_fill_all || j->run == j_fill_win || j->run == j_fill_half) bytes = W * H * 4;
#ifdef __aarch64__
		if (j->run == j_rot2x || j->run == j_flush) bytes = W * H * 16;
		if (j->run == j_flush && (!plat->display_flush || plat == &platform_virt)) continue;
#else
		if (j->run == j_rot90_32) bytes = W * H * 4;
		if (j->run == j_rot90_16) bytes = W * H * 2;
		/* the N950's frame buffer: its format's bytes; realview has no copy */
		if (j->run == j_flush) bytes = W * H * 4;
		if (j->run == j_flush && (!plat->display_flush || plat == &platform_realview)) continue;
#endif
		if (j->run == j_memset) bytes = W * H;
#ifdef __aarch64__
		if (j->run == j_flush_cols) continue;
#else
		if (j->run == j_flush_cols && (!n9_flush_by_columns || plat != &platform_n950)) continue;
#endif
		if (j->once) {
			u64 t = time_of(j, &blit_c, false);
			say("bench: %s: %u us", j->name, us(t));
			if (t) say(", %u MB/s", (u32)((u64)W * H * 4 * freq / t / 1000000));
			say_end();
			continue;
		}
		u64 tc = time_of(j, &blit_c, false);
		u64 tn = time_of(j, &blit_neon, true);
		u32 x10 = tn ? (u32)(tc * 10 / tn) : 0;
		static const char pad[] = "                          ";
		say("bench: %s:%s C %u us, asm %u us, %u.%ux", j->name,
		    &pad[MIN(strlen(j->name), sizeof(pad) - 1)], us(tc), us(tn), x10 / 10, x10 % 10);
		if (bytes && tn) say(", %u MB/s", (u32)((u64)bytes * freq / tn / 1000000));
		say_end();
	}
	say("bench: done");
	if (hold) say(", the screen kept for %u s (kolibri.benchhold=S)", hold);
	say_end();
	if (hold) show(hold);
	mm_free_mb(a_fb, fb_mb);
	mm_free_mb(a_own, own_mb);
	mm_free_mb(a_panel, panel_mb);
	mm_free_mb(a_img, 1);
	mm_free_mb(a_s, 4);
}
