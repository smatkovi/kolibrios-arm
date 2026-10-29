/* KolibriOS for ARM, AArch64: the device tree's timers, PSCI and idle
 * states (powerdt.h), after Linux's bindings: timer/arm,arch_timer.yaml
 * (the interrupts: secure, non-secure physical, virtual, hypervisor),
 * timer/arm,arch_timer_mmio.yaml (the frames' interrupts: physical,
 * virtual), cpu/idle-states.yaml and power/domain-idle-state.yaml, and
 * drivers/cpuidle/cpuidle-psci{,-domain}.c for how a core's states and
 * its cluster's go together. */
#include "powerdt.h"
#include "fdt.h"

static bool str_is(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

/* an interrupt specifier: its INTID (PPI 16 + n, SPI 32 + n) */
static u32 intid_of(const u8 *spec)
{
	u32 type = (u32)fdt_cells(spec, 1), num = (u32)fdt_cells(spec + 4, 1);
	return type == 1 ? 16 + num : 32 + num;
}

static bool enabled(const void *fdt, int node)
{
	int len;
	const char *st = fdt_prop(fdt, node, "status", &len);
	return !st || str_is(st, "okay") || str_is(st, "ok");
}

static void timers(const void *fdt, struct power_dt *d)
{
	int ac, sc, len;
	int g = fdt_find_compatible(fdt, "arm,gic-v3", -1, &ac, &sc);
	int ic = g >= 0 ? (int)fdt_u32(fdt, g, "#interrupt-cells", 3) : 3;
	if (ic < 2) return;
	int t = fdt_find_compatible(fdt, "arm,armv8-timer", -1, &ac, &sc);
	const u8 *p = t >= 0 ? fdt_prop(fdt, t, "interrupts", &len) : NULL;
	if (p && len >= 3 * ic * 4 && fdt_cells(p + 2 * ic * 4, 1) == 1)
		d->vtimer_id = intid_of(p + 2 * ic * 4);
	int m = fdt_find_compatible(fdt, "arm,armv7-timer-mem", -1, &ac, &sc);
	u64 a, s;
	if (m < 0 || !enabled(fdt, m) || !fdt_reg(fdt, m, ac, sc, 0, &a, &s)) return;
	int fac = fdt_u32(fdt, m, "#address-cells", 2), fsc = fdt_u32(fdt, m, "#size-cells", 1);
	for (int c = fdt_next_child(fdt, m, -1); c >= 0; c = fdt_next_child(fdt, m, c)) {
		const u8 *ip = fdt_prop(fdt, c, "interrupts", &len);
		u64 fa, fs;
		if (!enabled(fdt, c) || !ip || len < ic * 4 || !fdt_reg(fdt, c, fac, fsc, 0, &fa, &fs)) continue;
		d->ctl_pa = a;
		d->frame_pa = fa;
		d->frame_num = fdt_u32(fdt, c, "frame-number", 0);
		d->frame_irq = intid_of(ip);
		break;
	}
}

/* a state node into the list, kept sorted by residency within its group */
static void add_state(const void *fdt, int node, bool cluster, struct power_dt *d)
{
	int len;
	const void *pp = fdt_prop(fdt, node, "arm,psci-suspend-param", &len);
	if (node < 0 || !enabled(fdt, node) || !pp || len < 4 || d->nstates == PDT_STATES) return;
	struct idle_state s = { 0 };
	s.param = (u32)fdt_cells(pp, 1);
	s.entry_us = fdt_u32(fdt, node, "entry-latency-us", 0);
	s.exit_us = fdt_u32(fdt, node, "exit-latency-us", 0);
	s.min_us = fdt_u32(fdt, node, "min-residency-us", 0);
	s.timer_stop = fdt_prop(fdt, node, "local-timer-stop", &len) != NULL;
	s.cluster = cluster;
	const char *n = fdt_prop(fdt, node, "idle-state-name", &len);
	if (!n) n = fdt_node_name(fdt, node);
	u32 i = 0;
	for (; n && n[i] && i + 1 < sizeof(s.name); i++) s.name[i] = n[i];
	s.name[i] = 0;
	int at = d->nstates;
	while (at > 0 && d->st[at - 1].cluster == cluster && d->st[at - 1].min_us > s.min_us) {
		d->st[at] = d->st[at - 1];
		at--;
	}
	d->st[at] = s;
	d->nstates++;
}

/* the states a phandle list names */
static void add_states(const void *fdt, int node, const char *prop, bool cluster, struct power_dt *d)
{
	int len;
	const void *p = node >= 0 ? fdt_prop(fdt, node, prop, &len) : NULL;
	for (int i = 0; p && i < len / 4; i++)
		add_state(fdt, fdt_find_phandle(fdt, (u32)fdt_cells((const u8 *)p + 4 * i, 1)), cluster, d);
}

static int phandle_at(const void *fdt, int node, const char *prop)
{
	int len;
	const void *p = node >= 0 ? fdt_prop(fdt, node, prop, &len) : NULL;
	return p && len >= 4 ? fdt_find_phandle(fdt, (u32)fdt_cells(p, 1)) : -1;
}

static void psci(const void *fdt, u64 my_mpidr, struct power_dt *d)
{
	int ac, sc, len;
	int p = fdt_find_compatible(fdt, "arm,psci-1.0", -1, &ac, &sc);
	if (p < 0) p = fdt_find_compatible(fdt, "arm,psci-0.2", -1, &ac, &sc);
	if (p < 0 || !enabled(fdt, p)) return;
	const char *m = fdt_prop(fdt, p, "method", &len);
	if (!m || (!str_is(m, "smc") && !str_is(m, "hvc"))) return;
	d->psci = true;
	d->hvc = str_is(m, "hvc");
	/* the CPUs and their MPIDRs (reg, in /cpus' cells) */
	int cpus = fdt_find_name(fdt, "cpus", &ac, &sc);
	int cac = cpus >= 0 ? (int)fdt_u32(fdt, cpus, "#address-cells", 1) : 1;
	int me = -1;
	for (int c = cpus >= 0 ? fdt_next_child(fdt, cpus, -1) : -1; c >= 0; c = fdt_next_child(fdt, cpus, c)) {
		const char *dt = fdt_prop(fdt, c, "device_type", &len);
		u64 reg;
		if (!dt || !str_is(dt, "cpu") || !fdt_reg(fdt, c, cac, 0, 0, &reg, NULL)) continue;
		if (reg == (my_mpidr & 0xff00ffffffull)) {
			me = c;
			d->self = d->ncpus;
		}
		if (d->ncpus < PDT_CPUS) d->mpidr[d->ncpus++] = reg;
	}
	if (me < 0) return;
	/* its states: cpu-idle-states (flat), or its power domain's and the
	 * domain above it (the cluster) */
	if (fdt_prop(fdt, me, "cpu-idle-states", &len)) {
		add_states(fdt, me, "cpu-idle-states", false, d);
		return;
	}
	int pd = phandle_at(fdt, me, "power-domains");
	if (pd < 0) return;
	d->hierarchical = true;
	add_states(fdt, pd, "domain-idle-states", false, d);
	add_states(fdt, phandle_at(fdt, pd, "power-domains"), "domain-idle-states", true, d);
}

/* the RPM's parts for the SoC's sleep: the vMPM is a child of the
 * message RAM's node (sm6115.dtsi with pro1x-maemo-leste's patch 0006:
 * sram@1b8, 0x48 bytes, through the message RAM's ranges) */
static void rpm(const void *fdt, struct power_dt *d)
{
	int ac, sc, len;
	u64 a, s;
	int r = fdt_find_compatible(fdt, "qcom,rpm-msg-ram", -1, &ac, &sc);
	if (r >= 0 && fdt_reg(fdt, r, ac, sc, 0, &a, &s)) {
		d->msgram_pa = a;
		d->msgram_size = (u32)s;
	}
	int st = fdt_find_compatible(fdt, "qcom,rpm-stats", -1, &ac, &sc);
	if (st >= 0 && enabled(fdt, st) && fdt_reg(fdt, st, ac, sc, 0, &a, &s)) {
		d->stats_pa = a;
		d->stats_size = (u32)s;
	}
	int m = fdt_find_compatible(fdt, "qcom,mpm", -1, &ac, &sc);
	if (m < 0 || !enabled(fdt, m) || r < 0) return;
	int v = phandle_at(fdt, m, "qcom,rpm-msg-ram");
	const u8 *mb = fdt_prop(fdt, m, "mboxes", &len);
	if (v < 0 || !mb || len < 8) return;
	/* v below the message RAM: its reg in the RAM's cells, translated
	 * by the RAM's ranges (child address, parent address, size) */
	int rac = fdt_u32(fdt, r, "#address-cells", 1), rsc = fdt_u32(fdt, r, "#size-cells", 1);
	bool child = false;
	for (int c = fdt_next_child(fdt, r, -1); c >= 0; c = fdt_next_child(fdt, r, c))
		if (c == v) child = true;
	if (!child || !fdt_reg(fdt, v, rac, rsc, 0, &a, &s)) return;
	const u8 *rg = fdt_prop(fdt, r, "ranges", &len);
	int step = 4 * (rac + ac + rsc);
	u64 pa = 0;
	bool found = false;
	for (int o = 0; rg && o + step <= len; o += step) {
		u64 cb = fdt_cells(rg + o, rac), pb = fdt_cells(rg + o + 4 * rac, ac);
		u64 sz = fdt_cells(rg + o + 4 * (rac + ac), rsc);
		if (a >= cb && a + s <= cb + sz) {
			pa = pb + (a - cb);
			found = true;
			break;
		}
	}
	if (!found) return;
	d->vmpm_pa = pa;
	d->vmpm_size = (u32)s;
	d->mpm_bit = (u32)fdt_cells(mb + 4, 1);
	d->mpm_pins = fdt_u32(fdt, m, "qcom,mpm-pin-count", 0);
	const u8 *ip = fdt_prop(fdt, m, "interrupts", &len);
	if (ip && len >= 12 && fdt_cells(ip, 1) == 0) d->mpm_irq = intid_of(ip);
	d->mpm = d->mpm_pins && d->mpm_pins <= 96 && 4 * (2 + 5 * ((d->mpm_pins + 31) / 32)) <= d->vmpm_size;
}

void power_dt_parse(const void *fdt, u64 my_mpidr, struct power_dt *d)
{
	*d = (struct power_dt){ .vtimer_id = 27, .self = -1 };
	if (!fdt_ok(fdt)) return;
	timers(fdt, d);
	psci(fdt, my_mpidr, d);
	rpm(fdt, d);
}

bool power_dt_powerdown(u32 param, bool extended)
{
	return param & (extended ? 1u << 30 : 1u << 16);
}

int power_dt_choose(const struct power_dt *d, u32 us, bool cluster_ok, bool bcast_ok)
{
	int core = -1, best = -1;
	for (int i = 0; i < d->nstates; i++) {
		const struct idle_state *s = &d->st[i];
		if (s->cluster || s->min_us > us || (s->timer_stop && !bcast_ok)) continue;
		if (core < 0 || s->min_us >= d->st[core].min_us) core = i;
	}
	if (core < 0) return -1;
	best = core;
	/* the cluster's only below the core's deepest state, as genpd
	 * powers the cluster's domain off only after the core's */
	for (int i = 0; cluster_ok && i < d->nstates; i++) {
		const struct idle_state *s = &d->st[i];
		if (!s->cluster || s->min_us > us) continue;
		bool deepest_core = true;
		for (int j = 0; j < d->nstates; j++)
			if (!d->st[j].cluster && d->st[j].min_us > d->st[core].min_us) deepest_core = false;
		if (!deepest_core) break;
		if (!d->st[best].cluster || s->min_us >= d->st[best].min_us) best = i;
	}
	return best;
}
