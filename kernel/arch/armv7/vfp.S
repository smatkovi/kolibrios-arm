/* KolibriOS for ARM, ARMv7: the VFP/NEON registers saved and loaded
 * (fpu.c: d0-d31 and FPSCR in struct fpstate), FPEXC */
	.syntax unified
	.arm
	.fpu	neon
	.text

/* void fp_hw_on(void): FPEXC.EN, nothing else (EX, DEX cleared) */
	.global	fp_hw_on
fp_hw_on:
	mov	r0, #0x40000000
	vmsr	fpexc, r0
	bx	lr

	.global	fp_fpexc
fp_fpexc:
	vmrs	r0, fpexc
	bx	lr

	.global	fp_fpsid
fp_fpsid:
	vmrs	r0, fpsid
	bx	lr

	.global	fp_mvfr0
fp_mvfr0:
	vmrs	r0, mvfr0
	bx	lr

/* void fp_save(struct fpstate *a) */
	.global	fp_save
fp_save:
	vstmia	r0!, {d0-d15}
	vstmia	r0!, {d16-d31}
	vmrs	r1, fpscr
	str	r1, [r0]
	bx	lr

/* void fp_load(const struct fpstate *a) */
	.global	fp_load
fp_load:
	vldmia	r0!, {d0-d15}
	vldmia	r0!, {d16-d31}
	ldr	r1, [r0]
	vmsr	fpscr, r1
	bx	lr
