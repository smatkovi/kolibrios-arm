/* kfar for KolibriOS for ARM: the panels' drawing, reading of folders,
 * sorting and masks (kfar.asm: draw_border ... draw_time_column,
 * read_folder ... match_single_mask_rev_lowercase).
 *
 * Translated to C from kfar's kfar.asm, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0.
 *
 * The cells are written as the original writes them: where it goes
 * backwards (std, the sizes), the pointer goes backwards here too. */
#include "kfar.h"

static int column_left, column_top, column_width, column_height, column_index, last_column_index;

/* kfar.asm: draw_border */
void draw_border(u16 *edi, u8 ah, int ebx, int edx)
{
	u16 *p = edi;
	*p++ = CELL(0xC9, ah);
	for (int i = 0; i < ebx - 2; i++) *p++ = CELL(0xCD, ah);
	*p++ = CELL(0xBB, ah);
	edi += cur_width;
	for (int l = 0; l < edx - 2; l++, edi += cur_width) {
		p = edi;
		*p++ = CELL(0xBA, ah);
		for (int i = 0; i < ebx - 2; i++) *p++ = CELL(0x20, ah);
		*p++ = CELL(0xBA, ah);
	}
	p = edi;
	*p++ = CELL(0xC8, ah);
	for (int i = 0; i < ebx - 2; i++) *p++ = CELL(0xCD, ah);
	*p++ = CELL(0xBC, ah);
}

/* kfar.asm: calc_colwidths: the columns of zero width share what the
 * others leave of the panel */
void calc_colwidths(PanelData *ebp)
{
	const PanelMode *esi = &colmodes[ebp->colmode];
	int ecx = esi->NumColumns;
	int edx = ecx - 1;                      /* the separators */
	int ebx = 0;
	for (int i = 0; i < ecx; i++) {
		edx += esi->ColumnWidths[i];
		if (!esi->ColumnWidths[i]) ebx++;
	}
	u32 *edi = ebp->colwidths;
	int share = 0, rem = 0;
	if (ebx) {
		int eax = ebp->width - 2 - edx;
		if (eax < 0) eax = 0;
		share = eax / ebx;
		rem = eax % ebx;
		if (!share) {
			rem = 0;
			share = 1;
		}
	}
	for (int i = 0; i < ecx; i++) {
		int eax = esi->ColumnWidths[i];
		if (!eax) {
			eax = share;
			ebx--;
			if (ebx < rem) eax++;
		}
		*edi++ = eax;
	}
	*edi = 0;
}

/* kfar.asm: get_file_color: esi the entry, ecx its index */
static u8 get_file_color(PanelData *ebp, const u8 *esi, int ecx)
{
	bool cursor = ebp == active_panel && ecx == ebp->index;
	if (bdfe_sel(esi) & 1) return cursor ? esi[7] : esi[6];
	return cursor ? esi[5] : esi[4];
}

/* kfar.asm: draw_name_column */
static void draw_name_column(PanelData *ebp, u16 *edi)
{
	last_column_index = column_index;
	int edx = ebp->numfiles;
	for (int ecx = column_height - 1; ecx > 0; ecx--, edi += cur_width, column_index++) {
		if (column_index >= edx) break;
		u8 *esi = ebp->files[column_index];
		u32 ebx = bdfe_attr(esi);
		u8 ah = get_file_color(ebp, esi, column_index);
		const char *s = bdfe_name(esi);
		u16 *p = edi;
		int n = column_width;
		for (; n && *s; n--) *p++ = CELL(*s++, ah);
		if (!n && *s) {
			/* the name is longer than the column: '}' on the separator */
			*(u8 *)p = '}';
			continue;
		}
		/* aligned extensions (not for folders) */
		if (!(ebx & 0x10) && colmodes[ebp->colmode].bAlignExtensions) {
			int len = column_width - n;     /* the characters written */
			const char *dot = 0;
			for (int k = 1; k <= len; k++)
				if (s[-k] == '.') {
					dot = s - k;
					break;
				}
			if (len && dot) {
				int k = s - dot;        /* the dot and the extension */
				p -= k;
				int ext = k - 1;
				if (ext < 3) ext = 3;
				int ebx2 = column_width - ext - (p - edi);
				int pad = n + 1, tail = 0;
				if (pad > ebx2) {
					tail = pad - ebx2;
					pad = ebx2;
				}
				while (pad-- > 0) *p++ = CELL(' ', ah);
				for (const char *e = dot + 1; *e; e++) *p++ = CELL(*e, ah);
				n = tail;
			}
		}
		/* .noalignext */
		while (n-- > 0) *p++ = CELL(' ', ah);
	}
	/* .ret */
	ebp->colst += ebp->colsz;
	if (ebp != &panel1) return;
	/* the number of the viewer's and the editor's screens */
	int ebx = 0, esi = 0;
	for (int i = 0; i < num_screens; i++) {
		if (screens[i].vtable == &viewer_vtable) ebx++;
		if (screens[i].vtable == &editor_vtable) esi++;
	}
	if (!(ebx | esi)) return;
	u16 *p = get_console_ptr(0, 0);
	u8 c = panel_nscreens_color;
	char buf[24];
	int len = esi ? snprintf(buf, sizeof(buf), "[%d+%d]", ebx, esi) : snprintf(buf, sizeof(buf), "[%d]", ebx);
	for (int i = 0; i < len; i++) *p++ = CELL(buf[i], c);
}

/* kfar.asm: draw_empty_column */
static void draw_empty_column(PanelData *ebp, u16 *edi)
{
}

/* kfar.asm: the size of an entry, written backwards from edi (the
 * column's last cell) in the colour bh; returns edi and the cells left */
static u16 *write_size_back(u16 *edi, const u8 *esi, u8 bh, int *ecx)
{
	if (bdfe_is_dotdot(esi)) {
		*edi-- = CELL(' ', bh);
		*edi-- = CELL(' ', bh);
		*edi-- = CELL('p', bh);
		*edi-- = CELL('U', bh);
		*ecx -= 4;
		return edi;
	}
	if (bdfe_attr(esi) & 0x10) {
		static const char f[] = "Folder";
		for (int i = 5; i >= 0; i--) *edi-- = CELL(f[i], bh);
		*ecx -= 6;
		return edi;
	}
	/* .nofolder */
	u32 eax = *(const u32 *)(esi + 32), edx = *(const u32 *)(esi + 36);
	u8 al = 0;
	if (edx) {
		if (edx < 10000 / 4 * 1024) {
			al = 'G';
			edx <<= 2;
		} else {
			al = 'T';
			edx >>= 10;
		}
	} else {
		edx = eax;
		if (eax >= 1000000) {
			if (eax < 10000 * 1024) {
				al = 'K';
				edx >>= 10;
			} else {
				al = 'M';
				edx >>= 20;
			}
		}
	}
	if (al) {
		/* .write_letter */
		*edi-- = CELL(al, bh);
		*edi-- = CELL(' ', bh);
		*ecx -= 2;
	}
	/* .byte */
	do {
		*edi-- = CELL('0' + edx % 10, bh);
		(*ecx)--;
	} while ((edx /= 10));
	return edi;
}

/* kfar.asm: draw_size_column */
static void draw_size_column(PanelData *ebp, u16 *edi)
{
	edi += column_width - 1;
	int saved = last_column_index;
	for (int n = column_height - 1; n > 0; n--, edi += cur_width, last_column_index++) {
		if (last_column_index >= ebp->numfiles) break;
		const u8 *esi = ebp->files[last_column_index];
		u8 bh = get_file_color(ebp, esi, last_column_index);
		int ecx = column_width;
		u16 *p = write_size_back(edi, esi, bh, &ecx);
		/* .size_written */
		while (ecx-- > 0) *p-- = CELL(' ', bh);
		/* the separator on the left in the entry's colour */
		((u8 *)p)[1] = bh;
	}
	last_column_index = saved;
}

/* kfar.asm: draw_date_column */
static void draw_date_column(PanelData *ebp, u16 *edi)
{
	int saved = last_column_index;
	for (int n = column_height - 1; n > 0; n--, edi += cur_width, last_column_index++) {
		if (last_column_index >= ebp->numfiles) break;
		const u8 *esi = ebp->files[last_column_index];
		u8 bh = get_file_color(ebp, esi, last_column_index);
		((u8 *)edi)[-1] = bh;
		u8 d = esi[28], m = esi[29];
		u32 y = *(const u16 *)(esi + 30) % 100;
		edi[0] = CELL('0' + d / 10, bh);
		edi[1] = CELL('0' + d % 10, bh);
		edi[2] = CELL('.', bh);
		edi[3] = CELL('0' + m / 10, bh);
		edi[4] = CELL('0' + m % 10, bh);
		edi[5] = CELL('.', bh);
		edi[6] = CELL('0' + y / 10, bh);
		edi[7] = CELL('0' + y % 10, bh);
	}
	last_column_index = saved;
}

/* kfar.asm: draw_time_column */
static void draw_time_column(PanelData *ebp, u16 *edi)
{
	int saved = last_column_index;
	for (int n = column_height - 1; n > 0; n--, edi += cur_width, last_column_index++) {
		if (last_column_index >= ebp->numfiles) break;
		const u8 *esi = ebp->files[last_column_index];
		u8 bh = get_file_color(ebp, esi, last_column_index);
		((u8 *)edi)[-1] = bh;
		u8 h = esi[26], m = esi[25];
		edi[0] = CELL('0' + h / 10, bh);
		edi[1] = CELL('0' + h % 10, bh);
		edi[2] = CELL(':', bh);
		edi[3] = CELL('0' + m / 10, bh);
		edi[4] = CELL('0' + m % 10, bh);
	}
	last_column_index = saved;
}

/* kfar.asm: column_headers, draw_column_proc */
static const char *const column_headers[6] = { nullstr, aName, aSize, nullstr, aDate, aTime };
static void (*const draw_column_proc[6])(PanelData *, u16 *) = {
	draw_empty_column, draw_name_column, draw_size_column, 0, draw_date_column, draw_time_column,
};

/* kfar.asm: fill_total_info: " <size> bytes in <eax> file(s) " built
 * backwards before saved_file_name+512; returns its start, *ecx the
 * length */
static char *fill_total_info(u32 eax, u64 size, int *ecx)
{
	char *edi = saved_file_name + 511;
	*edi = ' ';
	if (eax != 1) *--edi = 's';
	memcpy(edi - 4, "file", 4);
	edi[-5] = ' ';
	edi -= 5;
	do *--edi = '0' + eax % 10; while ((eax /= 10));
	memcpy(edi - 4, " in ", 4);
	memcpy(edi - 8, "ytes", 4);
	memcpy(edi - 10, " b", 2);
	edi -= 10;
	char *esi = edi - 3;
	/* .selsizel: the digits, a comma every three */
	do {
		edi--;
		if (edi < esi) {
			*edi-- = ',';
			esi -= 4;
		}
		*edi = '0' + size % 10;
		size /= 10;
	} while (size);
	*--edi = ' ';
	*ecx = saved_file_name + 512 - edi;
	return edi;
}

/* kfar.asm: draw_panel */
void draw_panel(PanelData *ebp)
{
	u16 *edi = get_console_ptr(ebp->left, ebp->top);
	/* draw border */
	u8 ah = panel_border_color;
	draw_border(edi, ah, ebp->width, ebp->height);
	edi = get_console_ptr(ebp->left, ebp->top + ebp->height - 3);
	*edi++ = CELL(0xC7, ah);
	for (int i = 0; i < ebp->width - 2; i++) *edi++ = CELL(0xC4, ah);
	*edi = CELL(0xB6, ah);
	const PanelMode *esi = &colmodes[ebp->colmode];
	int ncols = esi->NumColumns;
	const u32 *ebx = ebp->colwidths;
	column_left = ebp->left + 1;
	column_top = ebp->top + 1;
	column_height = ebp->height - 4;
	ebp->colsz = column_height - 1;
	ebp->colst = 0;
	column_index = ebp->start;
	last_column_index = ebp->start;
	for (int c = 0; c < ncols; c++) {
		/* .columns_loop */
		column_width = *ebx++;
		if (c != ncols - 1) {
			/* the separator on the column's right */
			u16 *p = get_console_ptr(column_left + column_width, ebp->top);
			u8 h = panel_header_color;
			*p = CELL(0xD1, h);
			p += cur_width;
			for (int i = 0; i < column_height; i++, p += cur_width) *p = CELL(0xB3, h);
			*p = CELL(0xC1, h);
		}
		/* .skip_right_border: the column's header */
		edi = get_console_ptr(column_left, column_top);
		int type = esi->ColumnTypes[c] & 0xF;
		const char *hdr = column_headers[type];
		int ecx = (u8)hdr[0];
		hdr++;
		if (ecx >= column_width) ecx = column_width;
		u16 *p = edi;
		for (int i = (column_width - ecx) >> 1; i > 0; i--) *p++ = CELL(' ', column_header_color);
		for (int i = 0; i < ecx; i++) *p++ = CELL(hdr[i], column_header_color);
		for (int i = (column_width - ecx + 1) >> 1; i > 0; i--) *p++ = CELL(' ', column_header_color);
		edi += cur_width;
		/* the column itself */
		if (draw_column_proc[type]) draw_column_proc[type](ebp, edi);
		column_left += column_width + 1;
	}
	/* the panel's header (the current folder) */
	const char *s = ebp->dir;
	/* (plugins: GetPanelTitle) */
	char *d = cur_header;
	int ecx = ebp->width - 6;
	bool created = false;
	if (*s == '/') {
		ecx--;
		*d++ = *s++;
		for (;;) {
			char al = *s++;
			*d++ = al;
			ecx--;
			if (!al) {
				created = true;
				break;
			}
			if (al == '/') break;
		}
	}
	if (!created) {
		/* .copy_rest */
		int len = strlen(s);
		if (len > ecx) {
			*d++ = '.';
			*d++ = '.';
			*d++ = '.';
			ecx -= 3;
			s += len - ecx;
		}
		for (;;) {
			char al = *s++;
			*d++ = al;
			ecx--;
			if (!al) break;
		}
	}
	/* .header_created */
	edi = get_console_ptr(ebp->left + (ecx >> 1) + 3, ebp->top);
	ah = ebp == active_panel ? panel_active_header_color : panel_header_color;
	*edi++ = CELL(' ', ah);
	for (s = cur_header; *s; s++) *edi++ = CELL(*s, ah);
	*edi = CELL(' ', ah);
	/* the sort mode's letter */
	*(u8 *)get_console_ptr(ebp->left + 1, ebp->top + 1) = compare_names[ebp->sortmode];
	if (ebp->selected_num) {
		/* the selected files' information */
		int n;
		char *t = fill_total_info(ebp->selected_num, ebp->selected_size, &n);
		int eax = ebp->width - 2;
		if (n > eax) {
			n = eax;
			t = saved_file_name + 512 - eax;
			t[0] = '.';
			t[1] = '.';
			t[2] = '.';
		}
		u16 *p = get_console_ptr(ebp->left + ((eax - n) >> 1) + 1, ebp->top + ebp->height - 3);
		while (n--) *p++ = CELL(*t++, panel_numselected_color);
	}
	/* .skip_selected_info: the number and size of the panel's files */
	{
		int n;
		char *t = fill_total_info(ebp->total_num, ebp->total_size, &n);
		int eax = ebp->width - 2;
		if (n > eax) {
			n = eax;
			t[n - 3] = '.';
			t[n - 2] = '.';
			t[n - 1] = '.';
		}
		u16 *p = get_console_ptr(ebp->left + ((eax - n) >> 1) + 1, ebp->top + ebp->height - 1);
		while (n--) *p++ = CELL(*t++, panel_number_color);
	}
	if (ebp->numfiles) {
		/* the current file's information */
		const u8 *e = ebp->files[ebp->index];
		edi = get_console_ptr(ebp->left + ebp->width - 1, ebp->top + ebp->height - 2);
		ecx = ebp->width - 2;
		u8 c = panel_normal_color;
		if (ebp->colmode != 3) {
			/* the time of modification */
			edi -= 5;
			ecx -= 6;
			u8 h = e[24 + 2], m = e[24 + 1];
			edi[0] = CELL('0' + h / 10, c);
			edi[1] = CELL('0' + h % 10, c);
			edi[2] = CELL(':', c);
			edi[3] = CELL('0' + m / 10, c);
			edi[4] = CELL('0' + m % 10, c);
			*--edi = CELL(' ', c);
			/* the date of modification */
			edi -= 8;
			ecx -= 9;
			u8 dd = e[28 + 0], mm = e[28 + 1];
			u32 yy = *(const u16 *)(e + 28 + 2) % 100;
			edi[0] = CELL('0' + dd / 10, c);
			edi[1] = CELL('0' + dd % 10, c);
			edi[2] = CELL('.', c);
			edi[3] = CELL('0' + mm / 10, c);
			edi[4] = CELL('0' + mm % 10, c);
			edi[5] = CELL('.', c);
			edi[6] = CELL('0' + yy / 10, c);
			edi[7] = CELL('0' + yy % 10, c);
			*--edi = CELL(' ', c);
			/* the size, backwards */
			edi--;
			ecx--;
			if (bdfe_attr(e) & 0x10) {
				const char *f = bdfe_is_dotdot(e) ? aUp : aFolder;
				int len = bdfe_is_dotdot(e) ? aUp_size : aFolder_size;
				ecx -= len;
				for (int i = len - 1; i >= 0; i--) *edi-- = CELL(f[i], c);
			} else {
				/* .size_file */
				u32 edx = *(const u32 *)(e + 36);
				u8 al = 0;
				if (edx) {
					if (edx >= 10 * 256) {
						edx >>= 8;      /* in terabytes */
						al = 'T';
					} else {
						al = 'G';       /* in gigabytes */
						edx <<= 2;
					}
				} else {
					edx = *(const u32 *)(e + 32);
					if (edx >= 10 * (1 << 20)) {
						al = 'M';
						edx >>= 20;
					} else if (edx >= 1 << 20) {
						edx >>= 10;
						al = 'K';
					}
				}
				if (al) {
					/* .size_letter */
					*edi-- = CELL(al, c);
					ecx--;
					*edi-- = CELL(' ', c);
					ecx--;
				}
				/* .size_num */
				do {
					*edi-- = CELL('0' + edx % 10, c);
					ecx--;
				} while ((edx /= 10));
			}
			/* .size_done */
		}
		/* .show_curname: the name */
		edi -= ecx;
		const char *name = bdfe_name(e);
		int len = strlen(name);
		if (len > ecx) {
			edi[-1] = CELL('{', c);
			name += len - ecx;
		}
		while (ecx > 0 && *name) {
			*edi++ = CELL(*name++, c);
			ecx--;
		}
		while (ecx-- > 0) *edi++ = CELL(' ', c);
	}
	/* .skip_curinfo */
	draw_image();
}

/* kfar.asm: highlight_init: the entry's colours by the highlighting
 * groups (its attributes and its name in lower case) */
static void highlight_init(u8 *ebp)
{
	char *edi = lower_file_name;
	const char *esi = bdfe_name(ebp);
	while ((*edi = tolower_table[(u8)*esi++])) edi++;
	for (int ecx = 0; ecx < highlight_num_groups; ecx++) {
		const highlight *ebx = highlight_groups[ecx];
		u8 al = ebx->IncludeAttributes;
		if ((ebp[0] & al) != al) continue;
		if (!match_mask_rev_lowercase(lower_file_name, edi, ebx->Mask)) continue;
		ebp[4] = ebx->NormalColor ? ebx->NormalColor : panel_normal_color;
		ebp[5] = ebx->CursorColor ? ebx->CursorColor : panel_cursor_color;
		ebp[6] = ebx->SelectedColor ? ebx->SelectedColor : panel_selected_color;
		ebp[7] = ebx->SelectedCursorColor ? ebx->SelectedCursorColor : panel_selected_cursor_color;
		return;
	}
	/* .nohighlight */
	ebp[4] = panel_normal_color;
	ebp[5] = panel_cursor_color;
	ebp[6] = panel_selected_color;
	ebp[7] = panel_selected_cursor_color;
}

/* kfar.asm: read_folder: the panel's folder into its files (function
 * 70.1), the highlighting, the sorting; on an error the previous folder,
 * the folders above, /rd/1 */
void read_folder(PanelData *ebp)
{
	for (;;) {
		dirinfo.size = ebp->nfa;
		dirinfo.data = (u8 *)(ebp->files + ebp->nfa);
		dirinfo.name = ebp->dir;
		ebp->total_num = 0;
		ebp->total_size = 0;
		ebp->selected_num = 0;
		ebp->selected_size = 0;
		u32 eax, ebx;
		for (;;) {
			/* .retry: (a plugin's ReadFolder) .native */
			eax = fs_call(&dirinfo, &ebx);
			if (!eax || eax == 6) break;
			/* failed to read folder, notify user */
			if (bSilentFolderMode) goto dont_notify;
			const char *buttons[2] = { aRetry, aContinue };
			const char *strings[3] = { get_error_msg(eax), aCannotReadFolder, dirinfo.name };
			if (SayErr(3, strings, 2, buttons)) goto dont_notify;
		}
		/* .ok: all entries read? */
		u8 *hdr = dirinfo.data;
		u32 total = *(u32 *)(hdr + 8);
		if (total != ebx) {
			pgfree(ebp->files);
			u32 n = (total + 0xF) & ~0xF;
			u8 **f = xpgalloc(n * (4 + 304) + 32);
			if (!f) {
				/* (the original goes on with the freed buffer) */
				ebp->files = xpgalloc(16 * (4 + 304) + 32);
				ebp->nfa = 16;
				ebp->numfiles = ebp->index = ebp->start = 0;
				return;
			}
			ebp->files = f;
			ebp->nfa = n;
			prev_dir[0] = 0;
			continue;               /* read_folder */
		}
		/* .readdone */
		ebp->start = 0;
		ebp->index = 0;
		u8 **edi = ebp->files;
		u8 *e = (u8 *)(ebp->files + ebp->nfa) + 32;
		u8 *edx = 0;
		u32 ecx = *(u32 *)(e - 32 + 4);
		/* ignore the special entries of the folder '.' and of the label */
		for (; ecx; ecx--, e += 304) {
			/* .ptrinit */
			if (e[40] == '.' && !e[41]) continue;
			if (e[0] & 8) continue;
			bdfe_sel(e) = 0;
			if (!edx && bdfe_is_dotdot(e)) {
				edx = e;
				memmove(ebp->files + 1, ebp->files, (edi - ebp->files) * sizeof(u8 *));
				ebp->files[0] = e;
				edi++;
			} else {
				/* .nodotdot */
				*edi++ = e;
				if (!(e[0] & 0x10)) ebp->total_num++;
				ebp->total_size += bdfe_size(e);
			}
			/* .dotdot: the highlighting */
			highlight_init(e);
		}
		/* .loopdone */
		ebp->numfiles = edi - ebp->files;
		/* (plugin_flags & 1: a '..' for the plugin's root) */
		sort_files(ebp);
		return;
	dont_notify:
		/* if not on plugin panel, try to return to previous directory */
		if (!ebp->hPlugin && prev_dir[0]) {
			memcpy(ebp->dir, prev_dir, 1024);
			prev_dir[0] = 0;
			return;
		}
		bSilentFolderMode = 1;          /* enter silent mode */
		/* try to read parent folder */
		char *s = ebp->dir, *last = 0;
		int slashes = 0;
		for (; *s; s++)
			if (*s == '/') {
				slashes++;
				last = s;
			}
		/* .up1done */
		if (slashes > 2) {
			*last = 0;              /* .up */
			continue;
		}
		/* .noup: there is no parent folder, and we are not on plugin
		 * panel; try to read ramdisk */
		s = ebp->dir;
		if (!(s[0] == '/' && (s[1] | 0x20) == 'r' && (s[2] | 0x20) == 'd' && s[3] == '/' && s[4] == '1' && !s[5])) {
			strcpy(s, "/rd/1");
			continue;
		}
		/* .nosetrd: even the ramdisk could not be read */
		ebp->numfiles = 0;
		ebp->index = 0;
		ebp->start = 0;
		bSilentFolderMode = 0;          /* leave silent mode */
		return;
	}
}

/* kfar.asm: sort_files (the '..' stays first) */
void sort_files(PanelData *ebp)
{
	compare_fn ebx = compare_fns[ebp->sortmode];
	u8 **edx = ebp->files;
	u32 ecx = ebp->numfiles;
	if (ecx) {
		if (bdfe_is_dotdot(*edx)) {
			ecx--;
			edx++;
		}
		sort(edx, ecx, ebx);
	}
	/* .skip */
	bSilentFolderMode = 0;                  /* leave silent mode */
}

/* kfar.asm: match_symbol: in: al, [edi] = the symbols; out: as 'cmp
 * al,[edi]' after the lower case */
int match_symbol(u8 al, u8 edi)
{
	return (int)tolower_table[al] - (int)tolower_table[edi];
}

/* kfar.asm: strcmpi */
int strcmpi(const char *esi, const char *edi)
{
	for (;; esi++, edi++) {
		int r = match_symbol(*esi, *edi);
		if (r || !*esi) return r;
	}
}

/* kfar.asm: seek_ext: after the last dot, or the end */
static const char *seek_ext(const char *esi)
{
	const char *eax = strrchr(esi, '.');
	return eax ? eax + 1 : esi + strlen(esi);
}

/* the folders before the files (compare_*.1dir, .greater, .less) */
#define DIRS_FIRST(esi, edi)                                            \
	do {                                                            \
		bool d1 = bdfe_attr(esi) & 0x10, d2 = bdfe_attr(edi) & 0x10; \
		if (d1 != d2) return d1 ? -1 : 1;                       \
	} while (0)
static int cmpu(u32 a, u32 b) { return a > b ? 1 : a < b ? -1 : 0; }

/* kfar.asm: compare_name, compare_name_rev */
static int compare_name(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return strcmpi(bdfe_name(esi), bdfe_name(edi));
}

static int compare_name_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return strcmpi(bdfe_name(edi), bdfe_name(esi));
}

/* kfar.asm: compare_ext, compare_ext_rev */
static int compare_ext(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	int r = strcmpi(seek_ext(bdfe_name(esi)), seek_ext(bdfe_name(edi)));
	return r ? r : strcmpi(bdfe_name(esi), bdfe_name(edi));
}

static int compare_ext_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	int r = strcmpi(seek_ext(bdfe_name(edi)), seek_ext(bdfe_name(esi)));
	return r ? r : strcmpi(bdfe_name(edi), bdfe_name(esi));
}

/* kfar.asm: compare_modified ... compare_accessed_rev: the date's and
 * time's dwords (the newer first), then the names */
static int compare_times(const u8 *a, const u8 *b, int date, int time)
{
	int r = cmpu(*(const u32 *)(b + date), *(const u32 *)(a + date));
	if (!r) r = cmpu(*(const u32 *)(b + time), *(const u32 *)(a + time));
	return r ? r : strcmpi(bdfe_name(a), bdfe_name(b));
}

static int compare_modified(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(esi, edi, 28, 24);
}

static int compare_modified_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(edi, esi, 28, 24);
}

/* kfar.asm: compare_size, compare_size_rev (the larger first) */
static int compare_size(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(esi, edi, 36, 32);
}

static int compare_size_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(edi, esi, 36, 32);
}

/* kfar.asm: compare_unordered, compare_unordered_rev: as on the disk */
static int compare_unordered(const u8 *esi, const u8 *edi)
{
	return cmpu((u32)esi, (u32)edi);
}

static int compare_unordered_rev(const u8 *esi, const u8 *edi)
{
	return cmpu((u32)edi, (u32)esi);
}

/* kfar.asm: compare_created, compare_created_rev */
static int compare_created(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(esi, edi, 12, 8);
}

static int compare_created_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(edi, esi, 12, 8);
}

/* kfar.asm: compare_accessed, compare_accessed_rev */
static int compare_accessed(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(esi, edi, 20, 16);
}

static int compare_accessed_rev(const u8 *esi, const u8 *edi)
{
	DIRS_FIRST(esi, edi);
	return compare_times(edi, esi, 20, 16);
}

/* kfar.asm: compare_fns */
const compare_fn compare_fns[14] = {
	compare_name, compare_name_rev, compare_ext, compare_ext_rev, compare_modified, compare_modified_rev,
	compare_size, compare_size_rev, compare_unordered, compare_unordered_rev, compare_created,
	compare_created_rev, compare_accessed, compare_accessed_rev,
};

/* kfar.asm: match_single_mask_rev_lowercase: in: esi -> the end of the
 * mask, ecx -> its start, edi -> the end of the name, edx -> its start;
 * out: true (CF clear) <=> match. From the end backwards: '*', '?',
 * '[...]' with ranges a-z */
static bool match_single_mask_rev_lowercase(const char *esi, const char *ecx, const char *edi, const char *edx)
{
	for (;;) {
		/* .mask_symbol */
		esi--;
		if (esi < ecx) return edi == edx;       /* .mask_done */
		u8 al = *esi;
		if (al == '*') break;
		edi--;
		if (edi < edx) return false;
		if (al == '?') continue;
		if (al == ']') {
			/* .list_check */
			for (;;) {
				esi--;
				if (esi <= ecx) return false;
				al = *esi;
				if (al == '[') return false;
				if (esi[-1] == '-') {
					/* .range */
					if (al >= (u8)*edi && (u8)esi[-2] <= (u8)*edi) break;
					esi -= 2;
					continue;
				}
				if (al == (u8)*edi) break;
			}
			/* .listok */
			do {
				esi--;
				if (esi < ecx) return false;
			} while (*esi != '[');
			continue;
		}
		if (al != (u8)*edi) return false;
	}
	/* .asterisk */
	for (;;) {
		if (esi == ecx) return true;
		if (esi[-1] != '?') break;
		if (edi == edx) return false;
		esi--;
		edi--;
	}
	if (esi[-1] == ']') {
		/* .asterisk_common */
		for (;;) {
			if (match_single_mask_rev_lowercase(esi, ecx, edi, edx)) return true;
			edi--;
			if (edi < edx) return false;
		}
	}
	/* .asterisk_normal: the mask is ...<normal symbol>*... */
	u8 al = esi[-1];
	for (;;) {
		do {
			edi--;
			if (edi < edx) return false;
		} while (al != (u8)*edi);
		if (match_single_mask_rev_lowercase(esi - 1, ecx, edi, edx)) return true;
	}
}

/* kfar.asm: match_mask_rev_lowercase: in: esi -> the name, edx -> its
 * end, edi -> the mask (masks separated by ',' or ';', then '|' and the
 * masks excluded); out: true (CF clear) <=> match */
bool match_mask_rev_lowercase(const char *esi, const char *edx, const char *edi)
{
	const char *m = edi;
	for (;;) {
		/* .main_cycle */
		const char *ecx = m;
		while (*m && *m != ',' && *m != ';' && *m != '|') m++;
		char al = *m;
		bool ok = match_single_mask_rev_lowercase(m, ecx, edx, esi);
		m++;
		if (ok) break;                  /* .found */
		if (al == ',' || al == ';') continue;
		return false;                   /* .done_fail */
	}
	/* .found: the masks up to '|' or the end */
	char al = m[-1];
	while (al && al != '|') al = *m++;
	if (!al) return true;                   /* .done_succ */
	/* .test_exclude */
	for (;;) {
		const char *ecx = m;
		while (*m && *m != ',' && *m != ';') m++;
		al = *m;
		if (match_single_mask_rev_lowercase(m, ecx, edx, esi)) return false;
		m++;
		if (!al) return true;
	}
}
