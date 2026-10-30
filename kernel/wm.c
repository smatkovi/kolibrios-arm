/* KolibriOS for ARM: the window manager, in the kernel as in KolibriOS.
 *
 * One window per task (a KolibriOS thread). The windows lie in a stack,
 * the top one is active and gets the keys. When a part of the screen
 * changes owner, the kernel paints the desktop there and sends the
 * windows it uncovers a redraw event; programs then redraw themselves
 * (functions 12.1, 0, the primitives, 12.2). The kernel draws the frame
 * and the title of skinned windows when function 0 is called.
 *
 * The stack has three layers (function 18.25): the windows always at the
 * back, the normal ones, those always on top (a panel). The active window
 * (the keys go to it) is the one last activated, else the top normal
 * one; a click does not activate a window always on top (a panel's taps
 * are its buttons), its program activates it itself when it wants keys.
 *
 * In Hildon's look (function 48.8) the windows but the plain ones have
 * no frame or title and fill the work area (function 48.6), which the
 * shell's panel leaves free; they do not move. */
#include "gui.h"

#define ABS(v) ((v) < 0 ? -(v) : (v))

static int stack[MAX_TASKS];          /* slots, bottom to top */
static int nstack;
static int active_slot;               /* 0: the top normal window */
int wm_look = LOOK_CLASSIC;
int work_x0, work_y0, work_x1, work_y1;
int mouse_x, mouse_y, mouse_buttons;
static int mouse_events;              /* function 37.3's event bits */

static int drag_slot, drag_dx, drag_dy;
static int press_slot = 0, press_idx = -1;
static u32 press_buttons;

#define COL_ACTIVE 0x2c5c9e
#define COL_INACTIVE 0x7c8894
#define COL_OUTER 0x101820
#define COL_INNER 0x384858

static bool visible(struct task *t)
{
	return t->state != T_FREE && t->win.defined && !t->win.minimized;
}

/* in Hildon's look: maximised in the work area, without frame */
static bool managed(const struct window *w)
{
	return wm_look == LOOK_HILDON && w->style != 1;
}

/* function 9's +70 bit 0: the Hildon look maximises it */
bool wm_maximised(const struct window *w)
{
	return managed(w);
}

int wm_skin_h(const struct window *w)
{
	return w && managed(w) ? 0 : SKIN_H;
}

int wm_frame_w(const struct window *w)
{
	return w && managed(w) ? 0 : FRAME_W;
}

static void fit_work_area(struct window *w)
{
	w->x = work_x0;
	w->y = work_y0;
	w->w = work_x1 - work_x0 + 1;
	w->h = work_y1 - work_y0 + 1;
}

static bool intersects(struct window *w, int x, int y, int ww, int hh)
{
	return w->x < x + ww && x < w->x + w->w && w->y < y + hh && y < w->y + w->h;
}

static void owner_region(int x, int y, int w, int h)
{
	int x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN(x + w, scr.w), y1 = MIN(y + h, scr.h);
	if (x0 >= x1 || y0 >= y1) return;
	for (int yy = y0; yy < y1; yy++)
		memset(&scr.owner[yy * scr.w + x0], 0, x1 - x0);
	for (int i = 0; i < nstack; i++) {
		struct task *t = &tasks[stack[i]];
		if (!visible(t)) continue;
		struct window *wn = &t->win;
		int a0 = MAX(x0, wn->x), b0 = MAX(y0, wn->y);
		int a1 = MIN(x1, wn->x + wn->w), b1 = MIN(y1, wn->y + wn->h);
		for (int yy = b0; yy < b1; yy++)
			if (a0 < a1) memset(&scr.owner[yy * scr.w + a0], t->slot, a1 - a0);
	}
}

void wm_redraw_background(int x, int y, int w, int h)
{
	int y0 = MAX(y, 0), y1 = MIN(y + h, scr.h);
	for (int yy = y0; yy < y1; yy++) {
		u32 c;
		if (wm_look == LOOK_HILDON) {
			/* Hildon's dark home, a little blue at the top */
			int f = 255 - yy * 170 / scr.h;
			c = ((0x1c * f >> 8) << 16) | ((0x2a * f >> 8) << 8) | (0x3c * f >> 8);
		} else {
			/* KolibriOS's blue, darker towards the bottom */
			int f = 255 - yy * 110 / scr.h;
			c = ((0x30 * f >> 8) << 16) | ((0x68 * f >> 8) << 8) | (0xa8 * f >> 8);
		}
		gfx_rect(x, yy, w, 1, c, 0);
	}
}

/* a part of the screen may show other windows now */
static void expose(int x, int y, int w, int h)
{
	owner_region(x, y, w, h);
	wm_redraw_background(x, y, w, h);
	for (int i = 0; i < nstack; i++) {
		struct task *t = &tasks[stack[i]];
		if (visible(t) && intersects(&t->win, x, y, w, h)) {
			wm_draw_frame(t);
			task_post_event(t, EV_REDRAW);
		}
	}
}

static void stack_remove(int slot)
{
	int j = 0;
	for (int i = 0; i < nstack; i++)
		if (stack[i] != slot) stack[j++] = stack[i];
	nstack = j;
}

/* on top of its layer */
static void stack_push(int slot)
{
	stack_remove(slot);
	int z = tasks[slot].win.zpos, i = nstack;
	while (i > 0 && tasks[stack[i - 1]].win.zpos > z) i--;
	for (int j = nstack; j > i; j--) stack[j] = stack[j - 1];
	stack[i] = slot;
	nstack++;
}

struct task *wm_active(void)
{
	if (active_slot && visible(&tasks[active_slot])) return &tasks[active_slot];
	for (int pass = 0; pass < 2; pass++)
		for (int i = nstack - 1; i >= 0; i--) {
			struct task *t = &tasks[stack[i]];
			if (visible(t) && (pass || t->win.zpos <= ZPOS_NORMAL)) return t;
		}
	return NULL;
}

int wm_stack_pos(struct task *t)
{
	for (int i = 0; i < nstack; i++)
		if (stack[i] == t->slot) return i + 1;
	return 0;
}

int wm_stack_count(void) { return nstack; }
int wm_slot_at_pos(int pos) { return pos >= 1 && pos <= nstack ? stack[pos - 1] : 0; }

void wm_init(void)
{
	nstack = 0;
	work_x0 = 0;
	work_y0 = 0;
	work_x1 = scr.w - 1;
	work_y1 = scr.h - 1;
	memset(scr.owner, 0, scr.w * scr.h);
	wm_redraw_background(0, 0, scr.w, scr.h);
	mouse_x = scr.w / 2;
	mouse_y = scr.h / 2;
	cursor_move(mouse_x, mouse_y);
}

void wm_origin(struct task *t, int *ox, int *oy)
{
	struct window *w = &t->win;
	*ox = w->x;
	*oy = w->y;
	if (w->flags & 2) {          /* B: relative to the work area */
		if (w->style == 3 || w->style == 4) {
			*ox += wm_frame_w(w);
			*oy += wm_skin_h(w);
		}
	}
}

static void frame_lines(int x, int y, int w, int h, u32 c, int slot)
{
	gfx_rect(x, y, w, 1, c, slot);
	gfx_rect(x, y + h - 1, w, 1, c, slot);
	gfx_rect(x, y, 1, h, c, slot);
	gfx_rect(x + w - 1, y, 1, h, c, slot);
}

static u32 shade(u32 c, int f)  /* f/256 of the color, f > 256: lighter */
{
	int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
	r = MIN(r * f >> 8, 255); g = MIN(g * f >> 8, 255); b = MIN(b * f >> 8, 255);
	return (r << 16) | (g << 8) | b;
}

static void add_button(struct task *t, int x, int y, int w, int h, u32 id, u32 color, bool hidden, bool noframe)
{
	struct window *wn = &t->win;
	if (wn->nbtn >= MAX_BUTTONS) return;
	struct button *b = &wn->btn[wn->nbtn++];
	b->x = x; b->y = y; b->w = w; b->h = h;
	b->id = id; b->color = color; b->hidden = hidden; b->noframe = noframe;
}

void wm_draw_frame(struct task *t)
{
	struct window *w = &t->win;
	int s = t->slot;
	if (!visible(t)) return;
	if (w->style == 1) return;
	if (managed(w)) {            /* Hildon: the work area only */
		if (!(w->flags & 4)) {
			if (w->flags & 8) gfx_hgradient(w->x, w->y, w->w, w->h, w->work_color, s);
			else gfx_rect(w->x, w->y, w->w, w->h, w->work_color, s);
		}
		return;
	}
	bool active = wm_active() == t;
	if (w->style == 3 || w->style == 4 || w->style == 0 || w->style == 2) {
		u32 tc = active ? COL_ACTIVE : COL_INACTIVE;
		frame_lines(w->x, w->y, w->w, w->h, COL_OUTER, s);
		for (int i = 1; i < 4; i++)
			frame_lines(w->x + i, w->y + i, w->w - 2 * i, w->h - 2 * i, tc, s);
		frame_lines(w->x + 4, w->y + SKIN_H - 1, w->w - 8, w->h - SKIN_H - 3, COL_INNER, s);
		gfx_hgradient(w->x + 1, w->y + 1, w->w - 2, SKIN_H - 2, shade(tc, 300), s);
		/* close and minimize */
		int bx = w->x + w->w - 22, by = w->y + 3;
		gfx_rect(bx, by, 17, 15, active ? 0xc84040 : 0x9c6c6c, s);
		frame_lines(bx, by, 17, 15, COL_OUTER, s);
		gfx_text(bx + 5, by + 2, "x", 1, 0xffffff, 0, 1, false, 0, s, false);
		gfx_rect(bx - 20, by, 17, 15, shade(tc, 340), s);
		frame_lines(bx - 20, by, 17, 15, COL_OUTER, s);
		gfx_rect(bx - 16, by + 10, 9, 2, 0xffffff, s);
		if ((w->flags & 1) && w->title[0])
			gfx_text(w->x + 9, w->y + 5, w->title, -1, 0xffffff, 0, 1, false, 0, s, true);
		if (!(w->flags & 4) && w->h > SKIN_H + FRAME_W) {
			if (w->flags & 8)
				gfx_hgradient(w->x + FRAME_W, w->y + SKIN_H, w->w - 2 * FRAME_W,
					      w->h - SKIN_H - FRAME_W, w->work_color, s);
			else
				gfx_rect(w->x + FRAME_W, w->y + SKIN_H, w->w - 2 * FRAME_W,
					 w->h - SKIN_H - FRAME_W, w->work_color, s);
		}
	}
}

/* the standard buttons are defined by function 0, as in KolibriOS */
static void define_skin_buttons(struct task *t)
{
	struct window *w = &t->win;
	if (w->style != 3 && w->style != 4) return;
	if (managed(w)) return;      /* the shell's panel has them */
	for (int i = 0; i < w->nbtn; i++)
		if (w->btn[i].id == 1 || w->btn[i].id == 0xffff) return;
	int bx = w->x + w->w - 22, by = w->y + 3;
	add_button(t, bx, by, 17, 15, 1, 0, true, true);
	add_button(t, bx - 20, by, 17, 15, 0xffff, 0, true, true);
}

void wm_define_window(struct task *t, u32 ebx, u32 ecx, u32 edx, u32 esi, const char *title)
{
	struct window *w = &t->win;
	int style = (edx >> 24) & 0xf;
	if (style > 4) return;
	if (!w->defined) {
		int x = ebx >> 16, xs = ebx & 0xffff, y = ecx >> 16, ys = ecx & 0xffff;
		if (xs >= scr.w) { x = 0; xs = scr.w - 1; }
		if (x + xs >= scr.w) x = scr.w - 1 - xs;
		if (ys >= scr.h) { y = 0; ys = scr.h - 1; }
		if (y + ys >= scr.h) y = scr.h - 1 - ys;
		w->x = x; w->y = y; w->w = xs + 1; w->h = ys + 1;
		w->movable = ((esi >> 24) & 0xf) != 1;
		w->style = style;
		w->flags = (edx >> 28) & 0xf;
		w->zpos = ZPOS_NORMAL;
		if (managed(w)) {
			fit_work_area(w);
			w->movable = false;
		}
		if (title) strncpy0(w->title, title, sizeof(w->title));
		w->defined = true;
		struct task *old = wm_active();
		stack_push(t->slot);
		active_slot = t->slot;
		owner_region(w->x, w->y, w->w, w->h);
		if (old && old != t) { wm_draw_frame(old); task_post_event(old, EV_REDRAW); }
	}
	w->style = style;
	w->flags = (edx >> 28) & 0xf;
	w->work_color = edx & 0xffffff;
	w->title_color = esi & 0xffffff;
	if (title && !w->title[0]) strncpy0(w->title, title, sizeof(w->title));
	wm_draw_frame(t);
	define_skin_buttons(t);
}

void wm_activate(struct task *t)
{
	if (t->state == T_FREE || !t->win.defined) return;
	struct task *old = wm_active();
	t->win.minimized = false;
	stack_push(t->slot);
	active_slot = t->slot;
	owner_region(t->win.x, t->win.y, t->win.w, t->win.h);
	wm_draw_frame(t);
	task_post_event(t, EV_REDRAW);
	if (old && old != t) {
		wm_draw_frame(old);
		task_post_event(old, EV_REDRAW);
	}
}

void wm_minimize(struct task *t)
{
	struct window *w = &t->win;
	if (!w->defined || w->minimized) return;
	w->minimized = true;
	if (active_slot == t->slot) active_slot = 0;
	expose(w->x, w->y, w->w, w->h);
	struct task *a = wm_active();
	if (a) { wm_draw_frame(a); task_post_event(a, EV_REDRAW); }
}

void wm_move(struct task *t, int x, int y, int ww, int hh)
{
	struct window *w = &t->win;
	if (!w->defined) return;
	if (managed(w)) return;      /* maximised in the work area */
	int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
	if (x != -1) w->x = x;
	if (y != -1) w->y = y;
	if (ww != -1) w->w = ww + 1;
	if (hh != -1) w->h = hh + 1;
	if (w->w > scr.w) w->w = scr.w;
	if (w->h > scr.h) w->h = scr.h;
	if (w->x + w->w > scr.w) w->x = scr.w - w->w;
	if (w->y + w->h > scr.h) w->y = scr.h - w->h;
	if (w->x < 0) w->x = 0;
	if (w->y < 0) w->y = 0;
	int dx = w->x - ox, dy = w->y - oy;
	for (int i = 0; i < w->nbtn; i++) {
		w->btn[i].x += dx;
		w->btn[i].y += dy;
	}
	if (ww != -1 || hh != -1) {
		/* the skin's buttons follow the right edge */
		for (int i = 0; i < w->nbtn; i++)
			if (w->btn[i].id == 1 || w->btn[i].id == 0xffff)
				w->btn[i].x = w->x + w->w - 22 - (w->btn[i].id == 1 ? 0 : 20);
	}
	expose(ox, oy, ow, oh);
	expose(w->x, w->y, w->w, w->h);
}

void wm_remove(struct task *t)
{
	struct window *w = &t->win;
	bool was = w->defined && !w->minimized;
	struct task *a = wm_active();
	stack_remove(t->slot);
	w->defined = false;
	w->zpos = ZPOS_NORMAL;
	if (active_slot == t->slot) active_slot = 0;
	if (press_slot == t->slot) press_slot = 0;
	if (drag_slot == t->slot) drag_slot = 0;
	if (was) expose(w->x, w->y, w->w, w->h);
	if (a == t && (a = wm_active())) {
		wm_draw_frame(a);
		task_post_event(a, EV_REDRAW);
	}
}

/* a line of the screen inverted, where the slot owns it */
static void negate_line(int x, int y, int w, int h, int slot)
{
	static u8 px[3 * 4096];
	if (w <= 0 || h <= 0 || w * h > 4096) return;
	if (x < 0 || y < 0 || x + w > scr.w || y + h > scr.h) return;
	cursor_hide();                         /* the pointer is not the frame's */
	gfx_read(px, x, y, w, h);
	for (int i = 0; i < 3 * w * h; i++) px[i] ^= 0xff;
	gfx_image(x, y, w, h, px, slot);
	cursor_show();
}

/* a button pressed or let go: its frame inverted, as KolibriOS's kernel
 * does (negative_button), the face and what the program drew on it kept */
void wm_button_press_draw(struct task *t, struct button *b)
{
	if (b->noframe) return;
	int s = t->slot;
	negate_line(b->x, b->y, b->w, 1, s);
	negate_line(b->x, b->y + b->h - 1, b->w, 1, s);
	negate_line(b->x, b->y + 1, 1, b->h - 2, s);
	negate_line(b->x + b->w - 1, b->y + 1, 1, b->h - 2, s);
}

void wm_button_draw(struct task *t, struct button *b, bool pressed)
{
	int s = t->slot;
	if (b->hidden) {
		if (!b->noframe && pressed)
			frame_lines(b->x, b->y, b->w, b->h, 0xffffff, s);
		return;
	}
	gfx_rect(b->x, b->y, b->w, b->h, pressed ? shade(b->color, 200) : b->color, s);
	if (b->noframe) return;
	u32 hi = shade(b->color, pressed ? 160 : 340), lo = shade(b->color, pressed ? 340 : 160);
	gfx_rect(b->x, b->y, b->w, 1, hi, s);
	gfx_rect(b->x, b->y, 1, b->h, hi, s);
	gfx_rect(b->x, b->y + b->h - 1, b->w, 1, lo, s);
	gfx_rect(b->x + b->w - 1, b->y, 1, b->h, lo, s);
}

static int button_at(struct task *t, int x, int y)
{
	struct window *w = &t->win;
	for (int i = w->nbtn - 1; i >= 0; i--) {
		struct button *b = &w->btn[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

static void button_event(struct task *t, u32 id, u32 mbuttons)
{
	if (id == 0xffff) {
		wm_minimize(t);
		return;
	}
	t->btns[0] = (id << 8) | (mbuttons & ~1u & 0xff);
	t->btn_head = 1;
	t->btn_tail = 0;
	task_post_event(t, EV_BUTTON);
}

/* function 37.3's events: those of the last change of the mouse; they
 * stay until the next one (KolibriOS's BTN_DOWN), reading keeps them */
int wm_mouse_events(void)
{
	return mouse_events;
}

void wm_mouse(int x, int y, int buttons, int changed)
{
	int ox = mouse_x, oy = mouse_y;
	mouse_x = MAX(0, MIN(x, scr.w - 1));
	mouse_y = MAX(0, MIN(y, scr.h - 1));
	mouse_buttons = buttons;
	mouse_events = 0;
	for (int i = 0; i < 3; i++) {
		if (changed & (1 << i))
			mouse_events |= (buttons & (1 << i)) ? 1 << (8 + i) : 1 << (16 + i);
	}
	cursor_move(mouse_x, mouse_y);

	int slot = scr.owner[mouse_y * scr.w + mouse_x];
	struct task *t = slot ? &tasks[slot] : NULL;

	if ((changed & 1) && (buttons & 1)) {           /* left button down */
		/* function 37.3's bit 24: a double click (within 0.5 s, nearby) */
		static u32 last_click;
		static int last_cx, last_cy;
		if (last_click && ticks - last_click < 50 && ABS(mouse_x - last_cx) < 5 && ABS(mouse_y - last_cy) < 5) {
			mouse_events |= 1 << 24;
			last_click = 0;
		} else {
			last_click = ticks ? ticks : 1;
			last_cx = mouse_x;
			last_cy = mouse_y;
		}
		if (t) {
			if (wm_active() != t && t->win.zpos != ZPOS_ALWAYS_TOP) wm_activate(t);
			int bi = button_at(t, mouse_x, mouse_y);
			struct window *w = &t->win;
			if (bi >= 0) {
				press_slot = slot;
				press_idx = bi;
				press_buttons = buttons;
				wm_button_press_draw(t, &w->btn[bi]);
			} else if ((w->style == 3 || w->style == 4 || w->style == 0 || w->style == 2) &&
				   w->movable && mouse_y < w->y + SKIN_H) {
				drag_slot = slot;
				drag_dx = mouse_x - w->x;
				drag_dy = mouse_y - w->y;
			}
		}
	} else if ((changed & 1) && !(buttons & 1)) {   /* left button up */
		drag_slot = 0;
		if (press_slot) {
			struct task *p = &tasks[press_slot];
			if (p->state != T_FREE && press_idx < p->win.nbtn) {
				struct button *b = &p->win.btn[press_idx];
				wm_button_press_draw(p, b);
				if (mouse_x >= b->x && mouse_x < b->x + b->w &&
				    mouse_y >= b->y && mouse_y < b->y + b->h)
					button_event(p, b->id, press_buttons);
			}
			press_slot = 0;
		}
	} else if (drag_slot && (buttons & 1) && (ox != mouse_x || oy != mouse_y)) {
		struct task *d = &tasks[drag_slot];
		wm_move(d, mouse_x - drag_dx, mouse_y - drag_dy, -1, -1);
	}

	/* mouse events: to the window under the pointer and the active one,
	 * as their masks allow (bit 31: not while inactive) */
	struct task *a = wm_active();
	if (a && (a->event_mask & (1 << (EV_MOUSE - 1))))
		task_post_event(a, EV_MOUSE);
	if (t && t != a && (t->event_mask & (1 << (EV_MOUSE - 1))) && !(t->event_mask & 0x80000000u))
		task_post_event(t, EV_MOUSE);
}

void wm_key(u32 value)
{
	struct task *a = wm_active();
	if (!a) return;
	int n = (a->key_head + 1) % KEYBUF;
	if (n == a->key_tail) return;    /* full: dropped, as KolibriOS does */
	a->keys[a->key_head] = value;
	a->key_head = n;
	task_post_event(a, EV_KEY);
}

/* the whole screen again: the owners, the desktop, every window */
static void relayout(void)
{
	for (int i = 0; i < nstack; i++) {
		struct window *w = &tasks[stack[i]].win;
		if (managed(w)) {
			fit_work_area(w);
			w->movable = false;
			w->nbtn = 0;     /* defined again at the redraw */
		}
	}
	expose(0, 0, scr.w, scr.h);
}

int wm_set_look(int look)
{
	if (look != LOOK_CLASSIC && look != LOOK_HILDON) return -1;
	if (look != wm_look) {
		wm_look = look;
		relayout();
	}
	return 0;
}

void wm_set_work_area(int x0, int x1, int y0, int y1)
{
	x0 = MAX(x0, 0); y0 = MAX(y0, 0);
	x1 = MIN(x1, scr.w - 1); y1 = MIN(y1, scr.h - 1);
	if (x0 >= x1 || y0 >= y1) return;
	if (x0 == work_x0 && x1 == work_x1 && y0 == work_y0 && y1 == work_y1) return;
	work_x0 = x0; work_x1 = x1; work_y0 = y0; work_y1 = y1;
	if (wm_look == LOOK_HILDON) relayout();
}

void wm_set_zpos(struct task *t, int zpos)
{
	struct window *w = &t->win;
	if (zpos < ZPOS_ALWAYS_BACK || zpos > ZPOS_ALWAYS_TOP || !w->defined) return;
	w->zpos = zpos;
	stack_push(t->slot);
	expose(w->x, w->y, w->w, w->h);
}

void wm_restore(struct task *t)
{
	struct window *w = &t->win;
	if (!w->defined || !w->minimized) return;
	w->minimized = false;
	owner_region(w->x, w->y, w->w, w->h);
	wm_draw_frame(t);
	task_post_event(t, EV_REDRAW);
}

/* function 72.1: a key (2, the code as function 2 has it in ah) or a
 * button (3, its identifier) for the active window */
int wm_message(int kind, u32 code)
{
	struct task *a = wm_active();
	if (!a) return 1;
	if (kind == 2) {
		int n = (a->key_head + 1) % KEYBUF;
		if (n == a->key_tail) return 1;
		a->keys[a->key_head] = (code & 0xff) << 8;
		a->key_head = n;
		task_post_event(a, EV_KEY);
		return 0;
	}
	if (kind == 3) {
		button_event(a, code & 0xffffff, 1);
		return 0;
	}
	return -1;
}
