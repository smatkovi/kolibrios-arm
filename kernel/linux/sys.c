/* KolibriOS for ARM: the Linux personality's system calls (AArch64
 * numbers, asm-generic): x8 the number, x0-x5 the arguments, x0 the
 * result or -errno. Files, pipes, epoll and sockets are in file.c and
 * sock.c; this is memory, threads, futexes, time, signals (recorded, not
 * delivered) and the rest. */
#include "lx.h"
#include "net.h"
#include "rng.h"

extern void sched_yield(void);

static bool trace;

/* random numbers: the kernel's generator (rng.c); the first use by each
 * program said on the console, with what seeded it */
void lx_random_used(struct lx_proc *p, int how)
{
	if (p->rnd_said & 1u << how) return;
	p->rnd_said |= 1u << how;
	kprintf("lx: %s: %s from the kernel's generator, %s%s\n", p->name, how ? "/dev/(u)random" : "getrandom",
		rng_ready() ? "seeded from " : "", rng_ready() ? rng_sources() : "NOT seeded yet");
}

/* the real-time clock in ns: the RTC's seconds, the counter in between */
static s64 rt_offset;
static bool rt_valid;

static u64 realtime_ns(void)
{
	s64 rt = (s64)rtc_seconds() * 1000000000ll, m = lx_now_ns();
	if (!rt_valid || rt - (rt_offset + m) > 2000000000ll || rt_offset + m - rt > 2000000000ll) {
		rt_offset = rt - m;
		rt_valid = true;
	}
	return rt_offset + m;
}

struct ts { s64 sec, nsec; };

static long put_ts(struct lx_proc *p, u64 va, u64 ns)
{
	struct ts t = { ns / 1000000000ull, ns % 1000000000ull };
	return lx_copy_out(p, va, &t, sizeof(t));
}

static long get_ns(struct lx_proc *p, u64 va, s64 *ns)
{
	struct ts t;
	if (lx_copy_in(p, &t, va, sizeof(t))) return -EFAULT;
	if (t.nsec < 0 || t.nsec >= 1000000000ll || t.sec < 0) return -EINVAL;
	*ns = t.sec > 1000000000ll * 1000 ? (s64)1 << 62 : t.sec * 1000000000ll + t.nsec;
	return 0;
}

static u64 clock_ns(int clk)
{
	return clk == 0 || clk == 5 || clk == 8 ? realtime_ns() : lx_now_ns();
}

/* sleeping: the deadline on the first call, done once it passed */
static long sleep_until(struct lx_thread *l, s64 ns_from_now)
{
	if (l->retry) return lx_timed_out(l) ? 0 : LX_BLOCK;
	if (ns_from_now <= 0) return 0;
	lx_set_deadline(l, ns_from_now);
	return LX_BLOCK;
}

/* futexes: a waiter is a waiting thread with the address in its futex */
static int futex_wake(struct lx_proc *p, u64 addr, int n, u64 requeue_to, int nreq)
{
	int woken = 0, moved = 0;
	for (int i = 1; i < MAX_TASKS; i++) {
		struct task *t = &tasks[i];
		struct lx_thread *l = t->lx;
		if (t->state != T_LX_WAIT || !l || l->p != p || l->futex != addr || l->woken) continue;
		if (woken < n) {
			l->woken = true;
			l->futex = 0;
			woken++;
		} else if (requeue_to && moved < nreq) {
			l->futex = requeue_to;
			moved++;
		}
	}
	return woken + moved;
}

void lx_futex_wake_all(struct lx_proc *p, u64 addr, int n)
{
	futex_wake(p, addr, n, 0, 0);
}

static long sys_futex(struct task *t, u64 addr, int op, u32 val, u64 timeout, u64 addr2, u32 val3)
{
	struct lx_thread *l = t->lx;
	struct lx_proc *p = l->p;
	int cmd = op & 127;
	switch (cmd) {
	case 0:                                 /* WAIT */
	case 9: {                               /* WAIT_BITSET */
		if (l->retry) {
			if (l->woken) return 0;
			return lx_timed_out(l) ? -ETIMEDOUT : LX_BLOCK;
		}
		u32 cur;
		if (addr & 3) return -EINVAL;
		if (lx_copy_in(p, &cur, addr, 4)) return -EFAULT;
		if (cur != val) return -EAGAIN;
		if (timeout) {
			s64 ns;
			if (get_ns(p, timeout, &ns)) return -EINVAL;
			if (cmd == 9) ns -= (s64)clock_ns(op & 256 ? 0 : 1);   /* absolute */
			if (ns <= 0) return -ETIMEDOUT;
			lx_set_deadline(l, ns);
		}
		l->futex = addr;
		l->woken = false;
		return LX_BLOCK;
	}
	case 1:                                 /* WAKE */
	case 10:                                /* WAKE_BITSET */
		return futex_wake(p, addr, (int)val, 0, 0);
	case 3:                                 /* REQUEUE */
	case 4: {                               /* CMP_REQUEUE */
		if (cmd == 4) {
			u32 cur;
			if (lx_copy_in(p, &cur, addr, 4)) return -EFAULT;
			if (cur != val3) return -EAGAIN;
		}
		return futex_wake(p, addr, (int)val, addr2, (int)timeout);
	}
	default:
		return -ENOSYS;
	}
}

static long sys_uname(struct lx_proc *p, u64 va)
{
	static const char *const f[6] = { "Linux", "kolibrios", "6.1.0-kolibrios", "#1 KolibriOS for ARM",
					  "aarch64", "(none)" };
	char u[6 * 65];
	memset(u, 0, sizeof(u));
	for (int i = 0; i < 6; i++) strncpy0(u + 65 * i, f[i], 65);
	return lx_copy_out(p, va, u, sizeof(u));
}

static long sys_prlimit(struct lx_proc *p, int res, u64 old)
{
	u64 r[2] = { ~0ull, ~0ull };
	if (res == 7) r[0] = r[1] = LX_MAX_FD;                     /* NOFILE */
	if (res == 3) r[0] = LX_STACK_SIZE;                        /* STACK */
	return old ? lx_copy_out(p, old, r, sizeof(r)) : 0;
}

static long sys_sysinfo(struct lx_proc *p, u64 va)
{
	struct {
		s64 uptime;
		u64 loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
		u16 procs, pad;
		u32 pad2;
		u64 totalhigh, freehigh;
		u32 mem_unit;
		u8 f[4];
	} s;
	memset(&s, 0, sizeof(s));
	s.uptime = lx_now_ns() / 1000000000ull;
	s.totalram = (u64)mm_total_kb() << 10;
	s.freeram = (u64)mm_free_kb() << 10;
	s.procs = 1;
	s.mem_unit = 1;
	return lx_copy_out(p, va, &s, sizeof(s));
}

long lx_syscall(struct task *t)
{
	struct lx_thread *l = t->lx;
	struct lx_proc *p = l->p;
	u64 *a = l->x;
	int nr = (int)l->x[8];
	bool handled = false;
	long r = lx_file_syscall(t, nr, a, &handled);
	if (!handled) switch (nr) {
	case 93:                                /* exit */
		if (p->threads == 1) p->exit_code = (int)a[0];
		task_exit(t);
		r = 0;
		break;
	case 94:                                /* exit_group */
		lx_exit_group(p, (int)a[0]);
		r = 0;
		break;
	case 96:                                /* set_tid_address */
		l->clear_tid = a[0];
		r = t->pid;
		break;
	case 98:
		r = sys_futex(t, a[0], (int)a[1], (u32)a[2], a[3], a[4], (u32)a[5]);
		break;
	case 99:                                /* set_robust_list */
		r = 0;
		break;
	case 101: {                             /* nanosleep */
		s64 ns = 0;
		r = l->retry ? 0 : get_ns(p, a[0], &ns);
		if (!r) r = sleep_until(l, ns);
		break;
	}
	case 115: {                             /* clock_nanosleep */
		s64 ns = 0;
		r = l->retry ? 0 : get_ns(p, a[2], &ns);
		if (!r && !l->retry && (a[1] & 1)) ns -= (s64)clock_ns((int)a[0]);   /* TIMER_ABSTIME */
		if (!r) r = sleep_until(l, ns);
		break;
	}
	case 113:                               /* clock_gettime */
		r = put_ts(p, a[1], clock_ns((int)a[0]));
		break;
	case 114:                               /* clock_getres */
		r = a[1] ? put_ts(p, a[1], 1) : 0;
		break;
	case 169: {                             /* gettimeofday */
		u64 ns = realtime_ns();
		s64 tv[2] = { ns / 1000000000ull, ns % 1000000000ull / 1000 };
		r = a[0] ? lx_copy_out(p, a[0], tv, sizeof(tv)) : 0;
		break;
	}
	case 124:                               /* sched_yield */
		sched_yield();
		r = 0;
		break;
	case 122:                               /* sched_setaffinity */
		r = 0;
		break;
	case 123: {                             /* sched_getaffinity: one CPU */
		u64 m = 1;
		r = a[1] < 8 ? -EINVAL : lx_copy_out(p, a[2], &m, 8) ? -EFAULT : 8;
		break;
	}
	case 129: case 130: case 131:           /* kill, tkill, tgkill: not delivered */
		r = 0;
		break;
	case 132: {                             /* sigaltstack */
		u64 ss[3] = { 0, 2, 0 };            /* SS_DISABLE */
		r = a[1] ? lx_copy_out(p, a[1], ss, sizeof(ss)) : 0;
		break;
	}
	case 134: {                             /* rt_sigaction: recorded nowhere */
		u64 sa[4] = { 0 };
		r = a[2] ? lx_copy_out(p, a[2], sa, sizeof(sa)) : 0;
		break;
	}
	case 135: {                             /* rt_sigprocmask */
		u64 old = l->sigmask, set;
		r = 0;
		if (a[1]) {
			if (lx_copy_in(p, &set, a[1], 8)) { r = -EFAULT; break; }
			if (a[0] == 0) l->sigmask |= set;
			else if (a[0] == 1) l->sigmask &= ~set;
			else l->sigmask = set;
		}
		if (a[2]) r = lx_copy_out(p, a[2], &old, 8);
		break;
	}
	case 160:
		r = sys_uname(p, a[0]);
		break;
	case 163:                               /* getrlimit */
		r = sys_prlimit(p, (int)a[0], a[1]);
		break;
	case 261:                               /* prlimit64 */
		r = sys_prlimit(p, (int)a[1], a[3]);
		break;
	case 165: {                             /* getrusage */
		u8 z[144];
		memset(z, 0, sizeof(z));
		r = lx_copy_out(p, a[1], z, sizeof(z));
		break;
	}
	case 167:                               /* prctl: PR_SET_NAME, PR_GET_NAME */
		r = a[0] == 15 || a[0] == 16 ? 0 : -EINVAL;
		break;
	case 172: r = p->pid; break;            /* getpid */
	case 173: r = 1; break;                 /* getppid */
	case 174: case 175: case 176: case 177: /* getuid, geteuid, getgid, getegid */
		r = 1000;
		break;
	case 178: r = t->pid; break;            /* gettid */
	case 179: r = sys_sysinfo(p, a[0]); break;
	case 214: r = lx_brk(p, a[0]); break;
	case 215: r = lx_munmap(p, a[0], a[1]); break;
	case 216: r = -ENOMEM; break;           /* mremap: callers fall back */
	case 220:
		r = lx_clone(t, a[0], a[1], a[2], a[3], a[4]);
		break;
	case 222: {                             /* mmap */
		struct lx_file *f = NULL;
		if (!(a[3] & 0x20)) {               /* not anonymous */
			extern struct lx_file *lx_fd_file(struct lx_proc *p, int fd);
			f = lx_fd_file(p, (int)a[4]);
			if (!f) { r = -EBADF; break; }
		}
		r = lx_mmap(p, a[0], a[1], (u32)a[2] & 7, (u32)a[3], f, a[5]);
		break;
	}
	case 226: r = lx_mprotect(p, a[0], a[1], (u32)a[2] & 7); break;
	case 233: r = lx_madvise(p, a[0], a[1], (int)a[2]); break;
	case 232: r = 0; break;                 /* mincore */
	case 278: {                             /* getrandom */
		/* as Linux: GRND_NONBLOCK 1, GRND_RANDOM 2 (the same pool),
		 * GRND_INSECURE 4 (also before the seed), INSECURE with RANDOM
		 * refused; without INSECURE it waits for the seed (rng.c:
		 * a batch of jitter at each look), NONBLOCK: EAGAIN */
		u32 fl = (u32)a[2];
		if ((fl & ~7u) || (fl & 6) == 6) { r = -EINVAL; break; }
		if (!rng_ready() && !(fl & 4) && !rng_seed_step()) { r = fl & 1 ? -EAGAIN : LX_BLOCK; break; }
		lx_random_used(p, 0);
		u8 b[256];
		u64 done = 0, n = MIN(a[1], 1ul << 20);
		r = 0;
		while (done < n) {
			u32 k = MIN(n - done, sizeof(b));
			rng_bytes_insecure(b, k);
			if (lx_copy_out(p, a[0] + done, b, k)) { r = -EFAULT; break; }
			done += k;
		}
		memset(b, 0, sizeof(b));
		if (!r) r = done;
		break;
	}
	case 221: r = -EACCES; break;           /* execve */
	case 260: r = -ECHILD; break;           /* wait4 */
	case 435: r = -ENOSYS; break;           /* clone3: callers fall back to clone */
	case 283: r = -ENOSYS; break;           /* membarrier */
	case 293: r = -ENOSYS; break;           /* rseq */
	case 139:                               /* rt_sigreturn: no signal was delivered */
		kprintf("%s (Linux): rt_sigreturn without a signal\n", t->name);
		lx_exit_group(p, 128 + 11);
		r = 0;
		break;
	default:
		if (!l->retry) kprintf("%s (Linux): system call %d not there\n", t->name, nr);
		r = -ENOSYS;
		break;
	}
	if (trace && r != LX_BLOCK && t->lx == l)
		kprintf("[%u] %d(%x, %x, %x) = %d\n", t->pid, nr, (u32)a[0], (u32)a[1], (u32)a[2], (int)r);
	return r;
}

void lx_trace_on(void)
{
	trace = true;
}
