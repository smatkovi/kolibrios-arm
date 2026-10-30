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

/* The look (function 48.8, by the skin's file name): KolibriOS's skin, or
 * Hildon's (Maemo 5): the windows without frame or title, maximised in
 * the work area (function 48.6), one in front; the shell (apps/hildon)
 * draws the title, the buttons and the status area in its panel. */
enum { LOOK_CLASSIC, LOOK_HILDON };
extern int wm_look;
/* the work area (48.5, 48.6), inclusive */
extern int work_x0, work_y0, work_x1, work_y1;
/* window stack layers (function 18.25), KolibriOS's numbers */
enum { ZPOS_ALWAYS_BACK = -1, ZPOS_NORMAL = 0, ZPOS_ALWAYS_TOP = 1 };

/* gfx.c: primitives on the screen, only where slot owns the pixel
 * (slot -1: everywhere, for the mouse pointer and the boot screen) */
void gfx_init(void);
void gfx_pixel(int x, int y, u32 c, int slot);
void gfx_rect(int x, int y, int w, int h, u32 c, int slot);
void gfx_hgradient(int x, int y, int w, int h, u32 c, int slot);
void gfx_line(int x0, int y0, int x1, int y1, u32 c, int slot);
/* functions 7 and 36: an image of 3 bytes a pixel (B, G, R) */
void gfx_image(int x, int y, int w, int h, const u8 *bgr, int slot);
void gfx_read(u8 *bgr, int x, int y, int w, int h);
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
int wm_skin_h(const struct window *w);    /* its title height, its frame width */
bool wm_maximised(const struct window *w); /* managed by the Hildon look */
int wm_frame_w(const struct window *w);
int wm_set_look(int look);
void wm_set_work_area(int x0, int x1, int y0, int y1);
void wm_set_zpos(struct task *t, int zpos);
void wm_restore(struct task *t);          /* function 18.22: unminimised, not activated */
int wm_message(int kind, u32 code);       /* function 72.1: to the active window */
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
void wm_button_press_draw(struct task *t, struct button *b);   /* its frame inverted */
/* the window's origin for drawing (relative to the frame or the work area) */
void wm_origin(struct task *t, int *ox, int *oy);
extern int mouse_x, mouse_y, mouse_buttons;

#endif
