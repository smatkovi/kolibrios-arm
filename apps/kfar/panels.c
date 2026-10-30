/* kfar for KolibriOS for ARM: the panels' keys and operations
 * (kfar.asm: panels_OnKey ... panels_getname, find_extension).
 *
 * Translated to C from kfar's kfar.asm, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0.
 *
 * panels_OnKey's local labels are the functions panels_OnKey_x; the
 * keys reach them through panels_ctrlkeys (process_ctrl_keys), the key
 * bar's mouse through data.c's mouse_panels. Each of them takes the
 * panel (ebp) and the key (eax); the original's ecx (the index), edx
 * (start) and ebx (start + colst) are computed from the panel.
 *
 * Differences: plugin panels do not exist (kfar.h), their branches are
 * left out; PgUp to the first file scrolls the panel (the original
 * compares unsigned there and leaves the cursor above the panel).
 *
 * Added: F6 and Shift+F6, rename or move (kfar 0.65 has only their
 * names on the key bar): F5's and Shift+F5's dialog and its folders,
 * then function 70.10 within a volume, a copy and a delete between
 * volumes (panels_OnKey_f6, move_file). */
#include "kfar.h"

static const char aMenu[] = "Menu";

static bool quick_find(PanelData *ebp);
static bool quick_find_next(PanelData *ebp);
static bool quick_find_prev(PanelData *ebp);
static void panels_OnKey_copydone(PanelData *ebp);
static void panels_OnKey_bigfilename(void);
static void panels_OnKey_l2(const char *eax);
static void panels_OnKey_dotdot(PanelData *ebp);

#define PANEL(p) ((PanelData *)(p))

/* kfar.asm: panels_OnKey.done_redraw */
void panels_OnKey_done_redraw(PanelData *ebp)
{
	draw_panel(ebp);
}

/* kfar.asm: panels_OnKey.reread, .done_cmdbar */
void panels_OnKey_reread(PanelData *ebp)
{
	read_folder(ebp);
	draw_cmdbar();
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.up */
static void panels_OnKey_up(void *p, u32 eax)
{
	PanelData *ebp = p;
	int ecx = ebp->index;
	if (!ecx) return;
	ecx--;
	ebp->index = ecx;
	if (ecx < ebp->start) ebp->start = ecx;
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.down (and the end of .insert) */
static void panels_OnKey_down_to(PanelData *ebp, int ecx)
{
	ebp->index = ecx;
	if (ecx >= ebp->start + ebp->colst) ebp->start = ecx - ebp->colst + 1;
	panels_OnKey_done_redraw(ebp);
}

static void panels_OnKey_down(void *p, u32 eax)
{
	PanelData *ebp = p;
	int ecx = ebp->index + 1;
	if (ecx >= ebp->numfiles) return;
	panels_OnKey_down_to(ebp, ecx);
}

/* kfar.asm: panels_OnKey.insert: (de)select, then down */
static void panels_OnKey_insert(void *p, u32 eax)
{
	PanelData *ebp = p;
	int ecx = ebp->index;
	u8 *e = ebp->files[ecx];
	if (!bdfe_is_dotdot(e)) {
		bdfe_sel(e) ^= 1;
		if (bdfe_sel(e) & 1) {          /* .insert.increase */
			ebp->selected_size += bdfe_size(e);
			ebp->selected_num++;
		} else {
			ebp->selected_size -= bdfe_size(e);
			ebp->selected_num--;
		}
	}
	/* .insert.down */
	ecx++;
	if (ecx >= ebp->numfiles) {
		panels_OnKey_done_redraw(ebp);
		return;
	}
	panels_OnKey_down_to(ebp, ecx);
}

/* kfar.asm: panels_OnKey.finalize_left */
static void panels_OnKey_finalize_left(PanelData *ebp, int ecx, int eax)
{
	int edx = ebp->start;
	if (ecx < edx) {
		edx -= eax;
		if (edx < 0) edx = 0;
		ebp->start = edx;
	}
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.left: one column to the left */
static void panels_OnKey_left(void *p, u32 al)
{
	PanelData *ebp = p;
	int ecx = ebp->index;
	if (!ecx) return;
	int eax = ebp->colsz;
	ecx -= eax;
	if (ecx < 0) ecx = 0;
	ebp->index = ecx;
	panels_OnKey_finalize_left(ebp, ecx, eax);
}

/* kfar.asm: panels_OnKey.pgup */
static void panels_OnKey_pgup(void *p, u32 al)
{
	PanelData *ebp = p;
	int eax = ebp->colst - 1;
	if (!eax) eax = 1;
	int ecx = ebp->index;
	if (!ecx) return;
	ecx -= eax;
	if (ecx < 0) ecx = 0;
	ebp->index = ecx;
	panels_OnKey_finalize_left(ebp, ecx - 1, eax);
}

/* kfar.asm: panels_OnKey.finalize_right */
static void panels_OnKey_finalize_right(PanelData *ebp, int ecx, int eax)
{
	int ebx = ebp->start + ebp->colst;
	if (ecx >= ebx) {
		ebx += eax;
		if (ebx > ebp->numfiles) ebx = ebp->numfiles;
		ebx -= ebp->colst;
		if (ebx < 0) ebx = 0;
		ebp->start = ebx;
	}
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.right */
static void panels_OnKey_right(void *p, u32 al)
{
	PanelData *ebp = p;
	int eax = ebp->colsz;
	int ecx = ebp->index + eax;
	if (ecx >= ebp->numfiles) ecx = ebp->numfiles - 1;
	ebp->index = ecx;
	panels_OnKey_finalize_right(ebp, ecx, eax);
}

/* kfar.asm: panels_OnKey.pgdn */
static void panels_OnKey_pgdn(void *p, u32 al)
{
	PanelData *ebp = p;
	int eax = ebp->colst - 1;
	if (!eax) eax = 1;
	int ecx = ebp->index + eax;
	if (ecx >= ebp->numfiles) ecx = ebp->numfiles - 1;
	ebp->index = ecx;
	panels_OnKey_finalize_right(ebp, ecx + 1, eax);
}

/* kfar.asm: panels_OnKey.tab */
static void panels_OnKey_tab(void *p, u32 al)
{
	PanelData *ebp = p;
	active_panel = other_panel(active_panel);
	draw_cmdbar();
	draw_panel(ebp);
	panels_OnKey_done_redraw(active_panel);
}

/* kfar.asm: panels_OnKey.home */
static void panels_OnKey_home(void *p, u32 al)
{
	PanelData *ebp = p;
	ebp->start = 0;
	ebp->index = 0;
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.end */
static void panels_OnKey_end(void *p, u32 al)
{
	PanelData *ebp = p;
	int eax = ebp->numfiles - 1;
	ebp->index = eax;
	eax = eax + 1 - ebp->colst;
	if (eax < 0) eax = 0;
	ebp->start = eax;
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: the full name of the entry in execdata: dir/name; false if
 * too long (.bigfilename) */
static bool panels_OnKey_fullname(PanelData *ebp, const u8 *ecx)
{
	int n = strlen(ebp->dir);
	if (n >= (int)sizeof(execdata) - 1) return false;
	memcpy(execdata, ebp->dir, n);
	execdata[n++] = '/';
	const char *name = bdfe_name(ecx);
	int m = strlen(name) + 1;
	if (n + m > (int)sizeof(execdata)) return false;
	memcpy(execdata + n, name, m);
	return true;
}

/* kfar.asm: panels_OnKey.dorun: start the program (execptr) with the
 * parameters (execparams); the entry's full name in execdata */
static void panels_OnKey_dorun(PanelData *ebp, const u8 *ecx)
{
	if (!panels_OnKey_fullname(ebp, ecx)) {
		panels_OnKey_bigfilename();
		return;
	}
	/* for fasm call - special handling, because
	 * 1) fasm command line convention is different: fasm infile,outfile[,path]
	 *    rather than tinypad infile
	 * 2) fasm will probably create new file in directory, so we want to
	 *    reload panel data */
	int edx = 0;
	if (execparams) {
		const char *prog = execptr;
		int l = strlen(prog);
		if (l >= 4 && ((prog[l - 4] | 0x20) == 'f') && ((prog[l - 3] | 0x20) == 'a') &&
		    ((prog[l - 2] | 0x20) == 's') && ((prog[l - 1] | 0x20) == 'm')) {
			int n = strlen(execdata);
			if (n + 1 > (int)sizeof(execdata) / 2) {
				panels_OnKey_bigfilename();
				return;
			}
			/* "infile,outfile": the output without the extension, or .bin */
			char *edi = execdata + n;
			*edi = ',';
			memcpy(edi + 1, execdata, n + 1);
			edi += 1 + n;
			edx = 1;
			char *q = edi;
			while (--q > execdata + n + 1 && *q != '.' && *q != '/');
			if (*q == '.') {
				*q = 0;         /* .ptfound */
			} else {
				if (edi + 4 >= execdataend) {
					panels_OnKey_bigfilename();
					return;
				}
				strcpy(edi, ".bin");
			}
		}
	}
	/* .nofasm: if the command line is more than 256 symbols, the kernel
	 * will truncate it; we do not want this */
	if (execparams && strlen((const char *)execparams) > 256) {
		panels_OnKey_l2(aCmdLineTooBig);
		return;
	}
	int eax = fs_call(&execinfo, 0);
	if (restore_semicolon) {
		*restore_semicolon = ';';
		restore_semicolon = 0;
	}
	if (eax <= 0) {
		const char *strings[2] = { aRunError, get_error_msg(-eax) };
		SayErr(2, strings, 1, ContinueBtn);
		return;
	}
	if (edx) {
		kos_delay(20);
		panels_OnKey_ctrl_r(ebp, 0);
	}
}

/* kfar.asm: panels_OnKey.run_association2: edi the program */
void panels_OnKey_run_association2(PanelData *ebp, const u8 *ecx, char *edi)
{
	execparams = (u32)execdata;
	execptr = edi;
	panels_OnKey_dorun(ebp, ecx);
}

/* kfar.asm: panels_OnKey.bigfilename, .l2, .bigcmdline, .bigfoldername */
static void panels_OnKey_l2(const char *eax)
{
	if (restore_semicolon) {
		*restore_semicolon = ';';
		restore_semicolon = 0;
	}
	SayErr(1, &eax, 1, ContinueBtn);
}

static void panels_OnKey_bigfilename(void)
{
	panels_OnKey_l2(aFileNameTooBig);
}

static void panels_OnKey_bigfoldername2(PanelData *ebp)
{
	strcpy(ebp->dir, prev_dir);
	panels_OnKey_l2(aFolderNameTooBig);
}

/* kfar.asm: panels_OnKey.enter_folder */
static void panels_OnKey_enter_folder(PanelData *ebp, const u8 *ecx)
{
	const char *esi = bdfe_name(ecx);
	if (bdfe_is_dotdot(ecx)) {
		panels_OnKey_dotdot(ebp);
		return;
	}
	strcpy(prev_dir, ebp->dir);
	char *edi = ebp->dir + strlen(ebp->dir);
	char *edx = ebp->dir + 1024;
	if (edi + 1 >= edx) {
		panels_OnKey_l2(aFolderNameTooBig);
		return;
	}
	if (edi[-1] != '/') *edi++ = '/';
	for (;;) {
		char al = *esi++;
		if (edi >= edx) {
			panels_OnKey_bigfoldername2(ebp);
			return;
		}
		*edi++ = al;
		if (!al) break;
	}
	/* (a plugin's SetFolder here) */
	panels_OnKey_reread(ebp);
}

/* kfar.asm: panels_OnKey.dotdot: the folder above, the cursor on the
 * folder we come from */
static void panels_OnKey_dotdot(PanelData *ebp)
{
	char *edi = ebp->dir;
	if (edi[0] == '/' && edi[1] == 0) {
		/* (the root of a plugin's panel: close_plugin_panel) */
		return;                         /* .retd */
	}
	/* .dotdot_noroot */
	char *edx = edi;
	edi = strrchr(edi, '/');
	if (!edi) return;
	if (edi == edx) edi++;
	char saved[4];
	memcpy(saved, edi, 4);
	*edi = 0;
	/* .dotdot_native */
	read_folder(ebp);
	memcpy(edi, saved, 4);
	char *name = edi;
	if (*name == '/') name++;
	for (int i = 0; i < ebp->numfiles; i++) {
		const char *s = bdfe_name(ebp->files[i]), *t = name;
		/* the names up to the end of the folder's, in its 4 bytes
		 * kept (the rest of the old name follows) */
		for (;; s++, t++) {
			if (match_symbol(*s, *t)) break;
			if (!*s) goto scanfound;
		}
		continue;
	scanfound:
		ebp->index = i;
		if (i >= ebp->colst) ebp->start = i - ebp->colst + 1;
		break;
	}
	/* .scandone */
	*edi = 0;
	draw_cmdbar();
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.enter */
void panels_OnKey_enter(void *p, u32 al)
{
	PanelData *ebp = p;
	if (bQuickSearchMode) draw_image();
	if (!ebp->numfiles) return;
	u8 *ecx = get_curfile_folder_entry(ebp);
	if (bdfe_attr(ecx) & 0x10) {
		panels_OnKey_enter_folder(ebp, ecx);
		return;
	}
	/* todo: add <Enter> handling on plugin panel */
	if (ebp->hPlugin) return;
	/* generate full file name */
	if (!panels_OnKey_fullname(ebp, ecx)) {
		panels_OnKey_bigfilename();
		return;
	}
	/* try to open file and look for all plugins */
	_FILE *esi = open(execdata, O_READ);
	if (esi) {
		u32 edi = read(esi, filedata_buffer, filedata_buffer_size);
		close(esi);
		/* test for Kolibri executable (MENUET01, and MENUETA1 here) */
		if (edi >= 0x24 && (!memcmp(filedata_buffer, "MENUET", 6) ||
				    (!memcmp(filedata_buffer, "KPCK", 4) &&
				     *(u32 *)(filedata_buffer + 12) == 0x26914601)))
			goto run_app;
		/* .enter.noexec: the plugins' OpenFilePlugin (none) */
	}
	/* .noplugins: run program or association */
	char *edi = find_extension(ecx);
	if (!edi) goto run_app;
	/* .run_association */
	if (!*edi || *edi == ';') return;
	char *semi = strchr(edi, ';');
	if (semi) {
		*semi = 0;
		restore_semicolon = semi;
	}
	panels_OnKey_run_association2(ebp, ecx, edi);
	return;
run_app:
	execptr = execdata;
	execparams = 0;
	panels_OnKey_dorun(ebp, ecx);
}

/* kfar.asm: panels_OnKey.ctrl_f3_9: the sort modes */
void panels_OnKey_ctrl_f3_9(void *p, u32 eax)
{
	PanelData *ebp = p;
	u8 al = ((u8)eax - 0x3D) * 2;
	if (al == (ebp->sortmode & 0xFE))
		al = ((ebp->sortmode & 1) | al) ^ 1;
	ebp->sortmode = al;
	u8 *cur = ebp->numfiles ? ebp->files[ebp->index] : 0;
	sort_files(ebp);
	int ecx = 0;
	while (ecx < ebp->numfiles && ebp->files[ecx] != cur) ecx++;
	if (ecx == ebp->numfiles) ecx = 0;
	ebp->index = ecx;
	ecx -= ebp->start;
	if (ecx < 0)
		ebp->start += ecx;              /* .less_start */
	else if ((ecx -= ebp->colst) >= 0)
		ebp->start += ecx + 1;          /* .gr_end */
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.alt_f1_2: the menu of the drives (the folders
 * of the folders of "/") for the left or the right panel */
void panels_OnKey_alt_f1_2(void *p, u32 eax)
{
	PanelData *ebp = (u8)eax == 0x3B ? &panel1 : &panel2;
	typedef struct { listitem *next, *prev; char text[32]; } drive_item;
	/* get drives list */
	fileinfo *ebx = &dirinfo;
	ebx->size = 1;
	ebx->data = driveinfo;
	ebx->name = tmpname;
	tmpname[0] = '/';
	drive_item *edx = 0;
	int ecx = 0;
	for (ebx->first = 0;; ebx->first++) {
		/* .drive_loop_e */
		tmpname[1] = 0;
		if (fs_call(ebx, 0)) break;
		strncpy(tmpname + 1, (char *)driveinfo + 32 + 40, sizeof(tmpname) - 2);
		u32 first = ebx->first;
		for (ebx->first = 0;; ebx->first++) {
			/* .drive_loop_i */
			if (fs_call(ebx, 0)) break;
			const char *name = (char *)driveinfo + 32 + 40;
			if (strlen(tmpname) + 1 + strlen(name) >= sizeof(edx->text)) continue;
			drive_item *n = xpgrealloc(edx, (ecx + 1) * sizeof(drive_item));
			if (!n) {
				pgfree(edx);
				ebx->first = 0;
				return;
			}
			edx = n;
			strcpy(edx[ecx].text, tmpname);
			strcat(edx[ecx].text, "/");
			strcat(edx[ecx].text, name);
			ecx++;
		}
		/* .drive_loop_i_done */
		ebx->first = first;
	}
	/* .drive_loop_e_done */
	ebx->first = 0;
	if (!ecx) return;
	for (int i = 0; i < ecx; i++) {
		edx[i].next = i + 1 < ecx ? (listitem *)&edx[i + 1] : 0;
		edx[i].prev = i ? (listitem *)&edx[i - 1] : 0;
	}
	/* the current drive: its name begins the panel's folder */
	const char *dir = ebp->dir;
	drive_item *cur = edx;
	for (int i = 0; i < ecx; i++) {
		const char *s = edx[i].text, *t = dir;
		for (; *s; s++, t++)
			if (match_symbol(*s, *t)) break;
		if (!*s) {
			cur = &edx[i];          /* .cur_drive_found */
			break;
		}
	}
	int h = ebp->height - 2;
	if (h < 0) h += 2;
	int r = menu_centered_in(ebp->left, ebp->top, ebp->width, h, (listitem *)cur, aDrive, 1);
	if (r == -1) {
		pgfree(edx);
		return;
	}
	close_plugin_panels(ebp);
	memcpy(prev_dir, ebp->dir, 1024);
	strcpy(ebp->dir, ((drive_item *)r)->text);
	pgfree(edx);
	read_folder(ebp);
	panels_OnKey_done_redraw(ebp);
}

/* F6, Shift+F6 (an addition, see above): F5's dialog moves */
static u8 bMove;
static const char aMoveCaption[] = "Rename/Move";
static const char aMove1[] = "Rename or move ";
static const char aMove2[] = " to:";
static const char aRename[] = "[ Rename ]";
#define aRenameLength 10

/* the volume of a full name: its length ("/rd/1", "/tmp0/1"; /sys is
 * /rd/1) and its name in canon */
static int volume_of(const char *p, char *canon, int size)
{
	if (!strncmp(p, "/sys", 4) && (!p[4] || p[4] == '/')) {
		snprintf(canon, size, "/rd/1");
		return 4;
	}
	int n = 0, slashes = 0;
	for (; p[n]; n++)
		if (p[n] == '/' && ++slashes == 3) break;
	snprintf(canon, size, "%.*s", n, p);
	return n;
}

/* the entry e of the panel to CopyDestEditBuf.data (into it if
 * bDestIsFolder): 70.10 within the volume, else copy_file and, when the
 * copy is there, delete_file; true: cancel */
static bool move_file(PanelData *ebp, u8 *e)
{
	char from[1024], to[1024], vf[64], vt[64];
	const char *name = bdfe_name(e);
	if (snprintf(from, sizeof(from), "%s/%s", ebp->dir, name) >= (int)sizeof(from) ||
	    snprintf(to, sizeof(to), bDestIsFolder ? "%s/%s" : "%s", CopyDestEditBuf.data, name) >= (int)sizeof(to)) {
		panels_OnKey_bigfilename();
		return false;
	}
	volume_of(from, vf, sizeof(vf));
	int pt = volume_of(to, vt, sizeof(vt));
	if (!strcmpi(vf, vt) && to[pt]) {
		fileinfo fi = { 10, 0, 0, 0, to + pt, 0, from };
		for (;;) {
			u32 err = fs_call(&fi, 0);
			if (!err) return false;
			const char *strings[3] = { get_error_msg(err), aCannotWriteFile, to };
			int r = SayErr(3, strings, 2, RetryOrCancelBtn);
			if (r) return true;     /* Cancel, Esc */
		}
	}
	/* between volumes: a copy, then the source deleted if the copy is there */
	if (copy_file(ebp, e)) return true;
	fileinfo ai = { 5, 0, 0, 0, attrinfo_attr, 0, to };
	if (fs_call(&ai, 0)) return false;
	del_bSkipAll = 0;
	return delete_file(ebp, e);
}

/* kfar.asm: panels_OnKey.f5_common: the copy's dialog, then the copy;
 * CopyDestEditBuf.data has the proposed destination up to edi */
static void panels_OnKey_f5_common(PanelData *ebp, char *edi)
{
	source_hModule = ebp->hPlugin;
	source_hPlugin = ebp->hFile;
	default_attr = ebp == &panel1 ? left_dotdot_entry : right_dotdot_entry;
	*edi = 0;
	CopyDestEditBuf.pos = edi - CopyDestEditBuf.data;
	char *t = CopySourceTextBuf;
	const char *a1 = bMove ? aMove1 : aCopy1;
	t = strcpy(t, a1) + strlen(a1);
	u32 eax = ebp->selected_num;
	if (eax) {
		t += sprintf(t, "%u item", eax);
		if (eax != 1) *t++ = 's';
	} else {
		/* .f5_noselected1 (the move's text has its quotes here only) */
		*t++ = '"';
		const char *esi = bdfe_name(get_curfile_folder_entry(ebp));
		int len = strlen(esi);
		int edx = cur_width - 50;
		if (len > edx) {
			esi += len - edx;
			*t++ = '.';
			*t++ = '.';
			*t++ = '.';
		}
		while (*esi) *t++ = *esi++;
		*t++ = '"';
		if (bMove) *t = 0;
	}
	/* .f5_selected1 */
	strcpy(t, bMove ? aMove2 : aCopy2);
	struct copy_dlgdata *ebx = &copy_dlgdata;
	ebx->h.dialog.title = bMove ? aMoveCaption : aCopyCaption;
	ebx->copy.data = (void *)(bMove ? aRename : aCopy);
	int btnlen = bMove ? aRenameLength : aCopyLength;
	int w = cur_width - 12;
	ebx->h.dialog.width = w;
	w -= 2;
	ebx->lcopy.x2 = w;
	ebx->ename.x2 = w;
	w = (w >> 1) - 2;
	ebx->copy.x2 = w;
	w -= btnlen - 1;
	ebx->copy.x1 = w;
	w += btnlen + 3;
	ebx->cnl.x1 = w;
	w += aCancelBLength - 1;
	ebx->cnl.x2 = w;
	ebx->ename.flags = 0xC;
	ebx->copy.flags &= ~4;
	ebx->cnl.flags &= ~4;
	if (DialogBox(ebx) != (int)&copy_dlgdata.copy) return;
	/* copy */
	char *esi = CopyDestEditBuf.data;
	int n = strlen(esi);
	if (!n) return;
	edi = esi + n - 1;
	bEndSlash = 0;
	if (*edi == '/' && edi != esi) {
		*edi-- = 0;
		bEndSlash = 1;
	}
	/* a path that does not begin with a slash is relative to the folder */
	if (*esi != '/') {
		int ecx = strlen(ebp->dir) + 1;
		if (edi + ecx >= CopyDestEditBuf.data + 513) {
			panels_OnKey_bigfilename();
			return;
		}
		memmove(esi + ecx, esi, edi + 2 - esi);
		memcpy(esi, ebp->dir, ecx);
		esi[ecx - 1] = '/';
	}
	/* .copy_absolute_path: first make the folders above that do not
	 * exist yet; the last item can be a file or a folder: a folder if one
	 * of the sources at least is a folder */
	int edx = 0;
	if (ebp->selected_num) {
		for (int i = 0; i < ebp->numfiles; i++) {
			u8 *e = ebp->files[i];
			if ((bdfe_sel(e) & 1) && (bdfe_attr(e) & 0x10)) edx++;
		}
	} else {
		edx = (bdfe_attr(get_curfile_folder_entry(ebp)) & 0x10) != 0;
	}
	/* .f5_selected2 */
	int eax2 = 0;
	char *last = 0;
	for (edi = esi; *edi; edi++)
		if (*edi == '/') {
			eax2++;
			last = edi;
		}
	/* /sys is /rd/1: two levels (the original counts the slashes of
	 * "/file-system/partition/...", and /sys/name would be a partition) */
	if (!strncmp(esi, "/sys", 4) && (!esi[4] || esi[4] == '/')) eax2++;
	bNeedRestoreName = 0;
	u8 dl = 1;
	/* "/file-system/partition" folders already exist */
	if (eax2 > 2) {
		/* the destination is a folder if edx != 0 or bEndSlash != 0 */
		if (!edx && !bEndSlash) {
			edi = last;
			eax2--;
		}
		attrinfo.name = esi;
		int ecx = eax2;
		/* the last folder that exists already */
		for (;;) {
			/* .createuploop1 */
			if (eax2 <= 2) break;
			char c = *edi;
			*edi = 0;
			u32 err = fs_call(&attrinfo, 0);
			*edi = c;
			if (!err) {
				if (attrinfo_attr[0] & 0x10) break;
				/* the item required to be a folder, but is file */
				const char *strings[2] = { esi, aNotFolder };
				SayErr(2, strings, 1, ContinueBtn);
				return;
			}
			/* .createupcont1 */
			eax2--;
			do edi--; while (*edi != '/');
		}
		/* .createupdone1: make the folders we need */
		while (eax2 < ecx) {
			do edi++; while (*edi && *edi != '/');
			char c = *edi;
			*edi = 0;
			int r = makedir(2, RetryOrCancelBtn);
			*edi = c;
			bNeedRestoreName = 1;
			if (r) return;
			eax2++;
		}
		/* .createupdone */
		if (*edi) eax2++;
		if (eax2 > 2) {
			attrinfo_attr[0] = 0;   /* assume zero attributes if error */
			fs_call(&attrinfo, 0);
			dl = (attrinfo_attr[0] & 0x10) != 0;
		}
	}
	/* .docopy */
	u8 *e = get_curfile_folder_entry(ebp);
	if (bNeedRestoreName && !bEndSlash && !ebp->selected_num && (bdfe_attr(e) & 0x10)) dl = 0;
	/* start the copy itself */
	bDestIsFolder = dl;
	copy_bSkipAll = 0;
	copy_bSkipAll2 = 0;
	copy_bSkipAll3 = 0;
	/* (plugin_flags & 2: a plugin's GetFiles) */
	if (!ebp->selected_num) {
		if (bMove) move_file(ebp, e);
		else copy_file(ebp, e);
	} else {
		/* .f5_selected3 */
		for (int i = 0; i < ebp->numfiles; i++) {
			u8 *f = ebp->files[i];
			if (!(bdfe_sel(f) & 1)) continue;
			bDestIsFolder = dl;
			if (bMove ? move_file(ebp, f) : copy_file(ebp, f)) break;   /* .f5_multiple_cancel */
			bdfe_sel(f) &= ~1;
			draw_panel(ebp);
			if (bNeedRestoreName) delete_last_name_from_end(CopyDestEditBuf.data);
		}
	}
	panels_OnKey_copydone(ebp);
}

/* kfar.asm: panels_OnKey.copydone: both panels read again */
static void panels_OnKey_copydone(PanelData *ebp)
{
	panels_OnKey_ctrl_r(ebp, 0);
	panels_OnKey_ctrl_r(other_panel(ebp), 0);
}

/* kfar.asm: panels_OnKey.f5_2: the destination: esi's folder and '/' */
static void panels_OnKey_f5_2(PanelData *ebp, const char *esi)
{
	CopyDestEditBuf.maxlength = CopyDestEditBuf_length;
	CopyDestEditBuf.start = 0;
	char *edi = CopyDestEditBuf.data;
	for (; *esi; esi++) {
		if (edi >= CopyDestEditBuf.data + 511) {
			panels_OnKey_l2(aFolderNameTooBig);
			return;
		}
		*edi++ = *esi;
	}
	*edi++ = '/';
	panels_OnKey_f5_common(ebp, edi);
}

/* kfar.asm: panels_OnKey.f5: copy to the other panel */
void panels_OnKey_f5(void *p, u32 eax)
{
	PanelData *ebp = p;
	bMove = 0;
	/* todo: copy to plugin panel */
	if (other_panel(ebp)->hPlugin) return;
	panels_OnKey_f5_2(ebp, other_panel(ebp)->dir);
}

/* kfar.asm: panels_OnKey.shift_f5: copy in this panel (another name) */
void panels_OnKey_shift_f5(void *p, u32 eax)
{
	PanelData *ebp = p;
	bMove = 0;
	/* todo: copy to plugin panel */
	if (ebp->hPlugin) return;
	if (ebp->selected_num) {
		panels_OnKey_f5_2(ebp, ebp->dir);
		return;
	}
	const char *esi = bdfe_name(get_curfile_folder_entry(ebp));
	CopyDestEditBuf.maxlength = CopyDestEditBuf_length;
	CopyDestEditBuf.start = 0;
	char *edi = CopyDestEditBuf.data;
	while (*esi) *edi++ = *esi++;
	panels_OnKey_f5_common(ebp, edi);
}

/* F6: rename or move to the other panel (an addition, see above) */
void panels_OnKey_f6(void *p, u32 eax)
{
	PanelData *ebp = p;
	if (other_panel(ebp)->hPlugin || ebp->hPlugin) return;
	bMove = 1;
	panels_OnKey_f5_2(ebp, other_panel(ebp)->dir);
}

/* Shift+F6: rename in this panel (an addition) */
void panels_OnKey_shift_f6(void *p, u32 eax)
{
	PanelData *ebp = p;
	if (ebp->hPlugin) return;
	bMove = 1;
	if (ebp->selected_num) {
		panels_OnKey_f5_2(ebp, ebp->dir);
		return;
	}
	const char *esi = bdfe_name(get_curfile_folder_entry(ebp));
	CopyDestEditBuf.maxlength = CopyDestEditBuf_length;
	CopyDestEditBuf.start = 0;
	char *edi = CopyDestEditBuf.data;
	while (*esi) *edi++ = *esi++;
	panels_OnKey_f5_common(ebp, edi);
}

/* kfar.asm: panels_OnKey.f3: view */
void panels_OnKey_f3(void *p, u32 eax)
{
	PanelData *ebp = p;
	if (!ebp->numfiles) return;
	u8 *ecx = ebp->files[ebp->index];
	if (bdfe_attr(ecx) & 0x10) return;
	view_file(0, ebp->dir, ecx);
}

/* kfar.asm: panels_OnKey.f4: edit */
void panels_OnKey_f4(void *p, u32 eax)
{
	PanelData *ebp = p;
	if (!ebp->numfiles) return;
	edit_file(ebp, ebp->index);
}

/* kfar.asm: panels_OnKey.f8: delete */
void panels_OnKey_f8(void *p, u32 eax)
{
	PanelData *ebp = p;
	/* todo: delete files from plugin panel */
	if (ebp->hPlugin) return;
	if (ebp->selected_num) {
		/* .f8_has_selected */
		char *edi = saved_file_name + 511;
		*edi = 0;
		u32 n = ebp->selected_num;
		if (n != 1) *--edi = 's';
		edi -= 5;
		memcpy(edi, " item", 5);
		do *--edi = '0' + n % 10; while ((n /= 10));
		const char *strings[2] = { aConfirmDeleteText, edi };
		if (Message(aDeleteCaption, 2, strings, 2, DeleteOrCancelBtn)) return;
		del_bSkipAll = 0;
		for (int i = 0; i < ebp->numfiles; i++) {
			u8 *f = ebp->files[i];
			if ((bdfe_sel(f) & 1) && delete_file(ebp, f)) break;   /* .f8_multiple_cancel */
		}
		panels_OnKey_copydone(ebp);
		return;
	}
	if (!ebp->numfiles) return;
	u8 *ecx = get_curfile_folder_entry(ebp);
	if (bdfe_is_dotdot(ecx)) return;        /* .f8_not_allowed */
	if (bConfirmDelete) {
		strcpy(aConfirmDeleteTextBuf, aConfirmDeleteText);
		strcat(aConfirmDeleteTextBuf, bdfe_attr(ecx) & 0x10 ? aDeleteFolder : aDeleteFile);
		const char *strings[2] = { aConfirmDeleteTextBuf, bdfe_name(ecx) };
		if (Message(aDeleteCaption, 2, strings, 2, DeleteOrCancelBtn)) return;
	}
	/* .f8_allowed */
	del_bSkipAll = 0;
	delete_file(ebp, ecx);
	panels_OnKey_copydone(ebp);
}

/* kfar.asm: panels_OnKey.ctrl_r.doread: read the folder again, the
 * cursor on saved_file_name if it is still there, else at the index */
void panels_OnKey_ctrl_r_doread(PanelData *ebp, int index)
{
	int start = ebp->start;
	read_folder(ebp);
	ebp->start = start;
	ebp->index = index;
	for (int eax = 0; eax < ebp->numfiles; eax++)
		if (!strcmpi(bdfe_name(ebp->files[eax]), saved_file_name)) {
			ebp->index = eax;       /* .ctrl_r.found */
			break;
		}
	/* .ctrl_r.notfound */
	int eax = ebp->numfiles - 1;
	if ((u32)ebp->index > (u32)eax) ebp->index = eax;
	eax = ebp->index;
	if ((u32)ebp->start > (u32)eax) ebp->start = eax;
	eax = eax + 1 - ebp->colst;
	if (eax < 0) eax = 0;
	if ((u32)ebp->start < (u32)eax) ebp->start = eax;
	eax = ebp->numfiles - ebp->colst;
	if (eax > 0 && (u32)ebp->start > (u32)eax) ebp->start = eax;
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_OnKey.ctrl_r: rescan panel */
void panels_OnKey_ctrl_r(void *p, u32 eax)
{
	PanelData *ebp = p;
	int index = ebp->index;
	strcpy(saved_file_name, ebp->numfiles ? get_curfile_name(ebp) : "");
	panels_OnKey_ctrl_r_doread(ebp, index);
}

/* kfar.asm: panels_OnKey.menu: the context menu of the file (the Menu
 * key): the items of its association after ';' from [Menu] */
static void panels_OnKey_menu(void *p, u32 eax)
{
	PanelData *ebp = p;
	/* todo: context menu for plugin panel */
	if (ebp->hPlugin || !ebp->numfiles) return;
	/* ignore folders */
	u8 *e = get_curfile_folder_entry(ebp);
	if (bdfe_attr(e) & 0x10) return;
	char *edi = find_extension(e);
	if (!edi) return;
	/* known extension */
	edi = strchr(edi, ';');
	if (!edi) return;
	edi++;
	while (*edi == ' ' || *edi == 9) edi++;
	if (!*edi) return;
	const char *esi = bdfe_name(e);
	int ecx = strlen(esi) + 1;              /* the name's length + 1 = the title's + 1 */
	if (ecx >= 15) ecx = 15;
	listitem *first = 0, *edx = 0;
	for (;;) {
		/* .menucreateloop: a key of [Menu] up to ',' */
		char *k = edi;
		while (*edi && *edi != ',') edi++;
		char bl = *edi;
		*edi = 0;
		ini_get_str(app_path, aMenu, k, saved_file_name, 1024, nullstr);
		*edi = bl;
		char *comma = strchr(saved_file_name, ',');
		if (comma) {
			*comma = 0;
			/* the title padded to ecx-1, then the program */
			int tl = strlen(saved_file_name);
			int w = tl + 1 > ecx ? tl + 1 : ecx;
			listitem *it = pgalloc(sizeof(listitem) + w + strlen(comma + 1) + 1);
			if (!it) break;
			memset(it->text, ' ', w - 1);
			memcpy(it->text, saved_file_name, tl);
			it->text[w - 1] = 0;
			strcpy(it->text + w, comma + 1);
			if (edx) {
				edx->next = it;
				it->prev = edx;
			} else {
				first = it;
			}
			edx = it;
		}
		/* .menucreatecont */
		if (*edi++ != ',') break;
	}
	/* .menucreated */
	if (first) {
		int r = menu(first, esi, 1);
		if (r != -1) {
			char *prog = ((listitem *)r)->text;
			prog += strlen(prog) + 1;
			ebp = active_panel;
			panels_OnKey_run_association2(ebp, get_curfile_folder_entry(ebp), prog);
		}
	}
	/* .menuret_free */
	while (first) {
		listitem *n = first->next;
		pgfree(first);
		first = n;
	}
}

/* kfar.asm: panels_OnKey.f7: make a folder */
void panels_OnKey_f7(void *p, u32 eax)
{
	PanelData *ebp = p;
	CopyDestEditBuf.maxlength = CopyDestEditBuf_length;
	CopyDestEditBuf.pos = 0;
	CopyDestEditBuf.start = 0;
	CopyDestEditBuf.data[0] = 0;
	MkDir_Dlgdata *ebx = &mkdir_dlgdata;
	int w = cur_width - 12;
	ebx->h.dialog.width = w;
	w -= 2;
	ebx->dnam.x2 = w;
	w = (w >> 1) - 2;
	ebx->cont.x2 = w;
	w -= a_ContinueLength - 1;
	ebx->cont.x1 = w;
	w += a_ContinueLength + 3;
	ebx->cnl.x1 = w;
	w += aCancelBLength - 1;
	ebx->cnl.x2 = w;
	ebx->dnam.flags = 0xC;
	ebx->cont.flags &= ~4;
	ebx->cnl.flags &= ~4;
	if (DialogBox(ebx) != (int)&mkdir_dlgdata.cont) return;
	char *esi = CopyDestEditBuf.data;
	if (!*esi) return;
	if (*esi != '/') {
		/* relative to the panel's folder */
		int ecx = strlen(ebp->dir) + 1;
		char *edi = esi + strlen(esi) - 1;
		if (edi + ecx >= CopyDestEditBuf.data + 513) {
			panels_OnKey_bigfilename();
			return;
		}
		memmove(esi + ecx, esi, edi + 2 - esi);
		memcpy(esi, ebp->dir, ecx);
		esi[ecx - 1] = '/';
	}
	/* .mkdir_absolute_path */
	makedir(2, RetryOrCancelBtn);
	panels_OnKey_copydone(ebp);
}

/* kfar.asm: panels_OnKey.change_mode: LCtrl+1..4, the view modes */
static void panels_OnKey_change_mode(void *p, u32 eax)
{
	PanelData *ebp = p;
	ebp->colmode = (u8)eax - 1;
	calc_colwidths(ebp);
	draw_panel(ebp);
}

/* kfar.asm: panels_OnKey.quick_jump: RCtrl+0..9, [FolderShortcuts] */
static void panels_OnKey_quick_jump(void *p, u32 eax)
{
	PanelData *ebp = p;
	int i = (u8)eax - 1;
	if (i == 10) i = 0;
	char *s = FolderShortcuts[i];
	if (!s) return;
	close_plugin_panels(ebp);
	strcpy(prev_dir, ebp->dir);
	strcpy(ebp->dir, s);
	panels_OnKey_reread(ebp);
}

/* kfar.asm: panels_OnKey.greyplus, .greyminus: (de)select by a mask */
static void panels_OnKey_select_mask(PanelData *ebp, int deselect, const char *ecx)
{
	mark_dlgdata.h.dialog.title = ecx;
	mark_dlgdata.edit.flags = 0xC;
	char *edi = enter_string_buf.data;
	enter_string_buf.maxlength = 512;
	enter_string_buf.pos = 1;
	enter_string_buf.start = 0;
	edi[0] = '*';
	edi[1] = 0;
	if (DialogBox(&mark_dlgdata) == -1) return;
	for (char *s = edi; *s; s++) *s = tolower_table[(u8)*s];
	int n = ebp->numfiles;
	u8 **ebx = ebp->files;
	if (!n) return;
	if (bdfe_is_dotdot(*ebx)) {
		if (!--n) return;
		ebx++;
	}
	/* .selectloop */
	for (; n; n--, ebx++) {
		const char *esi = bdfe_name(*ebx);
		char *edx = lower_file_name;
		while ((*edx = tolower_table[(u8)*esi++])) edx++;
		if (!match_mask_rev_lowercase(lower_file_name, edx, edi)) continue;
		u8 *f = *ebx;
		if (deselect) {
			if (bdfe_sel(f) & 1) {
				bdfe_sel(f) &= ~1;
				ebp->selected_size -= bdfe_size(f);
				ebp->selected_num--;
			}
		} else if (!(bdfe_sel(f) & 1)) {
			/* .doselect */
			bdfe_sel(f) |= 1;
			ebp->selected_size += bdfe_size(f);
			ebp->selected_num++;
		}
	}
	panels_OnKey_done_redraw(ebp);
}

static void panels_OnKey_greyplus(void *p, u32 eax)
{
	panels_OnKey_select_mask(p, 0, aSelect);
}

static void panels_OnKey_greyminus(void *p, u32 eax)
{
	panels_OnKey_select_mask(p, 1, aDeselect);
}

/* kfar.asm: panels_OnKey.greyasterisk: invert the selection */
static void panels_OnKey_greyasterisk(void *p, u32 eax)
{
	PanelData *ebp = p;
	for (int i = 0; i < ebp->numfiles; i++) {
		u8 *f = ebp->files[i];
		if (bdfe_is_dotdot(f)) continue;
		bdfe_sel(f) ^= 1;
		if (bdfe_sel(f) & 1) {
			ebp->selected_size += bdfe_size(f);
			ebp->selected_num++;
		} else {
			/* .gadel */
			ebp->selected_size -= bdfe_size(f);
			ebp->selected_num--;
		}
	}
	panels_OnKey_done_redraw(ebp);
}

/* kfar.asm: panels_ctrlkeys: the keys of the panels (see kfar.h's
 * ctrlkey: the modifiers 4 bits each for Shift, Ctrl, Alt) */
#define K(sc, cs, h) { sc, 0, cs, h }
const ctrlkey panels_ctrlkeys[] = {
	K(0x48, 0, panels_OnKey_up),
	K(0x50, 0, panels_OnKey_down),
	K(0x4B, 0, panels_OnKey_left),
	K(0x4D, 0, panels_OnKey_right),
	K(0x51, 0, panels_OnKey_pgdn),
	K(0x49, 0, panels_OnKey_pgup),
	K(0xF, 0, panels_OnKey_tab),
	K(0x47, 0, panels_OnKey_home),
	K(0x4F, 0, panels_OnKey_end),
	K(0x1C, 0, panels_OnKey_enter),
	K(0x3D, 0, panels_OnKey_f3),
	K(0x3E, 0, panels_OnKey_f4),
	K(0x3F, 0, panels_OnKey_f5),
	K(0x3F, 1, panels_OnKey_shift_f5),
	K(0x40, 0, panels_OnKey_f6),            /* (an addition) */
	K(0x40, 1, panels_OnKey_shift_f6),
	K(0x41, 0, panels_OnKey_f7),
	K(0x41, 0x100, panels_OnKey_alt_f7),
	K(0x42, 0, panels_OnKey_f8),
	K(0x43, 0x100, alt_f9),
	K(0x44, 0, exit_handler),
	K(0x5D, 0, panels_OnKey_menu),
	K(0x3D, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x3E, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x3F, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x40, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x41, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x42, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x43, 0x10, panels_OnKey_ctrl_f3_9),
	K(0x3B, 0x100, panels_OnKey_alt_f1_2),
	K(0x3C, 0x100, panels_OnKey_alt_f1_2),
	K(0x58, 0, F12),
	K(0x13, 0x10, panels_OnKey_ctrl_r),
	K(2, 0x30, panels_OnKey_change_mode),
	K(3, 0x30, panels_OnKey_change_mode),
	K(4, 0x30, panels_OnKey_change_mode),
	K(5, 0x30, panels_OnKey_change_mode),
	K(2, 0x40, panels_OnKey_quick_jump),
	K(3, 0x40, panels_OnKey_quick_jump),
	K(4, 0x40, panels_OnKey_quick_jump),
	K(5, 0x40, panels_OnKey_quick_jump),
	K(6, 0x40, panels_OnKey_quick_jump),
	K(7, 0x40, panels_OnKey_quick_jump),
	K(8, 0x40, panels_OnKey_quick_jump),
	K(9, 0x40, panels_OnKey_quick_jump),
	K(10, 0x40, panels_OnKey_quick_jump),
	K(11, 0x40, panels_OnKey_quick_jump),
	K(0x52, 0, panels_OnKey_insert),
	K(0x4E, 0, panels_OnKey_greyplus),
	K(0x4A, 0, panels_OnKey_greyminus),
	K(0x37, 0, panels_OnKey_greyasterisk),
	{ 0 },
};

/* kfar.asm: panels_OnKey */
void panels_OnKey(void *unused, u32 eax)
{
	/* restore screen hidden by quick search box */
	if (bDisplayQuickSearch) {
		bDisplayQuickSearch = 0;
		HideDialogBox(&QuickSearchDlg, 0);
	}
	PanelData *ebp = active_panel;
	if (process_ctrl_keys(panels_ctrlkeys, eax, ebp)) {
		bQuickSearchMode = 0;
		return;
	}
	/* .nostdkey */
	u8 al = eax;
	if (bQuickSearchMode) {
		u32 ecx;
		if (al & 0x80) goto keepqsmode;
		if (al >= 0x40) goto leaveqsmode;
		if (al == 0x0E) {
			/* .qsbackspace */
			ecx = quick_search_buf.pos;
			if (ecx) {
				quick_search_buf.data[ecx - 1] = 0;
				quick_search_buf.pos--;
			}
			goto keepqsmode;
		}
		if (al == 0x1C) {
			/* .test_enter: (Shift+)Ctrl+Enter, the next/previous */
			if (!(ctrlstate & 0x0C)) goto keepqsmode;
			if (ctrlstate & 3) quick_find_prev(ebp);
			else quick_find_next(ebp);
			goto keepqsmode;
		}
		if (!scan2ascii[al]) goto leaveqsmode;
		u8 c = get_ascii_char(al);
		ecx = quick_search_buf.pos;
		if (ecx >= quicksearch_maxlen) goto keepqsmode;
		quick_search_buf.data[ecx] = c;
		quick_search_buf.data[ecx + 1] = 0;
		if (!quick_find(ebp)) {
			quick_search_buf.data[ecx] = 0;
			goto keepqsmode;
		}
		quick_search_buf.pos++;
		quick_search_buf.start = ecx >= 16 ? ecx - 16 : 0;
		goto keepqsmode;
	leaveqsmode:
		bQuickSearchMode = 0;
		draw_image();
		return;
	keepqsmode:
		ShowDialogBox(&QuickSearchDlg);
		bDisplayQuickSearch = 1;
		return;
	}
	/* .noqsmode: handle Alt+<key> combinations for quick search */
	if (!(ctrlstate & 0x30) || (ctrlstate & 0x0C) || al >= 0x40 || !scan2ascii[al]) return;
	u8 c = get_ascii_char(al);
	quick_search_buf.maxlength = quicksearch_maxlen - 1;
	quick_search_buf.pos = 1;
	quick_search_buf.start = 0;
	quick_search_buf.data[0] = c;
	quick_search_buf.data[1] = 0;
	if (!quick_find(ebp)) {
		quick_search_buf.data[0] = 0;
		quick_search_buf.pos = 0;
	}
	int x = ebp->left + 10, y = ebp->top + ebp->height;
	if (x >= cur_width - 21) x = cur_width - 21;
	if (y >= cur_height - 2) y = cur_height - 2;
	QuickSearchDlg.h.dialog.x = x;
	QuickSearchDlg.h.dialog.y = y;
	ShowDialogBox(&QuickSearchDlg);
	bQuickSearchMode = 1;
	bDisplayQuickSearch = 1;
}

/* kfar.asm: close_plugin_panel: close the plugin and restore the old
 * folder (no plugins here: the panel is never a plugin's) */
void close_plugin_panel(PanelData *ebp)
{
	ebp->hPlugin = 0;
	ebp->hFile = 0;
	ebp->plugin_flags = 0;
}

/* kfar.asm: close_plugin_panels */
void close_plugin_panels(PanelData *ebp)
{
	while (ebp->hPlugin) close_plugin_panel(ebp);
}

/* kfar.asm: close_handle_if_unused: edx = hPlugin, ebx = hFile (a
 * plugin's ClosePlugin when no screen uses them: no plugins here) */
void close_handle_if_unused(void *hPlugin, void *hFile)
{
	for (int ecx = 0; ecx < num_screens; ecx++)
		if (screens[ecx].vtable->IsHandleUsed(screens[ecx].data, hPlugin, hFile)) return;
}

/* kfar.asm: panels_IsHandleUsed */
bool panels_IsHandleUsed(void *unused, void *hPlugin, void *hFile)
{
	return (hPlugin == panel1.hPlugin && hFile == panel1.hFile) ||
	       (hPlugin == panel2.hPlugin && hFile == panel2.hFile);
}

/* kfar.asm: panels_OnExit: close plugin panels */
void panels_OnExit(void *unused)
{
	close_plugin_panels(&panel1);
	close_plugin_panels(&panel2);
}

/* kfar.asm: panels_OnRedraw */
void panels_OnRedraw(void *unused)
{
	cursor_x = -1;
	cursor_y = -1;
	draw_cmdbar();
	calc_colwidths(&panel1);
	draw_panel(&panel1);
	calc_colwidths(&panel2);
	draw_panel(&panel2);
}

/* kfar.asm: get_curfile_folder_entry */
u8 *get_curfile_folder_entry(PanelData *ebp)
{
	return ebp->files[ebp->index];
}

/* kfar.asm: get_curfile_name */
char *get_curfile_name(PanelData *ebp)
{
	return bdfe_name(get_curfile_folder_entry(ebp));
}

/* kfar.asm: quick_find: the next name (from the current) beginning with
 * the quick search's string; false (CF) if none */
static bool quick_find_from(PanelData *ebp, int ecx, bool skip_current)
{
	if (!ebp->numfiles) return false;
	for (;;) {
		if (!skip_current) {
			/* .scanloop */
			const char *edi = bdfe_name(ebp->files[ecx]);
			const char *esi = quick_search_buf.data;
			for (; *esi; esi++, edi++)
				if (match_symbol(*esi, *edi)) break;
			if (!*esi) break;               /* .ok */
		}
		skip_current = false;
		/* .no */
		if (++ecx >= ebp->numfiles) ecx = 0;
		if (ecx == ebp->index) return false;    /* .nof */
	}
	/* .ok */
	if (ecx == ebp->index) return true;
	ebp->index = ecx;
	ecx -= ebp->height >> 1;
	if (ecx < 0) ecx = 0;
	int eax = ebp->numfiles - ebp->colst;
	if (eax < 0) eax = ecx = 0;
	if (ecx >= eax) ecx = eax;
	ebp->start = ecx;
	draw_panel(ebp);
	return true;
}

static bool quick_find(PanelData *ebp)
{
	return quick_find_from(ebp, ebp->index, false);
}

/* kfar.asm: quick_find_next */
static bool quick_find_next(PanelData *ebp)
{
	return quick_find_from(ebp, ebp->index, true);
}

/* kfar.asm: quick_find_prev */
static bool quick_find_prev(PanelData *ebp)
{
	if (!ebp->numfiles) return false;
	int ecx = ebp->index;
	for (;;) {
		/* .scanloop */
		if (--ecx < 0) ecx = ebp->numfiles - 1;
		if (ecx == ebp->index) return false;
		const char *edi = bdfe_name(ebp->files[ecx]);
		const char *esi = quick_search_buf.data;
		for (; *esi; esi++, edi++)
			if (match_symbol(*esi, *edi)) break;
		if (!*esi) break;
	}
	/* quick_find.ok */
	int idx = ecx;
	ebp->index = idx;
	ecx -= ebp->height >> 1;
	if (ecx < 0) ecx = 0;
	int eax = ebp->numfiles - ebp->colst;
	if (eax < 0) eax = ecx = 0;
	if (ecx >= eax) ecx = eax;
	ebp->start = ecx;
	draw_panel(ebp);
	return true;
}

/* kfar.asm: panels_getname: the F12 menu's "Panels" and the active
 * panel's folder (its first three parts, then the end of the rest) and
 * file */
void panels_getname(void *unused, char *edi, int ecx)
{
	memcpy(edi, "Panels       ", 13);
	edi += 13;
	ecx -= 13;
	PanelData *ebp = active_panel;
	/* .native: the folder up to its third slash */
	const char *esi = ebp->dir;
	int edx = 3;
	for (;;) {
		char al = *esi++;
		*edi++ = al;
		ecx--;
		if (!al) {
			esi--;
			edi--;
			break;
		}
		if (al == '/' && !--edx) break;
	}
	/* .main: the rest of the folder, '/', the file's name: its end */
	char t[1024 + 264];
	if (ebp->numfiles)
		snprintf(t, sizeof(t), "%s/%s", esi, get_curfile_name(ebp));
	else
		snprintf(t, sizeof(t), "%s/", esi);
	int ebx = strlen(t);
	const char *s = t;
	if (ebx > ecx) {
		memcpy(edi, "...", 3);
		edi += 3;
		ecx -= 3;
		if (ecx < 0) ecx = 0;
		s = t + ebx - ecx;
	}
	strcpy(edi, s);
}

/* kfar.asm: find_extension: the association of the entry's extension */
char *find_extension(const u8 *ecx)
{
	const char *name = bdfe_name(ecx);
	const char *esi = strrchr(name, '.');
	/* empty extension */
	if (!esi) return 0;
	/* .found_ext */
	esi++;
	char *edi = associations;
	if (!edi) return 0;
	/* .find_loop: "key\0value\0" ... "\0" */
	while (*edi) {
		bool eq = !strcmpi(esi, edi);
		edi += strlen(edi) + 1;
		if (eq) return edi;             /* .found */
		edi += strlen(edi) + 1;
	}
	/* .notfound: unknown extension */
	return 0;
}
