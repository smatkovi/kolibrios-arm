/* KolibriOS for ARM, ARMv7 (Cortex-A8): VFP and NEON, the kernel's and
 * the programs' (d0-d31 and FPSCR), the programs' saved lazily.
 *
 * At the start (fpu_init) CPACR gives CP10 and CP11 (VFP, NEON) to PL1
 * only and FPEXC.EN switches them on: the kernel may use NEON at any
 * time, a program's VFP or NEON instruction is undefined. Most programs
 * never use them (they are built soft-float); assembler ones may. The
 * first one traps (the undefined instruction exception), the registers'
 * owner so far is saved, the program's own are loaded (zeros the first
 * time), CPACR gives them to PL0 too and the instruction runs again; the
 * program keeps them, also across switches, until another program or
 * the kernel's NEON needs them. Nothing is saved at a task switch, only
 * CPACR set on the way back to a program (fp_enter): full access for the
 * owner, PL1 only for the others. An undefined instruction from the
 * owner itself is a real one: the program ends, as before.
 *
 * The kernel's NEON (the drawing, blit.h's blit_begin) saves the owner's
 * values first; memcpy and memset (string.S) use NEON only when nobody's
 * values are in the registers (neon_free), general registers otherwise.
 *
 * AArch64 does the same for its AArch32 programs (arch/arm64/fpu.c). */
#include "fpu.h"

struct fpstate {
	u64 d[32];
	u32 fpscr;
	u32 pid;                        /* whose: a slot's next task starts from zeros */
};

/* vfp.S */
void fp_hw_on(void);                    /* FPEXC.EN */
void fp_save(struct fpstate *a);        /* d0-d31, FPSCR */
void fp_load(const struct fpstate *a);
u32 fp_fpexc(void);
u32 fp_fpsid(void);
u32 fp_mvfr0(void);

static struct fpstate *area[MAX_TASKS];
static bool fp_ok;
struct task *fp_owner;
u32 fp_owner_pid;
u32 fp_user;
u32 neon_free;

#define CPACR_CP10_CP11 (15u << 20)
#define CPACR_PL1 (5u << 20)            /* 01 01: privileged access only */
#define FPEXC_EX (1u << 31)
#define FPEXC_DEX (1u << 29)

static u32 cpacr(void)
{
	u32 v;
	asm volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(v));
	return v;
}

static void set_cpacr(u32 v)
{
	asm volatile("mcr p15, 0, %0, c1, c0, 2\n\tisb" :: "r"(v) : "memory");
}

void fpu_init(void)
{
	u32 c = cpacr() & ~CPACR_CP10_CP11;
	set_cpacr(c | CPACR_PL1);
	/* bits that stay 0: no VFP, or the secure side keeps it (NSACR) */
	if ((cpacr() & CPACR_CP10_CP11) != CPACR_PL1) {
		set_cpacr(c);
		kprintf("VFP/NEON: not there\n");
		return;
	}
	fp_hw_on();
	fp_ok = true;
	neon_free = 1;
	kprintf("VFP/NEON: FPSID %x, MVFR0 %x; the programs' registers lazily\n", fp_fpsid(), fp_mvfr0());
}

bool fp_available(void)
{
	return fp_ok;
}

void fp_set_user(u32 on)
{
	fp_user = on;
	set_cpacr((cpacr() & ~CPACR_CP10_CP11) | (on ? CPACR_CP10_CP11 : CPACR_PL1));
}

/* the owner's values saved (before the kernel's NEON or another owner's) */
void fp_release(void)
{
	struct task *t = fp_owner;
	fp_owner = NULL;
	neon_free = fp_ok;
	if (!t || t->state == T_FREE || fp_owner_pid != t->pid || !area[t->slot]) return;
	fp_save(area[t->slot]);
}

/* an undefined instruction from a program (start.S's frame: the pc after
 * it): the registers become its own and it runs again; false if it had
 * them already (a real undefined instruction) or there is no memory */
bool fp_trap(struct task *t, struct trapframe *tf)
{
	if (!fp_ok) return false;
	if (fp_owner == t && fp_owner_pid == t->pid) {
		/* the Cortex-A8 does all of VFPv3 itself; should another core
		 * hand an operation back to software, say so */
		if (fp_fpexc() & (FPEXC_EX | FPEXC_DEX)) {
			kprintf("%s (pid %u): a VFP operation for software (FPEXC %x): not emulated\n", t->name,
				t->pid, fp_fpexc());
			fp_hw_on();
		}
		return false;
	}
	struct fpstate *a = area[t->slot];
	if (!a) {
		a = area[t->slot] = kmalloc_aligned(sizeof(*a), 8);
		if (!a) return false;
		a->pid = 0;
	}
	if (a->pid != t->pid) {
		memset(a, 0, sizeof(*a));
		a->pid = t->pid;
	}
	fp_release();
	fp_load(a);
	fp_owner = t;
	fp_owner_pid = t->pid;
	neon_free = 0;
	fp_set_user(1);
	/* again: the instruction's address (Thumb: the exception's return
	 * address is 2 on, also for a 32-bit instruction) */
	tf->pc -= (tf->cpsr & PSR_T) ? 2 : 4;
	return true;
}

/* a task ends: its values in the registers are nobody's */
void fp_task_gone(struct task *t)
{
	if (fp_owner == t) {
		fp_owner = NULL;
		neon_free = fp_ok;
	}
}
