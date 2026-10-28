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

#define SYSREG_R(name) ({ unsigned long _v; asm volatile("mrs %0, " #name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) asm volatile("msr " #name ", %0" :: "r"((unsigned long)(v)))

#endif
