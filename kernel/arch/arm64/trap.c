/* KolibriOS for ARM, AArch64: exceptions. start.S saves x0-x30, ELR and
 * SPSR; a program (AArch32 at EL0) has its r0-r12 in x0-x12, its sp and
 * lr in x13 and x14, its pc in ELR and its CPSR in SPSR. */
#include "task.h"
#include "frame.h"

extern void arch_enter(struct frame64 *f) __attribute__((noreturn));
extern char stack_top[];

/* to the task the dispatch chose, or the idle loop (EL1, interrupts on) */
static void back_to(struct frame64 *f, struct task *c)
{
	if (c && c->lx) {
		lx_restore(f, c);              /* a Linux program's thread */
	} else if (c) {
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

void trap64(struct frame64 *f, int kind)
{
	struct trapframe tf;
	u64 esr = SYSREG_R(esr_el1), far = SYSREG_R(far_el1);
	int tkind;
	bool from_user = kind == K_SYNC_A32 || kind == K_IRQ_A32;
	switch (kind) {
	case K_IRQ_EL1:
	case K_IRQ_A32:
		tkind = TRAP_IRQ;
		break;
	case K_SYNC_A32: {
		u32 ec = esr >> 26;
		if (ec == 0x11) tkind = TRAP_SVC;                    /* SVC from AArch32 */
		else if (ec == 0x24) tkind = TRAP_DABT;              /* data abort, lower EL */
		else if (ec == 0x20) tkind = TRAP_PABT;              /* instruction abort, lower EL */
		else { tkind = TRAP_UNDEF; far = f->elr; }
		break;
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
	if (from_user) {
		for (int i = 0; i < 13; i++) tf.r[i] = f->x[i];
		tf.usr_sp = f->x[13];
		tf.usr_lr = f->x[14];
		tf.pad = 0;
		tf.pc = f->elr;
		tf.cpsr = f->spsr;
	} else {
		memset(&tf, 0, sizeof(tf));
	}
	back_to(f, trap_dispatch(&tf, tkind, from_user, far, (u32)esr));
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

const struct platform *arch_platform(void)
{
	const void *fdt = (const void *)boot_args[0];
	if ((fdt_root_compatible(fdt, "qcom,sm6115") || fdt_root_compatible(fdt, "fxtec,pro1x")) &&
	    pro1x_probe(fdt))
		return &platform_pro1x;
	virt_probe(fdt);
	return &platform_virt;
}
