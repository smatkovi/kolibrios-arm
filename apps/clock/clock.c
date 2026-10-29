/* clock: an analog clock, drawn with lines (function 38), in a fixed
 * skinned window; once a second. It fills its window's work area, so
 * that it is large where the window is (Hildon's look maximises it). */
#include "kos.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int R, CX, CY;

/* sin of 0..90 degrees, times 1024, in 6-degree steps (a minute) */
static const short sin6[16] = { 0, 107, 213, 316, 416, 512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018, 1024 };

static int isin(int deg)        /* deg: multiple of 6 */
{
	deg %= 360;
	if (deg < 0) deg += 360;
	int q = deg / 90, r = deg % 90;
	int v = q % 2 ? sin6[(90 - r) / 6] : sin6[r / 6];
	return q >= 2 ? -v : v;
}

static void hand(int deg, int len, u32 c, int thick)
{
	int x = CX + isin(deg) * len / 1024, y = CY - isin(deg + 90) * len / 1024;
	for (int d = -thick; d <= thick; d++) {
		kos_line(CX + d, CY, x + d, y, c);
		kos_line(CX, CY + d, x, y + d, c);
	}
}

static void draw(void)
{
	u32 t = kos_time();
	int h = ((t >> 4) & 0xf) * 10 + (t & 0xf);
	int m = ((t >> 12) & 0xf) * 10 + ((t >> 8) & 0xf);
	int s = ((t >> 20) & 0xf) * 10 + ((t >> 16) & 0xf);
	kos_begin_draw();
	kos_window(500, 40, 190, 200, KOS_WIN_FIXED | 0x101418, "Clock");
	/* the work area's size (function 9) */
	char info[1024];
	kos_thread_info(info, -1);
	int cw = *(u32 *)(info + 62) + 1, ch = *(u32 *)(info + 66) + 1;
	CX = cw / 2;
	CY = (ch - 16) / 2;
	R = MIN(cw, ch - 20) / 2 - 10;
	int th = R / 70;
	kos_rect(0, 0, cw, ch, 0x101418);
	for (int i = 0; i < 60; i++) {
		int a = i * 6, l = i % 5 ? 4 : 10;
		int x0 = CX + isin(a) * R / 1024, y0 = CY - isin(a + 90) * R / 1024;
		int x1 = CX + isin(a) * (R - l) / 1024, y1 = CY - isin(a + 90) * (R - l) / 1024;
		kos_line(x0, y0, x1, y1, i % 5 ? 0x607080 : 0xe0e8f0);
	}
	hand((h % 12) * 30 + (m / 12) * 6, R * 5 / 10, 0xe0e8f0, 1 + th);
	hand(m * 6, R * 8 / 10, 0xe0e8f0, 1 + th);
	hand(s * 6, R * 9 / 10, 0xe04040, th / 2);
	char buf[12];
	ksprintf(buf, "%02d:%02d:%02d", h, m, s);
	if (th) kos_text(CX - 32, CY + R + 6, buf, 0xa0b0c0, 1);
	else kos_text(CX - 24, CY + R + 4, buf, 0xa0b0c0, 0);
	kos_end_draw();
}

int main(void)
{
	draw();
	for (;;) {
		int ev = kos_wait_event_timeout(100);
		if (ev == KOS_EV_BUTTON && (kos_get_button() >> 8) == 1) kos_exit();
		if (ev == 0 || ev == KOS_EV_REDRAW) draw();
	}
}
