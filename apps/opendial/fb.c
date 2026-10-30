/* KolibriOS for ARM: opendial's three box_lib components, after
 * programs/develop/libraries/box_lib/filebrowser.asm, menubar.asm and
 * pathshow.asm (Marat Zakiyanov aka Mario79; GPL-2.0), for what opendial
 * asks of them (opendial.h's structures).
 *
 * FileBrowser: the folder's entries, a line each (line_size_y), the
 * entry's icon from the icon strip (folders 0, ".." 1, files by their
 * extension in the ini's "ext=ID" lines, else 2), its name (cut with a
 * red ".." when it does not fit), the cursor's line in select_color while
 * the list has the focus. Keys (key_action): 1 down, 2 up, 3 Page Down,
 * 4 Page Up, 5 Home, 6 End, 7 Enter (mouse_keys_delta 3: opendial enters
 * the folder or takes the file). The mouse: a press puts the cursor on
 * the line, a second press on it within mouse_key_delay hundredths is
 * Enter. What differs: no marking (Insert, the grey keys, the right
 * button: an open dialog takes one file) and no search by the first
 * letter (the original looks the key up in a keymap opendial leaves
 * empty); the lines are drawn whole each time.
 *
 * menu_bar: a bar (its text) that shows its items below it when it is
 * pressed (menu_bar_mouse, or menu_bar_activate from a key); the mouse
 * or Up, Down, Home, End, Enter choose (cursor_out from 1, click 1), Esc
 * or a press elsewhere closes it (cursor_out 0), Left and Right
 * (ret_key 1, 2) go to the menu beside. What is under the menu is kept
 * with function 36 and put back with 7, as there.
 *
 * PathShow: the path, its first 6 characters, "..." and its end when it
 * is wider than the area. */
#include <string.h>
#include <stdlib.h>
#include "opendial.h"

#define ENTRY(fb, i) ((fb)->folder_data + 32 + (i) * 304)

static void rect(int x, int y, int w, int h, u32 c)
{
	if (w > 0 && h > 0) kos_call(13, (u32)x << 16 | (u16)w, (u32)y << 16 | (u16)h, c & 0xffffff, 0, 0);
}

static void text(int x, int y, u32 flags, const char *s, int len)
{
	kos_call(4, (u32)x << 16 | (u16)y, flags, (u32)s, len, 0);
}

/* fb_char_toupper: cp866 */
static u8 toupper866(u8 c)
{
	if (c >= 'a' && c <= 'z') return c - 0x20;
	if (c >= 0xa0 && c <= 0xaf) return c - 0x20;
	if (c >= 0xe0 && c <= 0xef) return c - 0x50;
	return c;
}

/* fb_get_icon_number: the icon of a file's extension ("ext=ID" lines
 * between ini_file_start and ini_file_end), 2 if none */
static int fb_get_icon_number(struct file_browser *fb, const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1] || !fb->ini_file_start) return 2;
	const char *ext = dot + 1;
	size_t n = strlen(ext);
	for (const char *l = fb->ini_file_start; l < fb->ini_file_end;) {
		const char *e = memchr(l, '\n', fb->ini_file_end - l);
		if (!e) e = fb->ini_file_end;
		if ((size_t)(e - l) > n && l[n] == '=') {
			size_t i = 0;
			while (i < n && toupper866(l[i]) == toupper866(ext[i])) i++;
			if (i == n) {
				int id = 0;
				for (const char *p = l + n + 1; p < e && *p >= '0' && *p <= '9'; p++) id = id * 10 + *p - '0';
				return id;
			}
		}
		l = e + 1;
	}
	return 2;
}

/* fb_calc_max_panel_line, fb_calc_folder_sysdata, the first line shown
 * and the cursor kept on the entries */
static void fb_prepare(struct file_browser *fb)
{
	fb->max_panel_line = fb->size_y / (fb->line_size_y ? fb->line_size_y : 1);
	fb->folder_block = fb->folder_data ? *(u32 *)(fb->folder_data + 4) : 0;
	s32 left = (s32)fb->folder_block - (s32)fb->max_panel_line;
	if (left < 0) fb->start_draw_line = 0;
	else if ((u32)left < fb->start_draw_line) fb->start_draw_line = left;
	u32 row = fb->start_draw_cursor_line / fb->line_size_y, last = fb->max_panel_line ? fb->max_panel_line - 1 : 0;
	if (fb->folder_block && fb->start_draw_line + row >= fb->folder_block)
		row = fb->folder_block - 1 - fb->start_draw_line;
	if (row > last) row = last;
	fb->start_draw_cursor_line = row * fb->line_size_y;
}

/* fb_prepare_selected_BDVK_adress */
static void fb_prepare_selected(struct file_browser *fb)
{
	u32 i = fb->start_draw_line + fb->start_draw_cursor_line / fb->line_size_y;
	fb->selected_BDVK_adress = fb->folder_data ? ENTRY(fb, i) : NULL;
}

/* a line of the panel: its background (the cursor's colour after the
 * icon), the icon, the name */
static void fb_draw_line(struct file_browser *fb, u32 row)
{
	int y = fb->start_y + row * fb->line_size_y, lh = fb->line_size_y;
	int tx = fb->start_x + 3 + fb->icon_size_x;      /* the name's place */
	u32 i = fb->start_draw_line + row;
	int cursor = fb->select_panel_counter && fb->folder_block && row * fb->line_size_y == fb->start_draw_cursor_line;
	rect(fb->start_x, y, fb->icon_size_x + 2, lh, fb->background_color);
	rect(fb->start_x + fb->icon_size_x + 2, y, fb->size_x - fb->icon_size_x - 2, lh,
	     cursor ? fb->select_color : fb->background_color);
	if (i >= fb->folder_block) return;
	const u8 *e = ENTRY(fb, i);
	const char *name = (const char *)e + 40;
	/* fb_draw_icon */
	int icon;
	if (e[0] & 0x10) icon = strcmp(name, "..") ? 0 : 1;
	else icon = fb_get_icon_number(fb, name);
	u32 isize = fb->icon_size_x * fb->icon_size_y * 3;
	if (fb->icon_raw_area && icon >= 0 && icon < 75)
		kos_call(7, (u32)(fb->icon_raw_area + icon * isize), (u32)fb->icon_size_x << 16 | fb->icon_size_y,
			 (u32)(tx - 2 - fb->icon_size_x) << 16 | (u16)(y + (lh - fb->icon_size_y) / 2), 0, 0);
	/* the name, cut to the panel */
	int max = fb->size_x / fb->font_size_x - (23 + 2 + 2 + 2);
	int n = strlen(name), cut = n > max;
	if (cut) n = max > 0 ? max : 0;
	int d = lh - fb->font_size_y, ty = y + d / 2 + (d & 1);
	text(tx, ty, fb->text_color & 0xffffff, name, n);
	if (cut) text(tx + n * fb->font_size_x, ty, fb->reduct_text_color & 0xffffff, "..", 2);
}

/* fb_draw_panel */
void FileBrowser_draw(struct file_browser *fb)
{
	fb_prepare(fb);
	for (u32 r = 0; r < fb->max_panel_line; r++) fb_draw_line(fb, r);
	/* below the last line */
	int used = fb->max_panel_line * fb->line_size_y;
	rect(fb->start_x, fb->start_y + used, fb->size_x, fb->size_y - used, fb->background_color);
	fb_prepare_selected(fb);
}

/* the cursor to entry p, the lines shown moved to it */
static void fb_move(struct file_browser *fb, s32 p)
{
	fb_prepare(fb);
	if (!fb->folder_block) return;
	if (p < 0) p = 0;
	if ((u32)p >= fb->folder_block) p = fb->folder_block - 1;
	u32 top = fb->start_draw_line, rows = fb->max_panel_line ? fb->max_panel_line : 1;
	if ((u32)p < top) top = p;
	if ((u32)p >= top + rows) top = p - rows + 1;
	if (top != fb->start_draw_line) fb->draw_scroll_bar = 1;
	fb->start_draw_line = top;
	fb->start_draw_cursor_line = (p - top) * fb->line_size_y;
	FileBrowser_draw(fb);
}

/* fb_key */
void FileBrowser_key(struct file_browser *fb)
{
	fb_prepare(fb);
	s32 p = fb->start_draw_line + fb->start_draw_cursor_line / fb->line_size_y;
	s32 rows = fb->max_panel_line;
	switch (fb->key_action) {
	case 1: fb_move(fb, p + 1); break;               /* .arrow_down */
	case 2: fb_move(fb, p - 1); break;               /* .arrow_up */
	case 3: fb_move(fb, p + rows); break;            /* .page_down */
	case 4: fb_move(fb, p - rows); break;            /* .page_up */
	case 5: fb_move(fb, 0); break;                   /* .home */
	case 6: fb_move(fb, fb->folder_block - 1); break;/* .end */
	case 7:                                          /* .enter: fb_mouse.enter_3 */
		fb->mouse_keys = fb->mouse_keys_old = 0;
		fb->mouse_keys_delta = 3;
		break;
	}
	fb_prepare_selected(fb);
}

/* fb_mouse */
void FileBrowser_mouse(struct file_browser *fb)
{
	fb->mouse_keys_old = fb->mouse_keys;
	fb->mouse_keys = kos_call(37, 2, 0, 0, 0, 0);
	u32 pos = kos_call(37, 1, 0, 0, 0, 0);
	int x = (s16)(pos >> 16), y = (s16)pos;
	fb_prepare(fb);
	if (x < fb->start_x || x > fb->start_x + fb->size_x || y < fb->start_y || y > fb->start_y + fb->size_y) {
		fb_prepare_selected(fb);
		return;
	}
	if (fb->mouse_keys) fb->select_flag = 1;
	if (!(fb->mouse_keys & 1) || (fb->mouse_keys_old & 1) || !fb->folder_block) {
		fb_prepare_selected(fb);
		return;
	}
	/* a press: the line under the pointer (the last entry below them) */
	s32 p = fb->start_draw_line + (y - fb->start_y) / fb->line_size_y;
	if ((u32)p >= fb->folder_block) p = fb->folder_block - 1;
	s32 cur = fb->start_draw_line + fb->start_draw_cursor_line / fb->line_size_y;
	u32 now = kos_call(26, 9, 0, 0, 0, 0);
	if (p == cur && fb->mouse_keys_delta == 2 && now <= fb->mouse_keys_tick) {
		/* .enter_2: the second press in time */
		fb->mouse_keys = fb->mouse_keys_old = 0;
		fb->mouse_keys_delta = 3;
	} else {
		/* .enter_1: the cursor here, the time for a second press */
		fb->mouse_keys_delta = 2;
		fb->mouse_keys_tick = now + fb->mouse_key_delay;
		if (p != cur || !fb->select_panel_counter) fb_move(fb, p);
	}
	fb_prepare_selected(fb);
}

/* --- menu_bar ------------------------------------------------------ */

/* .get_next_text: the item after s, NULL at the end */
static const char *next_text(const struct menu_bar *m, const char *s)
{
	while (s < m->text_end && *s) s++;
	return s < m->text_end ? s + 1 : NULL;
}

static u32 items(const struct menu_bar *m)
{
	u32 n = 0;
	for (const char *s = m->pos_pointer; s && s < m->text_end && *s; s = next_text(m, s)) n++;
	return n;
}

/* .draw_1: the bar */
void menu_bar_draw(struct menu_bar *m)
{
	m->size_y1 = items(m) * m->interval;
	rect(m->start_x, m->start_y, m->size_x, m->size_y, m->select == 1 ? m->frnt_col : m->bckg_col);
	text(m->start_x + 4, m->start_y + (m->size_y - m->font_height) / 2,
	     0x80000000u | ((m->select == 1 ? m->frnt_text_col : m->bckg_text_col) & 0xffffff), m->text_pointer, 0);
}

/* .draw_3: the items, the cursor's in menu_sel_col */
static void draw_items(struct menu_bar *m)
{
	u32 k = 0;
	for (const char *s = m->pos_pointer; s && s < m->text_end && *s; s = next_text(m, s), k++) {
		int y = m->start_y1 + k * m->interval, sel = k == m->cursor;
		u32 bg = sel ? m->menu_sel_col : m->menu_col;
		rect(m->start_x1, y, m->size_x1, m->interval, bg);
		kos_call(4, (u32)(m->start_x1 + 4) << 16 | (u16)(y + (m->interval - m->font_height) / 2),
			 0xc0000000u | ((sel ? m->frnt_text_col : m->bckg_text_col) & 0xffffff), (u32)s, 0, bg & 0xffffff);
	}
}

/* .get_menu_area, .put_menu_area: what the menu covers */
static u8 *menu_area;
static void get_menu_area(struct menu_bar *m)
{
	u8 pi[1024];
	free(menu_area);
	menu_area = malloc(m->size_x1 * m->size_y1 * 3 + 1);
	if (!menu_area) return;
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	u32 x = m->start_x1 + *(u32 *)(pi + 34) + *(u32 *)(pi + 54);
	u32 y = m->start_y1 + *(u32 *)(pi + 38) + *(u32 *)(pi + 58);
	kos_call(36, (u32)menu_area, (u32)m->size_x1 << 16 | m->size_y1, x << 16 | (u16)y, 0, 0);
}
static void put_menu_area(struct menu_bar *m)
{
	if (!menu_area) return;
	kos_call(7, (u32)menu_area, (u32)m->size_x1 << 16 | m->size_y1, (u32)m->start_x1 << 16 | m->start_y1, 0, 0);
	free(menu_area);
	menu_area = NULL;
}

/* .exit_menu_3 (k 0: closed) and .exit_menu_2 (k 1: the cursor's item) */
static void menu_exit(struct menu_bar *m, int k)
{
	m->select = 0;
	put_menu_area(m);
	m->cursor_out = k ? m->cursor + 1 : 0;
	if (k && m->mouse_keys == m->mouse_keys_old) m->cursor_out = 0;   /* not a new press */
	menu_bar_draw(m);
	m->click = 1;
}

/* .activate (.start_loop): the menu shown until an item is chosen or it
 * is closed */
void menu_bar_activate(struct menu_bar *m)
{
	m->select = 1;
	menu_bar_draw(m);
	u32 max = items(m);
	if (m->cursor >= max) m->cursor = 0;
	get_menu_area(m);
	rect(m->start_x1, m->start_y1, m->size_x1, m->size_y1, m->menu_col);   /* .draw_2 */
	draw_items(m);
	for (;;) {
		int ev = kos_wait_event();
		if (ev == 1 || ev == 3) {          /* a redraw, a button: closed */
			if (ev == 3) kos_get_button();
			menu_exit(m, 0);
			return;
		}
		if (ev == 2) {
			u32 key = kos_get_key();
			if (key & 0xff) continue;
			u8 ah = key >> 8;              /* scan codes; 0xE0 before the cursor keys: skipped */
			if (ah == 80) { if (m->cursor + 1 < max) m->cursor++; draw_items(m); }
			else if (ah == 72) { if (m->cursor) m->cursor--; draw_items(m); }
			else if (ah == 71 || ah == 73) { m->cursor = 0; draw_items(m); }
			else if (ah == 79 || ah == 81) { m->cursor = max ? max - 1 : 0; draw_items(m); }
			else if (ah == 1) { menu_exit(m, 0); return; }
			else if (ah == 28) {           /* Enter: as a new press */
				m->mouse_keys_old = 0;
				m->mouse_keys = 1;
				menu_exit(m, 1);
				return;
			} else if (ah == 75) { m->ret_key = 1; menu_exit(m, 0); return; }
			else if (ah == 77) { m->ret_key = 2; menu_exit(m, 0); return; }
			continue;
		}
		if (ev == 6) {                     /* .mouse_menu */
			m->mouse_keys_old = m->mouse_keys;
			m->mouse_keys = kos_call(37, 2, 0, 0, 0, 0);
			u32 pos = kos_call(37, 1, 0, 0, 0, 0);
			int x = (s16)(pos >> 16), y = (s16)pos;
			if (x <= m->start_x1 || x >= m->start_x1 + m->size_x1 || y <= m->start_y1 ||
			    y >= m->start_y1 + m->size_y1) {   /* .close */
				if ((m->mouse_keys & 1) && !(m->mouse_keys_old & 1)) {
					menu_exit(m, 0);
					return;
				}
				continue;
			}
			u32 k = (y - m->start_y1) / m->interval;
			if (k >= max) k = max ? max - 1 : 0;
			if (k != m->cursor) {
				m->cursor = k;
				draw_items(m);
			}
			if ((m->mouse_keys & 1) && !(m->mouse_keys_old & 1)) {   /* a press on an item */
				menu_exit(m, 1);
				return;
			}
		}
	}
}

/* .mouse: the bar lit under the pointer; pressed, its menu (a bar of
 * type 1 is chosen itself: cursor_out 1) */
void menu_bar_mouse(struct menu_bar *m)
{
	m->mouse_keys_old = m->mouse_keys;
	m->mouse_keys = kos_call(37, 2, 0, 0, 0, 0);
	u32 pos = kos_call(37, 1, 0, 0, 0, 0);
	int x = (s16)(pos >> 16), y = (s16)pos;
	if (x < m->start_x || x > m->start_x + m->size_x - 1 || y < m->start_y || y > m->start_y + m->size_y) {
		/* .exit_menu */
		if (m->select) {
			m->select = 0;
			menu_bar_draw(m);
		}
		m->click = 0;
		return;
	}
	if (!(m->mouse_keys & 1)) {
		if (m->select != 1) {
			m->select = 1;
			menu_bar_draw(m);
		}
		m->click = 0;
		return;
	}
	int fresh = m->get_mouse_flag == 1 || m->mouse_keys != m->mouse_keys_old;
	m->get_mouse_flag = 0;
	if (!fresh) {
		m->click = 0;
		return;
	}
	if (m->type == 1) {
		m->cursor_out = 1;
		m->click = 1;
		return;
	}
	m->mouse_keys_old = m->mouse_keys;   /* the press that opened it is no choice */
	menu_bar_activate(m);
}

/* --- PathShow -------------------------------------------------------- */

/* .prepare: the text, or its first 6 characters, "..." and its end */
void PathShow_prepare(struct path_show *p)
{
	size_t n = strlen(p->text_pointer);
	if (n * p->font_size_x < p->area_size_x) {
		strcpy(p->work_area_pointer, p->text_pointer);
		return;
	}
	int max = p->area_size_x / p->font_size_x - 9;
	if (max < 0) max = 0;
	memcpy(p->work_area_pointer, p->text_pointer, 6);
	memcpy(p->work_area_pointer + 6, "...", 3);
	memcpy(p->work_area_pointer + 9, p->text_pointer + n - max, max);
	p->work_area_pointer[9 + max] = 0;
}

/* .draw */
void PathShow_draw(struct path_show *p)
{
	text(p->start_x, p->start_y, 0x80000000u | (p->font_color & 0xffffff), p->work_area_pointer, 0);
}
