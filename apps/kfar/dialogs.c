/* kfar for KolibriOS for ARM: the dialogs and menus (dialogs.inc).
 *
 * Translated to C from kfar's dialogs.inc, diamond (Evgeny Grechnikov)
 * and the KolibriOS team, GPL-2.0.
 *
 * A dialog is drawn into the console over what it covers, which is
 * saved under it (dlgtemplate.dataptr: the saved x, y, then the cells)
 * and restored when it is hidden. GenericBox runs its own event loop
 * and hands the keys to the dialog's procedure (ManagerDlgProc for
 * dialogs of items, MenuDlgProc for menus). */
#include "kfar.h"

/* the menus' data: dlgtemplate, then (dialogs.inc: menu_centered_in)
 * +40 cur_variant, +44 num_variants, +48 the first item shown, +52 the
 * last item shown, +56 cur_variant_idx */
typedef struct menudata {
	dlgtemplate t;
	listitem *cur_variant;
	u32 num_variants;
	listitem *begin_variant;
	listitem *end_variant;
	u32 cur_variant_idx;
} menudata;

static int MenuDlgProc(void *dlg, int msg, u32 param1, u32 param2);
static void MenuDlgProc_dodraw(menudata *ebx);

/* the area under the "No memory" dialog (no allocation for it) */
static u8 nomem_dlgsavearea[8 + (12 + 4) * (3 + 3) * 2] __attribute__((aligned(4)));

/* dialogs.inc: DlgClearFocus: ebx the first item, ecx their number */
static void DlgClearFocus(dlgitemtemplate *ebx, int ecx)
{
	while (ecx--) (ebx++)->flags &= ~4;
}

/* dialogs.inc: GenericBox: int __stdcall GenericBox(DLGTEMPLATE* dlg, void* DlgProc) */
int GenericBox(void *dlg, DlgProc_t DlgProc)
{
	int saved_x = cursor_x, saved_y = cursor_y;
	int eax = ShowGenericBox(dlg, DlgProc);
	if (eax) {
		cursor_y = saved_y;
		cursor_x = saved_x;
		return eax;
	}
	dlgtemplate *ebx = dlg;
	/* message loop */
	for (;;) {
		int ev = kos_wait_event();
		if (ev == 1) {                  /* .redraw */
			draw_window();
			continue;
		}
		if (ev == 2) {                  /* .key */
			u8 al = kos_get_key() >> 8;
			if (al == 0xE0) {
				bWasE0 = 1;
				continue;
			}
			u8 ah = bWasE0;
			bWasE0 = 0;
			switch (al) {
			case 0x1D: ctrlstate |= ah ? 8 : 4; continue;
			case 0x9D: ctrlstate &= ah ? ~8 : ~4; continue;
			case 0x2A: if (!ah) ctrlstate |= 1; continue;
			case 0xAA: if (!ah) ctrlstate &= ~1; continue;
			case 0x36: ctrlstate |= 2; continue;
			case 0xB6: ctrlstate &= ~2; continue;
			case 0x38: ctrlstate |= ah ? 0x20 : 0x10; continue;
			case 0xB8: ctrlstate &= ah ? ~0x20 : ~0x10; continue;
			}
			eax = DlgProc(ebx, 2, al | (u32)ah << 8, 0);
			if (!eax) continue;
			break;
		}
		if (ev != 6) kfar_exit();       /* the close button */
		/* .mouse */
		mousestate = (kos_call(37, 3, 0, 0, 0, 0) & (1 << 8)) ? 1 : 0;     /* left button down */
		u32 pos = kos_call(37, 1, 0, 0, 0, 0);
		/* 37.1 relative to the client area here, see kfar.c's mouse */
		pos = ((pos + ((u32)kfar_wnd_border() << 16)) & 0xffff0000u) | (u16)((s16)pos + skinh);
		if ((s16)pos < (s16)skinh) continue;
		int edx = (u16)((s16)pos - skinh) / font_height;
		/* the window's frame: kfar.c's (function 9); 5 in the original */
		int x = (int)(pos >> 16) - kfar_wnd_border();
		x = (u16)x / font_width;
		if (edx < ebx->y || x < ebx->x) continue;
		edx -= ebx->y;
		x -= ebx->x;
		if (edx >= ebx->height || x >= ebx->width) continue;
		if (DlgProc == MenuDlgProc) {
			/* a menu (the original tests the dword after the
			 * template: a dialog's DlgProc, usually 0) */
			menudata *m = (menudata *)ebx;
			if (edx >= (int)m->num_variants) edx = m->num_variants - 1;
			int size = (u8 *)m->begin_variant->next - (u8 *)m->begin_variant;   /* one listitem */
			m->cur_variant_idx = edx;
			m->cur_variant = (listitem *)((u8 *)m->begin_variant + edx * size);
			if (mousestate == 1) {
				eax = (int)m->cur_variant;
				break;
			}
			MenuDlgProc_dodraw(m);
			draw_image();
			continue;
		}
		/* .m_menu_end: a dialog: the buttons, edit boxes, check boxes */
		if (mousestate != 1) continue;
		DLGDATA *d = (DLGDATA *)ebx;
		dlgitemtemplate *it = DLG_ITEMS(d);
		int ecx = d->num_items;
		if (!ecx) continue;
		dlgitemtemplate *focus = 0;
		bool done = false;
		for (int i = 0; i < ecx; i++, it++) {
			if (it->type != 2 && it->type != 3 && it->type != 5) continue;
			if (it->x1 > x || it->y1 > edx || it->x2 < x || it->y2 < edx) continue;
			if (it->type == 2) {            /* a button */
				eax = (int)it;
				done = true;
			} else if (it->type == 5) {     /* a check box */
				it->flags ^= 0x10;
				focus = it;
			} else if (!(it->flags & 4)) {  /* an edit box */
				focus = it;
			}
			break;
		}
		if (done) break;
		if (!focus) continue;
		/* .m_new_focus */
		DlgClearFocus(DLG_ITEMS(d), ecx);
		focus->flags |= 4;
		ManagerDlgProc_dodraw(d);
		draw_image();
	}
	/* .exit */
	HideGenericBox(dlg, 0);
	cursor_y = saved_y;
	cursor_x = saved_x;
	draw_image();
	return eax;
}

/* dialogs.inc: save_console_data: the cells of x0..x1, y0..y1 to ebp */
static void save_console_data(u16 *ebp, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > cur_width) x1 = cur_width;
	if (x1 - x0 <= 0) return;
	if (y1 > cur_height) y1 = cur_height;
	if (y1 - y0 <= 0) return;
	for (int y = y0; y < y1; y++, ebp += x1 - x0)
		memcpy(ebp, get_console_ptr(x0, y), (x1 - x0) * 2);
}

/* dialogs.inc: restore_console_data */
static void restore_console_data(const u16 *ebp, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > cur_width) x1 = cur_width;
	if (x1 - x0 <= 0) return;
	if (y1 > cur_height) y1 = cur_height;
	if (y1 - y0 <= 0) return;
	for (int y = y0; y < y1; y++, ebp += x1 - x0)
		memcpy(get_console_ptr(x0, y), ebp, (x1 - x0) * 2);
}

/* dialogs.inc: ShowGenericBox: int __stdcall ShowGenericBox(DLGTEMPLATE* dlg, void* DlgProc) */
int ShowGenericBox(void *dlg, DlgProc_t DlgProc)
{
	dlgtemplate *ebx = dlg;
	int saved_x = ebx->x, saved_y = ebx->y;
	/* center window if required */
	if (ebx->x == -1) ebx->x = (u32)(cur_width - ebx->width) >> 1;
	if (ebx->y == -1) ebx->y = (u32)(cur_height - ebx->height) >> 1;
	/* some checks */
	if (ebx->x < 1 || ebx->x + ebx->width >= cur_width || ebx->y < 1 || ebx->y + ebx->height >= cur_height ||
	    ebx->border_size_x < 1 || ebx->border_size_y < 1) {
		/* .sizeerr */
		ebx->y = saved_y;
		ebx->x = saved_x;
		return -1;
	}
	/* set color if required */
	struct dlgcolors *edi = ebx->flags & 1 ? &dialog_colors : ebx->flags & 2 ? &warning_colors : 0;
	if (edi) {
		ebx->main_color = edi->main_color;
		ebx->border_color = edi->border_color;
		ebx->header_color = edi->header_color;
	}
	/* allocate memory for data under dialog; for the "No memory" dialog
	 * use the static data area */
	int *eax = (int *)nomem_dlgsavearea;
	if (ebx != &nomem_dlgdata.h.dialog) {
		eax = xpgalloc((ebx->width + ebx->border_size_x * 2 + 2) *
			       (ebx->height + ebx->border_size_y * 2 + 1) * 2 + 8);
		if (!eax) {
			ebx->y = saved_y;
			ebx->x = saved_x;
			return -1;
		}
	}
	ebx->dataptr = eax;
	eax[1] = saved_y;
	eax[0] = saved_x;
	/* save data */
	save_console_data((u16 *)(eax + 2), ebx->x - ebx->border_size_x, ebx->y - ebx->border_size_y,
			  ebx->x + ebx->width + ebx->border_size_x + 2, ebx->y + ebx->height + ebx->border_size_y + 1);
	/* draw shadow */
	if (!(ebx->flags & 4)) draw_dialog_shadow(ebx);
	DrawGenericBox(dlg, DlgProc);
	return 0;
}

/* dialogs.inc: draw_dialog_shadow: the cells right and below the
 * dialog in dark grey on black */
void draw_dialog_shadow(dlgtemplate *ebx)
{
	int eax = ebx->x - ebx->border_size_x;
	if (eax <= 0) eax = 0;
	eax += 2;
	int edx = ebx->y - ebx->border_size_y;
	if (edx <= 0) edx = 0;
	edx++;
	u16 *edi = get_console_ptr(eax, edx);
	int ecx = ebx->y + ebx->height + ebx->border_size_y + 1;
	if (ecx >= cur_height) ecx = cur_height;
	int rows = ecx - edx;
	ecx = ebx->x + ebx->width + ebx->border_size_x + 2;
	if (ecx >= cur_width) ecx = cur_width;
	int cols = ecx - eax;
	for (; rows > 0; rows--, edi += cur_width) {
		u8 *a = (u8 *)edi + 1;
		for (int i = 0; i < cols; i++, a += 2) {
			if (!(*a & 0x0F)) *a |= 8;
			*a &= 0x0F;
		}
	}
}

/* dialogs.inc: DrawGenericBox: void __stdcall DrawGenericBox(DLGDATA* dlg, void* DlgProc) */
void DrawGenericBox(void *dlg, DlgProc_t DlgProc)
{
	dlgtemplate *ebx = dlg;
	/* draw area background */
	int eax = ebx->x - ebx->border_size_x;
	if (eax <= 0) eax = 0;
	int edx = ebx->y - ebx->border_size_y;
	if (edx <= 0) edx = 0;
	u16 *edi = get_console_ptr(eax, edx);
	int ecx = ebx->x + ebx->width + ebx->border_size_x;
	if (ecx >= cur_width) ecx = cur_width;
	int esi = ecx - eax;
	ecx = ebx->y + ebx->height + ebx->border_size_y;
	if (ecx >= cur_height) ecx = cur_height;
	for (edx = ecx - edx; edx > 0; edx--, edi += cur_width)
		for (int i = 0; i < esi; i++) edi[i] = CELL(' ', ebx->border_color);
	/* draw border */
	draw_border(get_console_ptr(ebx->x - 1, ebx->y - 1), ebx->border_color, ebx->width + 2, ebx->height + 2);
	/* draw header */
	const char *s = ebx->title;
	if (s && *s) {
		int len = strlen(s) + 2;        /* the title's length + 2 */
		ecx = ebx->width;
		bool ok = true;
		if ((u32)len <= (u32)ecx) {
			edx = (ecx - len) >> 1;     /* .fullhea */
		} else {
			ecx -= 5;
			edx = 0;
			ok = ecx >= 0;
		}
		if (ok) {
			/* .drawhea */
			u16 *p = get_console_ptr(ebx->x + edx, ebx->y - 1);
			*p++ = CELL(' ', ebx->header_color);
			for (;;) {
				if (!--ecx) {
					/* .3 */
					*p++ = CELL('.', ebx->header_color);
					*p++ = CELL('.', ebx->header_color);
					*p++ = CELL('.', ebx->header_color);
					break;
				}
				if (!*s) break;
				*p++ = CELL(*s++, ebx->header_color);
			}
			/* .4 */
			*p = CELL(' ', ebx->header_color);
		}
	}
	/* .noheader: draw window background */
	edi = get_console_ptr(ebx->x, ebx->y);
	for (edx = ebx->height; edx > 0; edx--, edi += cur_width)
		for (int i = 0; i < ebx->width; i++) edi[i] = CELL(' ', ebx->main_color);
	/* send redraw message */
	DlgProc(ebx, 1, 0, 0);
	draw_image();
}

/* dialogs.inc: HideGenericBox, HideDialogBox: void __stdcall
 * HideDialogBox(DLGDATA* dlg, int bRedrawWindow) */
void HideGenericBox(void *dlg, int bRedrawWindow)
{
	dlgtemplate *ebx = dlg;
	int *ebp = ebx->dataptr;
	/* restore data */
	restore_console_data((u16 *)(ebp + 2), ebx->x - ebx->border_size_x, ebx->y - ebx->border_size_y,
			     ebx->x + ebx->width + ebx->border_size_x + 2,
			     ebx->y + ebx->height + ebx->border_size_y + 1);
	draw_keybar();
	ebx->y = ebp[1];
	ebx->x = ebp[0];
	if (ebx != &nomem_dlgdata.h.dialog) pgfree(ebp);
	cursor_x = -1;
	cursor_y = -1;
	if (bRedrawWindow) draw_image();
}

void HideDialogBox(void *dlg, int bRedrawWindow)
{
	HideGenericBox(dlg, bRedrawWindow);
}

/* dialogs.inc: menu: int __stdcall menu(void* variants, const char*
 * title, unsigned flags); variants points to the current item of a
 * doubly linked list */
int menu(listitem *variants, const char *title, u32 flags)
{
	return menu_centered_in(0, 0, cur_width, cur_height, variants, title, flags);
}

/* dialogs.inc: menu_centered_in: int __stdcall menu_centered_in(unsigned
 * left, unsigned top, unsigned width, unsigned height, void* variants,
 * const char* title, unsigned flags) */
int menu_centered_in(int left, int top, int width, int height, listitem *variants, const char *title,
		     u32 flags)
{
	menudata *ebx = xpgalloc(sizeof(menudata));
	if (!ebx) return -1;
	int eax = flags & 1 ? 3 : 1;
	ebx->t.border_size_x = eax;
	ebx->t.border_size_y = (eax + 1) >> 1;
	/* find the window's width and height */
	listitem *esi = variants;
	ebx->cur_variant = esi;
	ebx->cur_variant_idx = 0;
	while (esi->prev) {
		esi = esi->prev;
		ebx->cur_variant_idx++;
	}
	/* .find_width */
	ebx->begin_variant = esi;
	eax = 0;
	int ecx = 0;
	for (; esi; esi = esi->next, ecx++) {
		int len = strlen(esi->text);
		if (strchr(esi->text, '&')) len--;
		if (len > eax) eax = len;
	}
	eax += 3 + ebx->t.border_size_x * 2;
	if (eax >= cur_width) eax = cur_width;
	eax -= ebx->t.border_size_x * 2;
	ebx->t.width = eax;
	ebx->t.height = ecx;
	ebx->num_variants = ecx;
	eax = left + ((width - eax) >> 1);
	if (eax < ebx->t.border_size_x) eax = ebx->t.border_size_x;
	if (eax + ebx->t.width + ebx->t.border_size_x > cur_width)
		eax = cur_width - ebx->t.width - ebx->t.border_size_x;
	ebx->t.x = eax;
	ecx = top + ((height - ecx) >> 1);
	if (ecx < ebx->t.border_size_y) ecx = ebx->t.border_size_y;
	if (ecx + ebx->t.height + ebx->t.border_size_y > cur_height)
		ecx = cur_height - ebx->t.height - ebx->t.border_size_y;
	ebx->t.y = ecx;
	if ((u32)ebx->t.height > (u32)(cur_height - 6)) {
		ebx->t.height = cur_height - 6;
		ebx->t.y = 3;
	}
	/* .small_height: the items shown, the current one among them */
	ecx = ebx->t.height;
	listitem *it = ebx->cur_variant;
	ebx->begin_variant = it;
	if (--ecx) {
		for (int i = ecx; i && it->prev; i--) it = it->prev;
		ebx->begin_variant = it;
		while (ecx--) it = it->next;
	}
	ebx->end_variant = it;
	ebx->t.title = title;
	ebx->t.main_color = menu_normal_color;
	ebx->t.border_color = menu_border_color;
	ebx->t.header_color = menu_header_color;
	eax = GenericBox(ebx, MenuDlgProc);
	pgfree(ebx);
	return eax;
}

/* dialogs.inc: MenuDlgProc.line_prev, .line_next */
static listitem *MenuDlgProc_line_prev(menudata *ebx, listitem *eax)
{
	if (eax == ebx->begin_variant) {
		ebx->begin_variant = ebx->begin_variant->prev;
		ebx->end_variant = ebx->end_variant->prev;
	}
	ebx->cur_variant_idx--;
	return eax->prev;
}

static listitem *MenuDlgProc_line_next(menudata *ebx, listitem *eax)
{
	if (eax == ebx->end_variant) {
		ebx->begin_variant = ebx->begin_variant->next;
		ebx->end_variant = ebx->end_variant->next;
	}
	ebx->cur_variant_idx++;
	return eax->next;
}

/* dialogs.inc: MenuDlgProc.home, .end */
static void MenuDlgProc_home(menudata *ebx)
{
	listitem *eax = ebx->cur_variant;
	while (eax->prev) eax = eax->prev;
	ebx->begin_variant = eax;
	listitem *l = eax;
	for (int ecx = ebx->t.height - 1; ecx > 0; ecx--) l = l->next;
	ebx->end_variant = l;
	ebx->cur_variant_idx = 0;
	ebx->cur_variant = eax;
}

static void MenuDlgProc_end(menudata *ebx)
{
	listitem *eax = ebx->cur_variant;
	while (eax->next) eax = eax->next;
	ebx->end_variant = eax;
	listitem *f = eax;
	for (int ecx = ebx->t.height - 1; ecx > 0; ecx--) f = f->prev;
	ebx->begin_variant = f;
	ebx->cur_variant_idx = ebx->num_variants - 1;
	ebx->cur_variant = eax;
}

/* dialogs.inc: MenuDlgProc */
static int MenuDlgProc(void *dlg, int msg, u32 param1, u32 param2)
{
	menudata *ebx = dlg;
	if (msg == 1) {                         /* .draw */
		MenuDlgProc_dodraw(ebx);
		return 0;
	}
	if (msg != 2) return 0;
	/* .key */
	u8 al = param1;
	listitem *eax = ebx->cur_variant;
	int ecx;
	switch (al) {
	case 0x48:
	case 0x4B:                              /* .prev */
		if (!eax->prev) {
			MenuDlgProc_end(ebx);
			goto redraw;
		}
		eax = MenuDlgProc_line_prev(ebx, eax);
		break;
	case 0x4D:
	case 0x50:                              /* .next */
		if (!eax->next) {
			MenuDlgProc_home(ebx);
			goto redraw;
		}
		eax = MenuDlgProc_line_next(ebx, eax);
		break;
	case 0x1C:                              /* .enter */
		return (int)ebx->cur_variant;
	case 1:                                 /* .esc */
		return -1;
	case 0x47:
		MenuDlgProc_home(ebx);
		goto redraw;
	case 0x4F:
		MenuDlgProc_end(ebx);
		goto redraw;
	case 0x51:                              /* .pgdn */
		for (ecx = ebx->t.height; ecx && eax->next; ecx--) eax = MenuDlgProc_line_next(ebx, eax);
		break;
	case 0x49:                              /* .pgup */
		for (ecx = ebx->t.height; ecx && eax->prev; ecx--) eax = MenuDlgProc_line_prev(ebx, eax);
		break;
	case 0x52:                              /* .ins: close the CD's tray */
	case 0x53:                              /* .del: open it */
		if (eax->text[0] == '/' && eax->text[1] == 'c' && eax->text[2] == 'd')
			kos_call(24, al == 0x52 ? 5 : 4, eax->text[3] - '0', 0, 0, 0);
		return 0;
	default: {
		/* the item's letter after '&' */
		listitem *edx = ebx->cur_variant;
		while (edx->prev) edx = edx->prev;
		for (; edx; edx = edx->next) {
			char *esi = strchr(edx->text, '&');
			if (esi && ascii2scan[(u8)esi[1]] == al) return (int)edx;
		}
		return 0;
	}
	}
	/* .posret */
	ebx->cur_variant = eax;
redraw:
	MenuDlgProc_dodraw(ebx);
	draw_image();
	return 0;
}

/* dialogs.inc: MenuDlgProc.dodraw */
static void MenuDlgProc_dodraw(menudata *ebx)
{
	u16 *edi = get_console_ptr(ebx->t.x, ebx->t.y);
	for (listitem *esi = ebx->begin_variant; esi; esi = esi->next) {
		bool dl = false;
		bool sel = esi == ebx->cur_variant;
		u8 ah = sel ? menu_selected_color : menu_normal_color;
		u16 *p = edi;
		int ecx = ebx->t.width;
		*p++ = CELL(' ', ah);
		*p++ = CELL(' ', ah);
		ecx -= 3;
		const char *s = esi->text;
		u8 al = ' ';
		for (; ecx > 0; ecx--) {
			al = *s++;
			if (!al) {
				al = ' ';
				break;
			}
			if (al == '&' && !dl) {
				dl = true;
				al = *s++;
				*p++ = CELL(al, sel ? menu_selected_highlight_color : menu_highlight_color);
				continue;
			}
			*p++ = CELL(al, ah);
		}
		if (!ecx) al = *s ? ' ' : 0;    /* the text filled the line */
		*p++ = CELL(al, ah);            /* .1 */
		while (ecx-- > 0) *p++ = CELL(' ', ah);
		edi += cur_width;
		if (esi == ebx->end_variant) break;
	}
	/* the scroll bar */
	int ecx = ebx->t.height;
	if (ecx == (int)ebx->num_variants) return;
	ecx -= 2;
	if (ecx <= 0) return;
	int edx = ebx->cur_variant_idx * ecx / ebx->num_variants + 1;
	edi = get_console_ptr(ebx->t.x + ebx->t.width, ebx->t.y);
	*edi = CELL(0x1E, menu_scrollbar_color);
	edi += cur_width;
	for (; ecx; ecx--, edi += cur_width)
		*edi = CELL(--edx == 0 ? 0xB2 : 0xB0, menu_scrollbar_color);
	*edi = CELL(0x1F, menu_scrollbar_color);
}

/* dialogs.inc: DialogBox: int __stdcall DialogBox(DLGDATA* dlg) */
int DialogBox(void *dlg)
{
	return GenericBox(dlg, ManagerDlgProc);
}

/* dialogs.inc: ShowDialogBox */
int ShowDialogBox(void *dlg)
{
	return ShowGenericBox(dlg, ManagerDlgProc);
}

/* dialogs.inc: DrawDialogBox */
void DrawDialogBox(void *dlg)
{
	DrawGenericBox(dlg, ManagerDlgProc);
}

/* dialogs.inc: ManagerDlgProc.ret_test: the edit box's start after a
 * move of the position */
static void ManagerDlgProc_ret_test(dlgitemtemplate *ebx, editdata *edx)
{
	int eax = edx->pos;
	if ((int)edx->start >= eax) {
		edx->start = eax;
		return;
	}
	eax += ebx->x1 - ebx->x2;
	if ((int)edx->start < eax) edx->start = eax;
}

/* dialogs.inc: ManagerDlgProc: the dialogs of items */
int ManagerDlgProc(void *dlg, int msg, u32 param1, u32 param2)
{
	DLGDATA *ebp = dlg;
	if (msg == 1) {                         /* .draw */
		ManagerDlgProc_dodraw(ebp);
		return 0;
	}
	if (msg != 2) return 0;
	/* .key: find item with focus */
	dlgitemtemplate *items = DLG_ITEMS(ebp), *ebx = items;
	int ecx = ebp->num_items;
	while (ecx && !(ebx->flags & 4)) {
		ebx++;
		ecx--;
	}
	/* .nobtns */
	u8 al = param1;
	switch (al) {
	case 1:                                 /* .esc */
		return -1;
	case 0x1C:                              /* .enter */
		if (ebx->type == 2) return (int)ebx;
		ebx = items;
		for (ecx = ebp->num_items; ecx; ecx--, ebx++)
			if (ebx->type == 2 && (ebx->flags & 0x10)) break;
		return (int)ebx;                /* .enter_found */
	case 0xF:                               /* .tab */
		if (ctrlstate & 3) goto up;
		goto down;
	case 0x48:
		goto up;
	case 0x50:
		goto down;
	}
	if (ecx) {
		if (ebx->type == 3) goto key_edit;
		if (ebx->type == 5 && al == 0x39) {
			ebx->flags ^= 0x10;
			goto ret_draw;
		}
	}
	/* .nobtns2 */
	if (al == 0x4B) goto up;
	if (al == 0x4D) goto down;
	return 0;                               /* .ret0 */
down:                                           /* .right, .down */
	if (!ecx) return 0;
	ebx->flags &= ~4;
	while (--ecx) {
		ebx++;
		if (ebx->flags & 8) goto btn_found;
	}
	/* .find_first_btn */
	for (ebx = items; !(ebx->flags & 8); ebx++);
btn_found:
	ebx->flags |= 4;
ret_draw:
	ManagerDlgProc_dodraw(ebp);
	draw_image();
	return 0;
up:                                             /* .shift_tab, .left, .up */
	if (!ecx) return 0;
	ebx->flags &= ~4;
	ecx = ebp->num_items - ecx;             /* the focused item's index */
	while (ecx--) {
		ebx--;
		if (ebx->flags & 8) goto btn_found;
	}
	/* .find_last_btn */
	for (ebx = items + ebp->num_items - 1; !(ebx->flags & 8); ebx--);
	goto btn_found;
key_edit: {
	/* the keys in an edit box */
	if (al & 0x80) return 0;
	ebx->flags |= 0x10;
	editdata *edx = ebx->data;
	char *d = edx->data;
	switch (al) {
	case 0x4B:                              /* .editor_left */
		if (edx->pos) edx->pos--;
		break;
	case 0x4D:                              /* .editor_right */
		if (d[edx->pos]) edx->pos++;
		break;
	case 0x47:                              /* .editor_home */
		edx->pos = 0;
		break;
	case 0x4F:                              /* .editor_end */
		edx->pos = strlen(d);
		break;
	case 0x0E:                              /* .editor_backspace */
		if (edx->pos) {
			edx->pos--;
			memmove(d + edx->pos, d + edx->pos + 1, strlen(d + edx->pos + 1) + 1);
		}
		break;
	case 0x53:                              /* .editor_del */
		if (d[edx->pos]) memmove(d + edx->pos, d + edx->pos + 1, strlen(d + edx->pos + 1) + 1);
		break;
	default: {                              /* .editor_char */
		if (ctrlstate & 0x3C) goto ret_draw;
		u8 c = get_ascii_char(al);
		/* insert entered symbol */
		u32 n = strlen(d) + 1;
		if (n > edx->maxlength) break;  /* buffer capacity exceeded */
		memmove(d + edx->pos + 1, d + edx->pos, n - edx->pos);
		d[edx->pos++] = c;
		break;
	}
	}
	ManagerDlgProc_ret_test(ebx, edx);
	goto ret_draw;
}
}

/* dialogs.inc: dlgitem_get_console_ptr */
static u16 *dlgitem_get_console_ptr(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	return get_console_ptr(ebx->x1 + ebp->dialog.x, ebx->y1 + ebp->dialog.y);
}

/* dialogs.inc: draw_text_esi: the text esi in the item's width, aligned
 * as its flags say, in the colour ah */
static void draw_text_esi(DLGDATA *ebp, dlgitemtemplate *ebx, const char *esi, u8 ah)
{
	if (!esi) return;
	int ecx = strlen(esi);                  /* the string's length */
	int eax = 0;
	int edx = ebx->x2 - ebx->x1 + 1;
	if (ecx < edx) {
		switch (ebx->flags & 3) {
		case 0: eax = 0; break;                         /* .text_align_left */
		case 1: eax = (u32)(edx - ecx) >> 1; break;     /* .text_align_center */
		default: eax = edx - ecx; break;                /* right */
		}
	}
	/* .text_draw */
	u16 *edi = dlgitem_get_console_ptr(ebp, ebx);
	for (int i = 0; i < eax; i++) *edi++ = CELL(' ', ah);
	if (ecx > edx) {
		if (ebx->type == 3) {
			ecx = edx;
		} else {
			if (edx < 3) goto ret;
			*edi++ = CELL('.', ah);
			*edi++ = CELL('.', ah);
			*edi++ = CELL('.', ah);
			esi += ecx;
			ecx = edx - 3;
			esi -= ecx;
		}
	}
	/* .text_copy; check for password editboxes */
	if (ebx->type == 3 && (ebx->flags & 0x20))
		while (ecx-- > 0) *edi++ = CELL('*', ah);
	else
		while (ecx-- > 0) *edi++ = CELL(*esi++, ah);
ret: {
	u16 *end = get_console_ptr(ebp->dialog.x + ebx->x2 + 1, ebp->dialog.y + ebx->y1);
	while (edi < end) *edi++ = CELL(' ', ah);
}
}

/* dialogs.inc: draw_static_text */
static void draw_static_text(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	u8 ah = ebp->dialog.flags & 2 ? warning_main_color : dialog_main_color;
	draw_text_esi(ebp, ebx, ebx->data, ah);
}

/* dialogs.inc: draw_button */
static void draw_button(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	struct dlgcolors *ecx = ebp->dialog.flags & 2 ? &warning_colors : &dialog_colors;
	u8 ah = ebx->flags & 4 ? ecx->selected_btn_color : ecx->normal_btn_color;
	draw_text_esi(ebp, ebx, ebx->data, ah);
}

/* dialogs.inc: draw_editbox */
static void draw_editbox(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	editdata *edx = ebx->data;
	if (ebx->flags & 4) {
		cursor_x = ebx->x1 + edx->pos - edx->start + ebp->dialog.x;
		cursor_y = ebx->y1 + ebp->dialog.y;
	}
	struct dlgcolors *ecx = ebp->dialog.flags & 2 ? &warning_colors : &dialog_colors;
	u8 ah = ebx->flags & 0x10 ? ecx->edit_color : ecx->unmodified_edit_color;
	draw_text_esi(ebp, ebx, edx->data + edx->start, ah);
}

/* dialogs.inc: draw_h_separator: a horizontal line, from the frame
 * (x1 = -1) to the frame (the dialog's width); only the characters */
static void draw_h_separator(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	u16 *edi = dlgitem_get_console_ptr(ebp, ebx);
	for (int ecx = ebx->x1;; ecx++, edi++) {
		u8 al;
		bool done = false;
		if (ecx < 0) al = 0xC7;
		else if (ecx == ebp->dialog.width) { al = 0xB6; done = true; }
		else al = 0xC4;
		*(u8 *)edi = al;
		if (done) break;
		if (!((u32)ecx + 1 < (u32)ebx->x2)) break;
	}
}

/* dialogs.inc: draw_checkbox */
static void draw_checkbox(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	u16 *edi = dlgitem_get_console_ptr(ebp, ebx);
	if (ebx->flags & 4) {
		cursor_x = ebx->x1 + ebp->dialog.x + 1;
		cursor_y = ebx->y1 + ebp->dialog.y;
	}
	u8 ah = ebp->dialog.flags & 2 ? warning_main_color : dialog_main_color;
	*edi++ = CELL('[', ah);
	*edi++ = CELL(ebx->flags & 0x10 ? 'x' : ' ', ah);
	*edi++ = CELL(']', ah);
	*edi++ = CELL(' ', ah);
	/* the text: x2 = -1 means up to its end, then x2 is set to it */
	u32 ecx = (u32)ebx->x2 - (u32)ebx->x1;
	if ((u32)ebx->x2 < (u32)ebx->x1) return;
	if (ecx <= 3) return;
	ecx -= 3;
	const char *esi = ebx->data;
	for (; ecx; ecx--) {
		char al = *esi++;
		if (!al) {
			/* .correct */
			ebx->x2 -= ecx;
			return;
		}
		*edi++ = CELL(al, ah);
	}
}

/* dialogs.inc: draw_listbox */
void draw_listbox(DLGDATA *ebp, dlgitemtemplate *ebx)
{
	u16 *edi = dlgitem_get_console_ptr(ebp, ebx);
	listdata *edx = ebx->data;
	listitem *esi = edx->head;
	int rows = ebx->y2 - ebx->y1;
	for (int n = rows; n >= 0 && esi; n--, esi = esi->next, edi += cur_width) {
		int ecx = ebx->x2 - ebx->x1 + 1;
		int len = strlen(esi->text);
		int eax = 0;
		if (ecx < len) {
			switch (ebx->flags & 3) {
			case 0: break;                          /* .text_align_left */
			case 1: eax = (u32)(len - ecx) >> 1; break;
			default: eax = len - ecx; break;
			}
		}
		u8 ah = esi == edx->curitemptr ? dialog_selected_list_color : dialog_list_color;
		const char *s = esi->text + eax;
		u16 *p = edi;
		for (; ecx && *s; ecx--) *p++ = CELL(*s++, ah);
		while (ecx-- > 0) *p++ = CELL(' ', ah);
	}
	/* .listdone: the scroll bar */
	int ecx = rows + 1;
	u32 n = edx->numitems;
	if ((u32)ecx >= n) return;
	ecx -= 2;
	if (ecx <= 0) return;
	int pos = edx->curitem * ecx / n + 1;
	edi = get_console_ptr(ebx->x2 + ebp->dialog.x, ebx->y1 + ebp->dialog.y);
	*edi = CELL(0x1E, dialog_scroll_list_color);
	edi += cur_width;
	for (; ecx; ecx--, edi += cur_width)
		*edi = CELL(--pos == 0 ? 0xB1 : 0xB0, dialog_scroll_list_color);
	*edi = CELL(0x1F, dialog_scroll_list_color);
}

/* dialogs.inc: ManagerDlgProc.dodraw */
void ManagerDlgProc_dodraw(DLGDATA *ebp)
{
	static void (*const draw_functions[])(DLGDATA *, dlgitemtemplate *) = {
		0, draw_static_text, draw_button, draw_editbox, draw_h_separator, draw_checkbox, draw_listbox,
	};
	cursor_x = -1;
	cursor_y = -1;
	dlgitemtemplate *ebx = DLG_ITEMS(ebp);
	for (u32 ecx = ebp->num_items; ecx; ecx--, ebx++)
		if (ebx->type < 7 && draw_functions[ebx->type]) draw_functions[ebx->type](ebp, ebx);
}

/* dialogs.inc: listbox_key.line_prev, .calc_last_line, .line_next */
static listitem *listbox_key_line_prev(listdata *edx, listitem *eax)
{
	bool top = eax == edx->head;
	eax = eax->prev;
	if (top) edx->head = eax;
	edx->curitem--;
	return eax;
}

static listitem *listbox_key_calc_last_line(dlgitemtemplate *ebx, listdata *edx)
{
	listitem *esi = edx->head;
	for (int ecx = ebx->y2 - ebx->y1; ecx; ecx--) {
		esi = esi->next;
		if (!esi) break;
	}
	return esi;
}

static listitem *listbox_key_line_next(listdata *edx, listitem *eax, listitem **esi)
{
	bool last = eax == *esi;
	eax = eax->next;
	if (last) {
		edx->head = edx->head->next;
		*esi = eax;
	}
	edx->curitem++;
	return eax;
}

/* dialogs.inc: listbox_key: the keys of a list (for the dialogs'
 * procedures that have lists) */
void listbox_key(dlgitemtemplate *ebx, u32 al)
{
	listdata *edx = ebx->data;
	listitem *eax = edx->curitemptr, *esi;
	int ecx;
	switch ((u8)al) {
	case 0x50:                              /* .next */
		esi = listbox_key_calc_last_line(ebx, edx);
		if (eax->next) eax = listbox_key_line_next(edx, eax, &esi);
		break;
	case 0x51:                              /* .pgdn */
		esi = listbox_key_calc_last_line(ebx, edx);
		for (ecx = ebx->y2 - ebx->y1; ecx && eax->next; ecx--)
			eax = listbox_key_line_next(edx, eax, &esi);
		break;
	case 0x48:                              /* .prev */
		if (eax->prev) eax = listbox_key_line_prev(edx, eax);
		break;
	case 0x49:                              /* .pgup */
		for (ecx = ebx->y2 - ebx->y1; ecx && eax->prev; ecx--)
			eax = listbox_key_line_prev(edx, eax);
		break;
	case 0x47:                              /* .home */
		while (eax->prev) eax = eax->prev;
		edx->head = eax;
		edx->curitem = 0;
		break;
	case 0x4F: {                            /* .end */
		while (eax->next) eax = eax->next;
		listitem *h = eax;
		for (ecx = ebx->y2 - ebx->y1; ecx && h->prev; ecx--) h = h->prev;
		edx->head = h;
		edx->curitem = edx->numitems - 1;
		break;
	}
	default:
		return;
	}
	edx->curitemptr = eax;
}

/* dialogs.inc: SayNoMem: void __stdcall SayNoMem(void) */
void SayNoMem(void)
{
	nomem_dlgdata.h.dialog.x = -1;
	nomem_dlgdata.h.dialog.y = -1;
	DialogBox(&nomem_dlgdata);
}

/* dialogs.inc: ConfirmCancel: 0 = the user is sure, nonzero = the user
 * wants to continue */
int ConfirmCancel(void)
{
	return SayErrTitle(aCancelled, 1, ConfirmCancelMsg, 2, YesOrNoBtn);
}

/* dialogs.inc: Message (and SayErr, SayErrTitle with the warning's
 * colours): the strings centred, the buttons below them; returns the
 * button's index or -1 */
static int Message_flags(u32 flags, const char *title, int num_strings, const char **strings, int num_buttons,
			 const char **buttons)
{
	DLGDATA *ebx = xpgalloc(sizeof(DLGDATA) + (num_strings + num_buttons) * sizeof(dlgitemtemplate));
	if (!ebx) return -1;
	ebx->dialog.flags = flags;
	ebx->dialog.x = -1;
	ebx->dialog.y = -1;
	/* calculate width */
	int edx = 0;
	for (int i = 0; i < num_strings; i++) {
		int eax = strlen(strings[i]) + 2;
		if (eax > edx) edx = eax;
	}
	int ebp = 0;
	for (int i = 0; i < num_buttons; i++) ebp += strlen(buttons[i]) + 2;
	ebp += 2;
	if (ebp > edx) edx = ebp;
	if (edx >= cur_width - 8) edx = cur_width - 8;
	ebx->dialog.width = edx;
	ebx->dialog.height = num_strings + 1;
	ebx->dialog.border_size_x = 3;
	ebx->dialog.border_size_y = 2;
	ebx->dialog.title = title;
	ebx->DlgProc = 0;
	ebx->user_data = 0;
	ebx->num_items = num_strings + num_buttons;
	/* fill strings */
	dlgitemtemplate *edi = DLG_ITEMS(ebx);
	for (int i = 0; i < num_strings; i++, edi++)
		*edi = (dlgitemtemplate)DlgLbl(0, i, edx - 1, i, strings[i], 1);
	/* fill buttons */
	edx -= ebp;
	edx = edx < 0 ? 0 : (edx >> 1) + 1;     /* .big, .fillbtns */
	for (int i = 0; i < num_buttons; i++, edi++) {
		int x1 = edx;
		edx += strlen(buttons[i]) + 1;
		*edi = (dlgitemtemplate)DlgBtn(x1, ebx->dialog.height - 1, edx, ebx->dialog.height - 1, buttons[i],
					       i == 0 ? 9 | 4 : 9);
		edx++;
	}
	int eax = DialogBox(ebx);
	if (eax != -1)
		eax = ((dlgitemtemplate *)eax - DLG_ITEMS(ebx)) - num_strings;
	pgfree(ebx);
	return eax;
}

/* dialogs.inc: SayErr: int __stdcall SayErr(int num_strings, const
 * char* strings[], int num_buttons, const char* buttons[]) */
int SayErr(int num_strings, const char **strings, int num_buttons, const char **buttons)
{
	return SayErrTitle(aError, num_strings, strings, num_buttons, buttons);
}

/* dialogs.inc: SayErrTitle */
int SayErrTitle(const char *title, int num_strings, const char **strings, int num_buttons, const char **buttons)
{
	return Message_flags(2, title, num_strings, strings, num_buttons, buttons);
}

/* dialogs.inc: Message: int __stdcall Message(const char* title, int
 * num_strings, const char* strings[], int num_buttons, const char*
 * buttons[]) */
int Message(const char *title, int num_strings, const char **strings, int num_buttons, const char **buttons)
{
	return Message_flags(1, title, num_strings, strings, num_buttons, buttons);
}
