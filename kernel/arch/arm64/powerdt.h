/* KolibriOS for ARM, AArch64: what the device tree says about time and
 * power (powerdt.c), read once while the MMU is off. Pure reading, so
 * that tests/power runs it on the PC against the phone's own tree. */
#ifndef POWERDT_H
#define POWERDT_H

#ifdef POWER_HOST
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
#include <stdbool.h>
#else
#include "kernel.h"
#endif

/* an idle state (arm,idle-state, domain-idle-state) */
struct idle_state {
	u32 param;                      /* arm,psci-suspend-param */
	u32 entry_us, exit_us, min_us;  /* the latencies, the minimum residency */
	bool timer_stop;                /* local-timer-stop */
	bool cluster;                   /* the cluster's (the parent power domain) */
	char name[32];
};

#define PDT_CPUS 8
#define PDT_STATES 8

struct power_dt {
	/* the generic timer: the virtual timer's interrupt; the
	 * memory-mapped timer (arm,armv7-timer-mem): CNTCTLBase, the first
	 * enabled frame's CNTBase, its number, its physical timer's interrupt */
	u32 vtimer_id;
	u64 ctl_pa, frame_pa;
	u32 frame_num, frame_irq;
	/* PSCI: there, its conduit (hvc, else smc), the CPUs' MPIDRs, which
	 * of them we are (-1: not found) */
	bool psci, hvc;
	int ncpus, self;
	u64 mpidr[PDT_CPUS];
	/* this core's idle states, then its cluster's, each by residency */
	int nstates;
	struct idle_state st[PDT_STATES];
	bool hierarchical;              /* from power domains (OSI), not cpu-idle-states */
};

/* my_mpidr: MPIDR_EL1's affinity (Aff3 in bits 32-39, Aff2-0) */
void power_dt_parse(const void *fdt, u64 my_mpidr, struct power_dt *d);
/* the deepest state for an idle of us microseconds, as Linux's
 * hierarchical PSCI idle chooses: the core's state if its residency
 * fits, then the cluster's if allowed (cluster_ok: OSI mode, the other
 * cores off); states that stop the core's timer only with the broadcast
 * timer (bcast_ok). -1: none (wfi) */
int power_dt_choose(const struct power_dt *d, u32 us, bool cluster_ok, bool bcast_ok);
/* does the state lose the core's context (a power-down state)? The PSCI
 * parameter's type bit: 30 in the extended format, else 16 */
bool power_dt_powerdown(u32 param, bool extended);

#ifndef POWER_HOST
extern struct power_dt pdt;             /* the kernel's (psci.c) */
#endif

#endif
