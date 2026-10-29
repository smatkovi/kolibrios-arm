/* KolibriOS for ARM: AArch64 (QEMU's virt, the Pro1X); the programs run
 * at EL0 in AArch32 */
#ifndef ARCH_H
#define ARCH_H

/* programs' memory: 2 MiB blocks */
#define USER_ALIGN_MB 2

struct platform;
const struct platform *arch_platform(void);
void arch_start(void) __attribute__((noreturn));
#define ARCH_IRQ_OFF() asm volatile("msr daifset, #0xf")

/* the GICv3 (gic3.c) */
void gic3_init(addr_t dist_pa, addr_t redist_pa, void (*irq)(u32 id));
void gic3_irq(void);
void gic3_enable_spi(u32 id);
void gic3_enable_spi_edge(u32 id, bool edge);
void gic3_cpu_init(void);
/* a device region for the MMU's tables, from a probe (MMU off) */
void mmu_add_io(addr_t pa, u32 size);

#define SYSREG_R(name) ({ unsigned long _v; asm volatile("mrs %0, " #name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) asm volatile("msr " #name ", %0" :: "r"((unsigned long)(v)))

#endif
