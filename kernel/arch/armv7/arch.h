/* KolibriOS for ARM: ARMv7 (Cortex-A8: QEMU's realview-pb-a8, the N950) */
#ifndef ARCH_H
#define ARCH_H

/* programs' memory: 1 MiB sections */
#define USER_ALIGN_MB 1

struct platform;
const struct platform *arch_platform(void);
void arch_start(void) __attribute__((noreturn));
#define ARCH_IRQ_OFF() asm volatile("cpsid if")

#endif
