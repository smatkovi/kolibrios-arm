/* KolibriOS for ARM: OpenDialog (lib/proc_lib/proc_lib.h), after
 * proc_lib's opendial.mac (Marat Zakiyanov aka Mario79) for the
 * structure and its fields, and the dialog program programs/fs/opendial
 * (the KolibriOS team) for what it shows and its keys, GPL-2.0.
 *
 * The dialog runs in the caller's window (proc_lib.h says why): a
 * caption, the folder's path (editable: Enter goes there) with a button
 * to the folder above, the folder's list (folders first, then the files
 * of the filter's extensions, by name, with their sizes) and its scroll
 * bar, the file's name, the filter's switch and the buttons. Keys as
 * opendial's: Up, Down, Page Up, Page Down, Home, End move the cursor,
 * Enter enters a folder or chooses the file, Backspace goes up, Ctrl+R
 * reads the folder again, Escape cancels; Tab goes between the list and
 * the name. The mouse: a click puts the cursor, a second click on the
 * same line (or a double click) is Enter, the wheel scrolls. The folders
 * are read with function 70.1. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <kos.h>
#include <box_lib.h>
#include "proc_lib.h"

typedef short s16;

#define CAPT_H 20
#define ROW_H 16
#define PAD 6
#define BTN_H 22
#define ID_BASE 0xf100
enum { ID_CLOSE = ID_BASE + 1, ID_OK, ID_CANCEL, ID_UP, ID_FILTER };

struct entry { char *name; u32 attr; u32 size; };

static system_colors sc;
static open_dialog *od;
static char dir[1024];
static struct entry *ent;
static int nent, cur, top, filter_on = 1;
static int dx, dy, dw, dh;             /* the dialog in the window */
static int lx, ly, lw, lh, rows;       /* its list */
static char path_buf[1026], name_buf[258];
static u32 mouse_dd;
static edit_box ed_path, ed_name;
static scrollbar sb;
static void *focus;                    /* the list (&ent), or one of the boxes */
static int done, last_click_row = -1;
static u32 last_click_time;

static const char *title(void)
{
	return od->type == 1 ? "Save file" : od->type == 2 ? "Select folder" : "Open file";
}

/* the filter's extensions: u32 size, then the extensions ended by 0 */
static int filter_match(const char *name)
{
	const u32 *f = od->filter_area;
	if (!filter_on || !f || !*f || od->type == 2) return 1;
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	const char *e = (const char *)(f + 1), *end = e + *f;
	while (e < end && *e) {
		if (!strcasecmp(dot + 1, e)) return 1;
		e += strlen(e) + 1;
	}
	return 0;
}

static int cmp_entry(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	int fx = x->attr & 0x10, fy = y->attr & 0x10;
	if (!strcmp(x->name, "..")) return -1;
	if (!strcmp(y->name, "..")) return 1;
	if (fx != fy) return fx ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static void free_list(void)
{
	for (int i = 0; i < nent; i++) free(ent[i].name);
	free(ent);
	ent = NULL;
	nent = 0;
}

/* the folder dir read (70.1, 64 entries at a time) */
static void read_dir(void)
{
	free_list();
	int cap = 64;
	ent = malloc(cap * sizeof(*ent));
	u8 *buf = malloc(32 + 64 * 304);
	if (!ent || !buf) { free(buf); return; }
	if (strcmp(dir, "/")) {
		ent[nent].name = strdup("..");
		ent[nent].attr = 0x10;
		ent[nent++].size = 0;
	}
	for (u32 first = 0;;) {
		struct kos_r2 r = kos_read_folder(dir, buf, first, 64);
		if ((r.eax && r.eax != 6) || (s32)r.ebx <= 0) break;
		for (u32 i = 0; i < r.ebx; i++) {
			u8 *e = buf + 32 + i * 304;
			const char *n = (const char *)e + 40;
			u32 attr = *(u32 *)e;
			if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
			if (attr & 8) continue;            /* a volume label */
			if (!(attr & 0x10) && !filter_match(n)) continue;
			if (nent == cap) {
				struct entry *m = realloc(ent, (cap *= 2) * sizeof(*ent));
				if (!m) break;
				ent = m;
			}
			ent[nent].name = strdup(n);
			ent[nent].attr = attr;
			ent[nent++].size = *(u32 *)(e + 32);
		}
		first += r.ebx;
		if (r.eax == 6 || r.ebx < 64) break;
	}
	free(buf);
	qsort(ent, nent, sizeof(*ent), cmp_entry);
	cur = top = 0;
	edit_box_set_text(&ed_path, dir);
}

/* the folder above */
static void go_up(void)
{
	char *s = strrchr(dir, '/');
	if (!s) return;
	char was[264];
	strncpy(was, s + 1, sizeof(was) - 1);
	was[sizeof(was) - 1] = 0;
	if (s == dir) dir[1] = 0;
	else *s = 0;
	read_dir();
	for (int i = 0; i < nent; i++)
		if (!strcmp(ent[i].name, was)) { cur = i; break; }
}

static void enter_dir(const char *name)
{
	if (!strcmp(name, "..")) { go_up(); return; }
	size_t n = strlen(dir);
	if (n + strlen(name) + 2 >= sizeof(dir)) return;
	if (n > 1) dir[n++] = '/';
	else if (!n) dir[n++] = '/';
	strcpy(dir + n, name);
	read_dir();
}

static void finish(int ok)
{
	if (ok) {
		const char *name = name_buf;
		size_t n = strlen(dir);
		if (od->type == 2) {
			strcpy(od->openfile_path, dir);
		} else {
			strcpy(od->openfile_path, dir);
			if (n > 1) strcat(od->openfile_path, "/");
			strcat(od->openfile_path, name);
			if (od->filename_area) {
				strncpy(od->filename_area, name, 255);
				od->filename_area[255] = 0;
			}
		}
		if (od->opendir_path) strcpy(od->opendir_path, dir);
	}
	od->status = ok ? 1 : 0;
	done = 1;
}

/* Enter on the list's cursor, or OK */
static void accept(void)
{
	if (focus == &ent && cur < nent) {
		struct entry *e = &ent[cur];
		if (e->attr & 0x10) {
			if (od->type == 2 && strcmp(e->name, "..")) {
				enter_dir(e->name);
				finish(1);
				return;
			}
			enter_dir(e->name);
			return;
		}
		edit_box_set_text(&ed_name, e->name);
	}
	if (od->type != 2 && !name_buf[0]) return;    /* no name */
	/* a name typed that is a folder: into it */
	for (int i = 0; i < nent; i++)
		if ((ent[i].attr & 0x10) && !strcmp(ent[i].name, name_buf)) {
			enter_dir(name_buf);
			edit_box_set_text(&ed_name, "");
			return;
		}
	finish(1);
}

static void layout(void)
{
	u8 pi[1024];
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	int cw = *(s32 *)(pi + 62) + 1, ch = *(s32 *)(pi + 66) + 1;
	dw = od->x_size ? od->x_size : 420;
	dh = od->y_size ? od->y_size : 320;
	if (dw > cw) dw = cw;
	if (dh > ch) dh = ch;
	dx = (cw - dw) / 2;
	dy = (ch - dh) / 2;
	lx = dx + PAD;
	ly = dy + CAPT_H + PAD + BTN_H + PAD;
	lw = dw - 2 * PAD - 16;
	lh = dh - (ly - dy) - PAD - 2 * BTN_H - 2 * PAD;
	if (lh < ROW_H) lh = ROW_H;
	rows = lh / ROW_H;
	ed_path.left = dx + PAD + BTN_H + 4;
	ed_path.top = dy + CAPT_H + PAD + 4;
	ed_path.width = dw - 2 * PAD - BTN_H - 6;
	ed_name.left = dx + PAD + 66;
	ed_name.top = ly + lh + PAD + 4;
	ed_name.width = dw - 2 * PAD - 68;
	sb.x_pos = lx + lw;
	sb.x_size = 16;
	sb.y_pos = ly;
	sb.y_size = lh;
}

static void text(int x, int y, const char *s, int n, u32 c)
{
	kos_call(4, (u32)x << 16 | (u16)y, c & 0xffffff, (u32)s, n, 0);
}

static void button(int x, int y, int w, u32 id, const char *s)
{
	kos_button(x, y, w, BTN_H, id, sc.work_button);
	text(x + (w - (int)strlen(s) * 6) / 2, y + (BTN_H - 9) / 2, s, strlen(s), sc.work_button_text);
}

static void draw_list(void)
{
	if (cur >= nent) cur = nent ? nent - 1 : 0;
	if (cur < top) top = cur;
	if (cur >= top + rows) top = cur - rows + 1;
	if (top < 0) top = 0;
	kos_rect(lx, ly, lw, lh, 0xffffff);
	for (int r = 0; r < rows && top + r < nent; r++) {
		struct entry *e = &ent[top + r];
		int y = ly + r * ROW_H, sel = top + r == cur;
		u32 fg = 0;
		if (sel) {
			kos_rect(lx, y, lw, ROW_H, focus == &ent ? sc.work_button : sc.work_dark);
			fg = sc.work_button_text;
		}
		if (e->attr & 0x10) {              /* a folder: a small yellow one */
			kos_rect(lx + 3, y + 5, 10, 8, 0xe0b040);
			kos_rect(lx + 3, y + 3, 5, 2, 0xe0b040);
		} else {                           /* a file: a page */
			kos_rect(lx + 4, y + 3, 8, 10, 0x808080);
			kos_rect(lx + 5, y + 4, 6, 8, 0xffffff);
		}
		int maxc = (lw - 20 - 66) / 6, n = strlen(e->name);
		text(lx + 18, y + (ROW_H - 9) / 2, e->name, n > maxc ? maxc : n, fg);
		if (!(e->attr & 0x10)) {
			char num[16];
			int k = 0;
			u32 v = e->size;
			do num[15 - k++] = '0' + v % 10; while ((v /= 10) && k < 15);
			text(lx + lw - 4 - k * 6, y + (ROW_H - 9) / 2, num + 16 - k, k, fg);
		}
	}
	sb.max_area = nent;
	sb.cur_area = rows;
	sb.position = top;
	sb.bg_color = sc.work;
	sb.front_color = sc.work_button;
	sb.line_color = sc.work_graph;
	sb.all_redraw = 1;
	scrollbar_v_draw(&sb);
}

static void draw(void)
{
	layout();
	kos_rect(dx, dy, dw, dh, sc.frame);
	kos_rect(dx + 1, dy + 1, dw - 2, CAPT_H - 1, sc.grab);
	kos_rect(dx + 1, dy + CAPT_H, dw - 2, dh - CAPT_H - 1, sc.work);
	kos_call(4, (u32)(dx + 8) << 16 | (u16)(dy + 6), 0x80000000u | (sc.grab_text & 0xffffff), (u32)title(), 0, 0);
	int cx = dx + dw - CAPT_H, cy = dy + 2;
	kos_button(cx, cy, CAPT_H - 4, CAPT_H - 4, ID_CLOSE, sc.grab);
	kos_line(cx + 4, cy + 4, cx + CAPT_H - 9, cy + CAPT_H - 9, sc.grab_text);
	kos_line(cx + CAPT_H - 9, cy + 4, cx + 4, cy + CAPT_H - 9, sc.grab_text);
	/* the folder above, the path */
	int by = dy + CAPT_H + PAD;
	kos_button(dx + PAD, by, BTN_H, BTN_H, ID_UP, sc.work_button);
	for (int i = 0; i < 5; i++)                /* an arrow up */
		kos_rect(dx + PAD + 11 - i, by + 7 + i, 2 * i + 1, 1, sc.work_button_text);
	kos_rect(dx + PAD + 10, by + 12, 3, 5, sc.work_button_text);
	edit_box_draw(&ed_path);
	draw_list();
	/* the name, the filter, the buttons */
	int ny = ly + lh + PAD;
	text(dx + PAD, ny + 8, od->type == 2 ? "Folder:" : "Name:", od->type == 2 ? 7 : 5, sc.work_text);
	edit_box_draw(&ed_name);
	int b2 = ny + BTN_H + PAD;
	int bw = 80;
	button(dx + dw - PAD - bw, b2, bw, ID_CANCEL, "Cancel");
	button(dx + dw - 2 * PAD - 2 * bw, b2, bw, ID_OK, od->type == 1 ? "Save" : od->type == 2 ? "Select" : "Open");
	if (od->type != 2 && od->filter_area && *(u32 *)od->filter_area)
		button(dx + PAD, b2, 100, ID_FILTER, filter_on ? "Filter: on" : "Filter: off");
}

static void move_cursor(int to)
{
	if (to >= nent) to = nent - 1;
	if (to < 0) to = 0;
	if (to == cur) return;
	cur = to;
	if (focus == &ent && cur < nent && !(ent[cur].attr & 0x10) && od->type != 2) edit_box_set_text(&ed_name, ent[cur].name);
	draw_list();
}

static void set_focus(void *f)
{
	focus = f;
	ed_name.flags = (ed_name.flags & ~ed_focus) | (f == &ed_name ? ed_focus : 0);
	ed_path.flags = (ed_path.flags & ~ed_focus) | (f == &ed_path ? ed_focus : 0);
	edit_box_draw(&ed_name);
	edit_box_draw(&ed_path);
	draw_list();
}

static void key(u32 k)
{
	u8 c = k >> 8, sc_ = k >> 16;
	u32 mods = kos_call(66, 3, 0, 0, 0, 0);
	if (c == 27) { finish(0); return; }
	if (c == 9) {                              /* Tab: the list, the name, the path */
		set_focus(focus == &ent ? (void *)&ed_name : focus == &ed_name ? (void *)&ed_path : (void *)&ent);
		return;
	}
	if (focus == &ed_path) {
		if (c == 13) {
			strncpy(dir, path_buf, sizeof(dir) - 1);
			size_t n = strlen(dir);
			while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
			if (!n) strcpy(dir, "/");
			read_dir();
			set_focus(&ent);
			return;
		}
		edit_box_key(&ed_path, k);
		return;
	}
	if (focus == &ed_name) {
		if (c == 13) { accept(); return; }
		edit_box_key(&ed_name, k);
		return;
	}
	if ((mods & 0x0c) && sc_ == 0x13) { read_dir(); draw_list(); return; }   /* Ctrl+R */
	switch (c) {
	case 0xb2: move_cursor(cur - 1); return;  /* up */
	case 0xb1: move_cursor(cur + 1); return;  /* down */
	case 0xb8: move_cursor(cur - rows); return;   /* page up */
	case 0xb7: move_cursor(cur + rows); return;   /* page down */
	case 0xb4: move_cursor(0); return;        /* home */
	case 0xb5: move_cursor(nent - 1); return; /* end */
	case 13: accept(); if (!done) { draw_list(); edit_box_draw(&ed_path); } return;
	case 8: go_up(); draw_list(); edit_box_draw(&ed_path); return;
	}
	/* a character: into the name */
	if (c >= 32 && od->type != 2) {
		set_focus(&ed_name);
		edit_box_key(&ed_name, k);
	}
}

static void mouse(void)
{
	edit_box_mouse(&ed_path);
	edit_box_mouse(&ed_name);
	if (ed_path.flags & ed_focus) focus = &ed_path;
	else if (ed_name.flags & ed_focus) focus = &ed_name;
	if (sb.max_area > sb.cur_area) {
		sb.position = top;
		scrollbar_v_mouse(&sb);
		if (sb.redraw) {
			sb.redraw = 0;
			top = sb.position;
			if (cur < top) cur = top;
			if (cur >= top + rows) cur = top + rows - 1;
			draw_list();
		}
		if (sb.delta2) return;
	}
	u32 p = kos_call(37, 1, 0, 0, 0, 0);
	int x = (s16)(p >> 16), y = (s16)p;
	s16 wheel = kos_call(37, 7, 0, 0, 0, 0);
	if (wheel && x >= lx && x < lx + lw + 16 && y >= ly && y < ly + lh) {
		top += wheel * 3;
		if (top > nent - rows) top = nent - rows;
		if (top < 0) top = 0;
		if (cur < top) cur = top;
		if (cur >= top + rows) cur = top + rows - 1;
		draw_list();
	}
	u32 b = kos_call(37, 3, 0, 0, 0, 0);
	if (!(b & 0x100)) return;                  /* the left button pressed */
	if (x < lx || x >= lx + lw || y < ly || y >= ly + lh) return;
	int r = top + (y - ly) / ROW_H;
	if (r >= nent) return;
	u32 now = kos_call(26, 9, 0, 0, 0, 0);
	if (focus != &ent) set_focus(&ent);
	if ((b & 0x01000000) || (r == last_click_row && r == cur && now - last_click_time < 60)) {
		last_click_row = -1;
		cur = r;
		accept();
		if (!done) draw();
		return;
	}
	last_click_row = r;
	last_click_time = now;
	move_cursor(r);
}

void OpenDialog_init(open_dialog *d)
{
	/* .copy_dir_path: the folder of the file there */
	if (!d->openfile_path || !d->openfile_path[0]) {
		if (d->opendir_path) d->opendir_path[0] = 0;
		return;
	}
	strcpy(d->opendir_path, d->openfile_path);
	if (d->type == 2) return;
	char *s = strrchr(d->opendir_path, '/');
	if (s) *s = 0;
}

void OpenDialog_start(open_dialog *d)
{
	od = d;
	done = 0;
	kos_call(48, 3, (u32)&sc, sizeof(sc), 0, 0);
	/* the keys as characters while the dialog is up */
	u32 mode = kos_call(66, 2, 0, 0, 0, 0);
	kos_call(66, 1, 0, 0, 0, 0);
	u32 mask = kos_event_mask(0x27);
	const char *start = d->opendir_path && d->opendir_path[0] ? d->opendir_path : d->dir_default_path;
	strncpy(dir, start && start[0] ? start : "/sys", sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = 0;
	edit_box e0 = EDIT_BOX(200, 0, 0, 0xffffff, 0xa4c4e4, 0, 0, 0, 1024, path_buf, &mouse_dd, 0, 0, 0);
	edit_box e1 = EDIT_BOX(200, 0, 0, 0xffffff, 0xa4c4e4, 0, 0, 0, 255, name_buf, &mouse_dd, 0, 0, 0);
	ed_path = e0;
	ed_name = e1;
	edit_boxes_set_sys_color(&ed_path, 1, &sc);
	edit_boxes_set_sys_color(&ed_name, 1, &sc);
	scrollbar s0 = SCROLLBAR(16, 0, 100, 0, 15, 0, 0, 0, 0, 0, 0, 1);
	sb = s0;
	layout();
	read_dir();
	edit_box_set_text(&ed_name, d->type != 2 && d->filename_area ? d->filename_area : "");
	focus = &ent;
	if (d->type == 1) {
		focus = &ed_name;
		ed_name.flags |= ed_focus;
	}
	for (int i = 0; d->filename_area && i < nent; i++)
		if (!strcmp(ent[i].name, d->filename_area)) cur = i;
	if (d->draw_window) d->draw_window();
	draw();
	while (!done) {
		int ev = kos_wait_event();
		if (ev == 1) {
			if (d->draw_window) d->draw_window();
			draw();
		} else if (ev == 2) {
			u32 k = kos_get_key();
			if ((k & 0xff) == 0) key(k);
		} else if (ev == 3) {
			u32 id = kos_get_button() >> 8;
			if (id == ID_CLOSE || id == ID_CANCEL || id == 1) finish(0);
			else if (id == ID_OK) { void *f = focus; focus = NULL; accept(); focus = f; if (!done) draw(); }
			else if (id == ID_UP) { go_up(); draw(); }
			else if (id == ID_FILTER) { filter_on ^= 1; read_dir(); draw(); }
		} else if (ev == 6) {
			mouse();
		}
	}
	for (u32 id = ID_CLOSE; id <= ID_FILTER; id++) kos_button_delete(id);
	free_list();
	kos_call(66, 1, mode, 0, 0, 0);
	kos_event_mask(mask);
	if (d->draw_window) d->draw_window();
}
