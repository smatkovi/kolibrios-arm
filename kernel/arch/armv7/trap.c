/* KolibriOS for ARM: ARMv7's exceptions. start.S builds the trap frame
 * (struct trapframe) on the kernel stack and calls trap_entry(); the
 * frame it returns through is the task's to go on with. */
#include "task.h"
#include "fpu.h"

void trap_entry(struct trapframe *tf, int kind)
{
	bool from_user = (tf->cpsr & 0x1f) == PSR_USR;
	/* a program's first VFP or NEON instruction: its registers, and the
	 * instruction again (fpu.c) */
	if (kind == TRAP_UNDEF && from_user && fp_trap(current, tf)) return;
	addr_t far = 0;
	u32 fsr = 0;
	if (kind == TRAP_DABT) {
		asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(far));
		asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(fsr));
	} else if (kind == TRAP_PABT) {
		asm volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(far));
		asm volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(fsr));
	}
	struct task *c = trap_dispatch(tf, kind, from_user, far, fsr);
	if (c) {
		*tf = c->tf;
		fp_enter(c);
	} else {
		tf->pc = (u32)idle_loop;
		tf->cpsr = PSR_SVC | PSR_F;   /* interrupts on while idle */
	}
}

/* kmain's way into the first task: an SVC from the kernel */
void arch_start(void)
{
	asm volatile("svc #0");
	for (;;);
}

const struct platform *arch_platform(void)
{
#ifdef PLATFORM_N950
	return &platform_n950;
#else
	return &platform_realview;
#endif
}
