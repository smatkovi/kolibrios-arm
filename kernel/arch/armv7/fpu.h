/* KolibriOS for ARM, ARMv7: the programs' VFP and NEON registers,
 * lazily (fpu.c) */
#ifndef FPU_H
#define FPU_H

#include "task.h"

extern struct task *fp_owner;           /* whose values the registers hold, if a program's */
extern u32 fp_owner_pid, fp_user;       /* ... its pid; 1: CPACR lets programs use them */
extern u32 neon_free;                   /* string.S: NEON without saving anything first */
void fpu_init(void);                    /* after mmu_init: on for the kernel, trapped for programs */
bool fp_trap(struct task *t, struct trapframe *tf); /* an undefined instruction from a program */
void fp_set_user(u32 on);
void fp_release(void);                  /* the owner's values saved: the kernel's NEON next */
void fp_task_gone(struct task *t);

/* the way back to a program: its own registers in place, or a trap at
 * its first VFP or NEON instruction */
static inline void fp_enter(struct task *t)
{
	u32 on = fp_owner == t && fp_owner_pid == t->pid;
	if (on != fp_user) fp_set_user(on);
}

#endif
