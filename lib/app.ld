/* KolibriOS for ARM: a program, from address 0 of its own address space;
 * the kernel zeroes what follows the file (.bss) */
ENTRY(_start)
SECTIONS
{
	. = 0;
	.header : { KEEP(*(.header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	/* the libraries the program uses (lib/libc/dll.c): { table, name } pairs */
	.kos_import : { __import_start = .; KEEP(*(.kos_import)) LONG(0) LONG(0) }
	. = ALIGN(16);
	__image_end = .;
	.bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(16);
	__bss_end = .;
	/* MEMORY_SIZE (default 1 MiB), the stack at its top */
	__memory_size = DEFINED(MEMORY_SIZE) ? MEMORY_SIZE : 0x100000;
	__stack_top = __memory_size - 16;
	/DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
