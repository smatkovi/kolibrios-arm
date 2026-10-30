/* KolibriOS for ARM, AArch64: memcpy, memmove and memset in assembler
 * (lib.c's C versions stay for ARMv7 and SIMD=c).
 *
 * General registers only (ldp/stp: 16 bytes an instruction), no NEON:
 * these run everywhere in the kernel, also under a Linux thread whose
 * FP/SIMD registers the CPU still holds. Every access is aligned to its
 * size, as the C versions' were: they also run with the MMU off (all
 * of memory Device-nGnRnE then) and on device memory (the Pro1X's
 * uncached regions are Device-GRE), where an unaligned access faults.
 * So the fast paths need d and s alike modulo 8 (or 4); else bytes.
 * size_t is 32 bits in this kernel: n arrives in w2. */
	.text

/* void *memcpy(void *d, const void *s, size_t n) */
	.global	memcpy
	.type	memcpy, %function
	.balign	64
memcpy:
	mov	w2, w2
	mov	x8, x0
.Lcpy:	eor	x9, x0, x1
	tst	x9, #7
	b.ne	.Lc4
	/* alike modulo 8: bytes to the boundary, then 64 bytes a step */
1:	tst	x0, #7
	b.eq	2f
	cbz	x2, .Lcret
	ldrb	w9, [x1], #1
	strb	w9, [x0], #1
	sub	x2, x2, #1
	b	1b
2:	cmp	x2, #64
	b.lo	4f
3:	ldp	x9, x10, [x1]
	ldp	x11, x12, [x1, #16]
	ldp	x13, x14, [x1, #32]
	ldp	x15, x16, [x1, #48]
	add	x1, x1, #64
	stp	x9, x10, [x0]
	stp	x11, x12, [x0, #16]
	stp	x13, x14, [x0, #32]
	stp	x15, x16, [x0, #48]
	add	x0, x0, #64
	sub	x2, x2, #64
	cmp	x2, #64
	b.hs	3b
4:	cmp	x2, #8
	b.lo	.Lc1
	ldr	x9, [x1], #8
	str	x9, [x0], #8
	sub	x2, x2, #8
	b	4b
	/* alike modulo 4: words */
.Lc4:	tst	x9, #3
	b.ne	.Lc1
5:	tst	x0, #3
	b.eq	6f
	cbz	x2, .Lcret
	ldrb	w9, [x1], #1
	strb	w9, [x0], #1
	sub	x2, x2, #1
	b	5b
6:	cmp	x2, #16
	b.lo	7f
	ldp	w9, w10, [x1]
	ldp	w11, w12, [x1, #8]
	add	x1, x1, #16
	stp	w9, w10, [x0]
	stp	w11, w12, [x0, #8]
	add	x0, x0, #16
	sub	x2, x2, #16
	b	6b
7:	cmp	x2, #4
	b.lo	.Lc1
	ldr	w9, [x1], #4
	str	w9, [x0], #4
	sub	x2, x2, #4
	b	7b
.Lc1:	cbz	x2, .Lcret
8:	ldrb	w9, [x1], #1
	strb	w9, [x0], #1
	subs	x2, x2, #1
	b.ne	8b
.Lcret:	mov	x0, x8
	ret
	.size	memcpy, . - memcpy

/* void *memmove(void *d, const void *s, size_t n): forwards (memcpy's
 * way) unless d lies inside s's bytes; then from the end, 8 bytes a step
 * where they are alike modulo 8 */
	.global	memmove
	.type	memmove, %function
	.balign	64
memmove:
	mov	w2, w2
	mov	x8, x0
	cmp	x0, x1
	b.ls	.Lcpy
	add	x9, x1, x2
	cmp	x0, x9
	b.hs	.Lcpy
	add	x0, x0, x2			/* the ends */
	mov	x1, x9
	eor	x9, x0, x1
	tst	x9, #7
	b.ne	3f
1:	tst	x0, #7
	b.eq	2f
	cbz	x2, 4f
	ldrb	w9, [x1, #-1]!
	strb	w9, [x0, #-1]!
	sub	x2, x2, #1
	b	1b
2:	cmp	x2, #16
	b.lo	3f
	ldp	x9, x10, [x1, #-16]!
	stp	x9, x10, [x0, #-16]!
	sub	x2, x2, #16
	b	2b
3:	cbz	x2, 4f
	ldrb	w9, [x1, #-1]!
	strb	w9, [x0, #-1]!
	sub	x2, x2, #1
	b	3b
4:	mov	x0, x8
	ret
	.size	memmove, . - memmove

/* void *memset(void *d, int c, size_t n): the byte spread over 64 bits,
 * bytes to an 8-byte boundary, 64 bytes a step, 8, then bytes */
	.global	memset
	.type	memset, %function
	.balign	64
memset:
	mov	w2, w2
	mov	x8, x0
	and	x1, x1, #0xff
	orr	x1, x1, x1, lsl #8
	orr	x1, x1, x1, lsl #16
	orr	x1, x1, x1, lsl #32
1:	tst	x0, #7
	b.eq	2f
	cbz	x2, 6f
	strb	w1, [x0], #1
	sub	x2, x2, #1
	b	1b
2:	cmp	x2, #64
	b.lo	4f
3:	stp	x1, x1, [x0]
	stp	x1, x1, [x0, #16]
	stp	x1, x1, [x0, #32]
	stp	x1, x1, [x0, #48]
	add	x0, x0, #64
	sub	x2, x2, #64
	cmp	x2, #64
	b.hs	3b
4:	cmp	x2, #8
	b.lo	5f
	str	x1, [x0], #8
	sub	x2, x2, #8
	b	4b
5:	cbz	x2, 6f
	strb	w1, [x0], #1
	sub	x2, x2, #1
	b	5b
6:	mov	x0, x8
	ret
	.size	memset, . - memset
