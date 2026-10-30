/* sysbench: the kernel's ways in and out, timed from a program (the
 * AArch64 kernel's generic timer, readable at EL0: CNTVCT through
 * CP15): a system call that does almost nothing (function 26.9, the
 * tick count) many times; then the switch from task to task: a second
 * copy of itself (started with the parameter "peer") and this one give
 * the processor to each other (function 68.1) many times. The results
 * go to the debug board (function 63.1: the kernel's console); started
 * by kolibri.run=/sys/sysbench (docs/asm.md). In QEMU the instruction
 * counts of tools/qemu-prof say more than these times: its marks
 * (kbase=) around each part (tools/qemu-prof/sysbench.sh). */
#include "kos.h"

#define CALLS 100000
#define YIELDS 20000

static inline unsigned long long cntvct(void)
{
	u32 lo, hi;
	asm volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return (unsigned long long)hi << 32 | lo;
}

static inline u32 cntfrq(void)
{
	u32 f;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
	return f;
}

/* a mark for tools/qemu-prof (kbase=): its instruction counts so far */
static inline void mark(void)
{
	asm volatile("orr r7, r7, r7" ::: "memory");
}

static void yields(int n)
{
	for (int i = 0; i < n; i++) kos_call(68, 1, 0, 0, 0, 0);
}

/* nanoseconds of n counts, each */
static u32 ns_each(unsigned long long d, u32 freq, u32 n)
{
	return (u32)(d * 1000000 / (freq / 1000) / n);
}

int main(const char *params)
{
	if (params && !strcmp(params, "peer")) {
		yields(YIELDS + 1000);
		return 0;
	}
	u32 freq = cntfrq();
	kdebug("sysbench: %u Hz\n", freq);
	mark();
	unsigned long long t0 = cntvct();
	for (int i = 0; i < CALLS; i++) kos_call(26, 9, 0, 0, 0, 0);
	unsigned long long t1 = cntvct();
	mark();
	kdebug("sysbench: %u x function 26.9: %u ns each\n", CALLS, ns_each(t1 - t0, freq, CALLS));
	/* alone: yield returns at once (the scheduler keeps this task) */
	mark();
	t0 = cntvct();
	yields(YIELDS);
	t1 = cntvct();
	mark();
	kdebug("sysbench: %u x function 68.1 alone: %u ns each\n", YIELDS, ns_each(t1 - t0, freq, YIELDS));
	/* with the peer: each yield a switch there and back */
	kos_run("/sys/sysbench", "peer");
	yields(100);
	mark();
	t0 = cntvct();
	yields(YIELDS);
	t1 = cntvct();
	mark();
	kdebug("sysbench: %u x function 68.1 with a peer: %u ns each (a switch there and back)\n", YIELDS,
	       ns_each(t1 - t0, freq, YIELDS));
	kdebug("sysbench: done\n");
	return 0;
}
