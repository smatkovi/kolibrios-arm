/* KolibriOS for ARM: the system functions (docs/sysfuncs.txt of
 * KolibriOS). A program calls them with "svc #0x40"; r0-r6 stand for
 * eax, ebx, ecx, edx, esi, edi, ebp and return what they return. */
#include "gui.h"

#define EAX r[0]
#define EBX r[1]
#define ECX r[2]
#define EDX r[3]
#define ESI r[4]
#define EDI r[5]

static u8 bcd(int v) { return ((v / 10) << 4) | (v % 10); }

/* the civil date of a Unix time */
static void civil(u32 t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	*s = t % 60; t /= 60;
	*mi = t % 60; t /= 60;
	*h = t % 24; t /= 24;
	int z = t + 719468, era = z / 146097, doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*mo = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*mo <= 2);
}

static u32 boot_rtc, boot_ticks;

void rtc_set(u32 t)
{
	boot_rtc = t;
	boot_ticks = ticks;
}

u32 rtc_seconds(void)
{
	if (!boot_rtc && plat->rtc_time) {
		boot_rtc = plat->rtc_time();
		boot_ticks = ticks;
	}
	return boot_rtc + (ticks - boot_ticks) / 100;
}

static void draw_text(struct task *t, u32 *r)
{
	int ox, oy;
	wm_origin(t, &ox, &oy);
	u32 ecx = ECX;
	bool asciiz = ecx & 0x80000000u, fill = ecx & 0x40000000u;
	int font = (ecx >> 28) & 3;
	bool buffer = ecx & 0x08000000u;
	int scale = ((ecx >> 24) & 7) + 1;
	if (buffer) return;                    /* to a user buffer: not yet */
	const char *s;
	int len;
	if (asciiz) {
		s = ustr(t, EDX);
		len = -1;
	} else {
		len = ESI & 0xffff;
		s = uptr(t, EDX, len);
	}
	if (!s) return;
	gfx_text(ox + (s16)(EBX >> 16), oy + (s16)(EBX & 0xffff), s, len, ecx & 0xffffff,
		 font == 0 ? 0 : 1, scale, fill, EDI, t->slot, font == 3);
}

static void draw_number(struct task *t, u32 *r)
{
	u32 fmt = EBX, v = ECX;
	if (fmt & 1) {
		u32 *p = uptr(t, ECX, 4);
		if (!p) return;
		v = *p;
	}
	int bsel = (fmt >> 8) & 0xff;
	int base = bsel == 1 ? 16 : bsel == 2 ? 2 : 10;
	int digits = MIN((int)((fmt >> 16) & 0x3f), 32);
	char buf[33];
	buf[32] = 0;
	for (int i = 0; i < digits; i++) {
		int d = v % base;
		buf[31 - i] = d < 10 ? '0' + d : 'A' + d - 10;
		v /= base;
	}
	char *s = &buf[32 - digits];
	if (fmt & 0x80000000u)                 /* no leading zeros */
		while (*s == '0' && s[1]) s++;
	int ox, oy;
	wm_origin(t, &ox, &oy);
	u32 c = ESI;
	int font = (c >> 28) & 3;
	gfx_text(ox + (s16)(EDX >> 16), oy + (s16)(EDX & 0xffff), s, -1, c & 0xffffff,
		 font == 0 ? 0 : 1, ((c >> 24) & 7) + 1, c & 0x40000000u, EDI, t->slot, false);
}

/* function 36: a part of the screen into a buffer, 3 bytes (B, G, R) a
 * pixel; screen coordinates */
static void read_screen(struct task *t, u32 *r)
{
	int w = ECX >> 16, h = ECX & 0xffff, x = EDX >> 16, y = EDX & 0xffff;
	u8 *buf = uptr(t, EBX, (u32)w * h * 3);
	if (!buf) return;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			u8 *p = buf + (j * w + i) * 3;
			int xx = x + i, yy = y + j;
			u32 c = xx < scr.w && yy < scr.h ? scr.fb[yy * scr.stride + xx] : 0;
			p[0] = c; p[1] = c >> 8; p[2] = c >> 16;
		}
}

static void put_image(struct task *t, u32 *r)
{
	int w = ECX >> 16, h = ECX & 0xffff;
	const u8 *img = uptr(t, EBX, (u32)w * h * 3);
	if (!img) return;
	int ox, oy;
	wm_origin(t, &ox, &oy);
	int x = ox + (s16)(EDX >> 16), y = oy + (s16)(EDX & 0xffff);
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			const u8 *p = img + (j * w + i) * 3;
			gfx_pixel(x + i, y + j, p[0] | p[1] << 8 | p[2] << 16, t->slot);
		}
}

static void define_button(struct task *t, u32 *r)
{
	struct window *w = &t->win;
	u32 id = EDX & 0xffffff;
	if (EDX & 0x80000000u) {             /* delete */
		int j = 0;
		for (int i = 0; i < w->nbtn; i++)
			if (w->btn[i].id != id) w->btn[j++] = w->btn[i];
		w->nbtn = j;
		return;
	}
	if (w->nbtn >= MAX_BUTTONS || !w->defined) return;
	int ox, oy;
	wm_origin(t, &ox, &oy);
	struct button *b = &w->btn[w->nbtn++];
	b->x = ox + (s16)(EBX >> 16);
	b->y = oy + (s16)(ECX >> 16);
	b->w = (EBX & 0xffff) + 1;
	b->h = (ECX & 0xffff) + 1;
	b->id = id;
	b->color = ESI & 0xffffff;
	b->hidden = EDX & 0x40000000u;
	b->noframe = EDX & 0x20000000u;
	wm_button_draw(t, b, false);
}

static void thread_info(struct task *t, u32 *r)
{
	u8 *b = uptr(t, EBX, 1024);
	if (!b) { EAX = -1; return; }
	int slot = (s32)ECX == -1 ? t->slot : (int)ECX;
	int max = 0;
	for (int i = 1; i < MAX_TASKS; i++)
		if (tasks[i].state != T_FREE) max = i;
	EAX = max;
	memset(b, 0, 76);
	*(u16 *)(b + 6) = wm_slot_at_pos(slot);
	if (slot <= 0 || slot >= MAX_TASKS || tasks[slot].state == T_FREE) {
		*(u16 *)(b + 50) = 9;
		return;
	}
	struct task *o = &tasks[slot];
	struct window *w = &o->win;
	*(u16 *)(b + 4) = wm_stack_pos(o);
	memcpy(b + 10, o->name, MIN(strlen(o->name), 11));
	*(u32 *)(b + 22) = 0;
	*(u32 *)(b + 26) = o->mem_size - 1;
	*(u32 *)(b + 30) = o->pid;
	*(u32 *)(b + 34) = w->x;
	*(u32 *)(b + 38) = w->y;
	*(u32 *)(b + 42) = w->w - 1;
	*(u32 *)(b + 46) = w->h - 1;
	*(u16 *)(b + 50) = o->state == T_WAIT_EVENT || o->state == T_WAIT_TIMEOUT ? 5 :
			   o->state == T_SLEEP ? 1 : 0;
	bool skinned = w->style == 3 || w->style == 4;
	int fw = skinned ? wm_frame_w(w) : 0, sh = skinned ? wm_skin_h(w) : 0;
	*(u32 *)(b + 54) = fw;
	*(u32 *)(b + 58) = sh;
	*(u32 *)(b + 62) = w->w - 2 * fw - 1;
	*(u32 *)(b + 66) = w->h - sh - fw - 1;
	b[70] = w->minimized ? 2 : 0;
	memcpy(b + 71, &o->event_mask, 4);
}

static void wait_event(struct task *t, u32 *r, int timeout)
{
	int ev = task_take_event(t);
	if (ev || timeout == 0) {
		EAX = ev;
		return;
	}
	if (timeout > 0) {
		t->wake_tick = ticks + timeout;
		t->state = T_WAIT_TIMEOUT;
	} else {
		t->state = T_WAIT_EVENT;
	}
}

static u32 read_bdfe(u8 *e, const struct rd_file *f, bool with_name)
{
	memset(e, 0, with_name ? 304 : 40);
	*(u32 *)e = 0x01;                      /* read-only */
	e[4] = 1;                              /* cp866 */
	*(u32 *)(e + 32) = f->size;
	if (with_name) strncpy0((char *)e + 40, f->name, 264);
	return 0;
}

static void file_system(struct task *t, u32 *r)
{
	u8 *info = uptr(t, EBX, 21);
	if (!info) { EAX = 3; return; }
	u32 sub = *(u32 *)info, off = *(u32 *)(info + 4), flags = *(u32 *)(info + 8);
	u32 size = *(u32 *)(info + 12), buf = *(u32 *)(info + 16);
	const char *path = info[20] ? ustr(t, EBX + 20) : (uptr(t, EBX + 21, 4) ? ustr(t, *(u32 *)(info + 21)) : NULL);
	if (!path) { EAX = 3; return; }
	switch (sub) {
	case 0: {                              /* read file */
		const struct rd_file *f = fs_find(path);
		if (!f) { EAX = 5; EBX = -1; return; }
		u32 n = off >= f->size ? 0 : MIN(size, f->size - off);
		u8 *d = uptr(t, buf, n);
		if (!d) { EAX = 3; return; }
		memcpy(d, f->data + off, n);
		EAX = n < size ? 6 : 0;            /* 6: end of file */
		EBX = n;
		return;
	}
	case 1: {                              /* read folder: /sys only */
		extern const char *fs_rd_name(const char *);
		const char *n = fs_rd_name(path);
		if (!n || *n) { EAX = 5; return; }
		u8 *hd = uptr(t, buf, 32 + size * 304);
		if (!hd) { EAX = 3; return; }
		memset(hd, 0, 32);
		u32 got = 0;
		for (u32 i = off; i < (u32)fs_count() && got < size; i++, got++)
			read_bdfe(hd + 32 + got * 304, fs_get(i), true);
		*(u32 *)hd = 1;
		*(u32 *)(hd + 4) = got;
		*(u32 *)(hd + 8) = fs_count();
		EAX = got < size ? 6 : 0;
		EBX = got;
		(void)flags;
		return;
	}
	case 5: {                              /* information */
		const struct rd_file *f = fs_find(path);
		if (!f) { EAX = 5; return; }
		u8 *d = uptr(t, buf, 40);
		if (!d) { EAX = 3; return; }
		read_bdfe(d, f, false);
		EAX = 0;
		return;
	}
	case 7: {                              /* start a program */
		const char *params = flags ? ustr(t, flags) : NULL;
		char p[256];
		if (params) strncpy0(p, params, sizeof(p));
		int pid = load_program(path, params ? p : NULL, NULL);
		EAX = pid;
		return;
	}
	default:
		EAX = 2;                           /* not supported (read-only) */
		return;
	}
}

static void heap(struct task *t, u32 *r)
{
	switch (EBX) {
	case 1:                                /* yield */
		t->state = T_READY;
		extern void sched_yield(void);
		sched_yield();
		return;
	case 11:                               /* init heap */
		EAX = t->heap_end - t->heap_start;
		return;
	case 12: {                             /* allocate: a bump allocator */
		u32 n = ALIGN_UP(ECX + 8, 16);
		if (t->heap_brk + n > t->heap_end) { EAX = 0; return; }
		u32 p = t->heap_brk + 8;
		*(u32 *)(t->mem_pa + t->heap_brk) = n;
		t->heap_brk += n;
		EAX = p;
		return;
	}
	case 13:                               /* free: kept (not reused yet) */
		EAX = 1;
		return;
	default:
		EAX = -1;
		return;
	}
}

void syscall(struct task *t)
{
	u32 *r = t->tf.r;
	int ox, oy;
	switch ((s32)EAX) {
	case -1:
		task_exit(t);
		return;
	case 0: {
		const char *title = (EDX & 0x10000000u) && EDI ? ustr(t, EDI) : NULL;
		wm_define_window(t, EBX, ECX, EDX, ESI, title);
		return;
	}
	case 1:
		wm_origin(t, &ox, &oy);
		gfx_pixel(ox + EBX, oy + ECX, EDX, t->slot);
		return;
	case 2:
		if (wm_active() != t || t->key_head == t->key_tail) { EAX = 1; return; }
		EAX = t->keys[t->key_tail];
		t->key_tail = (t->key_tail + 1) % KEYBUF;
		return;
	case 3: {
		int y, mo, d, h, mi, s;
		civil(rtc_seconds(), &y, &mo, &d, &h, &mi, &s);
		EAX = bcd(s) << 16 | bcd(mi) << 8 | bcd(h);
		return;
	}
	case 4:
		draw_text(t, r);
		return;
	case 5:
		if (EBX) {
			t->wake_tick = ticks + EBX;
			t->state = T_SLEEP;
		}
		return;
	case 7:
		put_image(t, r);
		return;
	case 36:
		read_screen(t, r);
		return;
	case 8:
		define_button(t, r);
		return;
	case 9:
		thread_info(t, r);
		return;
	case 10:
		wait_event(t, r, -1);
		return;
	case 11:
		wait_event(t, r, 0);
		return;
	case 12:
		if (EBX == 1) { t->win.in_redraw = true; t->win.nbtn = 0; }
		else if (EBX == 2) t->win.in_redraw = false;
		return;
	case 13:
		wm_origin(t, &ox, &oy);
		if (EDX & 0x80000000u)
			gfx_hgradient(ox + (s16)(EBX >> 16), oy + (s16)(ECX >> 16), EBX & 0xffff, ECX & 0xffff, EDX, t->slot);
		else
			gfx_rect(ox + (s16)(EBX >> 16), oy + (s16)(ECX >> 16), EBX & 0xffff, ECX & 0xffff, EDX, t->slot);
		return;
	case 14:
		EAX = (u32)(scr.w - 1) << 16 | (scr.h - 1);
		return;
	case 17:
		if (t->btn_head == t->btn_tail) { EAX = 1; return; }
		EAX = t->btns[0];
		t->btn_head = t->btn_tail = 0;
		return;
	case 18:
		switch (EBX) {
		case 2: if (ECX < MAX_TASKS && ECX) task_exit(&tasks[ECX]); return;
		case 3: if (ECX < MAX_TASKS && ECX) wm_activate(&tasks[ECX]); return;
		case 7: { struct task *a = wm_active(); EAX = a ? a->slot : 0; return; }
		case 13: {
			u8 *b = uptr(t, ECX, 16);
			if (b) { b[0] = KOS_VERSION_MAJOR; b[1] = KOS_VERSION_MINOR; b[2] = 0; b[3] = 0; *(u32 *)(b + 4) = 0; b[8] = 0; *(u32 *)(b + 12) = 0; }
			return;
		}
		case 16: EAX = mm_free_kb(); return;
		case 17: EAX = mm_total_kb(); return;
		case 18:
			for (int i = 1; i < MAX_TASKS; i++)
				if (tasks[i].state != T_FREE && tasks[i].pid == ECX) { task_exit(&tasks[i]); EAX = 0; return; }
			EAX = -1;
			return;
		case 21:
			for (int i = 1; i < MAX_TASKS; i++)
				if (tasks[i].state != T_FREE && tasks[i].pid == ECX) { EAX = i; return; }
			EAX = 0;
			return;
		case 22: {                         /* minimise (0, 1) or restore (2, 3) a window */
			struct task *o = NULL;
			if (ECX == 0 || ECX == 2) {
				if (EDX && EDX < MAX_TASKS) o = &tasks[EDX];
			} else {
				for (int i = 1; i < MAX_TASKS; i++)
					if (tasks[i].state != T_FREE && tasks[i].pid == EDX) o = &tasks[i];
			}
			if (!o || o->state == T_FREE || !o->win.defined) { EAX = -1; return; }
			if (ECX < 2) wm_minimize(o);
			else wm_restore(o);
			EAX = 0;
			return;
		}
		case 25: {                         /* the window's layer: get (1), set (2) */
			struct task *o = NULL;
			if ((s32)EDX == -1) o = t;
			else
				for (int i = 1; i < MAX_TASKS; i++)
					if (tasks[i].state != T_FREE && tasks[i].pid == EDX) o = &tasks[i];
			if (!o) { EAX = -1; return; }
			if (ECX == 1) { EAX = o->win.zpos; return; }
			if (ECX == 2) { wm_set_zpos(o, (s32)ESI); EAX = 0; return; }
			EAX = -1;
			return;
		}
		default: EAX = -1; return;
		}
	case 23:
		wait_event(t, r, EBX);
		return;
	case 26:
		if (EBX == 9) EAX = ticks;
		else EAX = -1;
		return;
	case 29: {
		int y, mo, d, h, mi, s;
		civil(rtc_seconds(), &y, &mo, &d, &h, &mi, &s);
		EAX = bcd(d) << 16 | bcd(mo) << 8 | bcd(y % 100);
		return;
	}
	case 37:
		switch (EBX) {
		case 0: EAX = (u32)mouse_x << 16 | mouse_y; return;
		case 1: wm_origin(t, &ox, &oy);
			EAX = (u32)(mouse_x - t->win.x) << 16 | (u16)(mouse_y - t->win.y); return;
		case 2: EAX = mouse_buttons; return;
		case 3: { extern int wm_mouse_events(void); EAX = mouse_buttons | wm_mouse_events(); return; }
		case 7: EAX = 0; return;          /* scroll */
		default: EAX = 0; return;
		}
	case 38:
		wm_origin(t, &ox, &oy);
		gfx_line(ox + (s16)(EBX >> 16), oy + (s16)(ECX >> 16), ox + (s16)EBX, oy + (s16)ECX, EDX, t->slot);
		return;
	case 40: {
		u32 old = t->event_mask;
		t->event_mask = EBX;
		EAX = old;
		return;
	}
	case 47:
		draw_number(t, r);
		return;
	case 48:
		switch (EBX) {
		case 3: {
			u32 *c = uptr(t, ECX, MIN(EDX, 40));
			static const u32 std[10] = { 0x101820, 0x2c5c9e, 0xd0d8e0, 0x101820, 0xffffff,
						    0x2c5c9e, 0xffffff, 0x000000, 0x6080a0, 0xe8ecf0 };
			if (c) memcpy(c, std, MIN(EDX, 40));
			return;
		}
		case 4: EAX = wm_look == LOOK_HILDON ? 0 : SKIN_H; return;
		case 5: EAX = (u32)work_x0 << 16 | work_x1; EBX = (u32)work_y0 << 16 | work_y1; return;
		case 6: wm_set_work_area(ECX >> 16, ECX & 0xffff, EDX >> 16, EDX & 0xffff); return;
		case 7: EAX = (5 << 16) | 5; EBX = (4 << 16) | 4; return;
		case 8: {                          /* the skin, by its file name: built in */
			const char *path = ustr(t, ECX), *n = path;
			if (!path) { EAX = 1; return; }
			for (const char *p = path; *p; p++)
				if (*p == '/') n = p + 1;
			if (!strcmp(n, "hildon.skn")) EAX = wm_set_look(LOOK_HILDON);
			else if (!strcmp(n, "default.skn")) EAX = wm_set_look(LOOK_CLASSIC);
			else EAX = 1;
			return;
		}
		default: EAX = 0; return;
		}
	case 63:
		if (EBX == 1) kputc(ECX & 0xff);
		else if (EBX == 2) {
			int c = dbg_get();
			EAX = c < 0 ? 0 : c;
			EBX = c >= 0;
		}
		return;
	case 66:
		EAX = 0;                           /* keyboard mode: ASCII only */
		return;
	case 67:
		wm_move(t, EBX, ECX, EDX, ESI);
		return;
	case 68:
		heap(t, r);
		return;
	case 70:
		file_system(t, r);
		return;
	case 100: {                            /* KolibriOS for ARM: the modem */
		extern void sys_modem(struct task *t, u32 *r);
		sys_modem(t, r);
		return;
	}
#ifdef __aarch64__
	case 101: {                            /* KolibriOS for ARM: Linux programs */
		extern void sys_linux(struct task *t, u32 *r);
		sys_linux(t, r);
		return;
	}
#endif
	case 72:                               /* a message to the active window */
		EAX = EBX == 1 ? wm_message(ECX, EDX) : -1;
		return;
	case 71:
		if (EBX == 1) {
			const char *s = ustr(t, ECX);
			if (s) {
				strncpy0(t->win.title, s, sizeof(t->win.title));
				wm_draw_frame(t);
			}
		}
		return;
	default:
		kprintf("%s: function %d not implemented\n", t->name, (s32)EAX);
		EAX = -1;
		return;
	}
}
