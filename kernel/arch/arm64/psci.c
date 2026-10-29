/* KolibriOS for ARM, AArch64: the CPU's idle states through PSCI.
 *
 * The device tree (powerdt.c) gives the conduit and this core's states:
 * on the SM6115 (sm6115.dtsi) the core's rail power collapse
 * (0x40000003, at least 1182 us, its timer stops) below the cluster's
 * power domain with GDHS (0x40000022, 782 us) and its power collapse
 * (0x41000044, 7376 us), all power-down states. As Linux's
 * cpuidle-psci-domain.c: with the firmware in OS-initiated mode (OSI,
 * PSCI_FEATURES(CPU_SUSPEND) bit 0), the cluster's states once the core
 * goes into its own deepest; the other cores are off (Linux's kexec
 * took them down with CPU_OFF; AFFINITY_INFO says so), so this core is
 * the cluster's last. Without OSI the core's states only.
 *
 * A power-down state loses the core's context (suspend.S keeps it) and
 * its timer (local-timer-stop): the memory-mapped frame wakes it then
 * (timer.c's broadcast), and without that frame such states are left
 * out. kolibri.psci=off: wfi only; kolibri.psci=core: no cluster state;
 * kolibri.psci=cluster: the cluster's states also without OSI (the
 * firmware coordinates; QEMU has no OSI); kolibri.psci=simulate (QEMU):
 * the power-down's loss played after QEMU's CPU_SUSPEND (a wfi), so that
 * the way back is taken (with simulate,cluster both). */
#include "kernel.h"
#include "power.h"
#include "powerdt.h"
#include "net.h"

struct power_dt pdt;

#define PSCI_VERSION 0x84000000u
#define PSCI_CPU_SUSPEND 0xc4000001u
#define PSCI_AFFINITY_INFO 0xc4000004u
#define PSCI_FEATURES 0x8400000au
#define PSCI_SET_SUSPEND_MODE 0x8400000fu
#define PSCI_DENIED (-3)

extern long psci_suspend(u64 fid, u64 power_state, u64 hvc, u64 simulate);
extern u32 resumed;


static bool usable, osi, extended, simulate, cluster_ok;
static u32 version;
static int others_on, cluster_denied;
static char summary[96];

static long psci_call(u64 fid, u64 a1, u64 a2, u64 a3)
{
	register u64 x0 asm("x0") = fid, x1 asm("x1") = a1, x2 asm("x2") = a2, x3 asm("x3") = a3;
	if (pdt.hvc)
		asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
	else
		asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
	return (long)x0;
}

void psci_probe(void)
{
	/* the device tree is read (powerdt.c); the firmware is asked once
	 * the MMU is on (psci_init) */
}

static const char *mode_name(void)
{
	return osi ? "OS-initiated" : "platform-coordinated";
}

void psci_init(void)
{
	if (!pdt.psci) {
		kprintf("psci: none in the device tree: wfi\n");
		return;
	}
	version = (u32)psci_call(PSCI_VERSION, 0, 0, 0);
	u32 major = version >> 16, minor = version & 0xffff;
	long feat = major >= 1 ? psci_call(PSCI_FEATURES, PSCI_CPU_SUSPEND, 0, 0) : -1;
	if (feat >= 0) extended = feat & 2;
	/* the other CPUs: off, or still on (then no cluster state) */
	u32 off = 0;
	for (int i = 0; i < pdt.ncpus; i++) {
		if (i == pdt.self) continue;
		long r = psci_call(PSCI_AFFINITY_INFO, pdt.mpidr[i], 0, 0);
		if (r == 1) off |= 1u << i;
		else others_on++;
	}
	const char *arg = boot_arg("kolibri.psci");
	simulate = arg && !strncmp(arg, "simulate", 8);
	if (simulate && !strncmp(arg + 8, ",cluster", 8)) arg += 9;
	bool core_only = arg && !strncmp(arg, "core", 4);
	bool pc_cluster = arg && !strncmp(arg, "cluster", 7);
	/* OSI as Linux sets it, for the cluster's states */
	bool want_cluster = false;
	for (int i = 0; i < pdt.nstates; i++) want_cluster |= pdt.st[i].cluster;
	want_cluster &= pdt.hierarchical && !core_only;
	if (feat >= 0 && (feat & 1) && want_cluster) {
		long r = psci_call(PSCI_SET_SUSPEND_MODE, 1, 0, 0);
		osi = r == 0;
		if (r) kprintf("psci: OS-initiated mode refused (%d)\n", (int)r);
	} else if (feat >= 0 && (feat & 1)) {
		psci_call(PSCI_SET_SUSPEND_MODE, 0, 0, 0);
	}
	cluster_ok = (osi || (pc_cluster && want_cluster)) && !others_on;
	usable = pdt.nstates > 0 && !(arg && !strncmp(arg, "off", 3));
	kprintf("psci: %u.%u over %s, %s mode, %s state ids; CPUs off: %x of %u%s\n", major, minor,
		pdt.hvc ? "hvc" : "smc", mode_name(), extended ? "extended" : "original", off, pdt.ncpus,
		others_on ? " (others on: no cluster state)" : "");
	for (int i = 0; i < pdt.nstates; i++) {
		const struct idle_state *s = &pdt.st[i];
		bool tstop = s->timer_stop;
		kprintf("psci:   %s %s: %x, %u us at least, exit %u us%s%s\n", s->cluster ? "cluster" : "core",
			s->name, s->param, s->min_us, s->exit_us, tstop ? ", the timer stops" : "",
			(tstop && !timer_bcast_ok()) || (s->cluster && !cluster_ok) ? " (not used)" : "");
	}
	if (!usable) {
		kprintf("psci: %s\n", pdt.nstates ? "off (kolibri.psci=off): wfi" : "no idle states: wfi");
	}
	if (simulate) kprintf("psci: kolibri.psci=simulate: each power-down played after CPU_SUSPEND\n");
	strncpy0(summary, usable ? (cluster_ok ? "core and cluster states" : "core states") : "wfi only",
		 sizeof(summary));
}

const char *psci_summary(void)
{
	return summary;
}

bool psci_idle(u64 idle_ticks)
{
	if (!usable) return false;
	u64 f = timer_freq();
	u64 us64 = f ? idle_ticks * 1000000 / f : 0;
	u32 us = us64 > 0xffffffffu ? 0xffffffffu : (u32)us64;
	int i = power_dt_choose(&pdt, us, cluster_ok, timer_bcast_ok());
	if (i < 0) return false;
	const struct idle_state *s = &pdt.st[i];
	/* a core state with the timer stopping: the broadcast frame wakes it
	 * (a cluster state goes through the core's) */
	bool tstop = s->timer_stop;
	for (int j = 0; s->cluster && j < pdt.nstates; j++)
		if (!pdt.st[j].cluster && pdt.st[j].timer_stop) tstop = true;
	u64 now = timer_count();
	if (tstop) timer_bcast_set(now + idle_ticks);
	bool down = power_dt_powerdown(s->param, extended);
	u32 before = resumed;
	long r = psci_suspend(PSCI_CPU_SUSPEND, s->param, pdt.hvc, simulate && down);
	if (resumed != before || down) gic3_cpu_init();
	if (tstop) timer_bcast_set(0);
	if (r < 0) {
		pstats.st_fail++;
		if (pstats.st_fail == 1) kprintf("psci: CPU_SUSPEND %x refused (%d)\n", s->param, (int)r);
		/* the cluster refused again and again: the core's states only */
		if (s->cluster && r == PSCI_DENIED && ++cluster_denied == 100) {
			cluster_ok = false;
			kprintf("psci: the cluster's states refused 100 times: the core's only\n");
		}
		return false;
	}
	if (s->cluster) pstats.st_cluster++;
	else pstats.st_core++;
	return true;
}
