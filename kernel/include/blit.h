/* KolibriOS for ARM: the drawing's inner loops, in C (blit.c) and on
 * AArch64 in assembler with NEON (arch/arm64/blit.S), the same results
 * bit for bit (tests/asm). gfx.c clips, takes the mouse pointer away and
 * marks the dirty lines; these only run along what is left: no bounds
 * checks, nothing global. A slot below 0 draws everywhere (the owner map
 * is not read), else only where own[i] == slot. Pixels are 0x00RRGGBB,
 * counts in pixels unless a name says bytes.
 *
 * Which ones run is chosen at the start (blit_init): the assembler's on
 * AArch64 unless built with SIMD=c or started with kolibri.simd=c. */
#ifndef BLIT_H
#define BLIT_H

#ifdef BLIT_HOST
#include <stdint.h>
typedef uint8_t u8;
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
};

extern const struct blit_ops blit_c;
#if defined(__aarch64__) && !defined(SIMD_C)
extern const struct blit_ops blit_neon;
#endif
extern struct blit_ops blit;             /* the ones in use */

void blit_init(void);                    /* the choice, after mmu_init */

/* before the kernel's own use of the FP/SIMD registers: a Linux
 * thread's, if the CPU still holds them, saved (kernel/linux/lx.c keeps
 * them in the registers until another Linux thread runs) */
#if defined(__aarch64__) && !defined(BLIT_HOST)
struct lx_thread;
extern struct lx_thread *lx_fp_owner;
void lx_fp_release(void);
static inline void blit_begin(void)
{
	if (lx_fp_owner) lx_fp_release();
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
/* arch/arm64/blit.S */
void fill_neon(u32 *p, const u8 *own, int slot, u32 c, u32 n);
void glyph_neon(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot, const u8 *rows,
		u32 fw, u32 fh, u32 fg, u32 bg, int fill);
void from_bgr_neon(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n);
void to_bgr_neon(u8 *bgr, const u32 *p, u32 n);
void rot2x_neon(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w,
		u32 y0, u32 y1, u32 panel_w);

#endif
