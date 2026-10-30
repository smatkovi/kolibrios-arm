/* KolibriOS for ARM: the power management's device tree reading and its
 * choices on the PC (make -C tests/power host): powerdt.c against the
 * SM6115's tree (sm6115-power.dts here, the parts of Linux's sm6115.dtsi
 * that matter; PRO1X_DTB=... the phone's whole tree as well), and
 * against what tests/power/mkdtb.py makes for QEMU.
 *
 *   host SM6115.dtb [QEMU.dtb] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/arch/arm64/powerdt.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	static char buf[4][1 << 20];
	static int n;
	size_t len = fread(buf[n], 1, sizeof(buf[n]), f);
	fclose(f);
	if (!len) { fprintf(stderr, "%s: empty\n", path); exit(1); }
	return buf[n++];
}

static void show(const char *what, const struct power_dt *d)
{
	printf("%s: vtimer INTID %u, frame %#llx (CNTCTLBase %#llx, frame %u, INTID %u), psci %d (%s), "
	       "%d CPUs, self %d, %s\n", what, d->vtimer_id, d->frame_pa, d->ctl_pa, d->frame_num, d->frame_irq,
	       d->psci, d->hvc ? "hvc" : "smc", d->ncpus, d->self, d->hierarchical ? "hierarchical" : "flat");
	for (int i = 0; i < d->nstates; i++)
		printf("  %s %-28s %08x entry %4u exit %4u min %4u%s\n", d->st[i].cluster ? "cluster" : "core   ",
		       d->st[i].name, d->st[i].param, d->st[i].entry_us, d->st[i].exit_us, d->st[i].min_us,
		       d->st[i].timer_stop ? " timer-stop" : "");
}

static u32 param_of(const struct power_dt *d, int i)
{
	return i < 0 ? 0 : d->st[i].param;
}

/* the SM6115 as sm6115.dtsi describes it (and the phone's tree) */
static void sm6115(const void *fdt, const char *what)
{
	struct power_dt d;
	power_dt_parse(fdt, 0, &d);                      /* CPU 0: MPIDR 0 */
	show(what, &d);
	CHECK(d.vtimer_id == 19, "%s: the virtual timer is PPI 3, INTID 19 (%u)", what, d.vtimer_id);
	CHECK(d.ctl_pa == 0xf120000 && d.frame_pa == 0xf121000 && d.frame_num == 0 && d.frame_irq == 40,
	      "%s: frame 0 at 0xf121000, physical timer SPI 8", what);
	CHECK(d.psci && !d.hvc, "%s: PSCI over smc", what);
	CHECK(d.ncpus == 8 && d.self == 0, "%s: 8 CPUs, we are the first", what);
	CHECK(d.mpidr[4] == 0x100 && d.mpidr[7] == 0x103, "%s: the gold cluster's MPIDRs", what);
	CHECK(d.hierarchical, "%s: hierarchical", what);
	CHECK(d.nstates == 3, "%s: 3 states (%d)", what, d.nstates);
	CHECK(d.nstates == 3 && !d.st[0].cluster && d.st[0].param == 0x40000003 && d.st[0].timer_stop &&
	      d.st[0].min_us == 1182, "%s: the core's rail power collapse", what);
	CHECK(d.nstates == 3 && d.st[1].cluster && d.st[1].param == 0x40000022 && d.st[1].min_us == 782,
	      "%s: GDHS", what);
	CHECK(d.nstates == 3 && d.st[2].cluster && d.st[2].param == 0x41000044 && d.st[2].min_us == 7376,
	      "%s: the cluster's power collapse", what);
	/* the choices: by residency, the cluster below the core's state, the
	 * timer's stop needs the broadcast frame */
	CHECK(power_dt_choose(&d, 1000, true, true) < 0, "%s: 1 ms: wfi", what);
	CHECK(param_of(&d, power_dt_choose(&d, 2000, false, true)) == 0x40000003, "%s: 2 ms, PC: core", what);
	CHECK(param_of(&d, power_dt_choose(&d, 2000, true, true)) == 0x40000022, "%s: 2 ms, OSI: GDHS", what);
	CHECK(param_of(&d, power_dt_choose(&d, 7376, true, true)) == 0x41000044, "%s: 7.4 ms: collapse", what);
	CHECK(param_of(&d, power_dt_choose(&d, 600000000, true, true)) == 0x41000044, "%s: 10 min: collapse", what);
	CHECK(param_of(&d, power_dt_choose(&d, 600000000, false, true)) == 0x40000003, "%s: 10 min PC: core", what);
	CHECK(power_dt_choose(&d, 600000000, true, false) < 0, "%s: without the broadcast frame: wfi", what);
	/* the SoC's sleep held off (power_hold_awake: psci_idle chooses for an
	 * idle just short of the collapse's residency): GDHS, the GIC on */
	CHECK(param_of(&d, power_dt_choose(&d, d.st[2].min_us - 1, true, true)) == 0x40000022,
	      "%s: held awake: GDHS instead of the collapse", what);
	CHECK(power_dt_powerdown(0x40000003, true) && power_dt_powerdown(0x41000044, true) &&
	      !power_dt_powerdown(0x00000003, true), "%s: extended power-down bit", what);

	/* the RPM: the message RAM, the vMPM in it, the statistics */
	printf("%s: message RAM %#llx (%#x), vMPM %#llx (%#x bytes, %u pins, IPC bit %u, INTID %u), stats %#llx\n",
	       what, d.msgram_pa, d.msgram_size, d.vmpm_pa, d.vmpm_size, d.mpm_pins, d.mpm_bit, d.mpm_irq, d.stats_pa);
	CHECK(d.msgram_pa == 0x45f0000 && d.msgram_size == 0x7000, "%s: the RPM's message RAM", what);
	CHECK(d.mpm && d.vmpm_pa == 0x45f01b8 && d.vmpm_size == 0x48 && d.mpm_pins == 96 && d.mpm_bit == 1 &&
	      d.mpm_irq == 229, "%s: the vMPM at 0x45f01b8, 96 pins, IPC bit 1, SPI 197", what);
	CHECK(d.stats_pa == 0x4690000 && d.stats_size == 0x10000, "%s: the RPM's statistics", what);

	/* the gold cluster's first core */
	struct power_dt g;
	power_dt_parse(fdt, 0x100, &g);
	CHECK(g.self == 4 && g.nstates == 3 && g.st[1].param == 0x40000042, "%s: CPU 4's states", what);
	/* a core not in the tree */
	power_dt_parse(fdt, 0x200, &g);
	CHECK(g.self < 0 && g.nstates == 0, "%s: no such core: no states", what);
}

/* QEMU's virt with tests/power/mkdtb.py --psci */
static void qemu(const void *fdt)
{
	struct power_dt d;
	power_dt_parse(fdt, 0x80000000ull, &d);          /* QEMU's MPIDR: bit 31 set */
	show("qemu", &d);
	CHECK(d.vtimer_id == 27, "qemu: PPI 11, INTID 27 (%u)", d.vtimer_id);
	CHECK(!d.frame_pa, "qemu: no memory-mapped timer");
	CHECK(d.psci && d.hvc && d.ncpus == 1 && d.self == 0, "qemu: PSCI over hvc, one CPU");
	CHECK(d.nstates == 3 && d.st[0].param == 0x10003 && !d.st[0].timer_stop, "qemu: QEMU's states");
	CHECK(param_of(&d, power_dt_choose(&d, 2000, false, false)) == 0x10003, "qemu: 2 ms: the core's");
	CHECK(!power_dt_powerdown(0x10003, true) && power_dt_powerdown(0x10003, false),
	      "qemu: the original format's power-down bit");
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "host SM6115.dtb [QEMU.dtb [PRO1X.dtb]]\n"); return 2; }
	sm6115(load(argv[1]), "sm6115-power.dts");
	if (argc > 2) qemu(load(argv[2]));
	if (argc > 3) sm6115(load(argv[3]), "the phone's tree");
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
