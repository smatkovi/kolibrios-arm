/* KolibriOS for ARM, AArch64: a core's state across a PSCI CPU_SUSPEND
 * into a power-down state (psci.c), the way Linux's
 * arch/arm64/kernel/sleep.S keeps it: the callee-saved registers, the
 * stack pointer and the EL1 system registers in a context, cleaned to the
 * point of coherency; the firmware comes back at cpu_resume with the MMU
 * and the caches off and the context's address in x0, which loads it all
 * again and returns 0 from psci_suspend as if the call had returned. The
 * kernel's addresses are its physical ones (1:1), so the MMU goes on
 * under the running code. What else the core loses is set up again in C
 * (the GIC's CPU interface: gic3_cpu_init).
 *
 *   long psci_suspend(u64 fid, u64 power_state, u64 hvc, u64 simulate)
 *
 * hvc: the conduit (else smc). simulate (QEMU, whose CPU_SUSPEND is a
 * wfi): after a return without an error the core's state is destroyed as
 * a power-down would (the MMU and the caches off, the registers cleared)
 * and the firmware's way back is taken, through cpu_resume. */

	.arch	armv8-a+fp+simd

#define C_X19 0
#define C_SP 96
#define C_SCTLR 104
#define C_TCR 112
#define C_TTBR0 120
#define C_TTBR1 128
#define C_MAIR 136
#define C_VBAR 144
#define C_CPACR 152
#define C_CNTKCTL 160
#define C_TPIDR1 168
#define C_TPIDR0 176
#define C_TPIDRRO 184
#define C_SPEL0 192
#define C_CVAL 200
#define C_CTL 208
#define C_FPCR 216
#define C_FPSR 224
#define C_FLAGS 232
#define C_Q 256                 /* q0-q31 */
#define C_SIZE 768

	.text
	.global psci_suspend
psci_suspend:
	adrp	x9, sctx
	add	x9, x9, :lo12:sctx
	stp	x19, x20, [x9, #C_X19]
	stp	x21, x22, [x9, #C_X19 + 16]
	stp	x23, x24, [x9, #C_X19 + 32]
	stp	x25, x26, [x9, #C_X19 + 48]
	stp	x27, x28, [x9, #C_X19 + 64]
	stp	x29, x30, [x9, #C_X19 + 80]
	mov	x10, sp
	str	x10, [x9, #C_SP]
	mrs	x10, sctlr_el1
	str	x10, [x9, #C_SCTLR]
	mrs	x10, tcr_el1
	str	x10, [x9, #C_TCR]
	mrs	x10, ttbr0_el1
	str	x10, [x9, #C_TTBR0]
	mrs	x10, ttbr1_el1
	str	x10, [x9, #C_TTBR1]
	mrs	x10, mair_el1
	str	x10, [x9, #C_MAIR]
	mrs	x10, vbar_el1
	str	x10, [x9, #C_VBAR]
	mrs	x10, cpacr_el1
	str	x10, [x9, #C_CPACR]
	mrs	x10, cntkctl_el1
	str	x10, [x9, #C_CNTKCTL]
	mrs	x10, tpidr_el1
	str	x10, [x9, #C_TPIDR1]
	mrs	x10, tpidr_el0
	str	x10, [x9, #C_TPIDR0]
	mrs	x10, tpidrro_el0
	str	x10, [x9, #C_TPIDRRO]
	mrs	x10, sp_el0
	str	x10, [x9, #C_SPEL0]
	mrs	x10, cntv_cval_el0
	str	x10, [x9, #C_CVAL]
	mrs	x10, cntv_ctl_el0
	str	x10, [x9, #C_CTL]
	/* the FP/SIMD registers, if the kernel may use them (FPEN 01 or 11:
	 * a Linux thread's or a program's may still be in them: lx.c and
	 * fpu.c save them lazily) */
	str	xzr, [x9, #C_FLAGS]
	mrs	x10, cpacr_el1
	tbz	x10, #20, 1f
	add	x11, x9, #C_Q
	stp	q0, q1, [x11], #32
	stp	q2, q3, [x11], #32
	stp	q4, q5, [x11], #32
	stp	q6, q7, [x11], #32
	stp	q8, q9, [x11], #32
	stp	q10, q11, [x11], #32
	stp	q12, q13, [x11], #32
	stp	q14, q15, [x11], #32
	stp	q16, q17, [x11], #32
	stp	q18, q19, [x11], #32
	stp	q20, q21, [x11], #32
	stp	q22, q23, [x11], #32
	stp	q24, q25, [x11], #32
	stp	q26, q27, [x11], #32
	stp	q28, q29, [x11], #32
	stp	q30, q31, [x11], #32
	mrs	x10, fpcr
	str	x10, [x9, #C_FPCR]
	mrs	x10, fpsr
	str	x10, [x9, #C_FPSR]
	mov	x10, #1
	str	x10, [x9, #C_FLAGS]
1:	/* to the point of coherency: cpu_resume reads it with the caches off */
	mov	x10, x9
	add	x11, x9, #C_SIZE
2:	dc	civac, x10
	add	x10, x10, #64
	cmp	x10, x11
	b.lo	2b
	dsb	sy
	/* CPU_SUSPEND: x0 the function, x1 the state, x2 the entry point,
	 * x3 the context id; x19-x21 survive the call (SMCCC) */
	mov	x19, x2
	mov	x20, x3
	mov	x21, x9
	adr	x2, cpu_resume
	mov	x3, x9
	cbnz	x19, 3f
	smc	#0
	b	4f
3:	hvc	#0
4:	/* back without a power-down: refused, a pending interrupt, or QEMU */
	cbz	x20, 5f
	cbz	x0, simulate_loss
5:	ldp	x19, x20, [x21, #C_X19]
	ldr	x21, [x21, #C_X19 + 16]
	ret

/* QEMU: what a power-down leaves of the core, then the way back */
simulate_loss:
	mrs	x10, cpacr_el1
	ubfx	x10, x10, #20, #2
	cmp	x10, #3
	b.ne	6f
	movi	v0.16b, #0
	movi	v1.16b, #0
	movi	v2.16b, #0
	movi	v3.16b, #0
	movi	v4.16b, #0
	movi	v5.16b, #0
	movi	v6.16b, #0
	movi	v7.16b, #0
	movi	v8.16b, #0
	movi	v9.16b, #0
	movi	v10.16b, #0
	movi	v11.16b, #0
	movi	v12.16b, #0
	movi	v13.16b, #0
	movi	v14.16b, #0
	movi	v15.16b, #0
	movi	v16.16b, #0
	movi	v17.16b, #0
	movi	v18.16b, #0
	movi	v19.16b, #0
	movi	v20.16b, #0
	movi	v21.16b, #0
	movi	v22.16b, #0
	movi	v23.16b, #0
	movi	v24.16b, #0
	movi	v25.16b, #0
	movi	v26.16b, #0
	movi	v27.16b, #0
	movi	v28.16b, #0
	movi	v29.16b, #0
	movi	v30.16b, #0
	movi	v31.16b, #0
	msr	fpcr, xzr
	msr	fpsr, xzr
6:	mrs	x10, sctlr_el1
	mov	x11, #(1 << 0 | 1 << 2 | 1 << 12)     /* M, C, I */
	bic	x10, x10, x11
	msr	sctlr_el1, x10
	isb
	msr	ttbr0_el1, xzr
	msr	ttbr1_el1, xzr
	msr	tcr_el1, xzr
	msr	mair_el1, xzr
	msr	vbar_el1, xzr
	msr	cntkctl_el1, xzr
	msr	tpidr_el0, xzr
	msr	tpidrro_el0, xzr
	msr	tpidr_el1, xzr
	msr	sp_el0, xzr
	msr	cntv_ctl_el0, xzr
	msr	cntv_cval_el0, xzr
	msr	S3_0_C4_C6_0, xzr               /* ICC_PMR_EL1: everything masked */
	msr	S3_0_C12_C12_7, xzr             /* ICC_IGRPEN1_EL1: group 1 off */
	msr	cpacr_el1, xzr
	isb
	mov	x0, x21
	mov	x10, #0
	mov	sp, x10
	mov	x19, #0
	mov	x29, #0
	mov	x30, #0
	b	cpu_resume

/* the warm boot: EL1, the MMU and the caches off, x0 the context */
	.global cpu_resume
cpu_resume:
	msr	daifset, #0xf
	mov	x9, x0
	ldr	x10, [x9, #C_MAIR]
	msr	mair_el1, x10
	ldr	x10, [x9, #C_TCR]
	msr	tcr_el1, x10
	ldr	x10, [x9, #C_TTBR0]
	msr	ttbr0_el1, x10
	ldr	x10, [x9, #C_TTBR1]
	msr	ttbr1_el1, x10
	ldr	x10, [x9, #C_VBAR]
	msr	vbar_el1, x10
	ldr	x10, [x9, #C_CPACR]
	msr	cpacr_el1, x10
	ldr	x10, [x9, #C_CNTKCTL]
	msr	cntkctl_el1, x10
	ldr	x10, [x9, #C_TPIDR1]
	msr	tpidr_el1, x10
	ldr	x10, [x9, #C_TPIDR0]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #C_TPIDRRO]
	msr	tpidrro_el0, x10
	ldr	x10, [x9, #C_SPEL0]
	msr	sp_el0, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	ldr	x10, [x9, #C_SCTLR]
	msr	sctlr_el1, x10
	isb
	ic	iallu
	dsb	nsh
	isb
	/* the MMU and the caches on again: the kernel's memory as it was */
	ldr	x10, [x9, #C_SP]
	mov	sp, x10
	ldr	x10, [x9, #C_FLAGS]
	cbz	x10, 7f
	add	x11, x9, #C_Q
	ldp	q0, q1, [x11], #32
	ldp	q2, q3, [x11], #32
	ldp	q4, q5, [x11], #32
	ldp	q6, q7, [x11], #32
	ldp	q8, q9, [x11], #32
	ldp	q10, q11, [x11], #32
	ldp	q12, q13, [x11], #32
	ldp	q14, q15, [x11], #32
	ldp	q16, q17, [x11], #32
	ldp	q18, q19, [x11], #32
	ldp	q20, q21, [x11], #32
	ldp	q22, q23, [x11], #32
	ldp	q24, q25, [x11], #32
	ldp	q26, q27, [x11], #32
	ldp	q28, q29, [x11], #32
	ldp	q30, q31, [x11], #32
	ldr	x10, [x9, #C_FPCR]
	msr	fpcr, x10
	ldr	x10, [x9, #C_FPSR]
	msr	fpsr, x10
7:	ldr	x10, [x9, #C_CVAL]
	msr	cntv_cval_el0, x10
	ldr	x10, [x9, #C_CTL]
	msr	cntv_ctl_el0, x10
	isb
	ldp	x19, x20, [x9, #C_X19]
	ldp	x21, x22, [x9, #C_X19 + 16]
	ldp	x23, x24, [x9, #C_X19 + 32]
	ldp	x25, x26, [x9, #C_X19 + 48]
	ldp	x27, x28, [x9, #C_X19 + 64]
	ldp	x29, x30, [x9, #C_X19 + 80]
	adrp	x10, resumed
	add	x10, x10, :lo12:resumed
	ldr	w11, [x10]
	add	w11, w11, #1
	str	w11, [x10]
	mov	x0, #0
	ret

	.section .bss
	.balign	64
sctx:
	.space	C_SIZE
	.global resumed
	.balign	4
resumed:			/* the returns through cpu_resume */
	.space	4
