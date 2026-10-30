/* KolibriOS for ARM: the drawing's inner loops in C (blit.h): the
 * kernel's with SIMD=c or kolibri.simd=c, and the reference of
 * arch/arm64/blit.S's and arch/armv7/blit.S's (tests/asm). */
#include "blit.h"

void fill_c(u32 *p, const u8 *own, int slot, u32 c, u32 n)
{
	if (slot < 0)
		for (u32 i = 0; i < n; i++) p[i] = c;
	else
		for (u32 i = 0; i < n; i++)
			if (own[i] == slot) p[i] = c;
}

void glyph_c(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot, const u8 *rows,
	     u32 fw, u32 fh, u32 fg, u32 bg, int fill)
{
	for (u32 r = 0; r < fh; r++, p += stride, own += ostride) {
		u8 bits = rows[r];
		for (u32 x = 0; x < fw; x++) {
			int on = bits & (0x80 >> x);
			if (!on && !fill) continue;
			if (slot >= 0 && own[x] != slot) continue;
			p[x] = on ? fg : bg;
		}
	}
}

void from_bgr_c(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n)
{
	for (u32 i = 0; i < n; i++, bgr += 3)
		if (slot < 0 || own[i] == slot)
			p[i] = bgr[0] | bgr[1] << 8 | (u32)bgr[2] << 16;
}

void to_bgr_c(u8 *bgr, const u32 *p, u32 n)
{
	for (u32 i = 0; i < n; i++, bgr += 3) {
		u32 c = p[i];
		bgr[0] = c;
		bgr[1] = c >> 8;
		bgr[2] = c >> 16;
	}
}

/* landscape column by column: a column of the screen is a pair of the
 * panel's lines; two pixels in each 64-bit store */
void rot2x_c(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w,
	     u32 y0, u32 y1, u32 panel_w)
{
	static unsigned long long span[1080];
	u32 n = y1 - y0;
	if (n > sizeof(span) / sizeof(span[0])) n = sizeof(span) / sizeof(span[0]);
	unsigned long px0 = (unsigned long)(panel_w - 2 * y1) * 4;
	for (u32 lx = 0; lx < w; lx++) {
		const u32 *s = src + lx;
		for (u32 k = 0; k < n; k++) {
			unsigned long long v = s[(unsigned long)(y1 - 1 - k) * src_stride] | 0xff000000u;
			span[k] = v | v << 32;
		}
		for (u32 r = 0; r < 2; r++) {
			volatile unsigned long long *d =
				(volatile unsigned long long *)((char *)dst + (unsigned long)(2 * lx + r) * dst_stride + px0);
			for (u32 k = 0; k < n; k++) d[k] = span[k];
		}
	}
}

/* panel line by panel line: a column of the screen, bottom up; each
 * pixel a store of its own size */
void rot90_c(void *dst, u32 dst_stride, const u32 *src, u32 src_stride, u32 w, u32 n, u32 bpp)
{
	if (!n) return;
	for (u32 x = 0; x < w; x++) {
		const u32 *s = src + (unsigned long)(n - 1) * src_stride + x;
		if (bpp == 32) {
			volatile u32 *d = (volatile u32 *)((char *)dst + (unsigned long)x * dst_stride);
			for (u32 k = 0; k < n; k++, s -= src_stride) d[k] = *s;
		} else {
			volatile u16 *d = (volatile u16 *)((char *)dst + (unsigned long)x * dst_stride);
			for (u32 k = 0; k < n; k++, s -= src_stride) {
				u32 c = *s;
				d[k] = ((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 0x1f);
			}
		}
	}
}

const struct blit_ops blit_c = {
	"C", fill_c, glyph_c, from_bgr_c, to_bgr_c, rot2x_c, rot90_c,
};

/* the assembler's, each architecture its own turn; the other's in C */
#if defined(__aarch64__) && !defined(SIMD_C)
const struct blit_ops blit_neon = {
	"NEON", fill_neon, glyph_neon, from_bgr_neon, to_bgr_neon, rot2x_neon, rot90_c,
};
#elif defined(__arm__) && !defined(SIMD_C)
const struct blit_ops blit_neon = {
	"NEON", fill_neon, glyph_neon, from_bgr_neon, to_bgr_neon, rot2x_c, rot90_neon,
};
#endif

#ifndef BLIT_HOST
struct blit_ops blit;
const char *boot_arg(const char *name);

/* kolibri.simd=c: the value runs to the next space */
static bool simd_c(void)
{
	const char *v = boot_arg("kolibri.simd");
	return v && v[0] == 'c' && (!v[1] || v[1] == ' ');
}

/* the assembler's unless the build or the command line says C (or, on
 * ARMv7, the CPU has no NEON: arch/armv7/fpu.c) */
void blit_init(void)
{
	blit = blit_c;
#if defined(__aarch64__) && !defined(SIMD_C)
	/* the FP/SIMD registers not trapped at EL1 (lx_init does it again
	 * for EL0) */
	SYSREG_W(cpacr_el1, SYSREG_R(cpacr_el1) | 3ul << 20);
	asm volatile("isb");
	if (!simd_c()) blit = blit_neon;
#elif !defined(SIMD_C)
	extern bool fp_available(void);
	if (fp_available() && !simd_c()) blit = blit_neon;
#endif
	kprintf("drawing: %s\n", blit.name);
}
#endif
