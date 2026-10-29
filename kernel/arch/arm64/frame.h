/* KolibriOS for ARM, AArch64: what start.S saves on every exception */
#ifndef FRAME_H
#define FRAME_H

#include "kernel.h"

struct frame64 {
	u64 x[31];
	u64 elr, spsr, pad;
};

enum {
	K_SYNC_EL1 = 1, K_IRQ_EL1 = 2, K_SYNC_A32 = 3, K_IRQ_A32 = 4, K_SERROR = 5,
	K_SYNC_A64 = 6, K_IRQ_A64 = 7,  /* EL0 in AArch64: Linux programs (kernel/linux) */
};

struct task;
struct task *lx_trap(struct frame64 *f, bool irq, u64 esr, u64 far);
void lx_restore(struct frame64 *f, struct task *t);

#endif
