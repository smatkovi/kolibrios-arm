/* KolibriOS for ARM: entry, exception vectors and the trap frame.
 *
 * Every exception ends up in SVC mode on the one kernel stack, with the
 * interrupted context saved as a struct trapframe (include/kernel.h):
 * r0-r12, the user mode sp and lr, a pad word, pc and cpsr. trap_entry()
 * may replace it with another task's before the return, which is how
 * tasks switch. The kernel itself runs with interrupts off; only the idle
 * loop (SVC mode, no stack) waits with them on. */
	.syntax unified
	.arm

	.section .text.boot, "ax"
	.global _start
_start:
	/* a zImage's head, so that kexec (ubiboot's) takes the image: eight
	 * instructions, then the magic, the load address and the end */
	.rept	7
	mov	r0, r0
	.endr
	mov	r0, r0
	b	boot
	.word	0x016f2818
	.word	_start
	.word	__image_end
	.word	0x04030201
boot:
	/* from QEMU's -kernel, NOLO or kexec: r0 = 0, r1 = machine, r2 = tags/DTB */
	cpsid	if
	cps	#0x13
	ldr	sp, =svc_stack_top
	mov	r5, r0
	mov	r6, r1
	mov	r7, r2
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
1:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	1b
	ldr	r4, =boot_args			/* in the .bss: after clearing it */
	stm	r4, {r5-r7}
	ldr	r0, =vectors
	mcr	p15, 0, r0, c12, c0, 0		/* VBAR */
	isb
	bl	kmain
2:	wfi
	b	2b

	.balign	32
vectors:
	b	_start
	b	undef_entry
	b	svc_entry
	b	pabt_entry
	b	dabt_entry
	b	.
	b	irq_entry
	b	fiq_entry

/* entry: lr already adjusted to the return address */
.macro TRAP kind
	srsdb	sp!, #0x13			/* pc, cpsr onto the SVC stack */
	cps	#0x13
	sub	sp, sp, #12
	stmia	sp, {sp, lr}^			/* usr_sp, usr_lr (pad above) */
	push	{r0-r12}
	mov	r0, sp
	mov	r1, #\kind
	bl	trap_entry
	b	trap_return
.endm

undef_entry:
	TRAP	1
svc_entry:
	TRAP	2
pabt_entry:
	sub	lr, lr, #4
	TRAP	3
dabt_entry:
	sub	lr, lr, #8
	TRAP	4
irq_entry:
	sub	lr, lr, #4
	TRAP	5
fiq_entry:
	sub	lr, lr, #4
	TRAP	6

trap_return:
	pop	{r0-r12}
	ldmia	sp, {sp, lr}^
	nop
	add	sp, sp, #12
	rfeia	sp!

/* nothing to run: wait for an interrupt with them on (SVC mode, no
 * stack in use: the trap frame of the interrupt that wakes it is built
 * on the empty kernel stack) */
	.global idle_loop
idle_loop:
	wfi
	b	idle_loop

	.section .bss
	.balign	8
	.global boot_args
boot_args:
	.space	12
	.balign	16
svc_stack:
	.space	32768
svc_stack_top:
