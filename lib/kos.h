/* KolibriOS for ARM: the system functions for programs in C.
 *
 * KolibriOS's "int 0x40" with eax, ebx, ecx, edx, esi, edi is
 * "svc #0x40" with r0-r5 here; the numbers and meanings are KolibriOS's
 * (docs/sysfuncs.txt of KolibriOS). */
#ifndef KOS_H
#define KOS_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

struct kos_r2 { u32 eax, ebx; };

static inline u32 kos_call(u32 a, u32 b, u32 c, u32 d, u32 s, u32 di)
{
	register u32 r0 asm("r0") = a;
	register u32 r1 asm("r1") = b;
	register u32 r2 asm("r2") = c;
	register u32 r3 asm("r3") = d;
	register u32 r4 asm("r4") = s;
	register u32 r5 asm("r5") = di;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5) :: "memory");
	return r0;
}

static inline struct kos_r2 kos_call2(u32 a, u32 b, u32 c, u32 d, u32 s, u32 di)
{
	register u32 r0 asm("r0") = a;
	register u32 r1 asm("r1") = b;
	register u32 r2 asm("r2") = c;
	register u32 r3 asm("r3") = d;
	register u32 r4 asm("r4") = s;
	register u32 r5 asm("r5") = di;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5) :: "memory");
	struct kos_r2 r = { r0, r1 };
	return r;
}

/* events */
#define EVM_REDRAW 1
#define EVM_KEY 2
#define EVM_BUTTON 4
#define EVM_MOUSE 0x20
enum { KOS_EV_REDRAW = 1, KOS_EV_KEY = 2, KOS_EV_BUTTON = 3, KOS_EV_MOUSE = 6 };

/* window styles for kos_window(): skinned, with caption, coordinates
 * relative to the work area */
#define KOS_WIN_SKINNED 0x33000000u
#define KOS_WIN_FIXED   0x34000000u
#define KOS_WIN_PLAIN   0x01000000u

static inline void kos_exit(void) { kos_call(-1, 0, 0, 0, 0, 0); for (;;); }
static inline void kos_window(int x, int y, int w, int h, u32 style_color, const char *title)
{
	kos_call(0, (u32)x << 16 | (w - 1), (u32)y << 16 | (h - 1), style_color, 0, (u32)title);
}
static inline void kos_window_plain(int x, int y, int w, int h, u32 color, u32 movable_flags)
{
	kos_call(0, (u32)x << 16 | (w - 1), (u32)y << 16 | (h - 1), color, movable_flags, 0);
}
static inline void kos_pixel(int x, int y, u32 c) { kos_call(1, x, y, c, 0, 0); }
static inline u32 kos_get_key(void) { return kos_call(2, 0, 0, 0, 0, 0); }
static inline u32 kos_time(void) { return kos_call(3, 0, 0, 0, 0, 0); }
/* font 0 = 6x10, 1 = 8x16 (cp437), 3 = 8x16 UTF-8 */
static inline void kos_text(int x, int y, const char *s, u32 color, int font)
{
	kos_call(4, (u32)x << 16 | (u16)y, 0x80000000u | (u32)(font & 3) << 28 | (color & 0xffffff), (u32)s, 0, 0);
}
static inline void kos_text_bg(int x, int y, const char *s, u32 color, int font, u32 bg)
{
	kos_call(4, (u32)x << 16 | (u16)y, 0xc0000000u | (u32)(font & 3) << 28 | (color & 0xffffff), (u32)s, 0, bg);
}
static inline void kos_text_scaled(int x, int y, const char *s, u32 color, int font, int scale)
{
	kos_call(4, (u32)x << 16 | (u16)y,
		 0x80000000u | (u32)(font & 3) << 28 | (u32)((scale - 1) & 7) << 24 | (color & 0xffffff),
		 (u32)s, 0, 0);
}
static inline void kos_delay(u32 hundredths) { kos_call(5, hundredths, 0, 0, 0, 0); }
static inline void kos_button(int x, int y, int w, int h, u32 id, u32 color)
{
	kos_call(8, (u32)x << 16 | (w - 1), (u32)y << 16 | (h - 1), id, color, 0);
}
static inline void kos_button_hidden(int x, int y, int w, int h, u32 id)
{
	kos_call(8, (u32)x << 16 | (w - 1), (u32)y << 16 | (h - 1), id | 0x60000000u, 0, 0);
}
static inline void kos_button_delete(u32 id) { kos_call(8, 0, 0, id | 0x80000000u, 0, 0); }
static inline int kos_thread_info(void *buf, int slot) { return kos_call(9, (u32)buf, slot, 0, 0, 0); }
static inline int kos_wait_event(void) { return kos_call(10, 0, 0, 0, 0, 0); }
static inline int kos_check_event(void) { return kos_call(11, 0, 0, 0, 0, 0); }
static inline int kos_wait_event_timeout(u32 t) { return kos_call(23, t, 0, 0, 0, 0); }
static inline void kos_begin_draw(void) { kos_call(12, 1, 0, 0, 0, 0); }
static inline void kos_end_draw(void) { kos_call(12, 2, 0, 0, 0, 0); }
static inline void kos_rect(int x, int y, int w, int h, u32 c)
{
	kos_call(13, (u32)x << 16 | w, (u32)y << 16 | h, c, 0, 0);
}
static inline void kos_screen_size(int *w, int *h)
{
	u32 v = kos_call(14, 0, 0, 0, 0, 0);
	*w = (v >> 16) + 1;
	*h = (v & 0xffff) + 1;
}
static inline u32 kos_get_button(void) { return kos_call(17, 0, 0, 0, 0, 0); }
static inline void kos_activate_slot(int slot) { kos_call(18, 3, slot, 0, 0, 0); }
static inline int kos_active_slot(void) { return kos_call(18, 7, 0, 0, 0, 0); }
static inline void kos_kill_slot(int slot) { kos_call(18, 2, slot, 0, 0, 0); }
static inline u32 kos_free_ram_kb(void) { return kos_call(18, 16, 0, 0, 0, 0); }
static inline u32 kos_total_ram_kb(void) { return kos_call(18, 17, 0, 0, 0, 0); }
static inline u32 kos_uptime(void) { return kos_call(26, 9, 0, 0, 0, 0); }
static inline u32 kos_date(void) { return kos_call(29, 0, 0, 0, 0, 0); }
static inline u32 kos_mouse_pos(void) { return kos_call(37, 0, 0, 0, 0, 0); }
static inline u32 kos_mouse_win(void) { return kos_call(37, 1, 0, 0, 0, 0); }
static inline u32 kos_mouse_buttons(void) { return kos_call(37, 2, 0, 0, 0, 0); }
static inline void kos_line(int x0, int y0, int x1, int y1, u32 c)
{
	kos_call(38, (u32)x0 << 16 | (u16)x1, (u32)y0 << 16 | (u16)y1, c, 0, 0);
}
static inline u32 kos_event_mask(u32 m) { return kos_call(40, m, 0, 0, 0, 0); }
static inline void kos_number(int x, int y, u32 v, int digits, u32 color, int font)
{
	kos_call(47, (u32)digits << 16 | 0x80000000u, v, (u32)x << 16 | (u16)y,
		 (u32)(font & 3) << 28 | (color & 0xffffff), 0);
}
static inline int kos_skin_height(void) { return kos_call(48, 4, 0, 0, 0, 0); }
static inline void kos_debug_char(char c) { kos_call(63, 1, (u8)c, 0, 0, 0); }
static inline void kos_move_window(int x, int y, int w, int h) { kos_call(67, x, y, w, h, 0); }
static inline void *kos_malloc(u32 n) { return (void *)kos_call(68, 12, n, 0, 0, 0); }
static inline void kos_set_caption(const char *s) { kos_call(71, 1, (u32)s, 0, 0, 0); }
/* function 7: an image of 3 bytes (B, G, R) a pixel; 36: the screen's */
static inline void kos_put_image(const void *img, int w, int h, int x, int y)
{
	kos_call(7, (u32)img, (u32)w << 16 | h, (u32)x << 16 | (u16)y, 0, 0);
}
static inline void kos_read_screen(void *buf, int x, int y, int w, int h)
{
	kos_call(36, (u32)buf, (u32)w << 16 | h, (u32)x << 16 | (u16)y, 0, 0);
}
/* function 18.22: minimise (0) or restore (2) the window of a slot */
static inline void kos_minimize_slot(int slot) { kos_call(18, 22, 0, slot, 0, 0); }
static inline void kos_restore_slot(int slot) { kos_call(18, 22, 2, slot, 0, 0); }
/* function 18.25: the own window's layer (ZPOS_*) */
#define KOS_ZPOS_ALWAYS_BACK (-1)
#define KOS_ZPOS_NORMAL 0
#define KOS_ZPOS_ALWAYS_TOP 1
static inline void kos_set_zpos(int zpos) { kos_call(18, 25, 2, -1, zpos, 0); }
/* function 48: the work area (inclusive), the skin by its file name */
static inline void kos_set_work_area(int x0, int x1, int y0, int y1)
{
	kos_call(48, 6, (u32)x0 << 16 | x1, (u32)y0 << 16 | y1, 0, 0);
}
static inline int kos_set_skin(const char *path) { return kos_call(48, 8, (u32)path, 0, 0, 0); }
/* function 72.1: a button press (its identifier) for the active window */
static inline int kos_send_button(u32 id) { return kos_call(72, 1, 3, id, 0, 0); }

/* function 70: files */
struct kos_fileinfo {
	u32 sub, offset, flags, size, buf;
	u8 zero;
	const char *path __attribute__((packed));
} __attribute__((packed));

static inline int kos_run(const char *path, const char *params)
{
	struct kos_fileinfo fi = { 7, 0, (u32)params, 0, 0, 0, path };
	return kos_call(70, (u32)&fi, 0, 0, 0, 0);
}

static inline struct kos_r2 kos_read_file(const char *path, void *buf, u32 size, u32 offset)
{
	struct kos_fileinfo fi = { 0, offset, 0, size, (u32)buf, 0, path };
	return kos_call2(70, (u32)&fi, 0, 0, 0, 0);
}

/* function 70.2: a file created or rewritten; 70.9: a folder (with the
 * folders above it). Writable on the AArch64 kernel: the Linux
 * personality's tree (/home/user/..., /tmp/...), kept in RAM */
static inline struct kos_r2 kos_write_file(const char *path, const void *buf, u32 size)
{
	struct kos_fileinfo fi = { 2, 0, 0, size, (u32)buf, 0, path };
	return kos_call2(70, (u32)&fi, 0, 0, 0, 0);
}

static inline int kos_create_folder(const char *path)
{
	struct kos_fileinfo fi = { 9, 0, 0, 0, 0, 0, path };
	return kos_call(70, (u32)&fi, 0, 0, 0, 0);
}

/* folder: entries of 304 bytes (the name at +40) after a 32-byte header */
static inline struct kos_r2 kos_read_folder(const char *path, void *buf, u32 first, u32 count)
{
	struct kos_fileinfo fi = { 1, first, 1, count, (u32)buf, 0, path };
	return kos_call2(70, (u32)&fi, 0, 0, 0, 0);
}

/* KolibriOS for ARM's own: event 11 (mask bit 10, never in the default
 * mask), the system changed: a window came, went, was activated,
 * minimised or restored; the display went off or on; a call or a
 * message came or went (the shells wait for it instead of looking every
 * 250 ms); function 102.0, the system's state: KOS_SYS_DISPLAY_OFF while
 * the display is off (the Pro1X's standby) */
enum { KOS_EV_SYSTEM = 11 };
#define EVM_SYSTEM 0x400
#define KOS_SYS_DISPLAY_OFF 1
static inline u32 kos_system_state(void) { return kos_call(102, 0, 0, 0, 0, 0); }

/* threads (51), IPC (60), named shared memory (68.22, 68.23), libraries
 * (68.19): KolibriOS's functions (docs/threads.md) */
enum { KOS_EV_IPC = 7 };
#define EVM_IPC 0x40
/* a thread in this program's address space: entry with the stack
 * pointer stack (the top of memory of the program's own); its TID or -1.
 * It starts with all registers 0 and ends with function -1 (kos_exit) */
static inline int kos_thread_create(void (*entry)(void), void *stack)
{
	return kos_call(51, 1, (u32)entry, (u32)stack, 0, 0);
}
static inline int kos_thread_slot(void) { return kos_call(51, 2, 0, 0, 0, 0); }
static inline int kos_kill_pid(u32 pid) { return kos_call(18, 18, pid, 0, 0, 0); }
static inline int kos_slot_of_pid(u32 pid) { return kos_call(18, 21, pid, 0, 0, 0); }
/* the IPC buffer: +0 locked (non-zero), +4 the bytes used (8 at first),
 * then the messages: the sender's PID, the length, the data */
static inline void kos_ipc_set_area(void *buf, u32 size) { kos_call(60, 1, (u32)buf, size, 0, 0); }
/* 0 sent, 1 no buffer, 2 locked, 3 full, 4 no such PID */
static inline int kos_ipc_send(u32 pid, const void *msg, u32 len) { return kos_call(60, 2, pid, (u32)msg, len, 0); }
#define KOS_SHM_READ 0
#define KOS_SHM_WRITE 1
#define KOS_SHM_OPEN 0
#define KOS_SHM_OPEN_ALWAYS 4
#define KOS_SHM_CREATE 8
/* the area's address or NULL; *edx: 0 created, else its size, or the error */
static inline void *kos_shm_open(const char *name, u32 size, u32 flags, u32 *edx)
{
	register u32 r0 asm("r0") = 68;
	register u32 r1 asm("r1") = 22;
	register u32 r2 asm("r2") = (u32)name;
	register u32 r3 asm("r3") = size;
	register u32 r4 asm("r4") = flags;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) :: "memory");
	if (edx) *edx = r3;
	return (void *)r0;
}
static inline void kos_shm_close(const char *name) { kos_call(68, 23, (u32)name, 0, 0, 0); }
/* a library's export table ({ name, value } pairs, 0-terminated) or NULL */
static inline void *kos_load_dll(const char *path) { return (void *)kos_call(68, 19, (u32)path, 0, 0, 0); }

/* klib.c */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
unsigned strlen(const char *s);
char *strcpy(char *d, const char *s);
int strcmp(const char *a, const char *b);
char *utoa(u32 v, char *buf, int base);
int ksprintf(char *buf, const char *fmt, ...);
void kdebug(const char *fmt, ...);

#endif
