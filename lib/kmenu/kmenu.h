/* KolibriOS for ARM: kmenu, KolibriOS's library of menus: a menu bar in
 * the window and drop-down menus, with the functions and structures as
 * KolibriOS's C layer declares them (contrib/C_Layer/INCLUDE/
 * kolibri_kmenu.h) and t_edit calls them. KolibriOS's repository has
 * kmenu.obj as a binary only (data/common/lib/kmenu.obj), so this is
 * written anew after that interface, GPL-2.0.
 *
 * In KolibriOS a drop-down menu is a window of its own in a thread, and
 * an item chosen comes to the program as a button press. Here the
 * drop-down menus are drawn in the program's window, over its contents
 * (kept by function 36 and put back with 7 when the menu closes); an
 * item chosen comes as a button press too (function 72.1, to the
 * active window, which is the program's). Since the program's window
 * gets the mouse over a menu, kmainmenu_dispatch_cursorevent returns 1
 * when the mouse was the menu's, and kmainmenu_dispatch_key gives the
 * keys to an open menu (both KolibriOS for ARM additions). */
#ifndef KMENU_H
#define KMENU_H

#include <stddef.h>
#include <stdint.h>

#define KMENUITEM_NORMAL    0
#define KMENUITEM_SUBMENU   1
#define KMENUITEM_SEPARATOR 2

#define KMENUITEM_MAINMENU  0x80000000

#define KMENUITEM_SEPARATOR_WIDTH 10
#define KMENUITEM_SEPARATOR_HEIGHT 2

#define KMENU_LBORDER_SIZE 2
#define KMENU_DBORDER_SIZE 1

typedef struct {
	int type;
	uint32_t color;
	union {
		uint32_t bgcolor;
		void *buffer;
	};
} t_font;

typedef struct __attribute__((__packed__)) {
	uint16_t left, top, right, bottom;
} t_rect;

typedef struct kmenuitem_t {
	int type;
	char *text;
	union {
		void *submenu;
		size_t btnid;
	};
	void (*paint)(struct kmenuitem_t *item, t_rect *rc);
	int is_focused;
	int is_enabled;
	int is_visible;
	size_t style;
	int font_width, font_height;
	t_font font;
	size_t left, top;
	size_t width, height, pref_width, pref_height;
	size_t margin_left, margin_right, margin_top, margin_bottom;
	size_t padding_left, padding_right, padding_top, padding_bottom;
} kmenuitem_t;

typedef struct ksubmenu_t {
	kmenuitem_t **items;
	int count;
	void (*paint)(struct ksubmenu_t *menu);
	int is_opened;
	int focus_idx;
	size_t width, height;
	size_t left, top;
	int tid;
	int parent_wnd, parent_tid, submenu_tid, self_wnd;
	int level;
	size_t return_btnid;
	int set_new_padding, set_new_margin;
	t_rect items_padding;
	t_rect items_margin;
	struct ksubmenu_t *parent;
} ksubmenu_t;

/* sc: the system colours (function 48.3's), or NULL to read them */
void kmenu_init(void *sc);
ksubmenu_t *ksubmenu_new(void);
void ksubmenu_delete(ksubmenu_t *menu);
void ksubmenu_draw(ksubmenu_t *menu);
void ksubmenu_add(ksubmenu_t *menu, kmenuitem_t *item);
/* param: the button's identifier (KMENUITEM_NORMAL) or the submenu */
kmenuitem_t *kmenuitem_new(uint32_t type, const char *text, uint32_t param);
kmenuitem_t *kmenuitem__submenu_new(uint32_t type, const char *text, ksubmenu_t *submenu);
void kmenuitem_delete(kmenuitem_t *item);
void kmenuitem_draw(kmenuitem_t *item);
/* the menu bar along the window's top (its client area) */
void kmainmenu_draw(ksubmenu_t *menu);
int kmainmenu_dispatch_cursorevent(ksubmenu_t *menu);
/* key: function 2's result, in the program's mode (66.2); 1 if taken */
int kmainmenu_dispatch_key(ksubmenu_t *menu, uint32_t key);
/* the bar's height */
int kmainmenu_height(void);

#endif
