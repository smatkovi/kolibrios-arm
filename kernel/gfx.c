/* KolibriOS for ARM: drawing on the screen.
 *
 * As in KolibriOS, programs draw directly to the screen, and a map of
 * the screen (scr.owner, one byte per pixel) says which window shows each
 * pixel: a primitive draws only where its window owns the pixel, so a
 * covered window cannot paint over the one above it. The mouse pointer
 * is drawn over everything and taken away under each primitive. */
#include "gui.h"

struct screen scr;
extern const unsigned char font8x16[], font6x10[];

void gfx_init(void)
{
	scr.owner = kmalloc(scr.w * scr.h);
	scr.dirty_y0 = scr.h;
	scr.dirty_y1 = 0;
}

void gfx_dirty(int y0, int y1)
{
	if (y0 < scr.dirty_y0) scr.dirty_y0 = MAX(y0, 0);
	if (y1 > scr.dirty_y1) scr.dirty_y1 = MIN(y1, scr.h);
}

/* --- the mouse pointer ------------------------------------------------- */

#define CUR_W 12
#define CUR_H 19
static const char *cursor_img[CUR_H] = {
	"X           ",
	"XX          ",
	"X.X         ",
	"X..X        ",
	"X...X       ",
	"X....X      ",
	"X.....X     ",
	"X......X    ",
	"X.......X   ",
	"X........X  ",
	"X.........X ",
	"X......XXXXX",
	"X...X..X    ",
	"X..XX..X    ",
	"X.X  X..X   ",
	"XX   X..X   ",
	"X     X..X  ",
	"      X..X  ",
	"       XX   ",
};
static u32 under[CUR_W * CUR_H];
static int cur_x, cur_y, cur_shown, cur_hidden_depth;

static void cursor_draw(void)
{
	for (int y = 0; y < CUR_H; y++)
		for (int x = 0; x < CUR_W; x++) {
			int sx = cur_x + x, sy = cur_y + y;
			if (sx >= scr.w || sy >= scr.h) continue;
			u32 *p = &scr.fb[sy * scr.stride + sx];
			under[y * CUR_W + x] = *p;
			char c = cursor_img[y][x];
			if (c == 'X') *p = 0x000000;
			else if (c == '.') *p = 0xffffff;
		}
	gfx_dirty(cur_y, cur_y + CUR_H);
	cur_shown = 1;
}

static void cursor_erase(void)
{
	if (!cur_shown) return;
	for (int y = 0; y < CUR_H; y++)
		for (int x = 0; x < CUR_W; x++) {
			int sx = cur_x + x, sy = cur_y + y;
			if (sx >= scr.w || sy >= scr.h) continue;
			scr.fb[sy * scr.stride + sx] = under[y * CUR_W + x];
		}
	gfx_dirty(cur_y, cur_y + CUR_H);
	cur_shown = 0;
}

void cursor_hide(void)
{
	if (cur_hidden_depth++ == 0) cursor_erase();
}

void cursor_show(void)
{
	if (--cur_hidden_depth == 0) cursor_draw();
}

void cursor_move(int x, int y)
{
	cursor_hide();
	cur_x = x;
	cur_y = y;
	cursor_show();
}

/* a primitive's rectangle: the pointer off it while it draws */
static bool cur_over(int x, int y, int w, int h)
{
	return cur_shown && x < cur_x + CUR_W && cur_x < x + w && y < cur_y + CUR_H && cur_y < y + h;
}
#define BEGIN(x, y, w, h) bool _c = cur_over(x, y, w, h); if (_c) cursor_hide()
#define END() if (_c) cursor_show()

/* --- primitives -------------------------------------------------------- */

static inline void plot(int x, int y, u32 c, int slot)
{
	if ((unsigned)x >= (unsigned)scr.w || (unsigned)y >= (unsigned)scr.h) return;
	if (slot >= 0 && scr.owner[y * scr.w + x] != slot) return;
	scr.fb[y * scr.stride + x] = c & 0xffffff;
}

void gfx_pixel(int x, int y, u32 c, int slot)
{
	BEGIN(x, y, 1, 1);
	plot(x, y, c, slot);
	END();
	gfx_dirty(y, y + 1);
}

void gfx_rect(int x, int y, int w, int h, u32 c, int slot)
{
	int x0 = MAX(x, 0), y0 = MAX(y, 0);
	int x1 = MIN(x + w, scr.w), y1 = MIN(y + h, scr.h);
	if (x0 >= x1 || y0 >= y1) return;
	BEGIN(x0, y0, x1 - x0, y1 - y0);
	c &= 0xffffff;
	for (int yy = y0; yy < y1; yy++) {
		u32 *p = &scr.fb[yy * scr.stride];
		u8 *o = &scr.owner[yy * scr.w];
		if (slot < 0)
			for (int xx = x0; xx < x1; xx++) p[xx] = c;
		else
			for (int xx = x0; xx < x1; xx++)
				if (o[xx] == slot) p[xx] = c;
	}
	END();
	gfx_dirty(y0, y1);
}

/* KolibriOS's gradient: from the color down to darker, by lines */
void gfx_hgradient(int x, int y, int w, int h, u32 c, int slot)
{
	int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
	for (int i = 0; i < h; i++) {
		int f = 256 - i * 96 / (h ? h : 1);
		gfx_rect(x, y + i, w, 1, ((r * f >> 8) << 16) | ((g * f >> 8) << 8) | (b * f >> 8), slot);
	}
}

void gfx_line(int x0, int y0, int x1, int y1, u32 c, int slot)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	BEGIN(MIN(x0, x1), MIN(y0, y1), dx + 1, -dy + 1);
	for (;;) {
		plot(x0, y0, c, slot);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
	END();
	gfx_dirty(MIN(y0, y1), MAX(y0, y1) + 1);
}

int gfx_font_w(int font) { return font == 0 ? 6 : 8; }
int gfx_font_h(int font) { return font == 0 ? 10 : 16; }

/* the code page 437 glyph for a Unicode code point (Latin-1 and a few
 * others; KolibriOS's cp866 Cyrillic is not in the font) */
static u8 cp437_of(u32 u)
{
	static const u16 hi[128] = {
		0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7, 0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
		0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9, 0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0x20a7, 0x192,
		0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba, 0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
	};
	if (u < 128) return u;
	for (int i = 0; i < 48; i++)
		if (hi[i] == u) return 128 + i;
	if (u == 0xdf) return 0xe1;  /* sharp s */
	if (u == 0xb0) return 0xf8;  /* degree */
	return '?';
}

int gfx_text(int x, int y, const char *s, int len, u32 c, int font, int scale,
	     bool fill, u32 bg, int slot, bool utf8)
{
	const unsigned char *f = font == 0 ? font6x10 : font8x16;
	int fw = gfx_font_w(font), fh = gfx_font_h(font);
	if (scale < 1) scale = 1;
	int n = 0;
	const u8 *p = (const u8 *)s;
	const u8 *end = len >= 0 ? p + len : NULL;
	int width = 0;
	/* measure for the pointer and the dirty lines */
	for (const u8 *q = p; end ? q < end : *q; q++)
		if (!utf8 || (*q & 0xc0) != 0x80) n++;
	BEGIN(x, y, n * fw * scale, fh * scale);
	while (end ? p < end : *p) {
		u32 ch = *p++;
		if (utf8 && ch >= 0xc0) {
			int more = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : 1;
			ch &= 0x3f >> more;
			while (more-- && (end ? p < end : *p) && (*p & 0xc0) == 0x80)
				ch = ch << 6 | (*p++ & 0x3f);
			ch = cp437_of(ch);
		} else if (utf8 && ch >= 0x80) {
			continue;
		}
		const unsigned char *g = f + ch * fh;
		for (int row = 0; row < fh; row++) {
			u8 bits = g[row];
			for (int col = 0; col < fw; col++) {
				bool on = bits & (0x80 >> col);
				if (!on && !fill) continue;
				u32 col_c = on ? c : bg;
				if (scale == 1)
					plot(x + width + col, y + row, col_c, slot);
				else
					for (int sy = 0; sy < scale; sy++)
						for (int sx = 0; sx < scale; sx++)
							plot(x + width + col * scale + sx, y + row * scale + sy, col_c, slot);
			}
		}
		width += fw * scale;
	}
	END();
	gfx_dirty(y, y + fh * scale);
	return width;
}

void gfx_flush(void)
{
	if (scr.dirty_y0 >= scr.dirty_y1) return;
	if (plat->display_flush && !plat->display_flush(scr.dirty_y0, scr.dirty_y1))
		return;
	scr.dirty_y0 = scr.h;
	scr.dirty_y1 = 0;
}
