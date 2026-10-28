/* menu: the start menu, the programs of /sys; it closes when a program
 * starts or when another window becomes active */
#include "kos.h"

#define BTN_FIRST 10
#define ROW 24
#define W 190

static u8 folder[32 + 64 * 304];
static char names[64][24];
static int n, self_slot;

static int is_kex(const char *h)
{
	const char *m = "MENUETA1";
	for (int i = 0; i < 8; i++)
		if (h[i] != m[i]) return 0;
	return 1;
}

/* the ramdisk's programs (an ARM KolibriOS header), not the system's (@) */
static int is_program(const char *name)
{
	char path[64], head[8];
	if (name[0] == '@' || !strcmp(name, "menu")) return 0;
	ksprintf(path, "/sys/%s", name);
	struct kos_r2 r = kos_read_file(path, head, 8, 0);
	return r.ebx == 8 && is_kex(head);
}

int main(void)
{
	char info[1024];
	int sw, sh;
	kos_screen_size(&sw, &sh);
	kos_thread_info(info, -1);
	self_slot = kos_call(18, 21, *(u32 *)(info + 30), 0, 0, 0);
	struct kos_r2 r = kos_read_folder("/sys", folder, 0, 64);
	for (u32 i = 0; i < r.ebx && n < 64; i++) {
		const char *name = (const char *)folder + 32 + i * 304 + 40;
		if (is_program(name)) strcpy(names[n++], name);
	}
	int skin = kos_skin_height();
	int h = skin + 5 + n * ROW + 6;
	int y = sh - 30 - h;
	if (y < 0) y = 0;
	int seen_active = 0;
	for (;;) {
		int ev = kos_wait_event_timeout(20);
		if (ev == KOS_EV_REDRAW) {
			kos_begin_draw();
			kos_window(0, y, W, h, KOS_WIN_FIXED | 0xe8ecf0, "Programs");
			for (int i = 0; i < n; i++) {
				kos_button(4, 4 + i * ROW, W - 18, ROW - 2, BTN_FIRST + i, 0xd8e0ea);
				kos_text(12, 9 + i * ROW, names[i], 0x101820, 1);
			}
			kos_end_draw();
		} else if (ev == KOS_EV_BUTTON) {
			u32 id = kos_get_button() >> 8;
			if (id == 1) kos_exit();
			if (id >= BTN_FIRST && id < BTN_FIRST + (u32)n) {
				char path[64];
				ksprintf(path, "/sys/%s", names[id - BTN_FIRST]);
				kos_run(path, 0);
				kos_exit();
			}
		} else if (ev == KOS_EV_KEY) {
			if (((kos_get_key() >> 8) & 0xff) == 27) kos_exit();
		}
		/* away when another window is active (once ours was) */
		int a = kos_active_slot();
		if (a == self_slot) seen_active = 1;
		else if (seen_active) kos_exit();
	}
}
