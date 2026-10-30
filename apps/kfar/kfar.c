/* kfar for KolibriOS for ARM: the start, the events, the screens, the
 * console and its drawing (kfar.asm: start ... F12, init_console,
 * draw_window, draw_image, draw_keybar, draw_cmdbar).
 *
 * Translated to C from kfar's kfar.asm and dialogs.inc (get_ascii_char),
 * diamond (Evgeny Grechnikov) and the KolibriOS team, GPL-2.0.
 *
 * Differences from the original, for KolibriOS for ARM:
 * - the window's frame is function 9's client offset (+54) instead of
 *   the fixed 5 pixels: Hildon's look has maximised windows without a
 *   frame (and 48.4 = 0); there the window is treated as maximised (it
 *   cannot be resized to whole characters, function 67 does nothing) and
 *   Alt+F9 does nothing;
 * - libini is linked in (no 68.19), so the settings are always read;
 *   the plugins of [Plugins] are not loaded (see kfar.h). */
#include "kfar.h"
#include "font_data.h"

/* the window's frame: 5 pixels in KolibriOS's skinned windows, 0 in
 * Hildon's look (function 9: the client area's x offset) */
static int wnd_border = 5;
int kfar_wnd_border(void) { return wnd_border; }
static u8 procinfo_buf[1024];

static int min_y, max_y, min_x, max_x;
/* the cells under the cursor now and before (indices, -1: none) */
static int cur_cursor_pos = -1, old_cursor_pos = -1;

static const char aConfirmations[] = "Confirmations";
static const char aConfirmDelete[] = "Delete";
static const char aConfirmDeleteIncomplete[] = "DeleteIncomplete";
static const char aPanels[] = "Panels";
static const char aLeftViewMode[] = "LeftViewMode";
static const char aRightViewMode[] = "RightViewMode";
static const char aLeftSortMode[] = "LeftSortMode";
static const char aRightSortMode[] = "RightSortMode";
static const char aEditor[] = "Editor";
static const char aEolStyle[] = "EOLStyle";
static const char aAssociations[] = "Associations";
static const char aFolderShortcuts[] = "FolderShortcuts";
static char aShortcut[] = "Shortcut0";
#define aShortcut_d aShortcut[8]

/* function 65: an image with a palette, ebp = the padding at the end of
 * each row (r6 here) */
static inline void put_image_ext(const void *ebx, u32 ecx, u32 edx, u32 esi, const void *edi, int ebp)
{
	register u32 r0 asm("r0") = 65;
	register u32 r1 asm("r1") = (u32)ebx;
	register u32 r2 asm("r2") = ecx;
	register u32 r3 asm("r3") = edx;
	register u32 r4 asm("r4") = esi;
	register u32 r5 asm("r5") = (u32)edi;
	register u32 r6 asm("r6") = (u32)ebp;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6) :: "memory");
}

static int enum_associations_callback(const char *f_name, const char *sec_name, const char *key_name,
				      const char *key_value);
static void get_event_loop(void);
static void key(void);
static void mouse(void);
static void redraw_resize(void);
static void redraw_resize_draw(void);

/* kfar.asm: start */
int main(int argc, char **argv)
{
	kos_event_mask(3u << 30 | 0x27);
	/* identical_table, tolower_table (cp866: A-Z, the Cyrillic А-Я, Ё) */
	for (int i = 0; i < 256; i++) identical_table[i] = i;
	for (int i = 0; i < 256; i++) tolower_table[i] = i;
	for (int i = 'A'; i <= 'Z'; i++) tolower_table[i] = i + 0x20;
	for (int i = 0x80; i < 0x90; i++) tolower_table[i] = i + 0x20;     /* А-П */
	for (int i = 0x90; i < 0xA0; i++) tolower_table[i] = i + 0x50;     /* Р-Я */
	tolower_table[0xF0] = 0xF1;                                         /* Ё */
	/* isspace_table: 1 for everything that is not a letter, digit or _ */
	for (int i = 0; i < 256; i++) isspace_table[i] = 1;
	for (int i = 'A'; i <= 'Z'; i++) isspace_table[i] = 0;
	isspace_table['_'] = 0;
	for (int i = 'a'; i <= 'z'; i++) isspace_table[i] = 0;
	for (int i = 0x80; i <= 0xAF; i++) isspace_table[i] = 0;            /* А-п */
	for (int i = 0xE0; i <= 0xF1; i++) isspace_table[i] = 0;            /* р-ё */
	/* (the original's table has the digits as "spaces" too) */
	SearchStringEditBuf.maxlength = SearchStringEditBuf_length;
	SearchStringEditBuf.pos = SearchStringEditBuf.start = 0;
	SearchStringEditBuf.data[0] = 0;
	strncpy(app_path, argv[0], sizeof(app_path) - 5);
	init_console();
	draw_window();
	kos_call(66, 1, 1, 0, 0, 0);           /* set keyboard mode to scancodes */
	panel1.nfa = panel2.nfa = 200;
	panel1.files = pgalloc(200 * 4 + 32 + 200 * 304);
	panel2.files = pgalloc(200 * 4 + 32 + 200 * 304);
	if (!panel1.files || !panel2.files) kfar_exit();
	panel1.sortmode = panel2.sortmode = 0;  /* sort by name */
	panel1.colmode = panel2.colmode = 2;    /* the "medium" mode */
	num_screens = 1;
	screens = pgalloc(0x1000);
	if (!screens) kfar_exit();
	screens[0].vtable = &panels_vtable;
	active_screen_vtable = &panels_vtable;
	active_screen_keybar = keybar_panels;
	/* kfar.ini (libini is linked in: always there) */
	strcat(app_path, ".ini");
	bConfirmDelete = ini_get_int(app_path, aConfirmations, aConfirmDelete, 1);
	bConfirmDeleteIncomplete = ini_get_int(app_path, aConfirmations, aConfirmDeleteIncomplete, 0);
	int eax = ini_get_int(app_path, aPanels, aLeftViewMode, 2);
	if (eax >= 1 && eax <= 4) panel1.colmode = eax;
	eax = ini_get_int(app_path, aPanels, aRightViewMode, 2);
	if (eax >= 1 && eax <= 4) panel2.colmode = eax;
	eax = ini_get_int(app_path, aPanels, aLeftSortMode, 0);
	if ((u32)eax < 14) panel1.sortmode = eax;
	eax = ini_get_int(app_path, aPanels, aRightSortMode, 0);
	if ((u32)eax < 14) panel2.sortmode = eax;
	ini_get_str(app_path, aEditor, aEolStyle, saved_file_name, 512, nullstr);
	u8 cl = edit_eol_unix;
	if ((saved_file_name[0] | 0x20) == 'd') cl = edit_eol_dos;
	else if ((saved_file_name[0] | 0x20) == 'm') cl = edit_eol_mac;
	EditEOLStyle = cl;
	associations = xpgalloc(0x1000);
	if (associations)
		ini_enum_keys(app_path, aAssociations, enum_associations_callback);
	/* .skip_assoc: the folders of RCtrl+0..9 */
	for (aShortcut_d = '0'; aShortcut_d <= '9'; aShortcut_d++) {
		char *edi = saved_file_name;
		ini_get_str(app_path, aFolderShortcuts, aShortcut, edi, 512, nullstr);
		if (!*edi) continue;
		int ecx = strlen(edi) + 1;
		if (ecx != 2 && edi[ecx - 2] == '/') edi[ecx - 2] = 0;
		char *p = xpgalloc(ecx);
		if (!p) {
			memset(FolderShortcuts, 0, sizeof(FolderShortcuts));
			break;
		}
		strcpy(p, edi);
		FolderShortcuts[aShortcut_d - '0'] = p;
	}
	/* .skip_shortcuts: the plugins are not loaded (see kfar.h), so
	 * EditPlugInfo stays 0, as with no plugins in the original */
	EditPlugInfo = 0;
	/* .noini */
	editor_calc_datasize();
	strcpy(panel1.dir, def_left_dir);
	strcpy(panel2.dir, def_right_dir);
	draw_keybar();
	draw_cmdbar();
	prev_dir[0] = 0;
	calc_colwidths(&panel1);
	read_folder(&panel1);
	draw_panel(&panel1);
	bSilentFolderMode = 1;
	calc_colwidths(&panel2);
	read_folder(&panel2);
	draw_panel(&panel2);
	get_event_loop();
	return 0;
}

/* kfar.asm: event */
static void get_event_loop(void)
{
	for (;;) {
		int eax = get_event();
		if (eax == 1) redraw();
		else if (eax == 2) key();
		else if (eax == 6) mouse();
		else if (eax == 3) kfar_exit();     /* button: we have only one, close */
	}
}

/* kfar.asm: exit */
void kfar_exit(void)
{
	/* close all screens */
	while (num_screens) {
		screen *s = &screens[num_screens - 1];
		s->vtable->OnExit(s->data);
		pgfree(s->data);
		num_screens--;
	}
	/* (no plugins to unload) */
	ini_set_int(app_path, aPanels, aLeftViewMode, panel1.colmode);
	ini_set_int(app_path, aPanels, aRightViewMode, panel2.colmode);
	ini_set_int(app_path, aPanels, aLeftSortMode, panel1.sortmode);
	ini_set_int(app_path, aPanels, aRightSortMode, panel2.sortmode);
	kos_exit();
	__builtin_unreachable();
}

void exit_handler(void *ebp, u32 eax) { kfar_exit(); }

/* kfar.asm: get_event */
int get_event(void)
{
	for (;;) {
		if (idle_interval == -1)
			return kos_wait_event();
		int eax = kos_wait_event_timeout(idle_interval);
		if (eax) return eax;
		if (active_screen_vtable->OnIdle)
			active_screen_vtable->OnIdle(active_screen_data);
	}
}

/* kfar.asm: redraw */
void redraw(void)
{
	/* query kbd state from OS */
	u8 al = kos_call(66, 3, 0, 0, 0, 0) & 0x3F;
	if (al != ctrlstate) {
		ctrlstate = al;
		draw_keybar();
	}
	kos_thread_info(procinfo_buf, -1);
	u8 *ebx = procinfo_buf;
	/* test if rolled up: the height of a rolled up window is [skinh]+3 */
	if (*(int *)(ebx + 46) - skinh <= 5) {
		kos_begin_draw();
		/* ebx, ecx, edi are ignored by function 0 after the first redraw */
		kos_call(0, 0, 0, 0x53000000, 0, 0);
		kos_end_draw();
		return;
	}
	wnd_border = *(int *)(ebx + 54);
	bool ch = false, cl;
	wnd_width = *(int *)(ebx + 42);
	int eax = wnd_width - (wnd_border * 2 - 1);
	if (eax < 0) eax = 0;
	int edx = eax % font_width;
	eax /= font_width;
	if (eax < min_width) { eax = min_width; ch = true; }
	if (eax > max_width) { eax = max_width; ch = true; }
	cl = eax != cur_width;
	cur_width = eax;
	cl |= ch;
	fill_width = edx;
	ch = edx != 0;
	wnd_height = *(int *)(ebx + 46);
	eax = wnd_height - skinh - (wnd_border - 1);
	if (eax < 0) eax = 0;
	edx = eax % font_height;
	eax /= font_height;
	if (eax < min_height) { eax = min_height; cl = true; }
	if (eax > max_height) { eax = max_height; cl = true; }
	fill_height = edx;
	bool resized = eax != cur_height;
	cur_height = eax;
	if (!resized && !cl) {
		/* the remainders: resize to whole characters, unless maximised
		 * (a window without a frame: Hildon's look, maximised by the
		 * system) */
		if (!(edx || ch) || (ebx[70] & 1) || wnd_border == 0) {
			draw_window();
			return;
		}
	}
	redraw_resize();
}

/* kfar.asm: redraw.resize */
static void redraw_resize(void)
{
	if (wnd_border) {
		kos_call(67, -1, -1, cur_width * font_width + wnd_border * 2 - 1,
			 cur_height * font_height + skinh + wnd_border - 1, 0);
		redraw_resize_draw();
		return;
	}
	/* a maximised window without a frame (Hildon's look): no resize,
	 * so no redraw event follows; the remainders painted here */
	redraw_resize_draw();
	draw_window();
}

/* kfar.asm: redraw.resize_draw */
static void redraw_resize_draw(void)
{
	pgfree(MemForImage);
	MemForImage = 0;
	init_console();
	draw_keybar();
	cursor_size = cursor_normal_size;
	active_screen_vtable->OnRedraw(active_screen_data);
}

/* kfar.asm: alt_f9 (the window as large as the work area and back; not
 * in Hildon's look, where it is maximised already) */
void alt_f9(void *ebp, u32 eax)
{
	if (!wnd_border) return;
	if (saved_width != -1) {
		cur_width = saved_width;
		saved_width = -1;
		cur_height = saved_height;
		saved_height = -1;
		redraw_resize();
		return;
	}
	struct kos_r2 r = kos_call2(48, 5, 0, 0, 0, 0);
	u32 a = r.eax, b = r.ebx;
	int w = ((a & 0xffff) - (a >> 16) + 1) & 0xffff;
	w = (w - wnd_border * 2) / font_width;
	saved_width = cur_width;
	cur_width = w;
	int h = (b & 0xffff) - (b >> 16) - (wnd_border - 1) - skinh;
	h /= font_height;
	saved_height = cur_height;
	cur_height = h;
	kos_call(67, a >> 16, b >> 16, cur_width * font_width + wnd_border * 2 - 1,
		 cur_height * font_height + skinh + wnd_border - 1, 0);
	redraw_resize_draw();
}

/* kfar.asm: key */
static void key(void)
{
	u32 eax = kos_get_key();
	if (eax & 0xff) return;
	u8 al = eax >> 8;
	if (al == 0xE0) {
		bWasE0 = 1;
		return;
	}
	u8 ah = bWasE0;
	bWasE0 = 0;
	switch (al) {
	case 0x1D:                              /* .ctrl_down */
		ctrlstate |= ah ? 8 : 4;
		break;
	case 0x9D:                              /* .ctrl_up */
		ctrlstate &= ah ? ~8 : ~4;
		break;
	case 0x2A:                              /* .lshift_down */
		/* ignore E0 2A sequence: e.g. arrow keys with NumLock on
		 * generate E0 2A E0 xx / E0 xx+80 E0 AA where xx is the
		 * scancode, so we can safely ignore E0 2A */
		if (ah) return;
		ctrlstate |= 1;
		break;
	case 0xAA:                              /* .lshift_up: ignore E0 AA */
		if (ah) return;
		ctrlstate &= ~1;
		break;
	case 0x36:                              /* .rshift_down */
		ctrlstate |= 2;
		break;
	case 0xB6:                              /* .rshift_up */
		ctrlstate &= ~2;
		break;
	case 0x38:                              /* .alt_down */
		ctrlstate |= ah ? 0x20 : 0x10;
		break;
	case 0xB8:                              /* .alt_up */
		ctrlstate &= ah ? ~0x20 : ~0x10;
		break;
	default:
		active_screen_vtable->OnKey(active_screen_data, al | (u32)ah << 8);
		return;
	}
	/* .keybar */
	draw_keybar();
	draw_image();
}

/* kfar.asm: get_keybar_ind: in: eax = the x coordinate; out: the key
 * bar's index (0 to 95), 0 for F1 */
static int get_keybar_ind(int eax)
{
	int edi = (eax - wnd_border) / font_width;
	int esi = 0;
	if (ctrlstate & 3) esi |= 1;
	if (ctrlstate & 0xC) esi |= 2;
	if (ctrlstate & 0x30) esi |= 4;
	esi *= 12;                              /* the number of buttons F1-F12 */
	int ecx = 1, ebx = 6;
	eax = cur_width - (11 + 9 + 3 * 2 + 6);
	if (eax >= 7 * 11) ebx = eax / 11;
	int edx = 0;
	for (;;) {
		edx += 7;
		if (ecx >= 10) edx++;
		if (edx > edi || edx > cur_width || ecx == 12) break;
		edx += ebx - 6 + 1;
		if (edx > cur_width) break;
		ecx++;
	}
	return esi + ecx - 1;
}

/* kfar.asm: mouse */
static void mouse(void)
{
	u32 eax = kos_call(37, 3, 0, 0, 0, 0);
	if (eax & (1 << 24)) mousestate = 1;            /* left button double click */
	else if (eax & (1 << 8)) mousestate = 0;        /* left button down */
	else return;
	eax = kos_call(37, 1, 0, 0, 0, 0);
	/* 37.1 is relative to the client area here (as in KolibriOS's
	 * kernel now); the original's arithmetic is relative to the window */
	eax += (u32)wnd_border << 16;
	s16 y = (eax & 0xffff) + skinh;
	if (y < (s16)skinh) return;
	int edx = (u16)(y - skinh) / font_height;
	int x = eax >> 16;
	int ebx = cur_height - 1;
	if (edx == ebx) {
		/* .on_panel: the key bar */
		int ind = get_keybar_ind(x);
		const key_handler *tab = mouse_panels;
		if (active_screen_keybar == keybar_viewer) tab = mouse_viewer;
		else if (active_screen_keybar == keybar_editor) tab = mouse_editor;
		else if (active_screen_vtable != &panels_vtable) return;   /* (the file search has no key bar) */
		if (!tab[ind]) return;
		void *ebp = screens[active_screen].data;
		if (active_screen == 0) ebp = active_panel;
		tab[ind](ebp, 0x3B + ind % 12);         /* eax: the key's code F1...F12 */
		return;
	}
	if (active_screen > 0) return;
	int pos;
	if (edx < 2) pos = -1;                  /* .no_ch_pos */
	else {
		edx -= 2;                       /* the top border, the headers */
		ebx -= 7 - 1;
		if (edx >= ebx) pos = -1;
		else {
			int ecx = cur_width * font_width / 4;
			if ((((u16)(x - wnd_border) / ecx) & 1))
				edx += ebx;     /* the second column */
			pos = edx;
		}
	}
	ebx = cur_width * (font_width / 2) + wnd_border;
	if ((x <= ebx) != (active_panel == &panel1)) {
		/* .tab */
		active_panel = other_panel(active_panel);
		draw_cmdbar();
		draw_panel(other_panel(active_panel));
	}
	/* .move */
	PanelData *ebp = active_panel;
	edx = pos + ebp->start;             /* the number of files scrolled */
	if (edx > ebp->numfiles - 1) edx = ebp->numfiles - 1;
	if (edx >= 0) {
		ebp->index = edx;
		if (mousestate) {
			panels_OnKey_enter(ebp, 0x1C);
			return;
		}
	}
	draw_panel(ebp);
}

/* kfar.asm: ctrlkey_test0 ... ctrlkey_test4: al = the two bits of one
 * modifier (left, right) */
static bool ctrlkey_test(int test, u8 al)
{
	switch (test) {
	case 0: return al == 0;                 /* none */
	case 1: return al == 1 || al == 2;      /* exactly one */
	case 2: return al == 3;                 /* both */
	case 3: return al == 1;                 /* the left one */
	case 4: return al == 2;                 /* the right one */
	}
	return false;                           /* .fail */
}

/* kfar.asm: process_ctrl_keys */
bool process_ctrl_keys(const ctrlkey *esi, u32 al, void *ebp)
{
	for (; esi->scancode; esi++) {
		if ((u8)al != esi->scancode) continue;
		/* .check: Shift, Ctrl, Alt */
		bool ok = true;
		for (int cl = 0; cl <= 4 && ok; cl += 2) {
			int test = (esi->ctrlstate >> (cl * 2)) & 15;
			ok = ctrlkey_test(test, (ctrlstate >> cl) & 3);
		}
		if (!ok) continue;
		esi->handler(ebp, al);
		return true;
	}
	return false;
}

void dummy_key(void *ebp, u32 eax) {}

/* kfar.asm: enum_associations_callback: "key\0value\0" pairs, a zero at
 * the end */
static int enum_associations_callback(const char *f_name, const char *sec_name, const char *key_name,
				      const char *key_value)
{
	u32 esi = strlen(key_name) + 1 + strlen(key_value) + 1;   /* the entry's size */
	u32 ecx = associations_allocated;
	while (esi + associations_size > ecx) ecx += ecx;
	if (associations_allocated != ecx) {
		associations_allocated = ecx;
		char *p = xpgrealloc(associations, ecx);
		if (!p) {
			pgfree(associations);
			associations = 0;
			return 0;
		}
		associations = p;
	}
	char *edi = associations + associations_size - 1;
	associations_size += esi;
	strcpy(edi, key_name);
	strcpy(edi + strlen(key_name) + 1, key_value);
	associations[associations_size - 1] = 0;
	return 1;
}

/* kfar.asm: new_screen */
void *new_screen(u32 ecx, const screen_vtable *edx)
{
	void *ebp = xpgalloc(ecx);
	if (!ebp) return 0;
	screen *eax = xpgrealloc(screens, (num_screens + 1) * sizeof(screen));
	if (!eax) {
		pgfree(ebp);
		return 0;
	}
	screens = eax;
	screens[num_screens].vtable = edx;
	screens[num_screens].data = ebp;
	num_screens++;
	active_screen = num_screens - 1;
	active_screen_vtable = edx;
	active_screen_data = ebp;
	idle_interval = -1;
	if (edx->keybar) active_screen_keybar = edx->keybar;
	draw_keybar();
	return ebp;
}

/* kfar.asm: next_screen */
void next_screen(void)
{
	active_screen++;
	if (active_screen == num_screens) active_screen = 0;
	change_screen();
}

/* kfar.asm: delete_active_screen */
void delete_active_screen(void)
{
	void *ecx = screens[active_screen].data;
	memmove(&screens[active_screen], &screens[active_screen + 1],
		(num_screens - active_screen - 1) * sizeof(screen));
	num_screens--;
	pgfree(ecx);
	active_screen--;
	change_screen();
}

/* kfar.asm: change_screen */
void change_screen(void)
{
	idle_interval = -1;
	screen *s = &screens[active_screen];
	if (s->vtable->OnActivate) s->vtable->OnActivate(s->data);
	active_screen_vtable = s->vtable;
	active_screen_data = s->data;
	if (s->vtable->keybar) active_screen_keybar = s->vtable->keybar;
	draw_keybar();
	cursor_size = cursor_normal_size;
	s->vtable->OnRedraw(s->data);
}

/* kfar.asm: F12, the menu of the screens */
void F12(void *ebp, u32 eax)
{
	int esi = cur_width + 8;                /* an item: next, prev, the text */
	u8 *ebx = xpgalloc(esi * num_screens);
	if (!ebx) return;
	u8 *edi = ebx;
	for (int ecx = 0; ecx < num_screens; ecx++) {
		listitem *it = (listitem *)edi;
		it->next = ecx + 1 < num_screens ? (listitem *)(edi + esi) : 0;
		it->prev = ecx ? (listitem *)(edi - esi) : 0;
		char *t = it->text;
		*t++ = '&';
		if (ecx < 36) {
			*t++ = ecx < 10 ? '0' + ecx : 'A' + ecx - 10;
			*t++ = '.';
		} else {
			*t++ = ' ';
			*t++ = ' ';
		}
		*t++ = ' ';
		screens[ecx].vtable->getname(screens[ecx].data, t, cur_width - 12);
		edi += esi;
	}
	int r = menu((listitem *)(ebx + active_screen * esi), aScreens, 1);
	if (r != -1) active_screen = ((u8 *)r - ebx) / esi;
	pgfree(ebx);
	change_screen();
}

/* kfar.asm: init_console */
void init_console(void)
{
	pgfree(console_data_ptr);
	int ecx = cur_width * cur_height;
	console_data_ptr = pgalloc(ecx * 4);
	if (!console_data_ptr) kfar_exit();
	for (int i = 0; i < ecx; i++) console_data_ptr[i] = 0x0720;
	panel1.left = 0;
	panel1.top = 0;
	panel2.top = 0;
	int eax = (cur_width + 1) >> 1;
	panel1.width = eax;
	panel2.left = eax;
	panel2.width = cur_width - eax;
	panel1.height = panel2.height = cur_height - 2;
}

/* kfar.asm: draw_window */
void draw_window(void)
{
	kos_begin_draw();
	skinh = kos_skin_height();
	kos_call(0, 100 << 16 | (cur_width * font_width + wnd_border * 2 - 1),
		 100 << 16 | (skinh + cur_height * font_height + wnd_border - 1),
		 0x53000000, 0, (u32)header);
	if (fill_width)
		kos_call(13, (u32)(wnd_width - fill_width - (wnd_border - 1)) << 16 | fill_width,
			 (u32)skinh << 16 | (u16)(wnd_height - skinh - (wnd_border - 1)), 0, 0, 0);
	if (fill_height)
		kos_call(13, (u32)wnd_border << 16 | (u16)(wnd_width - (wnd_border * 2 - 1)),
			 (u32)(wnd_height - fill_height - (wnd_border - 1)) << 16 | fill_height, 0, 0, 0);
	if (MemForImage)
		put_image_ext(MemForImage, (u32)(cur_width * font_width) << 16 | (cur_height * font_height),
			      (u32)wnd_border << 16 | skinh, 8, console_colors, 0);
	kos_end_draw();
}

/* kfar.asm: draw_image.nomem */
static void draw_image_nomem(void)
{
	kos_call(13, (u32)wnd_border << 16 | (cur_width * font_width),
		 (u32)skinh << 16 | (cur_height * font_height), 0, 0, 0);
	kos_call(4, 32 << 16 | 32, 0xFFFFFF, (u32)nomem_draw, strlen(nomem_draw), 0);
}

/* kfar.asm: draw_image: the cells that changed since the last time into
 * the image (the font's pixels in the palette's colours), and that part
 * of the image to the window */
u8 bDrawImageDisabled;

void draw_image(void)
{
	if (bDrawImageDisabled) return;
	bMemForImageValidData = 1;
	if (!MemForImage) {
		/* allocate memory for image */
		MemForImage = pgalloc(cur_width * cur_height * font_width * font_height);
		if (!MemForImage) {
			draw_image_nomem();
			return;
		}
		bMemForImageValidData = 0;
	}
	int n = cur_width * cur_height;
	u16 *esi = console_data_ptr, *shadow = console_data_ptr + n;
	min_x = min_y = -1;
	max_x = max_y = 0;
	old_cursor_pos = cur_cursor_pos;
	cur_cursor_pos = cursor_y >= 0 && cursor_x >= 0 ? cursor_y * cur_width + cursor_x : -1;
	int stride = cur_width * font_width;
	for (int y = 0; y < cur_height; y++) {
		for (int x = 0; x < cur_width; x++) {
			int i = y * cur_width + x;
			u16 bx = esi[i];
			if (bMemForImageValidData && i != cur_cursor_pos && i != old_cursor_pos && bx == shadow[i])
				continue;       /* .skip_symbol */
			shadow[i] = bx;
			if (min_x == -1 || x < min_x) min_x = x;
			if (x > max_x) max_x = x;
			if (min_y == -1) min_y = y;
			max_y = y;
			u8 fg = (bx >> 8) & 0xF, bg = bx >> 12;
			const font_t *f = font + (u8)bx;
			u8 *edi = MemForImage + y * font_height * stride + x * font_width;
			for (int s = 0; s < font_height; s++, f += 256, edi += stride) {
				font_t c = *f;
				for (int p = 0; p < font_width; p++, c >>= 1)
					edi[p] = c & 1 ? fg : bg;
			}
		}
	}
	/* the cursor: the lowest cursor_size lines of its cell, inverted */
	if (cur_cursor_pos >= 0) {
		u8 *eax = MemForImage + ((cursor_y + 1) * font_height) * stride + cursor_x * font_width;
		for (int l = 0; l < cursor_size; l++) {
			eax -= stride;
			for (int p = 0; p < font_width; p++) eax[p] ^= 7;
		}
	}
	if (min_y == -1) return;               /* .nodraw */
	int w = (max_x - min_x + 1) * font_width, h = (max_y - min_y + 1) * font_height;
	put_image_ext(MemForImage + min_y * font_height * stride + min_x * font_width,
		      (u32)w << 16 | h, (u32)(min_x * font_width + wnd_border) << 16 | (min_y * font_height + skinh),
		      8, console_colors, stride - w);
}

/* kfar.asm: draw_keybar: the keys F1-F12 */
void draw_keybar(void)
{
	int eax = 0;
	if (ctrlstate & 3) eax |= 1;
	if (ctrlstate & 0xC) eax |= 2;
	if (ctrlstate & 0x30) eax |= 4;
	const char *esi = active_screen_keybar + eax * 6 * 12;  /* the text's length * the buttons */
	int ecx = 1;
	u16 *edi = get_console_ptr(0, cur_height - 1);
	int ebx = 6;
	eax = cur_width - (11 + 9 + 3 * 2 + 6);
	if (eax >= 7 * 11) ebx = eax / 11;
	int edx = 0;                            /* not to go beyond the panel */
	for (;;) {
		edx += 7;
		if (ecx >= 10) edx++;
		if (edx > cur_width) break;
		if (ecx < 10)
			*edi++ = CELL('0' + ecx, keybar_number_color);
		else {
			*edi++ = CELL('0' + ecx / 10, keybar_number_color);
			*edi++ = CELL('0' + ecx % 10, keybar_number_color);
		}
		for (int i = 0; i < 6; i++) *edi++ = CELL(*esi++, keybar_name_color);
		if (ecx == 12) break;
		edx += ebx - 6;
		for (int i = 0; i < ebx - 6; i++) *edi++ = CELL(' ', keybar_name_color);
		edx++;
		if (edx > cur_width) break;
		*edi++ = CELL(' ', keybar_bgr_color);
		ecx++;
	}
	/* .ret */
	if ((u8)edi[-1] == ' ') edi--;
	u16 *end = get_console_ptr(0, cur_height);
	while (edi < end) *edi++ = CELL(' ', keybar_name_color);   /* the rest in the buttons' colour */
	if (bDisplayQuickSearch) DrawDialogBox(&QuickSearchDlg);
}

/* kfar.asm: draw_cmdbar */
void draw_cmdbar(void)
{
	const char *esi = active_panel->dir;
	u16 *edi = get_console_ptr(0, cur_height - 2);
	int ecx = cur_width - 1;
	for (; ecx && *esi; ecx--) *edi++ = CELL(*esi++, cmdbar_prefix_color);
	*edi++ = CELL('>', cmdbar_prefix_color);
	while (ecx--) *edi++ = CELL(' ', cmdbar_normal_color);
}

/* dialogs.inc: get_ascii_char: query the keyboard layout (26.2: 1
 * normal, 2 with Shift) and translate the scancode to ASCII */
u8 get_ascii_char(u32 eax)
{
	int ecx = (ctrlstate & 3) ? 2 : 1;
	kos_call(26, 2, ecx, (u32)layout, 0, 0);
	return layout[eax & 0x7f];
}
