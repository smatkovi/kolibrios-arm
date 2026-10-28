/* board: KolibriOS's debug board, what the kernel and the programs write
 * to the debug output (function 63), the last lines */
#include "kos.h"

#define COLS 70
#define ROWS 22
static char lines[ROWS][COLS + 1];
static int cur, col;

static void put(int c)
{
	if (c == '\r') return;
	if (c == '\n' || col == COLS) {
		cur = (cur + 1) % ROWS;
		lines[cur][0] = 0;
		col = 0;
		if (c == '\n') return;
	}
	lines[cur][col++] = c;
	lines[cur][col] = 0;
}

static int poll(void)
{
	int got = 0;
	for (;;) {
		struct kos_r2 r = kos_call2(63, 2, 0, 0, 0, 0);
		if (!r.ebx) break;
		put(r.eax & 0xff);
		got = 1;
	}
	return got;
}

static void draw(void)
{
	kos_begin_draw();
	kos_window(40, 30, COLS * 6 + 24, ROWS * 11 + 36, KOS_WIN_SKINNED | 0x000000, "Debug board");
	kos_rect(0, 0, COLS * 6 + 14, ROWS * 11 + 8, 0x000000);
	for (int i = 0; i < ROWS; i++) {
		int l = (cur + 1 + i) % ROWS;
		kos_text(4, 4 + i * 11, lines[l], 0x80e080, 0);
	}
	kos_end_draw();
}

int main(void)
{
	poll();
	draw();
	for (;;) {
		int ev = kos_wait_event_timeout(25);
		if (ev == KOS_EV_BUTTON && (kos_get_button() >> 8) == 1) kos_exit();
		if (ev == KOS_EV_REDRAW || (ev == 0 && poll())) draw();
	}
}
