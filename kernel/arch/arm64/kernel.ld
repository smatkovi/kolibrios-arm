/* KolibriOS for ARM, AArch64: linked at 0, position independent (the
 * entry relocates it to where it was loaded) */
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		*(.text.boot)
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(8);
	.rela.dyn : {
		__rela_start = .;
		*(.rela .rela.*)
		__rela_end = .;
	}
	.data : { *(.data .data.*) *(.got .got.*) }
	.dynamic : { *(.dynamic) }
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }
	. = ALIGN(8);
	__image_end = .;
	.bss (NOLOAD) : ALIGN(4096) {
		__bss_start = .;
		*(.bss .bss.*) *(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	}
	. = ALIGN(4096);
	__kernel_end = .;
	__image_size = __kernel_end - _start;
	/DISCARD/ : { *(.comment) *(.note*) *(.interp) *(.eh_frame*) }
}
