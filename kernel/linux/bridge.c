/* KolibriOS for ARM: function 101, Linux programs for KolibriOS programs
 * (docs/messengers.md): one is started with its standard input and
 * output on pipes, which the KolibriOS program writes and reads; output
 * arriving, and the end, post event 8 (EV_NETWORK) to it. Its standard
 * error goes to the kernel's console. */
#include "lx.h"

#define EAX r[0]
#define EBX r[1]
#define ECX r[2]
#define EDX r[3]
#define ESI r[4]

struct handle {
	struct task *owner;
	u32 owner_pid;
	struct lx_proc *p;
	struct lx_file *in, *out;
	bool ended;
	int code;
};
static struct handle hs[16];

void lx_pipe_notify(struct lx_file *f, struct task *t);
bool lx_pipe_closed(struct lx_file *f);

/* a Linux path's file on the ramdisk: /bin/x is linux/bin/x */
const struct rd_file *lx_rd_file(const char *path)
{
	char name[64];
	strncpy0(name, "/sys/linux", sizeof(name));
	strncpy0(name + 10, path, sizeof(name) - 10);
	return path[0] == '/' ? fs_find(name) : NULL;
}

static struct lx_proc *proc_of(u32 pid)
{
	for (int i = 1; i < MAX_TASKS; i++)
		if (tasks[i].state != T_FREE && tasks[i].pid == pid && tasks[i].lx) return tasks[i].lx->p;
	return NULL;
}

static struct handle *handle_of(struct task *t, u32 i)
{
	struct handle *h = i < ARRAY_SIZE(hs) ? &hs[i] : NULL;
	return h && h->owner == t && h->owner_pid == t->pid ? h : NULL;
}

void lx_bridge_ended(struct lx_proc *p)
{
	for (u32 i = 0; i < ARRAY_SIZE(hs); i++)
		if (hs[i].owner && hs[i].p == p) {
			hs[i].ended = true;
			hs[i].code = p->exit_code;
			hs[i].p = NULL;
			task_post_event(hs[i].owner, EV_NETWORK);
		}
}

static void handle_close(struct handle *h)
{
	if (h->p) lx_exit_group(h->p, 128 + 15);
	if (h->in) lx_file_put(h->in);
	if (h->out) {
		lx_pipe_notify(h->out, NULL);
		lx_file_put(h->out);
	}
	memset(h, 0, sizeof(*h));
}

void sys_linux(struct task *t, u32 *r)
{
	struct handle *h = EBX == 1 ? NULL : handle_of(t, ECX);
	if (EBX != 1 && !h) {
		EAX = -1;
		return;
	}
	switch (EBX) {
	case 1: {                               /* start */
		const char *path = ustr(t, ECX), *args = EDX ? ustr(t, EDX) : NULL;
		const struct rd_file *f = path ? lx_rd_file(path) : NULL;
		u32 i = 0;
		while (i < ARRAY_SIZE(hs) && hs[i].owner) i++;
		if (!f || i == ARRAY_SIZE(hs)) {
			EAX = -1;
			return;
		}
		int pid = lx_exec(path, f, args, t);
		struct lx_proc *p = pid > 0 ? proc_of(pid) : NULL;
		if (!p) {
			EAX = -1;
			return;
		}
		h = &hs[i];
		h->owner = t;
		h->owner_pid = t->pid;
		h->p = p;
		h->in = p->pipe_in;             /* the process's stdin, our end */
		h->out = p->pipe_out;
		lx_pipe_notify(h->out, t);
		EAX = i;
		return;
	}
	case 2: {                               /* write its stdin */
		void *b = uptr(t, EDX, ESI);
		long n = b ? lx_file_write_kernel(h->in, b, ESI) : -EFAULT;
		EAX = n >= 0 ? (u32)n : n == -EAGAIN ? 0 : (u32)-1;
		return;
	}
	case 3: {                               /* read its stdout */
		void *b = uptr(t, EDX, ESI);
		long n = b ? lx_file_read_kernel(h->out, b, ESI) : -EFAULT;
		if (n == 0 || n == -EAGAIN) EAX = lx_pipe_closed(h->out) ? (u32)-1 : 0;
		else EAX = n > 0 ? (u32)n : (u32)-1;
		return;
	}
	case 4:                                 /* status */
		EAX = h->ended;
		EBX = h->code;
		return;
	case 5:                                 /* stop, forget */
		handle_close(h);
		EAX = 0;
		return;
	default:
		EAX = -1;
	}
}
