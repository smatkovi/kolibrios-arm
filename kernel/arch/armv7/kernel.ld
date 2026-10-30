/* KolibriOS for ARM: the kernel image, linked where it runs (KERNEL_BASE:
 * 0x70010000 on QEMU's realview-pb-a8, where QEMU loads a raw image,
 * 0x80008000 on the N950, where NOLO and kexec put a zImage) */
ENTRY(_start)
SECTIONS
{
	. = KERNEL_BASE;
	.text : {
		*(.text.boot)
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	__image_end = .;
	.bss (NOLOAD) : ALIGN(16384) {
		__bss_start = .;
		*(.bss .bss.*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	}
	__kernel_end = .;
	/DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
