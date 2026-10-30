/* phone: calls and SMS over the modem (function 100, lib/kos_modem.h), in
 * the Hildon look.
 *
 *   Phone      the dialler: the number, the keypad, Call
 *   the call   who, its state or how long it runs; Answer and Reject, or
 *              End (calls have no sound under KolibriOS yet)
 *   Messages   the conversations by number, newest first; one of them as
 *              bubbles (received on the left, sent on the right), a line
 *              to write in and Send; New message asks for the number first
 *
 * Finger-sized everywhere; the keyboard types the number and the text,
 * Enter calls or sends, Escape goes back. @hildon starts it when a call
 * comes in. */
#include "kos.h"
#include "kos_modem.h"

#define TABH 56
#define C_BG 0x0c0e12
#define C_TEXT 0xffffff
#define C_DIM 0x9aa0a8
#define C_HI 0x2f7fcf
#define C_TILE 0x23272d
#define C_GREEN 0x2e9e4a
#define C_RED 0xc83c3c
#define C_IN 0x2c3038
#define C_OUT 0x1f5fa8

enum { B_TAB_PHONE = 10, B_TAB_MSG, B_CALL, B_DEL, B_ANSWER, B_REJECT, B_END, B_NEW, B_BACK,
       B_SEND, B_NUMBER, B_TEXT, B_SPEAKER, B_KEY = 40, B_THREAD = 100 };

enum { V_PHONE, V_LIST, V_THREAD };
static int view = V_PHONE;
static int W, H;                       /* the work area */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
static struct kos_modem mdm;
static struct kos_call call;
static u32 shown_sms_seq = ~0u, shown_call_seq = ~0u, shown_secs = ~0u;

static char dial[32];
static char thread_no[32];             /* the conversation open, "" for a new one */
static char to_no[32];                 /* a new message's number */
static char text[600];
static int focus_number;               /* a new message: typing the number */

#define MAXSMS 96
static struct kos_sms msgs[MAXSMS];
static int nmsgs;

struct thr { char number[32]; int last, unread, count; };
static struct thr thr[MAXSMS];
static int nthr;

#ifndef NULL
#define NULL ((void *)0)
#endif

static int clen(const char *s)         /* characters (UTF-8 code points) */
{
	int n = 0;
	for (; *s; s++)
		if ((*s & 0xc0) != 0x80) n++;
	return n;
}

static void text_at(int x, int y, const char *s, u32 c, int scale)
{
	if (scale > 1) kos_text_scaled(x, y, s, c, 3, scale);
	else kos_text(x, y, s, c, 3);
}

static void text_center(int cx, int y, const char *s, u32 c, int scale)
{
	text_at(cx - clen(s) * 8 * scale / 2, y, s, c, scale);
}

static int isqrt(int v)
{
	int r = 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

static void rounded(int x, int y, int w, int h, int r, u32 c)
{
	if (r * 2 > h) r = h / 2;
	for (int i = 0; i < h; i++) {
		int d = 0;
		if (i < r) d = r - isqrt(r * r - (r - i) * (r - i));
		else if (i >= h - r) d = r - isqrt(r * r - (i - (h - r) + 1) * (i - (h - r) + 1));
		kos_rect(x + d, y + i, w - 2 * d, 1, c);
	}
}

static void button(int x, int y, int w, int h, int id, u32 c, const char *label, int scale)
{
	rounded(x, y, w, h, 12, c);
	kos_button_hidden(x, y, w, h, id);
	if (label) text_center(x + w / 2, y + (h - 16 * scale) / 2, label, C_TEXT, scale);
}

/* a handset, for the Call buttons */
static void handset(int cx, int cy, int s, u32 c)
{
	for (int i = -s; i <= s; i++) {
		int dy = isqrt(s * s - i * i) / 3;
		kos_rect(cx + i, cy + s / 3 - dy, 1, dy + 3, c);
	}
	kos_rect(cx - s, cy - s / 3, s / 2, s / 2 + 2, c);
	kos_rect(cx + s / 2, cy - s / 3, s / 2, s / 2 + 2, c);
}

static int name_eq(const char *a, const char *b)
{
	/* numbers alike when their digits are (a + and spaces aside) */
	for (;;) {
		while (*a && (*a < '0' || *a > '9')) a++;
		while (*b && (*b < '0' || *b > '9')) b++;
		if (!*a || !*b) return !*a && !*b;
		if (*a++ != *b++) return 0;
	}
}

/* --- the modem's data ------------------------------------------------- */

static void load_msgs(void)
{
	nmsgs = 0;
	for (u32 i = 0; i < mdm.sms_count && nmsgs < MAXSMS; i++)
		if (kos_call(100, KOS_MODEM_SMS, i, (u32)&msgs[nmsgs], 0, 0) == 0) nmsgs++;
	nthr = 0;
	for (int i = 0; i < nmsgs; i++) {
		int t = -1;
		for (int j = 0; j < nthr && t < 0; j++)
			if (name_eq(thr[j].number, msgs[i].number)) t = j;
		if (t < 0) {
			t = nthr++;
			strcpy(thr[t].number, msgs[i].number);
			thr[t].unread = thr[t].count = 0;
		}
		thr[t].last = i;
		thr[t].count++;
		if (msgs[i].dir == KOS_SMS_IN && msgs[i].state == KOS_SMS_UNREAD) thr[t].unread++;
	}
	/* newest first */
	for (int i = 1; i < nthr; i++)
		for (int j = i; j > 0 && thr[j].last > thr[j - 1].last; j--) {
			struct thr x = thr[j];
			thr[j] = thr[j - 1];
			thr[j - 1] = x;
		}
}

static void mark_thread_read(void)
{
	for (int i = 0; i < nmsgs; i++)
		if (msgs[i].dir == KOS_SMS_IN && msgs[i].state == KOS_SMS_UNREAD && name_eq(msgs[i].number, thread_no)) {
			kos_call(100, KOS_MODEM_READ, i, 0, 0, 0);
			msgs[i].state = KOS_SMS_READ;
		}
}

static void time_text(u32 t, char *s)
{
	if (!t) { s[0] = 0; return; }
	t += 3600;                                 /* Central European time (no time zone yet) */
	u32 d = t / 86400, sec = t % 86400;
	int z = d + 719468, era = z / 146097, doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	int day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9;
	ksprintf(s, "%d.%d. %02u:%02u", day, mon, sec / 3600, sec / 60 % 60);
}

/* --- drawing ------------------------------------------------------------ */

static void signal_bars(int x, int y)
{
	int bars = mdm.signal == 0 ? 0 : mdm.signal > -75 ? 4 : mdm.signal > -85 ? 3 : mdm.signal > -95 ? 2 : 1;
	if (mdm.registration != KOS_REG_HOME && mdm.registration != KOS_REG_ROAMING) bars = 0;
	for (int i = 0; i < 4; i++)
		kos_rect(x + i * 9, y + 20 - (i + 1) * 5, 6, (i + 1) * 5, i < bars ? C_TEXT : 0x404448);
}

static void tabs(void)
{
	kos_rect(0, 0, W, TABH, 0x15181c);
	int tw = 200;
	const char *names[2] = { "Phone", "Messages" };
	for (int i = 0; i < 2; i++) {
		int sel = (i == 0) == (view == V_PHONE);
		if (sel) kos_rect(i * tw, TABH - 5, tw, 5, C_HI);
		kos_button_hidden(i * tw, 0, tw, TABH, B_TAB_PHONE + i);
		char l[24];
		if (i == 1 && mdm.sms_unread) ksprintf(l, "Messages (%u)", mdm.sms_unread);
		else strcpy(l, names[i]);
		text_center(i * tw + tw / 2, TABH / 2 - 10, l, sel ? C_TEXT : C_DIM, 1);
	}
	char st[80];
	if (mdm.state == KOS_MODEM_READY && (mdm.registration == KOS_REG_HOME || mdm.registration == KOS_REG_ROAMING))
		ksprintf(st, "%s%s", mdm.operator_name[0] ? mdm.operator_name : "registered",
			 mdm.registration == KOS_REG_ROAMING ? " (roaming)" : "");
	else if (mdm.state == KOS_MODEM_READY)
		strcpy(st, mdm.registration == KOS_REG_SEARCHING ? "searching for a network" :
		       mdm.registration == KOS_REG_DENIED ? "network denied" : "no network");
	else
		strcpy(st, mdm.status[0] ? mdm.status : "no modem");
	if (mdm.fake) strcpy(st + strlen(st), " - test modem");
	int sx = W - 60 - clen(st) * 8;
	if (sx < 2 * tw + 10) sx = 2 * tw + 10;
	text_at(sx, TABH / 2 - 8, st, C_DIM, 1);
	signal_bars(W - 50, TABH / 2 - 12);
}

static void draw_phone(void)
{
	int top = TABH + 12, lw = W * 9 / 20;
	/* the number */
	rounded(16, top, lw - 32, 76, 10, C_TILE);
	kos_button_hidden(16, top, lw - 32, 76, B_NUMBER);
	int sc = clen(dial) * 24 > lw - 60 ? 2 : 3;
	if (dial[0]) text_at(32, top + (76 - 16 * sc) / 2, dial, C_TEXT, sc);
	else text_at(32, top + 30, "the number", C_DIM, 1);
	/* Call and delete */
	int by = top + 96, bh = 76;
	rounded(16, by, (lw - 48) * 2 / 3, bh, 14, dial[0] ? C_GREEN : 0x2a4a32);
	kos_button_hidden(16, by, (lw - 48) * 2 / 3, bh, B_CALL);
	handset(16 + (lw - 48) / 3 - 40, by + bh / 2, 18, C_TEXT);
	text_at(16 + (lw - 48) / 3 - 10, by + bh / 2 - 8, "Call", C_TEXT, 1);
	int dx = 16 + (lw - 48) * 2 / 3 + 16, dw = lw - 16 - dx;
	button(dx, by, dw, bh, B_DEL, C_TILE, "<-", 2);
	if (mdm.state != KOS_MODEM_READY) text_at(16, by + bh + 20, "The modem is not ready.", C_DIM, 1);
	/* the keypad */
	static const char *const keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
	static const char *const sub[12] = { "", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", "", "+", "" };
	int kx = lw, kw = (W - lw - 16) / 3, kh = (H - top - 8) / 4;
	for (int i = 0; i < 12; i++) {
		int x = kx + (i % 3) * kw, y = top + (i / 3) * kh;
		rounded(x + 4, y + 4, kw - 8, kh - 8, 10, C_TILE);
		kos_button_hidden(x + 4, y + 4, kw - 8, kh - 8, B_KEY + i);
		text_center(x + kw / 2, y + kh / 2 - 22, keys[i], C_TEXT, 2);
		if (sub[i][0]) text_center(x + kw / 2, y + kh / 2 + 14, sub[i], C_DIM, 1);
	}
}

static void draw_call(void)
{
	static const char *const what[] = { "", "Calling", "Ringing", "Incoming call", "", "On hold", "Call ended" };
	char st[40];
	if (call.state == KOS_CALL_ACTIVE) ksprintf(st, "%02u:%02u", call.seconds / 60, call.seconds % 60);
	else strcpy(st, call.state < 7 ? what[call.state] : "");
	int cy = TABH + (H - TABH) / 4;
	text_center(W / 2, cy - 40, st, call.state == KOS_CALL_INCOMING ? 0x7fd08f : C_DIM, 2);
	const char *who = call.number[0] ? call.number : "unknown number";
	text_center(W / 2, cy + 10, who, C_TEXT, clen(who) * 24 < W - 40 ? 3 : 2);
}

static void draw_call_buttons(void)
{
	int bh = 90, by = H - bh - 40, bw = W / 3;
	if (call.state == KOS_CALL_INCOMING) {
		rounded(W / 2 - bw - 20, by, bw, bh, 18, C_GREEN);
		kos_button_hidden(W / 2 - bw - 20, by, bw, bh, B_ANSWER);
		handset(W / 2 - bw / 2 - 20 - 50, by + bh / 2, 18, C_TEXT);
		text_at(W / 2 - bw / 2 - 20 - 20, by + bh / 2 - 16, "Answer", C_TEXT, 2);
		button(W / 2 + 20, by, bw, bh, B_REJECT, C_RED, "Reject", 2);
	} else if (call.state != KOS_CALL_ENDED) {
		int sound = mdm.audio == KOS_AUDIO_PREPARED;
		int sw = sound ? bw * 3 / 4 : 0, gap = sound ? 40 : 0, x = W / 2 - (bw + gap + sw) / 2;
		button(x, by, bw, bh, B_END, C_RED, "End call", 2);
		/* the loudspeaker (the earpiece otherwise): lit while on */
		if (sound)
			button(x + bw + gap, by, sw, bh, B_SPEAKER,
				mdm.audio_route == KOS_ROUTE_SPEAKER ? C_HI : C_TILE, "Speaker", 2);
	}
	if (mdm.audio != KOS_AUDIO_PREPARED)
		text_center(W / 2, H - 30, "No sound: start KolibriOS with pro1x-kexec.sh -a", C_DIM, 1);
}

/* the conversations */
static void draw_list(void)
{
	int y = TABH + 8, rh = 72;
	button(16, y, W - 32, rh - 12, B_NEW, C_TILE, "+  New message", 1);
	y += rh;
	if (!nthr)
		text_center(W / 2, y + 30, mdm.state == KOS_MODEM_READY ? "No messages" : "", C_DIM, 1);
	for (int i = 0; i < nthr && y + rh <= H; i++, y += rh) {
		const struct kos_sms *s = &msgs[thr[i].last];
		kos_rect(16, y + rh - 1, W - 32, 1, 0x30343a);
		kos_button_hidden(16, y, W - 32, rh - 2, B_THREAD + i);
		text_at(24, y + 8, thr[i].number, C_TEXT, 2);
		char tt[24];
		time_text(s->time, tt);
		text_at(W - 32 - clen(tt) * 8, y + 12, tt, C_DIM, 1);
		/* the last message, one line */
		char line[160];
		int max = (W - 120) / 8, n = 0;
		const char *p = s->text;
		if (s->dir == KOS_SMS_OUT) { strcpy(line, "You: "); n = 5; }
		int o = n;
		for (; *p && n < max; p++) {
			if ((*p & 0xc0) != 0x80) n++;
			line[o++] = *p == '\n' ? ' ' : *p;
		}
		while (*p && (*p & 0xc0) == 0x80) line[o++] = *p++;
		line[o] = 0;
		text_at(24, y + 44, line, thr[i].unread ? C_TEXT : C_DIM, 1);
		if (thr[i].unread) {
			char u[8];
			ksprintf(u, "%d", thr[i].unread);
			rounded(W - 60, y + 38, 30, 26, 13, C_HI);
			text_center(W - 45, y + 43, u, C_TEXT, 1);
		}
	}
}

/* a bubble's lines: up to cols characters each, broken at spaces */
static int wrap(const char *s, int cols, const char **starts, int *lens, int max)
{
	int n = 0;
	while (*s && n < max) {
		const char *line = s, *brk = NULL;
		int c = 0;
		const char *p = s;
		while (*p && *p != '\n' && c < cols) {
			if (*p == ' ') brk = p;
			p++;
			while (*p && (*p & 0xc0) == 0x80) p++;
			c++;
		}
		if (*p && *p != '\n' && brk && brk > line) p = brk;
		starts[n] = line;
		lens[n] = p - line;
		n++;
		s = p;
		if (*s == ' ' || *s == '\n') s++;
	}
	return n;
}

static void draw_thread(void)
{
	/* the title row: back, the number (a new message: the number field) */
	int y = TABH;
	kos_rect(0, y, W, 64, 0x15181c);
	button(8, y + 8, 80, 48, B_BACK, C_TILE, "<", 2);
	if (thread_no[0]) {
		text_at(104, y + 16, thread_no, C_TEXT, 2);
	} else {
		rounded(104, y + 8, W - 120, 48, 10, focus_number ? 0x2a3a50 : C_TILE);
		kos_button_hidden(104, y + 8, W - 120, 48, B_NUMBER);
		text_at(116, y + 24, to_no[0] ? to_no : "To: the number", to_no[0] ? C_TEXT : C_DIM, 1);
	}
	/* the compose line */
	int ch = 64, cy = H - ch;
	kos_rect(0, cy, W, ch, 0x15181c);
	rounded(8, cy + 8, W - 160, ch - 16, 10, !focus_number ? 0x2a3a50 : C_TILE);
	kos_button_hidden(8, cy + 8, W - 160, ch - 16, B_TEXT);
	{
		int cols = (W - 190) / 8, n = clen(text);
		const char *p = text;
		while (n > cols) {                       /* the end of what is typed */
			p++;
			while (*p && (*p & 0xc0) == 0x80) p++;
			n--;
		}
		text_at(20, cy + ch / 2 - 8, text[0] ? p : "Text", text[0] ? C_TEXT : C_DIM, 1);
	}
	button(W - 144, cy + 8, 136, ch - 16, B_SEND, text[0] ? C_HI : C_TILE, "Send", 1);
	/* the bubbles, the newest at the bottom */
	int top = TABH + 72, bottom = cy - 8;
	int bw = W * 2 / 3, cols = (bw - 28) / 8;
	for (int i = nmsgs - 1; i >= 0 && thread_no[0]; i--) {
		const struct kos_sms *s = &msgs[i];
		if (!name_eq(s->number, thread_no)) continue;
		const char *st[24];
		int ln[24];
		int nl = wrap(s->text, cols, st, ln, 24);
		int widest = 0;
		for (int k = 0; k < nl; k++) {
			char tmp[200];
			int l = MIN(ln[k], 199);
			memcpy(tmp, st[k], l);
			tmp[l] = 0;
			if (clen(tmp) > widest) widest = clen(tmp);
		}
		char tt[40];
		time_text(s->time, tt);
		if (s->dir == KOS_SMS_OUT && s->state == KOS_SMS_SENDING) strcpy(tt, "sending...");
		if (s->dir == KOS_SMS_OUT && s->state == KOS_SMS_FAILED) strcpy(tt, "not sent");
		int w = (widest > clen(tt) ? widest : clen(tt)) * 8 + 28, h = nl * 18 + 36;
		bottom -= h + 8;
		if (bottom < top) break;
		int x = s->dir == KOS_SMS_OUT ? W - w - 16 : 16;
		rounded(x, bottom, w, h, 12, s->dir == KOS_SMS_OUT ? C_OUT : C_IN);
		for (int k = 0; k < nl; k++) {
			char tmp[200];
			int l = MIN(ln[k], 199);
			memcpy(tmp, st[k], l);
			tmp[l] = 0;
			text_at(x + 14, bottom + 8 + 18 * k, tmp, C_TEXT, 1);
		}
		text_at(x + 14, bottom + h - 24, tt, s->state == KOS_SMS_FAILED ? 0xff9090 : 0xb8c4d0, 0);
	}
}

static void draw(void)
{
	kos_begin_draw();
	kos_window(0, 0, 800, 480, KOS_WIN_FIXED | C_BG, "Phone");
	char info[1024];
	kos_thread_info(info, -1);
	W = *(u32 *)(info + 62) + 1;
	H = *(u32 *)(info + 66) + 1;
	kos_rect(0, 0, W, H, C_BG);
	if (call.state != KOS_CALL_NONE) {
		kos_rect(0, 0, W, TABH, 0x15181c);
		text_at(16, TABH / 2 - 8, mdm.operator_name, C_DIM, 1);
		draw_call();
		draw_call_buttons();
	} else {
		tabs();
		if (view == V_PHONE) draw_phone();
		else if (view == V_LIST) draw_list();
		else draw_thread();
	}
	kos_end_draw();
}

/* --- input ------------------------------------------------------------------ */

static void append(char *s, int max, int c)
{
	int n = strlen(s);
	if (n + 2 < max) { s[n] = c; s[n + 1] = 0; }
}

static void backspace(char *s)
{
	int n = strlen(s);
	while (n > 0 && (s[n - 1] & 0xc0) == 0x80) n--;
	if (n > 0) n--;
	s[n] = 0;
}

static void send_text(void)
{
	const char *to = thread_no[0] ? thread_no : to_no;
	if (!to[0] || !text[0]) return;
	if (kos_call(100, KOS_MODEM_SEND, (u32)to, (u32)text, 0, 0) == 0) {
		if (!thread_no[0]) strcpy(thread_no, to_no);
		text[0] = 0;
	}
}

static void open_thread(int i)
{
	strcpy(thread_no, thr[i].number);
	view = V_THREAD;
	focus_number = 0;
	mark_thread_read();
}

static void on_button(int id)
{
	if (id == 1) kos_exit();
	if (id == B_TAB_PHONE) view = V_PHONE;
	else if (id == B_TAB_MSG) view = V_LIST;
	else if (id >= B_KEY && id < B_KEY + 12) append(dial, sizeof(dial), "123456789*0#"[id - B_KEY]);
	else if (id == B_DEL) backspace(dial);
	else if (id == B_CALL && dial[0]) kos_call(100, KOS_MODEM_DIAL, (u32)dial, 0, 0, 0);
	else if (id == B_ANSWER) kos_call(100, KOS_MODEM_ANSWER, 0, 0, 0, 0);
	else if (id == B_REJECT || id == B_END) kos_call(100, KOS_MODEM_HANGUP, 0, 0, 0, 0);
	else if (id == B_SPEAKER)
		kos_call(100, KOS_MODEM_ROUTE,
			mdm.audio_route == KOS_ROUTE_SPEAKER ? KOS_ROUTE_EARPIECE : KOS_ROUTE_SPEAKER, 0, 0, 0);
	else if (id == B_NEW) { view = V_THREAD; thread_no[0] = 0; to_no[0] = 0; focus_number = 1; }
	else if (id == B_BACK) view = V_LIST;
	else if (id == B_SEND) send_text();
	else if (id == B_NUMBER && view == V_THREAD) focus_number = 1;
	else if (id == B_TEXT) focus_number = 0;
	else if (id >= B_THREAD && id < B_THREAD + nthr) open_thread(id - B_THREAD);
	draw();
}

static void on_key(u32 k)
{
	if (k & 0xff) return;
	int c = (k >> 8) & 0xff;
	if (call.state != KOS_CALL_NONE) {
		if (c == 13 && call.state == KOS_CALL_INCOMING) kos_call(100, KOS_MODEM_ANSWER, 0, 0, 0, 0);
		if (c == 27) kos_call(100, KOS_MODEM_HANGUP, 0, 0, 0, 0);
		if (c == 's' || c == 'S') on_button(B_SPEAKER);
		return;
	}
	if (view == V_PHONE) {
		if ((c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+') append(dial, sizeof(dial), c);
		else if (c == 8) backspace(dial);
		else if (c == 13 && dial[0]) kos_call(100, KOS_MODEM_DIAL, (u32)dial, 0, 0, 0);
		else if (c == 9) view = V_LIST;
	} else if (view == V_LIST) {
		if (c == 9) view = V_PHONE;
		else if (c == 'n') on_button(B_NEW);
		else if (c == 13 && nthr) open_thread(0);
	} else {
		char *f = focus_number ? to_no : text;
		int max = focus_number ? (int)sizeof(to_no) : (int)sizeof(text);
		if (c == 27) view = V_LIST;
		else if (c == 8) backspace(f);
		else if (c == 9 && !thread_no[0]) focus_number = !focus_number;
		else if (c == 13) {
			if (focus_number) focus_number = 0;
			else send_text();
		} else if (c >= 32 && c < 127) {
			if (!focus_number || (c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#')
				append(f, max, c);
		}
	}
	draw();
}

static void refresh(int force)
{
	kos_call(100, KOS_MODEM_STATUS, (u32)&mdm, 0, 0, 0);
	kos_call(100, KOS_MODEM_CALL, (u32)&call, 0, 0, 0);
	int redraw = force;
	if (mdm.sms_seq != shown_sms_seq) {
		shown_sms_seq = mdm.sms_seq;
		load_msgs();
		if (view == V_THREAD && thread_no[0]) mark_thread_read();
		redraw = 1;
	}
	if (mdm.call_seq != shown_call_seq) {
		shown_call_seq = mdm.call_seq;
		redraw = 1;
		/* a call coming in: in front */
		if (call.state == KOS_CALL_INCOMING) {
			char info[1024];
			kos_thread_info(info, -1);
			kos_activate_slot(kos_call(18, 21, *(u32 *)(info + 30), 0, 0, 0));
		}
	}
	if (call.state == KOS_CALL_ACTIVE && call.seconds != shown_secs) {
		shown_secs = call.seconds;
		redraw = 1;
	}
	if (redraw) draw();
}

/* calls and messages come with the kernel's EV_SYSTEM; the time looked
 * at only for what changes without it: a call's seconds (every second),
 * the status line's signal and registration (5 s, the modem's own poll),
 * nothing while the display is off */
static u32 next_wait(void)
{
	if (kos_system_state() & KOS_SYS_DISPLAY_OFF) return 0;
	return call.state == KOS_CALL_ACTIVE ? 100 : 500;
}

int main(void)
{
	kos_event_mask(EVM_REDRAW | EVM_KEY | EVM_BUTTON | EVM_SYSTEM);
	refresh(1);
	u32 status_seq = 0;
	for (;;) {
		u32 w = next_wait();
		int ev = w ? kos_wait_event_timeout(w) : kos_wait_event();
		switch (ev) {
		case KOS_EV_REDRAW: draw(); break;
		case KOS_EV_KEY: on_key(kos_get_key()); break;
		case KOS_EV_BUTTON: on_button(kos_get_button() >> 8); break;
		case KOS_EV_SYSTEM:
		case 0: {
			struct kos_modem m2;
			kos_call(100, KOS_MODEM_STATUS, (u32)&m2, 0, 0, 0);
			/* the status line: redrawn when it changes */
			u32 h = m2.state * 131 + m2.registration * 17 + (u32)m2.signal + m2.sms_unread;
			for (const char *p = m2.status; *p; p++) h = h * 31 + *p;
			refresh(h != status_seq);
			status_seq = h;
			break;
		}
		}
	}
}
