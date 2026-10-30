/* KolibriOS for ARM, AArch64: the drawing's inner loops with NEON
 * (blit.h has what each one does; blit.c the same in C, which tests/asm
 * compares with these bit for bit). Called from C (AAPCS64): x0-x7 the
 * first arguments, the rest on the stack, 8 bytes each; v0-v7 and
 * v16-v31 free to use, v8-v15 not used (callee-saved). The u32 counts
 * arrive in w registers, whose upper halves are not defined: they are
 * extended first. Odd sizes go to the C functions (a tail call with the
 * arguments as they came).
 *
 * The kernel itself uses no FP/SIMD registers otherwise; the callers
 * make sure (blit_begin) that a Linux thread's are saved before these
 * overwrite them. */
	.arch	armv8-a+simd
	.text

/* void fill_neon(u32 *p, const u8 *own, int slot, u32 c, u32 n)
 *
 * 16 pixels a step: their 16 owner bytes compared at once; all of them
 * the slot's (a window's own area, the common case): four stores; none:
 * nothing; else the old pixels kept where the mask is clear (bit). */
	.global	fill_neon
	.type	fill_neon, %function
	.balign	64
fill_neon:
	mov	w4, w4
	dup	v0.4s, w3
	tbnz	w2, #31, .Lfa			/* slot < 0: everywhere */
	dup	v1.16b, w2
	cmp	x4, #16
	b.lo	.Lfo4
	/* to a 16-byte boundary of p (at most 3 pixels) */
.Lfoh:	tst	x0, #15
	b.eq	.Lfo16
	ldrb	w5, [x1], #1
	cmp	w5, w2
	b.ne	1f
	str	w3, [x0]
1:	add	x0, x0, #4
	sub	x4, x4, #1
	b	.Lfoh
.Lfo16:	cmp	x4, #16
	b.lo	.Lfo4
2:	ldr	q2, [x1], #16
	cmeq	v2.16b, v2.16b, v1.16b
	fmov	x5, d2
	mov	x6, v2.d[1]
	and	x7, x5, x6
	cmn	x7, #1
	b.eq	3f				/* all 16 owned */
	orr	x7, x5, x6
	cbz	x7, 4f				/* none */
	ld1	{v4.4s, v5.4s, v6.4s, v7.4s}, [x0]
	sxtl	v16.8h, v2.8b
	sxtl2	v17.8h, v2.16b
	sxtl	v18.4s, v16.4h
	sxtl2	v19.4s, v16.8h
	sxtl	v20.4s, v17.4h
	sxtl2	v21.4s, v17.8h
	bit	v4.16b, v0.16b, v18.16b
	bit	v5.16b, v0.16b, v19.16b
	bit	v6.16b, v0.16b, v20.16b
	bit	v7.16b, v0.16b, v21.16b
	st1	{v4.4s, v5.4s, v6.4s, v7.4s}, [x0]
	b	4f
3:	stp	q0, q0, [x0]
	stp	q0, q0, [x0, #32]
4:	add	x0, x0, #64
	sub	x4, x4, #16
	cmp	x4, #16
	b.hs	2b
	/* 4 pixels a step */
.Lfo4:	cmp	x4, #4
	b.lo	.Lfo1
	ldr	s2, [x1], #4
	cmeq	v2.8b, v2.8b, v1.8b
	fmov	w5, s2
	cbz	w5, 5f
	ldr	q4, [x0]
	sxtl	v16.8h, v2.8b
	sxtl	v18.4s, v16.4h
	bit	v4.16b, v0.16b, v18.16b
	str	q4, [x0]
5:	add	x0, x0, #16
	sub	x4, x4, #4
	b	.Lfo4
.Lfo1:	cbz	x4, 7f
6:	ldrb	w5, [x1], #1
	cmp	w5, w2
	b.ne	8f
	str	w3, [x0]
8:	add	x0, x0, #4
	subs	x4, x4, #1
	b.ne	6b
7:	ret

	/* everywhere: aligned to 16 bytes, then 64 bytes a step */
.Lfa:	cmp	x4, #16
	b.lo	.Lfa4
1:	tst	x0, #15
	b.eq	2f
	str	w3, [x0], #4
	sub	x4, x4, #1
	b	1b
2:	cmp	x4, #16
	b.lo	.Lfa4
3:	stp	q0, q0, [x0]
	stp	q0, q0, [x0, #32]
	add	x0, x0, #64
	sub	x4, x4, #16
	cmp	x4, #16
	b.hs	3b
.Lfa4:	cmp	x4, #4
	b.lo	.Lfa1
	str	q0, [x0], #16
	sub	x4, x4, #4
	b	.Lfa4
.Lfa1:	cbz	x4, 5f
4:	str	w3, [x0], #4
	subs	x4, x4, #1
	b.ne	4b
5:	ret
	.size	fill_neon, . - fill_neon

/* void glyph_neon(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot,
 *		   const u8 *rows, u32 fw, u32 fh, u32 fg, u32 bg, int fill)
 *
 * A row of the glyph a step: its bits spread to 8 byte lanes (cmtst
 * against 0x80, 0x40, ...), anded with the owner bytes' comparison and
 * the glyph's width, widened to word masks; the row's pixels loaded,
 * the colours put in where the mask is set, stored. The fonts' widths,
 * 8 and 6, here (6: a 16-byte and an 8-byte access, nothing beside the
 * glyph touched); others in C. fg, bg, fill: on the stack. */
	.global	glyph_neon
	.type	glyph_neon, %function
	.balign	64
glyph_neon:
	cmp	w6, #8
	b.eq	1f
	cmp	w6, #6
	b.ne	glyph_c
1:	ldr	w9, [sp]			/* fg */
	ldr	w10, [sp, #8]			/* bg */
	ldr	w11, [sp, #16]			/* fill */
	mov	w1, w1
	mov	w3, w3
	mov	w7, w7
	lsl	x1, x1, #2			/* the screen's stride in bytes */
	dup	v24.4s, w9
	dup	v25.4s, w10
	adr	x12, .Lbits
	ldr	d26, [x12]			/* 0x80, 0x40, ... 0x01 */
	ldr	d27, [x12, #8]			/* the width's columns: 8 or 6 */
	cmp	w6, #8
	b.eq	2f
	ldr	d27, [x12, #16]
2:	dup	v28.8b, w4			/* the slot */
	cbz	w7, 9f
.Lgrow:	ldrb	w13, [x5], #1
	dup	v1.8b, w13
	cmtst	v1.8b, v1.8b, v26.8b		/* the bits set */
	and	v2.8b, v1.8b, v27.8b		/* written: the bits ... */
	cbz	w11, 3f
	mov	v2.8b, v27.8b			/* ... or with fill all the width's */
3:	tbnz	w4, #31, 4f
	cmp	w6, #8
	b.ne	5f
	ldr	d3, [x2]
	b	6f
5:	ldr	s3, [x2]
	add	x14, x2, #4
	ld1	{v3.h}[2], [x14]
6:	cmeq	v3.8b, v3.8b, v28.8b
	and	v2.8b, v2.8b, v3.8b		/* only where the slot owns it */
4:	fmov	x13, d2
	cbz	x13, 8f				/* nothing to write in this row */
	sxtl	v4.8h, v1.8b
	sxtl	v16.4s, v4.4h
	sxtl2	v17.4s, v4.8h
	mov	v18.16b, v25.16b
	bit	v18.16b, v24.16b, v16.16b	/* the colours: fg on, bg off */
	mov	v19.16b, v25.16b
	bit	v19.16b, v24.16b, v17.16b
	sxtl	v5.8h, v2.8b
	sxtl	v20.4s, v5.4h
	sxtl2	v21.4s, v5.8h
	ldr	q22, [x0]
	bit	v22.16b, v18.16b, v20.16b
	str	q22, [x0]
	cmp	w6, #8
	b.ne	7f
	ldr	q23, [x0, #16]
	bit	v23.16b, v19.16b, v21.16b
	str	q23, [x0, #16]
	b	8f
7:	ldr	d23, [x0, #16]
	bit	v23.8b, v19.8b, v21.8b
	str	d23, [x0, #16]
8:	add	x0, x0, x1
	add	x2, x2, x3
	subs	w7, w7, #1
	b.ne	.Lgrow
9:	ret
	.balign	8
.Lbits:	.byte	0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
	.byte	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	.byte	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00
	.size	glyph_neon, . - glyph_neon

/* void from_bgr_neon(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n)
 *
 * 16 pixels a step: ld3 takes the blue, green and red bytes apart, two
 * rounds of zip put them together as words with a zero byte on top; the
 * owner mask as in fill_neon. The rest (n % 16) in C. */
	.global	from_bgr_neon
	.type	from_bgr_neon, %function
	.balign	64
from_bgr_neon:
	mov	w4, w4
	dup	v30.16b, w2
	movi	v31.16b, #0
1:	cmp	x4, #16
	b.lo	from_bgr_c
	ld3	{v0.16b, v1.16b, v2.16b}, [x3], #48
	zip1	v4.16b, v0.16b, v1.16b		/* B G, pixels 0-7 */
	zip2	v5.16b, v0.16b, v1.16b		/* 8-15 */
	zip1	v6.16b, v2.16b, v31.16b		/* R 0 */
	zip2	v7.16b, v2.16b, v31.16b
	zip1	v16.8h, v4.8h, v6.8h		/* B G R 0: pixels 0-3 */
	zip2	v17.8h, v4.8h, v6.8h		/* 4-7 */
	zip1	v18.8h, v5.8h, v7.8h		/* 8-11 */
	zip2	v19.8h, v5.8h, v7.8h		/* 12-15 */
	tbnz	w2, #31, 3f
	ldr	q3, [x1]
	cmeq	v3.16b, v3.16b, v30.16b
	fmov	x5, d3
	mov	x6, v3.d[1]
	and	x7, x5, x6
	cmn	x7, #1
	b.eq	3f				/* all 16 owned */
	orr	x7, x5, x6
	cbz	x7, 4f				/* none */
	ld1	{v20.4s, v21.4s, v22.4s, v23.4s}, [x0]
	sxtl	v24.8h, v3.8b
	sxtl2	v25.8h, v3.16b
	sxtl	v26.4s, v24.4h
	sxtl2	v27.4s, v24.8h
	sxtl	v28.4s, v25.4h
	sxtl2	v29.4s, v25.8h
	bif	v16.16b, v20.16b, v26.16b
	bif	v17.16b, v21.16b, v27.16b
	bif	v18.16b, v22.16b, v28.16b
	bif	v19.16b, v23.16b, v29.16b
3:	st1	{v16.4s, v17.4s, v18.4s, v19.4s}, [x0]
4:	add	x0, x0, #64
	add	x1, x1, #16
	sub	x4, x4, #16
	b	1b
	.size	from_bgr_neon, . - from_bgr_neon

/* void to_bgr_neon(u8 *bgr, const u32 *p, u32 n)
 *
 * 16 pixels a step: ld4 takes them apart into blue, green, red and the
 * top byte, st3 writes the first three back interleaved. */
	.global	to_bgr_neon
	.type	to_bgr_neon, %function
	.balign	64
to_bgr_neon:
	mov	w2, w2
1:	cmp	x2, #16
	b.lo	to_bgr_c
	ld4	{v0.16b, v1.16b, v2.16b, v3.16b}, [x1], #64
	st3	{v0.16b, v1.16b, v2.16b}, [x0], #48
	sub	x2, x2, #16
	b	1b
	.size	to_bgr_neon, . - to_bgr_neon

/* void rot2x_neon(void *dst, u32 dst_stride, const u32 *src, u32 src_stride,
 *		   u32 w, u32 y0, u32 y1, u32 panel_w)
 *
 * Four columns of the screen (a 16-byte load from each line) by four
 * lines at a time: transposed (trn1/trn2 of words, then of doublewords),
 * so that each column's four pixels lie in one register, bottom line
 * first, as the panel's line has them; alpha set, each pixel doubled
 * (zip with itself) and written to the column's two panel lines as 32
 * bytes. Eight panel lines advance together, 32 bytes a step, on
 * 8-byte elements (st1 .2d: the panel is device memory, only 8-byte
 * aligned where it starts). Lines left over (n % 4) one at a time, the
 * columns left over (w % 4) in C. */
	.global	rot2x_neon
	.type	rot2x_neon, %function
	.balign	64
rot2x_neon:
	mov	w1, w1
	mov	w3, w3
	mov	w4, w4
	mov	w5, w5
	mov	w6, w6
	mov	w7, w7
	sub	x8, x6, x5			/* n: the lines */
	cmp	x8, #1080
	b.hi	rot2x_c				/* more than the C version takes */
	stp	x19, x20, [sp, #-64]!
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	sub	x9, x7, x6, lsl #1		/* the panel's first pixel: panel_w - 2 y1 */
	lsl	x9, x9, #2
	add	x0, x0, x9			/* dst at px0, line 0 */
	lsl	x3, x3, #2			/* the screen's stride in bytes */
	sub	x10, x6, #1
	mul	x10, x10, x3
	add	x2, x2, x10			/* src at line y1 - 1 */
	mov	w10, #0xff000000
	dup	v31.4s, w10
	mov	x10, #0				/* lx */
.Lrcol:	add	x11, x10, #4
	cmp	x11, x4
	b.hi	.Lrtail
	/* the eight panel lines of columns lx .. lx + 3 */
	lsl	x11, x10, #1
	mul	x11, x11, x1
	add	x12, x0, x11
	add	x13, x12, x1
	add	x14, x13, x1
	add	x15, x14, x1
	add	x16, x15, x1
	add	x17, x16, x1
	add	x19, x17, x1
	add	x20, x19, x1
	add	x21, x2, x10, lsl #2		/* the screen at (lx, y1 - 1) */
	mov	x22, x8
.Lr4:	cmp	x22, #4
	b.lo	.Lr1
	ldr	q0, [x21]
	sub	x23, x21, x3
	ldr	q1, [x23]
	sub	x23, x23, x3
	ldr	q2, [x23]
	sub	x23, x23, x3
	ldr	q3, [x23]
	sub	x21, x23, x3
	trn1	v4.4s, v0.4s, v1.4s
	trn2	v5.4s, v0.4s, v1.4s
	trn1	v6.4s, v2.4s, v3.4s
	trn2	v7.4s, v2.4s, v3.4s
	trn1	v16.2d, v4.2d, v6.2d		/* column lx: lines y, y-1, y-2, y-3 */
	trn1	v17.2d, v5.2d, v7.2d		/* lx + 1 */
	trn2	v18.2d, v4.2d, v6.2d		/* lx + 2 */
	trn2	v19.2d, v5.2d, v7.2d		/* lx + 3 */
	orr	v16.16b, v16.16b, v31.16b
	orr	v17.16b, v17.16b, v31.16b
	orr	v18.16b, v18.16b, v31.16b
	orr	v19.16b, v19.16b, v31.16b
	zip1	v20.4s, v16.4s, v16.4s
	zip2	v21.4s, v16.4s, v16.4s
	zip1	v22.4s, v17.4s, v17.4s
	zip2	v23.4s, v17.4s, v17.4s
	zip1	v24.4s, v18.4s, v18.4s
	zip2	v25.4s, v18.4s, v18.4s
	zip1	v26.4s, v19.4s, v19.4s
	zip2	v27.4s, v19.4s, v19.4s
	st1	{v20.2d, v21.2d}, [x12], #32
	st1	{v20.2d, v21.2d}, [x13], #32
	st1	{v22.2d, v23.2d}, [x14], #32
	st1	{v22.2d, v23.2d}, [x15], #32
	st1	{v24.2d, v25.2d}, [x16], #32
	st1	{v24.2d, v25.2d}, [x17], #32
	st1	{v26.2d, v27.2d}, [x19], #32
	st1	{v26.2d, v27.2d}, [x20], #32
	sub	x22, x22, #4
	b	.Lr4
	/* a line at a time: four pixels, each doubled into 8 bytes */
.Lr1:	cbz	x22, .Lrnext
	ldr	q0, [x21]
	sub	x21, x21, x3
	orr	v0.16b, v0.16b, v31.16b
	zip1	v1.4s, v0.4s, v0.4s		/* lx, lx + 1 doubled */
	zip2	v2.4s, v0.4s, v0.4s		/* lx + 2, lx + 3 */
	st1	{v1.d}[0], [x12], #8
	st1	{v1.d}[0], [x13], #8
	st1	{v1.d}[1], [x14], #8
	st1	{v1.d}[1], [x15], #8
	st1	{v2.d}[0], [x16], #8
	st1	{v2.d}[0], [x17], #8
	st1	{v2.d}[1], [x19], #8
	st1	{v2.d}[1], [x20], #8
	sub	x22, x22, #1
	b	.Lr1
.Lrnext:
	add	x10, x10, #4
	b	.Lrcol
	/* the columns left over: rot2x_c(dst + 2 lx stride, stride,
	 * src + lx, src_stride, w - lx, y0, y1, panel_w) from the arguments */
.Lrtail:
	cmp	x10, x4
	b.hs	.Lrdone
	sub	x0, x0, x9			/* back to the arguments' dst */
	lsl	x11, x10, #1
	madd	x0, x11, x1, x0
	mov	x11, x6
	sub	x11, x11, #1
	mul	x11, x11, x3
	sub	x2, x2, x11			/* the arguments' src */
	add	x2, x2, x10, lsl #2
	lsr	x3, x3, #2
	sub	x4, x4, x10
	ldp	x25, x26, [sp, #48]
	ldp	x23, x24, [sp, #32]
	ldp	x21, x22, [sp, #16]
	ldp	x19, x20, [sp], #64
	b	rot2x_c
.Lrdone:
	ldp	x25, x26, [sp, #48]
	ldp	x23, x24, [sp, #32]
	ldp	x21, x22, [sp, #16]
	ldp	x19, x20, [sp], #64
	ret
	.size	rot2x_neon, . - rot2x_neon
