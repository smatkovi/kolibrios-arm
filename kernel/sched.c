/* KolibriOS for ARM: tasks, their events and the scheduler.
 *
 * A task is a KolibriOS thread: its slot, PID, window and events; the
 * threads of one program (function 51) share its process, the address
 * space (kernel/proc.c), which ends with its last thread. The kernel
 * keeps no stack per task: a task's state is its saved trap frame, and a task
 * that waits (functions 5, 10, 23) gets its return value written into
 * that frame when it wakes. Round robin, preempted by the 100 Hz tick;
 * when nothing is ready, sched_next_work says when the next timeout is
 * (tickless idle, AArch64). */
#include "gui.h"
#include "net.h"
#include "power.h"
#ifdef __aarch64__
#include "../linux/lx.h"
#endif

struct task tasks[MAX_TASKS];
struct task *current;
/* the highest slot a task has had: the loops over the tasks on every
 * trap (wake_waiters, schedule, sched_next_work) stop there */
static int task_hi;
volatile u32 ticks;
static u32 next_pid = 1;
static bool need_resched;
static int slice;

#define SLICE_TICKS 2

void sched_init(void)
{
	for (int i = 0; i < MAX_TASKS; i++)
		tasks[i].slot = i;
}

struct task *task_alloc(void)
{
	for (int i = 1; i < MAX_TASKS; i++) {
		struct task *t = &tasks[i];
		if (t->state != T_FREE) continue;
		memset(t, 0, sizeof(*t));
		t->slot = i;
		t->pid = next_pid++;
		if (i > task_hi) task_hi = i;
		t->event_mask = 7;   /* redraw, key, button */
		return t;
	}
	return NULL;
}

void task_exit(struct task *t)
{
	if (t->state == T_FREE) return;
#ifdef __aarch64__
	if (t->lx) lx_thread_gone(t);
#endif
	extern void fp_task_gone(struct task *t);     /* arch/<arch>/fpu.c */
	fp_task_gone(t);
	wm_remove(t);
	if (t->proc) proc_thread_gone(t);  /* the last thread: the address space goes */
	t->state = T_FREE;
	t->l1 = NULL;
	t->mem_pa = 0;
	t->mem_size = 0;
	t->proc = NULL;
	if (t == current) need_resched = true;
}

static u32 pending(struct task *t)
{
	u32 e = t->events;
	if (t->key_head != t->key_tail && wm_active() == t) e |= 1 << (EV_KEY - 1);
	if (t->btn_head != t->btn_tail) e |= 1 << (EV_BUTTON - 1);
	return e;
}

int task_take_event(struct task *t)
{
	u32 e = pending(t) & t->event_mask & 0x7ff;
	if (!e) return 0;
	int ev = __builtin_ctz(e) + 1;
	/* key and button stay while their buffers hold something */
	if (ev != EV_KEY && ev != EV_BUTTON)
		t->events &= ~(1u << (ev - 1));
	return ev;
}

void task_post_event(struct task *t, int ev)
{
	if (t->state == T_FREE) return;
	/* key and button come from their buffers (pending()) */
	if (ev == EV_KEY || ev == EV_BUTTON) return;
	t->events |= 1u << (ev - 1);
}

/* EV_SYSTEM: something a shell shows changed (wm.c, the standby, the
 * modem); only to the tasks with its bit in their mask, so that a
 * shell can wait for it instead of looking every 250 ms */
void notify_system(void)
{
	for (int i = 1; i <= task_hi; i++)
		if (tasks[i].state != T_FREE && (tasks[i].event_mask & 1u << (EV_SYSTEM - 1)))
			task_post_event(&tasks[i], EV_SYSTEM);
}

void wake_waiters(void)
{
	for (int i = 1; i <= task_hi; i++) {
		struct task *t = &tasks[i];
		switch (t->state) {
		case T_WAIT_EVENT:
		case T_WAIT_TIMEOUT: {
			int ev = task_take_event(t);
			if (ev) {
				t->tf.r[0] = ev;
				t->state = T_READY;
			} else if (t->state == T_WAIT_TIMEOUT && (s32)(ticks - t->wake_tick) >= 0) {
				t->tf.r[0] = 0;
				t->state = T_READY;
			}
			break;
		}
		case T_SLEEP:
			if ((s32)(ticks - t->wake_tick) >= 0) t->state = T_READY;
			break;
#ifdef __aarch64__
		case T_LX_WAIT:
			lx_retry(t);
			break;
#endif
		default:
			break;
		}
	}
}

static void switch_to(struct task *t)
{
	if (t == current) return;
	/* threads of one process: the same tables (ARMv7 flushes its TLB
	 * at every switch of them) */
	bool same = t && current && t->l1 && current->l1 == t->l1;
	current = t;
	if (!same) mmu_switch(t ? t->l1 : NULL);
}

void schedule(void)
{
	wake_waiters();
	if (current && current->state == T_READY && !need_resched) return;
	need_resched = false;
	/* round robin from the one after the current; slots above task_hi
	 * are free */
	int s = current ? current->slot : 0;
	for (int i = 0; i < task_hi; i++) {
		s = s >= task_hi ? 1 : s + 1;
		if (tasks[s].state == T_READY) {
			switch_to(&tasks[s]);
			return;
		}
	}
	switch_to(NULL);
}

/* function 68.1: the rest of the time slice to the others */
void sched_yield(void)
{
	need_resched = true;
}

void timer_tick(void)
{
	ticks++;
	if (plat->tick) plat->tick();
	kmodem_tick();
#ifdef __aarch64__
	net_tick();
#endif
	if (++slice >= SLICE_TICKS) {
		slice = 0;
		need_resched = true;
	}
	gfx_flush();
#ifdef __aarch64__
	power_tick();
#endif
}

/* ticks from now until t's timeout */
static u32 until(u32 tick)
{
	s32 d = (s32)(tick - ticks);
	return d > 0 ? (u32)d : 0;
}

int next_why, next_why_slot;

static u32 n_min(u32 n, u32 v, int why)
{
	if (v >= n) return n;
	next_why = why;
	return v;
}

u32 sched_next_work(void)
{
	u32 n = NEXT_NONE;
	next_why = WHY_NONE;
	for (int i = 1; i <= task_hi; i++) {
		struct task *t = &tasks[i];
		u32 v = NEXT_NONE;
		switch (t->state) {
		case T_READY:
			v = 0;
			break;
		case T_SLEEP:
		case T_WAIT_TIMEOUT:
			v = until(t->wake_tick);
#ifdef __aarch64__
			/* the display off: the programs' timeouts (a shell's
			 * look at its windows, a clock) together, on whole
			 * seconds, at most a second late */
			if (power_standby && v) v = (ticks + v + 99) / 100 * 100 - ticks;
#endif
			break;
#ifdef __aarch64__
		case T_LX_WAIT:
			/* a Linux thread's deadline, a count of the generic timer */
			if (t->lx && t->lx->deadline) v = timer_ticks_until(t->lx->deadline);
			break;
#endif
		default:
			break;
		}
		if (v < n) {
			n = v;
			next_why = WHY_TASK;
			next_why_slot = i;
		}
	}
	if (plat->tick) n = n_min(n, plat->next_tick ? plat->next_tick() : 0, WHY_PLAT);
	n = n_min(n, kmodem_next(), WHY_MODEM);
	if (scr.dirty_y0 < scr.dirty_y1) n = n_min(n, 1, WHY_SCREEN);
#ifdef __aarch64__
	extern u32 net_next(void);
	n = n_min(n, net_next(), WHY_NET);
	n = n_min(n, power_next(), WHY_POWER);
#endif
	return n;
}

/* the image, or one of the process's maps (shared memory, libraries):
 * the kernel's address of va and how many bytes from there are its. A
 * read-only area (68.22 with SHM_READ) is no buffer for uptr, whose
 * callers may write (as KolibriOS's kernel faults on such pages); its
 * strings (ustr) are fine */
static void *ubase(struct task *t, u32 va, u32 *left, bool write)
{
	if (va < t->mem_size) {
		*left = t->mem_size - va;
		return (void *)(t->mem_pa + va);
	}
	struct proc *p = t->proc;
	if (!p) return NULL;
	for (int i = 0; i < p->nmap; i++) {
		struct umap *m = &p->map[i];
		if (va - m->va < m->size) {
			if (write && m->ro) return NULL;
			*left = m->size - (va - m->va);
			return (void *)(m->pa + (va - m->va));
		}
	}
	return NULL;
}

void *uptr(struct task *t, u32 va, u32 len)
{
	if (va < t->mem_size && len <= t->mem_size - va) return (void *)(t->mem_pa + va);
	u32 left;
	void *k = ubase(t, va, &left, true);
	return k && len <= left ? k : NULL;
}

const char *ustr(struct task *t, u32 va)
{
	u32 left;
	const char *s = ubase(t, va, &left, false);
	if (!s) return NULL;
	for (u32 i = 0; i < left && i < 4096; i++)
		if (!s[i]) return s;
	return NULL;
}

static const char *trap_name(int kind)
{
	switch (kind) {
	case TRAP_UNDEF: return "undefined instruction";
	case TRAP_PABT: return "prefetch abort";
	case TRAP_DABT: return "data abort";
	default: return "exception";
	}
}

struct task *trap_dispatch(struct trapframe *tf, int kind, bool from_user, addr_t far, u32 fsr)
{
	struct task *t = current;
	if (from_user) {
		if (!t) panic("user trap without a task");
		if (tf != &t->tf) t->tf = *tf;  /* AArch64: saved there already */
	} else if (kind == TRAP_SVC) {
		/* kmain's start: the first switch to a task */
	} else if (kind != TRAP_IRQ && kind != TRAP_FIQ) {
		panic("%s in the kernel at %p (address %p, status %x)", trap_name(kind), (addr_t)tf->pc, far, fsr);
	}
	switch (kind) {
	case TRAP_IRQ:
	case TRAP_FIQ:
		plat->irq();
		break;
	case TRAP_SVC:
		if (from_user) syscall(t);
		break;
	default:
		kprintf("%s (pid %u): %s at pc %p, address %p: ended\n",
			t->name, t->pid, trap_name(kind), (addr_t)tf->pc, far);
		task_exit(t);
		break;
	}
	schedule();
	return current;
}
