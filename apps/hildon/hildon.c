/* @hildon: a shell in the style of Maemo 5's Hildon desktop.
 *
 * It switches the window manager to Hildon's look (function 48.8 with
 * "hildon.skn": windows without frame, maximised below the panel) and
 * keeps a panel on top of all windows (function 18.25), BAR high:
 *
 *   - an application in front: the task switcher button on the left, the
 *     application's title, the status area (clock, battery) and the close
 *     button on the right;
 *   - the home view: the desktop with a clock, the launcher button;
 *   - the task switcher: the open applications as tiles with the last
 *     picture of each (function 36, taken when one leaves it), a tap
 *     brings one to the front, its cross closes it, a tap beside them
 *     goes home;
 *   - the launcher: the programs of /sys as a grid of icons.
 *
 * The home view, the task switcher and the launcher cover the screen
 * (the shell's window grows, function 67); with an application in front
 * the window is the panel only. Everything is a finger wide; the arrow
 * keys, Enter and Escape work in the grids. The status area opens
 * @hstatus. The icons are drawn here, simple shapes. */
#include "kos.h"

#define BAR 56
#define NAVW 112
#define CLOSEW 80
#define STATUSW 170

#define BTN_NAV 2
#define BTN_CLOSE 3
#define BTN_STATUS 4
#define BTN_BACKDROP 5
#define BTN_TILE 100          /* + the slot */
#define BTN_TILE_CLOSE 200    /* + the slot */
#define BTN_PROG 300          /* + the program's index */

/* Hildon's dark theme */
#define C_TEXT 0xffffff
#define C_DIM 0x9aa0a8
#define C_HI 0x2f7fcf         /* selection */
#define C_TILE 0x262a30

enum { HOME, APP, SWITCHER, LAUNCHER };
static int mode = HOME, shown_mode = -1;
static int sw, sh, self_slot, front;   /* front: the application in front (APP) */
static int cur_h;                      /* the window's height now */
static int sel;                        /* keyboard selection in the grids */
static char info[1024];

/* the open applications */
struct app { int slot; char name[12]; };
static struct app apps[32];
static int napps;

/* the programs (the launcher) */
static u8 folder[32 + 64 * 304];
static char progs[64][24];
static int nprogs;

/* a picture of each application, a quarter of the work area, taken when
 * it was last in front */
#define THUMBS 9
#define TW_MAX 270
#define TH_MAX 136
struct thumb { int slot, w, h; u8 px[TW_MAX * TH_MAX * 3]; };
static struct thumb thumbs[THUMBS];
static u8 row[1100 * 3];
static int tw, th;

/* a close asked of the application in front: forced after 2 s */
static int closing, closing_tick;

static u32 shown_time;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static int isqrt(int v)
{
	int r = 0;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

/* --- what runs ------------------------------------------------------------ */

static void scan(void)
{
	napps = 0;
	int max = kos_thread_info(info, -1);
	for (int s = 2; s <= max && napps < 32; s++) {
		kos_thread_info(info, s);
		if (*(u16 *)(info + 50) == 9) continue;      /* a free slot */
		if (s == self_slot || info[10] == '@') continue;
		if ((int)*(u32 *)(info + 42) <= 0) continue;  /* no window */
		apps[napps].slot = s;
		memcpy(apps[napps].name, info + 10, 11);
		apps[napps].name[11] = 0;
		napps++;
	}
}

static int app_index(int slot)
{
	for (int i = 0; i < napps; i++)
		if (apps[i].slot == slot) return i;
	return -1;
}

static void lower(char *s)
{
	for (; *s; s++)
		if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* the title of a program: its name, or a better one */
static const char *title_of(const char *name, char *buf)
{
	static const char *const known[][2] = {
		{ "clock", "Clock" }, { "example", "Example" }, { "board", "Debug board" },
		{ "cpu", "Processes" },
	};
	char n[24];
	strcpy(n, name);
	lower(n);
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (!strcmp(n, known[i][0])) return known[i][1];
	strcpy(buf, n);
	if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] -= 32;
	return buf;
}

static int is_program(const char *name)
{
	char path[64], head[8];
	if (name[0] == '@' || !strcmp(name, "menu")) return 0;
	ksprintf(path, "/sys/%s", name);
	struct kos_r2 r = kos_read_file(path, head, 8, 0);
	if (r.ebx != 8) return 0;
	const char *m = "MENUETA1";
	for (int i = 0; i < 8; i++)
		if (head[i] != m[i]) return 0;
	return 1;
}

/* --- drawing ---------------------------------------------------------------- */

static u32 mix(u32 a, u32 b, int f)    /* f/256 of b */
{
	int r = ((a >> 16 & 0xff) * (256 - f) + (b >> 16 & 0xff) * f) >> 8;
	int g = ((a >> 8 & 0xff) * (256 - f) + (b >> 8 & 0xff) * f) >> 8;
	int c = ((a & 0xff) * (256 - f) + (b & 0xff) * f) >> 8;
	return (u32)r << 16 | (u32)g << 8 | c;
}

static void vgradient(int x, int y, int w, int h, u32 top, u32 bottom)
{
	for (int i = 0; i < h; i++)
		kos_rect(x, y + i, w, 1, mix(top, bottom, i * 256 / (h > 1 ? h - 1 : 1)));
}

/* a square with rounded corners (radius r) */
static void rounded(int x, int y, int w, int h, int r, u32 c)
{
	for (int i = 0; i < h; i++) {
		int d = 0;
		if (i < r) d = r - isqrt(r * r - (r - i) * (r - i));
		else if (i >= h - r) d = r - isqrt(r * r - (i - (h - r) + 1) * (i - (h - r) + 1));
		kos_rect(x + d, y + i, w - 2 * d, 1, c);
	}
}

static void circle(int cx, int cy, int r, u32 c, int thick)
{
	for (int t = 0; t < thick; t++, r--) {
		int x = r, y = 0, e = 1 - r;
		while (x >= y) {
			kos_pixel(cx + x, cy + y, c); kos_pixel(cx - x, cy + y, c);
			kos_pixel(cx + x, cy - y, c); kos_pixel(cx - x, cy - y, c);
			kos_pixel(cx + y, cy + x, c); kos_pixel(cx - y, cy + x, c);
			kos_pixel(cx + y, cy - x, c); kos_pixel(cx - y, cy - x, c);
			y++;
			if (e < 0) e += 2 * y + 1;
			else { x--; e += 2 * (y - x) + 1; }
		}
	}
}

static void disc(int cx, int cy, int r, u32 c)
{
	for (int dy = -r; dy <= r; dy++) {
		int dx = isqrt(r * r - dy * dy);
		kos_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
	}
}

static void thick_line(int x0, int y0, int x1, int y1, u32 c, int t)
{
	for (int d = -t / 2; d <= t / 2; d++) {
		kos_line(x0 + d, y0, x1 + d, y1, c);
		kos_line(x0, y0 + d, x1, y1 + d, c);
	}
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

static void text_center(int cx, int y, const char *s, u32 c, int scale)
{
	if (scale > 1) kos_text_scaled(cx - text_w(s, scale) / 2, y, s, c, 1, scale);
	else kos_text(cx - text_w(s, 1) / 2, y, s, c, 1);
}

/* the program's icon, size n, at x, y */
static void icon(const char *name, int x, int y, int n)
{
	char low[24];
	strcpy(low, name);
	lower(low);
	int r = n / 6;
	if (!strcmp(low, "clock")) {
		rounded(x, y, n, n, r, 0x30343c);
		int cx = x + n / 2, cy = y + n / 2, R = n * 2 / 5;
		circle(cx, cy, R, 0xf0f0f0, 3);
		for (int i = 0; i < 12; i++) {
			static const short s12[12] = { 0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500 };
			int sx = s12[i], cyv = s12[(i + 3) % 12];
			kos_pixel(cx + sx * (R - 5) / 1000, cy - cyv * (R - 5) / 1000, 0xf0f0f0);
		}
		thick_line(cx, cy, cx, cy - R * 3 / 4, 0xffffff, 3);
		thick_line(cx, cy, cx + R / 2, cy + R / 4, 0xffffff, 3);
		thick_line(cx, cy, cx - R / 3, cy + R * 2 / 3, C_HI, 1);
	} else if (!strcmp(low, "board")) {
		rounded(x, y, n, n, r, 0x101214);
		for (int i = 0; i < 5; i++)
			kos_rect(x + n / 6, y + n / 5 + i * n / 8, (i % 2 ? n / 2 : n * 2 / 3), n / 20 + 1, 0x58d068);
	} else if (!strcmp(low, "cpu")) {
		rounded(x, y, n, n, r, 0x2c3440);
		static const int bars[5] = { 40, 70, 55, 85, 30 };
		int bw = n / 8;
		for (int i = 0; i < 5; i++) {
			int bh = bars[i] * (n * 3 / 5) / 100;
			kos_rect(x + n / 8 + i * (bw + bw / 3), y + n * 4 / 5 - bh, bw, bh, i == 3 ? 0xe8a040 : C_HI);
		}
	} else {
		/* the first letter on a colour of the name */
		u32 h = 0;
		for (const char *p = low; *p; p++) h = h * 31 + (u8)*p;
		static const u32 pal[6] = { 0x3a78c8, 0x40a060, 0xc86a3a, 0x8a52c0, 0x2f9aa0, 0xb84a6a };
		rounded(x, y, n, n, r, pal[h % 6]);
		char l[2] = { low[0] >= 'a' && low[0] <= 'z' ? low[0] - 32 : low[0], 0 };
		int sc = n / 24;
		if (sc < 1) sc = 1;
		if (sc > 8) sc = 8;
		kos_text_scaled(x + (n - 8 * sc) / 2, y + (n - 16 * sc) / 2, l, 0xffffff, 1, sc);
	}
}

/* the panel's left button: the launcher's grid of squares or the task
 * switcher's two windows */
static void nav_icon(int launcher)
{
	int cx = NAVW / 2, cy = BAR / 2;
	if (launcher) {
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				kos_rect(cx - 15 + i * 11, cy - 15 + j * 11, 8, 8, C_TEXT);
	} else {
		kos_rect(cx - 16, cy - 14, 24, 18, C_DIM);
		kos_rect(cx - 8, cy - 6, 24, 18, C_TEXT);
		kos_rect(cx - 6, cy - 4, 20, 14, 0x303438);
	}
}

static void battery(int x, int y)
{
	/* not driven yet: an outline, three quarters full */
	kos_rect(x, y, 30, 2, C_TEXT); kos_rect(x, y + 14, 30, 2, C_TEXT);
	kos_rect(x, y, 2, 16, C_TEXT); kos_rect(x + 28, y, 2, 16, C_TEXT);
	kos_rect(x + 30, y + 5, 3, 6, C_TEXT);
	kos_rect(x + 4, y + 4, 16, 8, C_TEXT);
}

static void clock_text(char *s)
{
	u32 t = kos_time();
	s[0] = '0' + ((t >> 4) & 0xf); s[1] = '0' + (t & 0xf); s[2] = ':';
	s[3] = '0' + ((t >> 12) & 0xf); s[4] = '0' + ((t >> 8) & 0xf); s[5] = 0;
	shown_time = t & 0xffff;
}

static void panel(const char *title, int launcher_icon, int with_close)
{
	vgradient(0, 0, sw, BAR, 0x3a3d42, 0x18191b);
	kos_rect(0, BAR - 1, sw, 1, 0x000000);
	kos_button_hidden(0, 0, NAVW, BAR, BTN_NAV);
	kos_rect(NAVW, 8, 1, BAR - 16, 0x505458);
	nav_icon(launcher_icon);
	if (title) kos_text_scaled(NAVW + 20, BAR / 2 - 16, title, C_TEXT, 1, 2);
	int right = sw - (with_close ? CLOSEW : 0);
	int sx = right - STATUSW;
	kos_button_hidden(sx, 0, STATUSW, BAR, BTN_STATUS);
	char clk[6];
	clock_text(clk);
	kos_text_scaled(sx + 10, BAR / 2 - 16, clk, C_TEXT, 1, 2);
	battery(sx + STATUSW - 46, BAR / 2 - 8);
	if (with_close) {
		kos_rect(right, 8, 1, BAR - 16, 0x505458);
		kos_button_hidden(right, 0, CLOSEW, BAR, BTN_CLOSE);
		int cx = right + CLOSEW / 2, cy = BAR / 2;
		thick_line(cx - 12, cy - 12, cx + 12, cy + 12, C_TEXT, 3);
		thick_line(cx - 12, cy + 12, cx + 12, cy - 12, C_TEXT, 3);
	}
}

static void backdrop(void)
{
	/* the kernel's desktop in Hildon's look */
	for (int y = BAR; y < sh; y += 4) {
		int f = 255 - y * 170 / sh;
		u32 c = ((0x1c * f >> 8) << 16) | ((0x2a * f >> 8) << 8) | (0x3c * f >> 8);
		kos_rect(0, y, sw, MIN(4, sh - y), c);
	}
}

static void draw_home(void)
{
	backdrop();
	kos_button_hidden(0, BAR, sw, sh - BAR, BTN_BACKDROP);
	/* a clock and the date, as Hildon's clock widget */
	int w = 360, h = 170, x = sw - w - 40, y = BAR + 40;
	rounded(x, y, w, h, 18, 0x10141a);
	char clk[6];
	clock_text(clk);
	text_center(x + w / 2, y + 22, clk, C_TEXT, 5);
	u32 d = kos_date();
	static const char *const mon[12] = { "January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December" };
	/* function 29: 0x00DDMMYY in BCD */
	int yr = ((d >> 4) & 0xf) * 10 + (d & 0xf), m = ((d >> 12) & 0xf) * 10 + ((d >> 8) & 0xf);
	int day = ((d >> 20) & 0xf) * 10 + ((d >> 16) & 0xf);
	char ds[32];
	ksprintf(ds, "%d %s %d", day, m >= 1 && m <= 12 ? mon[m - 1] : "", yr < 70 ? 2000 + yr : 1900 + yr);
	text_center(x + w / 2, y + 120, ds, C_DIM, 1);
	if (!napps)
		text_center(sw / 2, sh - 40, "The applications: the button at the top left", C_DIM, 1);
}

static struct thumb *thumb_of(int slot)
{
	for (int i = 0; i < THUMBS; i++)
		if (thumbs[i].slot == slot) return &thumbs[i];
	return NULL;
}

/* the picture of the application in front, before the shell covers it */
static void take_thumb(int slot)
{
	struct thumb *t = thumb_of(slot);
	if (!t) {
		for (int i = 0; i < THUMBS && !t; i++)
			if (!thumbs[i].slot || app_index(thumbs[i].slot) < 0) t = &thumbs[i];
		if (!t) t = &thumbs[0];
	}
	t->slot = slot;
	t->w = tw;
	t->h = th;
	int ww = MIN(sw, 1100);
	for (int j = 0; j < th; j++) {
		kos_read_screen(row, 0, BAR + j * 4, ww, 1);
		for (int i = 0; i < tw; i++)
			memcpy(&t->px[(j * tw + i) * 3], &row[i * 4 * 3], 3);
	}
}

static void forget_thumb(int slot)
{
	struct thumb *t = thumb_of(slot);
	if (t) t->slot = 0;
}

/* the grids: cells of the screen below the panel */
static int cols, cell_w, cell_h;

static void grid_cell(int i, int *x, int *y)
{
	*x = (i % cols) * cell_w + (sw - cols * cell_w) / 2;
	*y = BAR + 16 + (i / cols) * cell_h;
}

static void draw_switcher(void)
{
	backdrop();
	kos_button_hidden(0, BAR, sw, sh - BAR, BTN_BACKDROP);
	cols = 3;
	cell_w = tw + 40;
	cell_h = th + 52;
	if (!napps) {
		text_center(sw / 2, sh / 2, "No applications open", C_DIM, 2);
		return;
	}
	for (int i = 0; i < napps && i < 9; i++) {
		int x, y;
		grid_cell(i, &x, &y);
		x += 20;
		struct thumb *t = thumb_of(apps[i].slot);
		if (i == sel) rounded(x - 6, y - 6, tw + 12, th + 12, 8, C_HI);
		if (t && t->w == tw)
			kos_put_image(t->px, t->w, t->h, x, y);
		else {
			kos_rect(x, y, tw, th, C_TILE);
			icon(apps[i].name, x + tw / 2 - 32, y + th / 2 - 32, 64);
		}
		kos_button_hidden(x, y, tw, th, BTN_TILE + apps[i].slot);
		char tb[24];
		text_center(x + tw / 2, y + th + 10, title_of(apps[i].name, tb), C_TEXT, 1);
		/* the cross, in a circle at the top right */
		int cx = x + tw - 4, cy = y + 4;
		disc(cx, cy, 20, 0x202226);
		circle(cx, cy, 20, C_TEXT, 2);
		thick_line(cx - 7, cy - 7, cx + 7, cy + 7, C_TEXT, 3);
		thick_line(cx - 7, cy + 7, cx + 7, cy - 7, C_TEXT, 3);
		kos_button_hidden(cx - 24, cy - 24, 48, 48, BTN_TILE_CLOSE + apps[i].slot);
	}
}

static void draw_launcher(void)
{
	vgradient(0, BAR, sw, sh - BAR, 0x15171a, 0x050506);
	cols = sw / 200;
	if (cols < 3) cols = 3;
	cell_w = sw / cols;
	cell_h = 150;
	for (int i = 0; i < nprogs; i++) {
		int x, y;
		grid_cell(i, &x, &y);
		int n = 84, ix = x + (cell_w - n) / 2;
		if (i == sel) rounded(x + 10, y - 6, cell_w - 20, cell_h - 12, 12, 0x24384e);
		icon(progs[i], ix, y + 4, n);
		char tb[24];
		text_center(x + cell_w / 2, y + n + 16, title_of(progs[i], tb), C_TEXT, 1);
		kos_button_hidden(x + 4, y - 4, cell_w - 8, cell_h - 8, BTN_PROG + i);
	}
}

static void draw(void)
{
	char tb[24];
	kos_begin_draw();
	kos_window_plain(0, 0, sw, cur_h, KOS_WIN_PLAIN, 0x01000000);
	int fi = app_index(front);
	switch (mode) {
	case APP:
		panel(fi >= 0 ? title_of(apps[fi].name, tb) : "", 0, 1);
		break;
	case HOME:
		panel(NULL, 1, 0);
		draw_home();
		break;
	case SWITCHER:
		panel(NULL, 1, 0);
		draw_switcher();
		break;
	case LAUNCHER:
		panel("Applications", 0, 0);
		draw_launcher();
		break;
	}
	kos_end_draw();
	shown_mode = mode;
}

/* --- switching ------------------------------------------------------------ */

static void set_height(int h)
{
	if (h == cur_h) return;
	cur_h = h;
	kos_move_window(0, 0, sw - 1, h - 1);
}

static void go(int m)
{
	if (m == mode && m != APP) return;
	if (mode == APP && m != APP && front) take_thumb(front);
	mode = m;
	sel = 0;
	if (m == APP) {
		set_height(BAR);
		kos_activate_slot(front);
	} else {
		set_height(sh);
		kos_activate_slot(self_slot);
	}
	draw();
}

static void launch(int i)
{
	char path[64];
	ksprintf(path, "/sys/%s", progs[i]);
	kos_run(path, 0);
	/* its window becomes the active one: the next scan shows it */
}

static void close_app(int slot)
{
	if (slot == self_slot || !slot) return;
	kos_activate_slot(slot);
	kos_send_button(1);
	closing = slot;
	closing_tick = kos_uptime();
	forget_thumb(slot);
	if (mode != APP) kos_activate_slot(self_slot);
}

/* what the system did meanwhile: an application came, one ended */
static u8 seen[64];

static void follow(void)
{
	scan();
	int a = kos_active_slot();
	if (closing && app_index(closing) < 0) closing = 0;
	if (closing && kos_uptime() - closing_tick > 200) {
		kos_kill_slot(closing);
		closing = 0;
	}
	/* a new window: a program just started, to the front (whether it
	 * or the shell defined its window first) */
	int fresh = 0;
	for (int i = 0; i < napps; i++)
		if (apps[i].slot < 64 && !seen[apps[i].slot]) fresh = apps[i].slot;
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < napps; i++)
		if (apps[i].slot < 64) seen[apps[i].slot] = 1;
	if (fresh || (mode == APP && a != front && a != self_slot && app_index(a) >= 0)) {
		front = fresh ? fresh : a;
		mode = -1;
		go(APP);
		return;
	}
	if (mode == APP && app_index(front) < 0) {
		/* the one in front ended: the next one, else home */
		front = napps ? apps[napps - 1].slot : 0;
		if (front) go(APP); else go(HOME);
		return;
	}
	/* covered applications do not get the keys (the status menu ended) */
	if (mode != APP && a != self_slot && app_index(a) >= 0)
		kos_activate_slot(self_slot);
	if (mode == SWITCHER && shown_mode == SWITCHER) {
		static int shown_n = -1;
		if (shown_n != napps) { shown_n = napps; draw(); }
	}
}

static void key(u32 k)
{
	int c = (k >> 8) & 0xff;
	int n = mode == LAUNCHER ? nprogs : mode == SWITCHER ? MIN(napps, 9) : 0;
	switch (c) {
	case 27:   /* Escape: back */
		if (mode == LAUNCHER || mode == SWITCHER) {
			if (front && app_index(front) >= 0) go(APP); else go(HOME);
		}
		return;
	case 0xb0: if (sel > 0) sel--; break;                  /* left */
	case 0xb3: if (sel + 1 < n) sel++; break;              /* right */
	case 0xb2: if (sel - cols >= 0) sel -= cols; break;    /* up */
	case 0xb1: if (sel + cols < n) sel += cols; break;     /* down */
	case 13:
		if (mode == LAUNCHER && sel < nprogs) launch(sel);
		else if (mode == SWITCHER && sel < napps) { front = apps[sel].slot; go(APP); }
		return;
	default:
		return;
	}
	if (mode == LAUNCHER || mode == SWITCHER) draw();
}

static void button(u32 id)
{
	if (id == BTN_NAV) {
		if (mode == APP) go(SWITCHER);
		else if (mode == SWITCHER || mode == HOME) go(LAUNCHER);
		else if (mode == LAUNCHER) go(napps ? SWITCHER : HOME);
	} else if (id == BTN_CLOSE && mode == APP) {
		close_app(front);
	} else if (id == BTN_STATUS) {
		/* the status menu, or away with it */
		int max = kos_thread_info(info, -1);
		for (int s = 2; s <= max; s++) {
			kos_thread_info(info, s);
			if (*(u16 *)(info + 50) != 9 && !strcmp(info + 10, "@hstatus")) {
				kos_kill_slot(s);
				return;
			}
		}
		kos_run("/sys/@hstatus", 0);
	} else if (id == BTN_BACKDROP) {
		if (mode == SWITCHER || mode == LAUNCHER) go(HOME);
	} else if (id >= BTN_PROG && id < BTN_PROG + (u32)nprogs) {
		launch(id - BTN_PROG);
	} else if (id >= BTN_TILE_CLOSE && id < BTN_TILE_CLOSE + 100) {
		close_app(id - BTN_TILE_CLOSE);
		scan();
		draw();
	} else if (id >= BTN_TILE && id < BTN_TILE + 100) {
		int s = id - BTN_TILE;
		if (app_index(s) >= 0) { front = s; go(APP); }
	}
}

/* params: "launcher" or "switcher" opens that view at the start (tests
 * without input) */
int main(const char *params)
{
	kos_screen_size(&sw, &sh);
	kos_thread_info(info, -1);
	self_slot = kos_call(18, 21, *(u32 *)(info + 30), 0, 0, 0);
	tw = MIN(sw / 4, TW_MAX);
	th = MIN((sh - BAR) / 4, TH_MAX);
	struct kos_r2 r = kos_read_folder("/sys", folder, 0, 64);
	for (u32 i = 0; i < r.ebx && nprogs < 64; i++) {
		const char *name = (const char *)folder + 32 + i * 304 + 40;
		if (is_program(name)) strcpy(progs[nprogs++], name);
	}
	kos_set_skin("/sys/hildon.skn");
	kos_set_work_area(0, sw - 1, BAR, sh - 1);
	cur_h = sh;
	scan();
	for (int i = 0; i < napps; i++)
		if (apps[i].slot < 64) seen[apps[i].slot] = 1;
	draw();
	kos_set_zpos(KOS_ZPOS_ALWAYS_TOP);
	kos_activate_slot(self_slot);
	if (params && !strcmp(params, "launcher")) go(LAUNCHER);
	else if (params && !strcmp(params, "switcher")) go(SWITCHER);
	else if (napps) {
		/* started with programs (autorun): the last one in front */
		front = apps[napps - 1].slot;
		go(APP);
	}
	for (;;) {
		int ev = kos_wait_event_timeout(25);
		switch (ev) {
		case KOS_EV_REDRAW:
			draw();
			break;
		case KOS_EV_KEY:
			key(kos_get_key());
			break;
		case KOS_EV_BUTTON:
			button(kos_get_button() >> 8);
			break;
		case 0:
			follow();
			if ((kos_time() & 0xffff) != shown_time && mode != SWITCHER && mode != LAUNCHER)
				draw();
			break;
		}
	}
}
