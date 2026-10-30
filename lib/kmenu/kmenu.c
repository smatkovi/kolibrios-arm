/* KolibriOS for ARM: kmenu (lib/kmenu/kmenu.h): the menu bar and the
 * drop-down menus, drawn in the program's window. KolibriOS's kmenu.obj
 * has no source in its repository; this follows its interface
 * (contrib/C_Layer/INCLUDE/kolibri_kmenu.h, t_edit's t_menu.inc) and its
 * look: the bar's items as text on the window's colour, a drop-down
 * menu as a framed box of items, the item under the mouse in the
 * button colours, separators as an engraved line, submenus opening to
 * the right with an arrow. GPL-2.0. */
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include "kmenu.h"

typedef short s16;

#define CHAR_W 6               /* font 0 */
#define BAR_H 20               /* the menu bar */
#define BAR_PAD 6              /* left and right of a bar item's text */
#define ITEM_H 20              /* a drop-down item */
#define ITEM_PAD_L 22          /* room for a mark on the left */
#define ITEM_PAD_R 20          /* ... and the submenu's arrow on the right */
#define SEP_H 8                /* a separator item */
#define BORDER 2               /* the drop-down's frame */
#define MAX_OPEN 4             /* menus open at once (the bar's and 3 levels) */

static struct {
	u32 frame, grab, work_dark, work_light, grab_text, work, work_button, work_button_text, work_text, work_graph;
} sc;

/* the open drop-down menus (level 0 opened from the bar), the
 * background each covers */
static ksubmenu_t *open_menu[MAX_OPEN];
static u8 *saved[MAX_OPEN];
static int depth, bar_item = -1, prev_buttons, pressed;

static u32 text_w(const char *s) { return s ? strlen(s) * CHAR_W : 0; }

void kmenu_init(void *colors)
{
	if (colors) memcpy(&sc, colors, sizeof(sc));
	else kos_call(48, 3, (u32)&sc, sizeof(sc), 0, 0);
}

ksubmenu_t *ksubmenu_new(void)
{
	ksubmenu_t *m = calloc(1, sizeof(*m));
	if (m) m->focus_idx = -1;
	return m;
}

void ksubmenu_delete(ksubmenu_t *m)
{
	if (!m) return;
	for (int i = 0; i < m->count; i++) kmenuitem_delete(m->items[i]);
	free(m->items);
	free(m);
}

void ksubmenu_add(ksubmenu_t *m, kmenuitem_t *item)
{
	if (!m || !item) return;
	kmenuitem_t **n = realloc(m->items, (m->count + 1) * sizeof(*n));
	if (!n) return;
	m->items = n;
	m->items[m->count++] = item;
	if (item->type == KMENUITEM_SUBMENU && item->submenu) ((ksubmenu_t *)item->submenu)->parent = m;
}

kmenuitem_t *kmenuitem_new(uint32_t type, const char *text, uint32_t param)
{
	kmenuitem_t *it = calloc(1, sizeof(*it));
	if (!it) return NULL;
	it->type = type;
	it->text = text ? strdup(text) : NULL;
	if (type == KMENUITEM_SUBMENU) it->submenu = (void *)param;
	else it->btnid = param;
	it->is_enabled = it->is_visible = 1;
	it->font_width = CHAR_W;
	it->font_height = 9;
	return it;
}

kmenuitem_t *kmenuitem__submenu_new(uint32_t type, const char *text, ksubmenu_t *submenu)
{
	return kmenuitem_new(type, text, (uint32_t)submenu);
}

void kmenuitem_delete(kmenuitem_t *it)
{
	if (!it) return;
	if (it->type == KMENUITEM_SUBMENU) ksubmenu_delete(it->submenu);
	free(it->text);
	free(it);
}

int kmainmenu_height(void) { return BAR_H; }

/* the window's client area on the screen and its size */
static void client_box(int *x, int *y, int *w, int *h)
{
	u8 pi[1024];
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	*x = *(s32 *)(pi + 34) + *(s32 *)(pi + 54);
	*y = *(s32 *)(pi + 38) + *(s32 *)(pi + 58);
	*w = *(s32 *)(pi + 62) + 1;
	*h = *(s32 *)(pi + 66) + 1;
}

static void text(int x, int y, const char *s, u32 color)
{
	kos_call(4, (u32)x << 16 | (u16)y, 0x80000000u | (color & 0xffffff), (u32)s, 0, 0);
}

void kmenuitem_draw(kmenuitem_t *it)
{
	int x = it->left, y = it->top, w = it->width, h = it->height;
	if (it->style & KMENUITEM_MAINMENU) {
		/* an item of the bar: raised when its menu is open */
		int on = it->is_focused;
		kos_rect(x, y, w, h, on ? sc.work_button : sc.work);
		text(x + BAR_PAD, y + (h - 9) / 2, it->text, on ? sc.work_button_text : sc.work_text);
		return;
	}
	if (it->type == KMENUITEM_SEPARATOR) {
		kos_rect(x, y, w, h, sc.work);
		kos_rect(x + 4, y + h / 2 - 1, w - 8, 1, sc.work_dark);
		kos_rect(x + 4, y + h / 2, w - 8, 1, sc.work_light);
		return;
	}
	int on = it->is_focused && it->is_enabled;
	u32 bg = on ? sc.work_button : sc.work, fg = on ? sc.work_button_text : sc.work_text;
	if (!it->is_enabled) fg = sc.work_dark;
	kos_rect(x, y, w, h, bg);
	text(x + ITEM_PAD_L, y + (h - 9) / 2, it->text, fg);
	if (it->type == KMENUITEM_SUBMENU) {
		/* the arrow: a small triangle pointing right */
		int ax = x + w - 12, ay = y + h / 2;
		for (int i = 0; i < 4; i++) kos_rect(ax + i, ay - 3 + i, 1, 7 - 2 * i, fg);
	}
}

/* the menu's size and its items' places */
static void layout(ksubmenu_t *m)
{
	u32 w = 100, h = 0;
	for (int i = 0; i < m->count; i++) {
		kmenuitem_t *it = m->items[i];
		u32 iw = text_w(it->text) + ITEM_PAD_L + ITEM_PAD_R;
		if (iw > w) w = iw;
		h += it->type == KMENUITEM_SEPARATOR ? SEP_H : ITEM_H;
	}
	m->width = w + 2 * BORDER;
	m->height = h + 2 * BORDER;
	u32 y = m->top + BORDER;
	for (int i = 0; i < m->count; i++) {
		kmenuitem_t *it = m->items[i];
		it->style &= ~KMENUITEM_MAINMENU;
		it->left = m->left + BORDER;
		it->top = y;
		it->width = w;
		it->height = it->type == KMENUITEM_SEPARATOR ? SEP_H : ITEM_H;
		y += it->height;
	}
}

void ksubmenu_draw(ksubmenu_t *m)
{
	int x = m->left, y = m->top, w = m->width, h = m->height;
	/* the frame: a dark line around, light and dark edges inside */
	kos_rect(x, y, w, 1, sc.work_graph);
	kos_rect(x, y + h - 1, w, 1, sc.work_graph);
	kos_rect(x, y, 1, h, sc.work_graph);
	kos_rect(x + w - 1, y, 1, h, sc.work_graph);
	kos_rect(x + 1, y + 1, w - 2, 1, sc.work_light);
	kos_rect(x + 1, y + 1, 1, h - 2, sc.work_light);
	kos_rect(x + 1, y + h - 2, w - 2, 1, sc.work_dark);
	kos_rect(x + w - 2, y + 1, 1, h - 2, sc.work_dark);
	for (int i = 0; i < m->count; i++) {
		m->items[i]->is_focused = i == m->focus_idx;
		kmenuitem_draw(m->items[i]);
	}
}

/* the bar's items placed and drawn */
void kmainmenu_draw(ksubmenu_t *m)
{
	/* a redraw of the window: the menus open are gone with it */
	for (int i = 0; i < depth; i++) {
		free(saved[i]);
		saved[i] = NULL;
		open_menu[i]->is_opened = 0;
	}
	depth = 0;
	bar_item = -1;
	if (!m) return;
	u32 x = 4;
	for (int i = 0; i < m->count; i++) {
		kmenuitem_t *it = m->items[i];
		it->style |= KMENUITEM_MAINMENU;
		it->left = x;
		it->top = 0;
		it->width = text_w(it->text) + 2 * BAR_PAD;
		it->height = BAR_H;
		it->is_focused = 0;
		x += it->width;
		kmenuitem_draw(it);
	}
}

/* a menu opened at (x, y) of the client area, its background kept */
static void open_at(ksubmenu_t *m, int x, int y)
{
	if (depth >= MAX_OPEN || !m) return;
	int cx, cy, cw, ch;
	client_box(&cx, &cy, &cw, &ch);
	m->left = x;
	m->top = y;
	layout(m);
	if (m->left + m->width > (u32)cw) m->left = cw > (int)m->width ? cw - (int)m->width : 0;
	if (m->top + m->height > (u32)ch) m->top = ch > (int)m->height ? ch - (int)m->height : 0;
	layout(m);
	m->focus_idx = -1;
	m->is_opened = 1;
	saved[depth] = malloc(m->width * m->height * 3);
	if (saved[depth]) kos_read_screen(saved[depth], cx + m->left, cy + m->top, m->width, m->height);
	open_menu[depth++] = m;
	ksubmenu_draw(m);
}

/* the menus from level k on closed, their backgrounds put back */
static void close_from(int k)
{
	while (depth > k) {
		ksubmenu_t *m = open_menu[--depth];
		if (saved[depth]) kos_put_image(saved[depth], m->width, m->height, m->left, m->top);
		free(saved[depth]);
		saved[depth] = NULL;
		m->is_opened = 0;
	}
}

static void bar_focus(ksubmenu_t *bar, int k)
{
	if (bar_item >= 0 && bar_item < bar->count) {
		bar->items[bar_item]->is_focused = 0;
		kmenuitem_draw(bar->items[bar_item]);
	}
	bar_item = k;
	if (k >= 0) {
		bar->items[k]->is_focused = 1;
		kmenuitem_draw(bar->items[k]);
	}
}

static void close_all(ksubmenu_t *bar)
{
	close_from(0);
	bar_focus(bar, -1);
}

/* the drop-down of the bar's item k */
static void open_bar_item(ksubmenu_t *bar, int k)
{
	close_from(0);
	bar_focus(bar, k);
	kmenuitem_t *it = bar->items[k];
	if (it->type == KMENUITEM_SUBMENU) open_at(it->submenu, it->left, BAR_H);
}

static void set_focus(int level, int idx)
{
	ksubmenu_t *m = open_menu[level];
	if (m->focus_idx == idx) return;
	int old = m->focus_idx;
	m->focus_idx = idx;
	if (old >= 0 && old < m->count) { m->items[old]->is_focused = 0; kmenuitem_draw(m->items[old]); }
	if (idx >= 0 && idx < m->count) { m->items[idx]->is_focused = 1; kmenuitem_draw(m->items[idx]); }
}

/* the item chosen: the menus closed, its button pressed */
static void choose(ksubmenu_t *bar, kmenuitem_t *it)
{
	close_all(bar);
	kos_send_button(it->btnid);
}

/* the submenu of a focused item opened next to it */
static void open_sub(int level, kmenuitem_t *it)
{
	close_from(level + 1);
	if (it->type == KMENUITEM_SUBMENU && it->is_enabled)
		open_at(it->submenu, it->left + it->width + BORDER, it->top - BORDER);
}

static int in(int x, int y, size_t l, size_t t, size_t w, size_t h)
{
	return x >= (int)l && x < (int)(l + w) && y >= (int)t && y < (int)(t + h);
}

int kmainmenu_dispatch_cursorevent(ksubmenu_t *bar)
{
	if (!bar) return 0;
	u32 p = kos_call(37, 1, 0, 0, 0, 0);
	int x = (s16)(p >> 16), y = (s16)p;
	int b = kos_call(37, 2, 0, 0, 0, 0) & 1;
	int press = b && !prev_buttons, release = !b && prev_buttons;
	prev_buttons = b;
	/* the bar's item under the mouse */
	int hit = -1;
	for (int i = 0; i < bar->count; i++)
		if (in(x, y, bar->items[i]->left, 0, bar->items[i]->width, BAR_H)) hit = i;
	if (!depth) {
		if (press && hit >= 0) {
			open_bar_item(bar, hit);
			pressed = 1;
			return 1;
		}
		if (pressed) {                     /* the press that opened a menu since closed */
			if (!b) pressed = 0;
			return 1;
		}
		return 0;
	}
	if (press) pressed = 1;
	if (hit >= 0) {
		if (hit != bar_item) open_bar_item(bar, hit);      /* along the bar */
		else if (press) close_all(bar);                    /* the open one again */
		if (!b) pressed = 0;
		return 1;
	}
	/* the deepest menu under the mouse */
	for (int l = depth - 1; l >= 0; l--) {
		ksubmenu_t *m = open_menu[l];
		if (!in(x, y, m->left, m->top, m->width, m->height)) continue;
		int idx = -1;
		for (int i = 0; i < m->count; i++) {
			kmenuitem_t *it = m->items[i];
			if (it->type != KMENUITEM_SEPARATOR && in(x, y, it->left, it->top, it->width, it->height)) idx = i;
		}
		set_focus(l, idx);
		if (idx >= 0) {
			kmenuitem_t *it = m->items[idx];
			if (it->type == KMENUITEM_SUBMENU) {
				if (!(l + 1 < depth && open_menu[l + 1] == it->submenu)) open_sub(l, it);
			} else {
				close_from(l + 1);
				if (release && pressed && it->is_enabled) {
					pressed = 0;
					choose(bar, it);
					return 1;
				}
			}
		}
		if (!b) pressed = 0;
		return 1;
	}
	/* outside: a press closes the menus and goes to the window */
	if (press) {
		close_all(bar);
		pressed = 0;
		return 0;
	}
	if (!b) pressed = 0;
	return 1;
}

/* the keys of an open menu: up, down, left, right, Enter, Escape */
int kmainmenu_dispatch_key(ksubmenu_t *bar, uint32_t key)
{
	if (!bar || !depth) return 0;
	int scan = kos_call(66, 2, 0, 0, 0, 0) == 1;
	u8 c = key >> 8;
	enum { NONE, UP, DOWN, LEFT, RIGHT, ENTER, ESC } k = NONE;
	if (scan) {
		switch (c) {
		case 0x48: k = UP; break;
		case 0x50: k = DOWN; break;
		case 0x4b: k = LEFT; break;
		case 0x4d: k = RIGHT; break;
		case 0x1c: k = ENTER; break;
		case 0x01: k = ESC; break;
		}
	} else {
		switch (c) {
		case 0xb2: k = UP; break;
		case 0xb1: k = DOWN; break;
		case 0xb0: k = LEFT; break;
		case 0xb3: k = RIGHT; break;
		case 13: k = ENTER; break;
		case 27: k = ESC; break;
		}
	}
	int l = depth - 1;
	ksubmenu_t *m = open_menu[l];
	int f = m->focus_idx;
	switch (k) {
	case UP:
	case DOWN:
		for (int n = 0; n < m->count; n++) {
			f = k == DOWN ? (f + 1) % m->count : (f <= 0 ? m->count - 1 : f - 1);
			if (m->items[f]->type != KMENUITEM_SEPARATOR) break;
		}
		set_focus(l, f);
		break;
	case RIGHT:
		if (f >= 0 && m->items[f]->type == KMENUITEM_SUBMENU) {
			open_sub(l, m->items[f]);
			if (depth > l + 1) set_focus(l + 1, 0);
		} else if (bar->count) {
			open_bar_item(bar, (bar_item + 1) % bar->count);
		}
		break;
	case LEFT:
		if (l > 0) close_from(l);
		else if (bar->count) open_bar_item(bar, bar_item <= 0 ? bar->count - 1 : bar_item - 1);
		break;
	case ENTER:
		if (f >= 0) {
			kmenuitem_t *it = m->items[f];
			if (it->type == KMENUITEM_SUBMENU) {
				open_sub(l, it);
				if (depth > l + 1) set_focus(l + 1, 0);
			} else if (it->is_enabled) {
				choose(bar, it);
			}
		}
		break;
	case ESC:
		if (l > 0) close_from(l);
		else close_all(bar);
		break;
	default:
		break;
	}
	return 1;
}
