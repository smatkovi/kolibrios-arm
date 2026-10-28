/* KolibriOS for ARM: the screen, drawing and the window manager */
#ifndef GUI_H
#define GUI_H

#include "task.h"

struct screen {
	u32 *fb;          /* 32 bpp 0x00RRGGBB */
	int w, h, stride; /* stride in pixels */
	u8 *owner;        /* w * h: the slot whose window shows the pixel, 0 = desktop */
	int dirty_y0, dirty_y1;
};
extern struct screen scr;

#define SKIN_H 22         /* the skinned windows' title height (function 48.4) */
#define FRAME_W 5         /* outer 1 + frame 3 + inner 1 */

/* gfx.c: primitives on the screen, only where slot owns the pixel
 * (slot -1: everywhere, for the mouse pointer and the boot screen) */
void gfx_init(void);
void gfx_pixel(int x, int y, u32 c, int slot);
void gfx_rect(int x, int y, int w, int h, u32 c, int slot);
void gfx_hgradient(int x, int y, int w, int h, u32 c, int slot);
void gfx_line(int x0, int y0, int x1, int y1, u32 c, int slot);
/* text: font 0 6x10, font 1 8x16; scale 1..8; returns the width drawn */
int gfx_text(int x, int y, const char *s, int len, u32 c, int font, int scale,
	     bool fill, u32 bg, int slot, bool utf8);
int gfx_font_w(int font);
int gfx_font_h(int font);
void gfx_dirty(int y0, int y1);
void gfx_flush(void);           /* the timer calls it */
/* the mouse pointer, drawn over everything */
void cursor_move(int x, int y);
void cursor_hide(void);
void cursor_show(void);

/* wm.c */
void wm_init(void);
void wm_define_window(struct task *t, u32 ebx, u32 ecx, u32 edx, u32 esi, const char *title);
void wm_draw_frame(struct task *t);
void wm_remove(struct task *t);           /* the task ends */
void wm_activate(struct task *t);
void wm_minimize(struct task *t);
void wm_move(struct task *t, int x, int y, int w, int h);
struct task *wm_active(void);
int wm_stack_pos(struct task *t);         /* 0 = bottom */
int wm_stack_count(void);
int wm_slot_at_pos(int pos);
void wm_redraw_background(int x, int y, int w, int h);
void wm_mouse(int x, int y, int buttons, int changed);
void wm_key(u32 value);                   /* to the active window */
void wm_button_draw(struct task *t, struct button *b, bool pressed);
/* the window's origin for drawing (relative to the frame or the work area) */
void wm_origin(struct task *t, int *ox, int *oy);
extern int mouse_x, mouse_y, mouse_buttons;

#endif
