/* KolibriOS for ARM, AArch64 (QEMU's virt, the Pro1X): entry and
 * exceptions.
 *
 * The image has the Linux arm64 Image header, so whatever starts a Linux
 * kernel starts it (QEMU's -kernel, kexec): at EL1, MMU off, the device
 * tree's address in x0, anywhere in RAM (2 MiB aligned). It is linked at 0
 * and position independent: the entry applies its own relative
 * relocations first.
 *
 * The programs run at EL0 in AArch32, as on ARMv7. An exception from a
 * program stores its r0-r14, pc and cpsr straight into its task's frame
 * (struct trapframe, current->tf) and comes back from a task's frame
 * the same way (trap32() in trap.c, the A32 vectors and a32_entry
 * below); the interrupt of the idle loop saves nothing (the loop has
 * nothing to keep). The rest (Linux programs, AArch64 at EL0; the
 * kernel's faults) saves x0-x30, ELR and SPSR on the one kernel stack
 * (SP_EL1) for trap64(); a Linux thread, or the idle loop, after an
 * AArch32 trap goes back through that frame too. */
	.section .text.boot, "ax"
	.global _start
_start:
	b	entry			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	__image_size		/* image_size, BSS included */
	.quad	0xa			/* little endian, 4 KiB pages, anywhere */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

entry:
	msr	daifset, #0xf
	mov	x19, x0			/* the device tree */
	adr	x20, _start		/* where we run */
	mrs	x1, CurrentEL
	cmp	x1, #(1 << 2)
	b.ne	.			/* only from EL1 */
	/* relative relocations: *(base + offset) = base + addend */
	adrp	x1, __rela_start
	add	x1, x1, :lo12:__rela_start
	adrp	x2, __rela_end
	add	x2, x2, :lo12:__rela_end
1:	cmp	x1, x2
	b.hs	2f
	ldp	x3, x4, [x1], #16
	ldr	x5, [x1], #8
	and	x4, x4, #0xffffffff
	cmp	x4, #1027		/* R_AARCH64_RELATIVE */
	b.ne	1b
	add	x5, x5, x20
	str	x5, [x20, x3]
	b	1b
2:	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b
4:	adrp	x1, stack_top
	add	x1, x1, :lo12:stack_top
	mov	sp, x1
	adrp	x1, boot_args
	add	x1, x1, :lo12:boot_args
	str	x19, [x1]
	str	x20, [x1, #8]
	adrp	x1, vectors
	add	x1, x1, :lo12:vectors
	msr	vbar_el1, x1
	isb
	bl	kmain
	b	.

/* the frame: x0-x30, ELR, SPSR, a pad word: 34 x 8 bytes */
#define FRAME (34 * 8)

.macro SAVE kind
	sub	sp, sp, #FRAME
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mrs	x0, elr_el1
	mrs	x1, spsr_el1
	stp	x0, x1, [sp, #248]
	mov	x0, sp
	mov	x1, #\kind
	bl	trap64
	b	trap_return
.endm

.macro VECTOR kind
	.balign	0x80
	SAVE	\kind
.endm

/* offsetof(struct task, tf) (trap.c checks it); struct trapframe: r0-r12,
 * usr_sp, usr_lr, a pad word, pc, cpsr (u32s) */
#define TASK_TF 24
#define K_IRQ_EL1 2
#define K_SYNC_A32 3
#define K_IRQ_A32 4

/* from a program (AArch32 at EL0): its registers into current->tf. x15-x30
 * are not the program's (AArch32's r0-r14 are x0-x14): free here, and
 * nothing of them is kept */
.macro A32 kind
	.balign	0x80
	adrp	x16, current
	ldr	x16, [x16, :lo12:current]
	add	x16, x16, #TASK_TF
	stp	w0, w1, [x16]
	stp	w2, w3, [x16, #8]
	stp	w4, w5, [x16, #16]
	stp	w6, w7, [x16, #24]
	stp	w8, w9, [x16, #32]
	stp	w10, w11, [x16, #40]
	stp	w12, w13, [x16, #48]
	stp	w14, wzr, [x16, #56]
	mrs	x17, elr_el1
	mrs	x18, spsr_el1
	stp	w17, w18, [x16, #64]
	mov	w0, #\kind
	b	a32_entry
.endm

/* the idle loop's interrupt: its registers are not needed again (it is
 * entered afresh, or a task is), its stack starts empty */
.macro IDLE_IRQ
	.balign	0x80
	adrp	x16, stack_top
	add	x16, x16, :lo12:stack_top
	mov	sp, x16
	mov	w0, #K_IRQ_EL1
	b	a32_entry
.endm

	.balign	0x800
vectors:
	VECTOR	1	/* EL1, SP_EL0: sync (not used) */
	VECTOR	2	/* irq */
	VECTOR	2	/* fiq */
	VECTOR	5	/* serror */
	VECTOR	1	/* EL1, SP_EL1: sync: a fault in the kernel */
	IDLE_IRQ	/* irq: in the idle loop */
	IDLE_IRQ	/* fiq */
	VECTOR	5	/* serror */
	VECTOR	6	/* EL0, AArch64 (Linux programs): sync: svc, aborts */
	VECTOR	7	/* irq */
	VECTOR	7	/* fiq */
	VECTOR	5	/* serror */
	A32	K_SYNC_A32	/* EL0, AArch32: sync: svc, aborts, undefined */
	A32	K_IRQ_A32	/* irq */
	A32	K_IRQ_A32	/* fiq */
	VECTOR	5	/* serror */

/* trap32(kind, esr, far, frame): the task to go back to by its frame
 * (an AArch32 one), or 0 after it filled the frame below the stack's
 * top for trap_return (a Linux thread, the idle loop) */
a32_entry:
	sub	sp, sp, #FRAME
	mrs	x1, esr_el1
	mrs	x2, far_el1
	mov	x3, sp
	bl	trap32
	cbz	x0, trap_return
	add	sp, sp, #FRAME
	/* into the program from its task's frame (x0) */
	.global	a32_return
a32_return:
	add	x16, x0, #TASK_TF
	ldp	w17, w18, [x16, #64]
	msr	elr_el1, x17
	msr	spsr_el1, x18
	ldp	w0, w1, [x16]
	ldp	w2, w3, [x16, #8]
	ldp	w4, w5, [x16, #16]
	ldp	w6, w7, [x16, #24]
	ldp	w8, w9, [x16, #32]
	ldp	w10, w11, [x16, #40]
	ldp	w12, w13, [x16, #48]
	ldr	w14, [x16, #56]
	eret

	.global trap_return
trap_return:
	ldp	x0, x1, [sp, #248]
	msr	elr_el1, x0
	msr	spsr_el1, x1
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #FRAME
	eret

/* kmain's way into the first task: the frame at the top of the kernel
 * stack (x0), returned through */
	.global arch_enter
arch_enter:
	mov	sp, x0
	b	trap_return

/* nothing to run: arch_idle (timer.c) with interrupts masked waits for
 * the next work or an interrupt, which is taken here once they are open
 * again: its frame on an empty stack, as from the old "wfi; b ." */
	.global idle_loop
idle_loop:
	msr	daifset, #2
	bl	arch_idle
	msr	daifclr, #2
	isb
	b	idle_loop

	.section .bss
	.balign	16
	.global boot_args
boot_args:
	.space	24
	.balign	16
	.global stack_top
stack:
	.space	65536
stack_top:
