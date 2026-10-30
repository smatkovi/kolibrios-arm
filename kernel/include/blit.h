/* KolibriOS for ARM: the drawing's inner loops, in C (blit.c) and in
 * assembler with NEON (arch/arm64/blit.S, arch/armv7/blit.S), the same
 * results bit for bit (tests/asm). gfx.c clips, takes the mouse pointer away and
 * marks the dirty lines; these only run along what is left: no bounds
 * checks, nothing global. A slot below 0 draws everywhere (the owner map
 * is not read), else only where own[i] == slot. Pixels are 0x00RRGGBB,
 * counts in pixels unless a name says bytes.
 *
 * Which ones run is chosen at the start (blit_init): the assembler's
 * unless built with SIMD=c or started with kolibri.simd=c. */
#ifndef BLIT_H
#define BLIT_H

#ifdef BLIT_HOST
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include "kernel.h"
#endif

struct blit_ops {
	const char *name;
	/* n pixels of color c (only where own[i] == slot, slot >= 0) */
	void (*fill)(u32 *p, const u8 *own, int slot, u32 c, u32 n);
	/* a glyph of fw (1..8) x fh pixels, unclipped: row r's bits in
	 * rows[r], the leftmost pixel in bit 7; fg where a bit is set, bg
	 * where it is not if fill, else nothing; stride and ostride in
	 * pixels (the screen's and the owner map's lines) */
	void (*glyph)(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot, const u8 *rows,
		      u32 fw, u32 fh, u32 fg, u32 bg, int fill);
	/* a line of an image of 3 bytes a pixel (B, G, R: functions 7, 36)
	 * onto the screen, and the screen's into such a line */
	void (*from_bgr)(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n);
	void (*to_bgr)(u8 *bgr, const u32 *p, u32 n);
	/* the Pro1X's flush: lines y0..y1 (y1 - y0 <= panel_w / 2) of a
	 * landscape screen of w x ? pixels (src, its lines src_stride
	 * pixels apart) to a portrait panel of panel_w pixels a line
	 * (dst, its lines dst_stride bytes apart), turned clockwise, each
	 * pixel doubled both ways, alpha opaque: the screen's (X, Y) is the
	 * panel's (panel_w - 2 - 2 Y, 2 X) and its three neighbours. The
	 * panel is device memory: aligned stores of at most 8-byte
	 * elements. */
	void (*rot2x)(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w,
		      u32 y0, u32 y1, u32 panel_w);
	/* the N950's flush: n lines of a landscape screen (src, its lines
	 * src_stride pixels apart, w pixels each) to a portrait panel,
	 * turned: the screen's line i, pixel x is the panel's line x (dst +
	 * x dst_stride bytes), pixel n - 1 - i (the band's bottom line
	 * first). bpp 32: the pixel as it is; 16: RGB565. The panel is
	 * uncached: aligned stores (the assembler's 16 lines and 4 columns
	 * at a time, 64 or 32 bytes to each panel line, from a dst and
	 * dst_stride 16-byte aligned; else C) */
	void (*rot90)(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w, u32 n,
		      u32 bpp);
};

extern const struct blit_ops blit_c;
#ifndef SIMD_C
extern const struct blit_ops blit_neon;
#endif
extern struct blit_ops blit;             /* the ones in use */

void blit_init(void);                    /* the choice, after mmu_init */

/* before the kernel's own use of the FP/SIMD registers: a Linux
 * thread's or a program's, if the CPU still holds them, saved (lx.c and
 * arch/<arch>/fpu.c keep them in the registers until someone else needs
 * them) */
#if !defined(__aarch64__) && !defined(BLIT_HOST) && !defined(SIMD_C)
struct task;
extern struct task *fp_owner;            /* arch/armv7/fpu.c */
void fp_release(void);
static inline void blit_begin(void)
{
	if (fp_owner) fp_release();
}
#elif defined(__aarch64__) && !defined(BLIT_HOST) && !defined(SIMD_C)
struct lx_thread;
struct task;
extern struct lx_thread *lx_fp_owner;
extern struct task *a32_fp_owner;        /* a program's (arch/arm64/fpu.c) */
void lx_fp_release(void);
void fp_release_all(void);
static inline void blit_begin(void)
{
	if (lx_fp_owner || a32_fp_owner) fp_release_all();
}
#else
static inline void blit_begin(void) {}
#endif

/* blit.c's, also the fallbacks the assembler's call for odd sizes */
void fill_c(u32 *p, const u8 *own, int slot, u32 c, u32 n);
void glyph_c(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot, const u8 *rows,
	     u32 fw, u32 fh, u32 fg, u32 bg, int fill);
void from_bgr_c(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n);
void to_bgr_c(u8 *bgr, const u32 *p, u32 n);
void rot2x_c(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w,
	     u32 y0, u32 y1, u32 panel_w);
void rot90_c(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w, u32 n, u32 bpp);
/* arch/<arch>/blit.S (rot2x_neon AArch64's, rot90_neon ARMv7's) */
void fill_neon(u32 *p, const u8 *own, int slot, u32 c, u32 n);
void glyph_neon(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot, const u8 *rows,
		u32 fw, u32 fh, u32 fg, u32 bg, int fill);
void from_bgr_neon(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n);
void to_bgr_neon(u8 *bgr, const u32 *p, u32 n);
void rot2x_neon(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w,
		u32 y0, u32 y1, u32 panel_w);
void rot90_neon(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w, u32 n, u32 bpp);

#endif
