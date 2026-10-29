/* KolibriOS for ARM, AArch64: kolibri.powertest=N, a measurement of how
 * the kernel idles, for N minutes (10 without a number): every minute a
 * line on the console with what happened in it,
 *
 *   power: 60 s: 61 wake-ups (1.0/s), timer 60, other 1, idle 99.9 %,
 *          wfi 61, core 0, cluster 0, refused 0, resumed 0
 *
 * the wake-ups (the idle loop's), the interrupts by kind, the share of
 * the time in idle, the idle states entered (psci.c) and how often the
 * core came back through cpu_resume (a real power-down); the platform adds
 * its own (the battery, the SoC's sleep counts: plat_power_report). On
 * the Pro1X the watchdog then resets the phone (kolibri.lifetime), and
 * tools/pro1x-read.py shows the console after the next boot. */
#include "power.h"
#include "net.h"
#include "task.h"

/* a line a minute; kolibri.powerline=S: every S seconds (QEMU tests) */
static u32 report_ticks = 6000;
static u32 minutes, next_report, end_tick;
static struct power_stats last;
static u64 last_count, start_count;
static struct power_stats first;

/* the platform's part of a report line, and of the start and the end */
void (*plat_power_report)(int when);

u32 power_test_minutes(void)
{
	return minutes;
}

static u32 num(const char *s)
{
	u32 v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
	return v;
}

/* x per s, tenths, over elapsed counter ticks */
static u32 per10(u32 x, u64 elapsed)
{
	return elapsed ? (u32)((u64)x * 10 * timer_freq() / elapsed) : 0;
}

static void line(const char *what, const struct power_stats *a, const struct power_stats *b, u64 elapsed)
{
	u32 w = b->wakeups - a->wakeups, r = per10(w, elapsed);
	u64 idle = b->idle_count - a->idle_count;
	u32 permille = elapsed ? (u32)(idle * 1000 / elapsed) : 0;
	kprintf("power: %s%s%u s: %u wake-ups (%u.%u/s), timer %u, other %u, idle %u.%u %%, "
		"wfi %u, core %u, cluster %u, refused %u, resumed %u\n",
		what, *what ? " " : "", (u32)((elapsed + timer_freq() / 2) / timer_freq()), w, r / 10, r % 10, b->irq_timer - a->irq_timer,
		b->irq_other - a->irq_other, permille / 10, permille % 10, b->st_wfi - a->st_wfi,
		b->st_core - a->st_core, b->st_cluster - a->st_cluster, b->st_fail - a->st_fail,
		b->resumed - a->resumed);
	/* the deadlines that woke it, by who (the next tick: the 100 Hz
	 * tick while something is about to happen) */
	static const char *const name[WHY_N] = { "tick", "tasks", "platform", "modem", "screen",
						 "network", "powertest", "none" };
	kprintf("power:   by deadline:");
	for (int i = 0; i < WHY_N; i++)
		if (b->why[i] != a->why[i]) kprintf(" %s %u", name[i], b->why[i] - a->why[i]);
	for (int i = 1; i < (int)ARRAY_SIZE(b->why_task) && i < MAX_TASKS; i++) {
		u32 d = (u16)(b->why_task[i] - a->why_task[i]);
		if (d) kprintf(", %s %u", tasks[i].state != T_FREE ? tasks[i].name : "(ended)", d);
	}
	kprintf("\n");
}

void power_init(void)
{
	const char *v = boot_arg("kolibri.powertest");
	if (!v) return;
	minutes = num(v);
	if (!minutes) minutes = 10;
	const char *l = boot_arg("kolibri.powerline");
	if (l && num(l)) report_ticks = num(l) * 100;
	next_report = ticks + report_ticks;
	end_tick = ticks + minutes * 6000;
	last = first = pstats;
	last_count = start_count = timer_count();
	kprintf("power: test for %u min, a line every %u s (%s)\n", minutes, report_ticks / 100,
		timer_tickless() ? "tickless idle" : "100 Hz tick");
	if (plat_power_report) plat_power_report(0);
}

void power_tick(void)
{
	if (!minutes || (s32)(ticks - next_report) < 0) return;
	u64 now = timer_count();
	extern u32 resumed;                     /* suspend.S */
	pstats.resumed = resumed;
	struct power_stats s = pstats;
	line("", &last, &s, now - last_count);
	if (plat_power_report) plat_power_report(1);
	last = s;
	last_count = now;
	next_report += report_ticks;
	if ((s32)(ticks - end_tick) >= 0) {
		line("test over, all", &first, &s, now - start_count);
		if (plat_power_report) plat_power_report(2);
		minutes = 0;
	}
}

u32 power_next(void)
{
	if (!minutes) return NEXT_NONE;
	s32 d = (s32)(next_report - ticks);
	return d > 0 ? (u32)d : 0;
}
