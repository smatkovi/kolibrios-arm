/* KolibriOS for ARM, AArch64: exceptions. start.S saves x0-x30, ELR and
 * SPSR; a program (AArch32 at EL0) has its r0-r12 in x0-x12, its sp and
 * lr in x13 and x14, its pc in ELR and its CPSR in SPSR. */
#include "task.h"
#include "frame.h"
#include "power.h"
#include "powerdt.h"

extern void arch_enter(struct frame64 *f) __attribute__((noreturn));
#include "fpu.h"
#include "net.h"
extern char stack_top[];

/* to the task the dispatch chose, or the idle loop (EL1, interrupts on) */
static void back_to(struct frame64 *f, struct task *c)
{
	if (c && c->lx) {
		lx_restore(f, c);              /* a Linux program's thread */
	} else if (c) {
		fp_a32_enter(c);
		for (int i = 0; i < 13; i++) f->x[i] = c->tf.r[i];
		f->x[13] = c->tf.usr_sp;
		f->x[14] = c->tf.usr_lr;
		for (int i = 15; i < 31; i++) f->x[i] = 0;
		f->elr = c->tf.pc;
		f->spsr = c->tf.cpsr;          /* AArch32 user: M[4] set */
	} else {
		f->elr = (u64)idle_loop;
		f->spsr = 0x5 | (1 << 9) | (1 << 8) | (1 << 6);  /* EL1h, D A F masked, I open */
	}
}

_Static_assert(__builtin_offsetof(struct task, tf) == 24, "start.S's TASK_TF");
_Static_assert(__builtin_offsetof(struct trapframe, usr_sp) == 52 &&
	       __builtin_offsetof(struct trapframe, pc) == 64, "start.S's frame");

/* the way in from a program (AArch32) and from the idle loop's interrupt
 * (start.S's A32 and IDLE_IRQ vectors): the program's registers are in
 * its task's frame already. Returns the AArch32 task to go back to,
 * whose frame start.S loads, or NULL after filling f for trap_return
 * (a Linux thread, the idle loop). */
struct task *trap32(int kind, u64 esr, u64 far, struct frame64 *f)
{
	struct task *t = current, *c;
	net_enter();                       /* lwIP's idle timers to the present first */
	if (kind == K_IRQ_EL1) {
		struct trapframe none;
		c = trap_dispatch(&none, TRAP_IRQ, false, 0, 0);
	} else {
		int tkind = TRAP_IRQ;
		if (kind == K_SYNC_A32) {
			u32 ec = esr >> 26;
			if (ec == 0x11) tkind = TRAP_SVC;                    /* SVC from AArch32 */
			else if (ec == 0x24) tkind = TRAP_DABT;              /* data abort, lower EL */
			else if (ec == 0x20) tkind = TRAP_PABT;              /* instruction abort, lower EL */
			else if (ec == 0x07 && fp_a32_trap(t)) return t;     /* FP/SIMD: now its own */
			else { tkind = TRAP_UNDEF; far = t->tf.pc; }
		}
		c = trap_dispatch(&t->tf, tkind, true, far, (u32)esr);
	}
	if (c && !c->lx) {
		fp_a32_enter(c);
		return c;
	}
	back_to(f, c);
	return NULL;
}

/* the rest of the exceptions, with all of x0-x30 in the frame: Linux
 * programs' (kernel/linux), the kernel's own faults */
void trap64(struct frame64 *f, int kind)
{
	u64 esr = SYSREG_R(esr_el1), far = SYSREG_R(far_el1);
	net_enter();
	switch (kind) {
	case K_IRQ_EL1: {
		struct trapframe none;
		back_to(f, trap_dispatch(&none, TRAP_IRQ, false, 0, 0));
		return;
	}
	case K_SYNC_EL1:
		panic("exception in the kernel: ESR %x (class %x) at %p, address %p",
		      (u32)esr, (u32)(esr >> 26), (addr_t)f->elr, (addr_t)far);
	case K_SERROR:
		panic("SError: ESR %x at %p", (u32)esr, (addr_t)f->elr);
	case K_SYNC_A64:
	case K_IRQ_A64:
		back_to(f, lx_trap(f, kind == K_IRQ_A64, esr, far));
		return;
	default:
		panic("exception kind %d: ESR %x", kind, (u32)esr);
	}
}

/* kmain's way into the first task: a frame at the top of the kernel stack */
void arch_start(void)
{
	struct trapframe tf;
	memset(&tf, 0, sizeof(tf));
	struct frame64 *f = (struct frame64 *)(stack_top - sizeof(struct frame64));
	struct task *c = trap_dispatch(&tf, TRAP_SVC, false, 0, 0);
	memset(f, 0, sizeof(*f));
	back_to(f, c);
	arch_enter(f);
}

extern int fdt_root_compatible(const void *fdt, const char *compat);
extern bool virt_probe(const void *fdt), pro1x_probe(const void *fdt);
extern addr_t boot_args[3];

/* the platform, and what the architecture reads from the device tree
 * while the MMU is off (the kernel does not map it): the timers, PSCI */
const struct platform *arch_platform(void)
{
	const void *fdt = (const void *)boot_args[0];
	const struct platform *p;
	if ((fdt_root_compatible(fdt, "qcom,sm6115") || fdt_root_compatible(fdt, "fxtec,pro1x")) &&
	    pro1x_probe(fdt)) {
		p = &platform_pro1x;
	} else {
		virt_probe(fdt);
		p = &platform_virt;
	}
	power_dt_parse(fdt, SYSREG_R(mpidr_el1), &pdt);
	/* kexec's (or QEMU's) seed for the random numbers, wiped in the tree */
	extern void rng_fdt_seed(void *fdt);
	rng_fdt_seed((void *)fdt);
	timer_probe();
	psci_probe();
	return p;
}
