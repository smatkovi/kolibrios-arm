/* KolibriOS for ARM: tasks, their events and the scheduler.
 *
 * A task is a KolibriOS thread with its own address space (threads of
 * one program, function 51, are not there yet). The kernel keeps no
 * stack per task: a task's state is its saved trap frame, and a task
 * that waits (functions 5, 10, 23) gets its return value written into
 * that frame when it wakes. Round robin, preempted by the 100 Hz tick. */
#include "gui.h"

struct task tasks[MAX_TASKS];
struct task *current;
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
		t->event_mask = 7;   /* redraw, key, button */
		return t;
	}
	return NULL;
}

void task_exit(struct task *t)
{
	if (t->state == T_FREE) return;
	wm_remove(t);
	if (t->mem_pa) mm_free_mb(t->mem_pa, t->mem_size >> 20);
	if (t->l1) mmu_free_user_table(t->l1);
	t->state = T_FREE;
	t->l1 = NULL;
	t->mem_pa = 0;
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
	u32 e = pending(t) & t->event_mask & 0x3ff;
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

void wake_waiters(void)
{
	for (int i = 1; i < MAX_TASKS; i++) {
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
		default:
			break;
		}
	}
}

static void switch_to(struct task *t)
{
	if (t == current) return;
	current = t;
	mmu_switch(t ? t->l1 : NULL);
}

void schedule(void)
{
	wake_waiters();
	if (current && current->state == T_READY && !need_resched) return;
	need_resched = false;
	int start = current ? current->slot : 0;
	for (int i = 1; i <= MAX_TASKS; i++) {
		int s = (start + i) % MAX_TASKS;
		if (s && tasks[s].state == T_READY) {
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
	if (++slice >= SLICE_TICKS) {
		slice = 0;
		need_resched = true;
	}
	gfx_flush();
}

void *uptr(struct task *t, u32 va, u32 len)
{
	if (va >= t->mem_size || len > t->mem_size - va) return NULL;
	return (void *)(t->mem_pa + va);
}

const char *ustr(struct task *t, u32 va)
{
	if (va >= t->mem_size) return NULL;
	const char *s = (const char *)(t->mem_pa + va);
	for (u32 i = 0; va + i < t->mem_size && i < 4096; i++)
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
		t->tf = *tf;
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
