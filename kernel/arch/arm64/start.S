/* KolibriOS for ARM, AArch64 (QEMU's virt, the Pro1X): entry and
 * exceptions.
 *
 * The image has the Linux arm64 Image header, so whatever starts a Linux
 * kernel starts it (QEMU's -kernel, kexec): at EL1, MMU off, the device
 * tree's address in x0, anywhere in RAM (2 MiB aligned). It is linked at 0
 * and position independent: the entry applies its own relative
 * relocations first.
 *
 * The programs run at EL0 in AArch32, as on ARMv7. Every exception saves
 * x0-x30, ELR and SPSR on the one kernel stack (SP_EL1); trap64() turns
 * that into the AArch32 view (struct trapframe) and back. */
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

	.balign	0x800
vectors:
	VECTOR	1	/* EL1, SP_EL0: sync (not used) */
	VECTOR	2	/* irq */
	VECTOR	2	/* fiq */
	VECTOR	5	/* serror */
	VECTOR	1	/* EL1, SP_EL1: sync: a fault in the kernel */
	VECTOR	2	/* irq: in the idle loop */
	VECTOR	2	/* fiq */
	VECTOR	5	/* serror */
	VECTOR	6	/* EL0, AArch64 (Linux programs): sync: svc, aborts */
	VECTOR	7	/* irq */
	VECTOR	7	/* fiq */
	VECTOR	5	/* serror */
	VECTOR	3	/* EL0, AArch32: sync: svc, aborts, undefined */
	VECTOR	4	/* irq */
	VECTOR	4	/* fiq */
	VECTOR	5	/* serror */

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

/* nothing to run: interrupts on, no stack in use */
	.global idle_loop
idle_loop:
	wfi
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
