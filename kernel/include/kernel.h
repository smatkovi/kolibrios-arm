/* KolibriOS for ARM: the kernel's common definitions */
#ifndef KERNEL_H
#define KERNEL_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef unsigned int size_t;
#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
typedef _Bool bool;
#define true 1
#define false 0
#endif
#define NULL ((void *)0)

#define KOS_VERSION_MAJOR 0
#define KOS_VERSION_MINOR 1

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static inline u32 rd32(u32 a) { return *(volatile u32 *)a; }
static inline void wr32(u32 a, u32 v) { *(volatile u32 *)a = v; }
static inline u8 rd8(u32 a) { return *(volatile u8 *)a; }
static inline void wr8(u32 a, u8 v) { *(volatile u8 *)a = v; }

/* lib.c */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy0(char *d, const char *s, size_t n); /* always terminated */
int kprintf(const char *fmt, ...);
void kputc(char c);
void kputs(const char *s);
void panic(const char *fmt, ...) __attribute__((noreturn));

/* The registers saved on every exception (start.S builds it on the
 * kernel stack; tasks keep a copy): r0-r12, the user mode sp and lr, a
 * pad word, the interrupted pc and cpsr */
struct trapframe {
	u32 r[13];
	u32 usr_sp, usr_lr, pad;
	u32 pc, cpsr;
};

enum { TRAP_UNDEF = 1, TRAP_SVC, TRAP_PABT, TRAP_DABT, TRAP_IRQ, TRAP_FIQ };

#define PSR_USR 0x10
#define PSR_SVC 0x13
#define PSR_I 0x80
#define PSR_F 0x40

/* the platform (kernel/platform/<name>.c) */
struct mem_region {
	u32 va, pa, size;
	u32 attr; /* MAP_* */
};
#define MAP_NORMAL 1   /* cacheable RAM */
#define MAP_DEVICE 2
#define MAP_UNCACHED 3 /* normal memory, not cached (frame buffers) */

struct platform {
	const char *name;
	u32 ram_base, ram_size;
	/* I/O regions to map (va != pa allowed: devices below 1 GiB move up) */
	const struct mem_region *io;
	int nio;
	void (*early_init)(void);     /* MMU off: debug output only */
	void (*init)(void);           /* MMU on: interrupts, timer, input */
	void (*putc)(char c);
	void (*irq)(void);            /* one interrupt (or all pending) */
	/* the display: the kernel draws into a 32 bpp XRGB buffer of
	 * width x height; flush() brings the changed lines to the screen */
	int (*display_init)(u32 **fb, int *width, int *height, int *stride);
	bool (*display_flush)(int y0, int y1); /* false: not now, again later */
	u32 (*rtc_time)(void);        /* Unix time, or 0 */
	bool cached_data;             /* D-cache on */
};
extern const struct platform *plat;
extern const struct platform platform_realview, platform_n950;

/* mmu.c */
void mmu_init(void);
u32 *mmu_new_user_table(void);
void mmu_map_user(u32 *l1, u32 va, u32 pa, u32 size);
void mmu_switch(u32 *l1);
void mmu_free_user_table(u32 *l1);
u32 mmu_io_va(u32 pa);
void mmu_set_uncached(u32 pa, u32 size);
void cache_sync_code(u32 va, u32 len);

/* mm.c: physical memory in 1 MiB blocks (task memory, frame buffers)
 * and a kernel heap for small objects */
void mm_init(u32 base, u32 size, u32 kernel_end);
u32 mm_alloc_mb(u32 mb);       /* physical address or 0 */
void mm_free_mb(u32 pa, u32 mb);
void mm_reserve(u32 pa, u32 size); /* in use already (a boot loader's frame buffer) */
u32 mm_free_kb(void);
u32 mm_total_kb(void);
void *kmalloc(u32 size);       /* never freed: tables, buffers */
void *kmalloc_aligned(u32 size, u32 align);

/* time */
extern volatile u32 ticks;     /* 1/100 s since boot */
void timer_tick(void);         /* the platform's 100 Hz interrupt */
u32 rtc_seconds(void);         /* Unix time */

/* input (input.c): what the platform's drivers report */
void input_key_set2(u8 byte);  /* PS/2 scan code set 2 byte */
void input_key(u8 set1, bool released); /* a key, XT (set 1) scan code */
void input_mouse_rel(int dx, int dy, int buttons, int wheel);
void input_mouse_abs(int x, int y, int buttons); /* touch screens */

/* sched.c */
void trap_entry(struct trapframe *tf, int kind);
void idle_loop(void);

#endif
