/* KolibriOS for ARM, AArch64: time and power: the tick, tickless idle,
 * the CPU's idle states (PSCI), the measurements of kolibri.powertest.
 *
 * Every part that needs the CPU at a time of its own (a task's timeout,
 * a poll, lwIP's timers) says how many ticks from now that is; the idle
 * loop sleeps until the earliest of them instead of waking 100 times a
 * second (kernel/arch/arm64/timer.c). */
#ifndef POWER_H
#define POWER_H

#include "kernel.h"

#define NEXT_NONE 0xffffffffu          /* nothing but an interrupt */

/* ticks from now until something needs the CPU (sched.c): the tasks'
 * timeouts, the platform, the modem, the network, the screen; 0 or 1:
 * the next tick */
u32 sched_next_work(void);
/* who set the last answer: a task (its slot), or a part of the kernel */
enum { WHY_TICK, WHY_TASK, WHY_PLAT, WHY_MODEM, WHY_SCREEN, WHY_NET, WHY_POWER, WHY_NONE, WHY_N };
extern int next_why, next_why_slot;
static inline u32 next_min(u32 a, u32 b) { return a < b ? a : b; }
/* milliseconds from now as ticks, rounded up */
static inline u32 next_of_ms(u32 ms) { return ms == NEXT_NONE ? NEXT_NONE : (ms + 9) / 10; }

/* kernel/arch/arm64/timer.c */
void timer_probe(void);                /* powerdt.c's findings, MMU off */
u32 timer_intid(void);                 /* the virtual timer's interrupt */
void timer_start(void);                /* from gic3_init: the tick on */
bool timer_irq(u32 id);                /* an interrupt of the timers: handled */
void timer_sync(void);                 /* ticks from the counter */
u64 timer_count(void);                 /* the virtual counter */
u64 timer_freq(void);
u32 timer_ticks_until(u64 count);      /* ticks to the first tick at or after it */
bool timer_tickless(void);
/* the broadcast timer (the memory-mapped frame): present, and set to
 * fire at a virtual count (0: off) */
bool timer_bcast_ok(void);
void timer_bcast_set(u64 count);

/* the measurements (kolibri.powertest) */
struct power_stats {
	u64 idle_count;                    /* counter ticks in idle */
	u32 wakeups;                       /* idle entries */
	u32 irq_timer, irq_other;
	u32 st_wfi, st_core, st_cluster;   /* the idle states entered */
	u32 st_fail;                       /* CPU_SUSPEND refused */
	u32 resumed;                       /* back through cpu_resume (suspend.S) */
	u32 why[WHY_N];                    /* the deadlines that woke it, by who */
	u16 why_task[64];                  /* ... by the task's slot */
};
extern struct power_stats pstats;

/* kernel/power.c */
void power_init(void);                 /* the command line */
void power_tick(void);                 /* from the tick */
u32 power_next(void);
u32 power_test_minutes(void);          /* kolibri.powertest=N, 0 */
extern bool power_standby;             /* the display off: timer slack */

/* kernel/arch/arm64/psci.c: the CPU's idle states */
void psci_probe(void);                 /* powerdt.c's findings, MMU off */
void psci_init(void);                  /* MMU on: the firmware asked, the mode set */
/* the deepest state worth entering for an idle of that many counter
 * ticks, entered; false: none (wfi) */
bool psci_idle(u64 idle_ticks);
const char *psci_summary(void);

#endif
