/* The N950's flush as it was before blit.rot90 (kernel/platform/n950.c
 * up to 2026-10-30): each screen line a panel column, pixel by pixel;
 * compiled as the kernel compiles it, for bench32's count of the old
 * way (tests/asm/Makefile) */
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;

#define PANEL_W 480
#define PANEL_H 864
#define HIDDEN 5
#define SCR_W (PANEL_H - 2 * HIDDEN)

void n950_flush_old(u32 *lcd, int lcd_bpp, int lcd_w, int lcd_h, const u32 *shadow, int y0, int y1)
{
	int lcd_stride = lcd_w;
	for (int y = y0; y < y1; y++) {
		const u32 *src = &shadow[y * SCR_W];
		int px = PANEL_W - 1 - y;
		if (px < 0 || px >= lcd_w) continue;
		if (lcd_bpp == 32) {
			for (int x = 0; x < SCR_W; x++) {
				int py = HIDDEN + x;
				if (py < lcd_h) lcd[py * lcd_stride + px] = src[x];
			}
		} else {
			u16 *l16 = (u16 *)lcd;
			for (int x = 0; x < SCR_W; x++) {
				int py = HIDDEN + x;
				u32 c = src[x];
				if (py < lcd_h)
					l16[py * lcd_stride + px] = ((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 0x1f);
			}
		}
	}
}
