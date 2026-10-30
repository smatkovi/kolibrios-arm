/* KolibriOS for ARM: box_lib's EditBox, translated to C from
 * programs/develop/libraries/box_lib/editbox.asm (Alexey Teplov <Lrz>,
 * Evtikhov Maxim, IgorA, the KolibriOS team; GPL-2.0).
 *
 * The private procedures (EditBox.__draw, __processKey, ...) keep their
 * names as static functions; in the original edi holds 'this'. Where the
 * original is unclear or wrong the C does what it meant, with a note:
 * __insertStr's new length (a remainder of a division there), the End
 * key with the caret at the end already (a register left undefined). */
#include <string.h>
#include <stdlib.h>
#include "bl_sys.h"

#define ES_PASSWORD     ed_pass
#define ES_FOCUS        ed_focus
#define ES_SELECT       ed_shift
#define ES_SHIFT_ON     ed_shift_on
#define ES_DRAWN_ONCE   ed_shift_bac
#define ES_INSERT_MODE  ed_insert
#define ES_MOUSE_ON     ed_mouse_on
#define ES_CTRL_ON      ed_ctrl_on
#define ES_ALT_ON       ed_alt_on
#define ES_DISABLED     ed_disabled
#define ES_ALWAYS_FOCUS ed_always_focus
#define ES_NUMERIC      ed_figure_only

/* keys.inc */
#define SCAN_CODE_ESCAPE 0x01
#define SCAN_CODE_TAB    0x0f
#define SCAN_CODE_RETURN 0x1c
#define SCAN_CODE_A      0x1e
#define SCAN_CODE_X      0x2d
#define SCAN_CODE_C      0x2e
#define SCAN_CODE_V      0x2f
#define SCAN_CODE_SPACE  0x39
#define SCAN_CODE_HOME   0x47
#define SCAN_CODE_CLEAR  0x4c
#define SCAN_CODE_DELETE 0x53
#define ASCII_KEY_BACK   0x08
#define ASCII_KEY_CLEAR  0x37
#define ASCII_KEY_LEFT   0xb0

#define visibleCount shift_old
#define selectionPos shift

static void EditBox__draw(edit_box *e);

static void EditBox__drawBorder(edit_box *e)
{
	u32 c = (e->flags & ES_FOCUS) ? e->focus_border_color : e->blur_border_color;
	u32 l = e->left, t = e->top, r = l + e->width, b = t + e->height + 1;
	bl_line(l << 16 | l, t << 16 | b, c);      /* left top -> left bottom */
	bl_line(l << 16 | r, b << 16 | b, c);      /* left bottom -> right bottom */
	bl_line(r << 16 | r, b << 16 | t, c);      /* right bottom -> right top */
	bl_line(r << 16 | l, t << 16 | t, c);      /* right top -> left top */
}

static u32 EditBox__visibleSmbCount(edit_box *e)
{
	return e->char_width ? (e->width - 4) / e->char_width : 0;
}

static void EditBox__drawBg(edit_box *e)
{
	u32 c = (e->flags & ES_DISABLED) ? 0xcacaca : e->color;
	bl_rect((e->left + 1) << 16 | (u16)(e->width - 1), (e->top + 1) << 16 | (u16)e->height, c);
}

/* the offset of the visible part of the text after the caret's position */
static void EditBox__calcOffset(edit_box *e)
{
	u32 vis = e->visibleCount, caret = e->pos, off = e->offset, step;
	if (!(off > caret)) {
		if (off + vis + 1 > caret) goto end;   /* + 1: the cursor on the left edge */
		if (e->size == caret) {
			e->offset = (s32)(e->size - vis) < 0 ? 0 : e->size - vis;
			goto end;
		}
		/* the visible part shifts by 8, or by its size if less */
		step = vis > 8 ? 8 : vis;
		e->offset += step;
		/* still too far from the caret: caret - visibleCount */
		if (e->offset + vis <= caret) e->offset = caret - vis;
		goto end;
	}
	step = vis > 8 ? 8 : vis;
	off = off < step ? 0 : off - step;
	if (off > caret) off = caret ? caret - 1 : 0;
	e->offset = off;
end:
	/* near the text's end: no empty room after it */
	off = e->offset;
	if (!(caret < e->size - vis) && off + vis > e->size)
		off = e->size - vis;
	e->offset = off;
}

static void EditBox__drawText(edit_box *e)
{
	u32 n = e->size - e->offset;
	if (e->visibleCount < n) n = e->visibleCount;
	if (!n) return;
	u32 xy = (e->left + 2) << 16 | (u16)(e->top + 3);
	if (!(e->flags & ES_PASSWORD)) {
		bl_text(xy, e->text_color, e->text + e->offset, n);
		return;
	}
	for (; n; n--, xy += e->char_width << 16)
		bl_text(xy, e->text_color, "*", 1);
}

static void EditBox__drawSelection(edit_box *e)
{
	if (!(e->flags & ES_SELECT)) return;
	u32 end = e->visibleCount + e->offset;
	u32 lo = e->pos, hi = e->selectionPos;
	if (hi < lo) { u32 t = hi; hi = lo; lo = t; }
	if (end < hi) hi = end;
	if (lo < e->offset) lo = e->offset;
	if (hi <= lo) return;
	lo -= e->offset;
	hi -= e->offset;
	u32 x = lo * e->char_width + e->left + 1, w = (hi - lo) * e->char_width + 1;
	bl_rect(x << 16 | (u16)w, (e->top + 1) << 16 | (u16)e->height, e->shift_color);
}

/* the font's size from text_color (function 4's bits 24-27 the scale,
 * 28 the font) */
static void EditBox__calcFontParameters(edit_box *e)
{
	u32 c = e->text_color & 0x17ffffff;
	u32 scale = ((c >> 24) & 0xf) + 1;
	if (!(c >> 28)) {                      /* 6x9 */
		e->char_width = 6 * scale;
		e->height = 9 * scale + 4;
	} else {                               /* 8x16 */
		e->char_width = 8 * scale;
		e->height = (8 * scale + 2) * 2;
	}
}

static void EditBox__terminateText(edit_box *e)
{
	if (e->text) { e->text[e->size] = 0; e->text[e->size + 1] = 0; }
}

static void EditBox__calcCursorPos(edit_box *e)
{
	e->cl_curs_x = (e->pos - e->offset) * e->char_width + e->left + 1;
	e->cl_curs_y = e->top + 2;
}

static void EditBox__drawCursor(edit_box *e)
{
	u32 x = e->cl_curs_x, y = e->cl_curs_y;
	bl_line(x << 16 | x, y << 16 | (u16)(y + e->height - 3), e->text_color & 0xffffff);
}

static void EditBox__draw(edit_box *e)
{
	EditBox__terminateText(e);
	EditBox__calcFontParameters(e);
	e->visibleCount = EditBox__visibleSmbCount(e);
	EditBox__calcOffset(e);
	e->flags |= ES_DRAWN_ONCE;
	EditBox__drawBorder(e);
	EditBox__drawBg(e);
	if (e->flags & ES_FOCUS) {
		EditBox__calcCursorPos(e);
		EditBox__drawSelection(e);
		EditBox__drawText(e);
		EditBox__drawCursor(e);
	} else {
		EditBox__drawText(e);
	}
}

void edit_box_draw(edit_box *e)
{
	EditBox__draw(e);
}

static void EditBox__processMouse(edit_box *e)
{
	if (!(e->flags & ES_DRAWN_ONCE) || (e->flags & ES_DISABLED)) return;
	u32 **owner = (u32 **)e->mouse_variable;   /* pp_mouseOwner */
	if (!(bl_mouse_buttons() & 1)) {           /* the left button up */
		e->flags &= ~ES_MOUSE_ON;
		if (owner) *owner = 0;
		return;
	}
	if (owner && *owner && *owner != (u32 *)e) goto mouse_blur;
	/* the mouse in the window's coordinates */
	u32 pos = bl_mouse_pos();
	if (e->flags & ES_MOUSE_ON) {
		/* the mouse drags a selection */
		e->flags |= ES_SELECT;
		if ((s32)pos < 0 || (pos >> 16) < e->left) {
			if (e->pos) e->pos--;
		} else if (e->width < (pos >> 16) - e->left) {
			if (e->pos < e->size) e->pos++;
		} else {
			u32 n = (e->char_width ? ((pos >> 16) - e->left) / e->char_width : 0) + e->offset;
			if (n < e->size) e->pos = n;
		}
		if ((u16)e->pos == e->selectionPos) e->flags &= ~ES_SELECT;
		EditBox__draw(e);
		return;
	}
	s16 y = pos, x = pos >> 16;
	if (y < (s16)e->top || y > (s16)(e->top + e->height)) goto mouse_blur;
	if (x < (s16)e->left || x > (s16)(e->left + e->width)) goto mouse_blur;
	/* the caret to the mouse */
	u32 n = (e->char_width ? (u32)(x - (s16)e->left) / e->char_width : 0) + e->offset;
	if (n > e->size) n = e->size;
	/* a selection starts */
	if (!(e->flags & ES_MOUSE_ON)) {
		e->pos = n;
		e->selectionPos = n;
		e->flags |= ES_MOUSE_ON;
		if (owner) *owner = (u32 *)e;
	}
	e->flags |= ES_FOCUS;
	e->flags &= ~ES_SELECT;
	EditBox__draw(e);
	return;
mouse_blur:
	if (!(e->flags & ES_ALWAYS_FOCUS)) e->flags &= ~ES_FOCUS;
	e->flags &= ~ES_SELECT;
	EditBox__draw(e);
}

void edit_box_mouse(edit_box *e)
{
	EditBox__processMouse(e);
}

/* returns non-zero when something was deleted */
static int EditBox__delSelection(edit_box *e)
{
	u32 lo = e->selectionPos, hi = e->pos;
	if (lo == hi) return 0;
	if (lo > hi) { u32 t = lo; lo = hi; hi = t; }
	u32 n = e->size - hi;
	if (n) memmove(e->text + lo, e->text + hi, n);
	e->pos = lo;
	e->size = lo + n;
	e->flags &= ~ES_SELECT;
	return 1;
}

static void EditBox__replaceSelectChar(edit_box *e, u8 c)
{
	u32 lo = e->selectionPos, hi = e->pos;
	if (lo == hi) return;
	if (lo > hi) { u32 t = lo; lo = hi; hi = t; }
	e->text[lo++] = c;
	u32 n = e->size - hi;
	if (n && e->text + hi != e->text + lo) memmove(e->text + lo, e->text + hi, n);
	e->size = lo + n;
	e->pos = lo;
	e->flags &= ~ES_SELECT;
}

/* returns non-zero when inserted */
static int EditBox__insertChar(edit_box *e, u8 c)
{
	if (e->size == e->max) return 0;
	if (e->size == e->pos) {
		e->text[e->size++] = c;
		e->pos++;
		return 1;
	}
	if (!(e->flags & ES_INSERT_MODE)) {
		memmove(e->text + e->pos + 1, e->text + e->pos, e->size - e->pos);
		e->text[e->pos] = c;
		e->size++;
		e->pos++;
		return 1;
	}
	e->text[e->pos++] = c;                 /* overwrite */
	return 1;
}

static void EditBox__selectionToClipboard(edit_box *e)
{
	if (!(e->flags & ES_SELECT)) return;
	u32 lo = e->selectionPos, hi = e->pos;
	if (lo == hi) return;
	if (lo > hi) { u32 t = lo; lo = hi; hi = t; }
	u32 n = hi - lo + sizeof(struct bl_clip);
	struct bl_clip *b = malloc(n);
	if (!b) return;
	b->cbSize = n;
	b->contentType = 0;
	b->encoding = 1;
	memcpy(b->data_, e->text + lo, hi - lo);
	bl_clip_write(b, n);
	free(b);
}

struct EditBox_InplaceStr { u32 len; const char *p_str; };

static void EditBox__insertStr(edit_box *e, const struct EditBox_InplaceStr *str)
{
	u32 caret = e->pos, s = (e->flags & ES_SELECT) ? e->selectionPos : caret;
	u32 startSel = s < caret ? s : caret, endSel = s < caret ? caret : s;
	u32 restLen = e->size - endSel;
	/* the original takes the remainder of a division by max + 1; meant
	 * is the length cut to max */
	u32 newLen = str->len + startSel + restLen;
	if (newLen > e->max) newLen = e->max;
	s32 room = newLen - startSel;
	if (!room) return;
	if (room > (s32)str->len) {
		/* room for the rest after the selection */
		restLen = room - str->len;
		u32 newCaret = startSel + str->len;
		if (endSel != newCaret) memmove(e->text + newCaret, e->text + endSel, restLen);
		memcpy(e->text + startSel, str->p_str, str->len);
		e->pos = newCaret;
		e->size = newLen;
		e->flags &= ~ES_SELECT;
		return;
	}
	memcpy(e->text + startSel, str->p_str, room);
	e->pos = newLen;
	e->size = newLen;
	e->flags &= ~ES_SELECT;
}

/* 0: digits only */
static int BEdit__validateStrNumeric(const struct EditBox_InplaceStr *str)
{
	for (u32 i = 0; i < str->len; i++)
		if ((u8)(str->p_str[i] - '0') > 9) return 1;
	return 0;
}

static void EditBox__processPrintableCharacter(edit_box *e, u32 k)
{
	u8 al = k, c = k;
	if ((e->flags & ES_NUMERIC) && (al < '0' || al > '9')) return;
	if (e->flags & ES_SELECT) {
		EditBox__replaceSelectChar(e, c);
		EditBox__draw(e);
		return;
	}
	if (EditBox__insertChar(e, c)) EditBox__draw(e);
}

static void commit_caret_move(edit_box *e, u32 to)
{
	if (e->flags & ES_SELECT) {
		e->flags &= ~ES_SELECT;
		if (e->flags & ES_SHIFT_ON) {
			e->flags |= ES_SELECT;
			if (to == e->pos) return;
			e->pos = to;
			EditBox__draw(e);
			return;
		}
		e->pos = to;
		e->selectionPos = e->pos;
		EditBox__draw(e);
		return;
	}
	if (to == e->pos) return;
	e->selectionPos = e->pos;
	e->pos = to;
	if ((e->flags & ES_SHIFT_ON) && (u16)to != e->selectionPos) e->flags |= ES_SELECT;
	EditBox__draw(e);
}

static void EditBox__processKey(edit_box *e, u32 key)
{
	if (!(e->flags & ES_DRAWN_ONCE) || !(e->flags & ES_FOCUS)) return;
	if (e->flags & (ES_MOUSE_ON | ES_DISABLED)) return;
	u32 m = bl_control_keys();
	if (m & 0x600) return;                 /* a Win key */
	e->flags &= ~(ES_ALT_ON | ES_CTRL_ON | ES_SHIFT_ON);
	if (m & 0x03) e->flags |= ES_SHIFT_ON;
	if (m & 0x0c) e->flags |= ES_CTRL_ON;
	if (m & 0x30) e->flags |= ES_ALT_ON;
	/* ror eax, 8: al the character, ah the scan code */
	u32 k = key >> 8 | key << 24;
	u8 al = k, ah = k >> 8;
	if (e->flags & ES_CTRL_ON) {
		if (ah == SCAN_CODE_V) {
			int n = bl_clip_count();
			if (n <= 0) return;
			struct bl_clip *b = bl_clip_read(n - 1);
			if ((u32)b == (u32)-1 || (u32)b == 1 || !b) return;
			if (b->contentType != 0 || (u8)b->encoding != 1) {   /* text, cp866 */
				bl_sys_free(b);
				return;
			}
			struct EditBox_InplaceStr str = { b->cbSize - sizeof(struct bl_clip), b->data_ };
			if ((e->flags & ES_NUMERIC) && BEdit__validateStrNumeric(&str)) {
				bl_sys_free(b);
				return;
			}
			EditBox__insertStr(e, &str);
			bl_sys_free(b);
			EditBox__draw(e);
		} else if (ah == SCAN_CODE_A) {
			if (!e->size) return;
			if ((e->flags & ES_SELECT) && e->selectionPos == 0 && e->pos == e->size) return;
			e->selectionPos = 0;
			e->pos = e->size;
			e->flags |= ES_SELECT;
			EditBox__draw(e);
		} else if (ah == SCAN_CODE_X) {
			EditBox__selectionToClipboard(e);
			if (EditBox__delSelection(e)) EditBox__draw(e);
		} else if (ah == SCAN_CODE_C) {
			EditBox__selectionToClipboard(e);
		}
		return;
	}
	if (ah <= SCAN_CODE_SPACE) {
		if (al == ASCII_KEY_BACK) {
			if (!e->size) return;
			if (!(e->flags & ES_SELECT)) {
				if (!e->pos) return;
				e->selectionPos = e->pos - 1;
				e->flags |= ES_SELECT;
			}
			EditBox__delSelection(e);
			EditBox__draw(e);
			return;
		}
		/* scan codes not supported */
		if (ah == SCAN_CODE_TAB || ah == SCAN_CODE_RETURN || ah == SCAN_CODE_ESCAPE) return;
		EditBox__processPrintableCharacter(e, k);
		return;
	}
	if (ah > SCAN_CODE_DELETE || ah < SCAN_CODE_HOME) return;
	if ((k & 0xffff) == (SCAN_CODE_CLEAR << 8 | ASCII_KEY_CLEAR)) return;
	if (al < ASCII_KEY_LEFT) {             /* the number pad's digits */
		EditBox__processPrintableCharacter(e, k);
		return;
	}
	u32 to;
	switch (al & 0x0f) {                   /* .unlock_numpad_filtration */
	case 0:                                /* left */
		to = e->pos ? e->pos - 1 : 0;
		commit_caret_move(e, to);
		return;
	case 3:                                /* right */
		to = e->pos != e->size ? e->pos + 1 : e->pos;
		commit_caret_move(e, to);
		return;
	case 4:                                /* home */
		commit_caret_move(e, 0);
		return;
	case 5:                                /* end */
		commit_caret_move(e, e->size);
		return;
	case 6:                                /* delete */
		if (!e->size) return;
		if (!(e->flags & ES_SELECT)) {
			if (e->pos == e->size) return;
			e->selectionPos = e->pos + 1;
			e->flags |= ES_SELECT;
		}
		EditBox__delSelection(e);
		EditBox__draw(e);
		return;
	case 9:                                /* insert */
		e->flags ^= ES_INSERT_MODE;
		return;
	default:                               /* down, up, page down, page up */
		return;
	}
}

void edit_box_key(edit_box *e, u32 key)
{
	EditBox__processKey(e, key);
}

void edit_box_key_safe(edit_box *e, u32 key)
{
	EditBox__processKey(e, key);
}

void edit_box_set_text(edit_box *e, const char *text)
{
	u32 n = 0;
	while (n < e->max && text[n]) n++;
	memcpy(e->text, text, n < e->max ? n + 1 : n);
	if (n == e->max && n) n--;             /* as the original: one short when full */
	e->pos = n;
	e->size = n;
	e->flags &= ~ES_SELECT;
	EditBox__draw(e);
}
