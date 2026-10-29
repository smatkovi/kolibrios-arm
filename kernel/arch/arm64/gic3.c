/* KolibriOS for ARM, AArch64: the GICv3 interrupt controller (system
 * register interface) and the 100 Hz tick from the virtual timer (PPI 27,
 * what Linux uses below a hypervisor). One core only. */
#include "kernel.h"

#define GICD_CTLR 0x0000
#define GICD_TYPER 0x0004
#define GICD_ISENABLER 0x0100
#define GICD_ICENABLER 0x0180
#define GICD_ICPENDR 0x0280
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
#define TIMER_PPI 27

static addr_t gicd, gicr;
static u32 timer_reload;
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
	u32 nlines = ((rd32(gicd + GICD_TYPER) & 0x1f) + 1) * 32;
	for (u32 i = 32; i < nlines && i < 1020; i += 32) {
		wr32(gicd + GICD_ICENABLER + i / 8, 0xffffffffu);
		wr32(gicd + GICD_ICPENDR + i / 8, 0xffffffffu);
	}
	wr32(gicr + SGI + GICR_ICENABLER0, ~(1u << TIMER_PPI));
	wr32(gicr + SGI + GICR_ICPENDR0, ~(1u << TIMER_PPI));
	wr32(gicr + SGI + GICR_IGROUPR0, 0xffffffffu);
	wr8(gicr + SGI + GICR_IPRIORITYR + TIMER_PPI, 0xa0);
	wr32(gicr + SGI + GICR_ISENABLER0, 1u << TIMER_PPI);
	SYSREG_W(S3_0_C12_C12_5, SYSREG_R(S3_0_C12_C12_5) | 7);  /* ICC_SRE_EL1 */
	asm volatile("isb");
	SYSREG_W(S3_0_C4_C6_0, 0xf0);                             /* ICC_PMR_EL1 */
	SYSREG_W(S3_0_C12_C12_3, 0);                              /* ICC_BPR1_EL1 */
	SYSREG_W(S3_0_C12_C12_4, SYSREG_R(S3_0_C12_C12_4) & ~2ul); /* ICC_CTLR_EL1: EOI deactivates */
	SYSREG_W(S3_0_C12_C12_7, 1);                              /* ICC_IGRPEN1_EL1 */
	asm volatile("isb");
	/* the tick */
	timer_reload = SYSREG_R(cntfrq_el0) / 100;
	SYSREG_W(cntv_tval_el0, timer_reload);
	SYSREG_W(cntv_ctl_el0, 1);
	asm volatile("isb");
}

/* a shared peripheral interrupt to this core */
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
	for (;;) {
		u64 iar = SYSREG_R(S3_0_C12_C12_0);                       /* ICC_IAR1_EL1 */
		u32 id = iar & 0xffffff;
		if (id >= 1020 && id <= 1023) break;
		if (id == TIMER_PPI) {
			SYSREG_W(cntv_tval_el0, timer_reload);
			timer_tick();
		} else if (other_irq) {
			other_irq(id);
		}
		SYSREG_W(S3_0_C12_C12_1, iar);                            /* ICC_EOIR1_EL1 */
		asm volatile("isb");
	}
}
