/* messenger: WhatsApp, Telegram and Signal for KolibriOS, in the Hildon
 * look. Each through its backend, a Linux program started through
 * function 101, speaking JSON lines (docs/messengers.md):
 *
 *  - WhatsApp (parameter "whatsapp"): Yappari's daemon (/bin/yappari-wa
 *    -stdio), linked by QR code (WhatsApp on the main phone: Linked
 *    devices, Link a device);
 *  - Telegram ("telegram"): libpurple with tdlib-purple (/bin/purple-host
 *    -stdio): the phone number, then what Telegram asks (the login code,
 *    the password); "telegram NUMBER" logs in with that number at once;
 *  - Signal ("signal"): presage (/bin/signal-host -stdio), linked as a
 *    secondary device by QR code (Signal on the phone: Settings, Linked
 *    devices).
 *
 * Without a parameter it asks which. Once linked:
 *
 *   the chats    the conversations, newest first: the name, the last
 *                message, its time, the unread ones as a number
 *   a chat       its messages as bubbles (received on the left, sent on
 *                the right, with their time and how far they got), a line
 *                to write in and Send; new ones appear as they come
 *
 * The keyboard: Up/Down choose a chat, Enter opens it; in a chat the keys
 * type, Enter sends, Up/Down and Page Up/Down scroll, Escape goes back;
 * Ctrl+Q ends the program. Everything is a finger wide for the touch
 * screen too. The chats (with their newest messages) are kept per service
 * in /home/user/.local/share/messenger/SERVICE/chats.json (the Linux
 * personality's RAM file system: kept while KolibriOS runs, lost when it
 * ends, as the backends' own stores). The backends' lines go to the debug
 * board too. */
#include "kos.h"
#include "qr.h"
#include "json.h"

typedef _Bool bool;
#define true 1
#define false 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define TABH 56
#define C_BG 0x0c0e12
#define C_TEXT 0xffffff
#define C_DIM 0x9aa0a8
#define C_HI 0x2f7fcf
#define C_TILE 0x23272d
#define C_BAR 0x15181c
#define C_IN 0x2c3038
#define C_OUT 0x1f5fa8
#define C_ERR 0xff7070
#define C_WHO 0x8fc3ff

enum { B_QR = 10, B_BACK, B_SEND, B_TEXT, B_OLDER, B_NEWER, B_PICK = 20, B_CHAT = 100 };

enum { S_NONE = -1, S_WA, S_TG, S_SG };
static const char *const svc_name[3] = { "WhatsApp", "Telegram", "Signal" };
static const char *const svc_dir[3] = { "whatsapp", "telegram", "signal" };
static const char *const svc_bin[3] = { "/bin/yappari-wa", "/bin/purple-host", "/bin/signal-host" };
static int svc = S_NONE, pick;

#define DATA_DIR "/home/user/.local/share/messenger"
static char dir[96], file[128];

/* --- the backend ---------------------------------------------------------- */

static int handle = -1;
static char state[48] = "starting";
static bool ended, asked_lists;
static int next_id = 1, status_id = -1, qr_id = -1, contacts_id = -1, groups_id = -1;
static char line[1 << 18];
static unsigned nline;
static struct jv jn[8192];
static char jsb[1 << 18];
static struct json J;

static int lx_start(const char *path, const char *args) { return kos_call(101, 1, (u32)path, (u32)args, 0, 0); }
static int lx_read(char *b, unsigned n) { return kos_call(101, 3, handle, (u32)b, n, 0); }

static void lx_write_all(const char *s, unsigned n)
{
	for (int tries = 0; n && tries < 200; tries++) {
		int w = kos_call(101, 2, handle, (u32)s, n, 0);
		if (w < 0) return;
		s += w, n -= w;
		if (n) kos_delay(1);
	}
}

/* a call: its arguments one by one */
static char cbuf[2048];
static int cn, cargs;

static int call_begin(const char *method)
{
	int id = next_id++;
	cn = ksprintf(cbuf, "{\"id\":%d,\"call\":\"%s\",\"args\":[", id, method);
	cargs = 0;
	return id;
}

static void arg_str(const char *s)
{
	if (cargs++) cbuf[cn++] = ',';
	cn += json_quote(cbuf + cn, sizeof(cbuf) - cn - 16, s);
}

static void arg_list1(const char *s)        /* ["s"] */
{
	if (cargs++) cbuf[cn++] = ',';
	cbuf[cn++] = '[';
	cn += json_quote(cbuf + cn, sizeof(cbuf) - cn - 16, s);
	cbuf[cn++] = ']';
}

static void call_end(void)
{
	if (handle < 0 || ended) return;
	cn += ksprintf(cbuf + cn, "]}\n");
	lx_write_all(cbuf, cn);
}

static int call0(const char *method)
{
	int id = call_begin(method);
	call_end();
	return id;
}

static void debug_line(const char *s)
{
	static const char *const tag[3] = { "[wa] ", "[tg] ", "[sg] " };
	for (const char *p = svc >= 0 ? tag[svc] : "[--] "; *p; p++) kos_debug_char(*p);
	for (unsigned i = 0; s[i] && i < 200; i++) kos_debug_char(s[i]);
	kos_debug_char('\n');
}

/* --- text helpers ----------------------------------------------------------- */

static int clen(const char *s)              /* characters (UTF-8 code points) */
{
	int n = 0;
	for (; *s; s++)
		if ((*s & 0xc0) != 0x80) n++;
	return n;
}

/* at most max characters of s into out (size bytes), "..." when cut */
static void clip(char *out, unsigned size, const char *s, int max)
{
	unsigned o = 0;
	int n = 0;
	for (; *s && o + 5 < size; s++) {
		if ((*s & 0xc0) != 0x80) {
			if (n == max) break;
			n++;
		}
		out[o++] = *s == '\n' || *s == '\t' ? ' ' : *s;
	}
	if (*s && max > 3 && o >= 3) {
		while (o && n > max - 3) {         /* room for the dots */
			o--;
			if ((out[o] & 0xc0) != 0x80) n--;
		}
		out[o++] = '.', out[o++] = '.', out[o++] = '.';
	}
	out[o] = 0;
}

static void copy(char *d, const char *s, unsigned size)
{
	unsigned i = 0;
	for (; s[i] && i + 1 < size; i++) d[i] = s[i];
	while (i && (d[i] & 0xc0) == 0x80 && (s[i] & 0xc0) == 0x80) i--;   /* no half character */
	d[i] = 0;
}

static bool prefix(const char *s, const char *p)
{
	while (*p && *s == *p) s++, p++;
	return !*p;
}

static bool contains(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c) return true;
	return false;
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

static void button(int x, int y, int w, int h, int id, u32 c, const char *label)
{
	rounded(x, y, w, h, 12, c);
	kos_button_hidden(x, y, w, h, id);
	if (label) text_center(x + w / 2, y + (h - 16) / 2, label, C_TEXT, 1);
}

/* --- time ------------------------------------------------------------------ */

static u32 bcd(u32 v) { return (v >> 4 & 15) * 10 + (v & 15); }

static u32 days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = y / 400, yoe = y - era * 400;
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (u32)(era * 146097 + doe - 719468);
}

static void civil(u32 days, int *y, int *m, int *d)
{
	int z = days + 719468, era = z / 146097, doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

/* Unix time from the clock (functions 3 and 29: UTC) */
static u32 now(void)
{
	u32 t = kos_time(), d = kos_date();
	u32 days = days_from_civil(2000 + bcd(d & 0xff), bcd(d >> 8 & 0xff), bcd(d >> 16 & 0xff));
	return days * 86400 + bcd(t & 0xff) * 3600 + bcd(t >> 8 & 0xff) * 60 + bcd(t >> 16 & 0xff);
}

#define TZ 3600                             /* Central European time (no time zone yet), as phone */

/* "14:05" today, "29.10." before (the list); with the day and the time
 * (a bubble) */
static void time_text(u32 t, char *s, bool full)
{
	if (!t) { s[0] = 0; return; }
	u32 lt = t + TZ, today = (now() + TZ) / 86400;
	int y, m, d;
	civil(lt / 86400, &y, &m, &d);
	u32 sec = lt % 86400;
	if (lt / 86400 == today) ksprintf(s, "%02u:%02u", sec / 3600, sec / 60 % 60);
	else if (full) ksprintf(s, "%d.%d. %02u:%02u", d, m, sec / 3600, sec / 60 % 60);
	else ksprintf(s, "%d.%d.", d, m);
}

/* --- the chats -------------------------------------------------------------- */

#define MAXCHATS 80
#define MAXMSG 40
#define CIDLEN 80
#define MIDLEN 48
#define NAMELEN 48
#define TEXTLEN 400

enum { ST_IN, ST_SENDING, ST_SENT, ST_DELIVERED, ST_READ, ST_FAILED };

struct msg {
	char id[MIDLEN];
	char from[CIDLEN];                      /* the sender (groups, read receipts) */
	char who[NAMELEN];                      /* the sender's name as it came */
	char text[TEXTLEN];
	u32 time;
	u8 out, status;
};

struct chat {
	char id[CIDLEN];
	char name[NAMELEN];
	u8 named;                               /* 0 none, 1 the sender's own name, 2 a contact's or group's */
	u8 group;
	u32 time;
	int unread, n;
	u32 typing_until;
	struct msg m[MAXMSG];                   /* oldest first */
};

static struct chat chats[MAXCHATS];
static int nchats, ord[MAXCHATS];
static int open_chat = -1, lsel, ltop, scroll;
static bool dirty;
static u32 saved_at;

/* the names: contacts and groups (GetContacts, GetGroups) */
#define MAXNAMES 1500
static struct { char id[CIDLEN]; char name[NAMELEN]; } names[MAXNAMES];
static int nnames;

static const char *name_of(const char *id)
{
	for (int i = 0; i < nnames; i++)
		if (!strcmp(names[i].id, id)) return names[i].name;
	return NULL;
}

static void name_add(const char *id, const char *name)
{
	if (!*id || !*name) return;
	for (int i = 0; i < nnames; i++)
		if (!strcmp(names[i].id, id)) { copy(names[i].name, name, NAMELEN); return; }
	if (nnames == MAXNAMES) return;
	copy(names[nnames].id, id, CIDLEN);
	copy(names[nnames].name, name, NAMELEN);
	nnames++;
}

static bool is_group(const char *id)
{
	if (svc == S_WA) {
		unsigned n = strlen(id);
		return n > 5 && !strcmp(id + n - 5, "@g.us");
	}
	if (svc == S_TG) return prefix(id, "chat");
	return prefix(id, "group:");
}

/* an address made readable: WhatsApp's number, Signal's id shortened */
static void pretty(const char *id, char *out, unsigned size)
{
	if (svc == S_WA && contains(id, '@') && id[0] >= '0' && id[0] <= '9') {
		unsigned o = 0;
		out[o++] = '+';
		for (; *id && *id != '@' && *id != ':' && o + 1 < size; id++) out[o++] = *id;
		out[o] = 0;
		return;
	}
	if (svc == S_SG && !prefix(id, "group:") && strlen(id) > 8) {
		char s[16];
		memcpy(s, id, 8);
		s[8] = 0;
		ksprintf(out, "Signal %s...", s);
		return;
	}
	copy(out, id, size);
}

static const char *chat_name(struct chat *c)
{
	static char buf[NAMELEN];
	if (c->name[0]) return c->name;
	if (c->group) return "A group";
	pretty(c->id, buf, sizeof(buf));
	return buf;
}

static void sort_chats(void)
{
	for (int i = 0; i < nchats; i++) ord[i] = i;
	for (int i = 1; i < nchats; i++)
		for (int j = i; j > 0 && chats[ord[j]].time > chats[ord[j - 1]].time; j--) {
			int x = ord[j];
			ord[j] = ord[j - 1];
			ord[j - 1] = x;
		}
}

static int chat_find(const char *id)
{
	for (int i = 0; i < nchats; i++)
		if (!strcmp(chats[i].id, id)) return i;
	return -1;
}

static void chat_names_update(struct chat *c)
{
	const char *n = name_of(c->id);
	if (n) {
		copy(c->name, n, NAMELEN);
		c->named = 2;
	}
}

static int chat_get(const char *id)
{
	int i = chat_find(id);
	if (i >= 0 || !*id) return i;
	if (nchats == MAXCHATS) {
		/* the oldest chat makes room (not the one open) */
		int old = -1;
		for (int k = 0; k < nchats; k++)
			if (k != open_chat && (old < 0 || chats[k].time < chats[old].time)) old = k;
		if (old < 0) return -1;
		i = old;
	} else {
		i = nchats++;
	}
	struct chat *c = &chats[i];
	memset(c, 0, sizeof(*c));
	copy(c->id, id, CIDLEN);
	c->group = is_group(id);
	chat_names_update(c);
	return i;
}

static struct msg *msg_find(struct chat *c, const char *id)
{
	if (!*id) return NULL;
	for (int i = 0; i < c->n; i++)
		if (!strcmp(c->m[i].id, id)) return &c->m[i];
	return NULL;
}

/* a message into its chat, in the order of time; the oldest goes when it
 * is full. NULL: it was there already (by its id), or older than all */
static struct msg *msg_add(struct chat *c, const char *id, u32 t)
{
	if (msg_find(c, id)) return NULL;
	int at = c->n;
	while (at > 0 && c->m[at - 1].time > t) at--;
	if (c->n == MAXMSG) {
		if (at == 0) return NULL;
		memcpy(&c->m[0], &c->m[1], sizeof(struct msg) * (MAXMSG - 1));
		c->n--;
		at--;
	}
	for (int k = c->n; k > at; k--) memcpy(&c->m[k], &c->m[k - 1], sizeof(struct msg));
	c->n++;
	struct msg *m = &c->m[at];
	memset(m, 0, sizeof(*m));
	copy(m->id, id, MIDLEN);
	m->time = t;
	if (t > c->time) c->time = t;
	return m;
}

/* the unread ones of the chat open: read, and the backend told so */
static void mark_read(struct chat *c)
{
	if (!c->unread) return;
	for (int i = c->n - 1, left = c->unread; i >= 0 && left > 0; i--) {
		struct msg *m = &c->m[i];
		if (m->out) continue;
		left--;
		if (!m->id[0] || svc != S_WA) continue;       /* WhatsApp's read receipts */
		call_begin("MarkRead");
		arg_str(c->id);
		arg_str(c->group ? m->from : "");
		arg_list1(m->id);
		call_end();
	}
	c->unread = 0;
	dirty = true;
}

/* a message from the backend: Message or History (id, chat, sender,
 * pushname, time, fromMe, kind, text, mime type, size) */
static void got_message(int args, bool live)
{
	const char *id = json_str(&J, json_at(&J, args, 0)), *chat = json_str(&J, json_at(&J, args, 1));
	const char *from = json_str(&J, json_at(&J, args, 2)), *push = json_str(&J, json_at(&J, args, 3));
	u32 t = (u32)json_num(&J, json_at(&J, args, 4));
	bool out = json_true(&J, json_at(&J, args, 5));
	const char *kind = json_str(&J, json_at(&J, args, 6)), *text = json_str(&J, json_at(&J, args, 7));
	int ci = chat_get(chat);
	if (ci < 0) return;
	struct chat *c = &chats[ci];
	/* a message of ours coming back (Telegram shows what was sent): the
	 * one still waiting for its id, by its text */
	if (out && *id && !msg_find(c, id))
		for (int i = c->n - 1; i >= 0; i--) {
			struct msg *w = &c->m[i];
			if (w->out && prefix(w->id, "local-") && !strcmp(w->text, text)) {
				copy(w->id, id, MIDLEN);
				if (w->status < ST_SENT) w->status = ST_SENT;
				dirty = true;
				return;
			}
		}
	struct msg *m = msg_add(c, id, t);
	if (!m) return;
	copy(m->from, from, CIDLEN);
	const char *n = name_of(from);
	copy(m->who, n ? n : *push ? push : "", NAMELEN);
	if (!m->who[0] && !out) pretty(from, m->who, NAMELEN);
	if (strcmp(kind, "text") && *kind) {
		char b[TEXTLEN];
		ksprintf(b, "[%s] ", kind);
		unsigned l = strlen(b);
		copy(b + l, text, TEXTLEN - l);
		copy(m->text, b, TEXTLEN);
	} else {
		copy(m->text, text, TEXTLEN);
	}
	m->out = out;
	m->status = out ? ST_SENT : ST_IN;
	/* a direct chat without a contact's name: the sender's own */
	if (!out && !c->group && c->named < 2 && *push) {
		copy(c->name, push, NAMELEN);
		c->named = 1;
	}
	if (!out && live) {
		c->unread++;
		c->typing_until = 0;
		if (ci == open_chat) mark_read(c);
	}
	dirty = true;
	sort_chats();
}

static void got_receipt(int args)
{
	const char *chat = json_str(&J, json_at(&J, args, 0));
	int ids = json_at(&J, args, 2);
	const char *kind = json_str(&J, json_at(&J, args, 3));
	int ci = chat_find(chat);
	if (ci < 0) return;
	struct chat *c = &chats[ci];
	if (prefix(kind, "read-self") || prefix(kind, "played-self")) {
		if (c->unread) c->unread = 0, dirty = true;
		return;
	}
	int st = !strcmp(kind, "server") ? ST_SENT : !strcmp(kind, "delivered") ? ST_DELIVERED :
		 prefix(kind, "read") || prefix(kind, "played") ? ST_READ : !strcmp(kind, "error") ? ST_FAILED : -1;
	if (st < 0) return;
	for (int k = 0; k < json_len(&J, ids); k++) {
		struct msg *m = msg_find(c, json_str(&J, json_at(&J, ids, k)));
		if (!m || !m->out) continue;
		if (st == ST_FAILED ? m->status < ST_DELIVERED : st > m->status) m->status = st;
		dirty = true;
	}
}

/* sending: the message shown at once ("local-N"), its id from the result */
#define MAXPEND 16
static struct { int call, chat; char local[MIDLEN]; } pend[MAXPEND];
static int local_n;
static char text[TEXTLEN];

static void send_text(void)
{
	if (open_chat < 0 || !text[0] || handle < 0 || ended) return;
	struct chat *c = &chats[open_chat];
	char lid[MIDLEN];
	ksprintf(lid, "local-%d", ++local_n);
	u32 t = now();
	if (c->n && c->m[c->n - 1].time > t) t = c->m[c->n - 1].time;
	struct msg *m = msg_add(c, lid, t);
	if (!m) return;
	copy(m->text, text, TEXTLEN);
	m->out = 1;
	m->status = ST_SENDING;
	int id = call_begin("SendText");
	arg_str("");                            /* the backend makes the message's id */
	arg_str(c->id);
	arg_str(text);
	call_end();
	int p = 0;
	while (p < MAXPEND - 1 && pend[p].call) p++;
	pend[p].call = id;
	pend[p].chat = open_chat;
	copy(pend[p].local, lid, MIDLEN);
	text[0] = 0;
	scroll = 0;
	dirty = true;
	sort_chats();
}

static void send_result(int p, const char *real, const char *error)
{
	struct chat *c = &chats[pend[p].chat];
	struct msg *m = msg_find(c, pend[p].local);
	pend[p].call = 0;
	if (!m) return;
	if (error) {
		m->status = ST_FAILED;
	} else if (*real) {
		if (!msg_find(c, real)) copy(m->id, real, MIDLEN);
	}
	dirty = true;
}

/* --- keeping the chats: JSON lines ---------------------------------------------
 *   ["messenger", 1, SERVICE]
 *   ["chat", ID, NAME, NAMED, UNREAD, TIME]
 *   ["msg", ID, FROM, WHO, TIME, OUT, STATUS, TEXT]       (its chat's, oldest first) */

static char fbuf[1 << 21];

static void save(void)
{
	if (svc < 0) return;
	unsigned n = 0, cap = sizeof(fbuf) - 1024;
	n += ksprintf(fbuf + n, "[\"messenger\",1,\"%s\"]\n", svc_dir[svc]);
	for (int k = 0; k < nchats && n < cap; k++) {
		struct chat *c = &chats[ord[k]];
		n += ksprintf(fbuf + n, "[\"chat\",");
		n += json_quote(fbuf + n, 200, c->id);
		fbuf[n++] = ',';
		n += json_quote(fbuf + n, 200, c->name);
		n += ksprintf(fbuf + n, ",%d,%d,%u]\n", c->named, c->unread, c->time);
		for (int i = 0; i < c->n && n < cap; i++) {
			struct msg *m = &c->m[i];
			n += ksprintf(fbuf + n, "[\"msg\",");
			n += json_quote(fbuf + n, 200, m->id);
			fbuf[n++] = ',';
			n += json_quote(fbuf + n, 200, m->from);
			fbuf[n++] = ',';
			n += json_quote(fbuf + n, 200, m->who);
			n += ksprintf(fbuf + n, ",%u,%s,%d,", m->time, m->out ? "true" : "false", m->status);
			n += json_quote(fbuf + n, 900, m->text);
			n += ksprintf(fbuf + n, "]\n");
		}
	}
	kos_create_folder(dir);
	struct kos_r2 r = kos_write_file(file, fbuf, n);
	if (r.eax) kdebug("messenger: %s not written (%u)\n", file, r.eax);
	else kdebug("messenger: %d chats kept in %s (%u bytes)\n", nchats, file, n);
	dirty = false;
	saved_at = kos_uptime();
}

static void load(void)
{
	struct kos_r2 r = kos_read_file(file, fbuf, sizeof(fbuf) - 1, 0);
	if (r.eax != 0 && r.eax != 6) return;
	fbuf[r.ebx] = 0;
	struct chat *c = NULL;
	for (char *s = fbuf; *s;) {
		char *e = s;
		while (*e && *e != '\n') e++;
		char keep = *e;
		*e = 0;
		int v = json_parse(&J, s);
		const char *kind = json_str(&J, json_at(&J, v, 0));
		if (!strcmp(kind, "chat")) {
			int ci = chat_get(json_str(&J, json_at(&J, v, 1)));
			c = ci >= 0 ? &chats[ci] : NULL;
			if (c) {
				if (c->named < 2) {
					copy(c->name, json_str(&J, json_at(&J, v, 2)), NAMELEN);
					c->named = (u8)json_num(&J, json_at(&J, v, 3));
				}
				c->unread = (int)json_num(&J, json_at(&J, v, 4));
				c->time = (u32)json_num(&J, json_at(&J, v, 5));
			}
		} else if (!strcmp(kind, "msg") && c) {
			struct msg *m = msg_add(c, json_str(&J, json_at(&J, v, 1)), (u32)json_num(&J, json_at(&J, v, 4)));
			if (m) {
				copy(m->from, json_str(&J, json_at(&J, v, 2)), CIDLEN);
				copy(m->who, json_str(&J, json_at(&J, v, 3)), NAMELEN);
				m->out = json_true(&J, json_at(&J, v, 5));
				m->status = (u8)json_num(&J, json_at(&J, v, 6));
				if (m->status == ST_SENDING) m->status = ST_FAILED;   /* it never got an answer */
				copy(m->text, json_str(&J, json_at(&J, v, 7)), TEXTLEN);
			}
		}
		*e = keep;
		s = *e ? e + 1 : e;
	}
	sort_chats();
	kdebug("messenger: %d chats from %s\n", nchats, file);
}

/* --- linking: the QR code, Telegram's questions ------------------------------ */

static char qr_text[512];
static int qr_size;
static unsigned char modules[QR_MAX_SIZE * QR_MAX_SIZE];
static char field[64];                       /* Telegram: what is typed */
static unsigned nfield;
static int ask_id = -1;                      /* Telegram's question (an Input signal) */
static char ask_text[160], err[160];
static const char *test_number;

static void set_qr(const char *t)
{
	copy(qr_text, t, sizeof(qr_text));
	qr_size = *t ? qr_encode(qr_text, strlen(qr_text), modules) : 0;
}

static bool linked(void)
{
	return !strcmp(state, "connected") || !strcmp(state, "connecting") || !strcmp(state, "disconnected");
}

static void ask_lists(void)
{
	if (asked_lists) return;
	asked_lists = true;
	contacts_id = call0("GetContacts");
	groups_id = call0("GetGroups");
}

static void got_state(const char *s)
{
	copy(state, s, sizeof(state));
	if (!strcmp(state, "connected")) {
		err[0] = 0;
		set_qr("");
		ask_lists();
	} else if (!strcmp(state, "connecting")) {
		set_qr("");
	}
}

/* a result's lists of contacts ([id, full name, push name]) or groups ([id, subject]) */
static void got_names(int list, bool groups)
{
	for (int k = 0; k < json_len(&J, list); k++) {
		int e = json_at(&J, list, k);
		const char *id = json_str(&J, json_at(&J, e, 0)), *a = json_str(&J, json_at(&J, e, 1));
		const char *b = groups ? "" : json_str(&J, json_at(&J, e, 2));
		name_add(id, *a ? a : b);
	}
	for (int i = 0; i < nchats; i++) {
		chat_names_update(&chats[i]);
		for (int k = 0; k < chats[i].n; k++) {
			struct msg *m = &chats[i].m[k];
			const char *n = m->out ? NULL : name_of(m->from);
			if (n) copy(m->who, n, NAMELEN);
		}
	}
	dirty = true;
}

static bool handle_line(const char *s)
{
	debug_line(s);
	int root = json_parse(&J, s);
	if (root < 0) return false;
	int sig = json_get(&J, root, "signal"), args = json_get(&J, root, "args");
	if (sig >= 0) {
		const char *name = json_str(&J, sig);
		if (!strcmp(name, "Message") || !strcmp(name, "History")) got_message(args, name[0] == 'M');
		else if (!strcmp(name, "Receipt")) got_receipt(args);
		else if (!strcmp(name, "StateChanged")) got_state(json_str(&J, json_at(&J, args, 0)));
		else if (!strcmp(name, "QRCode")) {
			if (!linked()) set_qr(json_str(&J, json_at(&J, args, 0)));
		} else if (!strcmp(name, "ChatState")) {
			int ci = chat_find(json_str(&J, json_at(&J, args, 0)));
			if (ci >= 0) chats[ci].typing_until = json_true(&J, json_at(&J, args, 2)) ? kos_uptime() + 1500 : 0;
		} else if (!strcmp(name, "Input")) {
			/* Telegram: id, title, primary, secondary, default */
			ask_id = (int)json_num(&J, json_at(&J, args, 0));
			const char *q = json_str(&J, json_at(&J, args, 2));
			if (!*q) q = json_str(&J, json_at(&J, args, 1));
			copy(ask_text, *q ? q : "Telegram asks", sizeof(ask_text));
			nfield = 0;
			field[0] = 0;
		} else if (!strcmp(name, "Error") || !strcmp(name, "Notice")) {
			const char *t = json_str(&J, json_at(&J, args, 0));
			if (!strcmp(name, "Notice") && json_len(&J, args) > 1 && *json_str(&J, json_at(&J, args, 1)))
				t = json_str(&J, json_at(&J, args, 1));
			copy(err, t, sizeof(err));
		} else {
			return false;
		}
		return true;
	}
	int idv = json_get(&J, root, "id");
	if (idv < 0) return false;
	int id = (int)json_num(&J, idv);
	int res = json_get(&J, root, "result"), error = json_get(&J, root, "error");
	if (id == status_id) {
		if (res >= 0) got_state(json_str(&J, json_at(&J, res, 0)));
		if (!strcmp(state, "logged-out")) {
			if (svc != S_TG) qr_id = call0("GetQR");
			else if (test_number) {
				call_begin("Login");
				arg_str(test_number);
				call_end();
			}
		}
		if (linked()) ask_lists();
		return true;
	}
	if (id == qr_id) {
		if (res >= 0) set_qr(json_str(&J, json_at(&J, res, 0)));
		else copy(err, json_str(&J, error), sizeof(err));
		return true;
	}
	if (id == contacts_id || id == groups_id) {
		if (res >= 0) got_names(json_at(&J, res, 0), id == groups_id);
		return true;
	}
	for (int p = 0; p < MAXPEND; p++)
		if (pend[p].call == id) {
			if (error >= 0) copy(err, json_str(&J, error), sizeof(err));
			send_result(p, json_str(&J, json_at(&J, res, 0)), error >= 0 ? json_str(&J, error) : NULL);
			return true;
		}
	if (error >= 0) {
		copy(err, json_str(&J, error), sizeof(err));
		return true;
	}
	return false;
}

static bool poll_backend(void)
{
	bool changed = false;
	for (;;) {
		if (nline + 1 >= sizeof(line)) nline = 0;          /* a line too long: dropped */
		int n = lx_read(line + nline, sizeof(line) - 1 - nline);
		if (n < 0) {
			if (!ended) changed = true;
			ended = true;
			return changed;
		}
		if (!n) return changed;
		nline += n;
		line[nline] = 0;
		char *start = line;
		for (char *nl = start; *nl; nl++) {
			if (*nl != '\n') continue;
			*nl = 0;
			if (handle_line(start)) changed = true;
			start = nl + 1;
		}
		nline = strlen(start);
		memcpy(line, start, nline + 1);
	}
}

static void start_service(int s)
{
	svc = s;
	ksprintf(dir, "%s/%s", DATA_DIR, svc_dir[svc]);
	ksprintf(file, "%s/chats.json", dir);
	kos_set_caption(svc_name[svc]);
	load();
	handle = lx_start(svc_bin[svc], "-stdio");
	if (handle < 0) {
		ksprintf(state, "not there (%s)", svc_bin[svc]);
		ended = true;
	} else {
		status_id = call0("Status");
	}
}

/* --- drawing ------------------------------------------------------------------ */

static int W, H;                             /* the window's inside */

static void bar(const char *title, const char *right)
{
	kos_rect(0, 0, W, TABH, C_BAR);
	text_at(20, TABH / 2 - 16, title, C_TEXT, 2);
	if (right && *right) {
		char r[64];
		clip(r, sizeof(r), right, (W - 20 - clen(title) * 16 - 60) / 8);
		text_at(W - 20 - clen(r) * 8, TABH / 2 - 8, r, err[0] && right == err ? C_ERR : C_DIM, 1);
	}
}

static void draw_pick(void)
{
	bar("Messenger", "");
	text_at(24, TABH + 20, "Which service?", C_DIM, 1);
	for (int i = 0; i < 3; i++) {
		int y = TABH + 52 + i * 84;
		rounded(24, y, W - 48, 72, 14, i == pick ? C_HI : C_TILE);
		kos_button_hidden(24, y, W - 48, 72, B_PICK + i);
		char l[32];
		ksprintf(l, "%d  %s", i + 1, svc_name[i]);
		text_at(48, y + 20, l, C_TEXT, 2);
	}
}

static void draw_qr(int w, int h)
{
	int side = (w < h ? w : h) - 24;
	int s = side / (qr_size + 8);
	if (s < 1) s = 1;
	int x0 = 24, y0 = TABH + 12, q = 4 * s, full = (qr_size + 8) * s;
	kos_rect(x0, y0, full, full, 0xffffff);
	for (int y = 0; y < qr_size; y++)
		for (int x = 0; x < qr_size;) {
			if (!qr_module(modules, qr_size, x, y)) { x++; continue; }
			int e = x;
			while (e < qr_size && qr_module(modules, qr_size, e, y)) e++;
			kos_rect(x0 + q + x * s, y0 + q + y * s, (e - x) * s, s, 0x000000);
			x = e;
		}
	int tx = x0 + full + 24;
	if (tx + 200 > w) tx = 24;
	int ty = tx == 24 ? y0 + full + 16 : y0;
	text_at(tx, ty, "Link this device:", C_TEXT, 1);
	if (svc == S_SG) {
		text_at(tx, ty + 24, "Signal on your phone,", C_DIM, 1);
		text_at(tx, ty + 44, "Settings, Linked devices,", C_DIM, 1);
		text_at(tx, ty + 64, "Link new device, scan the code.", C_DIM, 1);
	} else {
		text_at(tx, ty + 24, "WhatsApp on your phone,", C_DIM, 1);
		text_at(tx, ty + 44, "Linked devices,", C_DIM, 1);
		text_at(tx, ty + 64, "Link a device, scan the code.", C_DIM, 1);
	}
}

static void draw_link(void)
{
	char buf[160];
	bar(svc_name[svc], err[0] ? err : state);
	if (ended) {
		ksprintf(buf, "The %s service ended (%s).", svc_name[svc], state);
		text_at(24, TABH + 24, buf, C_TEXT, 1);
		return;
	}
	if (svc == S_TG) {
		const char *label = ask_id >= 0 ? ask_text : "Your phone number (+43...):";
		text_at(24, TABH + 28, label, C_DIM, 1);
		rounded(24, TABH + 56, MIN(W - 48, 420), 44, 10, 0x2a3a50);
		ksprintf(buf, "%s_", field);
		text_at(40, TABH + 70, buf, C_TEXT, 1);
		text_at(24, TABH + 116, "Enter: on", C_DIM, 1);
		return;
	}
	if (qr_size) {
		draw_qr(W, H - TABH);
		return;
	}
	ksprintf(buf, "%s: %s", svc_name[svc], state);
	text_at(24, TABH + 24, buf, C_TEXT, 1);
	if (!strcmp(state, "logged-out")) button(24, TABH + 60, 220, 48, B_QR, C_HI, "Link by QR code");
}

static void draw_list(void)
{
	bar(svc_name[svc], err[0] ? err : ended ? "the service ended" : state);
	int rh = 72, top = TABH + 4, rows = (H - top) / rh;
	if (!nchats) {
		text_center(W / 2, top + 60, linked() ? "No chats yet" : "", C_DIM, 1);
		return;
	}
	if (lsel >= nchats) lsel = nchats - 1;
	if (lsel < ltop) ltop = lsel;
	if (lsel >= ltop + rows) ltop = lsel - rows + 1;
	for (int k = ltop, y = top; k < nchats && y + rh <= H + 8; k++, y += rh) {
		struct chat *c = &chats[ord[k]];
		if (k == lsel) rounded(8, y + 2, W - 16, rh - 4, 12, C_TILE);
		kos_rect(16, y + rh - 1, W - 32, 1, 0x30343a);
		kos_button_hidden(8, y, W - 16, rh - 2, B_CHAT + k);
		char tt[24], nm[NAMELEN + 8], ln[200];
		time_text(c->time, tt, false);
		int ncols = (W - 64 - clen(tt) * 8) / 16;
		clip(nm, sizeof(nm), chat_name(c), ncols);
		text_at(24, y + 6, nm, C_TEXT, 2);
		text_at(W - 32 - clen(tt) * 8, y + 12, tt, c->unread ? C_HI : C_DIM, 1);
		/* the last message, one line */
		if (c->n) {
			struct msg *m = &c->m[c->n - 1];
			char lm[TEXTLEN + NAMELEN + 8];
			if (m->out) ksprintf(lm, "You: %s", m->text);
			else if (c->group && m->who[0]) ksprintf(lm, "%s: %s", m->who, m->text);
			else copy(lm, m->text, sizeof(lm));
			clip(ln, sizeof(ln), lm, (W - 120) / 8);
			text_at(24, y + 44, ln, c->unread ? C_TEXT : C_DIM, 1);
		}
		if (c->unread) {
			char u[8];
			ksprintf(u, "%d", c->unread);
			int bw = clen(u) * 8 + 18;
			rounded(W - 32 - bw, y + 38, bw, 26, 13, C_HI);
			text_center(W - 32 - bw / 2, y + 43, u, C_TEXT, 1);
		}
	}
}

/* a bubble's lines: up to cols characters each, broken at spaces */
static int wrap(const char *s, int cols, const char **starts, int *lens, int max)
{
	int n = 0;
	while (*s && n < max) {
		const char *ln = s, *brk = NULL;
		int c = 0;
		const char *p = s;
		while (*p && *p != '\n' && c < cols) {
			if (*p == ' ') brk = p;
			p++;
			while (*p && (*p & 0xc0) == 0x80) p++;
			c++;
		}
		if (*p && *p != '\n' && brk && brk > ln) p = brk;
		starts[n] = ln;
		lens[n] = p - ln;
		n++;
		s = p;
		if (*s == ' ' || *s == '\n') s++;
	}
	return n;
}

static void piece(char *out, const char *s, int len)
{
	int l = MIN(len, 199);
	memcpy(out, s, l);
	out[l] = 0;
}

static void draw_chat(void)
{
	struct chat *c = &chats[open_chat];
	/* the title row: back, the name, typing */
	kos_rect(0, 0, W, TABH, C_BAR);
	button(8, 4, 72, 48, B_BACK, C_TILE, "<");
	char nm[NAMELEN + 8];
	bool typing = c->typing_until && kos_uptime() < c->typing_until;
	clip(nm, sizeof(nm), chat_name(c), (W - 120) / 16);
	text_at(96, typing ? 4 : 12, nm, C_TEXT, 2);
	if (typing) text_at(96, 38, "typing...", C_HI, 0);
	else if (err[0]) text_at(W - 20 - MIN(clen(err), 40) * 8, 40, err, C_ERR, 0);
	/* the line to write in, Send */
	int ch = 60, cy = H - ch;
	kos_rect(0, cy, W, ch, C_BAR);
	rounded(8, cy + 8, W - 152, ch - 16, 10, 0x2a3a50);
	kos_button_hidden(8, cy + 8, W - 152, ch - 16, B_TEXT);
	{
		int cols = (W - 184) / 8, n = clen(text);
		const char *p = text;
		while (n > cols) {                       /* the end of what is typed */
			p++;
			while (*p && (*p & 0xc0) == 0x80) p++;
			n--;
		}
		char t[TEXTLEN + 2];
		ksprintf(t, "%s_", p);
		text_at(20, cy + ch / 2 - 8, text[0] ? t : "_", text[0] ? C_TEXT : C_DIM, 1);
	}
	button(W - 136, cy + 8, 128, ch - 16, B_SEND, text[0] ? C_HI : C_TILE, "Send");
	/* the bubbles, the newest at the bottom (scroll: that many newer ones hidden) */
	int top = TABH + 6, bottom = cy - 6;
	kos_button_hidden(0, top, W, (bottom - top) / 2, B_OLDER);
	kos_button_hidden(0, top + (bottom - top) / 2, W, (bottom - top) / 2, B_NEWER);
	int bw = W * 3 / 4, cols = (bw - 28) / 8;
	if (scroll > c->n - 1) scroll = c->n > 0 ? c->n - 1 : 0;
	if (!c->n) text_center(W / 2, top + 40, "No messages", C_DIM, 1);
	for (int i = c->n - 1 - scroll; i >= 0; i--) {
		struct msg *m = &c->m[i];
		const char *st[16];
		int ln[16];
		int nl = wrap(m->text[0] ? m->text : " ", cols, st, ln, 16);
		bool who = c->group && !m->out && m->who[0];
		char tt[48], tmp[200];
		time_text(m->time, tt, true);
		static const char *const how[] = { "", " sending...", " sent", " delivered", " read", " not sent" };
		if (m->out && m->status < 6) {
			unsigned l = strlen(tt);
			strcpy(tt + l, how[m->status]);
		}
		int widest = clen(tt);
		if (who && clen(m->who) > widest) widest = MIN(clen(m->who), cols);
		for (int k = 0; k < nl; k++) {
			piece(tmp, st[k], ln[k]);
			if (clen(tmp) > widest) widest = clen(tmp);
		}
		int w = widest * 8 + 28, h = nl * 18 + 34 + (who ? 18 : 0);
		bottom -= h + 8;
		if (bottom < top) break;
		int x = m->out ? W - w - 16 : 16;
		rounded(x, bottom, w, h, 12, m->out ? C_OUT : C_IN);
		int ty = bottom + 8;
		if (who) {
			clip(tmp, sizeof(tmp), m->who, cols);
			text_at(x + 14, ty, tmp, C_WHO, 1);
			ty += 18;
		}
		for (int k = 0; k < nl; k++) {
			piece(tmp, st[k], ln[k]);
			text_at(x + 14, ty + 18 * k, tmp, C_TEXT, 1);
		}
		text_at(x + 14, bottom + h - 22, tt, m->status == ST_FAILED ? 0xff9090 : 0xb8c4d0, 0);
	}
	if (scroll) text_center(W / 2, cy - 22, "(newer below: Down)", C_DIM, 0);
}

static void draw(void)
{
	kos_begin_draw();
	kos_window(0, 0, 800, 480, KOS_WIN_FIXED | C_BG, svc >= 0 ? svc_name[svc] : "Messenger");
	char info[1024];
	kos_thread_info(info, -1);
	W = *(u32 *)(info + 62) + 1;
	H = *(u32 *)(info + 66) + 1;
	kos_rect(0, 0, W, H, C_BG);
	if (svc < 0) draw_pick();
	else if (open_chat >= 0) draw_chat();
	else if (ended && !nchats) draw_link();
	else if (!strcmp(state, "logged-out") || qr_size || (svc == S_TG && ask_id >= 0)) draw_link();
	else draw_list();
	kos_end_draw();
}

/* --- input ------------------------------------------------------------------------ */

static void open_at(int k)
{
	if (k < 0 || k >= nchats) return;
	lsel = k;
	open_chat = ord[k];
	scroll = 0;
	text[0] = 0;
	err[0] = 0;
	mark_read(&chats[open_chat]);
}

/* back to the list, the chat left chosen there */
static void back(void)
{
	for (int k = 0; k < nchats; k++)
		if (ord[k] == open_chat) lsel = k;
	open_chat = -1;
	err[0] = 0;
}

static void leave(void)
{
	if (dirty) save();
	if (handle >= 0) kos_call(101, 5, handle, 0, 0, 0);
	kos_exit();
}

static void backspace(char *s)
{
	int n = strlen(s);
	while (n > 0 && (s[n - 1] & 0xc0) == 0x80) n--;
	if (n > 0) n--;
	s[n] = 0;
}

static void tg_key(int c)
{
	if (c == 13 && nfield) {
		if (ask_id >= 0) {
			char id[12];
			ksprintf(id, "%d", ask_id);
			call_begin("InputReply");
			arg_str(id);
			arg_str(field);
			call_end();
			ask_id = -1;
		} else {
			call_begin("Login");
			arg_str(field);
			call_end();
		}
		nfield = 0;
		field[0] = 0;
	} else if (c == 8 && nfield) {
		field[--nfield] = 0;
	} else if (c >= 32 && c < 127 && nfield + 1 < sizeof(field)) {
		field[nfield++] = (char)c;
		field[nfield] = 0;
	}
}

static void on_key(u32 k)
{
	if (k & 0xff) return;
	int c = (k >> 8) & 0xff;
	if (c == 17) leave();                        /* Ctrl+Q */
	if (svc < 0) {
		if (c >= '1' && c <= '3') start_service(c - '1');
		else if (c == 0xb2 && pick > 0) pick--;
		else if (c == 0xb1 && pick < 2) pick++;
		else if (c == 13) start_service(pick);
		return;
	}
	if (open_chat >= 0) {
		struct chat *ch = &chats[open_chat];
		if (c == 27) back();
		else if (c == 13) send_text();
		else if (c == 8) backspace(text);
		else if (c == 0xb2) scroll++;
		else if (c == 0xb1) { if (scroll) scroll--; }
		else if (c == 0xb8) scroll += 5;
		else if (c == 0xb7) scroll = scroll > 5 ? scroll - 5 : 0;
		else if (c >= 32 && c < 127) {
			int n = strlen(text);
			if (n + 2 < TEXTLEN) text[n] = (char)c, text[n + 1] = 0;
		}
		if (scroll > ch->n - 1) scroll = ch->n > 0 ? ch->n - 1 : 0;
		return;
	}
	if (svc == S_TG && (!strcmp(state, "logged-out") || ask_id >= 0)) {
		tg_key(c);
		return;
	}
	if (c == 0xb2 && lsel > 0) lsel--;
	else if (c == 0xb1 && lsel + 1 < nchats) lsel++;
	else if (c == 0xb8) lsel = lsel > 5 ? lsel - 5 : 0;
	else if (c == 0xb7) lsel = lsel + 5 < nchats ? lsel + 5 : nchats - 1;
	else if (c == 0xb4) lsel = 0;
	else if (c == 0xb5) lsel = nchats - 1;
	else if (c == 13) open_at(lsel);
	if (lsel < 0) lsel = 0;
}

static void on_button(u32 id)
{
	if (id == 1) leave();
	if (svc < 0) {
		if (id >= B_PICK && id < B_PICK + 3) start_service(id - B_PICK);
		return;
	}
	if (id == B_QR && handle >= 0) qr_id = call0("GetQR");
	else if (id == B_BACK) back();
	else if (id == B_SEND) send_text();
	else if (id == B_OLDER) scroll++;
	else if (id == B_NEWER && scroll) scroll--;
	else if (id >= B_CHAT && id < B_CHAT + MAXCHATS) open_at(id - B_CHAT);
}

int main(const char *params)
{
	json_init(&J, jn, sizeof(jn) / sizeof(jn[0]), jsb, sizeof(jsb));
	int s = S_NONE;
	if (params && prefix(params, "telegram")) {
		s = S_TG;
		if (params[8] == ' ' && params[9]) test_number = params + 9;
	} else if (params && !strcmp(params, "signal")) {
		s = S_SG;
	} else if (params && !strcmp(params, "whatsapp")) {
		s = S_WA;
	}
	kos_event_mask(7 | 1 << 7);                 /* redraw, key, button, network (function 101) */
	if (s >= 0) start_service(s);
	draw();
	u32 shown_typing = 0;
	for (;;) {
		int ev = kos_wait_event_timeout(50);
		bool redraw = ev == KOS_EV_REDRAW;
		if (ev == KOS_EV_KEY) on_key(kos_get_key()), redraw = true;
		else if (ev == KOS_EV_BUTTON) on_button(kos_get_button() >> 8), redraw = true;
		if (handle >= 0 && poll_backend()) redraw = true;
		/* "typing..." ends by itself */
		if (open_chat >= 0) {
			u32 t = chats[open_chat].typing_until && kos_uptime() < chats[open_chat].typing_until;
			if (t != shown_typing) shown_typing = t, redraw = true;
		}
		if (dirty && kos_uptime() - saved_at > 300) save();
		if (redraw) draw();
	}
}
