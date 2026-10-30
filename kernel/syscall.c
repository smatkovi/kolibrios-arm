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

/* the civil date of a Unix time (kfs.c too) */
void civil(u32 t, int *y, int *mo, int *d, int *h, int *mi, int *s)
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
	gfx_read(buf, x, y, w, h);
}

static void put_image(struct task *t, u32 *r)
{
	int w = ECX >> 16, h = ECX & 0xffff;
	const u8 *img = uptr(t, EBX, (u32)w * h * 3);
	if (!img) return;
	int ox, oy;
	wm_origin(t, &ox, &oy);
	int x = ox + (s16)(EDX >> 16), y = oy + (s16)(EDX & 0xffff);
	gfx_image(x, y, w, h, img, t->slot);
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

/* function 70: kfs.c (the volumes, their files and folders); 30 too */
void kfs_syscall(struct task *t, u32 *r);
void kfs_cur_dir(struct task *t, u32 *r);

static u32 heap_alloc(struct task *t, u32 size)
{
	u32 n = ALIGN_UP(size + 8, 16);
	if (size > t->heap_end || t->heap_brk + n > t->heap_end) return 0;
	u32 p = t->heap_brk + 8;
	*(u32 *)(t->mem_pa + t->heap_brk) = n;
	t->heap_brk += n;
	return p;
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
	case 12:                               /* allocate: a bump allocator */
		EAX = heap_alloc(t, ECX);
		return;
	case 13:                               /* free: kept (not reused yet) */
		EAX = 1;
		return;
	default:
		EAX = -1;
		return;
	}
}

/* function 65: an image of 1, 2, 4, 8 (with a palette), 9 (gray), 15,
 * 16, 24 or 32 bits a pixel, ebp (r6) bytes more after each row; drawn
 * a row at a time through function 7's way */
static void put_image_ext(struct task *t, u32 *r)
{
	static u8 row[3 * 4096];
	int w = ECX >> 16, h = ECX & 0xffff, bpp = ESI;
	s32 pad = r[6];
	if (w <= 0 || h <= 0 || w > 4096) return;
	u32 rowlen;
	switch (bpp) {
	case 1: case 2: case 4: rowlen = ((u32)w * bpp + 7) / 8; break;
	case 8: case 9: rowlen = w; break;
	case 15: case 16: rowlen = (u32)w * 2; break;
	case 24: rowlen = (u32)w * 3; break;
	case 32: rowlen = (u32)w * 4; break;
	default: return;
	}
	s32 stride = (s32)rowlen + pad;
	if (stride < (s32)rowlen) return;
	const u8 *img = uptr(t, EBX, (u32)stride * (h - 1) + rowlen);
	const u32 *pal = NULL;
	if (bpp <= 8) {
		pal = uptr(t, EDI, 4u << bpp);
		if (!pal) return;
	}
	if (!img) return;
	int ox, oy;
	wm_origin(t, &ox, &oy);
	int x = ox + (s16)(EDX >> 16), y = oy + (s16)(EDX & 0xffff);
	for (int j = 0; j < h; j++) {
		if (y + j < 0) continue;
		if (y + j >= scr.h) break;
		const u8 *s = img + (u32)stride * j;
		u8 *d = row;
		for (int i = 0; i < w; i++, d += 3) {
			u32 c;
			switch (bpp) {
			case 1: c = pal[(s[i >> 3] >> (7 - (i & 7))) & 1]; break;
			case 2: c = pal[(s[i >> 2] >> (6 - 2 * (i & 3))) & 3]; break;
			case 4: c = pal[(s[i >> 1] >> (i & 1 ? 0 : 4)) & 15]; break;
			case 8: c = pal[s[i]]; break;
			case 9: c = s[i] * 0x010101u; break;
			case 15: { u32 v = s[2 * i] | s[2 * i + 1] << 8;
				c = ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31; break; }
			case 16: { u32 v = s[2 * i] | s[2 * i + 1] << 8;
				c = (v >> 11) * 255 / 31 << 16 | ((v >> 5) & 63) * 255 / 63 << 8 | (v & 31) * 255 / 31; break; }
			case 24: c = s[3 * i] | s[3 * i + 1] << 8 | s[3 * i + 2] << 16; break;
			default: c = s[4 * i] | s[4 * i + 1] << 8 | s[4 * i + 2] << 16; break;
			}
			d[0] = c;
			d[1] = c >> 8;
			d[2] = c >> 16;
		}
		gfx_image(x, y + j, w, 1, row, t->slot);
	}
}

/* function 54: the clipboard, slots kept one after the other in a block
 * of 1 MiB; each slot is what the program wrote (KolibriOS's format: a
 * dword of the slot's size, its type, its encoding, the data). 54.1
 * gives the program a copy in its heap (68.12's memory, freed by 68.13) */
#define CLIP_SIZE (1u << 20)
#define CLIP_SLOTS 64
static u8 *clip;
static u32 clip_len[CLIP_SLOTS], clip_used;
static int clip_n;

static u32 heap_alloc(struct task *t, u32 size);

static void clipboard(struct task *t, u32 *r)
{
	switch (EBX) {
	case 0: EAX = clip_n; return;
	case 1: {
		if (ECX >= (u32)clip_n) { EAX = 1; return; }
		u32 off = 0;
		for (u32 i = 0; i < ECX; i++) off += clip_len[i];
		u32 p = heap_alloc(t, clip_len[ECX]);
		u8 *d = p ? uptr(t, p, clip_len[ECX]) : NULL;
		if (!d) { EAX = 1; return; }
		memcpy(d, clip + off, clip_len[ECX]);
		EAX = p;
		return;
	}
	case 2: {
		const u8 *s = ECX ? uptr(t, EDX, ECX) : NULL;
		if (!clip) clip = (u8 *)mm_alloc_mb(1);
		if (!s || !clip || clip_n == CLIP_SLOTS || clip_used + ECX > CLIP_SIZE) { EAX = 1; return; }
		memcpy(clip + clip_used, s, ECX);
		clip_used += ECX;
		clip_len[clip_n++] = ECX;
		EAX = 0;
		return;
	}
	case 3:
		if (!clip_n) { EAX = 1; return; }
		clip_used -= clip_len[--clip_n];
		EAX = 0;
		return;
	case 4: EAX = 0; return;
	default: EAX = -1; return;
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
		else if (EBX == 2 && ECX == 9) EAX = 1;            /* the layout's language: en */
		else if (EBX == 2) {                               /* the keyboard layout */
			extern void input_layout(int which, u8 *out);
			u8 *d = uptr(t, EDX, 128);
			if (d) input_layout(ECX, d);
			else EAX = -1;
		} else if (EBX == 5) EAX = 1;                      /* the system's language: en */
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
		case 7: {                         /* the wheel, for the active window, read once */
			extern int input_wheel;
			if (wm_active() != t) { EAX = 0; return; }
			EAX = (u16)(s16)MAX(-32768, MIN(input_wheel, 32767));
			input_wheel = 0;
			return;
		}
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
	case 66: {                             /* the keyboard */
		extern u32 input_modifiers(void);
		switch (EBX) {
		case 1: t->kbd_mode = ECX == 1; return;
		case 2: EAX = t->kbd_mode; return;
		case 3: EAX = input_modifiers(); return;
		case 4: case 5: EAX = 0; return;   /* hotkeys: accepted, never sent */
		default: return;
		}
	}
	case 54:
		clipboard(t, r);
		return;
	case 65:
		put_image_ext(t, r);
		return;
	case 30:
		kfs_cur_dir(t, r);
		return;
	case 67:
		wm_move(t, EBX, ECX, EDX, ESI);
		return;
	case 68:
		heap(t, r);
		return;
	case 70:
		kfs_syscall(t, r);
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
