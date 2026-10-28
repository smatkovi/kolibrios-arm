/* KolibriOS for ARM: the kernel's start */
#include "gui.h"

extern u8 __kernel_end[];
extern u32 boot_args[3];
void input_init(void);

#ifdef PLATFORM_N950
#define PLATFORM platform_n950
#else
#define PLATFORM platform_realview
#endif

/* the programs the kernel starts: the ramdisk's "autorun" (one path per
 * line), or the panel alone */
static void autorun(void)
{
	const struct rd_file *f = fs_find("/sys/autorun");
	if (!f) {
		load_program("/sys/@taskbar", NULL, NULL);
		return;
	}
	char line[96];
	u32 n = 0;
	for (u32 i = 0; i <= f->size; i++) {
		char c = i < f->size ? f->data[i] : '\n';
		if (c == '\n' || c == '\r') {
			line[n] = 0;
			if (n && line[0] != '#') {
				char *params = NULL;
				for (char *p = line; *p; p++)
					if (*p == ' ') { *p = 0; params = p + 1; break; }
				if (load_program(line, params, NULL) < 0)
					kprintf("autorun: %s not started\n", line);
			}
			n = 0;
		} else if (n + 1 < sizeof(line)) {
			line[n++] = c;
		}
	}
}

void kmain(void)
{
	plat = &PLATFORM;
	if (plat->early_init) plat->early_init();
	kprintf("\nKolibriOS for ARM %d.%d on %s\n", KOS_VERSION_MAJOR, KOS_VERSION_MINOR, plat->name);
	kprintf("boot arguments %p %p %p, kernel ends at %p\n", boot_args[0], boot_args[1], boot_args[2], (u32)__kernel_end);
	mm_init(plat->ram_base, plat->ram_size, (u32)__kernel_end);
	mmu_init();
	kprintf("MMU on, %u KiB free\n", mm_free_kb());
	if (plat->display_init(&scr.fb, &scr.w, &scr.h, &scr.stride) < 0)
		panic("no display");
	kprintf("display %dx%d\n", scr.w, scr.h);
	gfx_init();
	sched_init();
	wm_init();
	input_init();
	fs_init();
	plat->init();
	autorun();
	gfx_flush();
	kprintf("running\n");
	/* to the first task (or the idle loop) through the trap return */
	asm volatile("svc #0");
	panic("back in kmain");
}
