/* KolibriOS for ARM: msgbox (lib/msgbox/msgbox.h), translated to C from
 * programs/develop/libraries/msgbox/msgbox.asm (IgorA, the KolibriOS
 * team; strlen.inc: Thomas Mathys, linlen: Igor Afanasiev; GPL-2.0).
 *
 * As there: MsgBoxCreate starts a thread (51.1) on the stack the caller
 * gives; the thread (start) makes a skinned window in the middle of the
 * screen, sized by MsgBoxReInit from the text (lines of 10 pixels, 6 a
 * character, the buttons 20 high with their margins MB_BUT_OTX_*,
 * MB_BUT_OTY_1), with the caption as its title. Enter presses the
 * focused button, Left, Right and Tab move the focus, the window's close
 * button gives 0. The button goes into the data's first byte, the
 * thread calls the button's function (mb_setfunctions) and ends (-1);
 * the program goes on meanwhile and learns the answer from the byte or
 * the function.
 *
 * What differs: the original keeps the box in the library's variables,
 * so a second box (a function of the first one showing a message, as
 * t_edit's do when a file does not open) took over the first's text and
 * started on the stack the first one still ran on. Here each box has its
 * variables (struct box) and its stack: the caller's if no box runs on
 * it, else one of the library's own (MB_SPARE of them); a box finds its
 * variables by its stack. */
#include <string.h>
#include <kos.h>
#include "msgbox.h"

#define MB_FIRST_BUT_ID 3              /* the first button's identifier */
#define MB_MAX_BUT 8                   /* the most buttons of a box */
#define MB_BUT_HEIGHT 20               /* a button's height */
#define CHAR_WIDTH 6                   /* a character's width */
#define MB_BUT_OTY_1 5                 /* between the title and the text, the text and the buttons */
/* the margins at a button: [<-1->Caption<-2->]<-3-> */
#define MB_BUT_OTX_1 5
#define MB_BUT_OTX_2 10
#define MB_BUT_OTX_3 15
#define MB_TEXT_OFFSET 2               /* the caption's start in the data */
#define SF_DRAW_TEXT 4                 /* (the original's width adds it twice) */

#define MB_BOXES 4                     /* boxes at once */
#define MB_SPARE 2                     /* the library's own stacks */
#define MB_SPARE_SIZE 16384

/* the original's variables, a box's */
struct box {
	volatile int running;          /* its thread runs (cleared as it ends) */
	u32 tid;
	u8 *stack;                     /* its stack's top */
	char *mb_text;                 /* the data */
	const char *txtMsg;            /* the text */
	const char *txtBut[MB_MAX_BUT];/* the buttons' captions */
	int ot_left, ot_top;           /* the buttons' place */
	int but_width;                 /* all buttons with their gaps */
	int mb_left, mb_width, mb_top, mb_height;   /* the window on the screen */
	int mb_key_foc;                /* the focused button (from MB_FIRST_BUT_ID) */
	int mb_key_max;                /* the buttons */
	mb_function *volatile mb_funct;
	struct { u32 frame, grab, work_dark, work_light, grab_text, work, work_button, work_button_text, work_text,
		     work_graph; } sc;
	u8 mb_procinfo[1024];
};

static struct box boxes[MB_BOXES];
static struct box *mb_cur;             /* the box created last */
static u8 mb_spare[MB_SPARE][MB_SPARE_SIZE] __attribute__((aligned(16)));

/* strlen.inc's linlen: the length up to a 13, n at most */
static int linlen(const char *s, int n)
{
	int i = 0;
	while (i < n && s[i] != 13) i++;
	return i;
}

/* the box of the running thread: the one whose stack's top is the
 * nearest above a variable of its stack */
static struct box *box_of_stack(void)
{
	volatile char probe;
	u8 *sp = (u8 *)&probe;
	struct box *m = NULL;
	for (int i = 0; i < MB_BOXES; i++) {
		struct box *b = &boxes[i];
		if (b->running && b->stack > sp && (!m || b->stack < m->stack)) m = b;
	}
	return m;
}

/* MsgBoxReInit: the text's and the buttons' places, the window's size
 * and its place in the middle of the screen */
static void reinit(struct box *b, char *data)
{
	b->mb_text = data;
	const char *s = data + MB_TEXT_OFFSET;
	s += strlen(s) + 1;                    /* after the caption */
	b->txtMsg = s;
	/* --- calc window width --- */
	for (;;) {
		int n = strlen(s), l = linlen(s, n);
		int w = l * 6 + 10 + 2 * SF_DRAW_TEXT;
		if (b->mb_width < w) b->mb_width = w;
		s += l + 1;
		if (n - l <= 5) break;             /* the length of the remaining text */
	}
	/* --- go to first button text --- buttons 1 ... MB_MAX_BUT */
	s = b->txtMsg + strlen(b->txtMsg) + 1;
	for (int k = 0; k < MB_MAX_BUT; k++) {
		b->txtBut[k] = s;
		if (!*s) break;
		s += strlen(s) + 1;
	}
	/* --- calc button top --- */
	int lines = 1;
	for (s = b->txtMsg; *++s;)
		if (*s == 13) lines++;
	b->ot_top = lines * 10 + 2 * MB_BUT_OTY_1;
	/* --- calc window height --- */
	b->mb_height = b->ot_top + MB_BUT_OTY_1 + MB_BUT_HEIGHT + 5;   /* 5: the border */
	b->mb_height += kos_call(48, 4, 0, 0, 0, 0);                    /* the skin's title */
	/* --- calc window top, left --- */
	u32 scr = kos_call(14, 0, 0, 0, 0, 0);
	b->mb_top = (u16)((scr & 0xffff) - b->mb_height) >> 1;
	b->mb_left = (u16)((scr >> 16) - b->mb_width) >> 1;
	/* --- calc button width --- */
	int chars = 0, n = 0;
	b->mb_key_max = 0;
	for (s = b->txtBut[0]; *s; s += strlen(s) + 1) {
		n++;
		chars += strlen(s);
		b->mb_key_max++;
	}
	b->but_width = chars * CHAR_WIDTH + n * MB_BUT_OTX_3 + MB_BUT_OTX_2 - MB_BUT_OTX_3;
}

/* MsgBoxDrawButton: button id, its caption s of len characters, x << 16
 * | y the caption's place */
static void MsgBoxDrawButton(struct box *b, int id, const char *s, int len, u32 ebx)
{
	u32 face = id == b->mb_key_foc ? b->sc.work_button_text : b->sc.work_button;
	u32 x = (ebx >> 16) - MB_BUT_OTX_1, y = (ebx & 0xffff) - MB_BUT_OTY_1;
	/* drawn again on a key: defined again (KolibriOS keeps thousands of
	 * buttons, this kernel 64 a window) */
	kos_button_delete(id);
	kos_call(8, x << 16 | (len * CHAR_WIDTH + MB_BUT_OTX_2), y << 16 | MB_BUT_HEIGHT, id, face, 0);
	u32 c = b->sc.work_button_text;
	if (face == c) c = b->sc.work_button;
	kos_call(4, ebx, c & 0xffffff, (u32)s, len, 0);
}

/* MsgBoxDrawAllBut */
static void MsgBoxDrawAllBut(struct box *b)
{
	u32 ebx = (u32)b->ot_left << 16 | (u16)(b->ot_top + MB_BUT_OTY_1);
	int id = MB_FIRST_BUT_ID;
	for (int k = 0; k < MB_MAX_BUT; k++, id++) {
		const char *s = b->txtBut[k];
		int len = strlen(s);
		if (!len) break;
		MsgBoxDrawButton(b, id, s, len, ebx);
		ebx += (u32)(len * CHAR_WIDTH + MB_BUT_OTX_3) << 16;
	}
}

/* red_win */
static void red_win(struct box *b)
{
	kos_begin_draw();
	kos_call(0, (u32)b->mb_left << 16 | (u16)b->mb_width, (u32)b->mb_top << 16 | (u16)b->mb_height,
		 (b->sc.work & 0xffffff) | 0x33000000u, 0, (u32)(b->mb_text + MB_TEXT_OFFSET));
	kos_call(9, (u32)b->mb_procinfo, -1, 0, 0, 0);
	int cw = *(s32 *)(b->mb_procinfo + 62);    /* client_box.width */
	b->ot_left = (cw > b->but_width ? (cw - b->but_width) >> 1 : 0) + MB_BUT_OTX_1;
	/* the text, line by line */
	u32 ebx = MB_BUT_OTX_1 << 16 | MB_BUT_OTY_1;
	const char *s = b->txtMsg;
	for (;;) {
		int l = linlen(s, strlen(s));
		if (!l) break;
		kos_call(4, ebx, b->sc.work_text & 0xffffff, (u32)s, l, 0);
		s += l;
		if (!*s) break;
		s++;
		ebx += 10;
	}
	MsgBoxDrawAllBut(b);
	kos_end_draw();
}

/* close: the button (1 ...; 0 the window closed) in the data, its
 * function called, the thread ended */
static void close(struct box *b, int k)
{
	b->mb_text[0] = k;
	mb_function *f = b->mb_funct;
	if (k && f && f[k - 1]) f[k - 1]();
	b->running = 0;
	kos_exit();
}

/* start: the box's thread */
static void start(void)
{
	struct box *b = box_of_stack();
	if (!b) kos_exit();
	kos_call(48, 3, (u32)&b->sc, sizeof(b->sc), 0, 0);
	kos_event_mask(0x27);                  /* EVM_MOUSE + EVM_BUTTON + EVM_KEY + EVM_REDRAW */
	/* the button focused first */
	b->mb_key_foc = (u8)(b->mb_text[0] + MB_FIRST_BUT_ID - 1);
	reinit(b, b->mb_text);
	if (b->mb_key_foc < MB_FIRST_BUT_ID || b->mb_key_foc >= MB_FIRST_BUT_ID + b->mb_key_max)
		b->mb_key_foc = MB_FIRST_BUT_ID;   /* (an addition: a focus beyond the buttons) */
	for (;;) {                             /* still */
		int ev = kos_wait_event();
		if (ev == 1) {
			red_win(b);
		} else if (ev == 2) {              /* key: ASCII codes, a new thread's mode */
			u8 ah = kos_get_key() >> 8;
			if (ah == 13) close(b, b->mb_key_foc - MB_FIRST_BUT_ID + 1);
			if (ah == 176) {               /* Left */
				if (b->mb_key_foc <= MB_FIRST_BUT_ID) b->mb_key_foc += b->mb_key_max;
				b->mb_key_foc--;
				MsgBoxDrawAllBut(b);
			}
			if (ah == 9 || ah == 179) {    /* Tab, Right */
				int al = b->mb_key_max + MB_FIRST_BUT_ID;
				if (b->mb_key_foc != al) {
					if (++b->mb_key_foc >= al) b->mb_key_foc = MB_FIRST_BUT_ID;
					MsgBoxDrawAllBut(b);
				}
			}
		} else if (ev == 3) {
			u32 ah = kos_get_button() >> 8;
			if (ah >= MB_FIRST_BUT_ID) close(b, ah - MB_FIRST_BUT_ID + 1);
			if (ah == 1) close(b, 0);      /* .exit */
		}
	}
}

/* a box whose thread is gone (ended by another, 18.2 or 18.18) is free */
static void sweep(void)
{
	for (int i = 0; i < MB_BOXES; i++)
		if (boxes[i].running && !kos_slot_of_pid(boxes[i].tid)) boxes[i].running = 0;
}

static int stack_busy(const u8 *top)
{
	for (int i = 0; i < MB_BOXES; i++)
		if (boxes[i].running && boxes[i].stack == top) return 1;
	return 0;
}

/* MsgBoxCreate */
void mb_create(char *data, void *thread)
{
	sweep();
	u8 *top = thread;
	if (!top || stack_busy(top)) {         /* that stack in use: one of the library's */
		top = NULL;
		for (int i = 0; i < MB_SPARE && !top; i++)
			if (!stack_busy(mb_spare[i] + MB_SPARE_SIZE)) top = mb_spare[i] + MB_SPARE_SIZE;
	}
	struct box *b = NULL;
	for (int i = 0; i < MB_BOXES && !b; i++)
		if (!boxes[i].running) b = &boxes[i];
	if (!top || !b) return;                /* no room: not shown */
	memset(b, 0, sizeof(*b));
	b->mb_text = data;
	b->stack = top;
	b->mb_width = 300;
	b->mb_height = 50;
	b->mb_key_max = 1;
	b->running = 1;
	b->mb_funct = NULL;
	mb_cur = b;
	int tid = kos_thread_create(start, top);
	if (tid == -1) {
		b->running = 0;
		return;
	}
	b->tid = tid;
}

/* MsgBoxReInit (exported): the box created last */
void mb_reinit(char *data)
{
	if (mb_cur) reinit(mb_cur, data);
}

/* MsgBoxSetFunctions: the box created last */
void mb_setfunctions(mb_function *funcs)
{
	if (mb_cur) mb_cur->mb_funct = funcs;
}
