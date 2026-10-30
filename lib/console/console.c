/* KolibriOS for ARM: the console library (conio.h), translated to C from
 * KolibriOS's programs/develop/libraries/console_coff/console.asm
 * ("Functions for interaction with the console for KolibriOS programs",
 * diamond (Evgeny Grechnikov) 2006-2008, the KolibriOS team 2006-2025,
 * GPL-2.0), version 2.9's behaviour: the same window (80 x 25 characters
 * of console.obj's 8x16 font, a scroll bar from conscrlv.bmp, 300 lines
 * kept), colours, Esc sequences, keys and line editing; the names of its
 * labels kept (con.write_char -> con_write_char, ...).
 *
 * As console.obj, the window is a thread's of its own (con_thread,
 * console.asm's con.thread, created by con_init with 51.1): it draws,
 * reads the keys into the input buffer and the mouse, and is woken by
 * IPC (60.2) when the program has changed something (con_wake: a byte,
 * the operation). The program's calls write into the buffer and wait for
 * keys as console.obj's do (5 2 until there is one). Its close button
 * ends the program's thread that called con_init (18.18) and then its
 * own; con_exit(0) leaves the window with "[Finished]" in its title
 * after the program has ended, until it is closed. The Hildon look
 * maximises windows: the console then shows as many lines as fit
 * (wnd_height follows the window). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include "conio.h"
#include "console_font.h"     /* font8x16.bmp, by tools/kos-bmp.py */
#include "console_scroll.h"   /* conscrlv.bmp */

typedef short s16;
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define font_width console_font_W
#define font_height console_font_H

/* the scroll bar's parts in conscrlv.bmp (conscrl.inc) */
#define vscroll_width console_scroll_W
#define vscroll_btn_height 21
#define vscroll_bgr_height 2
#define vscroll_bar_height1 2
#define vscroll_bar_height2 1
#define vscroll_bar_height3 2
#define VS(row) (console_scroll + (row) * vscroll_width * 3)
#define vscroll_btn1 VS(0)
#define vscroll_btn2 VS(vscroll_btn_height)
#define vscroll_btn3 VS(2 * vscroll_btn_height)
#define vscroll_btn4 VS(3 * vscroll_btn_height)
#define vscroll_bgr1 VS(4 * vscroll_btn_height)
#define vscroll_bgr2 VS(4 * vscroll_btn_height + vscroll_bgr_height)
#define vscroll_bar1 VS(4 * vscroll_btn_height + 2 * vscroll_bgr_height)
#define vscroll_bar2 VS(4 * vscroll_btn_height + 2 * vscroll_bgr_height + vscroll_bar_height1)
#define vscroll_bar3 VS(4 * vscroll_btn_height + 2 * vscroll_bgr_height + vscroll_bar_height1 + vscroll_bar_height2)

int __con_is_load = 1;

static const u32 con_colors[16] = {
	0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xC0C0C0,
	0x808080, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
};

/* in the current implementation, the default values are: */
#define def_wnd_width 80
#define def_wnd_height 25
#define def_scr_width 80
#define def_scr_height 300
#define def_wnd_x 200
#define def_wnd_y 50

static struct {
	int init_cmd;
	u32 wnd_width, wnd_height;         /* the visible part, in characters */
	u32 main_scr_width, main_scr_height;
	u32 scr_width, scr_height;         /* the buffer in use */
	u8 *mainbuffer, *altbuffer, *data; /* 2 bytes a character: the code, the attribute */
	u8 *image;                         /* the visible part, a byte a pixel */
	u32 image_cap;
	u32 cur_x, cur_y, wnd_xpos, wnd_ypos;
	u32 saved_x, saved_y;              /* the main buffer's cursor (Esc[?1049h) */
	int cursor_height;
	int cursor_visible;
	int app_cursor_keys;
	u32 scroll_top, scroll_bot;        /* Esc[t;br: the scrolled lines */
	char title[256];
	u8 kbd_layout[128];
	int bWasE0;
	/* input: the keys typed, KolibriOS's 16-bit codes */
	u16 input[128];
	int input_start, input_end;
	/* the scroll bar */
	int data_width, data_height;
	int vscrollbar_pos, vscrollbar_size;
	int vscroll_pt;                    /* dragging: where the slider was taken, else -1 */
	int bUpPressed, bDownPressed, bScrollingUp, bScrollingDown;
	u32 press_time;
	int drawn, dirty;
	u32 last_draw;
	int skin_h, client_w, client_h;
} con = { .vscroll_pt = -1, .cursor_visible = 1 };

static volatile u32 con_flags = 0x07;           /* grey on black */
static u32 con_flags_attr;             /* 0x08 bright text, 0x80 bright background */

/* the Esc sequence being read */
static int con_esc, con_sci;
static u32 con_esc_attr_n, con_esc_attrs[4];
static int con_esc_private;
static char con_osc_str[256];
static int con_osc_strlen;

static void con_draw_window(void);
static void con_draw_image(void);

/* the thread and its wake-up (console.asm's con.thread_op, con.ipc_buf,
 * con.wake): the operations of the byte sent */
enum { OP_EXIT = 1, OP_SET_TITLE, OP_REDRAW, OP_GETCH, OP_RESIZE };
static volatile u8 con_thread_op;
/* dd 0, 8, 0, 0 and a byte: room for one message of a byte */
static volatile u8 con_ipc_buf[0x14] __attribute__((aligned(4))) = { 0, 0, 0, 0, 8 };
static volatile u32 con_console_tid, con_parent_tid;
static u32 con_stack[4096] __attribute__((aligned(16)));
static void con_wake(void);
static void con_thread(void);

/* --- the window ----------------------------------------------------- */

static inline void put_image_pal(const void *img, int w, int h, int x, int y, int bpp, const u32 *pal, int pad)
{
	register u32 r0 asm("r0") = 65, r1 asm("r1") = (u32)img, r2 asm("r2") = (u32)w << 16 | h;
	register u32 r3 asm("r3") = (u32)x << 16 | (u16)y, r4 asm("r4") = bpp, r5 asm("r5") = (u32)pal;
	register u32 r6 asm("r6") = pad;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6) :: "memory");
}

static int with_scrollbar(void) { return con.wnd_height < con.scr_height; }

/* the visible part of the buffer into the picture, the cursor inverted
 * over its last lines (con.data2image) */
static void con_data2image(void)
{
	u32 pw = con.wnd_width * font_width;
	for (u32 y = 0; y < con.wnd_height; y++) {
		const u8 *row = con.data + ((con.wnd_ypos + y) * con.scr_width + con.wnd_xpos) * 2;
		for (u32 x = 0; x < con.wnd_width; x++) {
			u8 c = row[2 * x], a = row[2 * x + 1];
			u8 fg = a & 15, bg = a >> 4;
			u8 *d = con.image + y * font_height * pw + x * font_width;
			for (int l = 0; l < font_height; l++, d += pw) {
				u32 bits = console_font[l][c];
				for (int i = 0; i < font_width; i++) d[i] = (bits >> i) & 1 ? fg : bg;
			}
		}
	}
	if (!con.cursor_visible || !con.cursor_height) return;
	if (con.cur_y < con.wnd_ypos || con.cur_y >= con.wnd_ypos + con.wnd_height) return;
	if (con.cur_x < con.wnd_xpos || con.cur_x >= con.wnd_xpos + con.wnd_width) return;
	u8 *d = con.image + ((con.cur_y - con.wnd_ypos + 1) * font_height) * pw +
		(con.cur_x - con.wnd_xpos) * font_width;
	for (int l = 0; l < con.cursor_height; l++) {
		d -= pw;
		for (int i = 0; i < font_width; i++) d[i] ^= 7;
	}
}

/* a part of the scroll bar, its pattern repeated down to limit */
static void vpattern(const u8 *img, int h, int y, int limit, int x)
{
	while (y < limit) {
		int n = MIN(h, limit - y);
		kos_put_image(img, vscroll_width, n, x, y);
		y += h;
	}
}

static void con_draw_image(void)
{
	if (!con.init_cmd || con.init_cmd == 2) return;
	con_data2image();
	con.data_width = con.wnd_width * font_width;
	con.data_height = con.wnd_height * font_height;
	put_image_pal(con.image, con.data_width, con.data_height, 0, 0, 8, con_colors, 0);
	/* what the text does not cover (a maximised window) */
	int right = con.data_width + (with_scrollbar() ? vscroll_width : 0);
	if (con.client_w > right) kos_rect(right, 0, con.client_w - right, con.client_h, 0);
	if (con.client_h > con.data_height) kos_rect(0, con.data_height, right, con.client_h - con.data_height, 0);
	if (with_scrollbar()) {
		int x = con.data_width, h = con.data_height;
		kos_put_image(con.bUpPressed ? vscroll_btn3 : vscroll_btn1, vscroll_width, vscroll_btn_height, x, 0);
		kos_put_image(con.bDownPressed ? vscroll_btn4 : vscroll_btn2, vscroll_width, vscroll_btn_height, x,
			      h - vscroll_btn_height);
		/* the slider's height and position */
		int span = h - 2 * vscroll_btn_height;
		int size = span * con.wnd_height / con.scr_height;
		if (size < 5) size = 5;
		con.vscrollbar_size = size;
		int range = con.scr_height - con.wnd_height;
		con.vscrollbar_pos = vscroll_btn_height + (range ? (span - size) * con.wnd_ypos / range : 0);
		vpattern(con.bScrollingUp ? vscroll_bgr2 : vscroll_bgr1, vscroll_bgr_height, vscroll_btn_height,
			 con.vscrollbar_pos, x);
		vpattern(con.bScrollingDown ? vscroll_bgr2 : vscroll_bgr1, vscroll_bgr_height,
			 con.vscrollbar_pos + size, h - vscroll_btn_height, x);
		int y = con.vscrollbar_pos;
		kos_put_image(vscroll_bar1, vscroll_width, vscroll_bar_height1, x, y);
		vpattern(vscroll_bar2, vscroll_bar_height2, y + vscroll_bar_height1, y + size - vscroll_bar_height3, x);
		kos_put_image(vscroll_bar3, vscroll_width, vscroll_bar_height3, x, y + size - vscroll_bar_height3);
	}
	con.drawn = 1;
	con.dirty = 0;
	con.last_draw = kos_uptime();
}

/* the picture's memory for wnd_width x wnd_height characters */
static int con_image_alloc(void)
{
	u32 need = con.wnd_width * con.wnd_height * font_width * font_height;
	if (need <= con.image_cap) return 0;
	u8 *p = realloc(con.image, need);
	if (!p) return -1;
	con.image = p;
	con.image_cap = need;
	return 0;
}

/* a maximised window (the Hildon look): as many lines as fit */
static void con_fit(void)
{
	u8 info[1024];
	kos_thread_info(info, -1);
	int cw = *(u32 *)(info + 62) + 1, ch = *(u32 *)(info + 66) + 1;
	con.client_w = cw;
	con.client_h = ch;
	if (kos_skin_height() != 0) return;   /* the classic look: the window as made */
	u32 rows = ch / font_height, cols = (cw - vscroll_width) / font_width;
	if (rows < 1) rows = 1;
	if (rows > con.scr_height) rows = con.scr_height;
	if (cols < 1) cols = 1;
	if (cols > con.scr_width) cols = con.scr_width;
	u32 ow = con.wnd_width, oh = con.wnd_height;
	con.wnd_width = cols;
	con.wnd_height = rows;
	if (con_image_alloc()) { con.wnd_width = ow; con.wnd_height = oh; return; }
	/* the cursor stays in view */
	if (con.cur_y >= con.wnd_ypos + con.wnd_height) con.wnd_ypos = con.cur_y - con.wnd_height + 1;
	if (con.wnd_ypos + con.wnd_height > con.scr_height) con.wnd_ypos = con.scr_height - con.wnd_height;
}

static void con_draw_window(void)
{
	kos_begin_draw();
	con.skin_h = kos_skin_height();
	int w = con.wnd_width * font_width + 5 + 5 - 1;
	int h = con.wnd_height * font_height + con.skin_h + 5 - 1;
	if (with_scrollbar()) w += vscroll_width;
	kos_call(0, (u32)def_wnd_x << 16 | w, (u32)def_wnd_y << 16 | h, 0x74000000, 0, (u32)con.title);
	con_fit();
	u8 info[1024];
	kos_thread_info(info, -1);
	if (!(info[70] & 6)) con_draw_image();  /* not rolled up or minimised */
	kos_end_draw();
}

/* --- the buffer --------------------------------------------------------- */

static u8 *con_get_data_ptr(void)
{
	return con.data + (con.cur_y * con.scr_width + con.cur_x) * 2;
}

static void con_clear(u8 *p, u32 cells)
{
	for (u32 i = 0; i < cells; i++) p[2 * i] = ' ', p[2 * i + 1] = 7;
}

/* the lines top..bot (inclusive) up by one, an empty one at bot */
static void con_scroll_lines_up(u32 top, u32 bot)
{
	u32 w = con.scr_width * 2;
	memmove(con.data + top * w, con.data + (top + 1) * w, (bot - top) * w);
	con_clear(con.data + bot * w, con.scr_width);
}

static void con_scroll_lines_down(u32 top, u32 bot)
{
	u32 w = con.scr_width * 2;
	memmove(con.data + (top + 1) * w, con.data + top * w, (bot - top) * w);
	con_clear(con.data + top * w, con.scr_width);
}

static void con_scr_scroll_up(void)
{
	con.cur_y--;
	con_scroll_lines_up(0, con.scr_height - 1);
}

static void con_newline(void)
{
	if (con.scroll_bot && con.cur_y == con.scroll_bot) {
		con_scroll_lines_up(con.scroll_top, con.scroll_bot);
		return;
	}
	if (++con.cur_y >= con.scr_height) con_scr_scroll_up();
}

static void con_write_char(u8 c)
{
	if (con.cur_x >= con.scr_width) {
		con.cur_x = 0;
		con_newline();
	}
	u8 *p = con_get_data_ptr();
	p[0] = c;
	p[1] = con_flags;
	con.cur_x++;
}

static void con_bell(void) {}

static u32 arg(int i, u32 def)
{
	return (u32)i < con_esc_attr_n && con_esc_attrs[i] ? con_esc_attrs[i] : def;
}

/* Esc[...m: the attributes (.set_attr) */
static void con_set_attr(void)
{
	static const u8 fg[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
	for (u32 i = 0; i < con_esc_attr_n; i++) {
		u32 a = con_esc_attrs[i];
		int color = -1, bgr = -1;
		if (a == 0) { con_flags_attr = 0; con_flags = (con_flags & ~0xffu) | 7; continue; }
		if (a == 1) { con_flags_attr |= 8; continue; }
		if (a == 5) { con_flags_attr |= 0x80; continue; }
		if (a == 7) { con_flags = (con_flags & ~0xffu) | 0x70; continue; }
		if (a == 27) { con_flags = (con_flags & ~0xffu) | 7; continue; }
		if (a >= 30 && a <= 37) color = fg[a - 30];
		else if (a == 39) color = 7;
		else if (a >= 40 && a <= 47) bgr = fg[a - 40] << 4;
		else if (a == 49) bgr = 0;
		else if (a >= 90 && a <= 97) color = fg[a - 90] + 8;
		else if (a >= 100 && a <= 107) bgr = (fg[a - 100] << 4) + 0x80;
		if (color >= 0) con_flags = (con_flags & ~0xfu) | ((con_flags_attr | color) & 0xff & ~0xf0u);
		if (bgr >= 0) con_flags = (con_flags & ~0xf0u) | ((con_flags_attr | bgr) & 0xf0);
	}
}

static void con_set_title_now(const char *t)
{
	strncpy(con.title, t, sizeof(con.title) - 1);
	con_thread_op = OP_SET_TITLE;
	con_wake();
}

/* Esc[?1049h / l: the alternative buffer (no lines kept) */
static void con_altbuff(int on)
{
	if (on && con.data != con.altbuffer) {
		con.saved_x = con.cur_x;
		con.saved_y = con.cur_y;
		con.data = con.altbuffer;
		con.scr_width = con.wnd_width;
		con.scr_height = con.wnd_height;
		con_clear(con.data, con.scr_width * con.scr_height);
		con.cur_x = con.cur_y = con.wnd_ypos = 0;
	} else if (!on && con.data == con.altbuffer) {
		con.data = con.mainbuffer;
		con.scr_width = con.main_scr_width;
		con.scr_height = con.main_scr_height;
		con.cur_x = con.saved_x;
		con.cur_y = con.saved_y;
	}
	con.scroll_top = con.scroll_bot = 0;
}

/* the end of a CSI sequence (.not_digit) */
static void con_csi(u8 c)
{
	con_esc = con_sci = 0;
	u32 n = arg(0, 1);
	switch (c) {
	case 'A': con.cur_y = con.cur_y > n ? con.cur_y - n : 0; break;
	case 'B': con.cur_y = MIN(con.cur_y + n, con.scr_height - 1); break;
	case 'C': con.cur_x = MIN(con.cur_x + n, con.scr_width - 1); break;
	case 'D': con.cur_x = con.cur_x > n ? con.cur_x - n : 0; break;
	case 'H': case 'f': {
		u32 y = arg(0, 1) - 1, x = arg(1, 1) - 1;
		if (y < con.scr_height) con.cur_y = y;
		if (x < con.scr_width) con.cur_x = x;
		break;
	}
	case 'd': if (n - 1 < con.scr_height) con.cur_y = n - 1; break;
	case 'J': {
		u32 m = con_esc_attr_n ? con_esc_attrs[0] : 0;
		u8 *p = con_get_data_ptr();
		u32 at = con.cur_y * con.scr_width + con.cur_x, all = con.scr_width * con.scr_height;
		if (m == 0) con_clear(p, all - at);
		else if (m == 1) con_clear(con.data, at + 1);
		else if (m == 2) { con_clear(con.data, all); con.cur_x = con.cur_y = 0; con.wnd_ypos = 0; }
		break;
	}
	case 'K': {
		u32 m = con_esc_attr_n ? con_esc_attrs[0] : 0;
		u8 *line = con.data + con.cur_y * con.scr_width * 2;
		if (m == 0) con_clear(line + con.cur_x * 2, con.scr_width - con.cur_x);
		else if (m == 1) con_clear(line, con.cur_x + 1);
		else con_clear(line, con.scr_width);
		break;
	}
	case 'L': case 'M': {
		u32 bot = con.scroll_bot ? con.scroll_bot : con.scr_height - 1;
		if (con.cur_y > bot) break;
		for (u32 i = 0; i < n; i++) {
			if (c == 'L') con_scroll_lines_down(con.cur_y, bot);
			else con_scroll_lines_up(con.cur_y, bot);
		}
		break;
	}
	case 'P': {
		u8 *p = con_get_data_ptr();
		u32 left = con.scr_width - con.cur_x;
		n = MIN(n, left);
		memmove(p, p + n * 2, (left - n) * 2);
		con_clear(p + (left - n) * 2, n);
		break;
	}
	case 'X': con_clear(con_get_data_ptr(), MIN(n, con.scr_width - con.cur_x)); break;
	case 'm': con_set_attr(); break;
	case 'r': {
		u32 t = arg(0, 1) - 1, b = arg(1, con.wnd_height) - 1;
		if (t < b && b < con.scr_height) con.scroll_top = t, con.scroll_bot = b;
		else con.scroll_top = con.scroll_bot = 0;
		break;
	}
	case 'h': case 'l': {
		int on = c == 'h';
		if (!con_esc_private) break;
		for (u32 i = 0; i < con_esc_attr_n; i++) {
			if (con_esc_attrs[i] == 1) con.app_cursor_keys = on;
			else if (con_esc_attrs[i] == 25) con.cursor_visible = on;
			else if (con_esc_attrs[i] == 1049) con_altbuff(on);
		}
		break;
	}
	}
	if (con.cur_y >= con.scr_height) con.cur_y = con.scr_height - 1;
}

static void con_write_special_char(u8 c)
{
	if (con_esc) {
		if (con_sci == 3) {                    /* an OSC string */
			if (c == 7 || c == 0x9c) {
				con_osc_str[con_osc_strlen] = 0;
				con_esc = con_sci = 0;
				if (con_esc_attrs[0] == 0 || con_esc_attrs[0] == 2) con_set_title_now(con_osc_str);
			} else if (con_osc_strlen < 255) {
				con_osc_str[con_osc_strlen++] = c;
			}
			return;
		}
		if (con_sci == 2) {                    /* Esc]<number>; then the string */
			if (c >= '0' && c <= '9') { con_esc_attrs[0] = con_esc_attrs[0] * 10 + c - '0'; return; }
			con_sci = 3;
			con_osc_strlen = 0;
			if (c != ';') con_write_special_char(c);
			return;
		}
		if (con_sci == 4) { con_esc = con_sci = 0; return; }   /* Esc( G0: ignored */
		if (con_sci == 1) {
			if (c == '?') { con_esc_private = 1; return; }
			if (c == ';') {
				if (con_esc_attr_n < 4) con_esc_attrs[con_esc_attr_n++] = 0;
				return;
			}
			if (c >= '0' && c <= '9') {
				u32 *a = &con_esc_attrs[con_esc_attr_n - 1];
				*a = *a * 10 + c - '0';
				return;
			}
			con_csi(c);
			return;
		}
		switch (c) {
		case '[': con_sci = 1; return;
		case ']': con_sci = 2; con_esc_attrs[0] = 0; return;
		case '(': con_sci = 4; return;
		case '>': case '=': con_esc = 0; return;
		case 'G': con_esc = 0; con_bell(); return;
		case 'H': con_esc = 0; c = 8; break;
		case 'I': con_esc = 0; c = 9; break;
		case 'J': con_esc = 0; c = 10; break;
		case 'M': con_esc = 0; c = 13; break;
		default:
			con_esc = 0;
			con_write_char(27);
			con_write_char(c);
			return;
		}
	}
	switch (c) {
	case 10:
		con.cur_x = 0;
		con_newline();
		return;
	case 13:
		con.cur_x = 0;
		return;
	case 27:
		con_esc = 1;
		con_sci = 0;
		con_esc_private = 0;
		con_esc_attr_n = 1;
		memset(con_esc_attrs, 0, sizeof(con_esc_attrs));
		return;
	case 8:
		if (con.cur_x) con.cur_x--;
		else if (con.cur_y) con.cur_y--, con.cur_x = con.scr_width - 1;
		return;
	case 7:
		con_bell();
		return;
	case 9:
		do con_write_char(' ');
		while (con.cur_x & 7);
		return;
	}
	con_write_char(c);
}

static void con_write_char_ex(u8 c)
{
	if (con_flags & CON_IGNORE_SPECIALS) con_write_char(c);
	else con_write_special_char(c);
}

/* the cursor into view; drawn now and then while writing, always before
 * waiting (con.update_screen) */
/* con.update_screen: the cursor in view, the thread asked to draw */
static void con_update_screen(void)
{
	if (con.cur_y < con.wnd_ypos) con.wnd_ypos = con.cur_y;
	else if (con.cur_y >= con.wnd_ypos + con.wnd_height) con.wnd_ypos = con.cur_y - con.wnd_height + 1;
	con_thread_op = OP_REDRAW;
	con_wake();
}

static void con_write(const char *s, int len)
{
	if (!con.init_cmd) return;
	for (; len < 0 ? *s : len > 0; s++, len -= len > 0)
		con_write_char_ex(*s);
	con_update_screen();
}

/* --- init and exit -------------------------------------------------------- */

int con_init_opt(dword wnd_width, dword wnd_height, dword scr_width, dword scr_height, const char *title)
{
	if (con.init_cmd) return 0;
	con.wnd_width = wnd_width == (dword)-1 ? def_wnd_width : wnd_width;
	con.wnd_height = wnd_height == (dword)-1 ? def_wnd_height : wnd_height;
	con.main_scr_width = scr_width == (dword)-1 ? def_scr_width : scr_width;
	con.main_scr_height = scr_height == (dword)-1 ? def_scr_height : scr_height;
	if (con.main_scr_width < con.wnd_width) con.main_scr_width = con.wnd_width;
	if (con.main_scr_height < con.wnd_height) con.main_scr_height = con.wnd_height;
	kos_call(68, 11, 0, 0, 0, 0);
	u32 cells = con.main_scr_width * con.main_scr_height;
	con.mainbuffer = malloc(cells * 2);
	/* the alternative buffer: as large as the window may get */
	con.altbuffer = malloc(con.main_scr_width * con.main_scr_height * 2);
	if (!con.mainbuffer || !con.altbuffer || con_image_alloc()) {
		kos_debug_char('!');
		kdebug("console: Not enough memory!\n");
		exit(1);
	}
	con.data = con.mainbuffer;
	con.scr_width = con.main_scr_width;
	con.scr_height = con.main_scr_height;
	con_clear(con.data, cells);
	con.cursor_height = (15 * font_height + 50) / 100;
	strncpy(con.title, title ? title : "Console", sizeof(con.title) - 1);
	con.init_cmd = 1;
	/* the thread, the window's: the caller is the one its close ends */
	u8 info[1024];
	kos_thread_info(info, -1);
	con_parent_tid = *(u32 *)(info + 30);
	int tid = kos_thread_create(con_thread, con_stack + sizeof(con_stack) / sizeof(con_stack[0]));
	if (tid < 0) {
		kdebug("Cannot create console thread!\n");
		exit(1);
	}
	con_console_tid = tid;
	return 0;
}

/* START, console.obj's entry for its C callers (con_start(1) after
 * loading it): there is nothing to set up before con_init */
int con_start(dword state)
{
	(void)state;
	return 1;
}

int con_init(void)
{
	return con_init_opt(-1, -1, -1, -1, "Console");
}

/* con_exit: the window closed with it (1), or left with "[Finished]" in
 * its title (0), its thread then living on after the program's end until
 * the window is closed */
void con_exit(int bCloseWindow)
{
	if (!con.init_cmd) return;
	if (bCloseWindow) {
		con_thread_op = OP_EXIT;
		con_wake();
		return;
	}
	strncat(con.title, " [Finished]", sizeof(con.title) - strlen(con.title) - 1);
	con.cursor_height = 0;
	con_thread_op = OP_SET_TITLE;
	con_wake();
	con_thread_op = OP_REDRAW;
	con_wake();
}

void con_set_title(const char *title)
{
	if (!con.init_cmd) return;
	con_set_title_now(title);
}

/* --- output ------------------------------------------------------------- */

void con_write_asciiz(const char *str) { con_write(str, -1); }
void con_write_string(const char *str, dword length) { con_write(str, length); }

int con_printf(const char *format, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf)) {
		char *big = malloc(n + 1);
		if (big) {
			va_start(ap, format);
			vsnprintf(big, n + 1, format, ap);
			va_end(ap);
			con_write(big, n);
			free(big);
			return n;
		}
		n = sizeof(buf) - 1;
	}
	con_write(buf, n);
	return n;
}

dword con_get_flags(void) { return con_flags; }

dword con_set_flags(dword new_flags)
{
	dword old = con_flags;
	con_flags = (con_flags & CON_WINDOW_CLOSED) | (new_flags & 0x1ff);
	return old;
}

int con_get_font_height(void) { return font_height; }
int con_get_cursor_height(void) { return con.cursor_height; }

int con_set_cursor_height(int new_height)
{
	int old = con.cursor_height;
	if (new_height >= 0 && new_height < font_height) con.cursor_height = new_height;
	if (con.init_cmd) con_update_screen();
	return old;
}

void con_cls(void)
{
	if (!con.init_cmd) return;
	con_clear(con.data, con.scr_width * con.scr_height);
	con.cur_x = con.cur_y = con.wnd_ypos = 0;
	con_update_screen();
}

void con_get_cursor_pos(int *px, int *py)
{
	*px = con.cur_x;
	*py = con.cur_y;
}

void con_set_cursor_pos(int x, int y)
{
	if ((u32)x < con.scr_width) con.cur_x = x;
	if ((u32)y < con.scr_height) con.cur_y = y;
	con_update_screen();
}

/* --- input ------------------------------------------------------------- */

/* scan codes with an ASCII code (scan_has_ascii) */
static const u32 scan_has_ascii[4] = { 0xdffffffe, 0x023ffbff, 0, 0 };
static const u8 extended_alt[128] = {
	0x00, 0x01, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x0E, 0xA5,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x00, 0x1E, 0x1F,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x00, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x00, 0x37, 0x00, 0x39, 0x00, 0x68, 0x69, 0x6A, 0x6B, 0x6C,
	0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x00, 0x00, 0x97, 0x98, 0x99, 0x4A, 0x9B, 0x9C, 0x9D, 0x4E, 0x9F,
	0xA0, 0xA1, 0xA2, 0xA3, 0x00, 0x00, 0x00, 0x8B, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static u8 extended_ctrl[128], extended_shift[128];
static const char extended_numlock[13] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

static void tables(void)
{
	static int made;
	if (made) return;
	made = 1;
	static const u8 ctrl_3b[30] = { 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x00, 0x00,
		0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74, 0x90, 0x75, 0x91, 0x76, 0x92, 0x93, 0x00, 0x00, 0x00, 0x89, 0x8A };
	static const u8 shift_3b[30] = { 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x00, 0x00,
		0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x00, 0x00, 0x00, 0x87, 0x88 };
	for (int i = 0; i < 0x3b; i++) extended_ctrl[i] = extended_shift[i] = i;
	extended_ctrl[0x0f] = 0x94;
	for (int i = 0; i < 30; i++) extended_ctrl[0x3b + i] = ctrl_3b[i], extended_shift[0x3b + i] = shift_3b[i];
}

static void con_put_input(u16 code)
{
	int n = (con.input_end + 1) % 128;
	if (n == con.input_start) return;        /* full: KolibriOS beeps */
	con.input[con.input_end] = code;
	con.input_end = n;
}

/* a scan code from function 2 into KolibriOS's 16-bit key code, ASCII
 * low, the extended code high (con.key) */
static void con_key(u32 eax)
{
	u8 sc = (eax >> 8) & 0xff;
	if (sc == 0xE0) { con.bWasE0 = 1; return; }
	int e0 = con.bWasE0;
	con.bWasE0 = 0;
	if (!sc || sc >= 0x80) return;           /* releases */
	if (sc == 0x1D || sc == 0x2A || sc == 0x36 || sc == 0x38 || sc == 0x3A || sc == 0x45 || sc == 0x46) return;
	tables();
	u32 mods = kos_call(66, 3, 0, 0, 0, 0);
	u8 dl = sc, dh;
	u16 code;
	if ((sc == 0x4e || sc == 0x4a) && !e0) {
		code = extended_numlock[sc - 71] | (u16)sc << 8;
		goto got;
	}
	if (!e0 && (mods & 0x80) && sc >= 71 && sc <= 83) {
		code = extended_numlock[sc - 71] | (u16)sc << 8;
		goto got;
	}
	if (!e0 && (scan_has_ascii[sc >> 5] >> (sc & 31) & 1) && !(mods & 0x30)) {
		kos_call(26, 2, mods & 3 ? 2 : 1, (u32)con.kbd_layout, 0, 0);
		dh = con.kbd_layout[sc];
		if (mods & 0xc) dh -= 0x60;
	} else {
		dh = 0;                            /* no ASCII code */
	}
	/* dh: the ASCII code; the scan code to the extended key code */
	if (mods & 0x30) dl = extended_alt[sc];
	else if (mods & 3) dl = extended_shift[sc];
	else if (mods & 0xc) dl = extended_ctrl[sc];
	else if (sc == 28) { code = 13 | 28 << 8; goto got; }
	else if (sc == 53 && e0) { code = '/' | 53 << 8; goto got; }
	else if (sc == 55) { code = '*' | 55 << 8; goto got; }
	else if (sc == 0x57 || sc == 0x58) dl = sc + 0x85 - 0x57;   /* F11, F12 */
	if (!dl) return;
	if (dl == 0x94) dh = 0;
	code = dh | (u16)dl << 8;
got:
	con_put_input(code);
}

static void con_mouse(void)
{
	s16 wheel = (s16)kos_call(37, 7, 0, 0, 0, 0);
	int range = (int)con.scr_height - (int)con.wnd_height;
	if (wheel) {
		int y = (int)con.wnd_ypos + wheel;
		con.wnd_ypos = y < 0 ? 0 : y > range ? range : y;
		con.dirty = 1;
		return;
	}
	u32 buttons = kos_mouse_buttons();
	int was_pressed = con.bUpPressed | con.bDownPressed | con.bScrollingUp | con.bScrollingDown;
	if (!(buttons & 1)) {
		con.vscroll_pt = -1;
		con.bUpPressed = con.bDownPressed = con.bScrollingUp = con.bScrollingDown = 0;
		if (was_pressed) con.dirty = 1;
		return;
	}
	u32 pos = kos_call(37, 1, 0, 0, 0, 0);    /* relative to the client area */
	int mx = (s16)(pos >> 16), my = (s16)pos;
	if (con.vscroll_pt >= 0) {             /* dragging the slider */
		int span = con.data_height - 2 * vscroll_btn_height - con.vscrollbar_size;
		int y = my - con.vscroll_pt - vscroll_btn_height;
		if (y < 0) y = 0;
		if (y > span) y = span;
		con.wnd_ypos = span > 0 ? y * range / span : 0;
		con.dirty = 1;
		return;
	}
	if (my < 0 || mx < con.data_width || mx >= con.data_width + vscroll_width || !with_scrollbar()) return;
	/* held: once, then again after 0.25 s (the original's first_time) */
	u32 t = kos_uptime();
	if (was_pressed && t - con.press_time < 25) return;
	if (!was_pressed) con.press_time = t;
	if (my < vscroll_btn_height) {
		con.bUpPressed = 1;
		if (con.wnd_ypos) con.wnd_ypos--;
	} else if (my >= con.data_height - vscroll_btn_height) {
		if (my >= con.data_height) return;
		con.bDownPressed = 1;
		if ((int)con.wnd_ypos < range) con.wnd_ypos++;
	} else if (my < con.vscrollbar_pos) {
		con.bScrollingUp = 1;
		con.wnd_ypos = con.wnd_ypos + 1 > con.wnd_height ? con.wnd_ypos + 1 - con.wnd_height : 0;
	} else if (my >= con.vscrollbar_pos + con.vscrollbar_size) {
		con.bScrollingDown = 1;
		int y = con.wnd_ypos + con.wnd_height - 1;
		con.wnd_ypos = y > range ? range : y;
	} else {
		con.vscroll_pt = my - con.vscrollbar_pos;
		return;
	}
	con.dirty = 1;
}

/* con.wake: the operation in con_thread_op to the thread, unless that one
 * is waiting there already; again after 5 1 while its buffer is full */
static void con_wake(void)
{
	if (!con_console_tid || con_thread_op == con_ipc_buf[0x10]) return;
	u8 op = con_thread_op;
	while (kos_ipc_send(con_console_tid, &op, 1)) {
		if (!con_console_tid) return;
		kos_delay(1);
	}
}

/* con.thread_exit: the program's calls find the window closed */
static void con_thread_exit(void)
{
	con_flags |= CON_WINDOW_CLOSED;
	con_console_tid = 0;
	kos_exit();
}

/* con.thread: the window's events and the program's wake-ups; drawn at
 * most every 20 ms, the rest of a burst of output after it */
static void con_thread(void)
{
	kos_event_mask(0x80000067);    /* redraw, key, button, mouse while active, IPC */
	kos_ipc_set_area((void *)con_ipc_buf, 0x11);
	kos_call(66, 1, 1, 0, 0, 0);   /* scan codes */
	con_draw_window();
	for (;;) {
		int held = con.bUpPressed | con.bDownPressed | con.bScrollingUp | con.bScrollingDown | (con.vscroll_pt >= 0);
		int ev = held ? kos_wait_event_timeout(5) : con.dirty ? kos_wait_event_timeout(2) : kos_wait_event();
		switch (ev) {
		case 1:
			con_draw_window();
			break;
		case 2:
			con_key(kos_get_key());
			break;
		case 3:
			if ((kos_get_button() >> 8) == 1) {
				/* we have only one button, close: the program's thread first */
				kos_kill_pid(con_parent_tid);
				con_thread_exit();
			}
			break;
		case 6:
			con_mouse();
			break;
		case 7: {                          /* con.ipc */
			u8 op = con_ipc_buf[0x10];
			*(volatile u32 *)(con_ipc_buf + 4) = 8;
			con_ipc_buf[0x10] = 0;
			if (op == OP_EXIT) con_thread_exit();
			if (op == OP_SET_TITLE) kos_set_caption(con.title);
			if (op == OP_REDRAW || op == OP_SET_TITLE) con.dirty = 1;
			/* OP_GETCH: the keys are in the input buffer already */
			break;
		}
		case 0:
			if (held) con_mouse();
			break;
		}
		if (con.dirty && kos_uptime() - con.last_draw >= 2) {
			kos_begin_draw();
			con_draw_image();
			kos_end_draw();
		}
	}
}

int con_kbhit(void)
{
	if (con_flags & CON_WINDOW_CLOSED) return 1;
	return con.input_start != *(volatile int *)&con.input_end;
}

/* con.force_entered_char: a key from the input buffer, waiting for one
 * as console.obj does (5 2); 0 when the window was closed */
static u16 con_take(void)
{
	while (con.input_start == *(volatile int *)&con.input_end) {
		if (con_flags & CON_WINDOW_CLOSED) return 0;
		kos_delay(2);
	}
	u16 c = con.input[con.input_start];
	con.input_start = (con.input_start + 1) % 128;
	return c;
}

static int con_entered_char = -1;     /* con_getch's second half of a key */

int con_getch(void)
{
	if (!con.init_cmd) con_init();
	if (con_flags & CON_WINDOW_CLOSED) return 0;
	if (con_entered_char >= 0) {
		int c = con_entered_char;
		con_entered_char = -1;
		return c;
	}
	u16 k = con_take();
	if (!(k & 0xff)) {                     /* extended: 0, then the code */
		con_entered_char = k >> 8;
		return 0;
	}
	return k & 0xff;
}

word con_getch2(void)
{
	if (!con.init_cmd) con_init();
	if (con_flags & CON_WINDOW_CLOSED) return 0;
	return con_take();
}

/* the line editing of con_gets2: the text after the cursor redrawn */
static void con_redraw_tail(char *s)
{
	u32 x = con.cur_x, y = con.cur_y;
	for (; *s; s++) con_write_char_ex(*s);
	con_write_char_ex(' ');
	con.cur_x = x;
	con.cur_y = y;
}

static void con_cursor_back(int n)
{
	while (n--) {
		if (con.cur_x) con.cur_x--;
		else if (con.cur_y) con.cur_y--, con.cur_x = con.scr_width - 1;
	}
}

static void con_cursor_fwd(int n)
{
	while (n--) {
		if (++con.cur_x >= con.scr_width) {
			con.cur_x = 0;
			con_newline();
		}
	}
}

char *con_gets2(con_gets2_callback callback, char *str, int n)
{
	if (!con.init_cmd) con_init();
	if (n <= 1) return str;
	str[0] = 0;
	int len = 0, pos = 0;
	for (;;) {
		con_update_screen();
		u16 k = con_getch2();
		if (con_flags & CON_WINDOW_CLOSED) return NULL;
		u8 c = k & 0xff, x = k >> 8;
		if (c == 13) {                     /* Enter: the newline kept */
			con_cursor_fwd(len - pos);
			if (len < n - 1) str[len++] = '\n';
			str[len] = 0;
			con.cur_x = 0;
			con_newline();
			con_update_screen();
			return str;
		}
		if (c == 8) {
			if (!pos) continue;
			memmove(str + pos - 1, str + pos, len - pos + 1);
			pos--, len--;
			con_cursor_back(1);
			con_redraw_tail(str + pos);
			continue;
		}
		if (c == 27) {                     /* Esc: the line cleared */
			con_cursor_back(pos);
			for (int i = 0; i < len; i++) con_write_char_ex(' ');
			con_cursor_back(len);
			len = pos = 0;
			str[0] = 0;
			continue;
		}
		if (c == 9) c = 0, x = 0x0f;       /* Tab: to the callback */
		if (c) {
			if (len >= n - 1) continue;
			memmove(str + pos + 1, str + pos, len - pos + 1);
			str[pos] = c;
			len++;
			con_write_char_ex(c);
			pos++;
			con_redraw_tail(str + pos);
			continue;
		}
		switch (x) {
		case 0x4b: if (pos) pos--, con_cursor_back(1); continue;              /* left */
		case 0x4d: if (pos < len) pos++, con_cursor_fwd(1); continue;         /* right */
		case 0x47: con_cursor_back(pos); pos = 0; continue;                   /* home */
		case 0x4f: con_cursor_fwd(len - pos); pos = len; continue;            /* end */
		case 0x53:                                                            /* delete */
			if (pos < len) {
				memmove(str + pos, str + pos + 1, len - pos);
				len--;
				con_redraw_tail(str + pos);
			}
			continue;
		}
		if (!callback) continue;
		/* the callback: the cursor at the line's end first */
		con_cursor_fwd(len - pos);
		int oldlen = len;
		pos = len;
		int r = callback(x, &str, &n, &pos);
		len = strlen(str);
		if (r == 0) {                      /* not changed */
			con_cursor_back(len - pos);
		} else if (r == 1 || r == 2) {     /* changed: the old line away, the new one shown */
			con_cursor_back(oldlen);
			for (int i = 0; i < oldlen; i++) con_write_char_ex(' ');
			con_cursor_back(oldlen);
			for (int i = 0; i < len; i++) con_write_char_ex(str[i]);
			if (pos > len) pos = len;
			con_cursor_back(len - pos);
		} else {
			return str;                    /* 3: back at once */
		}
	}
}

char *con_gets(char *str, int n)
{
	return con_gets2(NULL, str, n);
}

/* the keys as bytes: ASCII, or Esc sequences for the others */
int con_get_input(char *buf, int buflen)
{
	if (!con.init_cmd) con_init();
	int n = 0;
	while (n < buflen && con.input_start != *(volatile int *)&con.input_end) {
		u16 k = con_take();
		u8 c = k & 0xff, x = k >> 8;
		if (c) { buf[n++] = c; continue; }
		const char *seq = NULL;
		switch (x) {
		case 0x48: seq = con.app_cursor_keys ? "\033OA" : "\033[A"; break;
		case 0x50: seq = con.app_cursor_keys ? "\033OB" : "\033[B"; break;
		case 0x4d: seq = con.app_cursor_keys ? "\033OC" : "\033[C"; break;
		case 0x4b: seq = con.app_cursor_keys ? "\033OD" : "\033[D"; break;
		case 0x47: seq = "\033[H"; break;
		case 0x4f: seq = "\033[F"; break;
		case 0x49: seq = "\033[5~"; break;
		case 0x51: seq = "\033[6~"; break;
		case 0x52: seq = "\033[2~"; break;
		case 0x53: seq = "\033[3~"; break;
		}
		for (; seq && *seq && n < buflen; seq++) buf[n++] = *seq;
	}
	return n;
}
