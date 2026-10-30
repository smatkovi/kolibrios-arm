/* kfar for KolibriOS for ARM: the editor (F4).
 *
 * Translated to C from kfar's editor.inc, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0.
 *
 * The text is kept as editor.inc keeps it: in blocks of 4096 bytes after
 * the editor's data (editor_data, then the starts of the screen's lines,
 * editor_line), each block with its header (edit_block_header: the next
 * and the previous block as offsets from the editor's data, the end of
 * its text) and RESERVE_IN_BLOCK bytes free for quick inserting; the
 * unused blocks in a list (freeblocks). The screen's lines start at
 * absolute addresses (editor_line.block), the blocks are linked by their
 * offsets, as in the original.
 *
 * Differences from the original:
 * - the editor's data comes from malloc, which is not page aligned as
 *   68.12's memory is: the original's "and reg, not 0xFFF" on a block's
 *   address counts from the data's start here (PAGE());
 * - when the data grows (xpgrealloc) and moves, the screen's entry in the
 *   screens' list and all the line starts follow it (the original keeps
 *   the old address in the list and also moves the lines past the end of
 *   the text, which have none);
 * - fixed in the original's code: editor_delete_space.delete_block set
 *   first_block for the last block and last_block for the first one
 *   (swapped); editor_delete_space copied "esi" bytes instead of eax
 *   when the deletion crosses a block, and took the deleted block's next
 *   from the free list; editor_reserve_space did not set the previous
 *   block of the block after the new ones; editor_move_linestart stored
 *   the cursor's block as an address instead of an offset; edit_file
 *   read the first block's header without the data's address; saving an
 *   empty text and cancelling a write error left the stack unbalanced;
 *   a block merged into the one before it keeps the screen's first line
 *   (the original's line start would point into the freed block);
 * - plugins are not supported (kfar.h): their branches are left out, the
 *   checks of hPlugin (always 0) are kept. */
#include "kfar.h"

/* when a file is loaded into the editor, this file is fragmented to
 * blocks; each block has RESERVE_IN_BLOCK empty bytes to allow quick
 * inserting; must be dword-aligned! (edit.RESERVE_IN_BLOCK) */
#define RESERVE_IN_BLOCK 16

/* editor.inc: editor_data */
typedef struct editor_data {
	void *hPlugin;
	void *hFile;
	u32 memsize;            /* in pages */
	u8 encoding;
	u8 flags;               /* & 0x80: modified, & 0x40: the file is locked,
				 * & 0x20: the next key as a symbol,
				 * & 0x10: replace mode (vs insert) */
	u8 eol;
	u8 pad1;
	u32 first_block;        /* offsets from the editor's data */
	u32 last_block;
	u32 numfree;
	u32 freeblocks;
	u32 numlines;
	u32 curline;
	u32 curcol;
	u32 cur_block;
	u32 cur_offs;
	int cur_delta;          /* -1: on a symbol, else the columns beyond the line's end */
	int cursor_x;
	int cursor_y;
	u8 align200[0x200 - 64];
	char filename[1024];
	char hostname[1024];
	u8 buf[16384];          /* all I/O operations use this buffer */
	/* .basesize; .linedata_start: max_height editor_line */
} editor_data;
_Static_assert(offsetof(editor_data, hPlugin) == 0 && offsetof(editor_data, hFile) == 4,
	       "error in viewer_IsHandleUsed/editor_IsHandleUsed");
_Static_assert(offsetof(editor_data, filename) == 0x200 && offsetof(editor_data, hostname) == 0x600,
	       "error in viewer_getname/editor_getname");
#define editor_data_basesize sizeof(editor_data)

/* editor.inc: editor_line: a line's start; plugdata follows */
typedef struct __attribute__((packed)) editor_line {
	u8 *block;
	u16 offs;
} editor_line;
#define editor_line_plugdata 6
#define LINESIZE (editor_line_plugdata + EditPlugInfo)
#define linedata_start(ebp) ((u8 *)(ebp) + editor_data_basesize)
#define LINE(ebp, i) ((editor_line *)(linedata_start(ebp) + (i) * LINESIZE))

/* editor.inc: edit_block_header */
typedef struct edit_block_header { u32 next, prev, limit; } edit_block_header;
#define edit_block_header_size 12

#define HDR(p) ((edit_block_header *)(p))
#define BLK(ebp, off) ((u8 *)(ebp) + (off))
#define REL(ebp, p) ((u32)((u8 *)(p) - (u8 *)(ebp)))
/* the original's "and reg, not 0xFFF" on a block's address */
#define PAGE(ebp, p) ((u8 *)(ebp) + (REL(ebp, p) & ~0xFFFu))

static u8 editor_update_cur(editor_data *ebp);
static void editor_draw_text(editor_data *ebp);
static bool editor_test_cursor_y(editor_data *ebp);
static void editor_OnKey_right(void *ebp, u32 eax);
static void editor_OnKey_exit_save(void *ebp, u32 eax);
static void editor_OnKey_backspace(void *ebp, u32 eax);
static void editor_OnKey_home(void *ebp, u32 eax);
static void editor_OnKey_up(void *ebp, u32 eax);
static void editor_OnKey_pgup(void *ebp, u32 eax);
static void editor_OnKey_left(void *ebp, u32 eax);
static void editor_OnKey_end(void *ebp, u32 eax);
static void editor_OnKey_down(void *ebp, u32 eax);
static void editor_OnKey_pgdn(void *ebp, u32 eax);
static void editor_OnKey_ins(void *ebp, u32 eax);
static void editor_OnKey_del(void *ebp, u32 eax);

/* kfar.asm: editor_ctrlkeys */
const ctrlkey editor_ctrlkeys[] = {
	{ 1, 0, 0, editor_OnKey_exit_confirm },
	{ 0x0E, 0, 0, editor_OnKey_backspace },
	{ 0x3C, 0, 0, editor_OnKey_f2 },
	{ 0x41, 0, 0, editor_OnKey_f7 },
	{ 0x41, 0, 1, editor_OnKey_shift_f7 },
	{ 0x43, 0, 0x100, alt_f9 },
	{ 0x44, 0, 0, editor_OnKey_exit_confirm },
	{ 0x44, 0, 1, editor_OnKey_exit_save },
	{ 0x47, 0, 0, editor_OnKey_home },
	{ 0x48, 0, 0, editor_OnKey_up },
	{ 0x49, 0, 0, editor_OnKey_pgup },
	{ 0x4B, 0, 0, editor_OnKey_left },
	{ 0x4D, 0, 0, editor_OnKey_right },
	{ 0x4F, 0, 0, editor_OnKey_end },
	{ 0x50, 0, 0, editor_OnKey_down },
	{ 0x51, 0, 0, editor_OnKey_pgdn },
	{ 0x52, 0, 0, editor_OnKey_ins },
	{ 0x53, 0, 0, editor_OnKey_del },
	{ 0x58, 0, 0, F12 },
	{ 0x0F, 0, 0x100, dummy_key },          /* Alt+Tab */
	{ 0x0F, 0, 0x101, dummy_key },          /* Shift+Alt+Tab */
	{ 0 },
};

/* kfar.asm start: EditDataSize, the pages of editor_data and the lines */
void editor_calc_datasize(void)
{
	u32 eax = EditPlugInfo + editor_line_plugdata;
	eax *= max_height;
	eax += editor_data_basesize + 0xFFF;
	EditDataSize = eax >> 12;
}

/* a line's start, with its plugins' data zeroed */
static void editor_set_line(u8 *edi, u8 *esi, u32 ebx)
{
	((editor_line *)edi)->block = esi;
	((editor_line *)edi)->offs = ebx;
	memset(edi + editor_line_plugdata, 0, EditPlugInfo);
}

/* the editor's data moved (xpgrealloc): its screen and the line starts
 * follow it */
static void editor_moved(editor_data *old, editor_data *ebp)
{
	s32 d = (u8 *)ebp - (u8 *)old;
	for (int i = 0; i < num_screens; i++)
		if (screens[i].data == old) screens[i].data = ebp;
	if (active_screen_data == old) active_screen_data = ebp;
	for (int i = 0; i < max_height; i++) {
		editor_line *l = LINE(ebp, i);
		if (l->block) l->block += d;
	}
}

/* editor.inc: edit_file.nomemory */
static void edit_file_nomemory(void)
{
	const char *s[1] = { aEditNoMemory };
	SayErr(1, s, 1, ContinueBtn);
}

static void editor_init_lines(editor_data *ebp);

/* editor.inc: edit_file: the panel's file at the index ecx */
void edit_file(PanelData *panel, int idx)
{
	u8 *ecx = panel->files[idx];
	if (bdfe_attr(ecx) & 0x10) return;
	/* .file: calculate required memory size */
	if (*(u32 *)(ecx + 36)) {
		edit_file_nomemory();
		return;
	}
	/* block size = 4096; block header: edit_block_header.size bytes;
	 * some plugin-specific data can follow; reserve RESERVE_IN_BLOCK free
	 * bytes in the end of block */
	u32 ebx = 4096;
	EditBlockStart = EditPlugInfo + edit_block_header_size;
	ebx -= EditBlockStart + RESERVE_IN_BLOCK;
	EditBlockSize = ebx;
	/* now ebx = size of file data in each block */
	u32 eax = *(u32 *)(ecx + 32);
	if (!eax) eax = 1;
	u32 edx = eax % ebx;
	eax /= ebx;
	if (edx) eax++;
	eax += EditDataSize;
	/* eax = number of blocks + memory for editor_data structure */
	if (eax >= 0x80000000u >> 12) {
		edit_file_nomemory();
		return;
	}
	/* .memok */
	const char *esi = panel->dir;
	editor_data *ebp = new_screen(eax << 12, &editor_vtable);
	if (!ebp) return;
	ebp->memsize = eax;
	ebp->eol = EditEOLStyle;
	ebp->hPlugin = panel->hPlugin;
	/* (on a plugin's panel the host's name went to hostname: no plugins) */
	ebp->hFile = panel->hFile;
	ebp->encoding = encodings_cp866;
	ebp->flags = 0;
	ebp->numlines = 1;
	char *fn = ebp->filename;
	readinfo.name = fn;
	while (*esi) *fn++ = *esi++;
	if (fn[-1] != '/') *fn++ = '/';
	strcpy(fn, bdfe_name(ecx));
	/* load file into memory */
	s32 nblocks = ebp->memsize - EditDataSize;      /* esi = number of blocks */
	u32 first = EditDataSize << 12;
	ebp->first_block = ebp->cur_block = first;
	u8 *edi = BLK(ebp, first);                      /* edi -> first block */
	LINE(ebp, 0)->block = edi;
	readinfo.first = readinfo.first_hi = 0;
	readinfo.size = 16384;
	readinfo.data = ebp->buf;
	/* (plugins: their open) */
	u32 rest = EditBlockSize;               /* bytes rest in the current block */
	edi += EditBlockStart;
	for (;;) {
		/* .readloop, .readnative */
		u32 nread, err;
		for (;;) {
			err = fs_call(&readinfo, &nread);
			if (err == 0 || err == 6) break;
			const char *s[3] = { get_error_msg(err), aCannotReadFile, readinfo.name };
			if (SayErr(3, s, 2, RetryOrCancelBtn) != 0) {
				/* .readfailed */
				delete_active_screen();
				return;
			}
		}
		u64 pos = (readinfo.first | (u64)readinfo.first_hi << 32) + nread;
		readinfo.first = pos;
		readinfo.first_hi = pos >> 32;
		/* .readok: eax = number of bytes read */
		if (!nread && REL(ebp, edi) != ebp->first_block) break;
		u8 *src = readinfo.data;
		u32 left = nread;
		do {
			/* .loadloop */
			if (!rest) {
				if (--nblocks < 0) {
					ebp->memsize += 8;
					nblocks += 8;
					u32 ms = ebp->memsize;
					editor_data *n = 0;
					if (ms < 0x80000000u >> 12) {
						u32 edi_rel = REL(ebp, edi), src_off = src - ebp->buf;
						n = xpgrealloc(ebp, ms << 12);
						if (n) {
							editor_moved(ebp, n);
							ebp = n;
							edi = BLK(ebp, edi_rel);
							src = ebp->buf + src_off;
							readinfo.name = ebp->filename;
							readinfo.data = ebp->buf;
						}
					}
					if (!n) {
						/* .nomemory2 */
						edit_file_nomemory();
						delete_active_screen();
						return;
					}
				}
				/* .hasblock */
				u8 *page = PAGE(ebp, edi);
				HDR(page)->next = REL(ebp, page) + 0x1000;
				HDR(page)->prev = REL(ebp, page) - 0x1000;
				HDR(page)->limit = edi - page;
				/* info for plugins: zeroed */
				memset(page + edit_block_header_size, 0, EditPlugInfo == -1 ? 0 : EditPlugInfo);
				edi = page + 0x1000 + EditBlockStart;
				rest = EditBlockSize;
			}
			/* .hasplace */
			u32 n = left < rest ? left : rest;
			memcpy(edi, src, n);
			u8 *p = edi;
			src += n;
			edi += n;
			/* calculate number of lines in this block */
			if (n) {
				u8 al = p[-1];
				u32 rel = REL(ebp, p) - EditBlockStart;
				if (!(rel & 0xFFF)) {
					al = 0;
					if (rel != ebp->first_block) {
						u8 *prev = BLK(ebp, rel - 0x1000);
						al = prev[HDR(prev)->limit - 1];
					}
				}
				/* .0 */
				u8 dl = al == 13 ? 10 : 0;
				for (u32 c = n; c; c--) {
					/* .1 */
					al = *p++;
					if (al != 13 && al != 10) {
						dl = 0;
						continue;
					}
					bool same = al == dl;
					dl = 0;
					if (same) continue;         /* the LF of CR LF */
					ebp->numlines++;
					if (al != 10) dl = 10;
				}
			}
			/* .4 */
			rest -= n;
			left -= n;
		} while (left);
		if (nread != 16384) break;
	}
	/* .readdone */
	u8 *page = PAGE(ebp, edi);
	HDR(page)->next = 0;
	HDR(page)->prev = REL(ebp, page) - 0x1000;
	HDR(page)->limit = edi - page;
	memset(page + edit_block_header_size, 0, EditPlugInfo == -1 ? 0 : EditPlugInfo);
	edx = REL(ebp, page) + 0x1000;
	u32 nfree = ebp->memsize - (edx >> 12);
	ebp->numfree = nfree;
	if (nfree) {
		ebp->freeblocks = edx;
		u8 *f = page;
		for (u32 i = 0; i < nfree; i++) {
			/* .addfree */
			f += 0x1000;
			edx += 0x1000;
			*(u32 *)f = edx;
		}
		*(u32 *)f = 0;
	}
	/* .nofree */
	ebp->last_block = REL(ebp, page);
	HDR(BLK(ebp, EditDataSize << 12))->prev = 0;
	ebp->curline = ebp->curcol = 0;
	ebp->cursor_x = 0;
	ebp->cursor_y = 1;
	LINE(ebp, 0)->offs = EditBlockStart;
	ebp->cur_offs = EditBlockStart;
	ebp->cur_delta = HDR(BLK(ebp, ebp->first_block))->limit == (u32)EditBlockStart ? 0 : -1;
	editor_init_lines(ebp);
	editor_OnRedraw(ebp);
}

/* editor.inc: editor_step_forward (editor_normalize_offs after the step):
 * in: esi = block (must be nonzero), ebx = offset in block; out: esi =
 * block (zero iff EOF reached), ebx = offset in block; true (CF=1) iff
 * EOF NOT reached */
static bool editor_normalize_offs(editor_data *ebp, u8 **esi, u32 *ebx)
{
	if (*ebx < HDR(*esi)->limit) return true;
	u32 next = HDR(*esi)->next;
	*ebx = EditBlockStart;
	if (!next) {
		*esi = 0;
		return false;
	}
	*esi = BLK(ebp, next);
	return true;
}

static bool editor_step_forward(editor_data *ebp, u8 **esi, u32 *ebx)
{
	++*ebx;
	return editor_normalize_offs(ebp, esi, ebx);
}

/* editor.inc: editor_step_backward: in: esi = block (must be nonzero),
 * ebx = offset in block; out: esi = block (zero iff input was at the
 * beginning), ebx = offset in block; true (CF=1) iff start of file
 * reached */
static bool editor_step_backward(editor_data *ebp, u8 **esi, u32 *ebx)
{
	--*ebx;
	if (*ebx >= (u32)EditBlockStart) return false;
	u32 prev = HDR(*esi)->prev;
	if (!prev) {
		*esi = 0;
		return true;
	}
	*esi = BLK(ebp, prev);
	*ebx = HDR(*esi)->limit - 1;
	return false;
}

/* editor.inc: editor_save */
static void editor_save(editor_data *ebp);
/* editor.inc: editor_calc_filesize */
static u32 editor_calc_filesize(editor_data *ebp)
{
	u32 eax = 0;
	u32 esi = ebp->first_block;
	do {
		u8 *b = BLK(ebp, esi);
		eax += HDR(b)->limit - EditBlockStart;
		esi = HDR(b)->next;
	} while (esi);
	return eax;
}

/* editor.inc: editor_get_data: up to ecx bytes from esi:ebx to edi;
 * returns their number, esi:ebx after them */
static u32 editor_get_data(editor_data *ebp, u8 **esi, u32 *ebx, u8 *edi, u32 ecx)
{
	u8 *start = edi;
	if (!*esi) return 0;
	for (;;) {
		/* .loop */
		u32 edx = HDR(*esi)->limit - *ebx;
		u32 n = ecx < edx ? ecx : edx;
		memcpy(edi, *esi + *ebx, n);
		edi += n;
		*ebx += n;
		if (ecx < edx) break;
		ecx -= edx;
		u32 next = HDR(*esi)->next;
		*ebx = EditBlockStart;
		if (!next) {
			*esi = 0;
			break;
		}
		*esi = BLK(ebp, next);
	}
	return edi - start;
}

static void editor_save(editor_data *ebp)
{
	if (ebp->hPlugin) {
		const char *s[1] = { aCannotSaveToPlugin };
		SayErr(1, s, 1, ContinueBtn);
		return;
	}
	/* .native */
	fileinfo *ebx = &writeinfo;
	ebx->first = editor_calc_filesize(ebp);
	ebx->first_hi = 0;
	ebx->size = 0;
	ebx->data = 0;
	ebx->name = ebp->filename;
	for (;;) {
		/* .setsize_retry */
		ebx->code = 4;
		u32 err = fs_call(ebx, 0);
		ebx->code = 3;
		if (!err) break;
		const char *s[3] = { get_error_msg(err), aCannotWriteFile, ebx->name };
		if (SayErr(3, s, 2, RetryOrCancelBtn) != 0) return;
	}
	/* .sizeok */
	ebx->first = 0;
	u8 *esi = BLK(ebp, ebp->first_block);
	u32 off = EditBlockStart;
	if (editor_normalize_offs(ebp, &esi, &off)) {
		u8 *edi = ebp->buf;
		for (;;) {
			/* .loop */
			u32 eax = editor_get_data(ebp, &esi, &off, edi, 16384);
			if (!eax) break;
			ebx->size = eax;
			ebx->data = edi;
			for (;;) {
				/* .write_retry */
				u32 err = fs_call(ebx, 0);
				if (!err) break;
				const char *s[3] = { get_error_msg(err), aCannotWriteFile, ebx->name };
				if (SayErr(3, s, 2, RetryOrCancelBtn) != 0) return;
			}
			/* .writeok */
			u64 pos = (ebx->first | (u64)ebx->first_hi << 32) + ebx->size;
			ebx->first = pos;
			ebx->first_hi = pos >> 32;
		}
	}
	/* .done */
	ebp->flags &= ~0x80;
}

/* editor.inc: editor_get_pos: the screen's first line as esi:ebx (esi
 * zero at the end of the text) */
static void editor_get_pos(editor_data *ebp, u8 **esi, u32 *ebx)
{
	*esi = LINE(ebp, 0)->block;
	*ebx = LINE(ebp, 0)->offs;
	while (*esi && *ebx >= HDR(*esi)->limit) {
		u32 next = HDR(*esi)->next;
		*ebx = EditBlockStart;
		*esi = next ? BLK(ebp, next) : 0;
	}
}

/* editor.inc: editor_curline_start: edi = the cursor's line, esi:ebx its
 * start */
static void editor_curline_start(editor_data *ebp, u8 **edi, u8 **esi, u32 *ebx)
{
	*edi = (u8 *)LINE(ebp, ebp->cursor_y - 1);
	*esi = ((editor_line *)*edi)->block;
	*ebx = ((editor_line *)*edi)->offs;
}

/* editor.inc: editor_get_string: read string up to the end of line;
 * in: esi = block, ebx = offset in block, edi = destination, ecx =
 * maximum number of bytes to copy, edx = number of bytes to skip; out:
 * esi = block, ebx = offset in block, ecx = number of rest bytes (ecx_in
 * - ecx_out = number of copied characters) */
static void editor_get_string(editor_data *ebp, u8 **esi, u32 *ebx, u16 **edi, int *ecx, int edx)
{
	u8 ah = edit_normal_color;
	if (*ebx == HDR(*esi)->limit) return;  /* .retnp */
	int pos = 0;                            /* current position in line */
	for (;;) {
		/* .loop */
		if (!*ecx) return;
		u8 al = (*esi)[*ebx];
		if (al == 13 || al == 10) return;
		if (al == 9) {
			/* .tab */
			int t = editor_tabsize - pos % editor_tabsize;
			pos += t;
			edx -= t;
			if (edx < 0) {
				do {
					*(*edi)++ = CELL(' ', ah);
					if (!--*ecx) return;
				} while (++edx);
			}
		} else {
			pos++;
			if (--edx < 0) {
				edx = 0;
				*(*edi)++ = CELL(al, ah);
				--*ecx;
			}
		}
		/* .4 */
		if (!editor_step_forward(ebp, esi, ebx)) return;
	}
}

/* editor.inc: editor_find_newline: in: esi = block, ebx = offset in
 * block; out: esi = block, ebx = offset in block (the next line's start),
 * the line's length */
static u32 editor_find_newline(editor_data *ebp, u8 **esi, u32 *ebx)
{
	u32 ecx = 0;
	u8 al;
	if (!*esi) return 0;                    /* .ret0 */
	if (*ebx >= HDR(*esi)->limit) {
		*esi = 0;
		return 0;
	}
	do {
		/* .1 */
		al = (*esi)[*ebx];
		ecx++;
		editor_step_forward(ebp, esi, ebx);
		if (al == 13 || al == 10) goto nl;
	} while (*esi);
ret1:
	*esi = BLK(ebp, ebp->last_block);
	*ebx = HDR(*esi)->limit;
	return ecx;
nl:
	/* .2 */
	ecx--;
	if (!*esi) goto ret1;
	if (al != 13 || (*esi)[*ebx] != 10) return ecx;
	if (!editor_step_forward(ebp, esi, ebx)) goto ret1;
	return ecx;
}

/* editor.inc: editor_prev_newline: esi:ebx to the start of the line
 * before; returns its length */
static u32 editor_prev_newline(editor_data *ebp, u8 **esi, u32 *ebx)
{
	u32 ecx = 0;
	u8 al;
	if (!*esi) {
		*esi = BLK(ebp, ebp->last_block);
		*ebx = HDR(*esi)->limit;
	}
	if (editor_step_backward(ebp, esi, ebx)) goto ret;
	al = (*esi)[*ebx];
	if (editor_step_backward(ebp, esi, ebx)) goto ret;
	if (!(al == 10 && (*esi)[*ebx] == 13)) goto dot1;
	for (;;) {
		if (editor_step_backward(ebp, esi, ebx)) goto ret;
	dot1:
		/* .1 */
		ecx++;
		al = (*esi)[*ebx];
		if (al == 13 || al == 10) break;
	}
	ecx--;
	++*ebx;
	if (*ebx >= HDR(*esi)->limit) {
		u32 next = HDR(*esi)->next;
		*ebx = EditBlockStart;
		*esi = next ? BLK(ebp, next) : 0;
	}
ret:
	if (!*esi) {
		*esi = BLK(ebp, ebp->first_block);
		*ebx = EditBlockStart;
	}
	return ecx;
}

/* editor.inc: editor_init_lines: the screen's lines from the first one */
static void editor_init_lines(editor_data *ebp)
{
	u8 *esi;
	u32 ebx;
	editor_get_pos(ebp, &esi, &ebx);
	if (!esi) {
		esi = BLK(ebp, ebp->last_block);
		ebx = HDR(esi)->limit;
	}
	u8 *edi = linedata_start(ebp);
	for (int i = 0; i < max_height; i++, edi += LINESIZE) {
		/* .1 */
		editor_set_line(edi, esi, ebx);
		editor_find_newline(ebp, &esi, &ebx);
	}
}

/* editor.inc: editor_draw_status: the first row: the name, the flags,
 * the encoding, the line, the column, the symbol's code */
static void editor_draw_status(editor_data *ebp)
{
	u16 *edi = (u16 *)ebp->buf;
	u8 ah = edit_status_color;
	int ecx = cur_width - 56;
	if (ecx < 25) ecx = 25;
	edi = viewedit_draw_filename(ebp, edi, &ecx, ah);
	for (ecx++; ecx > 0; ecx--) *edi++ = CELL(' ', ah);
	*edi++ = CELL(ebp->flags & 0x80 ? '*' : ' ', ah);
	*edi++ = CELL(ebp->flags & 0x40 ? '-' : ' ', ah);
	*edi++ = CELL(ebp->flags & 0x20 ? '"' : ' ', ah);
	for (int i = 0; i < 10; i++) *edi++ = CELL(' ', ah);
	/* the encoding, aligned to the right of these spaces */
	{
		const char *esi = encodings_names[ebp->encoding];
		int n = 8;
		while (esi[n - 1] == ' ') n--;
		u16 *p = edi - 1;
		while (n) *p-- = CELL(esi[--n], ah);
	}
	for (int i = 0; i < aLine_size; i++) *edi++ = CELL(aLine[i], ah);
	for (int i = 0; i < 13; i++) *edi++ = CELL(' ', ah);
	/* "line/lines", aligned to the right of these spaces */
	{
		u16 *p = edi - 1;
		u32 eax = ebp->numlines;
		do {
			*p-- = CELL('0' + eax % 10, ah);
			eax /= 10;
		} while (eax);
		*p-- = CELL('/', ah);
		eax = ebp->curline + ebp->cursor_y;
		do {
			*p-- = CELL('0' + eax % 10, ah);
			eax /= 10;
		} while (eax);
		*p = CELL(' ', ah);
	}
	for (int i = 0; i < aCol_size; i++) *edi++ = CELL(aCol[i], ah);
	{
		u32 eax = ebp->curcol + ebp->cursor_x + 1;
		char d[12];
		int n = 0;
		do d[n++] = '0' + eax % 10; while ((eax /= 10));
		while (n) *edi++ = CELL(d[--n], ah);
	}
	for (int i = 0; i < 13; i++) *edi++ = CELL(' ', ah);
	u16 *con = get_console_ptr(0, 0);
	memcpy(con, ebp->buf, cur_width * 2);
	if (ebp->cur_delta == -1) {
		/* the code of the symbol under the cursor, at the right */
		u16 *e = con + cur_width;
#define SETCH(p, c) (*(p) = (*(p) & 0xff00) | (u8)(c))
		for (int i = 1; i <= 4; i++) SETCH(e - i, ' ');
		u8 c = BLK(ebp, ebp->cur_block)[ebp->cur_offs];
		u32 al = c / 100, edx = c % 100;
		if (al) SETCH(e - 3, '0' + al);
		u8 tens = edx / 10, units = edx % 10;
		if (tens || (u8)e[-3] != ' ') SETCH(e - 2, '0' + tens);
		SETCH(e - 1, '0' + units);
#undef SETCH
	}
}

/* editor.inc: editor_draw_line: the line starting at esi:ebx in the row
 * edx */
static void editor_draw_line(editor_data *ebp, u8 *esi, u32 ebx, int edx)
{
	int ecx = cur_width;
	if (!esi) {
		/* .2 */
		u16 *edi = get_console_ptr(0, edx);
		while (ecx--) *edi++ = CELL(' ', edit_normal_color);
		return;
	}
	u16 *edi = (u16 *)ebp->buf;
	editor_get_string(ebp, &esi, &ebx, &edi, &ecx, ebp->curcol);
	while (ecx-- > 0) *edi++ = CELL(' ', edit_normal_color);
	memcpy(get_console_ptr(0, edx), ebp->buf, cur_width * 2);
}

/* editor.inc: editor_draw_text */
static void editor_draw_text(editor_data *ebp)
{
	editor_draw_status(ebp);
	for (int edx = 1; edx < cur_height - 1; edx++) {
		editor_line *l = LINE(ebp, edx - 1);
		editor_draw_line(ebp, l->block, l->offs, edx);
	}
	draw_image();
}

/* editor.inc: editor_set_keybar: F8 names the other encoding */
static void editor_set_keybar(editor_data *ebp)
{
	int esi = ebp->encoding == encodings_cp1251 ? 0 : 1;
	memcpy(keybar_cp2, encodings_names[esi], 6);
	draw_keybar();
}

/* editor.inc: editor_test_cursor_x */
static void editor_test_cursor_x(editor_data *ebp)
{
	u32 ecx = cur_width - 1;
	if ((u32)ebp->cursor_x > ecx) {
		ebp->curcol += ebp->cursor_x - ecx;
		ebp->cursor_x = ecx;
		cursor_x = ecx;
	}
}

/* editor.inc: editor_OnRedraw */
void editor_OnRedraw(void *p)
{
	editor_data *ebp = p;
	cursor_x = ebp->cursor_x;
	cursor_y = ebp->cursor_y;
	if (ebp->flags & 0x10) cursor_size = cursor_big_size;
	editor_test_cursor_x(ebp);
	editor_test_cursor_y(ebp);
	editor_set_keybar(ebp);
	editor_draw_text(ebp);
}

/* editor.inc: editor_up_scroll: the view ecx lines up (then
 * editor_update_cur) */
static void editor_up_scroll(editor_data *ebp, u32 ecx)
{
	ebp->curline -= ecx;
	int edx = LINESIZE;
	u8 *base = linedata_start(ebp);
	memmove(base + ecx * edx, base, (max_height - ecx) * edx);
	u8 *edi = base + ecx * edx;
	u8 *esi = ((editor_line *)edi)->block;
	u32 ebx = ((editor_line *)edi)->offs;
	while (ecx--) {
		editor_prev_newline(ebp, &esi, &ebx);
		edi -= edx;
		editor_set_line(edi, esi, ebx);
	}
	/* fall through to editor_update_cur */
	editor_update_cur(ebp);
}

/* editor.inc: editor_update_cur: the cursor's block and offset from
 * cursor_x; returns al = 1 if the view moved (a full redraw is needed) */
static u8 editor_update_cur(editor_data *ebp)
{
	u32 ecx = ebp->cursor_x + ebp->curcol;
	u8 *edi, *esi;
	u32 ebx;
	editor_curline_start(ebp, &edi, &esi, &ebx);
	u32 edx = 0;                            /* current position in the line */
	u8 al;
	if (ebx == HDR(esi)->limit) goto notfound;
	for (;;) {
		/* .scan */
		u8 c = esi[ebx];
		if (c == 13 || c == 10) goto notfound;
		if (!ecx) goto found;
		if (c == 9) {
			/* .tab */
			u32 t = editor_tabsize - edx % editor_tabsize;
			if (ecx < t) {
				/* .curintab */
				al = edx < ebp->curcol;
				if (al) ebp->curcol = edx;
				ebp->cursor_x = edx - ebp->curcol;
				cursor_x = ebp->cursor_x;
				ebp->cur_delta = -1;    /* .founda */
				goto reta;
			}
			ecx -= t;
			edx += t;
			if (!editor_step_forward(ebp, &esi, &ebx)) goto notfound;
			if (!ecx) goto found;
			continue;
		}
		edx++;
		ecx--;
		if (!editor_step_forward(ebp, &esi, &ebx)) break;
	}
notfound:
	if (!esi) {
		esi = BLK(ebp, ebp->last_block);
		ebx = HDR(esi)->limit;
	}
	ebp->cur_delta = ecx;
	al = 0;
	goto reta;
found:
	al = 0;
	ebp->cur_delta = -1;                    /* .founda */
reta:
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
	return al;
}

/* editor.inc: editor_down_scroll: the view ecx lines down */
static void editor_down_scroll(editor_data *ebp, u32 ecx)
{
	int edx = LINESIZE;
	u8 *base = linedata_start(ebp);
	memmove(base, base + ecx * edx, (max_height - ecx) * edx);
	u8 *edi = base + (max_height - ecx) * edx;
	u8 *esi = ((editor_line *)(edi - edx))->block;
	u32 ebx = ((editor_line *)(edi - edx))->offs;
	while (ecx--) {
		editor_find_newline(ebp, &esi, &ebx);
		editor_set_line(edi, esi, ebx);
		edi += edx;
	}
}

/* editor.inc: editor_end_scroll: the cursor to the end of its line;
 * true (CF) if the view moved */
static bool editor_end_scroll(editor_data *ebp)
{
	u8 *edi, *esi;
	u32 ebx;
	editor_curline_start(ebp, &edi, &esi, &ebx);
	/* calculate visible length of the line (it differs from the real
	 * length if tabulations are present) */
	u32 ecx = 0;
	if (ebx != HDR(esi)->limit) {
		for (;;) {
			/* .calcloop */
			u8 al = esi[ebx];
			if (al == 10 || al == 13) break;
			if (al == 9) ecx += editor_tabsize - ecx % editor_tabsize;   /* .calctab */
			else ecx++;
			/* .calcnext */
			if (!editor_step_forward(ebp, &esi, &ebx)) break;
		}
	}
	/* .calcdone */
	if (!esi) {
		esi = BLK(ebp, ebp->last_block);
		ebx = HDR(esi)->limit;
	}
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
	ebp->cur_delta = 0;
	/* ecx = number of symbols in the line; calculate new position of
	 * view range */
	u32 eax = ebp->curcol;
	if (ecx < eax) {
		/* .toleft */
		ebp->curcol = ecx;
		ebp->cursor_x = 0;
		cursor_x = 0;
		return true;
	}
	if (ecx >= eax + cur_width) {
		/* .toright */
		eax = cur_width - 1;
		ebp->curcol = ecx - eax;
		ebp->cursor_x = eax;
		cursor_x = eax;
		return true;
	}
	ebp->cursor_x = ecx - ebp->curcol;
	cursor_x = ebp->cursor_x;
	return false;
}

/* editor.inc: editor_move_linestart: in: esi = block, ebx = start
 * offset, ecx = delta (bytes inserted at esi:ebx) */
static void editor_move_linestart(editor_data *ebp, u8 *esi, u32 ebx, u32 ecx)
{
	for (int i = 0; i < max_height; i++) {
		/* .0 */
		editor_line *l = LINE(ebp, i);
		if (l->block != esi) continue;
		u32 eax = l->offs, nb;
		if (eax > ebx) nb = eax + ecx;  /* .2 */
		else if (eax < HDR(esi)->limit) continue;
		else nb = eax;
		/* .3 */
		u8 *s = esi;
		while (nb >= HDR(s)->limit && HDR(s)->next) {
			nb = nb - HDR(s)->limit + EditBlockStart;
			s = BLK(ebp, HDR(s)->next);
		}
		/* .4 */
		l->block = s;
		l->offs = nb;
	}
	/* .5: the cursor's block */
	if (BLK(ebp, ebp->cur_block) != esi) return;
	if (ebp->cur_offs > ebx) ebp->cur_offs += ecx;
	if (ebp->cur_offs >= HDR(esi)->limit && HDR(esi)->next) {
		ebp->cur_offs = ebp->cur_offs - HDR(esi)->limit + EditBlockStart;
		ebp->cur_block = HDR(esi)->next;
	}
}

/* editor.inc: editor_reserve_space (editor_reserve_symbol: ecx = 1):
 * inserts empty space into the chain of blocks; in: ecx = number of
 * symbols to add, esi = block, ebx = offset; out: esi:ebx at the space;
 * true (CF) on failure. The data may move: *pebp follows it */
static bool editor_reserve_space(editor_data **pebp, u8 **pesi, u32 *pebx, u32 ecx)
{
	editor_data *ebp = *pebp;
	u8 *esi = *pesi;
	u32 ebx = *pebx;
	u32 eax = HDR(esi)->limit + ecx;
	if (eax <= 4096) {
		u32 old = HDR(esi)->limit;
		HDR(esi)->limit = eax;
		memmove(esi + ebx + ecx, esi + ebx, old - ebx);
		editor_move_linestart(ebp, esi, ebx, ecx);
		return false;
	}
	/* .add */
	u32 gap = ecx;
	u32 total = HDR(esi)->limit - EditBlockStart + ecx;
	u32 n = total / EditBlockSize;          /* the new blocks */
	if (n > ebp->numfree) {
		u32 add = n - ebp->numfree;
		u32 edx = ebp->memsize;
		u32 esi_rel = REL(ebp, esi);
		editor_data *ne = xpgrealloc(ebp, (add + edx) << 12);
		if (!ne) return true;           /* .nomem */
		editor_moved(ebp, ne);
		ebp = *pebp = ne;
		esi = BLK(ebp, esi_rel);
		ebp->memsize += add;
		edx <<= 12;
		ebp->numfree += add;
		u8 *edi = BLK(ebp, edx);
		u32 head = ebp->freeblocks;
		ebp->freeblocks = edx;
		for (u32 i = 0; i < add; i++) {
			edx += 0x1000;
			*(u32 *)edi = edx;
			edi += 0x1000;
		}
		*(u32 *)(edi - 0x1000) = head;
	}
	/* .norealloc */
	u32 oldnext = HDR(esi)->next;
	u8 *orig = esi;
	ebp->numfree -= n;
	u32 edi_rel = ebp->freeblocks;
	u8 *blk = esi;
	for (u32 i = 0; i < n; i++) {
		HDR(blk)->next = edi_rel;
		u8 *nb = BLK(ebp, edi_rel);
		u32 nextfree = *(u32 *)nb;
		/* next: filled later */
		HDR(nb)->prev = REL(ebp, blk);
		HDR(nb)->limit = 4096 - RESERVE_IN_BLOCK;
		total -= EditBlockSize;
		memset(nb + edit_block_header_size, 0, EditPlugInfo);
		edi_rel = nextfree;
		blk = nb;
	}
	ebp->freeblocks = edi_rel;
	u8 *edi = blk;
	esi = orig;
	HDR(edi)->next = oldnext;
	if (oldnext) HDR(BLK(ebp, oldnext))->prev = REL(ebp, edi);
	/* the data of the old block and of the space: between the old block
	 * and the last new one */
	ecx = total + EditBlockSize;
	u32 edx = ecx >> 1;
	eax = ecx - edx;
	if (eax >= (u32)EditBlockSize) {
		eax = EditBlockSize;
		edx = ecx - eax;
	}
	eax += EditBlockStart;
	edx += EditBlockStart;
	ecx = HDR(esi)->limit;
	HDR(esi)->limit = eax;
	HDR(edi)->limit = edx;
	ecx -= ebx;                             /* the bytes after the space */
	u32 c = ecx < edx - EditBlockStart ? ecx : edx - EditBlockStart;
	u32 tail = ecx - c;
	/* the end of them to the end of the last block, the rest to the end
	 * of the old block's new data */
	memcpy(edi + edx - c, esi + ebx + ecx - c, c);
	memmove(esi + eax - tail, esi + ebx, tail);
	if (ebx >= eax) memcpy(edi + EditBlockStart, esi + eax, ebx - eax);
	if (ebp->last_block == REL(ebp, esi)) ebp->last_block = REL(ebp, edi);
	editor_move_linestart(ebp, esi, ebx, gap);
	if (ebx >= HDR(esi)->limit) {
		ebx = ebx - HDR(esi)->limit + EditBlockStart;
		esi = BLK(ebp, HDR(esi)->next);
	}
	*pesi = esi;
	*pebx = ebx;
	return false;
}

/* editor.inc: editor_delete_space.delete_block: the block to the free
 * list */
static void editor_delete_space_delete_block(editor_data *ebp, u8 *esi)
{
	u32 eax = HDR(esi)->next, edx = HDR(esi)->prev;
	if (eax) HDR(BLK(ebp, eax))->prev = edx;
	else ebp->last_block = edx;
	if (edx) HDR(BLK(ebp, edx))->next = eax;
	else ebp->first_block = eax;
	*(u32 *)esi = ebp->freeblocks;
	ebp->freeblocks = REL(ebp, esi);
	ebp->numfree++;
}

/* editor.inc: editor_delete_space.collapse: (edi) + (esi) -> (edi);
 * returns edi */
static u8 *editor_delete_space_collapse(editor_data *ebp, u8 *edi, u8 *esi)
{
	u32 ecx = HDR(esi)->limit - EditBlockStart;
	u32 eax = HDR(edi)->limit;
	HDR(edi)->limit += ecx;
	memcpy(edi + eax, esi + EditBlockStart, ecx);
	/* the screen's first line follows its text */
	editor_line *l = LINE(ebp, 0);
	if (l->block == esi) {
		l->block = edi;
		l->offs = eax + l->offs - EditBlockStart;
	}
	editor_delete_space_delete_block(ebp, esi);
	return edi;
}

/* editor.inc: editor_delete_space.collapse_prev: returns the block the
 * text of esi is in */
static u8 *editor_delete_space_collapse_prev(editor_data *ebp, u8 *esi)
{
	u32 eax = HDR(esi)->prev;
	if (!eax) return esi;
	u8 *p = BLK(ebp, eax);
	u32 edx = HDR(esi)->limit - EditBlockStart + HDR(p)->limit;
	if (edx > 4096 - RESERVE_IN_BLOCK) return esi;
	return editor_delete_space_collapse(ebp, p, esi);
}

/* editor.inc: editor_delete_space.collapse_next */
static u8 *editor_delete_space_collapse_next(editor_data *ebp, u8 *esi)
{
	u32 eax = HDR(esi)->next;
	if (!eax) return esi;
	u8 *n = BLK(ebp, eax);
	u32 edx = HDR(esi)->limit - EditBlockStart + HDR(n)->limit;
	if (edx > 4096 - RESERVE_IN_BLOCK) return esi;
	return editor_delete_space_collapse(ebp, esi, n);
}

/* editor.inc: editor_delete_space (editor_delete_symbol: ecx = 1):
 * deletes ecx bytes of the text from esi:ebx */
static void editor_delete_space(editor_data *ebp, u8 *esi, u32 ebx, u32 ecx)
{
	u32 eax = HDR(esi)->limit - ebx;
	if (eax >= ecx) {
		HDR(esi)->limit -= ecx;
		memmove(esi + ebx, esi + ebx + ecx, eax - ecx);
	} else {
		/* .more1 */
		HDR(esi)->limit = ebx;
		ecx -= eax;
		esi = BLK(ebp, HDR(esi)->next);
		for (;;) {
			eax = HDR(esi)->limit - EditBlockStart;
			if (ecx < eax || !HDR(esi)->next) break;
			ecx -= eax;
			u32 next = HDR(esi)->next;
			editor_delete_space_delete_block(ebp, esi);
			esi = BLK(ebp, next);
		}
		if (ecx > eax) ecx = eax;
		u32 lim = HDR(esi)->limit;
		HDR(esi)->limit -= ecx;
		memmove(esi + EditBlockStart, esi + EditBlockStart + ecx, lim - EditBlockStart - ecx);
		esi = editor_delete_space_collapse_next(ebp, esi);
		esi = BLK(ebp, HDR(esi)->prev);
	}
	/* .done */
	esi = editor_delete_space_collapse_prev(ebp, esi);
	editor_delete_space_collapse_next(ebp, esi);
	editor_init_lines(ebp);
	editor_update_cur(ebp);
}

/* editor.inc: editor_cursor_left: true (CF) if a full redraw is needed */
static bool editor_cursor_left(editor_data *ebp)
{
	u8 *esi;
	u32 ebx;
	if (ebp->cur_delta != -1) {
		ebp->cur_delta--;
		if (ebp->cur_delta != -1) goto left_cursor;
	}
	/* .in_text */
	esi = BLK(ebp, ebp->cur_block);
	ebx = ebp->cur_offs;
	if (editor_step_backward(ebp, &esi, &ebx)) return false;
	u8 al = esi[ebx];
	if (al == 13 || al == 10) goto eol;     /* (.eol2 finds the CR of CR LF, which .eol does not use) */
	if (al == 9) goto tab;
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
left_cursor:
	if (ebp->cursor_x) {
		ebp->cursor_x--;
		cursor_x = ebp->cursor_x;       /* .set_cursor_x */
		return false;
	}
	ebp->curcol--;
	return true;
tab:
	if (ebp->cursor_x) {
		ebp->cursor_x--;
		cursor_x = ebp->cursor_x;
		return editor_update_cur(ebp) != 0;
	}
	ebp->curcol--;
	editor_update_cur(ebp);
	return true;
eol: {
		/* .eol */
		int eax = ebp->cursor_y - 1;
		bool full;
		if (!eax) {
			/* .scroll_left_toup */
			editor_up_scroll(ebp, 1);
			full = true;
		} else {
			full = false;   /* no full redraw */
			ebp->cursor_y = eax;
			cursor_y = eax;
		}
		/* .scroll_left_toend */
		bool cf = editor_end_scroll(ebp);
		return full || cf;
	}
}

/* editor.inc: editor_test_cursor_y: the cursor's line in the view;
 * true (CF) if it scrolled */
static bool editor_test_cursor_y(editor_data *ebp)
{
	u32 edx = cur_height - 2;
	u32 ecx = ebp->cursor_y;
	if (ecx > edx) {
		/* .scroll_down */
		ecx -= edx;
		ebp->curline += ecx;
		ebp->cursor_y -= ecx;
		cursor_y -= ecx;
		editor_down_scroll(ebp, ecx);
		return true;
	}
	ecx = ebp->curline + edx;
	if (ecx > ebp->numlines) {
		/* .scroll_up */
		ecx -= ebp->numlines;
		if (ecx >= ebp->curline) ecx = ebp->curline;
		if (!ecx) return false;
		ebp->cursor_y += ecx;
		cursor_y += ecx;
		editor_up_scroll(ebp, ecx);
		return true;
	}
	return false;
}

/* editor.inc: editor_OnKey.redraw_status */
static void editor_OnKey_redraw_status(editor_data *ebp)
{
	editor_draw_status(ebp);
	draw_image();
}

/* editor.inc: editor_OnKey.redraw_curline */
static void editor_OnKey_redraw_curline(editor_data *ebp)
{
	u8 *edi, *esi;
	u32 ebx;
	editor_curline_start(ebp, &edi, &esi, &ebx);
	editor_draw_line(ebp, esi, ebx, cursor_y);
}

/* editor.inc: editor_OnKey.symb_inserted */
static void editor_OnKey_symb_inserted(editor_data *ebp)
{
	editor_OnKey_redraw_curline(ebp);
	editor_OnKey_right(ebp, 0);
}

/* editor.inc: editor_OnKey.ret2 */
static void editor_OnKey_ret2(editor_data *ebp)
{
	editor_update_cur(ebp);
	editor_draw_text(ebp);
}

/* editor.inc: editor_OnKey.down_scroll */
static void editor_OnKey_down_scroll(editor_data *ebp)
{
	ebp->curline++;
	int eax = LINESIZE;
	u8 *edi = linedata_start(ebp);
	memmove(edi, edi + eax, eax * (max_height - 1));
	edi += eax * (max_height - 1);
	u8 *esi = ((editor_line *)(edi - eax))->block;
	u32 ebx = ((editor_line *)(edi - eax))->offs;
	editor_find_newline(ebp, &esi, &ebx);
	editor_set_line(edi, esi, ebx);
	editor_OnKey_ret2(ebp);
}

/* editor.inc: editor_OnKey.insert_newline: in: esi:ebx the cursor */
static void editor_OnKey_insert_newline(editor_data *ebp, u8 *esi, u32 ebx)
{
	u32 ecx = ebp->eol == edit_eol_dos ? 2 : 1;
	if (editor_reserve_space(&ebp, &esi, &ebx, ecx)) return;
	/* Inserting a newline just like that is risky: because of the
	 * two-byte line breaks, the old symbol may merge with the new one.
	 * That happens in two cases: the editor is set to the Unix style and
	 * the new line break goes right after a Mac line break; the editor is
	 * set to the Mac style and the new line break goes right before a
	 * Unix one. Then the line break is forced to the file's style. */
	u8 al = ebp->eol;
	if (al == edit_eol_dos) {
		/* .insert_eol_dos */
		esi[ebx] = 13;
		editor_step_forward(ebp, &esi, &ebx);
		esi[ebx] = 10;
	} else if (al == edit_eol_mac) {
		/* .insert_eol_mac: Mac-style for end-of-line */
		u8 c = 13;
		u8 *s = esi;
		u32 b = ebx;
		if (editor_step_forward(ebp, &s, &b) && s[b] == 10)
			c = 10;                 /* force Unix-style to avoid collapse */
		esi[ebx] = c;
	} else {
		/* Unix-style for end-of-line */
		u8 c = 10;
		u8 *s = esi;
		u32 b = ebx;
		if (!editor_step_backward(ebp, &s, &b) && s[b] == 13)
			c = 13;                 /* force Mac-style to avoid collapse */
		esi[ebx] = c;
	}
	/* .eol_correct */
	editor_step_forward(ebp, &esi, &ebx);
	if (!esi) {
		esi = BLK(ebp, ebp->last_block);
		ebx = HDR(esi)->limit;
	}
	ebp->cur_delta = 0;
	if (ebx != HDR(esi)->limit && esi[ebx] != 10 && esi[ebx] != 13) ebp->cur_delta--;
	/* the symbols are in the text; now the starts of the screen's lines */
	int cy = ebp->cursor_y, ls = LINESIZE;
	u8 *base = linedata_start(ebp);
	memmove(base + (cy + 1) * ls, base + cy * ls, (max_height - 1 - cy) * ls);
	editor_set_line(base + cy * ls, esi, ebx);
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
	/* one more line */
	ebp->numlines++;
	/* the cursor goes to the line's start */
	ebp->cursor_x = 0;
	ebp->curcol = 0;
	cursor_x = 0;
	/* and one line down */
	int eax = ebp->cursor_y + 1;
	if ((u32)eax >= (u32)(cur_height - 1)) {
		editor_OnKey_down_scroll(ebp);
		return;
	}
	ebp->cursor_y = eax;
	cursor_y = eax;
	editor_draw_text(ebp);
}

/* editor.inc: editor_OnKey.insert_after_eol: the spaces up to the
 * cursor beyond the line's end, then the symbol al */
static void editor_OnKey_insert_after_eol(editor_data *ebp, u8 *esi, u32 ebx, u8 al)
{
	u32 ecx = ebp->cur_delta + 1;
	if (editor_reserve_space(&ebp, &esi, &ebx, ecx)) return;
	u32 edx = ecx;
	u8 *edi;
	for (;;) {
		/* .2 */
		u32 n = HDR(esi)->limit - ebx;
		edi = esi + ebx;
		if (n >= edx) n = edx;
		ebx += n;
		edx -= n;
		memset(edi, ' ', n);
		edi += n;
		if (!edx) break;
		esi = BLK(ebp, HDR(esi)->next);
		ebx = EditBlockStart;
	}
	edi[-1] = al;
	ebx--;
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
	ebp->cur_delta = -1;
	editor_OnKey_symb_inserted(ebp);
}

/* editor.inc: editor_OnKey */
void editor_OnKey(void *p, u32 al)
{
	editor_data *ebp = p;
	if (al & 0x80) return;
	if (!(ebp->flags & 0x20)) {
		if (process_ctrl_keys(editor_ctrlkeys, al, ebp)) return;
	}
	/* .symbol */
	ebp->flags &= ~0x20;
	if (ebp->flags & 0x40) return;
	ebp->flags |= 0x80;
	u8 c = get_ascii_char(al & 0xff);
	u8 *esi = BLK(ebp, ebp->cur_block);
	u32 ebx = ebp->cur_offs;
	if (c == 10 || c == 13) {
		editor_OnKey_insert_newline(ebp, esi, ebx);
		return;
	}
	if (ebp->cur_delta != -1) {
		editor_OnKey_insert_after_eol(ebp, esi, ebx, c);
		return;
	}
	if (!(ebp->flags & 0x10)) {
		/* editor_reserve_symbol */
		if (editor_reserve_space(&ebp, &esi, &ebx, 1)) return;
	}
	/* .replace_symbol */
	if (ebx == HDR(esi)->limit) {
		esi = BLK(ebp, HDR(esi)->next);
		ebx = EditBlockStart;
	}
	esi[ebx] = c;
	editor_OnKey_symb_inserted(ebp);
}

/* editor.inc: editor_OnKey.exit */
static void editor_OnKey_exit(editor_data *ebp)
{
	editor_OnExit(ebp);
	delete_active_screen();
}

/* editor.inc: editor_OnKey.exit_confirm (Esc, F10) */
void editor_OnKey_exit_confirm(void *p, u32 eax)
{
	editor_data *ebp = p;
	if (ebp->flags & 0x80) {
		const char *s[1] = { aFileModified };
		int r = SayErrTitle(aEditorTitle, 1, s, 3, EditorExitBtn);
		if (r == 0) editor_save(ebp);   /* .save */
		else if (r != 1) return;
	}
	editor_OnKey_exit(ebp);
}

/* editor.inc: editor_OnKey.exit_save (Shift+F10) */
static void editor_OnKey_exit_save(void *p, u32 eax)
{
	editor_data *ebp = p;
	if (ebp->flags & 0x80) editor_save(ebp);        /* .save */
	editor_OnKey_exit(ebp);
}

/* editor.inc: editor_OnKey.f2 */
void editor_OnKey_f2(void *p, u32 eax)
{
	editor_save(p);
	editor_OnKey_redraw_status(p);
}

/* editor.inc: editor_OnKey.set_cursor_y */
static void editor_OnKey_set_cursor_y(editor_data *ebp, int eax)
{
	ebp->cursor_y = eax;
	cursor_y = eax;
	if (editor_update_cur(ebp)) editor_draw_text(ebp);
	else editor_OnKey_redraw_status(ebp);
}

/* editor.inc: editor_OnKey.up (.up_scroll) */
static void editor_OnKey_up(void *p, u32 a)
{
	editor_data *ebp = p;
	int eax = ebp->cursor_y - 1;
	if (eax) {
		editor_OnKey_set_cursor_y(ebp, eax);
		return;
	}
	/* .up_scroll */
	if (!ebp->curline) return;
	editor_up_scroll(ebp, 1);
	editor_draw_text(ebp);
}

/* editor.inc: editor_OnKey.down */
static void editor_OnKey_down(void *p, u32 a)
{
	editor_data *ebp = p;
	int eax = ebp->cursor_y + 1;
	if ((u32)eax + ebp->curline > ebp->numlines) return;
	if (eax != cur_height - 1) {
		editor_OnKey_set_cursor_y(ebp, eax);
		return;
	}
	editor_OnKey_down_scroll(ebp);
}

/* editor.inc: editor_OnKey.pgup */
static void editor_OnKey_pgup(void *p, u32 a)
{
	editor_data *ebp = p;
	u32 ecx = cur_height - 3;
	u32 old = ebp->curline;
	u32 edx = old >= ecx ? old - ecx : 0;
	ebp->curline = edx;
	u32 eax = old + ebp->cursor_y - 1;
	eax = eax >= ecx ? eax - ecx : 0;
	eax = eax - edx + 1;
	ebp->cursor_y = eax;
	cursor_y = eax;
	u32 n = old - edx;
	int ls = LINESIZE;
	u8 *base = linedata_start(ebp);
	memmove(base + n * ls, base, (max_height - n) * ls);
	if (n) {
		u8 *edi = base + n * ls;
		u8 *esi = ((editor_line *)edi)->block;
		u32 ebx = ((editor_line *)edi)->offs;
		while (n--) {
			editor_prev_newline(ebp, &esi, &ebx);
			edi -= ls;
			editor_set_line(edi, esi, ebx);
		}
	}
	editor_OnKey_ret2(ebp);
}

/* editor.inc: editor_OnKey.pgdn */
static void editor_OnKey_pgdn(void *p, u32 a)
{
	editor_data *ebp = p;
	u32 edx = cur_height - 2;
	u32 old = ebp->curline;
	u32 ecx = edx + old - 1;
	u32 eax = ebp->numlines >= edx ? ebp->numlines - edx : 0;
	if (ecx >= eax) ecx = eax;
	ebp->curline = ecx;
	eax = old + ebp->cursor_y + edx - 1;
	if (eax >= ebp->numlines) eax = ebp->numlines;
	eax -= ecx;
	ebp->cursor_y = eax;
	cursor_y = eax;
	editor_down_scroll(ebp, ecx - old);
	editor_OnKey_ret2(ebp);
}

/* editor.inc: editor_OnKey.left */
static void editor_OnKey_left(void *p, u32 a)
{
	editor_data *ebp = p;
	if (editor_cursor_left(ebp)) editor_draw_text(ebp);
	else editor_OnKey_redraw_status(ebp);
}

/* editor.inc: editor_OnKey.right */
static void editor_OnKey_right(void *p, u32 a)
{
	editor_data *ebp = p;
	u32 edx;
	if (ebp->cur_delta != -1) {
		if (ebp->curcol >= 0x80000000u - 0x1000) return;
		ebp->cur_delta++;
		edx = 1;
	} else {
		/* .right_in_text */
		u8 *esi = BLK(ebp, ebp->cur_block);
		u32 ebx = ebp->cur_offs;
		u8 al = esi[ebx];
		editor_step_forward(ebp, &esi, &ebx);
		if (!esi) {
			/* .right_eol0 */
			esi = BLK(ebp, ebp->last_block);
			ebx = HDR(esi)->limit;
			ebp->cur_delta++;
		} else if (esi[ebx] == 10 || esi[ebx] == 13) {
			ebp->cur_delta++;       /* .right_eol */
		}
		/* .right_ok */
		ebp->cur_block = REL(ebp, esi);
		ebp->cur_offs = ebx;
		edx = 1;
		if (al == 9) {
			u32 eax = ebp->curcol + ebp->cursor_x;
			edx = editor_tabsize - eax % editor_tabsize;
		}
	}
	/* .right_char */
	u32 eax = ebp->cursor_x + edx;
	if (eax < (u32)cur_width) {
		/* .set_cursor_x */
		ebp->cursor_x = eax;
		cursor_x = eax;
		editor_OnKey_redraw_status(ebp);
		return;
	}
	eax += ebp->curcol;
	edx = cur_width - 1;
	eax -= edx;
	ebp->cursor_x = edx;
	cursor_x = edx;
	ebp->curcol = eax;
	editor_draw_text(ebp);
}

/* editor.inc: editor_OnKey.home */
static void editor_OnKey_home(void *p, u32 a)
{
	editor_data *ebp = p;
	u8 *edi, *esi;
	u32 ebx;
	editor_curline_start(ebp, &edi, &esi, &ebx);
	ebp->cur_delta = 0;
	if (ebx != HDR(esi)->limit && esi[ebx] != 10 && esi[ebx] != 13) ebp->cur_delta--;
	ebp->cur_block = REL(ebp, esi);
	ebp->cur_offs = ebx;
	ebp->cursor_x = 0;
	cursor_x = 0;
	u32 eax = ebp->curcol;
	ebp->curcol = 0;
	if (eax) editor_draw_text(ebp);
	else editor_OnKey_redraw_status(ebp);
}

/* editor.inc: editor_OnKey.end */
static void editor_OnKey_end(void *p, u32 a)
{
	editor_data *ebp = p;
	if (editor_end_scroll(ebp)) editor_draw_text(ebp);
	else editor_OnKey_redraw_status(ebp);
}

/* editor.inc: editor_OnKey.ins: insert / replace */
static void editor_OnKey_ins(void *p, u32 a)
{
	editor_data *ebp = p;
	ebp->flags ^= 0x10;
	cursor_size = ebp->flags & 0x10 ? cursor_big_size : cursor_normal_size;
	draw_image();
}

/* editor.inc: editor_OnKey.del_char: in: esi:ebx the symbol; full: a
 * full redraw is needed */
static void editor_OnKey_del_char(editor_data *ebp, u8 *esi, u32 ebx, bool full)
{
	ebp->flags |= 0x80;
	u8 al = esi[ebx];
	u32 ecx = 1;
	if (al == 13) {
		u8 *s = esi;
		u32 b = ebx;
		if (editor_step_forward(ebp, &s, &b) && s[b] == 10) ecx++;
	}
	editor_delete_space(ebp, esi, ebx, ecx);
	if (al != 13 && al != 10) {
		if (!full) {
			editor_OnKey_redraw_curline(ebp);
			editor_OnKey_redraw_status(ebp);
			return;
		}
	} else {
		ebp->numlines--;
		editor_test_cursor_y(ebp);
	}
	/* .del1 */
	editor_draw_text(ebp);
}

/* editor.inc: editor_OnKey.backspace */
static void editor_OnKey_backspace(void *p, u32 a)
{
	editor_data *ebp = p;
	if (ebp->cur_delta != -1 && ebp->cur_delta != 0) {
		editor_OnKey_left(ebp, a);
		return;
	}
	if (ebp->flags & 0x40) return;
	u8 *esi = BLK(ebp, ebp->cur_block);
	u32 ebx = ebp->cur_offs;
	if (editor_step_backward(ebp, &esi, &ebx)) return;
	bool full = editor_cursor_left(ebp);
	if (esi[ebx] == 10) {
		u8 *s = esi;
		u32 b = ebx;
		if (!editor_step_backward(ebp, &s, &b) && s[b] == 13) {
			esi = s;
			ebx = b;
		}
	}
	editor_OnKey_del_char(ebp, esi, ebx, full);
}

/* editor.inc: editor_OnKey.del */
static void editor_OnKey_del(void *p, u32 a)
{
	editor_data *ebp = p;
	if (ebp->flags & 0x40) return;
	u8 *esi = BLK(ebp, ebp->cur_block);
	u32 ebx = ebp->cur_offs;
	if (!editor_normalize_offs(ebp, &esi, &ebx)) return;
	editor_OnKey_del_char(ebp, esi, ebx, false);
}

/* editor.inc: editor_OnKey.f7 */
void editor_OnKey_f7(void *ebp, u32 eax)
{
	if (find_in_file_dlg()) editor_OnKey_shift_f7(ebp, eax);
}

/* editor.inc: editor_OnKey.shift_f7: search string SearchString in file
 * starting from current cursor position */
void editor_OnKey_shift_f7(void *p, u32 a)
{
	editor_data *ebp = p;
	if (!SearchString[0]) return;
	const u8 *table = find_in_file_dlgdata.case_.flags & 0x10 ? identical_table : tolower_table;
	int whole = (find_in_file_dlgdata.whole.flags & 0x10) != 0;
	u32 ecx;
	/* the encoding: always cp866 for now; needs to be revisited after
	 * Unicode support in editor */
	u8 *edx = search_string_pre(SearchString, table, ebp->encoding, whole, &ecx);
	if (!edx) return;
	u8 *esi = BLK(ebp, ebp->cur_block);
	u32 ebx = ebp->cur_offs;
	if (!editor_normalize_offs(ebp, &esi, &ebx)) {
		search_failed(edx);             /* .f7.notfound */
		return;
	}
	/* the start of the current line, the delta in lines, the row */
	u8 *ls_esi = esi;
	u32 ls_ebx = ebx, delta = 0, row = 0;
	u32 state = 0;
	u8 *edi;
	u8 al;
	if (!whole) {
		al = esi[ebx];
		goto search_loop_next;
	}
	edi = edx;
	for (;;) {
		/* .search_loop: edx -> FSM, ecx = last state, esi:ebx ->
		 * current data, edi = current state */
		al = esi[ebx];                  /* get current symbol */
		state = edi[al];                /* calculate next state */
		if (state == ecx) goto found;   /* done? */
	search_loop_next:
		/* no; proceed to next symbol */
		ebx++;
		edi = edx + (state << 8);
		row++;
		if (ebx >= HDR(esi)->limit) {
			/* .f7.nextblock */
			if (!editor_normalize_offs(ebp, &esi, &ebx)) goto notfound_pop;
		}
		/* .f7.nonextblock */
		if (al != 10) {
			if (al != 13) continue;
			if (esi[ebx] == 10 && !editor_step_forward(ebp, &esi, &ebx)) goto notfound_pop;
		}
		/* .f7.newline */
		row = 0;
		delta++;
		ls_esi = esi;
		ls_ebx = ebx;
	}
notfound_pop:
	/* last chance - if we are looking for a whole word, EOF is ok for
	 * last symbol */
	if (whole) {
		esi = BLK(ebp, ebp->last_block);
		ebx = HDR(esi)->limit;
		state = edi[' '];
		if (state == ecx) goto found;
	}
	search_failed(edx);
	return;
found: {
		/* .f7.found */
		ebp->cur_delta = -1;
		s32 sb = ebx - ecx + 1;
		if (whole) sb++;
		while (sb < EditBlockStart) {
			sb -= EditBlockStart;
			esi = BLK(ebp, HDR(esi)->prev);
			sb += HDR(esi)->limit;
		}
		ebp->cur_block = REL(ebp, esi);
		ebp->cur_offs = sb;
		pgfree(edx);
		/* esi:ebx -> the line's start, row, delta in lines */
		esi = ls_esi;
		ebx = ls_ebx;
		if (whole) ecx--;
		u32 n = row - ecx + 1;
		u32 eax = ebp->curcol + ebp->cursor_x;
		if (delta) eax = 0;
		{
			u8 *s = esi;
			u32 b = ebx;
			for (; n; n--) {
				/* .f7.findpos1 */
				if (s[b] == 9) eax += editor_tabsize - eax % editor_tabsize;
				else eax++;
				editor_step_forward(ebp, &s, &b);
			}
		}
		/* .f7.foundpos1 */
		u32 redraw = 0;
		if (eax < ebp->curcol) {
			/* .f7.scrollleft */
			redraw++;
			ebp->curcol = eax;
		} else if (eax - ebp->curcol >= (u32)cur_width) {
			redraw++;
			ebp->curcol += eax - ebp->curcol - cur_width + 1;
		}
		/* .f7.xset */
		ebp->cursor_x = eax - ebp->curcol;
		cursor_x = ebp->cursor_x;
		u32 y = delta + ebp->cursor_y;
		if (y + 1 < (u32)cur_height) {
			ebp->cursor_y = y;
			cursor_y = y;
		} else {
			/* .f7.newview */
			y--;
			LINE(ebp, 0)->block = esi;
			LINE(ebp, 0)->offs = ebx;
			ebp->curline += y;
			ebp->cursor_y = 1;
			cursor_y = 1;
			editor_init_lines(ebp);
			editor_test_cursor_y(ebp);
			redraw++;
		}
		/* .f7.yset */
		if (!redraw) editor_OnKey_redraw_status(ebp);
		else editor_draw_text(ebp);
	}
}

/* editor.inc: editor_OnExit */
void editor_OnExit(void *p)
{
	editor_data *ebp = p;
	if (ebp->hPlugin) close_handle_if_unused(ebp->hPlugin, ebp->hFile);
}

/* viewer.inc: editor_getname ("Edit" and the file's name) */
void editor_getname(void *ebp, char *edi, int ecx)
{
	memcpy(edi, "Edit         ", 13);
	viewedit_getname(ebp, edi + 13, ecx);
}

/* viewer.inc: editor_IsHandleUsed (= viewer_IsHandleUsed) */
bool editor_IsHandleUsed(void *p, void *hPlugin, void *hFile)
{
	editor_data *ebp = p;
	return hPlugin == ebp->hPlugin && hFile == ebp->hFile;
}

/* editor.inc: find_in_file_dlg: the dialog of F7 (the viewer's and the
 * editor's); true if the user chose [ Search ] */
bool find_in_file_dlg(void)
{
	FindInFileDlgdata *ebx = &find_in_file_dlgdata;
	int eax = cur_width - 12;
	ebx->h.dialog.width = eax;
	eax -= 2;
	ebx->efind.x2 = eax;
	eax = (u32)eax >> 1;
	eax -= 2;
	ebx->search.x2 = eax;
	eax -= aSearchBLength - 1;
	ebx->search.x1 = eax;
	eax += aSearchBLength + 3;
	ebx->cnl.x1 = eax;
	eax += aCancelBLength - 1;
	ebx->cnl.x2 = eax;
	*(u8 *)&ebx->efind.flags = 0xC;
	ebx->search.flags &= ~4;
	ebx->cnl.flags &= ~4;
	ebx->case_.flags &= ~4;
	ebx->whole.flags &= ~4;
	return DialogBox(ebx) == (int)&ebx->search;
}
