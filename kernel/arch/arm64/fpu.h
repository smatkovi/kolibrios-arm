/* KolibriOS for ARM, AArch64: the programs' FP/SIMD registers, lazily
 * (fpu.c) */
#ifndef FPU_H
#define FPU_H

#include "task.h"

extern struct task *a32_fp_owner;       /* whose values the registers hold, if a program's */
extern u32 a32_fp_pid, fp_fpen;         /* ... its pid; CPACR_EL1.FPEN as set */
void fp_set_fpen(u32 v);
bool fp_a32_trap(struct task *t);       /* EC 0x07 from a program */
void fp_lx_enter(void);                 /* the way back to a Linux thread */
void fp_task_gone(struct task *t);

/* the way back to a program: its own registers in place, or a trap at
 * its first use */
static inline void fp_a32_enter(struct task *t)
{
	u32 v = a32_fp_owner == t && a32_fp_pid == t->pid ? 3 : 1;
	if (v != fp_fpen) fp_set_fpen(v);
}

#endif
