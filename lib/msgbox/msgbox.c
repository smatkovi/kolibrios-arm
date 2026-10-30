/* KolibriOS for ARM: msgbox (lib/msgbox/msgbox.h), translated to C from
 * programs/develop/libraries/msgbox/msgbox.asm (IgorA, the KolibriOS
 * team; strlen.inc: Thomas Mathys, linlen: Igor Afanasiev; GPL-2.0).
 *
 * The box's measures are the original's: the text in lines of 10
 * pixels, 6 a character, the buttons 20 high with their margins
 * (MB_BUT_OTX_*, MB_BUT_OTY_1). Its window, a skinned one in the middle
 * of the screen there, is a framed box with a caption in the middle of
 * the program's window here, with a close button in the caption; Enter
 * presses the focused button, Left, Right and Tab move the focus, and
 * Escape closes the box (an addition: the original's window has its
 * skin's close button for that). */
#include <string.h>
#include <kos.h>
#include "msgbox.h"

#define MB_FIRST_BUT_ID 3
#define MB_MAX_BUT 8
#define MB_BUT_HEIGHT 20
#define CHAR_WIDTH 6
#define MB_BUT_OTY_1 5
#define MB_BUT_OTX_1 5
#define MB_BUT_OTX_2 10
#define MB_BUT_OTX_3 15
#define MB_TEXT_OFFSET 2
/* the box in the program's window */
#define MB_CAPT_H 20                   /* its caption (the skin's place) */
#define MB_ID_BASE 0xf000              /* its buttons' identifiers */
#define MB_ID_CLOSE (MB_ID_BASE + 1)

static struct {
	u32 frame, grab, work_dark, work_light, grab_text, work, work_button, work_button_text, work_text, work_graph;
} sc;

static char *mb_text;                  /* the data */
static const char *txtMsg;             /* the text */
static const char *txtBut[MB_MAX_BUT];
static int ot_left, ot_top, but_width;
static int mb_left, mb_width = 300, mb_top, mb_height = 50;
static int mb_key_foc = MB_FIRST_BUT_ID, mb_key_max = 1;
static mb_function *mb_funct;
static int mb_open;

/* linlen: the length up to a 13 (or n) */
static int linlen(const char *s, int n)
{
	int i = 0;
	while (i < n && s[i] != 13) i++;
	return i;
}

/* the client area of the program's window */
static void client_size(int *w, int *h)
{
	u8 pi[1024];
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	*w = *(s32 *)(pi + 62) + 1;
	*h = *(s32 *)(pi + 66) + 1;
}

void mb_reinit(char *data)
{
	mb_text = data;
	const char *s = data + MB_TEXT_OFFSET;
	txtMsg = s + strlen(s) + 1;            /* after the caption */
	/* the width from the longest line */
	mb_width = 300;
	s = txtMsg;
	for (;;) {
		int n = strlen(s), l = linlen(s, n), w = l * 6 + 10 + 2 * 4;
		if (w > mb_width) mb_width = w;
		s += l + 1;
		if (n - l <= 5) break;             /* the rest's length */
	}
	/* the buttons' captions */
	s = txtMsg + strlen(txtMsg) + 1;
	int k = 0;
	for (; k < MB_MAX_BUT && *s; k++) {
		txtBut[k] = s;
		s += strlen(s) + 1;
	}
	mb_key_max = k;
	for (; k < MB_MAX_BUT; k++) txtBut[k] = NULL;
	/* the buttons' top */
	int lines = 1;
	for (s = txtMsg + 1; *s && s[-1]; s++)
		if (*s == 13) lines++;
	ot_top = lines * 10 + 2 * MB_BUT_OTY_1;
	/* the box's height */
	mb_height = ot_top + MB_BUT_OTY_1 + MB_BUT_HEIGHT + 5 + MB_CAPT_H;
	/* the buttons' width */
	int chars = 0;
	for (k = 0; k < mb_key_max; k++) chars += strlen(txtBut[k]);
	but_width = chars * CHAR_WIDTH + mb_key_max * MB_BUT_OTX_3 + MB_BUT_OTX_2 - MB_BUT_OTX_3;
	/* in the middle of the window */
	int cw, ch;
	client_size(&cw, &ch);
	if (mb_width > cw) mb_width = cw;
	mb_left = (cw - mb_width) / 2;
	mb_top = (ch - mb_height) / 2;
	if (mb_left < 0) mb_left = 0;
	if (mb_top < 0) mb_top = 0;
}

void mb_create(char *data, void *thread)
{
	(void)thread;
	kos_call(48, 3, (u32)&sc, sizeof(sc), 0, 0);
	mb_key_foc = MB_FIRST_BUT_ID + (u8)data[0] - 1;   /* the button focused first */
	mb_reinit(data);
	if (mb_key_foc < MB_FIRST_BUT_ID || mb_key_foc >= MB_FIRST_BUT_ID + mb_key_max) mb_key_foc = MB_FIRST_BUT_ID;
	mb_funct = NULL;
	mb_open = 1;
	mb_draw();
}

void mb_setfunctions(mb_function *funcs)
{
	mb_funct = funcs;
}

int mb_is_open(void) { return mb_open; }

static void MsgBoxDrawAllBut(void)
{
	int bx = mb_left + 1 + ot_left, by = mb_top + MB_CAPT_H + ot_top + MB_BUT_OTY_1;
	for (int k = 0; k < mb_key_max; k++) {
		int id = MB_FIRST_BUT_ID + k, len = strlen(txtBut[k]);
		int w = len * CHAR_WIDTH + MB_BUT_OTX_2;
		int focus = id == mb_key_foc;
		u32 bg = focus ? sc.work_button_text : sc.work_button, fg = focus ? sc.work_button : sc.work_button_text;
		kos_button(bx - MB_BUT_OTX_1, by - MB_BUT_OTY_1, w + 1, MB_BUT_HEIGHT + 1, MB_ID_BASE + id, bg);
		kos_call(4, (u32)bx << 16 | (u16)(by + 1), (fg & 0xffffff), (u32)txtBut[k], len, 0);
		bx += len * CHAR_WIDTH + MB_BUT_OTX_3;
	}
}

void mb_draw(void)
{
	if (!mb_open) return;
	int x = mb_left, y = mb_top, w = mb_width, h = mb_height;
	/* the frame, the caption, the work area */
	kos_rect(x, y, w, h, sc.frame);
	kos_rect(x + 1, y + 1, w - 2, MB_CAPT_H - 1, sc.grab);
	kos_rect(x + 1, y + MB_CAPT_H, w - 2, h - MB_CAPT_H - 1, sc.work);
	kos_call(4, (u32)(x + 8) << 16 | (u16)(y + 6), 0x80000000u | (sc.grab_text & 0xffffff),
		 (u32)(mb_text + MB_TEXT_OFFSET), 0, 0);
	/* the close button: a cross */
	int cx = x + w - MB_CAPT_H, cy = y + 2;
	kos_button(cx, cy, MB_CAPT_H - 4, MB_CAPT_H - 4, MB_ID_CLOSE, sc.grab);
	kos_line(cx + 4, cy + 4, cx + MB_CAPT_H - 9, cy + MB_CAPT_H - 9, sc.grab_text);
	kos_line(cx + MB_CAPT_H - 9, cy + 4, cx + 4, cy + MB_CAPT_H - 9, sc.grab_text);
	/* the text, line by line */
	ot_left = (w - 2 - but_width) / 2 + MB_BUT_OTX_1;
	const char *s = txtMsg;
	int ty = y + MB_CAPT_H + MB_BUT_OTY_1;
	for (;;) {
		int n = strlen(s), l = linlen(s, n);
		if (!l && !n) break;
		if (l) kos_call(4, (u32)(x + 1 + MB_BUT_OTX_1) << 16 | (u16)ty, sc.work_text & 0xffffff, (u32)s, l, 0);
		s += l;
		if (!*s) break;
		s++;
		ty += 10;
	}
	MsgBoxDrawAllBut();
}

/* the box closed with button k (1 ...; 0: closed), its function called */
static void close_box(int k)
{
	mb_text[0] = k;
	mb_open = 0;
	kos_button_delete(MB_ID_CLOSE);
	for (int i = 0; i < mb_key_max; i++) kos_button_delete(MB_ID_BASE + MB_FIRST_BUT_ID + i);
	if (k && mb_funct && mb_funct[k - 1]) mb_funct[k - 1]();
}

int mb_event(int ev)
{
	if (!mb_open) return 0;
	if (ev == 1) return 0;                 /* the program redraws, mb_draw at its end */
	if (ev == 2) {
		u32 key = kos_get_key();
		if ((key & 0xff) == 1) return 1;
		u8 ah = key >> 8;
		enum { NONE, ENTER, LEFT, RIGHT, ESC } k = NONE;
		if (kos_call(66, 2, 0, 0, 0, 0) == 1) {   /* the program reads scan codes */
			if (ah == 0x1c) k = ENTER;
			else if (ah == 0x4b) k = LEFT;
			else if (ah == 0x4d || ah == 0x0f) k = RIGHT;
			else if (ah == 0x01) k = ESC;
		} else {
			if (ah == 13) k = ENTER;
			else if (ah == 176) k = LEFT;
			else if (ah == 179 || ah == 9) k = RIGHT;
			else if (ah == 27) k = ESC;
		}
		if (k == ENTER) {
			close_box(mb_key_foc - MB_FIRST_BUT_ID + 1);
			return 2;
		}
		if (k == ESC) {
			close_box(0);
			return 2;
		}
		if (k == LEFT) {
			if (mb_key_foc <= MB_FIRST_BUT_ID) mb_key_foc += mb_key_max;
			mb_key_foc--;
			MsgBoxDrawAllBut();
		}
		if (k == RIGHT && mb_key_max > 1) {
			if (++mb_key_foc >= MB_FIRST_BUT_ID + mb_key_max) mb_key_foc = MB_FIRST_BUT_ID;
			MsgBoxDrawAllBut();
		}
		return 1;
	}
	if (ev == 3) {
		u32 id = kos_get_button() >> 8;
		if (id == MB_ID_CLOSE || id == 1) {    /* its close button, or the window's */
			close_box(0);
			return 2;
		}
		if (id >= MB_ID_BASE + MB_FIRST_BUT_ID && id < MB_ID_BASE + MB_FIRST_BUT_ID + (u32)mb_key_max) {
			close_box(id - MB_ID_BASE - MB_FIRST_BUT_ID + 1);
			return 2;
		}
		return 1;                          /* the program's buttons wait */
	}
	return 1;                              /* the mouse: the box is modal */
}
