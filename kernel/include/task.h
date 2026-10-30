/* KolibriOS for ARM: tasks (KolibriOS's threads: each with its slot, PID,
 * window and events) and processes (an address space and what is mapped
 * into it, shared by the threads of a program: function 51) */
#ifndef TASK_H
#define TASK_H

#include "kernel.h"

#define MAX_TASKS 128     /* slot 0 is unused, as in KolibriOS (scr.owner: at most 255) */
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
	T_LX_WAIT,        /* a Linux thread in a system call (kernel/linux) */
};

struct lx_thread;

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
	int zpos;         /* function 18.25: ZPOS_* */
	char title[64];
	struct button btn[MAX_BUTTONS];
	int nbtn;
	bool in_redraw;   /* between 12.1 and 12.2 */
};

/* memory mapped into a process beyond its image (VA 0 .. mem_size):
 * named shared memory (function 68.22) and libraries (68.19), in blocks
 * of USER_ALIGN_MB MiB from MAP_START on, below the first GiB's end */
#define MAX_MAPS 32
#define MAX_DLLS 16
#define MAP_START 0x10000000u
#define MAP_END 0x40000000u
enum { UMAP_SHM = 1, UMAP_DLL };
struct shmem;
struct umap {
	u32 va, size;
	addr_t pa;
	u8 kind;
	bool ro;
	u32 owner_pid;    /* shared memory: the thread that opened it (closed when it ends) */
	struct shmem *shm;
	u32 used;         /* libraries: the bytes taken from the front */
};
struct pdll {         /* a library loaded into the process (kernel/dll.c) */
	char path[128];
	u32 exports;      /* the VA of its export table */
	u32 refs;
};
struct proc {
	int threads;      /* its tasks; 0: the entry is free */
	u32 pid;          /* its first thread's */
	void *l1;
	addr_t mem_pa;    /* the image: VA 0 .. mem_size at mem_pa */
	u32 mem_size;
	u32 img_end;
	u32 heap_start, heap_end, heap_brk; /* function 68 */
	struct umap map[MAX_MAPS];
	int nmap;
	struct pdll dll[MAX_DLLS];
	int ndll;
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
	u32 wake_tick;    /* T_SLEEP, T_WAIT_TIMEOUT */
	u32 events;       /* pending events, bit (n - 1) for event n */
	u32 event_mask;   /* function 40 */
	u32 keys[KEYBUF]; /* function 2's values */
	int key_head, key_tail;
	u32 btns[BTNBUF]; /* function 17's values */
	int btn_head, btn_tail;
	struct window win;
	u8 kbd_mode;      /* function 66.1: 0 ASCII, 1 scan codes */
	char cwd[256];    /* function 30: the current folder ("/sys" at first) */
	struct lx_thread *lx;   /* a Linux program's thread (AArch64), or NULL */
	struct proc *proc;      /* a KolibriOS program's process, or NULL (Linux) */
	u32 ipc_start, ipc_size;  /* function 60.1: the IPC buffer (VA), or 0 */
	u8 priority;            /* function 51.3/51.4: kept, not used by the scheduler */
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

/* proc.c: processes, threads (51), IPC (60), named shared memory
 * (68.22, 68.23) */
struct proc *proc_alloc(void);
void proc_thread_gone(struct task *t);  /* from task_exit */
struct task *task_by_pid(u32 pid);
void sys_threads(struct task *t, u32 *r);
void sys_ipc(struct task *t, u32 *r);
void sys_shmem_open(struct task *t, u32 *r);
void sys_shmem_close(struct task *t, u32 *r);
/* a region of the process's map area (blocks of USER_ALIGN_MB MiB),
 * mapped, zeroed; NULL if there is no room or memory */
struct umap *proc_map_new(struct proc *p, u32 size, int kind);
void proc_map_free(struct proc *p, struct umap *m);

/* dll.c: libraries (68.18, 68.19) */
void sys_load_dll(struct task *t, u32 *r);

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
