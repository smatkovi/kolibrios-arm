/* messenger: WhatsApp and Telegram for KolibriOS, the first part. Each
 * through its backend, a Linux program started through function 101
 * (docs/messengers.md), in JSON lines:
 *
 *  - WhatsApp: Yappari's daemon (/bin/yappari-wa -stdio): its state, and
 *    linking by QR code (WhatsApp on the main phone: Linked devices, Link
 *    a device);
 *  - Telegram (parameter "telegram"): libpurple with tdlib-purple
 *    (/bin/purple-host -stdio): the phone number, then what Telegram asks
 *    (the login code, the password). "telegram NUMBER" logs in with that
 *    number at once (tests, QEMU has no keyboard).
 *
 * The chats come next. The backends' lines go to the debug board too. */
#include "kos.h"
#include "qr.h"

typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)

#define BG 0x000000
#define FG 0xffffff
#define DIM 0x9a9a9a
#define ACCENT 0x2a8ad4

static int handle = -1;
static char state[48] = "starting";
static char qr_text[512];
static int qr_size;
static unsigned char modules[QR_MAX_SIZE * QR_MAX_SIZE];
static char line[8192];
static unsigned nline;
static int next_id = 1, status_id, qr_id;
static bool ended;

static int lx_start(const char *path, const char *args) { return kos_call(101, 1, (u32)path, (u32)args, 0, 0); }
static int lx_write(const char *s, unsigned n) { return kos_call(101, 2, handle, (u32)s, n, 0); }
static int lx_read(char *b, unsigned n) { return kos_call(101, 3, handle, (u32)b, n, 0); }

static bool tg;                        /* Telegram, not WhatsApp */
static char field[64];                 /* what is typed */
static unsigned nfield;
static int ask_id = -1;                /* Telegram's question (an Input signal) */
static char ask_text[96], err[128];
static const char *test_number;

/* a call with up to two string arguments (digits and plain text) */
static int call_args(const char *method, const char *a0, const char *a1)
{
	char buf[256];
	int id = next_id++;
	int n = ksprintf(buf, "{\"id\":%d,\"call\":\"%s\",\"args\":[", id, method);
	const char *a[2] = { a0, a1 };
	for (int i = 0; i < 2 && a[i]; i++) {
		if (i) buf[n++] = ',';
		buf[n++] = '"';
		for (const char *c = a[i]; *c && n < (int)sizeof(buf) - 8; c++) {
			if (*c == '"' || *c == '\\') buf[n++] = '\\';
			buf[n++] = *c;
		}
		buf[n++] = '"';
	}
	n += ksprintf(buf + n, "]}\n");
	lx_write(buf, n);
	return id;
}

static int call(const char *method)
{
	char buf[96];
	int id = next_id++;
	int n = ksprintf(buf, "{\"id\":%d,\"call\":\"%s\",\"args\":[]}\n", id, method);
	lx_write(buf, n);
	return id;
}

static void debug_line(const char *s)
{
	kos_debug_char('[');
	for (const char *p = "wa] "; *p; p++) kos_debug_char(*p);
	for (unsigned i = 0; s[i] && i < 160; i++) kos_debug_char(s[i]);
	kos_debug_char('\n');
}

/* the JSON the daemon writes, as far as needed here: a key's place, the
 * first string of an array after a key */
static const char *find(const char *s, const char *key)
{
	unsigned n = strlen(key);
	for (; *s; s++) {
		unsigned i = 0;
		while (i < n && s[i] == key[i]) i++;
		if (i == n) return s + n;
	}
	return NULL;
}

static bool first_string(const char *s, const char *key, char *out, unsigned size)
{
	const char *p = find(s, key);
	if (!p) return false;
	while (*p && *p != '"' && *p != ']') p++;
	if (*p != '"') return false;
	unsigned o = 0;
	for (p++; *p && *p != '"' && o + 1 < size; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
				unsigned v = 0;
				for (int i = 1; i <= 4; i++) {
					char c = p[i];
					v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
				}
				out[o++] = v < 128 ? (char)v : '?';
				p += 4;
				continue;
			}
			out[o++] = *p == 'n' ? '\n' : *p;
			continue;
		}
		out[o++] = *p;
	}
	out[o] = 0;
	return true;
}

static int reply_id(const char *s)
{
	const char *p = find(s, "\"id\":");
	int v = 0;
	while (p && *p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
	return p ? v : -1;
}

static void set_qr(const char *t)
{
	strcpy(qr_text, t);
	qr_size = *t ? qr_encode(qr_text, strlen(qr_text), modules) : 0;
}

static bool tg_line(const char *s)
{
	char v[160];
	if (find(s, "\"signal\":\"StateChanged\"")) {
		if (first_string(s, "\"args\":", v, sizeof(state))) strcpy(state, v);
		if (!strcmp(state, "connected")) err[0] = 0;
		return true;
	}
	if (find(s, "\"signal\":\"Input\"")) {
		const char *p = find(s, "\"args\":[");
		ask_id = 0;
		while (p && *p >= '0' && *p <= '9') ask_id = ask_id * 10 + *p++ - '0';
		if (!first_string(s, "\"args\":", ask_text, sizeof(ask_text))) strcpy(ask_text, "Telegram asks");
		nfield = 0;
		field[0] = 0;
		return true;
	}
	if (find(s, "\"signal\":\"Error\"") || find(s, "\"signal\":\"Notice\"")) {
		if (first_string(s, "\"args\":", v, sizeof(err))) strcpy(err, v);
		return true;
	}
	if (reply_id(s) == status_id && first_string(s, "\"result\":", v, sizeof(state))) {
		strcpy(state, v);
		if (test_number && !strcmp(state, "logged-out")) call_args("Login", test_number, 0);
		return true;
	}
	return false;
}

static bool handle_line(const char *s)
{
	char v[512];
	debug_line(s);
	if (tg) return tg_line(s);
	if (find(s, "\"signal\":\"QRCode\"")) {
		if (first_string(s, "\"args\":", v, sizeof(v))) set_qr(v);
		return true;
	}
	if (find(s, "\"signal\":\"StateChanged\"")) {
		if (first_string(s, "\"args\":", v, sizeof(state))) strcpy(state, v);
		if (!strcmp(state, "connected") || !strcmp(state, "connecting")) set_qr("");
		return true;
	}
	int id = reply_id(s);
	if (id == status_id && first_string(s, "\"result\":", v, sizeof(state))) {
		strcpy(state, v);
		if (!strcmp(state, "logged-out")) qr_id = call("GetQR");
		return true;
	}
	if (id == qr_id) {
		if (first_string(s, "\"result\":", v, sizeof(v))) set_qr(v);
		else if (first_string(s, "\"error\":", v, sizeof(v))) strcpy(state, "error");
		return true;
	}
	return false;
}

static void tg_draw(int sw, int sh)
{
	char buf[160];
	kos_begin_draw();
	kos_window(0, 0, sw, sh, KOS_WIN_SKINNED | BG, "Telegram");
	ksprintf(buf, "Telegram: %s", ended ? "the service ended" : state);
	kos_text(24, 24, buf, FG, 1);
	if (err[0]) kos_text(24, 48, err, 0xff6060, 1);
	const char *label = ask_id >= 0 ? ask_text :
			    !strcmp(state, "connected") ? NULL : "Your phone number (+43...):";
	if (label && !ended) {
		kos_text(24, 84, label, DIM, 1);
		kos_rect(24, 108, 360, 32, 0x202020);
		ksprintf(buf, "%s_", field);
		kos_text(32, 116, buf, FG, 1);
		kos_text(24, 152, "Enter: on", DIM, 1);
	}
	kos_end_draw();
}

static void tg_key(u32 k)
{
	u32 c = (k >> 8) & 0xff;
	if (c == 13 && nfield) {
		if (ask_id >= 0) {
			char id[12];
			ksprintf(id, "%d", ask_id);
			call_args("InputReply", id, field);
			ask_id = -1;
		} else {
			call_args("Login", field, 0);
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

static void draw(void)
{
	if (tg) {
		int sw, sh;
		kos_screen_size(&sw, &sh);
		tg_draw(sw, sh);
		return;
	}
	int sw, sh;
	kos_screen_size(&sw, &sh);
	kos_begin_draw();
	kos_window(0, 0, sw, sh, KOS_WIN_SKINNED | BG, "WhatsApp");
	/* the window's inside: the work area (the Hildon shell's panel above it) */
	struct kos_r2 wa = kos_call2(48, 5, 0, 0, 0, 0);
	int w = (int)(wa.eax & 0xffff) - (int)(wa.eax >> 16) - 10;
	int h = (int)(wa.ebx & 0xffff) - (int)(wa.ebx >> 16) - kos_skin_height() - 10;
	char buf[96];
	if (ended) {
		kos_text(24, 24, "The WhatsApp service ended.", FG, 1);
	} else if (!strcmp(state, "connected")) {
		kos_text(24, 24, "Linked to WhatsApp.", FG, 1);
		kos_text(24, 48, "The chats come in a later version.", DIM, 1);
	} else if (qr_size) {
		/* the QR code with its quiet zone, as large as fits */
		int side = (w < h ? w : h) - 24;
		int s = side / (qr_size + 8);
		if (s < 1) s = 1;
		int x0 = 24, y0 = 24, q = 4 * s, full = (qr_size + 8) * s;
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
		kos_text(tx, ty, "Link this device:", FG, 1);
		kos_text(tx, ty + 24, "WhatsApp on your phone,", DIM, 1);
		kos_text(tx, ty + 44, "Linked devices,", DIM, 1);
		kos_text(tx, ty + 64, "Link a device, scan the code.", DIM, 1);
	} else {
		ksprintf(buf, "WhatsApp: %s", state);
		kos_text(24, 24, buf, FG, 1);
		if (!strcmp(state, "logged-out")) {
			kos_button(24, 60, 200, 36, 10, ACCENT);
			kos_text(44, 70, "Link by QR code", FG, 1);
		}
	}
	kos_end_draw();
}

static bool poll_daemon(void)
{
	bool changed = false;
	for (;;) {
		if (nline + 1 >= sizeof(line)) nline = 0;              /* a line too long: dropped */
		int n = lx_read(line + nline, sizeof(line) - 1 - nline);
		if (n < 0) {
			if (!ended) changed = true;
			ended = true;
			return changed;
		}
		if (!n) return changed;
		nline += n;
		line[nline] = 0;
		char *start = line, *nl;
		while ((nl = (char *)find(start, "\n"))) {
			nl[-1] = 0;
			if (handle_line(start)) changed = true;
			start = nl;
		}
		nline = strlen(start);
		memcpy(line, start, nline + 1);
	}
}

int main(const char *params)
{
	const char *t = "telegram";
	unsigned i = 0;
	while (params && t[i] && params[i] == t[i]) i++;
	if (params && !t[i]) {
		tg = true;
		if (params[8] == ' ' && params[9]) test_number = params + 9;
	}
	kos_event_mask(7 | 1 << 7);                 /* redraw, key, button, network (function 101) */
	const char *backend = tg ? "/bin/purple-host" : "/bin/yappari-wa";
	handle = lx_start(backend, "-stdio");
	if (handle < 0) {
		ksprintf(state, "not there (%s)", backend);
		ended = true;
	} else {
		status_id = call("Status");
	}
	draw();
	for (;;) {
		int ev = kos_wait_event_timeout(50);
		if (ev == KOS_EV_REDRAW) draw();
		else if (ev == KOS_EV_KEY) {
			u32 k = kos_get_key();
			if (tg) {
				tg_key(k);
				draw();
			}
		}
		else if (ev == KOS_EV_BUTTON) {
			u32 id = kos_get_button() >> 8;
			if (id == 1) {
				if (handle >= 0) kos_call(101, 5, handle, 0, 0, 0);
				kos_exit();
			}
			if (id == 10 && handle >= 0) qr_id = call("GetQR");
		}
		if (handle >= 0 && poll_daemon()) draw();
	}
}
