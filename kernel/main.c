/* KolibriOS for ARM: the kernel's start */
#include "gui.h"

extern u8 __kernel_end[];
extern addr_t boot_args[3];
void input_init(void);


#ifdef __aarch64__
#include "fdt.h"
#include "net.h"
#endif

/* the look the command line asks for (kolibri.look=classic or hildon, on
 * the AArch64 kernel, which has a device tree): the ramdisk's
 * autorun.classic or autorun.hildon instead of autorun. Read before the
 * MMU is on: the kernel does not map the device tree. */
static const char *autorun_path = "/sys/autorun";
static char cmdline[512];               /* the kernel command line, kept */

static void look_arg(void)
{
#ifdef __aarch64__
	const void *fdt = (const void *)boot_args[0];
	if (!fdt_ok(fdt)) return;
	int ac, sc, len = 0;
	int ch = fdt_find_name(fdt, "chosen", &ac, &sc);
	const char *a = ch >= 0 ? fdt_prop(fdt, ch, "bootargs", &len) : NULL;
	if (a) strncpy0(cmdline, a, MIN((u32)len + 1, sizeof(cmdline)));
	for (int i = 0; a && i + 13 <= len; i++) {
		if ((i && a[i - 1] != ' ') || strncmp(a + i, "kolibri.look=", 13)) continue;
		if (i + 20 <= len && !strncmp(a + i + 13, "classic", 7)) autorun_path = "/sys/autorun.classic";
		if (i + 19 <= len && !strncmp(a + i + 13, "hildon", 6)) autorun_path = "/sys/autorun.hildon";
	}
#endif
}

/* an argument of the kernel command line: its value after "=", "" for
 * one without, NULL if it is not there */
const char *boot_arg(const char *name)
{
	u32 n = strlen(name);
	for (const char *p = cmdline; *p; p++) {
		if ((p != cmdline && p[-1] != ' ') || strncmp(p, name, n)) continue;
		if (p[n] == '=') return p + n + 1;
		if (!p[n] || p[n] == ' ') return "";
	}
	return NULL;
}

/* the programs the kernel starts: the ramdisk's "autorun" (one path per
 * line), or the panel alone */
static void autorun(void)
{
	const struct rd_file *f = fs_find(autorun_path);
	if (f && strcmp(autorun_path, "/sys/autorun")) kprintf("autorun: %s\n", autorun_path);
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
	plat = arch_platform();
	look_arg();
	if (plat->early_init) plat->early_init();
	kprintf("\nKolibriOS for ARM %d.%d on %s\n", KOS_VERSION_MAJOR, KOS_VERSION_MINOR, plat->name);
	kprintf("boot arguments %p %p %p, kernel ends at %p\n", boot_args[0], boot_args[1], boot_args[2], (addr_t)__kernel_end);
	mm_init((addr_t)__kernel_end);
	mmu_init();
	kprintf("MMU on, %u MiB of %u MiB free\n", mm_free_kb() >> 10, mm_total_kb() >> 10);
	if (plat->display_init(&scr.fb, &scr.w, &scr.h, &scr.stride) < 0)
		panic("no display");
	kprintf("display %dx%d\n", scr.w, scr.h);
	gfx_init();
	sched_init();
	wm_init();
	input_init();
	fs_init();
	plat->init();
	/* the modem: the Pro1X's starts in its platform; in QEMU a fake one */
#ifdef __aarch64__
	if (plat == &platform_virt) kmodem_fake();
#else
	if (plat == &platform_realview) kmodem_fake();
#endif
#ifdef __aarch64__
	/* the Linux personality (kernel/linux); kolibri.lxtrace: its system
	 * calls on the console */
	extern void lx_init(void), lx_trace_on(void);
	lx_init();
	if (boot_arg("kolibri.lxtrace")) lx_trace_on();
#endif
	autorun();
#ifdef __aarch64__
	/* kolibri.run=/bin/x (a Linux program) or /sys/x: one more program
	 * from the ramdisk (tests) */
	const char *run = boot_arg("kolibri.run");
	if (run && *run) {
		char path[64] = "/sys/linux";
		u32 n = 0, at = strncmp(run, "/sys/", 5) ? 10 : 0;
		while (run[n] && run[n] != ' ' && at + n + 1 < sizeof(path)) n++;
		memcpy(path + at, run, n);
		path[at + n] = 0;
		if (load_program(path, NULL, NULL) < 0) kprintf("kolibri.run: %s not started\n", run);
	}
#endif
	gfx_flush();
	kprintf("running\n");
	/* to the first task (or the idle loop) through the trap return */
	arch_start();
	panic("back in kmain");
}
