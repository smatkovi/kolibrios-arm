/* KolibriOS for ARM: a program's header and start (kernel/loader.c) */
	.syntax unified
	.arm
	.section .header, "ax"
	.global __header
__header:
	.ascii	"MENUETA1"
	.word	1			/* version */
	.word	_start
	.word	__image_end		/* end of the image */
	.word	__memory_size		/* memory the program gets */
	.word	__stack_top
	.word	__params		/* the parameters (256 bytes) */
	.word	__path			/* the program's path (1 KiB) */

	.text
	.global _start
_start:
	mov	fp, #0
	ldr	r0, =__params
#ifdef LIBC
	bl	__libc_start_main	/* lib/libc/start.c: main(argc, argv) */
#else
	bl	main
#endif
	mvn	r0, #0			/* function -1: end */
	svc	#0x40
1:	b	1b

	.bss
	.balign	4
	.global __params, __path
__params:
	.space	256
__path:
	.space	1024
