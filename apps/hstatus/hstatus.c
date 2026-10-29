/* @hstatus: the status menu of the Hildon shell (apps/hildon), below its
 * status area: the date and time, the battery, the memory, how long the
 * system runs. A tap on it, Escape, or another window taken to the front
 * closes it. */
#include "kos.h"

#define BAR 56
#define W 340
#define ROW 52
#define ROWS 4

static void row(int i, const char *label, const char *value)
{
	int y = 12 + i * ROW;
	kos_text(20, y + 6, label, 0x9aa0a8, 1);
	kos_text_scaled(20, y + 22, value, 0xffffff, 1, 1);
	if (i < ROWS - 1) kos_rect(12, y + ROW - 6, W - 24, 1, 0x3a3e44);
}

static void draw(int sw)
{
	char date[32], time[16], mem[48], up[32];
	u32 t = kos_time(), d = kos_date();
	ksprintf(time, "%c%c:%c%c", '0' + ((t >> 4) & 0xf), '0' + (t & 0xf),
		 '0' + ((t >> 12) & 0xf), '0' + ((t >> 8) & 0xf));
	/* function 29: 0x00DDMMYY in BCD */
	int yr = ((d >> 4) & 0xf) * 10 + (d & 0xf);
	ksprintf(date, "%c%c.%c%c.%d, %s", '0' + ((d >> 20) & 0xf), '0' + ((d >> 16) & 0xf),
		 '0' + ((d >> 12) & 0xf), '0' + ((d >> 8) & 0xf),
		 yr < 70 ? 2000 + yr : 1900 + yr, time);
	ksprintf(mem, "%u MiB free of %u MiB", kos_free_ram_kb() >> 10, kos_total_ram_kb() >> 10);
	u32 s = kos_uptime() / 100;
	ksprintf(up, "%u h %u min", s / 3600, s / 60 % 60);
	int h = 12 + ROWS * ROW + 4;
	kos_begin_draw();
	/* a plain window (style 1): Hildon's look does not maximise it */
	kos_window_plain(sw - W - 8, BAR, W, h, KOS_WIN_PLAIN, 0x01000000);
	kos_rect(0, 0, W, h, 0x5a5e66);
	kos_rect(1, 0, W - 2, h - 1, 0x202328);
	kos_button_hidden(0, 0, W, h, 2);
	row(0, "Date and time", date);
	row(1, "Battery", "not measured yet");
	row(2, "Memory", mem);
	row(3, "Running for", up);
	kos_end_draw();
}

int main(void)
{
	char info[1024];
	int sw, sh;
	kos_screen_size(&sw, &sh);
	kos_thread_info(info, -1);
	int self = kos_call(18, 21, *(u32 *)(info + 30), 0, 0, 0);
	draw(sw);
	kos_set_zpos(KOS_ZPOS_ALWAYS_TOP);
	kos_activate_slot(self);
	for (;;) {
		int ev = kos_wait_event_timeout(20);
		if (ev == KOS_EV_REDRAW) draw(sw);
		else if (ev == KOS_EV_BUTTON) { kos_get_button(); kos_exit(); }
		else if (ev == KOS_EV_KEY) {
			if (((kos_get_key() >> 8) & 0xff) == 27) kos_exit();
		}
		if (kos_active_slot() != self) kos_exit();
	}
}
