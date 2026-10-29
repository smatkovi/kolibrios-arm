/* KolibriOS for ARM, AArch64: the GICv3 interrupt controller (system
 * register interface). The timers' interrupts go to timer.c (the virtual
 * timer: the device tree's PPI, what Linux uses below a hypervisor). One
 * core only. */
#include "kernel.h"
#include "power.h"

#define GICD_CTLR 0x0000
#define GICD_TYPER 0x0004
#define GICD_ISENABLER 0x0100
#define GICD_ICENABLER 0x0180
#define GICD_ICPENDR 0x0280
#define GICD_ICFGR 0x0c00
#define GICD_IGROUPR 0x0080
#define GICD_IPRIORITYR 0x0400
#define GICD_IROUTER 0x6000
#define GICR_TYPER 0x0008
#define GICR_WAKER 0x0014
#define SGI 0x10000
#define GICR_IGROUPR0 0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_ICENABLER0 0x0180
#define GICR_ICPENDR0 0x0280
#define GICR_IPRIORITYR 0x0400
static addr_t gicd, gicr;
static void (*other_irq)(u32 id);

void gic3_init(addr_t dist_pa, addr_t redist_pa, void (*irq)(u32 id))
{
	other_irq = irq;
	gicd = mmu_io_va(dist_pa);
	/* this core's redistributor: the frame with its affinity */
	u64 mpidr = SYSREG_R(mpidr_el1);
	u32 aff = (mpidr & 0xffffff) | ((mpidr >> 32) & 0xff) << 24;
	addr_t r = mmu_io_va(redist_pa);
	for (int i = 0; i < 16; i++, r += 0x20000) {
		u64 typer = *(volatile u64 *)(r + GICR_TYPER);
		if ((typer >> 32) == aff) { gicr = r; break; }
		if (typer & (1 << 4)) break;            /* the last one */
	}
	if (!gicr) gicr = mmu_io_va(redist_pa);
	wr32(gicd + GICD_CTLR, (1 << 4) | (1 << 1) | (1 << 0)); /* ARE_NS, group 1 */
	for (int i = 0; i < 100000 && (rd32(gicd + GICD_CTLR) & (1u << 31)); i++)
		;
	wr32(gicr + GICR_WAKER, rd32(gicr + GICR_WAKER) & ~2u);
	for (int i = 0; i < 100000 && (rd32(gicr + GICR_WAKER) & 4); i++)
		;
	/* nothing of what the previous system (Linux, before kexec) had on:
	 * the shared interrupts off, the private ones but the timer */
	u32 ppi = timer_intid();
	u32 nlines = ((rd32(gicd + GICD_TYPER) & 0x1f) + 1) * 32;
	for (u32 i = 32; i < nlines && i < 1020; i += 32) {
		wr32(gicd + GICD_ICENABLER + i / 8, 0xffffffffu);
		wr32(gicd + GICD_ICPENDR + i / 8, 0xffffffffu);
	}
	wr32(gicr + SGI + GICR_ICENABLER0, ~(1u << ppi));
	wr32(gicr + SGI + GICR_ICPENDR0, ~(1u << ppi));
	wr32(gicr + SGI + GICR_IGROUPR0, 0xffffffffu);
	wr8(gicr + SGI + GICR_IPRIORITYR + ppi, 0xa0);
	wr32(gicr + SGI + GICR_ISENABLER0, 1u << ppi);
	gic3_cpu_init();
	timer_start();
}

/* the CPU interface (system registers): at the start, and again after a
 * power-down idle state (psci.c), which loses it, as Linux's
 * gic_cpu_sys_reg_init does from its CPU PM notifier */
void gic3_cpu_init(void)
{
	SYSREG_W(S3_0_C12_C12_5, SYSREG_R(S3_0_C12_C12_5) | 7);  /* ICC_SRE_EL1 */
	asm volatile("isb");
	SYSREG_W(S3_0_C4_C6_0, 0xf0);                             /* ICC_PMR_EL1 */
	SYSREG_W(S3_0_C12_C12_3, 0);                              /* ICC_BPR1_EL1 */
	SYSREG_W(S3_0_C12_C12_4, SYSREG_R(S3_0_C12_C12_4) & ~2ul); /* ICC_CTLR_EL1: EOI deactivates */
	SYSREG_W(S3_0_C12_C12_7, 1);                              /* ICC_IGRPEN1_EL1 */
	asm volatile("isb");
}

/* a shared peripheral interrupt to this core, edge or level triggered */
void gic3_enable_spi_edge(u32 id, bool edge)
{
	addr_t cfg = gicd + GICD_ICFGR + 4 * (id / 16);
	u32 bit = 2u << 2 * (id % 16);
	wr32(cfg, edge ? rd32(cfg) | bit : rd32(cfg) & ~bit);
	gic3_enable_spi(id);
}

/* ... as it is configured (level: the reset value; or what Linux left) */
void gic3_enable_spi(u32 id)
{
	wr8(gicd + GICD_IPRIORITYR + id, 0xa0);
	/* group 1 (QEMU's resets to 0; with TrustZone the write is ignored) */
	wr32(gicd + GICD_IGROUPR + 4 * (id / 32), rd32(gicd + GICD_IGROUPR + 4 * (id / 32)) | 1u << (id % 32));
	*(volatile u64 *)(gicd + GICD_IROUTER + 8 * id) = SYSREG_R(mpidr_el1) & 0xff00ffffff;
	wr32(gicd + GICD_ISENABLER + 4 * (id / 32), 1u << (id % 32));
}

void gic3_irq(void)
{
	timer_sync();
	for (;;) {
		u64 iar = SYSREG_R(S3_0_C12_C12_0);                       /* ICC_IAR1_EL1 */
		u32 id = iar & 0xffffff;
		if (id >= 1020 && id <= 1023) break;
		if (!timer_irq(id)) {
			pstats.irq_other++;
			if (other_irq) other_irq(id);
		}
		SYSREG_W(S3_0_C12_C12_1, iar);                            /* ICC_EOIR1_EL1 */
		asm volatile("isb");
	}
}
