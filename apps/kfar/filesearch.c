/* kfar for KolibriOS for ARM: the file search (Alt+F7): the query, the
 * search through the folders, the list of the files found with Go to,
 * View and New search.
 *
 * Translated to C from kfar's filetool.inc (filesearch_data ...
 * filesearch_test_file), diamond (Evgeny Grechnikov) and the KolibriOS
 * team, GPL-2.0.
 *
 * KolibriOS for ARM has no threads (function 51), so filesearch_thread
 * is not a second thread but a loop that goes a step at a time: its
 * state (the stack of the positions in the parent folders, which was
 * the thread's stack, the position in the current folder, the entry,
 * the automaton's state in the file being read) is kept in
 * filesearch_data, filesearch_thread_step does one step (a part of a
 * folder read, one entry, one block of a file), and filesearch_OnIdle,
 * which kfar calls when no event came within idle_interval (1 while
 * searching, 10 in the original), runs steps for 5/100 s, then does what
 * the original's OnIdle does with what the thread found (the redraw at
 * most every 10/100 s, as the original's idle_interval of 10). The
 * synchronisation becomes state: "slot" is non-zero while a search
 * exists (the thread's slot in the original), "alive" while it runs;
 * stop = 1 ends it at the next step (filesearch_stop_thread), stop = 2
 * pauses it while ConfirmCancel asks; filesearch_wait_thread ends a
 * search still alive. The search only goes on while its screen is the
 * active one (only the active screen gets OnIdle); the original's thread
 * also ran behind other screens. */
#include "kfar.h"

/* kfar.c: draw_image does nothing while this is set (the original
 * patches draw_image's first byte with "ret" while the screen below the
 * dialog redraws itself) */
extern u8 bDrawImageDisabled;
/* panels.c: kfar.asm's panels_OnKey.ctrl_r.doread: the panel read again
 * with the cursor on saved_file_name (or at index) */
void panels_OnKey_ctrl_r_doread(PanelData *ebp, int index);

#define search_dir_query_size 32
#define search_filebuf_size 32768

enum {
	skip_read_folder = 1, skip_set_folder = 2, skip_open_file = 4, skip_read_file = 8, skip_bigname = 0x10,
};
/* the steps of filesearch_thread */
enum { FS_READ_FOLDER, FS_SCAN, FS_TEST_FILE };

typedef struct filesearch_data {
	const screen_vtable *prev_screen_vtable;        /* previous in the window stack */
	void *prev_screen_data;
	int tid;                /* -1 for the query stage, the search's id for the search stage */
	int slot;               /* 0 if the search is not running (the thread's slot) */
	u8 *result_blocks;      /* head of heap blocks for resulting data */
	u32 cur_result_ptr;     /* offset of free data in the current heap block for result */
	u8 stop;                /* set to 1 when the search needs to be stopped */
	u8 skip_errors_mask;
	u8 updating;
	u8 datachanged;
	listdata list;
	listitem *list_end;
	FileSearch_QueryTemplate query_dlgdata;
	FileSearch_SearchTemplate search_dlgdata;
	EDITBUF(511) mask;
	EDITBUF(255) string;
	char caption[268];
	char statusstr[80];
	/* filesearch_thread's state (its stack and registers in the original) */
	bool alive;
	int phase;
	u8 *fsm;                /* edx: search_string_pre's automaton */
	u32 final_state;        /* ecx */
	u32 stack[1024];        /* the positions in the parent folders, -1 at the bottom */
	int sp;
	u32 pos;                /* esi: the position in the current folder */
	u32 nread;              /* ebx: the entries in dir_area */
	u32 cur;                /* the entry of dir_area being looked at */
	char *edi;              /* after curdir's zero in curfile */
	const u8 *state;        /* filesearch_test_file's edi */
	u32 last_update;        /* the tick of the last redraw */
	fileinfo fs;
	char curfile[1024];
	char curdir[1024];
	char lowername[264];
	u8 dir_area[32 + 304 * search_dir_query_size];
	u8 filebuf[search_filebuf_size];
} filesearch_data;

static void panels_OnKey_alt_f7_reinit(filesearch_data *ebp);
static void filesearch_done(filesearch_data *ebp);
static void filesearch_thread_step(filesearch_data *ebp);
static void filesearch_set_dlgflags(filesearch_data *ebp);
static void filesearch_free_result(filesearch_data *ebp);
static void filesearch_wait_thread(filesearch_data *ebp);
static void filesearch_thread_start(filesearch_data *ebp);

/* filetool.inc: panels_OnKey.alt_f7 */
void panels_OnKey_alt_f7(void *p, u32 eax)
{
	filesearch_data *ebp = new_screen(sizeof(filesearch_data), &filesearch_vtable);
	if (!ebp) return;
	ebp->prev_screen_vtable = &panels_vtable;
	ebp->query_dlgdata = filesearch_query_template;
	ebp->search_dlgdata = filesearch_search_template;
	ebp->query_dlgdata.edit2.data = &ebp->string;
	ebp->string.maxlength = 253;
	ebp->string.pos = 0;
	ebp->string.start = 0;
	ebp->string.data[0] = 0;
	ebp->query_dlgdata.edit1.data = &ebp->mask;
	ebp->mask.maxlength = 511;
	ebp->mask.start = 0;
	ebp->mask.data[0] = '*';
	ebp->mask.data[1] = 0;
	ebp->mask.pos = 1;
	ebp->query_dlgdata.case_.flags = (find_in_file_dlgdata.case_.flags & 0x10) | 8;
	ebp->query_dlgdata.whole.flags = (find_in_file_dlgdata.whole.flags & 0x10) | 8;
	ebp->search_dlgdata.h.dialog.title = ebp->caption;
	ebp->search_dlgdata.list1.data = &ebp->list;
	panels_OnKey_alt_f7_reinit(ebp);
}

/* filetool.inc: panels_OnKey.alt_f7.reinit */
static void panels_OnKey_alt_f7_reinit(filesearch_data *ebp)
{
	ebp->tid = -1;
	ebp->slot = 0;
	ebp->result_blocks = 0;
	ebp->cur_result_ptr = 0x4000;
	memset(&ebp->list, 0, sizeof(ebp->list));
	ebp->list_end = 0;
	ebp->stop = 0;
	ebp->query_dlgdata.h.dialog.main_color = dialog_main_color;
	ebp->search_dlgdata.h.dialog.main_color = dialog_main_color;
	ebp->query_dlgdata.h.dialog.border_color = dialog_border_color;
	ebp->search_dlgdata.h.dialog.border_color = dialog_border_color;
	ebp->query_dlgdata.h.dialog.header_color = dialog_header_color;
	ebp->search_dlgdata.h.dialog.header_color = dialog_header_color;
	filesearch_OnRedraw(ebp);
}

/* filetool.inc: filesearch_OnRedraw: the screen below, then the dialog
 * of the stage */
void filesearch_OnRedraw(void *p)
{
	filesearch_data *ebp = p;
	bDrawImageDisabled = 1;
	ebp->prev_screen_vtable->OnRedraw(ebp->prev_screen_data);
	bDrawImageDisabled = 0;
	DLGDATA *dlg;
	if (ebp->tid == -1) {
		FileSearch_QueryTemplate *ebx = &ebp->query_dlgdata;
		dlg = &ebx->h;
		int eax = cur_width;
		ebx->h.dialog.x = 6;
		eax -= 12;
		ebx->h.dialog.width = eax;
		eax -= 2;
		ebx->edit1.x2 = eax;
		ebx->edit2.x2 = eax;
		eax = (u32)eax >> 1;
		eax -= 2;
		ebx->search.x2 = eax;
		eax -= aSearchBLength - 1;
		ebx->search.x1 = eax;
		eax += aSearchBLength + 3;
		ebx->cnl.x1 = eax;
		eax += aCancelBLength - 1;
		ebx->cnl.x2 = eax;
		ebx->h.dialog.y = (u32)(cur_height - ebx->h.dialog.height) >> 1;
	} else {
		/* .prepare_search_dlg: width: for big screens use all screen
		 * except for one column on each side, that is, cur_width - 10
		 * (standard frame width is 4); if cur_width - 10 does not
		 * contain enough place for all buttons with two spaces between
		 * them, but cur_width - 2 does, use fixed size = place for all
		 * buttons with two spaces between them; for small screens use
		 * cur_width - 2 as maximum possible */
		enum { btnsize = 2 + aNewSearchLen + 2 + aGotoLen + 2 + aViewLen + 2 + aCancelB2Length + 2 };
		FileSearch_SearchTemplate *ebx = &ebp->search_dlgdata;
		dlg = &ebx->h;
		int eax = cur_width, edx = eax;
		eax -= 10;
		if ((u32)eax < btnsize) {
			eax += 8;
			if ((u32)eax >= btnsize) eax = btnsize;
		}
		ebx->h.dialog.width = eax;
		ebx->h.dialog.x = (u32)(edx - eax) >> 1;
		eax--;
		ebx->list1.x2 = eax;
		ebx->lbl2.x2 = eax;
		if (!ebp->slot) ebx->lbl1.x2 = eax;
		eax -= aNewSearchLen + aGotoLen + aViewLen + aCancelB2Length - 1;
		int q = eax / 5, r = eax % 5;
		edx = (r >> 1) + q;
		eax = q + 1;
		ebx->btn1.x1 = edx;
		edx += aNewSearchLen - 1;
		ebx->btn1.x2 = edx;
		edx += eax;
		ebx->btn2.x1 = edx;
		edx += aGotoLen - 1;
		ebx->btn2.x2 = edx;
		edx += eax;
		ebx->btn3.x1 = edx;
		edx += aViewLen - 1;
		ebx->btn3.x2 = edx;
		edx += eax;
		ebx->btn4.x1 = edx;
		edx += aCancelB2Length - 1;
		ebx->btn4.x2 = edx;
		eax = cur_height;
		edx = eax;
		eax -= 8;
		if ((u32)eax < 6) eax = 6;
		ebx->h.dialog.height = eax;
		ebx->h.dialog.y = (u32)(edx - eax) >> 1;
		eax--;
		ebx->btn1.y1 = ebx->btn2.y1 = ebx->btn3.y1 = ebx->btn4.y1 = eax;
		eax--;
		ebx->lin2.y1 = eax;
		eax--;
		ebx->lbl1.y1 = ebx->lbl2.y1 = eax;
		eax--;
		ebx->lin1.y1 = eax;
		eax--;
		ebx->list1.y2 = eax;
	}
	/* .dlg_prepared */
	draw_dialog_shadow(&dlg->dialog);
	DrawDialogBox(dlg);
}

/* filetool.inc: filesearch_OnActivate */
void filesearch_OnActivate(void *p)
{
	filesearch_data *ebp = p;
	void *eax = active_screen_data;
	if (eax != ebp) {
		ebp->prev_screen_data = eax;
		ebp->prev_screen_vtable = active_screen_vtable;
	}
	/* the screen below must still exist; if not, the one before this */
	int i;
	for (i = 0; i < num_screens; i++)
		if (screens[i].data == ebp->prev_screen_data) break;
	if (i == num_screens) {
		ebp->prev_screen_data = screens[active_screen - 1].data;
		ebp->prev_screen_vtable = screens[active_screen - 1].vtable;
	}
	if (ebp->slot) idle_interval = 1;
	/* continue to filesearch_OnIdle */
	filesearch_OnIdle(ebp);
}

/* filesearch_thread.cancel, .done: the end of the search */
static void filesearch_thread_end(filesearch_data *ebp)
{
	ebp->datachanged = 2;
	ebp->alive = false;
	pgfree(ebp->fsm);
	ebp->fsm = 0;
}

/* filetool.inc: filesearch_OnIdle: the search's steps (see the top),
 * then the redraw when it found something, the end when it ended */
void filesearch_OnIdle(void *p)
{
	filesearch_data *ebp = p;
	if (ebp->alive && ebp->stop != 2) {
		u32 t0 = kos_uptime();
		do filesearch_thread_step(ebp);
		while (ebp->alive && kos_uptime() - t0 < 5);
	}
	u8 al = ebp->datachanged;
	ebp->datachanged = 0;
	if (al > 1) {
		/* .done */
		filesearch_wait_thread(ebp);
		filesearch_done(ebp);
	} else if (al == 1) {
		/* .new: at most every 10/100 s, as the original's interval */
		if (kos_uptime() - ebp->last_update < 10) {
			ebp->datachanged = 1;
			return;
		}
	} else {
		return;                         /* .ret */
	}
	ebp->last_update = kos_uptime();
	filesearch_OnRedraw(ebp);
}

/* filetool.inc: filesearch_OnKey */
void filesearch_OnKey(void *p, u32 eax)
{
	filesearch_data *ebp = p;
	u8 al = eax;
	if (al == 0x58) {
		F12(ebp, eax);
		return;
	}
	if (ebp->tid != -1) {
		if ((al >= 0x47 && al <= 0x49) || (al >= 0x4F && al <= 0x51)) {
			/* the keys of the list */
			listbox_key(&ebp->search_dlgdata.list1, al);
			draw_listbox(&ebp->search_dlgdata.h, &ebp->search_dlgdata.list1);
			draw_image();
			return;
		}
		/* .notforlist */
		if (al == 0x3D) goto view;
	}
	/* .handle_generic */
	DLGDATA *ebx = ebp->tid == -1 ? &ebp->query_dlgdata.h : &ebp->search_dlgdata.h;   /* filesearch_getcurdlg */
	int r = ManagerDlgProc(ebx, 2, eax, 0);
	if (!r) return;
	if (r != -1) {
		dlgitemtemplate *item = (dlgitemtemplate *)r;
		if (item == &ebp->query_dlgdata.search) goto query;
		if (item == &ebp->search_dlgdata.btn1) goto newsearch;
		if (item == &ebp->search_dlgdata.btn2) goto go_to;
		if (item == &ebp->search_dlgdata.btn3) goto view;
	}
	/* .esc */
	if (ebp->slot) {
		ebp->stop = 2;
		ebp->stop = ConfirmCancel() == 0;
		return;
	}
do_exit:					/* .exit */
	filesearch_OnExit(ebp);
	delete_active_screen();
	return;
view: {
		listitem *esi = ebp->list.curitemptr;
		if (!esi) return;
		view_file(1, esi->text, 0);
		return;
	}
go_to: {
		listitem *it = ebp->list.curitemptr;
		if (!it) return;
		char *esi = it->text;
		if (esi[0] != '/' || !esi[1]) return;
		PanelData *panel = active_panel;
		close_plugin_panels(panel);
		char *ecx = esi;
		char *e = esi + strlen(esi);
		while (*--e != '/');
		*e = 0;
		strcpy(saved_file_name, e + 1);
		if (strcmpi(ecx, panel->dir)) {
			strcpy(panel->dir, ecx);
			panel->start = 0;
			panel->index = 0;
		}
		/* .goto.samedir */
		panels_OnKey_ctrl_r_doread(panel, panel->index);
		goto do_exit;
	}
newsearch:
	if (ebp->slot) {
		/* filesearch_stop_thread */
		ebp->stop = 1;
		filesearch_wait_thread(ebp);
	}
	filesearch_free_result(ebp);
	panels_OnKey_alt_f7_reinit(ebp);
	return;
query: {
		PanelData *esi = active_panel;
		if (esi->hPlugin) {
			const char *s = aCannotSearchOnPlugin;
			SayErr(1, &s, 1, ContinueBtn);
			goto do_exit;
		}
		strcpy(ebp->curdir, esi->dir);
		/* (the thread: here the search's steps in OnIdle) */
		ebp->tid = 1;
		ebp->updating = 0;
		ebp->datachanged = 0;
		ebp->slot = 1;
		filesearch_thread_start(ebp);
		idle_interval = 1;
		/* the caption: "Find file: <mask>" */
		char *edi = ebp->caption;       /* the search dialog's title */
		strcpy(edi, aFileSearch);
		edi += strlen(edi);
		*edi++ = ':';
		*edi++ = ' ';
		strcpy(edi, ebp->mask.data);
		/* the status: 'Searching "<string>" in:', at most 10 characters
		 * of the string, '"string"' left out if it is empty */
		edi = ebp->statusstr;
		ebp->search_dlgdata.lbl1.data = edi;
		const char *s = aSearchingIn;
		do *edi++ = *s; while (*s++ != '"');
		const char *str = ebp->string.data;
		int ecx = 10;
		bool ended = false;
		for (;;) {
			if (!*str) {
				ended = true;
				break;
			}
			*edi++ = *str++;
			if (!--ecx) break;
		}
		*edi++ = *s++;                  /* the closing '"' */
		if (!ended) {
			edi[-1] = '.';
			edi[-2] = '.';
			edi[-3] = '.';
			edi[-4] = '"';
		}
		if (edi[-2] == '"') edi -= 3;
		do *edi++ = *s; while (*s++);
		int len = edi - ebp->statusstr;
		ebp->search_dlgdata.lbl1.x2 = len;
		ebp->search_dlgdata.lbl2.x1 = len + 1;
		ebp->search_dlgdata.lbl2.data = ebp->curdir;
		filesearch_set_dlgflags(ebp);           /* use it? I think, yes */
		if (!ebp->slot) filesearch_done(ebp);
		filesearch_OnRedraw(ebp);
		return;
	}
}

/* filetool.inc: filesearch_done: "Search done. Found N file(s)" */
static void filesearch_done(filesearch_data *ebp)
{
	if (active_screen_data == ebp) idle_interval = -1;
	ebp->search_dlgdata.lbl2.data = 0;
	char *edi = ebp->search_dlgdata.lbl1.data;
	const char *esi = aSearchDone;
	while ((*edi++ = *esi++) != '?');
	edi--;
	char buf[12];
	int n = 0;
	u32 eax = ebp->list.numitems;
	do buf[n++] = '0' + eax % 10; while ((eax /= 10));
	while (n) *edi++ = buf[--n];
	while ((*edi++ = *esi++));
}

/* filetool.inc: filesearch_getname */
void filesearch_getname(void *ebp, char *edi, int ecx)
{
	memcpy(edi, "Find dialog  ", 13);
}

/* filetool.inc: filesearch_wait_thread (with filesearch_stop_thread's
 * stop = 1 before it: the search ended at once, see the top) */
static void filesearch_wait_thread(filesearch_data *ebp)
{
	if (ebp->slot && ebp->alive) filesearch_thread_end(ebp);
	/* .secondary_thread_exited */
	ebp->slot = 0;
}

/* filetool.inc: filesearch_OnExit */
void filesearch_OnExit(void *p)
{
	filesearch_data *ebp = p;
	/* filesearch_stop_thread */
	ebp->stop = 1;
	filesearch_wait_thread(ebp);
	filesearch_free_result(ebp);
}

/* filetool.inc: filesearch_IsHandleUsed: never */
bool filesearch_IsHandleUsed(void *ebp, void *hPlugin, void *hFile)
{
	return false;
}

/* filetool.inc: filesearch_set_dlgflags: the search's settings to the
 * viewer's and the editor's search (F7) */
static void filesearch_set_dlgflags(filesearch_data *ebp)
{
	find_in_file_dlgdata.case_.flags = (ebp->query_dlgdata.case_.flags & 0x10) | 8;
	find_in_file_dlgdata.whole.flags = (ebp->query_dlgdata.whole.flags & 0x10) | 8;
	SearchStringEditBuf.start = 0;
	SearchStringEditBuf.pos = 0;
	strcpy(SearchString, ebp->string.data);
}

/* filetool.inc: filesearch_free_result */
static void filesearch_free_result(filesearch_data *ebp)
{
	u8 *ecx = ebp->result_blocks;
	while (ecx) {
		u8 *next = *(u8 **)ecx;
		pgfree(ecx);
		ecx = next;
	}
}

/* filetool.inc: filesearch_thread, its start: initialize the search for
 * the string */
static void filesearch_thread_start(filesearch_data *ebp)
{
	ebp->skip_errors_mask = 0;
	ebp->fsm = 0;
	ebp->final_state = 0;
	ebp->alive = true;
	ebp->stop = 0;
	if (ebp->string.data[0]) {
		const u8 *esi = ebp->query_dlgdata.case_.flags & 0x10 ? identical_table : tolower_table;
		int whole = (ebp->query_dlgdata.whole.flags & 0x10) != 0;
		/* force cp866 */
		ebp->fsm = search_string_pre(ebp->string.data, esi, encodings_cp866, whole, &ebp->final_state);
		if (!ebp->fsm) {
			filesearch_thread_end(ebp);
			return;
		}
	}
	/* .noprepare */
	ebp->sp = 0;
	ebp->stack[ebp->sp++] = -1;
	/* .enter_recursion: start position: zero */
	ebp->pos = 0;
	ebp->phase = FS_READ_FOLDER;
}

/* filetool.inc: recursive_read_folder_err, recursive_read_file_err:
 * returns the button chosen (0 retry, 1 skip, 2 skip all, 3 cancel,
 * -1 Esc) */
static int recursive_read_err(const char *what, const char *edx, u32 eax)
{
	const char *strings[3] = { get_error_msg(eax), what, edx };
	return SayErr(3, strings, 4, DeleteErrorBtn);
}

/* filesearch_thread.file_found: the result added to the list */
static bool filesearch_file_found(filesearch_data *ebp)
{
	/* allocate memory for the new result (4-aligned here) */
	const char *ebx = ebp->curfile;
	u32 esi = (strlen(ebx) + 1 + 8 + 3) & ~3u;
	u32 eax = ebp->cur_result_ptr;
	if (0x4000 - eax < esi) {
		u8 *blk = xpgalloc(0x4000);
		if (!blk) return false;
		*(u8 **)blk = ebp->result_blocks;
		ebp->result_blocks = blk;
		eax = 4;
		ebp->cur_result_ptr = eax;
	}
	/* .nonewblock */
	listitem *it = (listitem *)(ebp->result_blocks + eax);
	ebp->cur_result_ptr += esi;
	strcpy(it->text, ebx);
	listitem *last = ebp->list_end;
	it->prev = last;
	it->next = 0;
	ebp->list_end = it;
	if (last) last->next = it;
	ebp->list.numitems++;
	if (!ebp->list.curitemptr) ebp->list.curitemptr = it;
	if (!ebp->list.head) ebp->list.head = it;
	ebp->datachanged = 1;
	return true;
}

/* filesearch_thread.scan_folder_next_pop, .scan_folder_next: curfile
 * back to curdir, the next entry */
static void filesearch_next_pop(filesearch_data *ebp)
{
	ebp->edi[-1] = 0;
	ebp->pos++;
	ebp->cur++;
}

/* filetool.inc: filesearch_test_file, a block: 0 not decided yet, 1 the
 * string is in the file, -1 it is not, -2 cancel */
static int filesearch_test_file_step(filesearch_data *ebp)
{
	fileinfo *ebx = &ebp->fs;
	u32 n, eax;
	for (;;) {
		/* .read_retry */
		eax = fs_call(ebx, &n);
		if (!eax || eax == 6) break;
		if (ebp->skip_errors_mask & skip_read_file) return -1;
		int al = recursive_read_err(aCannotReadFile, ebp->curfile, eax);
		if (!al) continue;
		if (al == 2) {
			al = 1;
			ebp->skip_errors_mask |= skip_read_file;
		}
		if (al != 1) return -2;
		return -1;              /* .ret_failed */
	}
	/* .readok: edi = the current state, edx -> the automaton, ecx =
	 * the final state (the original's loop, unrolled twice, reads one
	 * byte beyond the data when there is an odd number of them) */
	const u8 *edi = ebp->state;
	for (u32 i = 0; i < n; i++) {
		u32 s = edi[ebp->filebuf[i]];
		if (s == ebp->final_state) return 1;            /* .ret_ok_pop */
		edi = ebp->fsm + (s << 8);
	}
	ebp->state = edi;
	/* .scandone, .eof */
	u64 pos = (ebx->first | (u64)ebx->first_hi << 32) + n;
	ebx->first = pos;
	ebx->first_hi = pos >> 32;
	if (n == search_filebuf_size) return 0;                 /* .loop */
	/* EOF, last chance for whole-words-only search */
	if (!(ebp->query_dlgdata.whole.flags & 0x10)) return -1;
	return edi[' '] == ebp->final_state ? 1 : -1;
}

/* filetool.inc: filesearch_thread, a step */
static void filesearch_thread_step(filesearch_data *ebp)
{
	/* .check_stop */
	if (ebp->stop == 1) {
		filesearch_thread_end(ebp);             /* .cancel */
		return;
	}
	if (ebp->stop > 1) return;
	switch (ebp->phase) {
	case FS_READ_FOLDER: {
		/* .read_folder_loop */
		strcpy(ebp->curfile, ebp->curdir);
		ebp->edi = ebp->curfile + strlen(ebp->curfile) + 1;
		u32 ebx;
		for (;;) {
			/* .read_retry */
			ebp->fs = (fileinfo){ 1, ebp->pos, 0, search_dir_query_size, ebp->dir_area, 0, ebp->curfile };
			u32 eax = fs_call(&ebp->fs, &ebx);
			if (!eax || eax == 6) break;
			/* (the original takes the count of the area's header,
			 * not written on an error: none here) */
			ebx = 0;
			*(u32 *)(ebp->dir_area + 4) = 0;
			if (ebp->skip_errors_mask & skip_read_folder) break;    /* .skip1 */
			int al = recursive_read_err(aCannotReadFolder, ebp->curdir, eax);
			if (!al) continue;
			if (al == 2) {
				al = 1;
				ebp->skip_errors_mask |= skip_read_folder;
			}
			if (al != 1) {
				filesearch_thread_end(ebp);     /* .cancel */
				return;
			}
			break;
		}
		/* .read_folder_ok */
		ebp->nread = ebx;
		ebp->cur = 0;
		ebp->phase = FS_SCAN;
		return;
	}
	case FS_SCAN: {
		/* .scan_folder_loop */
		if (ebp->cur >= ebp->nread) {
			/* .scan_folder_done */
			if (*(u32 *)(ebp->dir_area + 4) == search_dir_query_size) {
				ebp->phase = FS_READ_FOLDER;
				return;
			}
			u32 esi = ebp->stack[--ebp->sp];
			if (++esi == 0) {
				filesearch_thread_end(ebp);     /* .done */
				return;
			}
			ebp->pos = esi;
			*strrchr(ebp->curdir, '/') = 0;
			ebp->datachanged = 1;
			ebp->phase = FS_READ_FOLDER;
			return;
		}
		u8 *eax = ebp->dir_area + 32 + ebp->cur * 304;
		/* ignore the special entries "." and ".." */
		if ((eax[40] == '.' && eax[41] == 0) || bdfe_is_dotdot(eax)) {
			ebp->pos++;                     /* .scan_folder_next */
			ebp->cur++;
			return;
		}
		/* construct the name */
		char *edi = ebp->edi, *end = ebp->curfile + 1023;
		const char *s = bdfe_name(eax);
		edi[-1] = '/';
		for (;;) {
			if (edi >= end) {
				/* .namebig */
				*edi = 0;
				if (!(ebp->skip_errors_mask & skip_bigname)) {
					const char *strings[2] = {
						bdfe_attr(eax) & 0x10 ? aFolderNameTooBig : aFileNameTooBig, ebp->curfile };
					int al = SayErr(2, strings, 3, SkipOrCancelBtn);
					if (al == 1) {
						al = 0;
						ebp->skip_errors_mask |= skip_bigname;
					}
					if (al) {
						filesearch_thread_end(ebp);
						return;
					}
				}
				filesearch_next_pop(ebp);       /* .namebig.skip */
				return;
			}
			if (!(*edi++ = *s++)) break;
		}
		if (bdfe_attr(eax) & 0x10) {
			/* it is a nested folder, enter recursion */
			strcpy(ebp->curdir, ebp->curfile);
			ebp->datachanged = 1;
			if (ebp->sp == (int)(sizeof(ebp->stack) / sizeof(ebp->stack[0]))) {
				filesearch_next_pop(ebp);       /* (the stack full: skipped) */
				return;
			}
			ebp->stack[ebp->sp++] = ebp->pos;
			ebp->pos = 0;                   /* .enter_recursion */
			ebp->phase = FS_READ_FOLDER;
			return;
		}
		/* .scan_file: it is a file; first check: does the file name
		 * match the mask? */
		char *lw = ebp->lowername;
		s = bdfe_name(eax);
		do *lw = tolower_table[(u8)*s++]; while (*lw++);
		if (!match_mask_rev_lowercase(ebp->lowername, lw - 1, ebp->mask.data)) {
			filesearch_next_pop(ebp);
			return;
		}
		/* ok, second check: is the string present in this file? */
		if (!ebp->final_state) {
			if (!filesearch_file_found(ebp)) {
				filesearch_thread_end(ebp);
				return;
			}
			filesearch_next_pop(ebp);
			return;
		}
		ebp->fs = (fileinfo){ 0, 0, 0, search_filebuf_size, ebp->filebuf, 0, ebp->curfile };
		ebp->state = ebp->fsm;
		if (ebp->query_dlgdata.whole.flags & 0x10) ebp->state += 256;
		ebp->phase = FS_TEST_FILE;
		return;
	}
	case FS_TEST_FILE: {
		int r = filesearch_test_file_step(ebp);
		if (!r) return;                         /* the next block */
		if (r == -2) {
			filesearch_thread_end(ebp);     /* filesearch_thread.cancel */
			return;
		}
		ebp->phase = FS_SCAN;
		if (r == 1 && !filesearch_file_found(ebp)) {
			filesearch_thread_end(ebp);
			return;
		}
		filesearch_next_pop(ebp);
		return;
	}
	}
}
