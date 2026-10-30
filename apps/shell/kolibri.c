/* KolibriOS for ARM: the shell's system functions. KolibriOS's shell
 * (programs/system/shell, by Pavel "Albom" Mamontov and the KolibriOS
 * team: Siemargl, Coldy, Leency, rgimad and others, GPL-2.0) calls them
 * from system/kolibri.c, which does each with x86's int 0x40; this file
 * replaces it, with the same functions and arguments, through svc #0x40
 * (lib/kos.h). The shell's own sources are KolibriOS's, unchanged but
 * for apps/shell/arm.patch. */
#include <string.h>
#include <stdio.h>
#include <kos.h>
#include "system/kolibri.h"

void kol_exit() { kos_exit(); }
void kol_sleep(unsigned d) { kos_call(5, d, 0, 0, 0, 0); }

void kol_wnd_define(unsigned x, unsigned y, unsigned w, unsigned h, unsigned cs, unsigned b, char *t)
{
	kos_call(0, x * 65536 + w, y * 65536 + h, cs, b, (u32)t);
}

void kol_wnd_move(unsigned x, unsigned y) { kos_call(67, x, y, -1, -1, 0); }
void kol_event_mask(unsigned e) { kos_call(40, e, 0, 0, 0, 0); }
unsigned kol_event_wait() { return kos_call(10, 0, 0, 0, 0, 0); }
unsigned kol_event_wait_time(unsigned time) { return kos_call(23, time, 0, 0, 0, 0); }
unsigned kol_event_check() { return kos_call(11, 0, 0, 0, 0, 0); }
void kol_paint_start() { kos_call(12, 1, 0, 0, 0, 0); }
void kol_paint_end() { kos_call(12, 2, 0, 0, 0, 0); }
void kol_paint_pixel(unsigned x, unsigned y, unsigned c) { kos_call(1, x, y, c, 0, 0); }

void kol_paint_bar(unsigned x, unsigned y, unsigned w, unsigned h, unsigned c)
{
	kos_call(13, x * 65536 + w, y * 65536 + h, c, 0, 0);
}

void kol_paint_line(unsigned x1, unsigned y1, unsigned x2, unsigned y2, unsigned c)
{
	kos_call(38, x1 * 65536 + x2, y1 * 65536 + y2, c, 0, 0);
}

void kol_paint_string(unsigned x, unsigned y, char *s, unsigned c)
{
	kos_call(4, x * 65536 + y, c, (u32)s, 0, 0);
}

void kol_paint_image(unsigned x, unsigned y, unsigned w, unsigned h, char *d)
{
	kos_call(7, (u32)d, w * 65536 + h, x * 65536 + y, 0, 0);
}

void kol_paint_image_pal(unsigned x, unsigned y, unsigned w, unsigned h, char *d, unsigned *palette)
{
	kos_call(65, (u32)d, w * 65536 + h, x * 65536 + y, 8, (u32)palette);
}

unsigned kol_key_get() { return kos_call(2, 0, 0, 0, 0, 0); }
unsigned kol_key_control() { return kos_call(66, 3, 0, 0, 0, 0); }
void kol_key_lang_set(unsigned lang) { kos_call(21, 2, 9, lang, 0, 0); }
unsigned kol_key_lang_get() { return kos_call(26, 2, 9, 0, 0, 0); }
void kol_key_mode_set(unsigned mode) { kos_call(66, 1, mode, 0, 0, 0); }
unsigned kol_key_mode_get() { return kos_call(66, 2, 0, 0, 0, 0); }
unsigned kol_btn_get() { return kos_call(17, 0, 0, 0, 0, 0); }

void kol_btn_define(unsigned x, unsigned y, unsigned w, unsigned h, unsigned d, unsigned c)
{
	kos_call(8, x * 65536 + w, y * 65536 + h, d, c, 0);
}

void kol_btn_type(unsigned t) { kos_call(48, 1, t, 0, 0, 0); }
void kol_wnd_caption(char *s) { kos_call(71, 1, (u32)s, 0, 0, 0); }
unsigned kol_mouse_pos() { return kos_call(37, 0, 0, 0, 0, 0); }
unsigned kol_mouse_posw() { return kos_call(37, 1, 0, 0, 0, 0); }
unsigned kol_mouse_btn() { return kos_call(37, 2, 0, 0, 0, 0); }
void kol_board_putc(char c) { kos_call(63, 1, (u8)c, 0, 0, 0); }

void kol_board_puts(char *s)
{
	while (*s) kol_board_putc(*s++);
}

void kol_board_puti(int n)
{
	char c;
	if (n > 1) kol_board_puti(n / 10);
	c = n % 10 + '0';
	kol_board_putc(c);
}

int kol_file_70(kol_struct70 *k) { return kos_call(70, (u32)k, 0, 0, 0, 0); }

/* shared libraries (68.19): not in KolibriOS for ARM, its libraries are
 * linked into the programs */
kol_struct_import *kol_cofflib_load(char *name) { (void)name; return NULL; }

void *kol_cofflib_procload(kol_struct_import *imp, char *name)
{
	for (int i = 0; imp && imp[i].name; i++)
		if (!strcmp(imp[i].name, name)) return imp[i].data;
	return NULL;
}

unsigned kol_cofflib_procnum(kol_struct_import *imp)
{
	unsigned n = 0;
	while (imp && imp[n].name) n++;
	return n;
}

void kol_cofflib_procname(kol_struct_import *imp, char *name, unsigned n)
{
	strcpy(name, imp && imp[n].name ? imp[n].name : "");
}

unsigned kol_system_cpufreq() { return kos_call(18, 5, 0, 0, 0, 0); }
unsigned kol_system_mem() { return kos_call(18, 17, 0, 0, 0, 0); }
unsigned kol_system_memfree() { return kos_call(18, 16, 0, 0, 0, 0); }
unsigned kol_system_time_get() { return kos_call(3, 0, 0, 0, 0, 0); }
unsigned kol_system_date_get() { return kos_call(29, 0, 0, 0, 0, 0); }

/* 18.9 (shut down, reboot): not in KolibriOS for ARM, which has no
 * power controller driver on the PC's boards; the commands say so */
unsigned kol_system_end(unsigned param)
{
	extern int con_printf(const char *, ...);
	unsigned r = kos_call(18, 9, param, 0, 0, 0);
	if (r == (unsigned)-1) con_printf("  KolibriOS for ARM cannot turn off or restart the machine yet.\n\r");
	return r;
}

void kol_path_file2dir(char *dir, char *fname)
{
	unsigned i;
	strcpy(dir, fname);
	for (i = strlen(dir);; --i)
		if ('/' == dir[i]) {
			dir[i] = '\0';
			return;
		}
}

void kol_path_full(char *full, char *fname)
{
	char temp[256];
	if (*fname == '/') {
		strncpy(temp, fname + 1, 2);
		temp[2] = 0;
		if (!strcmp("rd", temp) || !strcmp("hd", temp) || !strcmp("cd", temp)) strcpy(full, fname);
	}
}

void kol_screen_wait_rr() { kos_call(18, 14, 0, 0, 0, 0); }
unsigned kol_skin_height() { return kos_call(48, 4, 0, 0, 0, 0); }
unsigned kol_thread_start(unsigned start, unsigned stack) { return kos_call(51, 1, start, stack, 0, 0); }
unsigned kol_time_tick() { return kos_call(26, 9, 0, 0, 0, 0); }
unsigned kol_sound_speaker(char data[]) { return kos_call(55, 55, 0, 0, (u32)data, 0); }
unsigned kol_process_info(unsigned slot, char buf1k[]) { return kos_call(9, (u32)buf1k, slot, 0, 0, 0); }
int kol_process_kill_pid(unsigned process) { return kos_call(18, 18, process, 0, 0, 0); }
int kol_kill_process(unsigned process) { return kos_call(18, 2, process, 0, 0, 0); }
void kol_get_kernel_ver(struct kernel_version *kv) { kos_call(18, 13, (u32)kv, 0, 0, 0); }

/* named shared memory (68.22, 68.23): not in KolibriOS for ARM */
int kol_buffer_open(char name[], int mode, int size, char **buf)
{
	(void)name; (void)mode; (void)size;
	*buf = NULL;
	return E_NOTFOUND;
}

void kol_buffer_close(char name[]) { (void)name; }

/* the current folder: function 30 (ktcc's libc has these) */
void set_cwd(const char *cwd) { kos_call(30, 1, (u32)cwd, 0, 0, 0); }
int getcwd(char *buf, unsigned size) { return kos_call(30, 2, (u32)buf, size, 0, 0); }

/* ktcc's libc exports mkdir as a pointer (cmd_mkdir.c: extern int
 * (*mkdir)(char*)): 0 made (or there already), else an error */
static int kos_mkdir(char *path) { return kos_create_folder(path) ? -1 : 0; }
int (*mkdir)(char *) = kos_mkdir;
