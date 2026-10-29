/* KolibriOS for ARM: the Linux personality's files.
 *
 * One tree: a RAM file system (directories and files people write:
 * /tmp, /home/user, ...), the ramdisk's "linux/..." files read-only under
 * it (/bin/yappari-wa is the ramdisk's linux/bin/yappari-wa), and a few
 * made-up ones: /dev/null, /dev/zero, /dev/urandom, /etc/resolv.conf (the
 * DHCP lease's name server), /etc/hosts. The RAM file system is lost when
 * KolibriOS ends. Then pipes, eventfd, epoll, and the sockets (sock.c). */
#include "lx.h"
#include "sock.h"
#include "net.h"

enum { F_CONSOLE = 1, F_NULL, F_ZERO, F_RANDOM, F_TEXT, F_RD, F_RAM, F_DIR, F_PIPE_R, F_PIPE_W,
       F_EVENTFD, F_EPOLL, F_SOCK };

#define O_ACCMODE 3
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 040000
#define O_CLOEXEC 02000000
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000

#define EPOLLIN 1
#define EPOLLOUT 4
#define EPOLLERR 8
#define EPOLLHUP 0x10
#define EPOLLRDHUP 0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)

#define PATH_MAX 256
#define offsetof(t, m) __builtin_offsetof(t, m)
#define PIPE_SIZE 65536

struct rnode {
	char name[56];
	struct rnode *parent;
	bool used, dir, unlinked;
	int opens;
	u64 size, mtime;
	addr_t *idx;                    /* 512 pages of 512 data pages */
};

struct pipe {
	u32 r, used;
	int readers, writers;
	u32 gen;
	struct task *notify;            /* a KolibriOS program reading it (function 101) */
	u8 buf[PIPE_SIZE];
};

struct ep_item {
	struct lx_file *f;
	int fd;
	u32 events, seen;
	u64 data;
};
#define EP_ITEMS 127
struct epoll {
	int n;
	struct ep_item it[EP_ITEMS];
};

struct lx_file {
	int type, refs;
	u32 flags;
	u64 pos;
	union {
		const struct rd_file *rd;
		struct rnode *node;
		struct pipe *pipe;
		struct epoll *ep;
		struct lx_sock *sock;
		struct { u64 count; u32 gen; bool sem; } ev;
	};
	char path[PATH_MAX];            /* directories, made-up text */
	u32 text_len;
};

#define MAX_FILES 512
#define MAX_NODES 1024
static struct lx_file files[MAX_FILES];
static struct rnode nodes[MAX_NODES];
static struct rnode *root;
static struct epoll *epolls[16];

/* nodes of the RAM file system */
static struct rnode *node_new(struct rnode *parent, const char *name, bool dir)
{
	for (int i = 0; i < MAX_NODES; i++) {
		struct rnode *n = &nodes[i];
		if (n->used) continue;
		memset(n, 0, sizeof(*n));
		n->used = true;
		n->dir = dir;
		n->parent = parent;
		n->mtime = rtc_seconds();
		strncpy0(n->name, name, sizeof(n->name));
		return n;
	}
	return NULL;
}

static struct rnode *node_child(struct rnode *dir, const char *name, u32 len)
{
	for (int i = 0; i < MAX_NODES; i++) {
		struct rnode *n = &nodes[i];
		if (n->used && !n->unlinked && n->parent == dir && strlen(n->name) == len &&
		    !strncmp(n->name, name, len))
			return n;
	}
	return NULL;
}

static u8 *node_page(struct rnode *n, u64 index, bool make)
{
	if (index >= 512 * 512) return NULL;
	if (!n->idx) {
		if (!make) return NULL;
		n->idx = (addr_t *)lx_page_alloc();
		if (!n->idx) return NULL;
	}
	addr_t *l2 = (addr_t *)n->idx[index / 512];
	if (!l2) {
		if (!make || !(l2 = (addr_t *)lx_page_alloc())) return NULL;
		n->idx[index / 512] = (addr_t)l2;
	}
	if (!l2[index % 512]) {
		if (!make || !(l2[index % 512] = lx_page_alloc())) return NULL;
	}
	return (u8 *)l2[index % 512];
}

static void node_truncate(struct rnode *n, u64 size)
{
	if (n->idx) {
		u64 keep = (size + 4095) / 4096;
		for (u64 i = 0; i < 512; i++) {
			addr_t *l2 = (addr_t *)n->idx[i];
			if (!l2) continue;
			for (u64 j = 0; j < 512; j++)
				if (l2[j] && i * 512 + j >= keep) lx_page_free(l2[j]), l2[j] = 0;
			if (i * 512 >= keep) lx_page_free((addr_t)l2), n->idx[i] = 0;
		}
		if (!keep) lx_page_free((addr_t)n->idx), n->idx = NULL;
	}
	if (size < n->size && (size & 4095)) {
		u8 *pg = node_page(n, size / 4096, false);
		if (pg) memset(pg + (size & 4095), 0, 4096 - (size & 4095));
	}
	n->size = size;
}

static void node_free(struct rnode *n)
{
	node_truncate(n, 0);
	n->used = false;
}

static long node_read(struct rnode *n, void *buf, u64 len, u64 off)
{
	if (off >= n->size) return 0;
	len = MIN(len, n->size - off);
	for (u64 done = 0; done < len;) {
		u64 o = off + done, k = MIN(len - done, 4096 - (o & 4095));
		u8 *pg = node_page(n, o / 4096, false);
		if (pg) memcpy((u8 *)buf + done, pg + (o & 4095), k);
		else memset((u8 *)buf + done, 0, k);
		done += k;
	}
	return len;
}

static long node_write(struct rnode *n, const void *buf, u64 len, u64 off)
{
	for (u64 done = 0; done < len;) {
		u64 o = off + done, k = MIN(len - done, 4096 - (o & 4095));
		u8 *pg = node_page(n, o / 4096, true);
		if (!pg) return done ? (long)done : -ENOSPC;
		memcpy(pg + (o & 4095), (const u8 *)buf + done, k);
		done += k;
	}
	if (off + len > n->size) n->size = off + len;
	n->mtime = rtc_seconds();
	return len;
}

void lx_fs_init(void)
{
	root = node_new(NULL, "", true);
	static const char *const dirs[] = { "tmp", "home", "var", "run", "etc", "dev", "root" };
	for (u32 i = 0; i < ARRAY_SIZE(dirs); i++) node_new(root, dirs[i], true);
	struct rnode *home = node_child(root, "home", 4);
	node_new(home, "user", true);
	node_new(node_child(root, "var", 3), "tmp", true);
}

/* paths: made absolute, "." and ".." resolved */
static int normalize(const char *base, const char *path, char *out)
{
	char buf[2 * PATH_MAX];
	u32 n = 0;
	if (path[0] != '/') {
		strncpy0(buf, base, PATH_MAX);
		n = strlen(buf);
		buf[n++] = '/';
	}
	strncpy0(buf + n, path, sizeof(buf) - n);
	u32 o = 0;
	out[o++] = '/';
	for (const char *s = buf; *s;) {
		while (*s == '/') s++;
		const char *e = s;
		while (*e && *e != '/') e++;
		u32 l = e - s;
		if (!l || (l == 1 && s[0] == '.')) {
		} else if (l == 2 && s[0] == '.' && s[1] == '.') {
			while (o > 1 && out[o - 1] != '/') o--;
			if (o > 1) o--;
		} else {
			if (o > 1) out[o++] = '/';
			if (o + l >= PATH_MAX) return -ENAMETOOLONG;
			memcpy(out + o, s, l);
			o += l;
		}
		s = e;
	}
	out[o] = 0;
	return 0;
}

/* what a path names */
struct look {
	struct rnode *node;             /* the RAM file system's */
	const struct rd_file *rd;       /* the ramdisk's */
	bool rd_dir;                    /* a directory of ramdisk files */
	int special;                    /* F_NULL ... F_TEXT */
	struct rnode *parent;           /* for a new one: the directory (RAM), its name */
	const char *last;
};

static bool rd_dir_exists(const char *path)
{
	char pre[PATH_MAX + 8];
	strncpy0(pre, "linux", sizeof(pre));
	strncpy0(pre + 5, path, sizeof(pre) - 5);
	u32 l = strlen(pre);
	if (pre[l - 1] != '/') pre[l++] = '/', pre[l] = 0;
	for (int i = 0; i < fs_count(); i++)
		if (!strncmp(fs_get(i)->name, pre, l)) return true;
	return false;
}

static const struct rd_file *rd_lookup(const char *path)
{
	char name[PATH_MAX + 8];
	strncpy0(name, "linux", sizeof(name));
	strncpy0(name + 5, path, sizeof(name) - 5);
	for (int i = 0; i < fs_count(); i++)
		if (!strcmp(fs_get(i)->name, name)) return fs_get(i);
	return NULL;
}

/* a new name whose directory is not in the RAM file system: read-only
 * when the directory is the ramdisk's, else it is not there (mkdir -p,
 * as Rust's create_dir_all, goes on with the parent only on ENOENT) */
static long no_parent(const char *path)
{
	char d[PATH_MAX];
	strncpy0(d, path, sizeof(d));
	int l = strlen(d);
	while (l > 1 && d[l - 1] == '/') d[--l] = 0;
	while (l > 0 && d[l - 1] != '/') d[--l] = 0;
	while (l > 1 && d[l - 1] == '/') d[--l] = 0;
	return l && rd_dir_exists(d) ? -EROFS : -ENOENT;
}

static void look_up(const char *path, struct look *lk)
{
	memset(lk, 0, sizeof(*lk));
	static const struct { const char *p; int t; } special[] = {
		{ "/dev/null", F_NULL }, { "/dev/zero", F_ZERO }, { "/dev/urandom", F_RANDOM },
		{ "/dev/random", F_RANDOM }, { "/etc/resolv.conf", F_TEXT }, { "/etc/hosts", F_TEXT },
		{ "/dev/stdout", F_CONSOLE }, { "/dev/stderr", F_CONSOLE },
	};
	for (u32 i = 0; i < ARRAY_SIZE(special); i++)
		if (!strcmp(path, special[i].p)) { lk->special = special[i].t; return; }
	struct rnode *n = root;
	const char *s = path;
	while (n) {
		while (*s == '/') s++;
		if (!*s) { lk->node = n; return; }
		const char *e = s;
		while (*e && *e != '/') e++;
		struct rnode *c = n->dir ? node_child(n, s, e - s) : NULL;
		if (!c) {
			const char *rest = e;
			while (*rest == '/') rest++;
			if (!*rest && n->dir) { lk->parent = n; lk->last = s; }
			break;
		}
		n = c;
		s = e;
	}
	lk->rd = rd_lookup(path);
	if (!lk->rd) lk->rd_dir = rd_dir_exists(path);
}

static bool look_exists(const struct look *lk)
{
	return lk->node || lk->rd || lk->rd_dir || lk->special;
}

/* files and descriptors */
static struct lx_file *file_new(int type)
{
	for (int i = 0; i < MAX_FILES; i++) {
		struct lx_file *f = &files[i];
		if (f->refs) continue;
		memset(f, 0, offsetof(struct lx_file, path));
		f->path[0] = 0;
		f->type = type;
		f->refs = 1;
		return f;
	}
	return NULL;
}

struct lx_file *lx_file_get(struct lx_file *f)
{
	f->refs++;
	return f;
}

static void epoll_forget(struct lx_file *f)
{
	for (u32 i = 0; i < ARRAY_SIZE(epolls); i++) {
		struct epoll *e = epolls[i];
		if (!e) continue;
		for (int j = 0; j < e->n; j++)
			if (e->it[j].f == f) e->it[j--] = e->it[--e->n];
	}
}

void lx_file_put(struct lx_file *f)
{
	if (--f->refs > 0) return;
	epoll_forget(f);
	switch (f->type) {
	case F_RAM:
		if (--f->node->opens == 0 && f->node->unlinked) node_free(f->node);
		break;
	case F_PIPE_R:
	case F_PIPE_W: {
		struct pipe *p = f->pipe;
		if (f->type == F_PIPE_R) p->readers--; else p->writers--;
		p->gen++;
		if (p->notify && f->type == F_PIPE_W) task_post_event(p->notify, EV_NETWORK);
		if (!p->readers && !p->writers) mm_free_mb((addr_t)p, 1);
		break;
	}
	case F_EPOLL:
		for (u32 i = 0; i < ARRAY_SIZE(epolls); i++)
			if (epolls[i] == f->ep) epolls[i] = NULL;
		lx_page_free((addr_t)f->ep);
		break;
	case F_SOCK:
		sock_close(f->sock);
		break;
	}
	f->refs = 0;
}

int lx_fd_install(struct lx_proc *p, struct lx_file *f, bool cloexec)
{
	for (int i = 0; i < LX_MAX_FD; i++)
		if (!p->fd[i]) {
			p->fd[i] = f;
			p->cloexec[i] = cloexec;
			return i;
		}
	lx_file_put(f);
	return -EMFILE;
}

struct lx_file *lx_fd_file(struct lx_proc *p, int fd)
{
	return fd >= 0 && fd < LX_MAX_FD ? p->fd[fd] : NULL;
}

void lx_files_close_all(struct lx_proc *p)
{
	for (int i = 0; i < LX_MAX_FD; i++)
		if (p->fd[i]) lx_file_put(p->fd[i]), p->fd[i] = NULL;
}

struct lx_file *lx_pipe_new(struct lx_file **read_end)
{
	struct pipe *pp = (struct pipe *)mm_alloc_mb(1);
	if (!pp) return NULL;
	memset(pp, 0, offsetof(struct pipe, buf));
	struct lx_file *r = file_new(F_PIPE_R), *w = r ? file_new(F_PIPE_W) : NULL;
	if (!w) {
		if (r) r->refs = 0;
		mm_free_mb((addr_t)pp, 1);
		return NULL;
	}
	r->pipe = w->pipe = pp;
	pp->readers = pp->writers = 1;
	*read_end = r;
	return w;
}

int lx_std_files(struct lx_proc *p, bool pipes)
{
	struct lx_file *in, *out, *err = file_new(F_CONSOLE);
	if (pipes) {
		struct lx_file *in_r, *out_r;
		p->pipe_in = lx_pipe_new(&in_r);
		struct lx_file *out_w = p->pipe_in ? lx_pipe_new(&out_r) : NULL;
		if (!out_w || !err) return -1;
		in = in_r;
		out = out_w;
		p->pipe_out = out_r;
	} else {
		in = file_new(F_NULL);
		out = file_new(F_CONSOLE);
		if (!in || !out || !err) return -1;
	}
	p->fd[0] = in;
	p->fd[1] = out;
	p->fd[2] = err;
	return 0;
}

u64 lx_file_size(struct lx_file *f)
{
	switch (f->type) {
	case F_RD: return f->rd->size;
	case F_RAM: return f->node->size;
	case F_TEXT: return f->text_len;
	default: return 0;
	}
}

long lx_file_pread(struct lx_file *f, void *buf, u64 len, u64 off)
{
	switch (f->type) {
	case F_RD:
		if (off >= f->rd->size) return 0;
		len = MIN(len, f->rd->size - off);
		memcpy(buf, f->rd->data + off, len);
		return len;
	case F_TEXT:
		if (off >= f->text_len) return 0;
		len = MIN(len, f->text_len - off);
		memcpy(buf, f->path + off, len);
		return len;
	case F_RAM:
		return node_read(f->node, buf, len, off);
	default:
		return -ESPIPE;
	}
}

/* reading and writing a kernel buffer; LX_BLOCK when a blocking one has to wait */
static long file_read(struct lx_file *f, void *buf, u64 len, u64 *pos)
{
	bool nb = f->flags & O_NONBLOCK;
	switch (f->type) {
	case F_NULL: case F_CONSOLE: return 0;
	case F_ZERO: memset(buf, 0, len); return len;
	case F_RANDOM: lx_random(buf, len); return len;
	case F_RD: case F_TEXT: case F_RAM: {
		long n = lx_file_pread(f, buf, len, *pos);
		if (n > 0) *pos += n;
		return n;
	}
	case F_DIR: return -EISDIR;
	case F_PIPE_R: {
		struct pipe *p = f->pipe;
		if (!p->used) return p->writers ? (nb ? -EAGAIN : LX_BLOCK) : 0;
		u32 n = MIN(len, (u64)p->used);
		for (u32 i = 0; i < n; i++) ((u8 *)buf)[i] = p->buf[(p->r + i) % PIPE_SIZE];
		p->r = (p->r + n) % PIPE_SIZE;
		p->used -= n;
		p->gen++;
		return n;
	}
	case F_EVENTFD: {
		if (len < 8) return -EINVAL;
		if (!f->ev.count) return nb ? -EAGAIN : LX_BLOCK;
		u64 v = f->ev.sem ? 1 : f->ev.count;
		f->ev.count -= v;
		f->ev.gen++;
		memcpy(buf, &v, 8);
		return 8;
	}
	case F_SOCK: return sock_read(f->sock, buf, len, nb);
	default: return -EINVAL;
	}
}

static long file_write(struct lx_file *f, const void *buf, u64 len, u64 *pos)
{
	bool nb = f->flags & O_NONBLOCK;
	switch (f->type) {
	case F_NULL: case F_ZERO: case F_RANDOM: return len;
	case F_CONSOLE:
		for (u64 i = 0; i < len; i++) kputc(((const char *)buf)[i]);
		return len;
	case F_RAM: {
		u64 o = f->flags & O_APPEND ? f->node->size : *pos;
		long n = node_write(f->node, buf, len, o);
		if (n > 0) *pos = o + n;
		return n;
	}
	case F_RD: case F_TEXT: case F_DIR: return -EBADF;
	case F_PIPE_W: {
		struct pipe *p = f->pipe;
		if (!p->readers) return -EPIPE;
		if (p->used == PIPE_SIZE) return nb ? -EAGAIN : LX_BLOCK;
		u32 n = MIN(len, (u64)(PIPE_SIZE - p->used));
		for (u32 i = 0; i < n; i++) p->buf[(p->r + p->used + i) % PIPE_SIZE] = ((const u8 *)buf)[i];
		p->used += n;
		p->gen++;
		if (p->notify) task_post_event(p->notify, EV_NETWORK);
		return n;
	}
	case F_EVENTFD: {
		u64 v;
		if (len < 8) return -EINVAL;
		memcpy(&v, buf, 8);
		if (v == ~0ull) return -EINVAL;
		f->ev.count += v;
		f->ev.gen++;
		return 8;
	}
	case F_SOCK: return sock_write(f->sock, buf, len, nb);
	default: return -EINVAL;
	}
}

long lx_file_read_kernel(struct lx_file *f, void *buf, u64 len)
{
	u64 pos = 0;
	u32 saved = f->flags;
	f->flags |= O_NONBLOCK;
	long n = file_read(f, buf, len, &pos);
	f->flags = saved;
	return n;
}

long lx_file_write_kernel(struct lx_file *f, const void *buf, u64 len)
{
	u64 pos = 0;
	u32 saved = f->flags;
	f->flags |= O_NONBLOCK;
	long n = file_write(f, buf, len, &pos);
	f->flags = saved;
	return n;
}

void lx_pipe_notify(struct lx_file *f, struct task *t)
{
	if (f && (f->type == F_PIPE_R || f->type == F_PIPE_W)) f->pipe->notify = t;
}

bool lx_pipe_closed(struct lx_file *f)
{
	return f->type == F_PIPE_R ? !f->pipe->writers && !f->pipe->used : !f->pipe->readers;
}

static u32 file_poll(struct lx_file *f, u32 *gen)
{
	*gen = 0;
	switch (f->type) {
	case F_PIPE_R:
		*gen = f->pipe->gen;
		return (f->pipe->used ? EPOLLIN : 0) | (f->pipe->writers ? 0 : EPOLLHUP);
	case F_PIPE_W:
		*gen = f->pipe->gen;
		return (f->pipe->used < PIPE_SIZE ? EPOLLOUT : 0) | (f->pipe->readers ? 0 : EPOLLERR);
	case F_EVENTFD:
		*gen = f->ev.gen;
		return (f->ev.count ? EPOLLIN : 0) | EPOLLOUT;
	case F_SOCK:
		return sock_poll(f->sock, gen);
	default:
		return EPOLLIN | EPOLLOUT;
	}
}

/* system calls on user buffers, in pieces of a page */
static long rw_user(struct task *t, struct lx_file *f, u64 ubuf, u64 len, u64 *pos, bool write)
{
	struct lx_proc *p = t->lx->p;
	u8 kb[4096];
	u64 done = 0;
	while (done < len) {
		u64 k = MIN(len - done, sizeof(kb));
		long n;
		if (write) {
			if (lx_copy_in(p, kb, ubuf + done, k)) return done ? (long)done : -EFAULT;
			n = file_write(f, kb, k, pos);
		} else {
			n = file_read(f, kb, k, pos);
			if (n > 0 && lx_copy_out(p, ubuf + done, kb, n)) return done ? (long)done : -EFAULT;
		}
		if (n <= 0 || n == LX_BLOCK) return done ? (long)done : n;
		done += n;
		if ((u64)n < k) break;
	}
	return done;
}

static long rw_vec(struct task *t, struct lx_file *f, u64 iov, int cnt, u64 *pos, bool write)
{
	u64 total = 0;
	for (int i = 0; i < cnt; i++) {
		u64 v[2];
		if (lx_copy_in(t->lx->p, v, iov + 16 * i, 16)) return total ? (long)total : -EFAULT;
		long n = rw_user(t, f, v[0], v[1], pos, write);
		if (n < 0 || n == LX_BLOCK) return total ? (long)total : n;
		total += n;
		if ((u64)n < v[1]) break;
	}
	return total;
}

/* preadv, pwritev and their -v2 (offset -1: the file's position; the
 * flags RWF_APPEND and RWF_NOAPPEND, the others only hints here). On a
 * 64-bit kernel the offset is a[3] alone (pos_from_hilo). */
#define RWF_APPEND 0x10
#define RWF_NOAPPEND 0x20
#define RWF_KNOWN 0xff

static long sys_prwv(struct task *t, struct lx_file *f, int nr, u64 *a)
{
	bool write = nr == 70 || nr == 287, v2 = nr >= 286;
	s64 off = (s64)a[3];
	u32 rwf = v2 ? (u32)a[5] : 0;
	if (rwf & ~RWF_KNOWN) return -EOPNOTSUPP;
	if (v2 && off == -1) return rw_vec(t, f, a[1], (int)a[2], &f->pos, write);
	if (f->type != F_RD && f->type != F_RAM && f->type != F_TEXT) return -ESPIPE;
	if (off < 0) return -EINVAL;
	u64 pos = (u64)off;
	u32 was = f->flags;
	if (rwf & RWF_NOAPPEND) f->flags &= ~O_APPEND;
	if (rwf & RWF_APPEND) f->flags |= O_APPEND;
	long r = rw_vec(t, f, a[1], (int)a[2], &pos, write);
	f->flags = was;
	return r;
}

static int user_path(struct task *t, int dirfd, u64 upath, char *out)
{
	struct lx_proc *p = t->lx->p;
	char raw[PATH_MAX];
	int n = lx_str_in(p, raw, upath, sizeof(raw));
	if (n < 0) return n;
	const char *base = p->cwd;
	if (raw[0] != '/' && dirfd != AT_FDCWD) {
		struct lx_file *d = lx_fd_file(p, dirfd);
		if (!d) return -EBADF;
		if (d->type != F_DIR) return -ENOTDIR;
		base = d->path;
	}
	return normalize(base, raw, out);
}

static void make_text(struct lx_file *f, const char *path)
{
	char *s = f->path;
	if (!strcmp(path, "/etc/hosts")) {
		strncpy0(s, "127.0.0.1 localhost\n::1 localhost\n", PATH_MAX);
	} else {
		u32 d = net_dns(0);
		strncpy0(s, "nameserver ", PATH_MAX);
		char *e = s + strlen(s);
		for (int i = 0; i < 4; i++) {
			u32 b = (d >> (8 * i)) & 255;
			if (b >= 100) *e++ = '0' + b / 100;
			if (b >= 10) *e++ = '0' + b / 10 % 10;
			*e++ = '0' + b % 10;
			*e++ = i < 3 ? '.' : '\n';
		}
		strncpy0(e, "options attempts:2 timeout:3\n", PATH_MAX - (e - s));
	}
	f->text_len = strlen(s);
}

static long sys_openat(struct task *t, int dirfd, u64 upath, u32 flags)
{
	struct lx_proc *p = t->lx->p;
	char path[PATH_MAX];
	int e = user_path(t, dirfd, upath, path);
	if (e) return e;
	struct look lk;
	look_up(path, &lk);
	bool wr = (flags & O_ACCMODE) != 0;
	struct lx_file *f = NULL;
	if (!look_exists(&lk)) {
		if (!(flags & O_CREAT)) return -ENOENT;
		if (!lk.parent) return no_parent(path);
		struct rnode *n = node_new(lk.parent, lk.last, false);
		if (!n) return -ENOSPC;
		look_up(path, &lk);
	} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
		return -EEXIST;
	}
	if (lk.special) {
		f = file_new(lk.special);
		if (f && lk.special == F_TEXT) make_text(f, path);
	} else if (lk.node && lk.node->dir) {
		if (wr) return -EISDIR;
		f = file_new(F_DIR);
		if (f) strncpy0(f->path, path, PATH_MAX);
	} else if (lk.node) {
		if (flags & O_DIRECTORY) return -ENOTDIR;
		f = file_new(F_RAM);
		if (f) {
			f->node = lk.node;
			lk.node->opens++;
			if ((flags & O_TRUNC) && wr) node_truncate(lk.node, 0);
		}
	} else if (lk.rd_dir) {
		if (wr) return -EISDIR;
		f = file_new(F_DIR);
		if (f) strncpy0(f->path, path, PATH_MAX);
	} else {
		if (wr) return -EROFS;
		if (flags & O_DIRECTORY) return -ENOTDIR;
		f = file_new(F_RD);
		if (f) f->rd = lk.rd;
	}
	if (!f) return -ENFILE;
	f->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
	return lx_fd_install(p, f, flags & O_CLOEXEC);
}

/* struct stat of AArch64 (asm-generic) */
struct kstat {
	u64 dev, ino;
	u32 mode, nlink, uid, gid;
	u64 rdev, pad1;
	s64 size;
	s32 blksize, pad2;
	s64 blocks;
	s64 atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
	u32 unused[2];
};

static void fill_stat(struct kstat *s, int type, u64 size, u64 ino, u64 mtime)
{
	memset(s, 0, sizeof(*s));
	s->dev = 1;
	s->ino = ino;
	s->nlink = 1;
	s->uid = s->gid = 1000;
	s->blksize = 4096;
	s->size = size;
	s->blocks = (size + 511) / 512;
	s->atime = s->mtime = s->ctime = mtime;
	switch (type) {
	case F_DIR: s->mode = 0040755; s->nlink = 2; break;
	case F_RD: s->mode = 0100755; break;
	case F_RAM: case F_TEXT: s->mode = 0100644; break;
	case F_PIPE_R: case F_PIPE_W: s->mode = 0010600; break;
	case F_SOCK: s->mode = 0140777; break;
	case F_CONSOLE: s->mode = 0020620; s->rdev = 0x8801; break;
	default: s->mode = 0020666; s->rdev = 0x103; break;    /* character devices */
	}
}

static void stat_file(struct lx_file *f, struct kstat *s)
{
	u64 ino = (u64)(f - files) + 100000;
	switch (f->type) {
	case F_RAM: fill_stat(s, F_RAM, f->node->size, f->node - nodes + 2, f->node->mtime); break;
	case F_RD: fill_stat(s, F_RD, f->rd->size, (addr_t)f->rd & 0xffffff, 0); break;
	default: fill_stat(s, f->type, lx_file_size(f), ino, 0); break;
	}
}

static long sys_fstatat(struct task *t, int dirfd, u64 upath, u64 ust, u32 flags)
{
	struct lx_proc *p = t->lx->p;
	struct kstat s;
	char path[PATH_MAX];
	char first;
	if (lx_copy_in(p, &first, upath, 1)) return -EFAULT;
	if (!first && (flags & AT_EMPTY_PATH)) {
		struct lx_file *f = lx_fd_file(p, dirfd);
		if (!f) return -EBADF;
		stat_file(f, &s);
		return lx_copy_out(p, ust, &s, sizeof(s));
	}
	int e = user_path(t, dirfd, upath, path);
	if (e) return e;
	struct look lk;
	look_up(path, &lk);
	if (lk.node) fill_stat(&s, lk.node->dir ? F_DIR : F_RAM, lk.node->size, lk.node - nodes + 2, lk.node->mtime);
	else if (lk.rd) fill_stat(&s, F_RD, lk.rd->size, (addr_t)lk.rd & 0xffffff, 0);
	else if (lk.rd_dir) fill_stat(&s, F_DIR, 0, 1, 0);
	else if (lk.special) fill_stat(&s, lk.special, 0, 3, 0);
	else return -ENOENT;
	return lx_copy_out(p, ust, &s, sizeof(s));
}

/* getdents64: the RAM file system's entries, then the ramdisk's */
static long sys_getdents(struct task *t, struct lx_file *f, u64 ubuf, u64 len)
{
	struct lx_proc *p = t->lx->p;
	if (f->type != F_DIR) return -ENOTDIR;
	struct look lk;
	look_up(f->path, &lk);
	char names[64][56];
	u8 types[64];
	int n = 0;
	if (lk.node)
		for (int i = 0; i < MAX_NODES && n < 64; i++)
			if (nodes[i].used && !nodes[i].unlinked && nodes[i].parent == lk.node && &nodes[i] != root) {
				strncpy0(names[n], nodes[i].name, 56);
				types[n++] = nodes[i].dir ? 4 : 8;
			}
	char pre[PATH_MAX + 8];
	strncpy0(pre, "linux", sizeof(pre));
	strncpy0(pre + 5, f->path, sizeof(pre) - 5);
	u32 pl = strlen(pre);
	if (pre[pl - 1] != '/') pre[pl++] = '/', pre[pl] = 0;
	for (int i = 0; i < fs_count() && n < 64; i++) {
		const char *nm = fs_get(i)->name;
		if (strncmp(nm, pre, pl)) continue;
		const char *rest = nm + pl, *sl = rest;
		while (*sl && *sl != '/') sl++;
		char one[56];
		strncpy0(one, rest, MIN((u32)(sl - rest) + 1, sizeof(one)));
		bool dup = false;
		for (int j = 0; j < n; j++) if (!strcmp(names[j], one)) dup = true;
		if (dup) continue;
		strncpy0(names[n], one, 56);
		types[n++] = *sl ? 4 : 8;
	}
	u64 off = 0;
	u8 rec[80];
	for (int i = (int)f->pos; i < n; i++) {
		u32 nl = strlen(names[i]);
		u32 rl = ALIGN_UP(19 + nl + 1, 8);
		if (off + rl > len) {
			if (!off) return -EINVAL;
			break;
		}
		memset(rec, 0, sizeof(rec));
		u64 ino = 1000 + i;
		s64 next = i + 1;
		memcpy(rec, &ino, 8);
		memcpy(rec + 8, &next, 8);
		rec[16] = rl & 255;
		rec[17] = rl >> 8;
		rec[18] = types[i];
		memcpy(rec + 19, names[i], nl);
		if (lx_copy_out(p, ubuf + off, rec, rl)) return -EFAULT;
		off += rl;
		f->pos = i + 1;
	}
	return off;
}

static long sys_unlinkat(struct task *t, int dirfd, u64 upath, u32 flags)
{
	char path[PATH_MAX];
	int e = user_path(t, dirfd, upath, path);
	if (e) return e;
	struct look lk;
	look_up(path, &lk);
	if (!lk.node) return lk.rd || lk.rd_dir ? -EROFS : -ENOENT;
	if (lk.node == root) return -EBUSY;
	if (flags & AT_REMOVEDIR) {
		if (!lk.node->dir) return -ENOTDIR;
		for (int i = 0; i < MAX_NODES; i++)
			if (nodes[i].used && !nodes[i].unlinked && nodes[i].parent == lk.node) return -ENOTEMPTY;
	} else if (lk.node->dir) {
		return -EISDIR;
	}
	if (lk.node->opens) lk.node->unlinked = true;
	else node_free(lk.node);
	return 0;
}

static long sys_mkdirat(struct task *t, int dirfd, u64 upath)
{
	char path[PATH_MAX];
	int e = user_path(t, dirfd, upath, path);
	if (e) return e;
	struct look lk;
	look_up(path, &lk);
	if (look_exists(&lk)) return -EEXIST;
	if (!lk.parent) return no_parent(path);
	return node_new(lk.parent, lk.last, true) ? 0 : -ENOSPC;
}

static long sys_renameat(struct task *t, int od, u64 op, int nd, u64 np)
{
	char from[PATH_MAX], to[PATH_MAX];
	int e = user_path(t, od, op, from);
	if (!e) e = user_path(t, nd, np, to);
	if (e) return e;
	struct look a, b;
	look_up(from, &a);
	if (!a.node) return a.rd ? -EROFS : -ENOENT;
	look_up(to, &b);
	if (b.node == a.node) return 0;
	if (b.node) {
		if (b.node->dir) return -EISDIR;
		struct rnode *parent = b.node->parent;
		char name[56];
		strncpy0(name, b.node->name, sizeof(name));
		if (b.node->opens) b.node->unlinked = true;
		else node_free(b.node);
		a.node->parent = parent;
		strncpy0(a.node->name, name, sizeof(a.node->name));
		return 0;
	}
	if (!b.parent) return b.rd ? -EROFS : -ENOENT;
	a.node->parent = b.parent;
	strncpy0(a.node->name, b.last, sizeof(a.node->name));
	return 0;
}

/* epoll */
static long sys_epoll_ctl(struct task *t, int epfd, int op, int fd, u64 uev)
{
	struct lx_proc *p = t->lx->p;
	struct lx_file *e = lx_fd_file(p, epfd), *f = lx_fd_file(p, fd);
	if (!e || !f) return -EBADF;
	if (e->type != F_EPOLL) return -EINVAL;
	if (f->type != F_PIPE_R && f->type != F_PIPE_W && f->type != F_EVENTFD && f->type != F_SOCK)
		return -EPERM;                      /* regular files: as Linux */
	struct epoll *ep = e->ep;
	u32 ev[4];
	if (op != 2 && lx_copy_in(p, ev, uev, 16)) return -EFAULT;
	int i;
	for (i = 0; i < ep->n; i++)
		if (ep->it[i].f == f && ep->it[i].fd == fd) break;
	switch (op) {
	case 1:                                 /* ADD */
		if (i < ep->n) return -EEXIST;
		if (ep->n >= EP_ITEMS) return -ENOSPC;
		ep->n++;
		/* fall through */
	case 3:                                 /* MOD */
		if (i >= ep->n) return -ENOENT;
		ep->it[i].f = f;
		ep->it[i].fd = fd;
		ep->it[i].events = ev[0];
		memcpy(&ep->it[i].data, &ev[2], 8);
		ep->it[i].seen = ~0u;
		return 0;
	case 2:                                 /* DEL */
		if (i >= ep->n) return -ENOENT;
		ep->it[i] = ep->it[--ep->n];
		return 0;
	}
	return -EINVAL;
}

static long sys_epoll_wait(struct task *t, int epfd, u64 uev, int max, int timeout)
{
	struct lx_thread *l = t->lx;
	struct lx_file *e = lx_fd_file(l->p, epfd);
	if (!e || e->type != F_EPOLL) return -EBADF;
	if (max <= 0) return -EINVAL;
	struct epoll *ep = e->ep;
	int n = 0;
	for (int i = 0; i < ep->n && n < max; i++) {
		struct ep_item *it = &ep->it[i];
		if (!it->events) continue;          /* a one-shot that fired */
		u32 gen, cur = file_poll(it->f, &gen) & (it->events | EPOLLERR | EPOLLHUP);
		if (!cur) continue;
		if (it->events & EPOLLET) {
			if (gen == it->seen) continue;
			it->seen = gen;
		}
		u32 rec[4] = { cur, 0, (u32)it->data, (u32)(it->data >> 32) };
		if (lx_copy_out(l->p, uev + 16 * n, rec, 16)) return -EFAULT;
		if (it->events & EPOLLONESHOT) it->events = 0;
		n++;
	}
	if (n) return n;
	if (l->retry) return lx_timed_out(l) ? 0 : LX_BLOCK;
	if (!timeout) return 0;
	if (timeout > 0) lx_set_deadline(l, (u64)timeout * 1000000ull);
	return LX_BLOCK;
}

/* ppoll, for musl's poll() */
static long sys_ppoll(struct task *t, u64 ufds, u32 nfds, u64 uts)
{
	struct lx_thread *l = t->lx;
	int n = 0;
	for (u32 i = 0; i < nfds && i < 64; i++) {
		struct { s32 fd; s16 events, revents; } pf;
		if (lx_copy_in(l->p, &pf, ufds + 8 * i, 8)) return -EFAULT;
		struct lx_file *f = lx_fd_file(l->p, pf.fd);
		u32 gen, cur = f ? file_poll(f, &gen) : 0x20;        /* POLLNVAL */
		pf.revents = cur & (pf.events | EPOLLERR | EPOLLHUP | 0x20);
		if (pf.revents) n++;
		if (lx_copy_out(l->p, ufds + 8 * i + 6, &pf.revents, 2)) return -EFAULT;
	}
	if (n) return n;
	if (l->retry) return lx_timed_out(l) ? 0 : LX_BLOCK;
	if (uts) {
		s64 ts[2];
		if (lx_copy_in(l->p, ts, uts, 16)) return -EFAULT;
		if (!ts[0] && !ts[1]) return 0;
		lx_set_deadline(l, ts[0] * 1000000000ull + ts[1]);
	}
	return LX_BLOCK;
}

static long sys_fcntl(struct lx_proc *p, int fd, int cmd, u64 arg)
{
	struct lx_file *f = lx_fd_file(p, fd);
	if (!f) return -EBADF;
	switch (cmd) {
	case 0:                                 /* F_DUPFD */
	case 1030:                              /* F_DUPFD_CLOEXEC */
		for (int i = (int)arg; i < LX_MAX_FD; i++)
			if (!p->fd[i]) {
				p->fd[i] = lx_file_get(f);
				p->cloexec[i] = cmd == 1030;
				return i;
			}
		return -EMFILE;
	case 1: return p->cloexec[fd];          /* F_GETFD */
	case 2: p->cloexec[fd] = arg & 1; return 0;
	case 3: return f->flags;                /* F_GETFL */
	case 4:                                 /* F_SETFL */
		f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK));
		return 0;
	case 5: {                               /* F_GETLK: never locked by another */
		s16 unlck = 2;
		return lx_copy_out(p, arg, &unlck, 2);
	}
	case 6: case 7: case 36: case 37: case 38:     /* locks: granted, one process */
		return 0;
	case 1031: return PIPE_SIZE;            /* F_GETPIPE_SZ */
	default: return -EINVAL;
	}
}

long lx_file_syscall(struct task *t, int nr, u64 *a, bool *handled)
{
	struct lx_thread *l = t->lx;
	struct lx_proc *p = l->p;
	struct lx_file *f = (int)a[0] >= 0 && (int)a[0] < LX_MAX_FD ? p->fd[(int)a[0]] : NULL;
	*handled = true;
	switch (nr) {
	case 63:                                /* read */
		return f ? rw_user(t, f, a[1], a[2], &f->pos, false) : -EBADF;
	case 64:                                /* write */
		return f ? rw_user(t, f, a[1], a[2], &f->pos, true) : -EBADF;
	case 65: return f ? rw_vec(t, f, a[1], (int)a[2], &f->pos, false) : -EBADF;  /* readv */
	case 66: return f ? rw_vec(t, f, a[1], (int)a[2], &f->pos, true) : -EBADF;   /* writev */
	case 67: case 68: {                     /* pread64, pwrite64 */
		if (!f) return -EBADF;
		if (f->type != F_RD && f->type != F_RAM && f->type != F_TEXT) return -ESPIPE;
		u64 pos = a[3];
		return rw_user(t, f, a[1], a[2], &pos, nr == 68);
	}
	case 69: case 70:                       /* preadv, pwritev */
	case 286: case 287:                     /* preadv2, pwritev2 (musl's pwrite: RWF_NOAPPEND) */
		return f ? sys_prwv(t, f, nr, a) : -EBADF;
	case 56: return sys_openat(t, (int)a[0], a[1], (u32)a[2]);
	case 57:                                /* close */
		if (!f) return -EBADF;
		p->fd[(int)a[0]] = NULL;
		lx_file_put(f);
		return 0;
	case 62: {                              /* lseek */
		if (!f) return -EBADF;
		if (f->type != F_RD && f->type != F_RAM && f->type != F_TEXT && f->type != F_DIR) return -ESPIPE;
		s64 base = a[2] == 0 ? 0 : a[2] == 1 ? (s64)f->pos : a[2] == 2 ? (s64)lx_file_size(f) : -1;
		if (base < 0 || base + (s64)a[1] < 0) return -EINVAL;
		f->pos = base + (s64)a[1];
		return f->pos;
	}
	case 79: return sys_fstatat(t, (int)a[0], a[1], a[2], (u32)a[3]);
	case 80: {                              /* fstat */
		if (!f) return -EBADF;
		struct kstat s;
		stat_file(f, &s);
		return lx_copy_out(p, a[1], &s, sizeof(s));
	}
	case 48: case 439: {                    /* faccessat, faccessat2 */
		char path[PATH_MAX];
		int e = user_path(t, (int)a[0], a[1], path);
		if (e) return e;
		struct look lk;
		look_up(path, &lk);
		if (!look_exists(&lk)) return -ENOENT;
		return (a[2] & 2) && (lk.rd || lk.rd_dir) ? -EROFS : 0;
	}
	case 78: {                              /* readlinkat: no links here */
		char path[PATH_MAX];
		int e = user_path(t, (int)a[0], a[1], path);
		if (e) return e;
		if (!strcmp(path, "/proc/self/exe")) return -ENOENT;
		struct look lk;
		look_up(path, &lk);
		return look_exists(&lk) ? -EINVAL : -ENOENT;
	}
	case 61: return f ? sys_getdents(t, f, a[1], a[2]) : -EBADF;
	case 34: return sys_mkdirat(t, (int)a[0], a[1]);
	case 35: return sys_unlinkat(t, (int)a[0], a[1], (u32)a[2]);
	case 38: return sys_renameat(t, (int)a[0], a[1], (int)a[2], a[3]);
	case 276: return sys_renameat(t, (int)a[0], a[1], (int)a[2], a[3]);   /* renameat2 */
	case 37: case 36: return -EPERM;        /* linkat, symlinkat */
	case 46:                                /* ftruncate */
		if (!f) return -EBADF;
		if (f->type != F_RAM) return -EINVAL;
		node_truncate(f->node, a[1]);
		return 0;
	case 82: case 83: case 81:              /* fsync, fdatasync, sync */
		return 0;
	case 52: case 53: case 54: case 55: case 88: case 32:   /* fchmod(at), fchown(at), utimensat, flock */
		return 0;
	case 47: return 0;                      /* fallocate */
	case 43: case 44: {                     /* statfs, fstatfs */
		u64 s[15] = { 0x858458f6, 4096, 1u << 18, 1u << 17, 1u << 17, 1024, 512, 0, 255, 4096 };
		return lx_copy_out(p, a[1], s, sizeof(s));
	}
	case 17: {                              /* getcwd */
		u32 n = strlen(p->cwd) + 1;
		if (a[1] < n) return -ERANGE;
		return lx_copy_out(p, a[0], p->cwd, n) ? -EFAULT : (long)n;
	}
	case 49: {                              /* chdir */
		char path[PATH_MAX];
		int e = user_path(t, AT_FDCWD, a[0], path);
		if (e) return e;
		struct look lk;
		look_up(path, &lk);
		if (!(lk.node && lk.node->dir) && !lk.rd_dir) return look_exists(&lk) ? -ENOTDIR : -ENOENT;
		strncpy0(p->cwd, path, sizeof(p->cwd));
		return 0;
	}
	case 50:                                /* fchdir */
		if (!f) return -EBADF;
		if (f->type != F_DIR) return -ENOTDIR;
		strncpy0(p->cwd, f->path, sizeof(p->cwd));
		return 0;
	case 23:                                /* dup */
		return f ? sys_fcntl(p, (int)a[0], 0, 0) : -EBADF;
	case 24: {                              /* dup3 */
		int nf = (int)a[1];
		if (!f || nf < 0 || nf >= LX_MAX_FD) return -EBADF;
		if (nf == (int)a[0]) return -EINVAL;
		if (p->fd[nf]) lx_file_put(p->fd[nf]);
		p->fd[nf] = lx_file_get(f);
		p->cloexec[nf] = (a[2] & O_CLOEXEC) != 0;
		return nf;
	}
	case 25: return sys_fcntl(p, (int)a[0], (int)a[1], a[2]);
	case 29:                                /* ioctl */
		if (!f) return -EBADF;
		if (a[1] == 0x5421) {               /* FIONBIO */
			u32 on;
			if (lx_copy_in(p, &on, a[2], 4)) return -EFAULT;
			f->flags = on ? f->flags | O_NONBLOCK : f->flags & ~O_NONBLOCK;
			return 0;
		}
		if (a[1] == 0x541b && f->type == F_PIPE_R) {   /* FIONREAD */
			u32 n = f->pipe->used;
			return lx_copy_out(p, a[2], &n, 4);
		}
		return -ENOTTY;
	case 59: {                              /* pipe2 */
		struct lx_file *r, *w = lx_pipe_new(&r);
		if (!w) return -ENFILE;
		r->flags = w->flags = a[1] & O_NONBLOCK;
		w->flags |= O_WRONLY;
		int fr = lx_fd_install(p, r, a[1] & O_CLOEXEC);
		if (fr < 0) { lx_file_put(w); return fr; }
		int fw = lx_fd_install(p, w, a[1] & O_CLOEXEC);
		if (fw < 0) { p->fd[fr] = NULL; lx_file_put(r); return fw; }
		s32 fds[2] = { fr, fw };
		return lx_copy_out(p, a[0], fds, 8);
	}
	case 19: {                              /* eventfd2 */
		struct lx_file *e = file_new(F_EVENTFD);
		if (!e) return -ENFILE;
		e->ev.count = (u32)a[0];
		e->ev.sem = a[1] & 1;
		e->flags = (a[1] & O_NONBLOCK) | O_RDWR;
		return lx_fd_install(p, e, a[1] & O_CLOEXEC);
	}
	case 20: {                              /* epoll_create1 */
		struct lx_file *e = file_new(F_EPOLL);
		if (!e) return -ENFILE;
		e->ep = (struct epoll *)lx_page_alloc();
		u32 i = 0;
		while (i < ARRAY_SIZE(epolls) && epolls[i]) i++;
		if (!e->ep || i == ARRAY_SIZE(epolls)) {
			if (e->ep) lx_page_free((addr_t)e->ep);
			e->refs = 0;
			return -ENOMEM;
		}
		epolls[i] = e->ep;
		return lx_fd_install(p, e, a[0] & O_CLOEXEC);
	}
	case 21: return sys_epoll_ctl(t, (int)a[0], (int)a[1], (int)a[2], a[3]);
	case 22: return sys_epoll_wait(t, (int)a[0], a[1], (int)a[2], (int)a[3]);
	case 73: return sys_ppoll(t, a[0], (u32)a[1], a[2]);
	default:
		break;
	}
	long r = sock_syscall(t, nr, a, handled);
	return r;
}

/* sock.c's descriptors */
int lx_sock_install(struct lx_proc *p, struct lx_sock *s, u32 flags)
{
	struct lx_file *f = file_new(F_SOCK);
	if (!f) {
		sock_close(s);
		return -ENFILE;
	}
	f->sock = s;
	f->flags = (flags & O_NONBLOCK) | O_RDWR;
	return lx_fd_install(p, f, flags & O_CLOEXEC);
}

struct lx_sock *lx_fd_sock(struct lx_proc *p, int fd, int *err)
{
	struct lx_file *f = lx_fd_file(p, fd);
	*err = !f ? -EBADF : f->type != F_SOCK ? -ENOTSOCK : 0;
	return *err ? NULL : f->sock;
}

bool lx_fd_nonblock(struct lx_proc *p, int fd)
{
	struct lx_file *f = lx_fd_file(p, fd);
	return f && (f->flags & O_NONBLOCK);
}
