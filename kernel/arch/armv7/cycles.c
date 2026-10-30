/* KolibriOS for ARM, ARMv7: a clock for kolibri.bench (bench.c), which
 * the Cortex-A8 has no generic timer for: its cycle counter (PMCCNTR,
 * the CPU's clock), its rate measured once against a counter of the
 * platform's own (QEMU's realview: the 24 MHz counter of the system
 * registers; the N950: the 32 kHz sync timer) over 50 ms. Should the
 * cycle counter not run, that counter itself. Called with the
 * interrupts masked: nothing else reads them. */
#include "kernel.h"
#include "power.h"

#ifdef PLATFORM_N950
#define COUNTER_PA 0x48320010u          /* 32KSYNCNT_CR */
#define COUNTER_HZ 32768u
#define CM_ICLKEN_WKUP 0x48004c10u      /* bit 2: EN_32KSYNC */
#else
#define COUNTER_PA 0x1000005cu          /* SYS_24MHZ */
#define COUNTER_HZ 24000000u
#endif

static u64 freq, hi;
static u32 last;
static bool cycles;                     /* the cycle counter runs */

static u32 counter(void)
{
	return rd32(mmu_io_va(COUNTER_PA));
}

static u32 pmccntr(void)
{
	u32 v;
	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
	return v;
}

static u32 now32(void)
{
	return cycles ? pmccntr() : counter();
}

/* 64 bits from the 32 of the counter: read often enough (a bench's run
 * is well under the 4 s of a wrap at 1 GHz) */
u64 timer_count(void)
{
	if (!freq) timer_freq();
	u32 c = now32();
	if (c < last) hi += 1ull << 32;
	last = c;
	return hi | c;
}

u64 timer_freq(void)
{
	if (freq) return freq;
#ifdef PLATFORM_N950
	wr32(CM_ICLKEN_WKUP, rd32(CM_ICLKEN_WKUP) | 1u << 2);
#endif
	/* PMCR: E (on), C (the cycle counter reset); PMCNTENSET: C */
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(1u | 1u << 2));
	asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31));
	asm volatile("isb");
	u32 c0 = counter(), p0 = pmccntr(), c1, p1;
	do {
		c1 = counter();
		p1 = pmccntr();
	} while (c1 - c0 < COUNTER_HZ / 20);
	u32 dp = p1 - p0;
	cycles = dp > 1000;
	freq = cycles ? (u64)dp * COUNTER_HZ / (c1 - c0) : COUNTER_HZ;
	last = now32();
	hi = 0;
	return freq;
}
