/* the ramdisk (tools/mkrd.py), in the kernel image */
	.section .rodata
	.balign 16
	.global ramdisk_start, ramdisk_end
ramdisk_start:
	.incbin RAMDISK
ramdisk_end:
