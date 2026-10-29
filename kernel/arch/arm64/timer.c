/* KolibriOS for ARM, AArch64: time and tickless idle.
 *
 * The clock is the generic timer's virtual counter. Its virtual timer,
 * at the interrupt the device tree's arm,armv8-timer node names third
 * (the binding's order: secure, non-secure physical, virtual, hypervisor;
 * QEMU's virt: PPI 11, INTID 27; the SM6115: PPI 3, INTID 19), gives the
 * 100 Hz tick while tasks run, on a fixed grid of the counter. When
 * nothing is ready, the idle loop asks when something next needs the CPU
 * (sched_next_work) and moves the timer there instead: one interrupt
 * rather than 100 a second (tickless idle; kolibri.tick=periodic keeps
 * the 100 Hz tick). `ticks` follows the counter, read again at every
 * interrupt, so that it is right after a long idle.
 *
 * The memory-mapped timer (arm,armv7-timer-mem: CNTCTLBase and its
 * frames, Linux's drivers/clocksource/arm_arch_timer_mmio.c for the
 * registers) is the broadcast timer: the core's own timer stops in the
 * SM6115's power-down idle states (local-timer-stop), so before such a
 * state the frame's physical timer is set to the same deadline
 * (psci.c). */
#include "kernel.h"
#include "power.h"
#include "powerdt.h"
#include "net.h"

struct power_stats pstats;

static u32 vtimer_id = 27;              /* without a device tree entry: QEMU's */
static u64 freq, period, base;
static bool periodic_only;

/* the memory-mapped timer: CNTCTLBase, one frame's CNTBase, its
 * physical timer's interrupt */
#define CNTTIDR 0x08
#define CNTACR(n) (0x40 + 4 * (n))
#define CNTACR_ALL 0x3fu                /* RPCT RVCT RFRQ RVOFF RWVT RWPT */
#define CNTACR_RPCT (1u << 0)
#define CNTACR_RWPT (1u << 5)
#define CNTPCT_LO 0x00
#define CNTFRQ 0x10
#define CNTP_CVAL_LO 0x20
#define CNTP_CTL 0x2c
static addr_t ctl_pa, frame_pa, frame;
static u32 frame_num, frame_irq;
static bool bcast;

u64 timer_count(void)
{
	asm volatile("isb");
	return SYSREG_R(cntvct_el0);
}

u64 timer_freq(void)
{
	return freq;
}

u32 timer_intid(void)
{
	return vtimer_id;
}

bool timer_tickless(void)
{
	return !periodic_only;
}

void timer_probe(void)
{
	vtimer_id = pdt.vtimer_id;
	ctl_pa = pdt.ctl_pa;
	frame_pa = pdt.frame_pa;
	frame_num = pdt.frame_num;
	frame_irq = pdt.frame_irq;
	if (ctl_pa && frame_pa) {
		mmu_add_io(ctl_pa, 0x1000);
		mmu_add_io(frame_pa, 0x1000);
	}
}

/* the frame's physical timer, if this side may use it (CNTACR, as
 * Linux's find_best_frame sets it) */
static void bcast_init(void)
{
	if (!ctl_pa || !frame_pa || frame_num > 7) return;
	addr_t c = mmu_io_va(ctl_pa);
	frame = mmu_io_va(frame_pa);
	wr32(c + CNTACR(frame_num), CNTACR_ALL);
	u32 acr = rd32(c + CNTACR(frame_num));
	if ((acr & (CNTACR_RPCT | CNTACR_RWPT)) != (CNTACR_RPCT | CNTACR_RWPT)) {
		kprintf("timer: frame %u at %p not ours (CNTACR %x)\n", frame_num, frame_pa, acr);
		return;
	}
	wr32(frame + CNTP_CTL, 0);
	gic3_enable_spi(frame_irq);
	bcast = true;
	kprintf("timer: broadcast: frame %u at %p, INTID %u, %u Hz\n", frame_num, frame_pa, frame_irq,
		rd32(frame + CNTFRQ));
}

static u64 frame_count(void)
{
	u32 hi, lo;
	do {
		hi = rd32(frame + CNTPCT_LO + 4);
		lo = rd32(frame + CNTPCT_LO);
	} while (hi != rd32(frame + CNTPCT_LO + 4));
	return (u64)hi << 32 | lo;
}

bool timer_bcast_ok(void)
{
	return bcast;
}

/* the frame's timer at a virtual count: the frame counts the physical
 * count, which may be offset from the virtual one (CNTVOFF) */
void timer_bcast_set(u64 count)
{
	if (!bcast) return;
	wr32(frame + CNTP_CTL, 0);
	if (!count) return;
	u64 at = count + (frame_count() - timer_count());
	wr32(frame + CNTP_CVAL_LO, (u32)at);
	wr32(frame + CNTP_CVAL_LO + 4, (u32)(at >> 32));
	wr32(frame + CNTP_CTL, 1);              /* enabled, not masked */
	asm volatile("dsb sy");
}

/* the ticks from `ticks` to the first one at or after the count c */
u32 timer_ticks_until(u64 c)
{
	if (!period) return 0;
	u64 t = base + (u64)ticks * period;
	if (c <= t) return 0;
	u64 n = (c - t + period - 1) / period;
	return n < 0xfffffffeu ? (u32)n : 0xfffffffeu;
}

void timer_sync(void)
{
	if (period) ticks = (u32)((timer_count() - base) / period);
}

/* the tick's next point on the grid after the count c */
static u64 next_grid(u64 c)
{
	return base + ((c - base) / period + 1) * period;
}

void timer_start(void)
{
	freq = SYSREG_R(cntfrq_el0);
	period = freq / 100;
	base = timer_count() - (u64)ticks * period;
	const char *v = boot_arg("kolibri.tick");
	periodic_only = v && !strncmp(v, "periodic", 8);
	SYSREG_W(cntv_cval_el0, next_grid(timer_count()));
	SYSREG_W(cntv_ctl_el0, 1);
	asm volatile("isb");
	kprintf("timer: %u Hz, INTID %u, %s\n", (u32)freq, vtimer_id,
		periodic_only ? "100 Hz always (kolibri.tick=periodic)" : "tickless idle");
	bcast_init();
	psci_init();
	power_init();
}

/* the virtual timer, or the broadcast frame's */
bool timer_irq(u32 id)
{
	if (id == frame_irq && bcast) {
		wr32(frame + CNTP_CTL, 0);
	} else if (id != vtimer_id) {
		return false;
	}
	u64 now = timer_count();
	ticks = (u32)((now - base) / period);
	SYSREG_W(cntv_cval_el0, next_grid(now));
	asm volatile("isb");
	pstats.irq_timer++;
	timer_tick();
	return true;
}

/* the longest idle without a look at the clock: 10 minutes */
#define MAX_IDLE_TICKS 60000u

/* nothing to run (start.S's idle loop, interrupts masked): until the
 * next work or an interrupt. The wake-up's interrupt is taken when the
 * loop opens them again. */
void arch_idle(void)
{
	timer_sync();
	u64 now = timer_count();
	u64 dl = next_grid(now);
	int why = WHY_TICK, slot = 0;
	if (!periodic_only) {
		u32 n = sched_next_work();
		if (n > 1) {
			dl = base + ((u64)ticks + MIN(n, MAX_IDLE_TICKS)) * period;
			SYSREG_W(cntv_cval_el0, dl);
			asm volatile("isb");
			why = next_why;
			slot = next_why_slot;
		}
	}
	pstats.wakeups++;
	if (!psci_idle(dl > now ? dl - now : 0)) {
		asm volatile("dsb sy\nwfi" ::: "memory");
		pstats.st_wfi++;
	}
	u64 after = timer_count();
	pstats.idle_count += after - now;
	/* the deadline woke it: whose */
	if (after >= dl) {
		pstats.why[why]++;
		if (why == WHY_TASK && slot < (int)ARRAY_SIZE(pstats.why_task)) pstats.why_task[slot]++;
	}
	/* woken before the deadline: the tick again, for what the interrupt
	 * makes ready (a deadline that passed fires when unmasked) */
	if (after < dl) {
		SYSREG_W(cntv_cval_el0, next_grid(after));
		asm volatile("isb");
	}
}
