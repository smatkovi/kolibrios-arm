/* KolibriOS for ARM: processes and their threads (function 51), IPC
 * (function 60), named shared memory (68.22, 68.23) and futexes (77.0-
 * 77.3), as KolibriOS's kernel has them (core/taskman.inc's
 * new_sys_threads, core/sys32.inc's terminate, core/memory.inc's sys_IPC,
 * core/heap.inc's shmem_open and shmem_close; the futexes as
 * docs/sysfuncs.txt describes them).
 *
 * A process is an address space: the program's image from VA 0 (its
 * memory, one piece of physical memory), its heap (68.11, 68.12), and
 * what is mapped beyond it: named shared memory and libraries (68.19,
 * dll.c), in blocks of USER_ALIGN_MB MiB (1 MiB sections on ARMv7, 2 MiB
 * blocks on AArch64; KolibriOS maps 4 KiB pages) from MAP_START on. A
 * thread is a task of its own: its slot, PID, window, events, keys,
 * IPC buffer and FP/SIMD registers (lazily, by slot and PID: fpu.c), the
 * same tables as the other threads of its process. The process ends
 * with its last thread; ending one thread (-1, 18.2, 18.18, a fault)
 * leaves the others running, as in KolibriOS (console.obj's thread ends
 * its program's main thread by 18.18 and then itself). */
#include "gui.h"

static struct proc procs[MAX_TASKS];

#define BLOCK ((u32)USER_ALIGN_MB << 20)

static inline void put32(void *p, u32 v) { struct __attribute__((packed)) w32 { u32 v; } *q = p; q->v = v; }
static inline u32 get32(const void *p) { const struct __attribute__((packed)) w32 { u32 v; } *q = p; return q->v; }

struct proc *proc_alloc(void)
{
	for (int i = 0; i < MAX_TASKS; i++)
		if (!procs[i].threads) {
			memset(&procs[i], 0, sizeof(procs[i]));
			return &procs[i];
		}
	return NULL;
}

struct task *task_by_pid(u32 pid)
{
	for (int i = 1; i < MAX_TASKS; i++)
		if (tasks[i].state != T_FREE && tasks[i].pid == pid) return &tasks[i];
	return NULL;
}

/* --- the map area ------------------------------------------------- */

struct umap *proc_map_new(struct proc *p, u32 size, int kind)
{
	if (p->nmap == MAX_MAPS || !size) return NULL;
	size = ALIGN_UP(size, BLOCK);
	/* the lowest free range, from MAP_START (or behind a large image) on */
	u32 va = MAX(MAP_START, ALIGN_UP(p->mem_size, BLOCK));
	for (bool moved = true; moved;) {
		moved = false;
		for (int i = 0; i < p->nmap; i++) {
			struct umap *m = &p->map[i];
			if (va < m->va + m->size && m->va < va + size) {
				va = m->va + m->size;
				moved = true;
			}
		}
	}
	if (va + size > MAP_END || va + size < va) return NULL;
	addr_t pa = 0;
	if (kind != UMAP_SHM) {
		pa = mm_alloc_mb_aligned(size >> 20, USER_ALIGN_MB);
		if (!pa) return NULL;
		memset((void *)pa, 0, size);
	}
	struct umap *m = &p->map[p->nmap++];
	memset(m, 0, sizeof(*m));
	m->va = va;
	m->size = size;
	m->pa = pa;
	m->kind = kind;
	static u32 seq;
	m->seq = ++seq;
	if (pa) mmu_map_user(p->l1, va, pa, size);
	return m;
}

static void futex_proc_gone(struct proc *p);

/* the mapping gone (its memory too, unless it is shared memory's) */
void proc_map_free(struct proc *p, struct umap *m)
{
	mmu_unmap_user(p->l1, m->va, m->size);
	if (m->kind != UMAP_SHM && m->pa) mm_free_mb(m->pa, m->size >> 20);
	int i = m - p->map;
	p->map[i] = p->map[--p->nmap];
}

/* --- named shared memory (68.22, 68.23) ------------------------------ */

#define MAX_SHMEM 64
#define SHM_WRITE 1
#define SHM_ACCESS_MASK 3
#define SHM_OPEN (0 << 2)
#define SHM_OPEN_ALWAYS (1 << 2)
#define SHM_CREATE (2 << 2)
#define SHM_OPEN_MASK (3 << 2)
#define E_NOTFOUND 5
#define E_ACCESS 10
#define E_NOMEM 30
#define E_PARAM 33

struct shmem {
	char name[32];
	addr_t pa;
	u32 size;         /* as asked for, in whole pages (4 KiB) */
	u32 blocks;       /* the memory: whole blocks of USER_ALIGN_MB MiB */
	u32 access;
	int refs;         /* its mappings */
};
static struct shmem shm[MAX_SHMEM];

static void shm_unref(struct shmem *s)
{
	if (--s->refs > 0) return;
	mm_free_mb(s->pa, s->blocks * USER_ALIGN_MB);
	s->name[0] = 0;
	s->pa = 0;
}

static void shm_close_map(struct proc *p, struct umap *m)
{
	struct shmem *s = m->shm;
	proc_map_free(p, m);
	if (s) shm_unref(s);
}

/* KolibriOS compares 32 bytes of the names (strncmp) */
static bool shm_name_is(const struct shmem *s, const char *n)
{
	return s->pa && !strncmp(s->name, n, 31);
}

void sys_shmem_open(struct task *t, u32 *r)
{
	const char *name = ustr(t, r[2]);
	u32 size = r[3], access = r[4], action = access & SHM_OPEN_MASK;
	struct proc *p = t->proc;
	r[0] = 0;
	if (!name || !p) { r[3] = E_PARAM; return; }
	struct shmem *s = NULL;
	for (int i = 0; i < MAX_SHMEM && !s; i++)
		if (shm_name_is(&shm[i], name)) s = &shm[i];
	u32 owner_access = 0;
	if (!s) {
		if (action == SHM_OPEN) { r[3] = E_NOTFOUND; return; }
		if (action != SHM_CREATE && action != SHM_OPEN_ALWAYS) { r[3] = E_PARAM; return; }
		if (!size || size > MAP_END - MAP_START) { r[3] = E_PARAM; return; }
		for (int i = 0; i < MAX_SHMEM && !s; i++)
			if (!shm[i].pa) s = &shm[i];
		size = ALIGN_UP(size, 4096u);
		u32 blocks = ALIGN_UP(size, BLOCK) / BLOCK;
		addr_t pa = s ? mm_alloc_mb_aligned(blocks * USER_ALIGN_MB, USER_ALIGN_MB) : 0;
		if (!pa) { r[3] = E_NOMEM; return; }
		memset((void *)pa, 0, blocks * BLOCK);
		memset(s, 0, sizeof(*s));
		strncpy0(s->name, name, sizeof(s->name));
		s->pa = pa;
		s->size = size;
		s->blocks = blocks;
		s->access = access & SHM_ACCESS_MASK;
		action = SHM_OPEN;
		owner_access = SHM_WRITE;
	} else if (action == SHM_CREATE) {
		r[3] = E_ACCESS;                   /* it is there already */
		return;
	} else if (action != SHM_OPEN && action != SHM_OPEN_ALWAYS) {
		r[3] = E_PARAM;
		return;
	}
	/* .create_map: no more than its creator allowed */
	u32 acc = access & SHM_ACCESS_MASK;
	if (acc > s->access) {
		r[3] = E_ACCESS;
		return;
	}
	s->refs++;
	struct umap *m = proc_map_new(p, s->blocks * BLOCK, UMAP_SHM);
	if (!m) {
		shm_unref(s);
		r[3] = E_NOMEM;
		return;
	}
	m->pa = s->pa;
	m->shm = s;
	m->owner_pid = t->pid;
	m->ro = !((acc | owner_access) & SHM_WRITE);
	mmu_map_user_ro(p->l1, m->va, m->pa, m->size, m->ro);
	r[0] = m->va;
	r[3] = owner_access ? 0 : s->size;     /* created: 0; opened: its size */
}

/* 68.23: the thread's latest mapping of the area of that name (KolibriOS
 * finds the newest of the thread's objects first), else one its process
 * kept from a thread that ended */
void sys_shmem_close(struct task *t, u32 *r)
{
	const char *name = ustr(t, r[2]);
	struct proc *p = t->proc;
	if (!name || !p) return;
	for (int pass = 0; pass < 2; pass++) {
		struct umap *best = NULL;
		for (int i = 0; i < p->nmap; i++) {
			struct umap *m = &p->map[i];
			if (m->kind == UMAP_SHM && m->owner_pid == (pass ? 0 : t->pid) && m->shm &&
			    !strncmp(m->shm->name, name, 31) && (!best || m->seq > best->seq))
				best = m;
		}
		if (best) {
			shm_close_map(p, best);
			return;
		}
	}
}

/* --- threads (51) ------------------------------------------------------- */

/* task_exit. The thread's shared memory stays mapped, the process's now
 * (KolibriOS drops the thread's reference and leaves the pages mapped;
 * here the memory lives while it is mapped). The last thread ends the
 * process: its maps, libraries, image and tables. */
void proc_thread_gone(struct task *t)
{
	struct proc *p = t->proc;
	for (int i = 0; i < p->nmap; i++)
		if (p->map[i].owner_pid == t->pid) p->map[i].owner_pid = 0;
	if (--p->threads > 0) return;
	futex_proc_gone(p);
	while (p->nmap) {
		struct umap *m = &p->map[p->nmap - 1];
		if (m->kind == UMAP_SHM) shm_close_map(p, m);
		else proc_map_free(p, m);
	}
	if (p->mem_pa) mm_free_mb(p->mem_pa, p->mem_size >> 20);
	if (p->l1) mmu_free_user_table(p->l1);
	p->mem_pa = 0;
	p->l1 = NULL;
	p->ndll = 0;
}

/* 51.1: a thread in the caller's process, from entry with the stack
 * pointer stack (the program's memory); its TID, -1 if no slot is free */
static u32 thread_create(struct task *t, u32 entry, u32 stack)
{
	struct proc *p = t->proc;
	if (!p) return -1;
	struct task *n = task_alloc();
	if (!n) return -1;
	/* new_sys_threads: the process's name; set_app_params: the folder,
	 * the default event mask, a window to come, a redraw first */
	memcpy(n->name, t->name, sizeof(n->name));
	n->proc = p;
	p->threads++;
	n->l1 = t->l1;
	n->mem_pa = t->mem_pa;
	n->mem_size = t->mem_size;
	strncpy0(n->cwd, t->cwd, sizeof(n->cwd));
	memset(&n->tf, 0, sizeof(n->tf));
	n->tf.pc = entry & ~1u;
	n->tf.usr_sp = stack;
	n->tf.cpsr = PSR_USR | PSR_F | (entry & 1 ? PSR_T : 0);
	n->events = 1u << (EV_REDRAW - 1);
	n->state = T_READY;
	return n->pid;
}

void sys_threads(struct task *t, u32 *r)
{
	switch (r[1]) {
	case 1:
		r[0] = thread_create(t, r[2], r[3]);
		return;
	case 2:
		r[0] = t->slot;
		return;
	case 3:
	case 4: {
		int slot = (s32)r[2] == -1 ? t->slot : (int)r[2];
		if (slot <= 0 || slot >= MAX_TASKS || tasks[slot].state == T_FREE) { r[0] = -1; return; }
		struct task *o = &tasks[slot];
		r[0] = o->priority;
		if (r[1] == 4) o->priority = r[3] & 0xff;
		return;
	}
	default:
		r[0] = -1;
		return;
	}
}

/* --- IPC (60) ---------------------------------------------------------- */

static u32 ipc_send(struct task *t, u32 pid, u32 msg_va, u32 len)
{
	struct task *d = task_by_pid(pid);
	if (!d || d->lx) return 4;                 /* no such PID */
	if (!d->ipc_start) return 1;               /* no buffer */
	u8 *buf = uptr(d, d->ipc_start, d->ipc_size);
	const u8 *msg = len ? uptr(t, msg_va, len) : (const u8 *)"";
	if (!buf || d->ipc_size < 8) return 1;
	if (!msg) return 3;
	if (get32(buf)) return 2;                  /* locked */
	u32 used = get32(buf + 4);
	if (used > d->ipc_size || 8 + len > d->ipc_size - used) return 3;   /* full */
	put32(buf + used, t->pid);
	put32(buf + used + 4, len);
	memcpy(buf + used + 8, msg, len);
	put32(buf + 4, used + 8 + len);
	task_post_event(d, EV_IPC);
	return 0;
}

void sys_ipc(struct task *t, u32 *r)
{
	switch (r[1]) {
	case 1:                                    /* the area for receiving */
		if (r[2] && !uptr(t, r[2], r[3])) { r[0] = -1; return; }
		t->ipc_start = r[2];
		t->ipc_size = r[2] ? r[3] : 0;
		r[0] = 0;
		return;
	case 2:
		r[0] = ipc_send(t, r[2], r[3], r[4]);
		return;
	default:
		r[0] = -1;
		return;
	}
}

/* --- futexes (77.0-77.3) ------------------------------------------------
 * A futex is a dword of the program's (its control value) by which its
 * threads wait for each other: 77.2 waits while the dword still holds the
 * value given (else -2 at once), until 77.3 wakes it or its timeout (in
 * hundredths of a second, 0: none) passes (-1); 77.3 wakes that many
 * waiters (their 77.2 answers 0) and answers how many. The check and the
 * wait are one system call (the kernel runs with interrupts off), so a
 * 77.3 that comes after the program changed the dword cannot be missed:
 * a thread that waits for something another thread does needs no polling
 * (console.obj's program thread waits so for a key, lib/console). The
 * handles are the process's; they go with its last thread. */
#define MAX_FUTEX 128
static struct futex {
	struct proc *p;                    /* its process, NULL: free */
	u32 va;                            /* the control value's address */
} futexes[MAX_FUTEX];

static struct futex *futex_of(struct task *t, u32 h)
{
	if (!h || h > MAX_FUTEX || !t->proc || futexes[h - 1].p != t->proc) return NULL;
	return &futexes[h - 1];
}

/* up to n of its waiters woken with the answer r; how many */
static u32 futex_wake(u32 h, u32 n, u32 r)
{
	u32 woken = 0;
	for (int i = 1; i < MAX_TASKS && woken < n; i++) {
		struct task *w = &tasks[i];
		if (w->state != T_FUTEX || w->futex != h) continue;
		w->tf.r[0] = r;
		w->futex = 0;
		w->state = T_READY;
		woken++;
	}
	return woken;
}

static void futex_proc_gone(struct proc *p)
{
	for (int i = 0; i < MAX_FUTEX; i++)
		if (futexes[i].p == p) futexes[i].p = NULL;
}

void sys_posix(struct task *t, u32 *r)
{
	switch (r[1]) {
	case 0: {                                  /* create: ECX the control value's address */
		r[0] = 0;
		if (!t->proc || (r[2] & 3) || !uptr(t, r[2], 4)) return;
		for (int i = 0; i < MAX_FUTEX; i++)
			if (!futexes[i].p) {
				futexes[i].p = t->proc;
				futexes[i].va = r[2];
				r[0] = i + 1;
				return;
			}
		return;
	}
	case 1: {                                  /* destroy: its waiters as by a timeout */
		struct futex *f = futex_of(t, r[2]);
		if (!f) { r[0] = -1; return; }
		futex_wake(r[2], MAX_TASKS, -1);
		f->p = NULL;
		r[0] = 0;
		return;
	}
	case 2: {                                  /* wait: EDX the value, ESI the timeout */
		struct futex *f = futex_of(t, r[2]);
		const u32 *v = f ? uptr(t, f->va, 4) : NULL;
		if (!v) { r[0] = -1; return; }
		if (get32(v) != r[3]) { r[0] = -2; return; }
		t->futex = r[2];
		t->futex_timed = r[4] != 0;
		t->wake_tick = ticks + r[4];
		t->state = T_FUTEX;
		return;
	}
	case 3: {                                  /* wake: EDX waiters at most */
		r[0] = futex_of(t, r[2]) ? futex_wake(r[2], r[3], 0) : 0;
		return;
	}
	case 4: case 5: case 6: case 7:            /* reserved */
		r[0] = -1;
		return;
	default:                                   /* the pipes (10, 11, 13) are not here */
		r[0] = -9;                         /* -EBADF */
		return;
	}
}
