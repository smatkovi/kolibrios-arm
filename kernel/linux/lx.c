/* KolibriOS for ARM: the Linux personality: starting a static AArch64
 * ELF program, its threads as kernel tasks, the way in from EL0 (system
 * calls, page faults, interrupts) and back, waiting, ending. */
#include "lx.h"
#include "frame.h"
#include "fpu.h"

extern void lx_fp_save(u8 *area, u64 *fpcr);
extern void lx_fp_load(const u8 *area, const u64 *fpcr);

/* whose FP/SIMD registers the CPU holds: saved only when another Linux
 * thread runs, or when the kernel draws with NEON (blit.h) */
struct lx_thread *lx_fp_owner;
#define fp_owner lx_fp_owner

void lx_fp_release(void)
{
	if (!fp_owner) return;
	lx_fp_save(fp_owner->fp, &fp_owner->fpcr);
	fp_owner = NULL;
}
static u64 freq;
static u64 hwcap;

void lx_init(void)
{
	freq = SYSREG_R(cntfrq_el0);
	SYSREG_W(cntkctl_el1, SYSREG_R(cntkctl_el1) | 2);      /* EL0 reads CNTVCT (Go's cputicks) */
	SYSREG_W(cpacr_el1, SYSREG_R(cpacr_el1) | 3ul << 20);  /* FP/SIMD not trapped */
	asm volatile("isb");
	/* AT_HWCAP: what the CPU has (Go and musl choose their crypto by it) */
	u64 isar0 = SYSREG_R(id_aa64isar0_el1);
	hwcap = 1 << 0 | 1 << 1;                                 /* FP, ASIMD */
	if (((isar0 >> 4) & 15) >= 1) hwcap |= 1 << 3;          /* AES */
	if (((isar0 >> 4) & 15) >= 2) hwcap |= 1 << 4;          /* PMULL */
	if (((isar0 >> 8) & 15) >= 1) hwcap |= 1 << 5;          /* SHA1 */
	if (((isar0 >> 12) & 15) >= 1) hwcap |= 1 << 6;         /* SHA2 */
	if (((isar0 >> 12) & 15) >= 2) hwcap |= 1 << 21;        /* SHA512 */
	if (((isar0 >> 16) & 15) >= 1) hwcap |= 1 << 7;         /* CRC32 */
	if (((isar0 >> 20) & 15) >= 2) hwcap |= 1 << 8;         /* atomics */
	lx_fs_init();
}

u64 lx_now_ns(void)
{
	u64 c = SYSREG_R(cntvct_el0);
	return c / freq * 1000000000ull + c % freq * 1000000000ull / freq;
}

u64 lx_ticks_of_ns(u64 ns)
{
	return ns / 1000000000ull * freq + ns % 1000000000ull * freq / 1000000000ull;
}

void lx_set_deadline(struct lx_thread *l, u64 ns)
{
	u64 d = SYSREG_R(cntvct_el0) + lx_ticks_of_ns(ns);
	l->deadline = d ? d : 1;
}

bool lx_timed_out(struct lx_thread *l)
{
	return l->deadline && SYSREG_R(cntvct_el0) >= l->deadline;
}

/* the ELF */
struct ehdr {
	u8 ident[16];
	u16 type, machine;
	u32 version;
	u64 entry, phoff, shoff;
	u32 flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
	u32 type, flags;
	u64 offset, vaddr, paddr, filesz, memsz, align;
};
#define PT_LOAD 1
#define PT_INTERP 3
#define PIE_BASE 0x400000ul

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7,
       AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_PLATFORM = 15,
       AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25, AT_HWCAP2 = 26,
       AT_EXECFN = 31 };

static const char *const env[] = {
	"HOME=/home/user", "PATH=/bin", "LANG=C.UTF-8", "TMPDIR=/tmp", "USER=user", NULL,
};

static struct lx_thread *thread_new(void)
{
	struct lx_thread *l = (struct lx_thread *)lx_page_alloc();
	return l;
}

/* the main thread's stack: argc, argv, envp, the auxiliary vector, the
 * strings above them */
static u64 build_stack(struct lx_proc *p, const char *path, const char *args,
		       const struct ehdr *h, u64 bias, u64 phdr_va)
{
	char argbuf[256];
	const char *argv[32];
	int argc = 0;
	argv[argc++] = path;
	strncpy0(argbuf, args ? args : "", sizeof(argbuf));
	for (char *s = argbuf; *s && argc < 31;) {
		while (*s == ' ') *s++ = 0;
		if (!*s) break;
		argv[argc++] = s;
		while (*s && *s != ' ') s++;
	}
	int envc = 0;
	while (env[envc]) envc++;
	u64 sp = LX_STACK_TOP;
#define PUSH_STR(s) ({ u32 _n = strlen(s) + 1; sp -= _n; lx_copy_out(p, sp, (s), _n); sp; })
	u64 a_str[32], e_str[8];
	u64 execfn = PUSH_STR(path);
	for (int i = 0; i < argc; i++) a_str[i] = PUSH_STR(argv[i]);
	for (int i = 0; i < envc; i++) e_str[i] = PUSH_STR(env[i]);
	u64 plat_s = PUSH_STR("aarch64");
	u8 rnd[16];
	lx_random(rnd, sizeof(rnd));
	sp -= 16;
	lx_copy_out(p, sp, rnd, 16);
	u64 rnd_va = sp;
	u64 aux[] = {
		AT_PHDR, phdr_va, AT_PHENT, sizeof(struct phdr), AT_PHNUM, h->phnum,
		AT_PAGESZ, 4096, AT_BASE, 0, AT_ENTRY, h->entry + bias,
		AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0, AT_SECURE, 0,
		AT_HWCAP, hwcap, AT_HWCAP2, 0, AT_CLKTCK, 100, AT_PLATFORM, plat_s,
		AT_RANDOM, rnd_va, AT_EXECFN, execfn, AT_NULL, 0,
	};
	u64 n = 1 + argc + 1 + envc + 1 + ARRAY_SIZE(aux);
	sp = (sp - n * 8) & ~15ul;
	u64 v = sp;
	u64 w = argc;
	lx_copy_out(p, v, &w, 8), v += 8;
	for (int i = 0; i < argc; i++) lx_copy_out(p, v, &a_str[i], 8), v += 8;
	w = 0;
	lx_copy_out(p, v, &w, 8), v += 8;
	for (int i = 0; i < envc; i++) lx_copy_out(p, v, &e_str[i], 8), v += 8;
	lx_copy_out(p, v, &w, 8), v += 8;
	lx_copy_out(p, v, aux, sizeof(aux));
	return sp;
}

int lx_exec(const char *path, const struct rd_file *f, const char *args, struct task *starter)
{
	const struct ehdr *h = (const void *)f->data;
	if (f->size < sizeof(*h) || memcmp(h->ident, "\177ELF\2\1", 6) || h->machine != 183 ||
	    (h->type != 2 && h->type != 3) || h->phentsize != sizeof(struct phdr) ||
	    h->phoff + (u64)h->phnum * sizeof(struct phdr) > f->size) {
		kprintf("%s: not a static AArch64 Linux program\n", path);
		return -31;
	}
	const struct phdr *ph = (const void *)(f->data + h->phoff);
	u64 bias = h->type == 3 ? PIE_BASE : 0, phdr_va = 0, top = 0;
	for (int i = 0; i < h->phnum; i++)
		if (ph[i].type == PT_INTERP) {
			kprintf("%s: a dynamically linked program (only static ones run)\n", path);
			return -31;
		}
	struct lx_proc *p = lx_proc_new();
	struct task *t = p ? task_alloc() : NULL;
	struct lx_thread *l = t ? thread_new() : NULL;
	if (!l) {
		if (t) t->state = T_FREE;
		if (p) lx_proc_free_memory(p);
		return -30;
	}
	for (int i = 0; i < h->phnum; i++) {
		const struct phdr *s = &ph[i];
		if (s->type != PT_LOAD || !s->memsz) continue;
		u64 va = s->vaddr + bias, start = va & ~4095ul, end = ALIGN_UP(va + s->memsz, 4096);
		if (!lx_user_range(start, end - start) || end > LX_STACK_TOP - LX_STACK_SIZE ||
		    s->offset + s->filesz > f->size) {
			kprintf("%s: a segment at %p does not fit\n", path, (addr_t)va);
			lx_proc_free_memory(p);
			t->state = T_FREE;
			lx_page_free((addr_t)l);
			return -31;
		}
		/* written first, its protection after */
		lx_vma_add(p, start, end, PROT_READ | PROT_WRITE);
		lx_copy_out(p, va, f->data + s->offset, s->filesz);
		u32 prot = (s->flags & 4 ? PROT_READ : 0) | (s->flags & 2 ? PROT_WRITE : 0) |
			   (s->flags & 1 ? PROT_EXEC : 0);
		lx_mprotect(p, start, end - start, prot);
		if (h->phoff >= s->offset && h->phoff < s->offset + s->filesz)
			phdr_va = va + h->phoff - s->offset;
		top = MAX(top, end);
	}
	p->brk_start = p->brk = top;
	lx_vma_add(p, LX_STACK_TOP - LX_STACK_SIZE, LX_STACK_TOP, PROT_READ | PROT_WRITE);
	l->sp = build_stack(p, path, args, h, bias, phdr_va);
	l->pc = h->entry + bias;
	l->pstate = 0;                          /* EL0t, AArch64, interrupts on */
	l->p = p;
	p->threads = 1;
	p->pid = t->pid;
	p->starter = starter;
	if (lx_std_files(p, starter != NULL) < 0) {
		lx_proc_free_memory(p);
		t->state = T_FREE;
		lx_page_free((addr_t)l);
		return -30;
	}
	t->lx = l;
	t->l1 = p->root;
	const char *base = path;
	for (const char *c = path; *c; c++)
		if (*c == '/') base = c + 1;
	strncpy0(t->name, base, sizeof(t->name));
	t->state = T_READY;
	kprintf("started %s (Linux, AArch64): pid %u, entry %p\n", t->name, t->pid, (addr_t)l->pc);
	return t->pid;
}

/* clone(CLONE_VM | CLONE_THREAD | ...): a thread in the same process */
#define CLONE_VM 0x100
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID 0x1000000

long lx_clone(struct task *t, u64 flags, u64 stack, u64 ptid, u64 tls, u64 ctid)
{
	struct lx_thread *l = t->lx;
	if (!(flags & CLONE_VM)) return -ENOSYS;           /* no fork */
	struct task *n = task_alloc();
	struct lx_thread *nl = n ? thread_new() : NULL;
	if (!nl) {
		if (n) n->state = T_FREE;
		return -EAGAIN;
	}
	memcpy(nl->x, l->x, sizeof(nl->x));
	nl->x[0] = 0;
	nl->sp = stack ? stack : l->sp;
	nl->pc = l->pc;
	nl->pstate = l->pstate;
	nl->tpidr = flags & CLONE_SETTLS ? tls : l->tpidr;
	nl->fpcr = l->fpcr;
	nl->p = l->p;
	nl->sigmask = l->sigmask;
	u32 tid = n->pid;
	if (flags & CLONE_PARENT_SETTID) lx_copy_out(l->p, ptid, &tid, 4);
	if (flags & CLONE_CHILD_SETTID) lx_copy_out(l->p, ctid, &tid, 4);
	if (flags & CLONE_CHILD_CLEARTID) nl->clear_tid = ctid;
	l->p->threads++;
	n->lx = nl;
	n->l1 = l->p->root;
	memcpy(n->name, t->name, sizeof(n->name));
	n->state = T_READY;
	return tid;
}

/* a thread ends: task_exit calls this. The last one ends the process. */
void lx_thread_gone(struct task *t)
{
	struct lx_thread *l = t->lx;
	struct lx_proc *p = l->p;
	t->lx = NULL;
	t->l1 = NULL;
	if (fp_owner == l) fp_owner = NULL;
	if (l->clear_tid && !p->exited) {
		u32 z = 0;
		lx_copy_out(p, l->clear_tid, &z, 4);
		lx_futex_wake_all(p, l->clear_tid, 1);
	}
	lx_page_free((addr_t)l);
	if (--p->threads > 0) return;
	kprintf("%s (Linux, pid %u): ended, code %d\n", t->name, p->pid, p->exit_code);
	lx_files_close_all(p);
	if (p->starter) lx_bridge_ended(p);
	if ((SYSREG_R(ttbr0_el1) & 0x0000fffffffff000ul) == (u64)p->root) mmu_switch(NULL);
	mmu_asid_forget(p->root);
	lx_proc_free_memory(p);
}

void lx_exit_group(struct lx_proc *p, int code)
{
	if (!p->exited) p->exit_code = code;
	p->exited = true;
	for (int i = 1; i < MAX_TASKS; i++)
		if (tasks[i].state != T_FREE && tasks[i].lx && tasks[i].lx->p == p)
			task_exit(&tasks[i]);
}

/* the way in: an exception from EL0 in AArch64 */
struct task *lx_trap(struct frame64 *f, bool irq, u64 esr, u64 far)
{
	struct task *t = current;
	struct lx_thread *l = t ? t->lx : NULL;
	if (!l) panic("an AArch64 exception without a Linux thread");
	memcpy(l->x, f->x, sizeof(l->x));
	l->pc = f->elr;
	l->pstate = f->spsr;
	l->sp = SYSREG_R(sp_el0);
	l->tpidr = SYSREG_R(tpidr_el0);
	if (irq) {
		plat->irq();
	} else {
		u32 ec = esr >> 26, fsc = esr & 0x3f;
		if (ec == 0x15) {                                   /* SVC */
			l->retry = false;
			long r = lx_syscall(t);
			if (r == LX_BLOCK) {
				t->state = T_LX_WAIT;
				l->retry = true;
			} else if (t->lx == l) {
				l->x[0] = r;
				l->deadline = 0;
				l->woken = false;
				l->futex = 0;
			}
		} else if ((ec == 0x24 || ec == 0x20) && fsc >= 4 && fsc <= 11 &&
			   lx_fault(l->p, far, ec == 0x24 && (esr & (1 << 6)), ec == 0x20)) {
			/* a page made present: the instruction again */
		} else {
			kprintf("%s (Linux, pid %u, thread %u): exception class %x (ESR %x) at pc %p, "
				"address %p, sp %p, lr %p: ended\n", t->name, l->p->pid, t->pid, ec,
				(u32)esr, (addr_t)l->pc, (addr_t)far, (addr_t)l->sp, (addr_t)l->x[30]);
			lx_exit_group(l->p, 128 + 11);
		}
	}
	schedule();
	return current;
}

/* the way out: the thread's registers into the frame trap_return loads */
void lx_restore(struct frame64 *f, struct task *t)
{
	struct lx_thread *l = t->lx;
	memcpy(f->x, l->x, sizeof(l->x));
	f->elr = l->pc;
	f->spsr = l->pstate;
	SYSREG_W(sp_el0, l->sp);
	SYSREG_W(tpidr_el0, l->tpidr);
	fp_lx_enter();                     /* a program's FP registers saved, not trapped */
	if (fp_owner != l) {
		if (fp_owner) lx_fp_save(fp_owner->fp, &fp_owner->fpcr);
		lx_fp_load(l->fp, &l->fpcr);
		fp_owner = l;
	}
}

/* the scheduler: a waiting thread's call once more */
bool lx_retry(struct task *t)
{
	struct lx_thread *l = t->lx;
	long r = lx_syscall(t);
	if (r == LX_BLOCK) return false;
	if (t->lx != l) return true;                        /* it ended */
	l->x[0] = r;
	l->retry = false;
	l->deadline = 0;
	l->woken = false;
	l->futex = 0;
	t->state = T_READY;
	return true;
}
