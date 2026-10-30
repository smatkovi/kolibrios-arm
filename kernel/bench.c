/* KolibriOS for ARM, AArch64: kolibri.bench, the drawing's inner loops
 * and the string functions timed on the device, C against assembler
 * (blit.h, arch/arm64/blit.S, string.S), with the generic timer, before
 * the first program starts (interrupts are still masked: nothing comes
 * between). Each one runs kolibri.bench=N times (5), the shortest
 * counts. The buffers are the screen's size, in RAM of their own; the
 * last line is the real flush on the Pro1X: the panel's device memory.
 * The lines go to the console (on the Pro1X its RAM copy, which
 * tools/pro1x-read.py reads after the next boot; docs/asm.md). */
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

static void j_rot2x(const struct blit_ops *b, bool f)
{
	b->rot2x(panel, H * 2 * 4, fb, W, W, 0, H, H * 2);
}

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

/* the platform's own flush of the whole screen, with the table in use */
static void j_flush(const struct blit_ops *b, bool f)
{
	struct blit_ops keep = blit;
	blit = *b;
	plat->display_flush(0, scr.h);
	blit = keep;
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
		{ "fill, everywhere", j_fill_all, 0 },
		{ "fill, a window's 2/3", j_fill_win, 0 },
		{ "fill, the covered 1/3", j_fill_half, 0 },
		{ "text 8x16 on its colour", j_text16, 0 },
		{ "text 6x10, no background", j_text10, 0 },
		{ "image 9 x 270x136 (fn 7)", j_from_bgr, 9 * 270 * 136 * 4 },
		{ "read a quarter (fn 36)", j_to_bgr, 0 },
		{ "turn+double to RAM", j_rot2x, 0 },
		{ "memcpy 2 MiB", j_memcpy, 2u << 20 },
		{ "memcpy 2 MiB, d+4", j_memcpy4, 2u << 20 },
		{ "memset the owner map", j_memset, 0 },
		{ "the flush (display)", j_flush, 0 },
	};
	kprintf("bench: %d x %d, %u Hz counter, best of %d; C, then assembler (NEON)\n", W, H,
		(u32)freq, reps);
	for (u32 i = 0; i < ARRAY_SIZE(jobs); i++) {
		const struct job *j = &jobs[i];
		u32 bytes = j->bytes;
		if (j->run == j_fill_all || j->run == j_fill_win || j->run == j_fill_half) bytes = W * H * 4;
		if (j->run == j_rot2x || j->run == j_flush) bytes = W * H * 16;
		if (j->run == j_memset) bytes = W * H;
		if (j->run == j_flush && (!plat->display_flush || plat == &platform_virt)) continue;
		u64 tc = time_of(j, &blit_c, false);
		u64 tn = time_of(j, &blit_neon, true);
		u32 x10 = tn ? (u32)(tc * 10 / tn) : 0;
		static const char pad[] = "                          ";
		kprintf("bench: %s:%s C %u us, asm %u us, %u.%ux", j->name,
			&pad[MIN(strlen(j->name), sizeof(pad) - 1)], us(tc), us(tn), x10 / 10, x10 % 10);
		if (bytes && tn) kprintf(", %u MB/s", (u32)((u64)bytes * freq / tn / 1000000));
		kprintf("\n");
	}
	kprintf("bench: done\n");
	mm_free_mb(a_fb, fb_mb);
	mm_free_mb(a_own, own_mb);
	mm_free_mb(a_panel, panel_mb);
	mm_free_mb(a_img, 1);
	mm_free_mb(a_s, 4);
}
