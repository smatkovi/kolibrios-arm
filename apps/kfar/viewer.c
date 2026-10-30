/* kfar for KolibriOS for ARM: the viewer (F3): text and hex modes, the
 * wrap, the encodings, the search in the file.
 *
 * Translated to C from kfar's viewer.inc, diamond (Evgeny Grechnikov)
 * and the KolibriOS team, GPL-2.0.
 *
 * Differences from the original:
 * - files on plugin panels: as the original's (the plugin's getattr,
 *   open, read, setpos, close; hostname the host file's name);
 * - the percentage in the status line is the FPU's result computed with
 *   integers (viewer_percent);
 * - viewer_get_prev_char goes back to the right place when the buffer
 *   starts below 8192 (the original adds 8192 after reading 0-8191);
 * - viewedit_draw_filename and viewedit_getname stop at the end of a
 *   name with fewer than three '/' (/sys/file), viewedit_getname at the
 *   end of the F12 menu's line;
 * - search_failed quotes the string in a buffer of its own. */
#include "kfar.h"

typedef struct viewer_data {
	void *hPlugin;
	void *hFile;
	u64 buf_start;
	u64 cur_pos;
	u64 filesize;
	u8 *buf_pos;
	u32 buf_size;
	u64 col;
	void *hWorkFile;
	u8 encoding;
	u8 flags;               /* & 1: hex mode, & 2: unwrap, & 4: big-endian unicode */
	u8 bEofReached;
	u8 pad1;
	u64 selected_start;
	u64 selected_len;
	u8 pad2[0x200 - 72];    /* align 200h */
	char filename[1024];
	char hostname[1024];
	u8 buf[16384];
} viewer_data;

_Static_assert(offsetof(viewer_data, hPlugin) == 0 && offsetof(viewer_data, hFile) == 4,
	       "viewer_IsHandleUsed/editor_IsHandleUsed");
_Static_assert(offsetof(viewer_data, filename) == 0x200 && offsetof(viewer_data, hostname) == 0x600,
	       "viewer_getname/editor_getname");

static void viewer_draw_text(viewer_data *ebp);
static void viewer_seek(viewer_data *ebp, u64 pos);
static void viewer_seek_to_current(viewer_data *ebp);
static void viewer_prev_newline(viewer_data *ebp);
static void viewer_set_keybar(viewer_data *ebp);

/* viewer.inc: viewer_clear_selection */
static void viewer_clear_selection(viewer_data *ebp)
{
	ebp->selected_start = 0;
	ebp->selected_len = 0;
}

/* viewer.inc: ask_retry_ignore: the read error eax; 0 = retry */
static int ask_retry_ignore(viewer_data *ebp, u32 eax)
{
	const char *s[3] = { get_error_msg(eax), aCannotReadFile, ebp->filename };
	return SayErr(3, s, 2, RetryOrIgnoreBtn);
}

/* viewer.inc: view_file */
void view_file(int eax, const char *esi, const u8 *ecx)
{
	viewer_data *ebp = new_screen(sizeof(viewer_data), &viewer_vtable);
	if (!ebp) return;
	char *edi = ebp->filename;
	if (eax) {
		ebp->hPlugin = 0;
		ebp->hFile = 0;
		ebp->hostname[0] = 0;
		strcpy(edi, esi);
	} else {
		/* .frompanel */
		PanelData *panel = PANEL_OF_DIR(esi);
		ebp->hPlugin = panel->hPlugin;
		if (ebp->hPlugin) {
			/* the host file's name, its last part */
			const char *h = panel_host_name(panel), *t = strrchr(h, '/');
			strcpy(ebp->hostname, t ? t + 1 : h);
		}
		/* .nocopyhostname */
		ebp->hFile = panel->hFile;
		while (*esi) *edi++ = *esi++;
		if (edi[-1] != '/') *edi++ = '/';
		strcpy(edi, bdfe_name(ecx));
	}
	/* .namecopied */
	ebp->encoding = encodings_cp866;
	ebp->flags = 0;
	ebp->col = 0;
	viewer_clear_selection(ebp);
	attrinfo.name = ebp->filename;
	readinfo.name = ebp->filename;
	fileinfo *ebx = &attrinfo;
	u32 err;
attr_retry:
	if (ebp->hPlugin)
		err = PANEL_PLUGIN(ebp)->getattr(ebp->hFile, ebp->filename, attrinfo_attr);
	else
		err = fs_call(ebx, 0);          /* .attr_native */
	if (err) {
		const char *s[3] = { get_error_msg(err), aCannotReadFile, ebp->filename };
		if (SayErr(3, s, 2, RetryOrCancelBtn) == 0) {
			ebx = &attrinfo;
			goto attr_retry;
		}
		delete_active_screen();
		return;
	}
	ebp->filesize = *(const u32 *)(attrinfo_attr + 32) | (u64)*(const u32 *)(attrinfo_attr + 36) << 32;
	readinfo.first = readinfo.first_hi = 0;
	ebp->buf_start = 0;
	ebp->cur_pos = 0;
	readinfo.size = 16384;
	readinfo.data = ebp->buf;
	ebp->buf_pos = ebp->buf;
	if (ebp->hPlugin) {
		ebp->hWorkFile = PANEL_PLUGIN(ebp)->open(ebp->hFile, ebp->filename, O_READ);
		if (!ebp->hWorkFile) {
			/* ..openerr_in_screen */
			SayErr(1, aCannotOpenFile_ptr, 1, ContinueBtn);
			delete_active_screen();
			return;
		}
	}
	/* .retry */
	u32 nread;
	if (ebp->hPlugin) {
		nread = PANEL_PLUGIN(ebp)->read(ebp->hWorkFile, readinfo.data, 16384);
		/* -1: let's hope that plugin says error itself */
		if (nread == (u32)-1) nread = 0;
		ebp->buf_size = nread;
		err = 0;
	} else {
		/* .read_native */
		err = fs_call(&readinfo, &nread);
		ebp->buf_size = nread;
	}
	if (err && err != 6) {
		/* .readerr */
		const char *s[3] = { get_error_msg(err), aCannotReadFile, ebp->filename };
		if (SayErr(3, s, 2, RetryOrCancelBtn) == 0) {
			ebx = &readinfo;
			goto attr_retry;
		}
		delete_active_screen();
		return;
	}
	/* .readok */
	if (nread >= 2) {
		u16 w = ebp->buf[0] | ebp->buf[1] << 8;
		if (w == 0xFEFF || w == 0xFFFE) {
			if (w == 0xFFFE) ebp->flags |= 4;
			ebp->encoding = encodings_unicode;
		}
	}
	/* .nounicode */
	viewer_OnRedraw(ebp);
}

/* viewer.inc: viewer_OnRedraw */
void viewer_OnRedraw(void *p)
{
	viewer_data *ebp = p;
	cursor_x = -1;
	cursor_y = -1;
	viewer_set_keybar(ebp);
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_skip_unicode_marker */
static void viewer_skip_unicode_marker(viewer_data *ebp)
{
	if (ebp->encoding != encodings_unicode || bForHex || ebp->buf_start || ebp->buf_pos != ebp->buf)
		return;
	u16 w = ebp->buf[0] | ebp->buf[1] << 8;
	if (w == 0xFFFE || w == 0xFEFF)
		ebp->buf_pos += 2;
}

/* viewer.inc: viewer_load_next: the buffer's second half to the first,
 * the next 8 KiB after it; false (ZF) if nothing more was read */
static bool viewer_load_next(viewer_data *ebp)
{
	ebp->buf_start += 8192;
	u64 first = ebp->buf_start + (16384 - 8192);
	readinfo.first = first;
	readinfo.first_hi = first >> 32;
	readinfo.size = 8192;
	memcpy(ebp->buf, ebp->buf + 8192, 16384 - 8192);
	readinfo.data = ebp->buf + 8192;
	readinfo.name = ebp->filename;
	u32 ebx;
	for (;;) {                              /* .readretry */
		if (ebp->hPlugin) {
			ebx = PANEL_PLUGIN(ebp)->read(ebp->hWorkFile, readinfo.data, 8192);
			if (ebx == (u32)-1) ebx = 0;
			break;
		}
		/* .native */
		u32 eax = fs_call(&readinfo, &ebx);
		if (eax == 0 || eax == 6) break;
		if (ask_retry_ignore(ebp, eax)) break;  /* .readerr */
	}
	/* .readok */
	if (ebx > 8192) ebx = 0;                /* (an error ignored) */
	ebp->buf_pos -= 8192;
	ebx += 16384 - 8192;
	ebp->buf_size = ebx;
	return ebx != 16384 - 8192;
}

/* viewer.inc: uni2ansi_char: a UTF-16 character in cp866 (the Russian
 * letters), ' ' for the others */
static u8 uni2ansi_char(viewer_data *ebp, u16 ax)
{
	if (ebp->flags & 4) ax = ax >> 8 | ax << 8;
	if (ax < 0x80) return ax;
	if (ax == 0x401) return 0xF0;           /* .yo1: Ё */
	if (ax == 0x451) return 0xF1;           /* .yo2: ё */
	if (ax >= 0x410 && ax < 0x440) return (u8)ax + 0x70;   /* .rus1: 0x410-0x43F -> 0x80-0xAF */
	if (ax >= 0x440 && ax < 0x450) return (u8)ax + 0xA0;   /* .rus2: 0x440-0x44F -> 0xE0-0xEF */
	return ' ';                             /* .unk */
}

/* viewer.inc: read_unicode_char */
static u8 read_unicode_char(viewer_data *ebp, const u8 *eax)
{
	return uni2ansi_char(ebp, eax[0] | eax[1] << 8);
}

/* viewer.inc: viewer_get_next_char: the character at buf_pos into al
 * (with bForHex in Unicode: the 16-bit unit into ax); true (CF) at the
 * end of the file, eax unchanged */
static bool viewer_get_next_char(viewer_data *ebp, u32 *eax)
{
	viewer_skip_unicode_marker(ebp);
	u8 *p = ebp->buf_pos;
	if (p >= ebp->buf + ebp->buf_size) {
		if (ebp->buf_size < 16384) return true;         /* .err */
		if (!viewer_load_next(ebp)) return true;
		p = ebp->buf_pos;
	}
	/* .buffered */
	if (ebp->encoding == encodings_unicode) {
		ebp->buf_pos++;
		if (!bForHex)
			*eax = (*eax & ~0xffu) | read_unicode_char(ebp, p);
		else
			*eax = (*eax & ~0xffffu) | p[0] | p[1] << 8;
	} else {
		*eax = (*eax & ~0xffu) | *p;
	}
	/* .done */
	ebp->buf_pos++;
	return false;
}

/* viewer.inc: viewer_get_prev_char: the character before buf_pos into
 * al; true (CF) at the start of the file */
static bool viewer_get_prev_char(viewer_data *ebp, u32 *eax)
{
	if (ebp->buf_pos <= ebp->buf) {
		if (!ebp->buf_start) return true;
		u64 old = ebp->buf_start;
		u64 t = old >= 8192 ? old - 8192 : 0;
		viewer_seek(ebp, t);
		ebp->buf_pos += old - t;        /* the original: 8192 */
	}
	/* .buffered */
	u8 *p = ebp->buf_pos - 1;
	if (ebp->encoding == encodings_unicode) {
		p--;
		ebp->buf_pos = p;
		*eax = (*eax & ~0xffu) | read_unicode_char(ebp, p);
	} else {
		ebp->buf_pos = p;
		*eax = (*eax & ~0xffu) | *p;
	}
	return false;
}

/* the percentage of the status line: kfar computes cur_pos / filesize *
 * 100 with the FPU, rounding down (fpu_cw: 64-bit precision, RC = 01) at
 * each step; the result is the exact floor, or one less when the exact
 * value is an integer whose hundredth has no exact binary fraction
 * (cur_pos = filesize / 5 shows 19) */
static u32 viewer_percent(u64 c, u64 f)
{
	if (!f) return 100;                     /* .size_zero */
	if (c > (u64)-1 / 100) return f >= 100 ? c / (f / 100) : 100;
	u64 n = c * 100, q = n / f;
	if (n % f == 0 && q % 25) q--;
	return q;
}

/* viewer.inc: viewer_draw_text.write_char: the character al at the
 * column scrpos, if it is on the screen (the arrows '<' and '>' at the
 * edges) */
static void write_char(viewer_data *ebp, u16 **edi, u8 al)
{
	if (scrpos < ebp->col) {
		/* .left */
		if (!scrpos) *(*edi)++ = CELL('<', view_arrows_color);
	} else {
		u64 d = scrpos - ebp->col;
		if (!(d >> 32) && (d || !scrpos)) {
			u32 ecx = d;
			if (ecx < (u32)cur_width) {
				/* .do */
				u8 ah = view_normal_color;
				u64 off = (u64)(u32)(ebp->buf_pos - ebp->buf - 1) + ebp->buf_start;
				if (off - ebp->selected_start < ebp->selected_len)
					ah = view_selected_color;
				*(*edi)++ = CELL(al, ah);
			} else if (ecx == (u32)cur_width) {
				(*edi)[-1] = CELL('>', view_arrows_color);
			}
		}
	}
	/* .skip */
	scrpos++;
}

/* viewer.inc: viewer_draw_text.hex_digit, .safechar: a cell at edi if
 * it is before the end of the line ebx */
static void safechar(u16 **edi, u16 *ebx, u8 al)
{
	if (*edi < ebx) *(*edi)++ = CELL(al, view_normal_color);
}

static void hex_digit(u16 **edi, u16 *ebx, u8 al)
{
	safechar(edi, ebx, al < 10 ? '0' + al : 'A' + al - 10);
}

/* viewer.inc: viewer_draw_text: the status line, then the text or the
 * hex dump from cur_pos */
static void viewer_draw_text(viewer_data *ebp)
{
again:
	viewer_seek_to_current(ebp);
	u16 *edi = get_console_ptr(0, 0);
	u8 ah = view_status_color;
	int ecx = cur_width - 44;
	if ((u32)ecx < 20) ecx = 20;
	edi = viewedit_draw_filename(ebp, edi, &ecx, ah);
	for (ecx += 11; ecx > 0; ecx--) *edi++ = CELL(' ', ah);
	/* the encoding, right-aligned before edi */
	{
		const char *names = encodings_names[ebp->encoding];
		int s = 8;
		u16 *d = edi - 1;
		while (names[--s] == ' ');
		for (; s >= 0; s--) *d-- = CELL(names[s], ah);
	}
	for (int i = 0; i < 13; i++) *edi++ = CELL(' ', ah);
	/* the file's size, its last digit at edi */
	{
		u16 *d = edi;
		u64 v = ebp->filesize;
		do {
			*d-- = CELL('0' + v % 10, view_status_color);
			v /= 10;
		} while (v);
		*d = CELL(' ', view_status_color);
	}
	edi++;
	for (int i = 0; i < 5; i++) *edi++ = CELL(' ', ah);
	*edi++ = CELL('C', ah);
	*edi++ = CELL('o', ah);
	*edi++ = CELL('l', ah);
	*edi++ = CELL(' ', ah);
	u16 *colstart = edi;
	if (ebp->col >= 100000) {
		/* .col_big: "..." and the last two digits */
		u32 r = ebp->col % 100;
		*edi++ = CELL('.', view_status_color);
		*edi++ = CELL('.', view_status_color);
		*edi++ = CELL('.', view_status_color);
		*edi++ = CELL('0' + r / 10, view_status_color);
		*edi++ = CELL('0' + r % 10, view_status_color);
	} else {
		char buf[8];
		int n = 0;
		u32 v = ebp->col;
		do buf[n++] = '0' + v % 10; while ((v /= 10));
		while (n) *edi++ = CELL(buf[--n], view_status_color);
	}
	/* .col_done */
	while (edi < colstart + 10) *edi++ = CELL(' ', view_status_color);
	{
		u32 t = viewer_percent(ebp->cur_pos, ebp->filesize);
		u8 *c = (u8 *)(edi - 1);
		*c = '%';
		c -= 2;
		do {
			*c = '0' + t % 10;
			c -= 2;
		} while ((t /= 10));
	}
	edi = get_console_ptr(0, 1);
	int edx;
	bool cf;
	if (ebp->flags & 1) goto DrawHex;
	edx = 2;
	u32 col = 0;                            /* ecx: the column in the line */
	scrpos = 0;
	viewer_right_side = 0;
	u32 eax = 0;
	for (;;) {
_1:
		if (viewer_get_next_char(ebp, &eax)) {
			cf = true;
			goto done;
		}
_2:
		if ((u8)eax == 0xD || (u8)eax == 0xA || (col >= (u32)cur_width && !(ebp->flags & 2))) {
			/* .newline */
			u32 n;
			if (scrpos < ebp->col) {
				n = 0;                  /* .line_full */
			} else {
				u64 d = scrpos - ebp->col;
				if (d >> 32) goto line_filled;
				n = d;
				if (n > (u32)cur_width) goto line_filled;
			}
			if (!n && scrpos) n = 1;
			for (n = cur_width - n; n; n--) *edi++ = CELL(' ', view_normal_color);
line_filled:
			if (scrpos >= viewer_right_side) viewer_right_side = scrpos;
			col = 0;
			scrpos = 0;
			edx++;
			if (edx >= cur_height) {
				cf = false;
				goto done;
			}
		}
		/* .no_newline */
		if ((u8)eax == 0xD || (u8)eax == 0xA) {
			/* .3: CR LF and LF CR are one line end */
			u8 bl = eax;
			if (viewer_get_next_char(ebp, &eax)) {
				cf = true;
				goto done;
			}
			if ((bl ^ (u8)eax) == (0xA ^ 0xD)) goto _1;
			goto _2;
		}
		/* .4 */
		u8 al = eax;
		if ((al & 0x80) && ebp->encoding != encodings_unicode) {
			al = encodings_tables[ebp->encoding][al - 0x80];
			eax = al;
		}
		if (al == 9) {
			/* .tab */
			u32 n = viewer_tabsize - col % viewer_tabsize;
			u32 end = col + n;
			if (!(ebp->flags & 2) && end > (u32)cur_width) {
				n -= end - cur_width;
				end = cur_width;
			}
			do write_char(ebp, &edi, ' '); while (--n);
			col = end;
			continue;
		}
		write_char(ebp, &edi, al);
		col++;
		if (col & 0x80000000u) col %= viewer_tabsize;
	}
done:
	ebp->bEofReached = cf;
done2:
	edx++;
	if (edx < cur_height && ebp->cur_pos) {
		/* not enough lines to fill the screen: one line back */
		viewer_seek_to_current(ebp);
		viewer_prev_newline(ebp);
		goto again;
	}
	{
		u16 *end = get_console_ptr(0, cur_height - 1);
		while (edi < end) *edi++ = CELL(' ', view_normal_color);
	}
	if (ebp->bEofReached) {
		u16 *d = get_console_ptr(cur_width - 4, 0);
		d[0] = CELL('1', view_status_color);
		d[1] = CELL('0', view_status_color);
		d[2] = CELL('0', view_status_color);
	}
	draw_image();
	return;

DrawHex:
	{
		u32 esi = 0;                    /* the line */
		u16 *ebx;
		ebp->bEofReached = 0;
		u32 c = 0;
		for (;;) {
			/* .line */
			u16 *line = edi;
			for (int i = 0; i < cur_width; i++) *edi++ = CELL(' ', view_normal_color);
			ebx = edi;
			edi = line;
			bForHex = 1;
			cf = viewer_get_next_char(ebp, &c);
			bForHex = 0;
			if (cf) goto hexdone;
			{
				u16 *d = edi + 36;
				safechar(&d, ebx, 0xB3);
			}
			/* the offset: 10 hex digits */
			u64 off = ebp->cur_pos + ((u64)esi << 4);
			hex_digit(&edi, ebx, (off >> 36) & 0xF);
			hex_digit(&edi, ebx, (off >> 32) & 0xF);
			{
				u16 *d = edi + 7;
				u32 lo = off;
				for (int i = 0; i < 8; i++, lo >>= 4) {
					u16 *dd = d - i;
					safechar(&dd, ebx, (lo & 0xF) < 10 ? '0' + (lo & 0xF) : 'A' + (lo & 0xF) - 10);
				}
				edi = d - 8 + 9;
			}
			*edi++ = CELL(':', view_normal_color);
			*edi++ = CELL(' ', view_normal_color);
			u32 ecx = 0;
			for (;;) {
				if (ebp->encoding != encodings_unicode) {
					u8 al = c;
					hex_digit(&edi, ebx, al >> 4);
					hex_digit(&edi, ebx, al & 0xF);
					safechar(&edi, ebx, ' ');
					u16 *d = edi + 48 - 2 * ecx;
					if (ecx >= 8) d -= 2;
					if (al & 0x80) al = encodings_tables[ebp->encoding][al - 0x80];
					safechar(&d, ebx, al);
					ecx++;
					if (ecx == 8) {
						safechar(&edi, ebx, 0xB3);
						safechar(&edi, ebx, ' ');
					}
					if (ecx == 16) break;
				} else {
					/* .hexchar_unicode */
					u16 w = c;
					hex_digit(&edi, ebx, w >> 12);
					hex_digit(&edi, ebx, (w >> 8) & 0xF);
					hex_digit(&edi, ebx, (w & 0xFF) >> 4);
					hex_digit(&edi, ebx, w & 0xF);
					safechar(&edi, ebx, ' ');
					u16 *d = edi + 38 - 4 * ecx;
					if (ecx >= 4) d -= 2;
					safechar(&d, ebx, uni2ansi_char(ebp, w));
					ecx++;
					if (ecx == 4) {
						safechar(&edi, ebx, 0xB3);
						safechar(&edi, ebx, ' ');
					}
					if (ecx == 8) break;
				}
				/* .hexchar */
				bForHex = 1;
				cf = viewer_get_next_char(ebp, &c);
				bForHex = 0;
				if (cf) goto hexdone;
			}
			/* .hexchar_nextline */
			edi = ebx;
			esi += 3;
			edx = esi;
			if (esi >= (u32)cur_height) goto done2;
			esi -= 2;
		}
hexdone:
		ebp->bEofReached = 1;
		edi = ebx;
		edx = esi + 2;
		goto done2;
	}
}

/* viewer.inc: viewedit_draw_filename */
u16 *viewedit_draw_filename(void *ebp, u16 *edi, int *ecx_p, u8 ah)
{
	const char *esi = VIEWEDIT_FILENAME(ebp);
	u32 ecx = *ecx_p;
	int edx = VIEWEDIT_HPLUGIN(ebp) ? 1 : 3;
	u8 al;
	/* .5: the name up to its third '/' (the volume) */
	for (;;) {
		al = *esi++;
		if (al == '/' && !--edx) break;
		if (!al) {
			esi--;          /* fewer '/' (not in the original) */
			goto _7;
		}
		*edi++ = CELL(al, ah);
		if (!--ecx) break;
	}
	/* .6 */
	ecx--;
	*edi++ = CELL(al, ah);
	{
		/* the rest, with "..." in front if it does not fit */
		u32 len = strlen(esi);
		if (ecx < len) {
			*edi++ = CELL('.', ah);
			*edi++ = CELL('.', ah);
			*edi++ = CELL('.', ah);
			ecx -= 3;
			esi += len - ecx;
		}
	}
_7:
	while ((al = *esi++)) {
		ecx--;
		*edi++ = CELL(al, ah);
	}
	*ecx_p = ecx;
	return edi;
}

/* viewer.inc: viewer_seek_to_current */
static void viewer_seek_to_current(viewer_data *ebp)
{
	viewer_seek(ebp, ebp->cur_pos);
}

/* viewer.inc: viewer_seek.q: reads readinfo (on a plugin's panel its
 * setpos and read); the bytes read */
static u32 viewer_seek_q(viewer_data *ebp)
{
	if (ebp->hPlugin) {
		PluginInfo *eax = PANEL_PLUGIN(ebp);
		eax->setpos(ebp->hWorkFile, readinfo.first | (u64)readinfo.first_hi << 32);
		u32 ebx = eax->read(ebp->hWorkFile, readinfo.data, readinfo.size);
		return ebx == (u32)-1 ? 0 : ebx;       /* (the plugin has said why) */
	}
	for (;;) {
		u32 ebx;
		u32 eax = fs_call(&readinfo, &ebx);
		if (eax == 0 || eax == 6) return ebx;   /* .readok */
		if (ask_retry_ignore(ebp, eax))         /* .readerr */
			return ebx > readinfo.size ? 0 : ebx;
	}
}

/* viewer.inc: viewer_seek: the buffer at pos, buf_pos there */
static void viewer_seek(viewer_data *ebp, u64 pos)
{
	u64 d = pos - ebp->buf_start;
	u32 eax, ebx;
	if (pos < ebp->buf_start) {
		/* .prev */
		if ((u32)(d >> 32) != 0xFFFFFFFFu) goto read_full;
		u32 lo = (u32)d & ~511u;
		if (lo <= (u32)-16384) goto read_full;
		u32 k = -lo, ecx = lo + 16384;  /* the bytes kept */
		memmove(ebp->buf + k, ebp->buf, ecx);
		ebp->buf_start -= k;
		eax = k;
		if (ebp->buf_size >= ecx) ebp->buf_size = ecx;
		goto doread;
	}
	if (d < 16384) {
		ebp->buf_pos = ebp->buf + (u32)d;
		return;
	}
	/* .next */
	{
		u64 sh = (d | 511) - (16384 - 1);
		if (sh >> 32) goto read_full;
		eax = sh;
		if (eax >= 16384) goto read_full;
		if (ebp->buf_size < 16384) goto read_full;
		memmove(ebp->buf, ebp->buf + eax, 16384 - eax);
		ebp->buf_start += eax;
		readinfo.data = ebp->buf + 16384 - eax;
		u64 first = ebp->buf_start + (16384 - eax);
		readinfo.first = first;
		readinfo.first_hi = first >> 32;
		readinfo.size = eax;
		readinfo.name = ebp->filename;
		ebx = viewer_seek_q(ebp);
		ebx = ebx - readinfo.size + 16384;
		goto ret;
	}
read_full:
	{
		u64 s = pos & ~(u64)511;
		ebp->buf_start = s >= 8192 - 512 ? s - (8192 - 512) : 0;
		ebp->buf_size = 0;
		eax = 16384;
	}
doread:
	readinfo.first = ebp->buf_start;
	readinfo.first_hi = ebp->buf_start >> 32;
	readinfo.size = eax;
	readinfo.data = ebp->buf;
	readinfo.name = ebp->filename;
	ebx = viewer_seek_q(ebp);
	if (ebx == readinfo.size) ebx += ebp->buf_size;
ret:
	ebp->buf_size = ebx;
	ebp->buf_pos = ebp->buf + (u32)(pos - ebp->buf_start);
}

/* viewer.inc: viewer_set_curpos */
static void viewer_set_curpos(viewer_data *ebp)
{
	ebp->cur_pos = ebp->buf_start + (u32)(ebp->buf_pos - ebp->buf);
}

/* viewer.inc: viewer_next_newline: cur_pos to the next line on the
 * screen; true (CF) at the end of the file */
static bool viewer_next_newline(viewer_data *ebp)
{
	u32 eax = 0;
	if (ebp->flags & 1) {
		u32 ecx = ebp->encoding == encodings_unicode ? 8 : 16;
		do {
			if (viewer_get_next_char(ebp, &eax)) return true;
		} while (--ecx);
		viewer_set_curpos(ebp);
		return false;
	}
	/* .nohex */
	u32 ecx = 0;
	bool cf;
	for (;;) {
		if (viewer_get_next_char(ebp, &eax)) {
			cf = true;
			break;
		}
		u8 al = eax;
		if (al == 0xD || al == 0xA) {
			/* .1 */
			u8 cl = al;
			if (viewer_get_next_char(ebp, &eax)) {
				cf = true;
				break;
			}
			if ((cl ^ (u8)eax) != (0xD ^ 0xA)) {
				ebp->buf_pos--;
				if (ebp->encoding == encodings_unicode) ebp->buf_pos--;
			}
			cf = false;     /* .2 */
			break;
		}
		if (al == 9)            /* .tab */
			ecx += viewer_tabsize - ecx % viewer_tabsize;
		else
			ecx++;
		/* .next */
		if (ebp->flags & 2) continue;
		if (ecx <= (u32)cur_width) continue;
		viewer_get_prev_char(ebp, &eax);
		cf = false;             /* .2 */
		break;
	}
	/* .ret */
	viewer_set_curpos(ebp);
	return cf;
}

/* viewer.inc: viewer_prev_newline: cur_pos to the previous line on the
 * screen */
static void viewer_prev_newline(viewer_data *ebp)
{
	u32 eax = 0;
	if (ebp->flags & 1) {
		u32 ecx = ebp->encoding == encodings_unicode ? 8 : 16;
		do {
			if (viewer_get_prev_char(ebp, &eax)) break;
		} while (--ecx);
		viewer_set_curpos(ebp);
		return;
	}
	/* .nohex: back to the line's start, at most 8192 characters (a
	 * whole number of lines) */
	int edx = 8192 - 8192 % cur_width;
	if (viewer_get_prev_char(ebp, &eax)) goto _0;
	edx--;
	if ((u8)eax == 0xD || (u8)eax == 0xA) {
		/* .2 */
		u8 cl = eax;
		if (viewer_get_prev_char(ebp, &eax)) goto _0;
		edx--;
		if ((cl ^ (u8)eax) != (0xD ^ 0xA)) goto _3;
		goto next;
	}
_3:
	if ((u8)eax == 0xD || (u8)eax == 0xA) goto _1;
next:
	if (viewer_get_prev_char(ebp, &eax)) goto _0;
	if (!--edx) goto _0;
	goto _3;
_1:
	viewer_get_next_char(ebp, &eax);
_0:
	{
		/* the last line on the screen that starts before the old
		 * position */
		u64 old = ebp->cur_pos, start;
		viewer_set_curpos(ebp);
		for (;;) {
			start = ebp->cur_pos;
			if (viewer_next_newline(ebp)) break;
			if (ebp->cur_pos >= old) break;
		}
		/* .ret3 */
		ebp->cur_pos = start;
	}
	/* .ret */
	viewer_seek_to_current(ebp);
}

/* viewer.inc: viewer_set_keybar: F2, F4 and F8 as the mode is */
static void viewer_set_keybar(viewer_data *ebp)
{
	char *eax = keybar_viewer;
	memcpy(eax + (4 - 1) * 6, ebp->flags & 1 ? "Text" : "Hex ", 4);
	memcpy(eax + 6, ebp->flags & 2 ? "Wrap  " : "Unwrap", 6);
	/* F8: the other of cp866 and cp1251 */
	int esi = ebp->encoding == encodings_cp1251 ? 0 : 1;
	memcpy(keybar_cp, encodings_names[esi], 6);
	draw_keybar();
}

/* viewer.inc: viewer_OnExit: a plugin's file closed, the plugin too
 * when no screen uses it */
void viewer_OnExit(void *p)
{
	viewer_data *ebp = p;
	PluginInfo *edx = PANEL_PLUGIN(ebp);
	if (!edx) return;
	ebp->hPlugin = 0;
	edx->close(ebp->hWorkFile);
	close_handle_if_unused(edx, ebp->hFile);
}

/* viewer.inc: viewer_IsHandleUsed */
bool viewer_IsHandleUsed(void *p, void *hPlugin, void *hFile)
{
	viewer_data *ebp = p;
	return hPlugin == ebp->hPlugin && hFile == ebp->hFile;
}

/* viewer.inc: editor_IsHandleUsed, the same label (editor_data has
 * hPlugin and hFile where viewer_data has them); weak, editor.c may
 * have its own */
__attribute__((weak)) bool editor_IsHandleUsed(void *ebp, void *hPlugin, void *hFile)
{
	return viewer_IsHandleUsed(ebp, hPlugin, hFile);
}

/* viewer.inc: viewer_OnKey */
void viewer_OnKey(void *ebp, u32 al)
{
	process_ctrl_keys(viewer_ctrlkeys, al, ebp);
}

/* viewer.inc: viewer_OnKey.exit */
void viewer_OnKey_exit(void *ebp, u32 eax)
{
	viewer_OnExit(ebp);
	delete_active_screen();
}

/* viewer.inc: viewer_OnKey.down */
static void viewer_OnKey_down(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->bEofReached) return;
	viewer_seek_to_current(ebp);
	if (viewer_next_newline(ebp)) return;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.pgdn */
static void viewer_OnKey_pgdn(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->bEofReached) return;
	viewer_seek_to_current(ebp);
	for (int ebx = cur_height - 3; ebx; ebx--)
		if (viewer_next_newline(ebp)) return;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.up */
static void viewer_OnKey_up(void *p, u32 eax)
{
	viewer_data *ebp = p;
	viewer_seek_to_current(ebp);
	viewer_prev_newline(ebp);
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.pgup */
static void viewer_OnKey_pgup(void *p, u32 eax)
{
	viewer_data *ebp = p;
	viewer_seek_to_current(ebp);
	for (int ebx = cur_height - 3; ebx; ebx--)
		viewer_prev_newline(ebp);
	/* .done_redraw */
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.done_redraw_keybar */
static void viewer_OnKey_done_redraw_keybar(viewer_data *ebp)
{
	viewer_set_keybar(ebp);
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.f8: cp866 <-> cp1251 */
void viewer_OnKey_f8(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (--ebp->encoding) ebp->encoding = encodings_cp1251;
	viewer_OnKey_done_redraw_keybar(ebp);
}

/* viewer.inc: viewer_OnKey.f2: the wrap */
void viewer_OnKey_f2(void *p, u32 eax)
{
	viewer_data *ebp = p;
	ebp->flags ^= 2;
	viewer_OnKey_done_redraw_keybar(ebp);
}

/* viewer.inc: viewer_OnKey.f4: text <-> hex */
void viewer_OnKey_f4(void *p, u32 eax)
{
	viewer_data *ebp = p;
	ebp->flags ^= 1;
	viewer_set_keybar(ebp);
	if (ebp->flags & 1) {
		/* .tohex */
		ebp->cur_pos &= ~(u64)0xF;
	} else {
		ebp->cur_pos += ebp->encoding == encodings_unicode ? 2 : 1;
		viewer_seek_to_current(ebp);
		viewer_prev_newline(ebp);
	}
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.home */
static void viewer_OnKey_home(void *p, u32 eax)
{
	viewer_data *ebp = p;
	ebp->cur_pos = 0;
	ebp->col = 0;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.end */
static void viewer_OnKey_end(void *p, u32 eax)
{
	viewer_data *ebp = p;
	ebp->cur_pos = ebp->filesize & ~(u64)0xF;
	ebp->col = 0;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.right */
static void viewer_OnKey_right(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->col == (u64)-1) return;
	ebp->col++;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.left */
static void viewer_OnKey_left(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (!ebp->col) return;
	ebp->col--;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.ctrl_shift_left */
static void viewer_OnKey_ctrl_shift_left(void *p, u32 eax)
{
	viewer_data *ebp = p;
	ebp->col = 0;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.ctrl_left: 20 columns left; in hex a byte */
static void viewer_OnKey_ctrl_left(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->flags & 1) {
		/* .ctrl_left_hex */
		viewer_seek_to_current(ebp);
		if (viewer_get_prev_char(ebp, &eax)) return;
		viewer_set_curpos(ebp);
		viewer_draw_text(ebp);
		return;
	}
	if (ebp->col < 20) {
		viewer_OnKey_ctrl_shift_left(ebp, eax);
		return;
	}
	ebp->col -= 20;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.ctrl_right: 20 columns right; in hex a byte */
static void viewer_OnKey_ctrl_right(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->flags & 1) {
		/* .ctrl_right_hex */
		viewer_seek_to_current(ebp);
		viewer_get_next_char(ebp, &eax);
		viewer_set_curpos(ebp);
		viewer_draw_text(ebp);
		return;
	}
	if (ebp->col > (u64)-1 - 20) return;
	ebp->col += 20;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.ctrl_shift_right: to the longest line's end */
static void viewer_OnKey_ctrl_shift_right(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (ebp->flags & 1) return;
	ebp->col = viewer_right_side >= (u64)cur_width ? viewer_right_side - cur_width : 0;
	viewer_draw_text(ebp);
}

/* viewer.inc: viewer_OnKey.shift_f8: the menu of the encodings */
static void viewer_OnKey_shift_f8(void *p, u32 eax)
{
	viewer_data *ebp = p;
	int i = 0;
	while (encodings_menu_code[i] != ebp->encoding) i++;
	int r = menu((listitem *)&encodings_menu[i], aTables, 1);
	if (r == -1) return;
	u8 al = encodings_menu_code[(menuitem32 *)r - encodings_menu];
	ebp->encoding = al;
	if (al == encodings_unicode)
		ebp->cur_pos = (ebp->cur_pos + 1) & ~(u64)1;
	viewer_OnKey_done_redraw_keybar(ebp);
}

/* viewer.inc: viewer_OnKey.f7: the search dialog, then the search */
void viewer_OnKey_f7(void *p, u32 eax)
{
	viewer_data *ebp = p;
	viewer_clear_selection(ebp);
	if (find_in_file_dlg())
		viewer_OnKey_shift_f7(ebp, eax);
}

/* viewer.inc: viewer_OnKey.shift_f7: search SearchString in the file
 * from the current position (or after the last found one) */
void viewer_OnKey_shift_f7(void *p, u32 eax)
{
	viewer_data *ebp = p;
	if (!SearchString[0]) return;
	u64 pos = ebp->selected_start + 1;
	if (!ebp->selected_len) pos = ebp->cur_pos;
	viewer_clear_selection(ebp);
	viewer_seek(ebp, pos);
	viewer_skip_unicode_marker(ebp);
	const u8 *table = tolower_table;
	if (find_in_file_dlgdata.case_.flags & 0x10) table = identical_table;
	bool whole = find_in_file_dlgdata.whole.flags & 0x10;
	u32 ecx;
	u8 *edx = search_string_pre(SearchString, table, ebp->encoding, whole, &ecx);
	if (!edx) return;               /* (no memory; the original goes on) */
	/* edx -> FSM, ecx = last state, edi = current state,
	 * esi = buf_pos, ebx = end of buffer */
	u8 *esi = ebp->buf_pos, *ebx = ebp->buf + ebp->buf_size, *edi = edx;
	if (esi >= ebx) {
		if (ebx != ebp->buf + 16384) goto notfound;
		if (!viewer_load_next(ebp)) goto notfound;
		esi = ebp->buf_pos;
		ebx = ebp->buf + ebp->buf_size;
	}
	if (whole) {
		if (ebp->buf_start || ebp->buf_pos != ebp->buf) {
			u32 c = 0;
			viewer_get_prev_char(ebp, &c);
			esi++;
			if (!isspace_table[(u8)c]) goto search_loop;
		}
		/* .f7.startspace */
		edi += 256;
	}
search_loop:
	for (;;) {
		/* get current symbol, calculate next state */
		u32 st = edi[*esi];
		if (st == ecx) goto found;
		/* .search_loop_next: proceed to next symbol */
		edi = edx + (st << 8);
		esi++;
		if (esi < ebx) continue;
		if (ebx != ebp->buf + 16384) goto notfound;
		ebp->buf_pos = esi;
		if (!viewer_load_next(ebp)) goto notfoundt;
		esi = ebp->buf_pos;
		ebx = ebp->buf + ebp->buf_size;
	}
notfoundt:
	/* last chance - if we are looking for a whole word, EOF is ok for last symbol */
	if (whole) {
		u32 st = edi[' '];
		esi++;
		if (st == ecx) goto found;
	}
notfound:
	search_failed(edx);
	viewer_draw_text(ebp);
	return;
found:
	pgfree(edx);
	ebp->buf_pos = esi;
	viewer_set_curpos(ebp);
	pos = ebp->cur_pos;
	ebp->selected_len = ecx;
	pos -= ecx - 1;
	ebp->selected_start = pos;
	pos++;
	if (whole) {
		ebp->selected_len -= 2;
		ebp->selected_start = pos;
		pos++;
	}
	ebp->cur_pos = pos;
	viewer_seek(ebp, pos);
	viewer_prev_newline(ebp);
	viewer_draw_text(ebp);
}

/* viewer.inc: search_failed: "Could not find the string "<string>"" */
void search_failed(u8 *edx)
{
	pgfree(edx);
	char quoted[sizeof(SearchStringEditBuf.data) + 2];
	quoted[0] = '"';
	strcpy(quoted + 1, SearchString);
	strcat(quoted, "\"");
	const char *s[2] = { aStringNotFound, quoted };
	SayErrTitle(aSearch, 2, s, 1, ContinueBtn);
}

_Static_assert(sizeof("View         ") == 14 && sizeof("Edit         ") == 14, "13 characters");

/* viewer.inc: viewer_getname */
void viewer_getname(void *ebp, char *edi, int ecx)
{
	memcpy(edi, "View         ", 13);
	viewedit_getname(ebp, edi + 13, ecx);
}

/* viewer.inc: editor_getname (in viewer.inc in the original); weak,
 * editor.c may have its own */
__attribute__((weak)) void editor_getname(void *ebp, char *edi, int ecx)
{
	memcpy(edi, "Edit         ", 13);
	viewedit_getname(ebp, edi + 13, ecx);
}

/* viewer.inc: viewedit_getname: the volume ("/rd/1/"), then as much of
 * the rest of the name as fits ("..." in front) */
void viewedit_getname(void *ebp, char *edi, int ecx)
{
	ecx -= 13;
	const char *esi = VIEWEDIT_FILENAME(ebp);
	int edx = 3;
	if (VIEWEDIT_HPLUGIN(ebp)) {
		/* the host file's name in up to half of the line, then the
		 * plugin's file */
		const char *h = VIEWEDIT_HOSTNAME(ebp);
		char *start = edi;
		for (int n = ecx >> 1; n > 0 && *h; n--) *edi++ = *h++;
		ecx -= edi - start;
		goto main;
	}
	/* .native */
	for (;;) {
		char al = *esi++;
		*edi++ = al;
		ecx--;
		if (!al) return;                /* .ret */
		if (al == '/' && !--edx) break;
		if (ecx <= 3) break;            /* (the menu's line is full) */
	}
main:;
	/* .main */
	u32 len = strlen(esi);
	if (len > (u32)ecx) {
		memset(edi, '.', 3);
		edi += 3;
		ecx -= 3;
		esi += len - ecx;
	}
	strcpy(edi, esi);
}

/* viewer.inc: viewer_ctrlkeys */
const ctrlkey viewer_ctrlkeys[] = {
	{ 1, 0, 0, viewer_OnKey_exit },
	{ 0x51, 0, 0, viewer_OnKey_pgdn },
	{ 0x49, 0, 0, viewer_OnKey_pgup },
	{ 0x50, 0, 0, viewer_OnKey_down },
	{ 0x48, 0, 0, viewer_OnKey_up },
	{ 0x4B, 0, 0, viewer_OnKey_left },
	{ 0x4B, 0, 0x10, viewer_OnKey_ctrl_left },
	{ 0x4B, 0, 0x11, viewer_OnKey_ctrl_shift_left },
	{ 0x4C, 0, 0, viewer_OnKey_exit },              /* Numpad5 */
	{ 0x4D, 0, 0, viewer_OnKey_right },
	{ 0x4D, 0, 0x10, viewer_OnKey_ctrl_right },
	{ 0x4D, 0, 0x11, viewer_OnKey_ctrl_shift_right },
	{ 0x3C, 0, 0, viewer_OnKey_f2 },
	{ 0x3D, 0, 0, viewer_OnKey_exit },              /* F3 */
	{ 0x3E, 0, 0, viewer_OnKey_f4 },
	{ 0x41, 0, 0, viewer_OnKey_f7 },
	{ 0x41, 0, 1, viewer_OnKey_shift_f7 },
	{ 0x42, 0, 0, viewer_OnKey_f8 },
	{ 0x42, 0, 1, viewer_OnKey_shift_f8 },
	{ 0x44, 0, 0, viewer_OnKey_exit },              /* F10 */
	{ 0x47, 0, 0, viewer_OnKey_home },
	{ 0x4F, 0, 0, viewer_OnKey_end },
	{ 0x58, 0, 0, F12 },
	{ 0x43, 0, 0x100, alt_f9 },
	{ 0 },
};
