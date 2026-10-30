/* KolibriOS for ARM: the kernel's start */
#include "gui.h"

extern u8 __kernel_end[];
extern addr_t boot_args[3];
void input_init(void);


#ifdef __aarch64__
#include "fdt.h"
#include "net.h"
#endif

/* the kernel command line, read before the MMU is on (the kernel does
 * not map what the boot loader left): the image's own arguments (make
 * CMDLINE=..., build/<platform>/cmdline.h), then the boot loader's: on
 * AArch64 the device tree's /chosen/bootargs, on ARMv7 the ATAGs' command
 * line (r2: kexec's and NOLO's, QEMU's -append with the raw kernel.bin) */
#ifndef KOLIBRI_CMDLINE
#define KOLIBRI_CMDLINE ""
#endif
static const char *autorun_path = "/sys/autorun";
static char cmdline[512];               /* the kernel command line, kept */

#ifndef __aarch64__
#define ATAG_CORE 0x54410001u
#define ATAG_CMDLINE 0x54410009u
static const char *atag_cmdline(addr_t at, u32 *len)
{
	if (at < plat->ram_base || at - plat->ram_base >= plat->ram_size - 16384 || (at & 3)) return NULL;
	const u32 *t = (const u32 *)at;
	if (t[1] != ATAG_CORE || (t[0] != 2 && t[0] != 5)) return NULL;
	for (u32 words = 0; t[0] >= 2 && words + t[0] < 4096; words += t[0], t += t[0])
		if (t[1] == ATAG_CMDLINE) {
			*len = (t[0] - 2) * 4;
			return (const char *)(t + 2);
		}
	return NULL;
}
#endif

const char *boot_arg(const char *name);

static void cmdline_init(void)
{
	const char *a = NULL;
	u32 len = 0;
#ifdef __aarch64__
	const void *fdt = (const void *)boot_args[0];
	if (fdt_ok(fdt)) {
		int ac, sc, l = 0;
		int ch = fdt_find_name(fdt, "chosen", &ac, &sc);
		a = ch >= 0 ? fdt_prop(fdt, ch, "bootargs", &l) : NULL;
		len = l;
	}
#else
	a = atag_cmdline(boot_args[2], &len);
#endif
	u32 n = strlen(KOLIBRI_CMDLINE);
	strncpy0(cmdline, KOLIBRI_CMDLINE, sizeof(cmdline));
	if (a && len && n + 2 < sizeof(cmdline)) {
		if (n) cmdline[n++] = ' ';
		strncpy0(cmdline + n, a, MIN(len + 1, sizeof(cmdline) - n));
	}
	/* the look: the ramdisk's autorun.classic or autorun.hildon instead
	 * of autorun (kolibri.look=classic or hildon) */
	const char *look = boot_arg("kolibri.look");
	if (look && !strncmp(look, "classic", 7)) autorun_path = "/sys/autorun.classic";
	if (look && !strncmp(look, "hildon", 6)) autorun_path = "/sys/autorun.hildon";
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
				/* the Hildon shell: its look from the start, so the
				 * programs started beside it see its colours (48.3)
				 * before it sets its skin (48.8) */
				u32 ll = strlen(line);
				if (ll >= 8 && !strcmp(line + ll - 8, "/@hildon")) wm_set_look(LOOK_HILDON);
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
	cmdline_init();
	if (plat->early_init) plat->early_init();
	kprintf("\nKolibriOS for ARM %d.%d on %s\n", KOS_VERSION_MAJOR, KOS_VERSION_MINOR, plat->name);
	kprintf("boot arguments %p %p %p, kernel ends at %p\n", boot_args[0], boot_args[1], boot_args[2], (addr_t)__kernel_end);
	if (cmdline[0]) kprintf("command line: %s\n", cmdline);
	mm_init((addr_t)__kernel_end);
	mmu_init();
#ifndef __aarch64__
	/* VFP and NEON: on for the kernel, lazily for the programs
	 * (arch/armv7/fpu.c; AArch64: blit_init, arch/arm64/fpu.c) */
	extern void fpu_init(void);
	fpu_init();
#endif
	kprintf("MMU on, %u MiB of %u MiB free\n", mm_free_kb() >> 10, mm_total_kb() >> 10);
	if (plat->display_init(&scr.fb, &scr.w, &scr.h, &scr.stride) < 0)
		panic("no display");
	kprintf("display %dx%d\n", scr.w, scr.h);
	gfx_init();
	sched_init();
	wm_init();
	input_init();
	fs_init();
	extern void kfs_init(void);
	kfs_init();
	plat->init();
	/* the disks the platform's drivers found: their partitions and file
	 * systems (read only), the container (kernel/storage, docs/storage.md) */
	extern void storage_start(void), kfs_settings_init(void);
	storage_start();
	kfs_settings_init();
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
#ifndef SIMD_C
	/* kolibri.bench: the drawing and the string functions timed, C
	 * against assembler (bench.c, docs/asm.md) */
	extern void bench_run(void);
	if (boot_arg("kolibri.bench")) bench_run();
#endif
	autorun();
	/* kolibri.run=/sys/x, or /bin/x (a Linux program, AArch64): one more
	 * program from the ramdisk (tests) */
	const char *run = boot_arg("kolibri.run");
	if (run && *run) {
		char path[64] = "/sys/linux";
		u32 n = 0, at = strncmp(run, "/sys/", 5) ? 10 : 0;
		while (run[n] && run[n] != ' ' && at + n + 1 < sizeof(path)) n++;
		memcpy(path + at, run, n);
		path[at + n] = 0;
		/* kolibri.args=a,b: its parameters, "a b" */
		char params[96] = "";
		const char *pa = boot_arg("kolibri.args");
		for (u32 i = 0; pa && pa[i] && pa[i] != ' ' && i + 1 < sizeof(params); i++)
			params[i] = pa[i] == ',' ? ' ' : pa[i], params[i + 1] = 0;
		if (load_program(path, params[0] ? params : NULL, NULL) < 0)
			kprintf("kolibri.run: %s not started\n", run);
	}
	gfx_flush();
	kprintf("running\n");
	/* to the first task (or the idle loop) through the trap return */
	arch_start();
	panic("back in kmain");
}
