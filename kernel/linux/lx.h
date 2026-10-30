/* KolibriOS for ARM: the Linux personality of the AArch64 kernel: static
 * Linux AArch64 programs (ELF) at EL0 next to the KolibriOS programs, with
 * the system calls Go's runtime and musl need (docs/messengers.md).
 *
 * A process is its address space (4 KiB pages of its own, allocated on
 * first touch), its files and its threads; each thread is a kernel task
 * (struct task) with its registers here. The kernel keeps no stack per
 * task: a system call that has to wait returns LX_BLOCK, the thread waits
 * (T_LX_WAIT), and the scheduler calls it again (retry) until it has its
 * result. A call therefore blocks only before it has changed anything. */
#ifndef LX_H
#define LX_H

#include "task.h"
#include "rng.h"

#define LX_BLOCK (-4096l)              /* not a result: wait, call again */

/* the address space: the first GiB (the program, its heap, the main
 * thread's stack) and 16-256 GiB (mmap); the kernel's mappings elsewhere */
#define LX_LOW_END (1ul << 30)
#define LX_MAP_START (16ul << 30)
#define LX_MAP_END (256ul << 30)
#define LX_STACK_TOP LX_LOW_END
#define LX_STACK_SIZE (8ul << 20)

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define LX_MAX_VMAS 1024
#define LX_MAX_FD 256

struct vma {
	u64 start, end;
	u32 prot;
};

struct lx_file;

struct lx_proc {
	u64 *root;                      /* the L1 table */
	struct vma vma[LX_MAX_VMAS];
	int nvma;
	u64 brk_start, brk;
	u64 map_next;                   /* where mmap looks first */
	int threads;
	u32 pid;
	struct lx_file *fd[LX_MAX_FD];
	u8 cloexec[LX_MAX_FD];
	char cwd[128];
	struct task *starter;           /* a KolibriOS program to tell (function 101), or NULL */
	struct lx_file *pipe_in, *pipe_out;
	int exit_code;
	bool exited;
	char name[24];                  /* the program's file name, for messages */
	u8 rnd_said;                    /* lx_random_used: said once each */
};

struct lx_thread {
	u64 x[31], sp, pc, pstate, tpidr;
	u8 fp[512] __attribute__((aligned(16)));
	u64 fpcr, fpsr;
	struct lx_proc *p;
	u64 clear_tid;                  /* set_tid_address, CLONE_CHILD_CLEARTID */
	/* waiting */
	bool retry;                     /* the call runs again (after LX_BLOCK) */
	u64 deadline;                   /* the generic timer's count, 0: none */
	u64 futex;                      /* the address waited on */
	bool woken;                     /* by FUTEX_WAKE */
	u64 sigmask;
};

/* lx.c */
void lx_init(void);
int lx_exec(const char *path, const struct rd_file *f, const char *args, struct task *starter);
void lx_thread_gone(struct task *t);     /* from task_exit */
bool lx_retry(struct task *t);           /* from the scheduler: true when it can run again */
void lx_exit_group(struct lx_proc *p, int code);
long lx_clone(struct task *t, u64 flags, u64 stack, u64 ptid, u64 tls, u64 ctid);
void lx_bridge_ended(struct lx_proc *p);       /* bridge.c: tell the starter */
u64 lx_now_ns(void);                     /* monotonic */
u64 lx_ticks_of_ns(u64 ns);              /* the generic timer's count */
bool lx_timed_out(struct lx_thread *l);  /* its deadline passed */
void lx_set_deadline(struct lx_thread *l, u64 ns_from_now);

/* vm.c: memory */
addr_t lx_page_alloc(void);              /* a zeroed 4 KiB page, 0 if none */
void lx_page_free(addr_t pa);
struct lx_proc *lx_proc_new(void);
void lx_proc_free_memory(struct lx_proc *p);
long lx_mmap(struct lx_proc *p, u64 addr, u64 len, u32 prot, u32 flags, struct lx_file *f, u64 off);
long lx_munmap(struct lx_proc *p, u64 addr, u64 len);
long lx_mprotect(struct lx_proc *p, u64 addr, u64 len, u32 prot);
long lx_madvise(struct lx_proc *p, u64 addr, u64 len, int advice);
long lx_brk(struct lx_proc *p, u64 addr);
bool lx_vma_add(struct lx_proc *p, u64 start, u64 end, u32 prot);
bool lx_fault(struct lx_proc *p, u64 va, bool write, bool exec);
int lx_copy_in(struct lx_proc *p, void *dst, u64 va, u64 len);
int lx_copy_out(struct lx_proc *p, u64 va, const void *src, u64 len);
int lx_str_in(struct lx_proc *p, char *dst, u64 va, u32 size);
bool lx_user_range(u64 va, u64 len);

/* sys.c */
long lx_syscall(struct task *t);
void lx_futex_wake_all(struct lx_proc *p, u64 addr, int n);
void lx_random_used(struct lx_proc *p, int how);  /* sys.c: 0 getrandom, 1 /dev/urandom */

/* file.c */
void lx_fs_init(void);
long lx_file_syscall(struct task *t, int nr, u64 *a, bool *handled);
void lx_files_close_all(struct lx_proc *p);
int lx_fd_install(struct lx_proc *p, struct lx_file *f, bool cloexec);
int lx_std_files(struct lx_proc *p, bool pipes);
struct lx_file *lx_pipe_new(struct lx_file **read_end);
long lx_file_read_kernel(struct lx_file *f, void *buf, u64 len);   /* function 101 */
long lx_file_write_kernel(struct lx_file *f, const void *buf, u64 len);
void lx_file_put(struct lx_file *f);
struct lx_file *lx_file_get(struct lx_file *f);
u64 lx_file_size(struct lx_file *f);
/* function 70 of KolibriOS programs on the Linux tree (by absolute path) */
long lx_kfs_read(const char *path, void *buf, u64 len, u64 off, u64 *size);
long lx_kfs_write(const char *path, const void *buf, u64 len, u64 off, bool rewrite);
long lx_kfs_mkdir(const char *path);
long lx_kfs_info(const char *path, u64 *size, bool *dir);
long lx_file_pread(struct lx_file *f, void *buf, u64 len, u64 off);

/* the errors */
enum {
	EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, ENXIO = 6, E2BIG = 7, ENOEXEC = 8,
	EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EACCES = 13, EFAULT = 14, EBUSY = 16,
	EEXIST = 17, EXDEV = 18, ENODEV = 19, ENOTDIR = 20, EISDIR = 21, EINVAL = 22, ENFILE = 23,
	EMFILE = 24, ENOTTY = 25, EFBIG = 27, ENOSPC = 28, ESPIPE = 29, EROFS = 30, EPIPE = 32,
	ERANGE = 34, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39, ELOOP = 40,
	ENOTSOCK = 88, EDESTADDRREQ = 89, EMSGSIZE = 90, EPROTOTYPE = 91, ENOPROTOOPT = 92,
	EPROTONOSUPPORT = 93, EOPNOTSUPP = 95, EAFNOSUPPORT = 97, EADDRINUSE = 98,
	EADDRNOTAVAIL = 99, ENETDOWN = 100, ENETUNREACH = 101, ECONNABORTED = 103,
	ECONNRESET = 104, ENOBUFS = 105, EISCONN = 106, ENOTCONN = 107, ETIMEDOUT = 110,
	ECONNREFUSED = 111, EHOSTUNREACH = 113, EALREADY = 114, EINPROGRESS = 115,
};

#endif
