/* @taskbar: the panel at the bottom of the screen, as KolibriOS has it: a
 * menu button, a button per window (the active one highlighted; a tap
 * brings a window to the front), the clock */
#include "kos.h"

#define H 30
#define BTN_MENU 2
#define BTN_SLOT 100

static int sw, sh, self_slot;
static char info[1024];

struct entry { int slot; char name[12]; };
static struct entry list[32], shown[32];
static int nlist, nshown, active, shown_active;
static u32 shown_time;

static void scan(void)
{
	nlist = 0;
	int max = kos_thread_info(info, -1);
	for (int s = 2; s <= max && nlist < 32; s++) {
		kos_thread_info(info, s);
		if (*(u16 *)(info + 50) == 9) continue;   /* free slot */
		if (s == self_slot) continue;
		if (info[10] == '@') continue;            /* system programs */
		if (*(u32 *)(info + 42) == 0) continue;    /* no window */
		list[nlist].slot = s;
		memcpy(list[nlist].name, info + 10, 11);
		list[nlist].name[11] = 0;
		nlist++;
	}
	active = kos_active_slot();
}

static void draw(void)
{
	kos_begin_draw();
	kos_window_plain(0, sh - H, sw, H, KOS_WIN_PLAIN, 0x01000000);
	for (int y = 0; y < H; y++) {
		int f = 60 - y;
		kos_rect(0, y, sw, 1, (u32)(0x20 + f / 2) << 16 | (u32)(0x28 + f / 2) << 8 | (0x38 + f));
	}
	kos_rect(0, 0, sw, 1, 0x6880a8);
	kos_button(3, 3, 70, H - 6, BTN_MENU, 0x3c78c0);
	kos_text(14, 8, "KolibriOS", 0xffffff, 0);
	int x = 80, bw = 110;
	for (int i = 0; i < nlist && x + bw < sw - 60; i++) {
		u32 c = list[i].slot == active ? 0x5a8cc8 : 0x3a4c64;
		kos_button(x, 3, bw, H - 6, BTN_SLOT + list[i].slot, c);
		kos_text(x + 6, 9, list[i].name, 0xffffff, 0);
		x += bw + 4;
	}
	u32 t = kos_time();
	char clock[6] = { '0' + ((t >> 4) & 0xf), '0' + (t & 0xf), ':', '0' + ((t >> 12) & 0xf), '0' + ((t >> 8) & 0xf), 0 };
	kos_text(sw - 50, 7, clock, 0xffffff, 1);
	kos_end_draw();
	memcpy(shown, list, sizeof(list));
	nshown = nlist;
	shown_active = active;
	shown_time = t & 0xffff;
}

static int changed(void)
{
	if (nlist != nshown || active != shown_active || (kos_time() & 0xffff) != shown_time) return 1;
	for (int i = 0; i < nlist; i++)
		if (list[i].slot != shown[i].slot || strcmp(list[i].name, shown[i].name)) return 1;
	return 0;
}

/* the windows' changes come with the kernel's EV_SYSTEM; a timeout only
 * for the clock's next minute (none while the display is off) */
static u32 next_wait(void)
{
	if (kos_system_state() & KOS_SYS_DISPLAY_OFF) return 0;
	u32 t = kos_time();
	u32 s = ((t >> 20) & 0xf) * 10 + ((t >> 16) & 0xf);
	return (60 - (s < 60 ? s : 59)) * 100;
}

int main(void)
{
	kos_screen_size(&sw, &sh);
	kos_thread_info(info, -1);
	u32 pid = *(u32 *)(info + 30);
	self_slot = kos_call(18, 21, pid, 0, 0, 0);
	scan();
	draw();
	kos_event_mask(EVM_REDRAW | EVM_KEY | EVM_BUTTON | EVM_SYSTEM);
	for (;;) {
		u32 w = next_wait();
		int ev = w ? kos_wait_event_timeout(w) : kos_wait_event();
		switch (ev) {
		case KOS_EV_REDRAW:
			scan();
			draw();
			break;
		case KOS_EV_BUTTON: {
			u32 id = kos_get_button() >> 8;
			if (id == BTN_MENU)
				kos_run("/sys/menu", 0);
			else if (id >= BTN_SLOT)
				kos_activate_slot(id - BTN_SLOT);
			break;
		}
		case KOS_EV_KEY:
			kos_get_key();                 /* taken, else the event stays */
			break;
		case KOS_EV_SYSTEM:
		case 0:
			scan();
			if (changed()) draw();
			break;
		}
	}
}
