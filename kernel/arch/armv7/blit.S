/* KolibriOS for ARM, ARMv7 (Cortex-A8: the N950, QEMU's realview): the
 * drawing's inner loops with NEON (blit.h has what each one does;
 * blit.c the same in C, which tests/asm compares with these bit for bit).
 * Called from C (AAPCS): r0-r3 the first arguments, the rest on the
 * stack; r4-r11 kept. The kernel and its callers are soft-float, nobody
 * keeps values in d8-d15: all of q0-q15 are free here; a program's
 * registers are saved before (blit.h's blit_begin, arch/armv7/fpu.c).
 * Odd sizes go to the C functions.
 *
 * For the Cortex-A8: its NEON unit runs behind the ARM pipeline, in
 * order; a NEON to ARM register transfer waits for it (some 20 cycles),
 * so the decisions (all of a span the window's? none?) are taken on the
 * ARM side, from the owner bytes loaded into general registers. Stores
 * are aligned where the screen allows (the pixels to a 16-byte boundary
 * first: the N950's shadow lines are 3416 bytes apart, 8-byte aligned);
 * alignment hints only where the address is known to keep them (a false
 * one faults). pld ahead of the loads. */
	.syntax	unified
	.arm
	.fpu	neon
	.text

/* void fill_neon(u32 *p, const u8 *own, int slot, u32 c, u32 n)
 *
 * 16 pixels a step: their 16 owner bytes in r4-r7, compared with the
 * slot's in each byte (r8); all of them the slot's (a window's own area,
 * the common case): 64 bytes stored; none: nothing; else the bytes made
 * masks in NEON and the old pixels kept where they are clear (vbit). */
	.global	fill_neon
	.type	fill_neon, %function
	.balign	64
fill_neon:
	push	{r4-r11, lr}
	ldr	lr, [sp, #36]			/* n */
	vdup.32	q0, r3
	vmov	q1, q0
	cmp	r2, #0
	blt	.Lfa				/* slot < 0: everywhere */
	and	r8, r2, #0xff
	orr	r8, r8, r8, lsl #8
	orr	r8, r8, r8, lsl #16		/* the slot in each byte */
	movw	r10, #0x0101
	movt	r10, #0x0101
	lsl	r11, r10, #7			/* 0x80808080 */
	/* to a 16-byte boundary of p (at most 3 pixels) */
.Lfoh:	cmp	lr, #0
	beq	.Lfdone
	tst	r0, #15
	beq	.Lfo16
	ldrb	r12, [r1], #1
	cmp	r12, r2
	streq	r3, [r0]
	add	r0, r0, #4
	sub	lr, lr, #1
	b	.Lfoh
.Lfo16:	subs	lr, lr, #16
	blo	.Lfo4p
.Lfo16l:
	pld	[r1, #64]
	ldr	r4, [r1]
	ldr	r5, [r1, #4]
	ldr	r6, [r1, #8]
	ldr	r7, [r1, #12]
	add	r1, r1, #16
	cmp	r4, r8
	cmpeq	r5, r8
	cmpeq	r6, r8
	cmpeq	r7, r8
	bne	.Lfomix
	vst1.32	{d0-d3}, [r0 :128]!		/* all 16 owned */
	vst1.32	{d0-d3}, [r0 :128]!
	subs	lr, lr, #16
	bhs	.Lfo16l
	b	.Lfo4p
	/* zero bytes where owned; none: no zero byte in any word,
	 * (x - 0x01010101) & ~x & 0x80808080 == 0 for each */
.Lfomix:
	eor	r4, r4, r8
	eor	r5, r5, r8
	eor	r6, r6, r8
	eor	r7, r7, r8
	sub	r9, r4, r10
	bic	r9, r9, r4
	sub	r12, r5, r10
	bic	r12, r12, r5
	orr	r9, r9, r12
	sub	r12, r6, r10
	bic	r12, r12, r6
	orr	r9, r9, r12
	sub	r12, r7, r10
	bic	r12, r12, r7
	orr	r9, r9, r12
	tst	r9, r11
	addeq	r0, r0, #64			/* none */
	beq	.Lfonext
	vmov	d4, r4, r5
	vmov	d5, r6, r7
	vceq.i8	q2, q2, #0			/* 0xff where the slot owns it */
	vmovl.s8 q3, d4
	vmovl.s8 q8, d5
	vmovl.s16 q9, d6
	vmovl.s16 q10, d7
	vmovl.s16 q11, d16
	vmovl.s16 q12, d17
	add	r12, r0, #32
	vld1.32	{d8-d11}, [r0 :128]
	vld1.32	{d12-d15}, [r12 :128]
	vbit	q4, q0, q9
	vbit	q5, q0, q10
	vbit	q6, q0, q11
	vbit	q7, q0, q12
	vst1.32	{d8-d11}, [r0 :128]!
	vst1.32	{d12-d15}, [r0 :128]!
.Lfonext:
	subs	lr, lr, #16
	bhs	.Lfo16l
.Lfo4p:	add	lr, lr, #16
	/* 4 pixels a step (p stays 16-byte aligned) */
.Lfo4:	cmp	lr, #4
	blo	.Lfo1
	ldr	r4, [r1], #4
	sub	lr, lr, #4
	cmp	r4, r8
	bne	1f
	vst1.32	{d0-d1}, [r0 :128]!
	b	.Lfo4
1:	eor	r4, r4, r8
	vmov	d4, r4, r4
	vceq.i8	d4, d4, #0
	vmovl.s8 q3, d4
	vmovl.s16 q9, d6
	vld1.32	{d8-d9}, [r0 :128]
	vbit	q4, q0, q9
	vst1.32	{d8-d9}, [r0 :128]!
	b	.Lfo4
.Lfo1:	cmp	lr, #0
	beq	.Lfdone
2:	ldrb	r12, [r1], #1
	cmp	r12, r2
	streq	r3, [r0]
	add	r0, r0, #4
	subs	lr, lr, #1
	bne	2b
.Lfdone:
	pop	{r4-r11, pc}

	/* everywhere: to a 16-byte boundary, then 64 bytes a step */
.Lfa:	cmp	lr, #0
	beq	.Lfdone
	tst	r0, #15
	beq	1f
	str	r3, [r0], #4
	sub	lr, lr, #1
	b	.Lfa
1:	subs	lr, lr, #16
	blo	3f
2:	vst1.32	{d0-d3}, [r0 :128]!
	vst1.32	{d0-d3}, [r0 :128]!
	subs	lr, lr, #16
	bhs	2b
3:	adds	lr, lr, #16
	beq	.Lfdone
4:	cmp	lr, #4
	blo	5f
	vst1.32	{d0-d1}, [r0 :128]!
	sub	lr, lr, #4
	b	4b
5:	cmp	lr, #0
	beq	.Lfdone
6:	str	r3, [r0], #4
	subs	lr, lr, #1
	bne	6b
	pop	{r4-r11, pc}
	.size	fill_neon, . - fill_neon

/* void glyph_neon(u32 *p, u32 stride, const u8 *own, u32 ostride, int slot,
 *		   const u8 *rows, u32 fw, u32 fh, u32 fg, u32 bg, int fill)
 *
 * A row of the glyph a step: its bits spread to 8 byte lanes (vtst
 * against 0x80, 0x40, ...), or-ed with all of them for fill, and-ed with
 * the width's columns and the owner bytes' comparison, widened to word
 * masks; the colours (vbsl: fg where a bit is set, bg where not) put
 * into the row's pixels where the mask is set (vbit). The row is
 * written back whether or not anything changed in it (no NEON to ARM
 * test). The fonts' widths, 8 and 6 (6: 24 bytes, nothing beside the
 * glyph touched), here; others in C. */
	.global	glyph_neon
	.type	glyph_neon, %function
	.balign	64
glyph_neon:
	ldr	r12, [sp, #8]			/* fw */
	cmp	r12, #8
	cmpne	r12, #6
	bne	glyph_c
	push	{r4-r8, lr}
	ldr	r4, [sp, #24]			/* slot */
	ldr	r5, [sp, #28]			/* rows */
	ldr	r6, [sp, #32]			/* fw */
	ldr	r7, [sp, #36]			/* fh */
	ldr	r12, [sp, #40]
	vdup.32	q12, r12			/* fg */
	ldr	r12, [sp, #44]
	vdup.32	q13, r12			/* bg */
	ldr	r12, [sp, #48]			/* fill */
	cmp	r12, #0
	movne	r12, #0xff
	vdup.8	d31, r12			/* all of the row with fill */
	lsl	r1, r1, #2			/* the screen's stride in bytes */
	adr	r12, .Lbits
	vld1.8	{d28}, [r12]!			/* 0x80, 0x40, ... 0x01 */
	cmp	r6, #8
	addne	r12, r12, #8
	vld1.8	{d29}, [r12]			/* the width's columns: 8 or 6 */
	vdup.8	d30, r4				/* the slot */
	cmp	r7, #0
	beq	9f
	cmp	r6, #8
	bne	.Lg6

.macro	GROW w
	ldrb	r12, [r5], #1
	vdup.8	d0, r12
	vtst.8	d0, d0, d28			/* the bits set */
	vorr	d1, d0, d31			/* written: the bits, or all with fill, */
	vand	d1, d1, d29			/* ... of the width */
	cmp	r4, #0
	blt	1f
	.if	\w == 8
	vld1.8	{d2}, [r2]
	.else
	vld1.32	{d2[0]}, [r2]
	add	r12, r2, #4
	vld1.16	{d2[2]}, [r12]
	.endif
	vceq.i8	d2, d2, d30
	vand	d1, d1, d2			/* ... where the slot owns it */
1:	vmovl.s8 q2, d0
	vmovl.s16 q3, d4
	vmovl.s16 q4, d5
	vbsl	q3, q12, q13			/* the colours: fg on, bg off */
	vbsl	q4, q12, q13
	vmovl.s8 q2, d1
	vmovl.s16 q8, d4
	vmovl.s16 q9, d5
	.if	\w == 8
	vld1.32	{d20-d23}, [r0]
	vbit	q10, q3, q8
	vbit	q11, q4, q9
	vst1.32	{d20-d23}, [r0]
	.else
	vld1.32	{d20-d22}, [r0]
	vbit	q10, q3, q8
	vbit	d22, d8, d18
	vst1.32	{d20-d22}, [r0]
	.endif
	add	r0, r0, r1
	add	r2, r2, r3
.endm

.Lg8:	GROW	8
	subs	r7, r7, #1
	bne	.Lg8
	pop	{r4-r8, pc}
.Lg6:	GROW	6
	subs	r7, r7, #1
	bne	.Lg6
9:	pop	{r4-r8, pc}
	.balign	8
.Lbits:	.byte	0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
	.byte	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	.byte	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00
	.size	glyph_neon, . - glyph_neon

/* void from_bgr_neon(u32 *p, const u8 *own, int slot, const u8 *bgr, u32 n)
 *
 * vld3.8 takes 8 pixels' blue, green and red bytes apart (d0, d1, d2);
 * vst4.8 with a zero d3 writes them as 8 words. Everywhere: 16 pixels a
 * step, two of each. Where a slot owns them: 8 a step, the owner bytes
 * compared on the ARM side as in fill_neon; mixed: the words made in
 * registers (vzip of B with G, R with 0, then of the halves) and put in
 * where the mask is set. p to a 16-byte boundary first; the rest
 * (fewer than 8) in C. */
	.global	from_bgr_neon
	.type	from_bgr_neon, %function
	.balign	64
from_bgr_neon:
	push	{r4-r10, lr}
	ldr	lr, [sp, #32]			/* n */
	vmov.i8	d3, #0
	vmov.i8	d7, #0
	/* to a 16-byte boundary of p, a pixel at a time */
.Lbh:	cmp	lr, #0
	beq	.Lbdone
	tst	r0, #15
	beq	.Lb8
	ldrb	r6, [r1], #1
	ldrb	r4, [r3]
	ldrb	r5, [r3, #1]
	ldrb	r7, [r3, #2]
	add	r3, r3, #3
	cmp	r2, #0
	cmpge	r6, r2
	orreq	r4, r4, r5, lsl #8
	orreq	r4, r4, r7, lsl #16
	streq	r4, [r0]
	cmp	r2, #0				/* slot < 0: the pixel everywhere */
	orrlt	r4, r4, r5, lsl #8
	orrlt	r4, r4, r7, lsl #16
	strlt	r4, [r0]
	add	r0, r0, #4
	sub	lr, lr, #1
	b	.Lbh
.Lb8:	cmp	r2, #0
	blt	.Lba
	and	r8, r2, #0xff
	orr	r8, r8, r8, lsl #8
	orr	r8, r8, r8, lsl #16
	movw	r10, #0x0101
	movt	r10, #0x0101
	lsl	r9, r10, #7			/* 0x80808080 */
	subs	lr, lr, #8
	blo	.Lbtail
.Lbm:	pld	[r3, #96]
	vld3.8	{d0-d2}, [r3]!
	ldr	r4, [r1]
	ldr	r5, [r1, #4]
	add	r1, r1, #8
	cmp	r4, r8
	cmpeq	r5, r8
	bne	1f
	vst4.8	{d0-d3}, [r0 :128]!		/* all 8 owned */
	subs	lr, lr, #8
	bhs	.Lbm
	b	.Lbtail
1:	eor	r4, r4, r8
	eor	r5, r5, r8
	sub	r6, r4, r10
	bic	r6, r6, r4
	sub	r7, r5, r10
	bic	r7, r7, r5
	orr	r6, r6, r7
	tst	r6, r9
	addeq	r0, r0, #32			/* none */
	beq	2f
	vmov	d16, r4, r5
	vceq.i8	d16, d16, #0
	vmovl.s8 q9, d16
	vmovl.s16 q10, d18
	vmovl.s16 q11, d19
	vzip.8	d0, d1				/* B G, pixels 0-3 and 4-7 */
	vzip.8	d2, d3				/* R 0 */
	vzip.16	q0, q1				/* B G R 0: pixels 0-3 (q0), 4-7 (q1) */
	vld1.32	{d24-d27}, [r0 :128]
	vbit	q12, q0, q10
	vbit	q13, q1, q11
	vst1.32	{d24-d27}, [r0 :128]!
	vmov.i8	d3, #0
2:	subs	lr, lr, #8
	bhs	.Lbm
	b	.Lbtail
	/* everywhere: 16 pixels a step, then 8 */
.Lba:	subs	lr, lr, #16
	blo	2f
1:	pld	[r3, #192]
	vld3.8	{d0-d2}, [r3]!
	vld3.8	{d4-d6}, [r3]!
	vst4.8	{d0-d3}, [r0 :128]!
	vst4.8	{d4-d7}, [r0 :128]!
	subs	lr, lr, #16
	bhs	1b
2:	adds	lr, lr, #8
	blo	.Lbtail
	vld3.8	{d0-d2}, [r3]!
	vst4.8	{d0-d3}, [r0 :128]!
	sub	lr, lr, #8
	/* the rest in C: from_bgr_c(p, own, slot, bgr, n), n on the stack */
.Lbtail:
	adds	lr, lr, #8
	beq	.Lbdone
	str	lr, [sp, #32]
	pop	{r4-r10, lr}
	b	from_bgr_c
.Lbdone:
	pop	{r4-r10, pc}
	.size	from_bgr_neon, . - from_bgr_neon

/* void to_bgr_neon(u8 *bgr, const u32 *p, u32 n)
 *
 * 16 pixels a step: vld4.8 takes them apart into blue, green, red and
 * the top byte, vst3.8 writes the first three back interleaved. p to a
 * 16-byte boundary first; the rest in C. */
	.global	to_bgr_neon
	.type	to_bgr_neon, %function
	.balign	64
to_bgr_neon:
1:	cmp	r2, #0
	bxeq	lr
	tst	r1, #15
	beq	2f
	ldr	r3, [r1], #4
	strb	r3, [r0]
	lsr	r3, r3, #8
	strb	r3, [r0, #1]
	lsr	r3, r3, #8
	strb	r3, [r0, #2]
	add	r0, r0, #3
	sub	r2, r2, #1
	b	1b
2:	subs	r2, r2, #16
	blo	4f
3:	pld	[r1, #128]
	vld4.8	{d0-d3}, [r1 :128]!
	vld4.8	{d4-d7}, [r1 :128]!
	vst3.8	{d0-d2}, [r0]!
	vst3.8	{d4-d6}, [r0]!
	subs	r2, r2, #16
	bhs	3b
4:	adds	r2, r2, #16
	bxeq	lr
	b	to_bgr_c
	.size	to_bgr_neon, . - to_bgr_neon

/* void rot90_neon(void *dst, u32 dst_stride, const u32 *src, u32 src_stride,
 *		   u32 w, u32 n, u32 bpp)
 *
 * The N950's flush. Bands of 16 screen lines from the bottom up; along
 * a band, blocks of 4 columns (32 bpp) or 2 (16 bpp): a 16-byte (8-byte)
 * load from each of the 16 lines, transposed so that a column's 16
 * pixels, bottom line first, lie in consecutive registers, as the
 * panel line wants them; written as 64 bytes (32 bpp) or, made RGB565
 * (vshrn, vmovn, vsri), 32 bytes to each of the block's panel lines.
 * Each panel line's runs are whole and aligned (dst, dst_stride 16-byte
 * aligned; the panel is uncached). The columns left over (w % 4, w % 2)
 * and the lines (n % 16, the band's top) in C; all of it in C for an
 * unaligned panel or another bpp. */
	.global	rot90_neon
	.type	rot90_neon, %function
	.balign	64
rot90_neon:
	push	{r0-r11, lr}			/* the arguments kept for C */
	ldr	r4, [sp, #52]			/* w */
	ldr	r5, [sp, #56]			/* n */
	ldr	r6, [sp, #60]			/* bpp */
	orr	r12, r0, r1
	tst	r12, #15
	bne	.Lrallc
	cmp	r5, #16
	blo	.Lrallc
	lsl	r3, r3, #2			/* the screen's stride in bytes */
	rsb	r12, r3, #0			/* ... a line up */
	sub	r7, r5, #1
	mla	r7, r7, r3, r2			/* the band's bottom line: n - 1 */
	mov	r8, r0				/* the band's first panel pixel */
	bic	r9, r5, #15			/* the lines in whole bands */
	cmp	r6, #32
	beq	.Lr32
	cmp	r6, #16
	bne	.Lrallc
	cmp	r4, #2
	blo	.Lrallc
	b	.Lr16band

	/* 32 bpp: line j of the band (0 the bottom) to q(j / 4 + 4 (j % 4)) */
.Lr32:	cmp	r4, #4
	blo	.Lrallc
	sub	r2, r1, #32			/* a panel line on, less what one store moved */
.Lr32band:
	mov	r10, r7
	mov	r11, r8
	mov	lr, r4
.Lr32col:
	.irp	q, 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15
	pld	[r10, #128]
	vld1.32	{q\q}, [r10], r12
	.endr
	add	r10, r10, r3, lsl #4		/* back to the band's bottom line ... */
	add	r10, r10, #16			/* ... 4 columns on */
	/* each group of 4 lines (q s, s + 4, s + 8, s + 12) transposed:
	 * column 0 in q s, 1 in q s + 4, 2 in q s + 8, 3 in q s + 12 */
	vtrn.32	q0, q4
	vtrn.32	q8, q12
	vswp	d1, d16
	vswp	d9, d24
	vtrn.32	q1, q5
	vtrn.32	q9, q13
	vswp	d3, d18
	vswp	d11, d26
	vtrn.32	q2, q6
	vtrn.32	q10, q14
	vswp	d5, d20
	vswp	d13, d28
	vtrn.32	q3, q7
	vtrn.32	q11, q15
	vswp	d7, d22
	vswp	d15, d30
	/* column 0: q0-q3, 1: q4-q7, 2: q8-q11, 3: q12-q15 */
	vst1.32	{d0-d3}, [r11 :128]!
	vst1.32	{d4-d7}, [r11 :128], r2
	vst1.32	{d8-d11}, [r11 :128]!
	vst1.32	{d12-d15}, [r11 :128], r2
	vst1.32	{d16-d19}, [r11 :128]!
	vst1.32	{d20-d23}, [r11 :128], r2
	vst1.32	{d24-d27}, [r11 :128]!
	vst1.32	{d28-d31}, [r11 :128], r2
	sub	lr, lr, #4
	cmp	lr, #4
	bhs	.Lr32col
	sub	r7, r7, r3, lsl #4		/* the next band: 16 lines up, */
	add	r8, r8, #64			/* 16 pixels on in the panel lines */
	subs	r9, r9, #16
	bne	.Lr32band
	mov	r6, #4				/* the columns of a block */
	b	.Lrrest

	/* 16 bpp: line j to d(j / 2 + 8 (j % 2)); after vtrn.32 of
	 * d k, d k + 8: column 0 in d0-d7, column 1 in d8-d15 */
.Lr16band:
	mov	r10, r7
	mov	r11, r8
	mov	lr, r4
.Lr16col:
	.irp	d, 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15
	pld	[r10, #128]
	vld1.32	{d\d}, [r10], r12
	.endr
	add	r10, r10, r3, lsl #4
	add	r10, r10, #8			/* 2 columns on */
	vtrn.32	d0, d8
	vtrn.32	d1, d9
	vtrn.32	d2, d10
	vtrn.32	d3, d11
	vtrn.32	d4, d12
	vtrn.32	d5, d13
	vtrn.32	d6, d14
	vtrn.32	d7, d15
	/* RGB565: (c >> 8) keeps R's top bits, then (c >> 5) G's, (c << 8)
	 * >> 11 of the low half B's: q8, q9 column 0, q10, q11 column 1 */
	vshrn.u32 d16, q0, #8
	vshrn.u32 d17, q1, #8
	vshrn.u32 d18, q2, #8
	vshrn.u32 d19, q3, #8
	vshrn.u32 d20, q4, #8
	vshrn.u32 d21, q5, #8
	vshrn.u32 d22, q6, #8
	vshrn.u32 d23, q7, #8
	vmovn.u32 d24, q0
	vmovn.u32 d25, q1
	vmovn.u32 d26, q2
	vmovn.u32 d27, q3
	vmovn.u32 d28, q4
	vmovn.u32 d29, q5
	vmovn.u32 d30, q6
	vmovn.u32 d31, q7
	vsri.16	q8, q12, #5
	vsri.16	q9, q13, #5
	vsri.16	q10, q14, #5
	vsri.16	q11, q15, #5
	vshl.u16 q12, q12, #8
	vshl.u16 q13, q13, #8
	vshl.u16 q14, q14, #8
	vshl.u16 q15, q15, #8
	vsri.16	q8, q12, #11
	vsri.16	q9, q13, #11
	vsri.16	q10, q14, #11
	vsri.16	q11, q15, #11
	vst1.16	{d16-d19}, [r11 :128], r1
	vst1.16	{d20-d23}, [r11 :128], r1
	sub	lr, lr, #2
	cmp	lr, #2
	bhs	.Lr16col
	sub	r7, r7, r3, lsl #4
	add	r8, r8, #32
	subs	r9, r9, #16
	bne	.Lr16band
	mov	r6, #2
	b	.Lrrest

	/* what is left, in C: the columns from w - w % cols on, all n lines;
	 * the lines above the bands (n % 16), the columns before. The
	 * arguments at sp: dst, dst_stride, src, src_stride; w, n, bpp at
	 * sp + 52 */
.Lrrest:
	sub	sp, sp, #16
	ldr	r4, [sp, #68]			/* w */
	ldr	r5, [sp, #72]			/* n */
	ldr	r7, [sp, #76]			/* bpp */
	sub	r6, r6, #1
	bic	r9, r4, r6			/* the columns done */
	subs	r10, r4, r9
	beq	1f
	str	r10, [sp]
	str	r5, [sp, #4]
	str	r7, [sp, #8]
	ldr	r0, [sp, #16]
	ldr	r1, [sp, #20]
	mla	r0, r9, r1, r0			/* dst + x0 dst_stride */
	ldr	r2, [sp, #24]
	add	r2, r2, r9, lsl #2		/* src + x0 */
	ldr	r3, [sp, #28]
	bl	rot90_c
1:	ands	r10, r5, #15			/* the lines above the bands */
	beq	2f
	str	r9, [sp]
	str	r10, [sp, #4]
	str	r7, [sp, #8]
	ldr	r0, [sp, #16]
	sub	r11, r5, r10
	lsr	r12, r7, #3
	mla	r0, r11, r12, r0		/* dst + (n - n % 16) bytes a pixel */
	ldr	r1, [sp, #20]
	ldr	r2, [sp, #24]
	ldr	r3, [sp, #28]
	bl	rot90_c
2:	add	sp, sp, #16
	pop	{r0-r11, pc}
	/* all of it in C, the arguments as they came */
.Lrallc:
	pop	{r0-r11, lr}
	b	rot90_c
	.size	rot90_neon, . - rot90_neon
