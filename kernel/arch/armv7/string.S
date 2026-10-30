/* KolibriOS for ARM, ARMv7 (Cortex-A8): memcpy and memset in assembler
 * (lib.c's C versions stay for SIMD=c and kolibri.bench; memmove stays
 * C, on this memcpy).
 *
 * They run everywhere in the kernel, also while a program's VFP/NEON
 * registers are in the CPU (arch/armv7/fpu.c keeps them there lazily):
 * NEON (64 bytes a step) only for large sizes and only when nobody's
 * values are in the registers (neon_free), else general registers
 * (ldm/stm, 32 bytes a step). Before fpu_init (neon_free 0) the MMU may
 * be off, all of memory strongly ordered: every access is aligned to its
 * size, as the C versions' were; so the fast paths need d and s alike
 * modulo 4, else bytes. Alignment hints only on what is aligned. */
	.syntax	unified
	.arm
	.fpu	neon
	.text

#define NEON_MIN 128

/* void *memcpy(void *d, const void *s, size_t n) */
	.global	memcpy
	.type	memcpy, %function
	.balign	64
memcpy:
	push	{r0, r4-r11, lr}
	eor	r3, r0, r1
	tst	r3, #3
	bne	.Lcb				/* not alike modulo 4: bytes */
	/* bytes to the word boundary */
1:	tst	r0, #3
	beq	2f
	cmp	r2, #0
	beq	.Lcret
	ldrb	r3, [r1], #1
	strb	r3, [r0], #1
	sub	r2, r2, #1
	b	1b
2:	cmp	r2, #NEON_MIN
	blo	.Lcw
	ldr	r3, =neon_free
	ldr	r3, [r3]
	cmp	r3, #0
	beq	.Lcw
	/* NEON: words to a 16-byte boundary of d, then 64 bytes a step;
	 * the loads aligned too if s is alike modulo 16 */
3:	tst	r0, #15
	ldrne	r3, [r1], #4
	strne	r3, [r0], #4
	subne	r2, r2, #4
	bne	3b
	sub	r2, r2, #64
	eor	r3, r0, r1
	tst	r3, #15
	bne	5f
4:	pld	[r1, #192]
	vld1.32	{d0-d3}, [r1 :128]!
	vld1.32	{d4-d7}, [r1 :128]!
	vst1.32	{d0-d3}, [r0 :128]!
	vst1.32	{d4-d7}, [r0 :128]!
	subs	r2, r2, #64
	bhs	4b
	b	6f
5:	pld	[r1, #192]
	vld1.32	{d0-d3}, [r1]!
	vld1.32	{d4-d7}, [r1]!
	vst1.32	{d0-d3}, [r0 :128]!
	vst1.32	{d4-d7}, [r0 :128]!
	subs	r2, r2, #64
	bhs	5b
6:	add	r2, r2, #64
	/* general registers: 32 bytes a step, then words, then bytes */
.Lcw:	subs	r2, r2, #32
	blo	8f
7:	pld	[r1, #64]
	ldm	r1!, {r3-r10}
	stm	r0!, {r3-r10}
	subs	r2, r2, #32
	bhs	7b
8:	adds	r2, r2, #28			/* the words: n - 4 */
	blo	10f
9:	ldr	r3, [r1], #4
	str	r3, [r0], #4
	subs	r2, r2, #4
	bhs	9b
10:	add	r2, r2, #4
.Lcb:	cmp	r2, #0
	beq	.Lcret
11:	ldrb	r3, [r1], #1
	strb	r3, [r0], #1
	subs	r2, r2, #1
	bne	11b
.Lcret:	pop	{r0, r4-r11, pc}
	.size	memcpy, . - memcpy

/* void *memset(void *d, int c, size_t n): the byte spread over a word,
 * bytes to the word boundary, then as memcpy: NEON or stm, words, bytes */
	.global	memset
	.type	memset, %function
	.balign	64
memset:
	push	{r0, r4-r11, lr}
	and	r1, r1, #0xff
	orr	r1, r1, r1, lsl #8
	orr	r1, r1, r1, lsl #16
1:	tst	r0, #3
	beq	2f
	cmp	r2, #0
	beq	.Lsret
	strb	r1, [r0], #1
	sub	r2, r2, #1
	b	1b
2:	cmp	r2, #NEON_MIN
	blo	.Lsw
	ldr	r3, =neon_free
	ldr	r3, [r3]
	cmp	r3, #0
	beq	.Lsw
	vdup.32	q0, r1
	vmov	q1, q0
3:	tst	r0, #15
	strne	r1, [r0], #4
	subne	r2, r2, #4
	bne	3b
	sub	r2, r2, #64
4:	vst1.32	{d0-d3}, [r0 :128]!
	vst1.32	{d0-d3}, [r0 :128]!
	subs	r2, r2, #64
	bhs	4b
	add	r2, r2, #64
.Lsw:	mov	r3, r1
	mov	r4, r1
	mov	r5, r1
	mov	r6, r1
	mov	r7, r1
	mov	r8, r1
	mov	r9, r1
	subs	r2, r2, #32
	blo	6f
5:	stm	r0!, {r1, r3-r9}
	subs	r2, r2, #32
	bhs	5b
6:	adds	r2, r2, #28
	blo	8f
7:	str	r1, [r0], #4
	subs	r2, r2, #4
	bhs	7b
8:	adds	r2, r2, #4
	beq	.Lsret
9:	strb	r1, [r0], #1
	subs	r2, r2, #1
	bne	9b
.Lsret:	pop	{r0, r4-r11, pc}
	.size	memset, . - memset
