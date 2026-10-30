/* KolibriOS for ARM, AArch64: the FP/SIMD registers of the programs
 * (AArch32: VFP and NEON, d0-d31 and FPSCR in v0-v31 and FPCR/FPSR),
 * saved lazily.
 *
 * Most programs never touch them (they are built soft-float), assembler
 * ones may. A program runs with CPACR_EL1.FPEN = 01 (EL0's use traps,
 * the kernel's does not) unless the registers hold its own values: its
 * first FP or NEON instruction then traps (EC 0x07), the registers'
 * owner so far is saved, the program's own are loaded (zeros the first
 * time) and it runs on with FPEN = 11, also after switches, until
 * another one needs them. Nothing is saved at a task switch.
 *
 * The Linux threads' registers are lx.c's (lx_fp_owner: saved when
 * another Linux thread runs, loaded when one comes back); at most one
 * of the two owners holds the registers. The kernel's own NEON (blit.h's
 * blit_begin) saves whichever owner it is first. */
#include "fpu.h"
#include "blit.h"

struct fpstate {
	u8 q[512];                      /* q0-q31 */
	u64 fpcr, fpsr;
	u32 pid;                        /* whose: a slot's next task starts from zeros */
};

extern void lx_fp_save(u8 *area, u64 *fpcr);
extern void lx_fp_load(const u8 *area, const u64 *fpcr);
struct lx_thread;
extern struct lx_thread *lx_fp_owner;   /* lx.c */
void lx_fp_release(void);
void fp_release_all(void);

static struct fpstate *area[MAX_TASKS];
struct task *a32_fp_owner;
u32 a32_fp_pid;
u32 fp_fpen = 3;                        /* blit_init, lx_init: 11 */
#define owner_pid a32_fp_pid

void fp_set_fpen(u32 v)
{
	fp_fpen = v;
	SYSREG_W(cpacr_el1, (SYSREG_R(cpacr_el1) & ~(3ul << 20)) | (u64)v << 20);
	/* the eret back to EL0 synchronises; the kernel may use them either way */
}

static void set_fpen(u32 v)
{
	if (v != fp_fpen) fp_set_fpen(v);
}

/* the program's registers saved (the kernel or a Linux thread needs them) */
void fp_a32_release(void)
{
	struct task *t = a32_fp_owner;
	a32_fp_owner = NULL;
	if (!t || t->state == T_FREE || owner_pid != t->pid || !area[t->slot]) return;
	lx_fp_save(area[t->slot]->q, &area[t->slot]->fpcr);
}

/* blit.h: before the kernel's NEON */
void fp_release_all(void)
{
	if (lx_fp_owner) lx_fp_release();
	if (a32_fp_owner) fp_a32_release();
}

/* the way back to a Linux thread (lx.c, before it loads its own) */
void fp_lx_enter(void)
{
	if (a32_fp_owner) fp_a32_release();
	set_fpen(3);
}

/* EC 0x07 from a program: the registers become its own; false if there
 * is no memory for them (the program ends as at an undefined one) */
bool fp_a32_trap(struct task *t)
{
	struct fpstate *a = area[t->slot];
	if (!a) {
		a = area[t->slot] = kmalloc_aligned(sizeof(*a), 16);
		if (!a) return false;
		a->pid = 0;
	}
	if (a->pid != t->pid) {
		memset(a, 0, sizeof(*a));
		a->pid = t->pid;
	}
	fp_release_all();
	lx_fp_load(a->q, &a->fpcr);
	a32_fp_owner = t;
	owner_pid = t->pid;
	set_fpen(3);
	return true;
}

/* a task ends: its values in the registers are nobody's */
void fp_task_gone(struct task *t)
{
	if (a32_fp_owner == t) a32_fp_owner = NULL;
}
