/* KolibriOS for ARM: tasks (threads with their address space), their
 * windows and events */
#ifndef TASK_H
#define TASK_H

#include "kernel.h"

#define MAX_TASKS 64      /* slot 0 is unused, as in KolibriOS */
#define MAX_BUTTONS 64    /* per window */
#define KEYBUF 64
#define BTNBUF 16

/* events (function 10, 11, 23 return these numbers) */
enum {
	EV_REDRAW = 1, EV_KEY = 2, EV_BUTTON = 3, EV_BACKGROUND = 5,
	EV_MOUSE = 6, EV_IPC = 7, EV_NETWORK = 8, EV_DEBUG = 9,
};

enum task_state {
	T_FREE = 0,
	T_READY,
	T_WAIT_EVENT,     /* function 10 */
	T_WAIT_TIMEOUT,   /* function 23 */
	T_SLEEP,          /* function 5 */
	T_ZOMBIE,
};

struct button {
	int x, y, w, h;   /* screen coordinates */
	u32 id;           /* 24 bits */
	u32 color;
	bool hidden, noframe;
};

struct window {
	bool defined;     /* function 0 was called */
	bool minimized;
	int x, y, w, h;   /* screen position; w, h are KolibriOS sizes (+1) */
	int style;        /* Y of edx: 1, 3, 4 */
	u32 flags;        /* X of edx: caption, relative, no fill, gradient */
	u32 work_color;
	u32 title_color;
	bool movable;
	char title[64];
	struct button btn[MAX_BUTTONS];
	int nbtn;
	bool in_redraw;   /* between 12.1 and 12.2 */
};

struct task {
	enum task_state state;
	int slot;
	u32 pid;
	char name[12];    /* the file name, as function 9 reports it */
	struct trapframe tf;
	void *l1;         /* the page table (TTBR0) */
	addr_t mem_pa;    /* the address space: VA 0.. mem_size at mem_pa */
	u32 mem_size;
	u32 img_end;
	u32 wake_tick;    /* T_SLEEP, T_WAIT_TIMEOUT */
	u32 events;       /* pending events, bit (n - 1) for event n */
	u32 event_mask;   /* function 40 */
	u32 keys[KEYBUF]; /* function 2's values */
	int key_head, key_tail;
	u32 btns[BTNBUF]; /* function 17's values */
	int btn_head, btn_tail;
	struct window win;
	u32 heap_start, heap_end, heap_brk; /* function 68 */
};

extern struct task tasks[MAX_TASKS];
extern struct task *current;

/* sched.c */
void sched_init(void);
void schedule(void);
struct task *task_alloc(void);
void task_exit(struct task *t);
void task_post_event(struct task *t, int ev);
int task_take_event(struct task *t); /* 0: none in the mask */
void wake_waiters(void);

/* a user pointer, checked against the task's address space: the
 * kernel's (identity mapped) address of it, or NULL */
void *uptr(struct task *t, u32 va, u32 len);
const char *ustr(struct task *t, u32 va); /* a terminated string or NULL */

/* syscall.c */
void syscall(struct task *t);

/* loader.c */
int load_program(const char *path, const char *params, struct task **out);

/* fs.c: the ramdisk */
struct rd_file {
	const char *name;
	const u8 *data;
	u32 size;
};
void fs_init(void);
const struct rd_file *fs_find(const char *path);
int fs_count(void);
const struct rd_file *fs_get(int i);

#endif
