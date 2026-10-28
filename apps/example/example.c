/* example: KolibriOS's example program in C: a window with text, two
 * buttons and the last key pressed */
#include "kos.h"

static int clicks;
static u32 last_key;

static void draw(void)
{
	char buf[64];
	kos_begin_draw();
	kos_window(120, 60, 360, 210, KOS_WIN_SKINNED | 0xf0f0f0, "Example");
	kos_text(12, 10, "KolibriOS on ARM: a program in C,", 0x202020, 1);
	kos_text(12, 28, "its system calls by svc #0x40.", 0x202020, 1);
	kos_button(12, 60, 140, 28, 10, 0x6a9ad8);
	kos_text(30, 66, "Click me", 0xffffff, 1);
	kos_button(170, 60, 140, 28, 11, 0xc86060);
	kos_text(204, 66, "Close", 0xffffff, 1);
	ksprintf(buf, "Clicks: %d", clicks);
	kos_text_bg(12, 104, buf, 0x103060, 1, 0xf0f0f0);
	if (last_key) {
		u32 a = (last_key >> 8) & 0xff, sc = (last_key >> 16) & 0xff;
		ksprintf(buf, "Key: '%c' ascii %u, scan code 0x%x  ", a >= 32 && a < 127 ? a : '?', a, sc);
	} else {
		ksprintf(buf, "Press a key");
	}
	kos_text_bg(12, 128, buf, 0x103060, 1, 0xf0f0f0);
	kos_end_draw();
}

int main(void)
{
	draw();
	for (;;) {
		switch (kos_wait_event()) {
		case KOS_EV_REDRAW:
			draw();
			break;
		case KOS_EV_KEY:
			last_key = kos_get_key();
			draw();
			break;
		case KOS_EV_BUTTON: {
			u32 id = kos_get_button() >> 8;
			if (id == 1 || id == 11) kos_exit();
			if (id == 10) { clicks++; draw(); }
			break;
		}
		}
	}
}
