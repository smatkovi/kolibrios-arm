/* KolibriOS for ARM: KolibriOS's file systems, function 70 (files and
 * folders) and function 30 (the current folder), as sysfuncs.txt has
 * them.
 *
 * The volumes:
 *   /                 virtual: rd, tmp0 (and linux on AArch64)
 *   /rd/1 = /sys      the ramdisk: the files built into the kernel
 *                     (kernel/fs.c, tools/mkrd.py) as a tree of folders
 *                     (from the '/' in their names), writable as
 *                     KolibriOS's ramdisk is: a file keeps pointing at
 *                     the kernel's copy until it is written (copy on
 *                     write); what is written is lost at a reset
 *   /tmp0/1           an empty RAM disk (KolibriOS's tmpdisk's name)
 *   /linux/..., and any other absolute path on AArch64: the Linux
 *                     personality's tree (kernel/linux/file.c: /home/user,
 *                     /tmp, ...), in RAM too
 * The RAM disks keep their data in 4 KiB pages taken from the free
 * memory in blocks of 1 MiB (never given back, reused). Names compare
 * without regard to case and keep it, as FAT does. Paths are bytes:
 * cp866 or UTF-8 as the program writes them; a UTF-16 path (encoding
 * byte 2) is turned into UTF-8. */
#include "gui.h"

/* a program's buffers need not be aligned: words stored and read so the
 * compiler cannot merge them into strd/ldrd (which fault on ARMv7) */
static inline void put32(void *p, u32 v) { struct __attribute__((packed)) w { u32 v; } *q = p; q->v = v; }
static inline u32 get32(const void *p) { const struct __attribute__((packed)) w { u32 v; } *q = p; return q->v; }

#define NAME_MAX 256
#define PATH_MAX 1024
#define PTRS (4096 / sizeof(addr_t))
#define RESERVE_KB (16 * 1024)          /* the RAM disks leave this much free */

/* KolibriOS's error codes */
enum { E_OK = 0, E_UNSUPPORTED = 2, E_UNKNOWN_FS = 3, E_NOT_FOUND = 5, E_EOF = 6, E_POINTER = 7,
       E_DISK_FULL = 8, E_FS = 9, E_ACCESS = 10, E_DEVICE = 11, E_MEMORY = 12 };

#define A_RO 0x01
#define A_DIR 0x10
#define A_ARCH 0x20

struct knode {
	char name[NAME_MAX];
	struct knode *parent, *child, *next;   /* the tree: first child, next sibling */
	u32 attr;                              /* KolibriOS's attributes */
	u32 ctime, atime, mtime;               /* Unix times */
	u64 size;
	const u8 *ro;                          /* the kernel's ramdisk bytes, until written */
	addr_t *idx;                           /* an index of index pages of data pages */
};

static struct knode *free_nodes;
static struct knode rd_root, tmp_root;
static addr_t free_pages;

/* --- memory: nodes, pages -------------------------------------------- */

static struct knode *node_alloc(void)
{
	if (!free_nodes) {
		if (mm_free_kb() < RESERVE_KB) return NULL;
		struct knode *b = kmalloc(sizeof(struct knode) * 64);
		if (!b) return NULL;
		for (int i = 0; i < 64; i++) b[i].next = free_nodes, free_nodes = &b[i];
	}
	struct knode *n = free_nodes;
	free_nodes = n->next;
	memset(n, 0, sizeof(*n));
	return n;
}

static addr_t page_alloc(void)
{
	if (!free_pages) {
		if (mm_free_kb() < RESERVE_KB + 1024) return 0;
		addr_t b = mm_alloc_mb(1);
		if (!b) return 0;
		for (u32 o = 0; o < (1u << 20); o += 4096) {
			*(addr_t *)(b + o) = free_pages;
			free_pages = b + o;
		}
	}
	addr_t p = free_pages;
	free_pages = *(addr_t *)p;
	memset((void *)p, 0, 4096);
	return p;
}

static void page_free(addr_t p)
{
	*(addr_t *)p = free_pages;
	free_pages = p;
}

/* --- a node's data ------------------------------------------------------ */

static u8 *node_page(struct knode *n, u64 index, bool make)
{
	if (index >= (u64)PTRS * PTRS) return NULL;
	if (!n->idx) {
		if (!make || !(n->idx = (addr_t *)page_alloc())) return NULL;
	}
	addr_t *l2 = (addr_t *)n->idx[index / PTRS];
	if (!l2) {
		if (!make || !(l2 = (addr_t *)page_alloc())) return NULL;
		n->idx[index / PTRS] = (addr_t)l2;
	}
	if (!l2[index % PTRS]) {
		if (!make || !(l2[index % PTRS] = page_alloc())) return NULL;
	}
	return (u8 *)l2[index % PTRS];
}

/* the pages past size given back, the rest of the last one zeroed */
static void node_drop(struct knode *n, u64 size)
{
	if (n->idx) {
		u64 keep = (size + 4095) / 4096;
		for (u64 i = 0; i < PTRS; i++) {
			addr_t *l2 = (addr_t *)n->idx[i];
			if (!l2) continue;
			for (u64 j = 0; j < PTRS; j++)
				if (l2[j] && i * PTRS + j >= keep) page_free(l2[j]), l2[j] = 0;
			if (i * PTRS >= keep) page_free((addr_t)l2), n->idx[i] = 0;
		}
		if (!keep) page_free((addr_t)n->idx), n->idx = NULL;
	}
	if (size < n->size && (size & 4095)) {
		u8 *pg = node_page(n, size / 4096, false);
		if (pg) memset(pg + (size & 4095), 0, 4096 - (size & 4095));
	}
}

static u32 now(void) { return rtc_seconds(); }

/* a file still on the kernel's ramdisk bytes gets pages of its own */
static int node_own(struct knode *n)
{
	if (!n->ro) return E_OK;
	for (u64 o = 0; o < n->size; o += 4096) {
		u8 *pg = node_page(n, o / 4096, true);
		if (!pg) { node_drop(n, 0); return E_DISK_FULL; }
		memcpy(pg, n->ro + o, MIN(n->size - o, 4096));
	}
	n->ro = NULL;
	return E_OK;
}

static u32 node_read(struct knode *n, void *buf, u32 len, u64 off)
{
	if (off >= n->size) return 0;
	len = MIN(len, n->size - off);
	n->atime = now();
	if (n->ro) {
		memcpy(buf, n->ro + off, len);
		return len;
	}
	for (u32 done = 0; done < len;) {
		u64 o = off + done;
		u32 k = MIN(len - done, 4096 - (o & 4095));
		u8 *pg = node_page(n, o / 4096, false);
		if (pg) memcpy((u8 *)buf + done, pg + (o & 4095), k);
		else memset((u8 *)buf + done, 0, k);
		done += k;
	}
	return len;
}

/* the size set: cut (pages back) or grown (zeros) */
static int node_resize(struct knode *n, u64 size)
{
	int e = node_own(n);
	if (e) return e;
	if (size < n->size) node_drop(n, size);
	n->size = size;
	n->mtime = now();
	return E_OK;
}

/* written at off; *done the bytes written (fewer: the disk is full) */
static int node_write(struct knode *n, const void *buf, u32 len, u64 off, u32 *done)
{
	*done = 0;
	int e = node_own(n);
	if (e) return e;
	while (*done < len) {
		u64 o = off + *done;
		u32 k = MIN(len - *done, 4096 - (o & 4095));
		u8 *pg = node_page(n, o / 4096, true);
		if (!pg) break;
		memcpy(pg + (o & 4095), (const u8 *)buf + *done, k);
		*done += k;
	}
	if (off + *done > n->size) n->size = off + *done;
	n->mtime = now();
	return *done < len ? E_DISK_FULL : E_OK;
}

/* --- the tree ----------------------------------------------------------- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool name_is(const char *a, const char *b, u32 blen)
{
	u32 i = 0;
	for (; i < blen && a[i]; i++)
		if (lower((u8)a[i]) != lower((u8)b[i])) return false;
	return i == blen && !a[i];
}

static struct knode *child(struct knode *dir, const char *name, u32 len)
{
	for (struct knode *c = dir->child; c; c = c->next)
		if (name_is(c->name, name, len)) return c;
	return NULL;
}

static void node_link(struct knode *dir, struct knode *n)
{
	n->parent = dir;
	n->next = NULL;
	struct knode **p = &dir->child;
	while (*p) p = &(*p)->next;
	*p = n;
}

static void node_unlink(struct knode *n)
{
	struct knode **p = &n->parent->child;
	while (*p && *p != n) p = &(*p)->next;
	if (*p) *p = n->next;
	n->next = NULL;
}

static struct knode *node_new(struct knode *dir, const char *name, u32 len, bool folder)
{
	if (!len || len >= NAME_MAX) return NULL;
	struct knode *n = node_alloc();
	if (!n) return NULL;
	memcpy(n->name, name, len);
	n->name[len] = 0;
	n->attr = folder ? A_DIR : A_ARCH;
	n->ctime = n->atime = n->mtime = now();
	node_link(dir, n);
	return n;
}

static void node_free(struct knode *n)
{
	node_unlink(n);
	if (!n->ro) node_drop(n, 0);
	n->next = free_nodes;
	free_nodes = n;
}

/* a path inside a volume ("a/b/c", "" for its root): the node, or NULL
 * with *parent the folder it would be in (when that exists) and *last
 * the last name */
static struct knode *walk(struct knode *root, const char *rest, struct knode **parent, const char **last)
{
	struct knode *n = root;
	if (parent) *parent = NULL;
	for (const char *s = rest; *s;) {
		const char *e = s;
		while (*e && *e != '/') e++;
		struct knode *c = n->attr & A_DIR ? child(n, s, e - s) : NULL;
		if (!c) {
			if (!*e && (n->attr & A_DIR)) {
				if (parent) *parent = n;
				if (last) *last = s;
			}
			return NULL;
		}
		n = c;
		s = *e ? e + 1 : e;
	}
	return n;
}

static const char *find_char(const char *s, int c);

/* the built-in ramdisk as the tree of /rd/1 */
void kfs_init(void)
{
	rd_root.attr = tmp_root.attr = A_DIR;
	u32 t = now();
	rd_root.ctime = rd_root.mtime = tmp_root.ctime = tmp_root.mtime = t;
	for (int i = 0; i < fs_count(); i++) {
		const struct rd_file *f = fs_get(i);
		struct knode *dir = &rd_root;
		const char *s = f->name;
		for (const char *e; (e = find_char(s, '/')); s = e + 1) {
			struct knode *c = child(dir, s, e - s);
			if (!c) c = node_new(dir, s, e - s, true);
			if (!c) break;
			dir = c;
		}
		struct knode *n = node_new(dir, s, strlen(s), false);
		if (!n) break;
		n->ro = f->data;
		n->size = f->size;
	}
}

/* --- paths --------------------------------------------------------------- */

static const char *find_char(const char *s, int c)
{
	for (; *s; s++)
		if (*s == c) return s;
	return NULL;
}

/* the program's path as a kernel string: without its encoding byte (1
 * cp866, 3 UTF-8: bytes as they are; 2 UTF-16LE: made UTF-8) */
static int user_path(struct task *t, u32 va, char *out)
{
	const u8 *s = uptr(t, va, 1);
	if (!s) return E_POINTER;
	bool utf16 = false;
	u32 at = 0, o = 0;
	if (s[0] >= 1 && s[0] <= 3) utf16 = s[0] == 2, at = 1;
	else if (s[0] == '/' && uptr(t, va + 1, 1) && s[1] >= 1 && s[1] <= 3) {
		utf16 = s[1] == 2;
		at = 2;
		out[o++] = '/';
		if (!utf16 && uptr(t, va + 2, 1) && s[2] == '/') at = 3;
	}
	if (utf16) {
		if (at == 2) {                     /* "/" 2, then UTF-16 */
			const u16 *w0 = uptr(t, va + 2, 2);
			if (w0 && *w0 == '/') at = 4;
		}
		for (u32 i = 0;; i++) {
			const u8 *w = uptr(t, va + at + 2 * i, 2);
			if (!w) return E_POINTER;
			u32 c = w[0] | w[1] << 8;
			if (!c) break;
			if (o + 4 >= PATH_MAX) return E_NOT_FOUND;
			if (c < 0x80) out[o++] = c;
			else if (c < 0x800) out[o++] = 0xc0 | c >> 6, out[o++] = 0x80 | (c & 63);
			else out[o++] = 0xe0 | c >> 12, out[o++] = 0x80 | ((c >> 6) & 63), out[o++] = 0x80 | (c & 63);
		}
		out[o] = 0;
		return E_OK;
	}
	const char *p = ustr(t, va + at);
	if (!p) return E_POINTER;
	if (strlen(p) + o >= PATH_MAX) return E_NOT_FOUND;
	strncpy0(out + o, p, PATH_MAX - o);
	return E_OK;
}

/* an absolute path, "." and ".." resolved, no double '/'; relative to
 * the folder base */
static int absolute(const char *base, const char *path, char *out)
{
	char buf[2 * PATH_MAX];
	u32 n = 0;
	if (path[0] != '/') {
		strncpy0(buf, base && *base ? base : "/sys", PATH_MAX);
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
			if (o + l >= PATH_MAX) return E_NOT_FOUND;
			memcpy(out + o, s, l);
			o += l;
		}
		s = e;
	}
	out[o] = 0;
	return E_OK;
}

enum { V_NONE, V_ROOT, V_RDDEV, V_TMPDEV, V_TREE, V_LINUX };

/* which volume an absolute path is on; for a tree its root and the path
 * inside it, for the Linux tree its path there */
static int volume(const char *abs, struct knode **root, const char **rest)
{
	const char *s = abs + 1, *e = s;
	while (*e && *e != '/') e++;
	u32 l = e - s;
	const char *after = *e ? e + 1 : e;
	if (!l) return V_ROOT;
	if (name_is("sys", s, l)) {
		*root = &rd_root;
		*rest = after;
		return V_TREE;
	}
	if (name_is("rd", s, l) || name_is("tmp0", s, l)) {
		bool rd = l == 2;
		if (!*after) return rd ? V_RDDEV : V_TMPDEV;
		if (after[0] != '1' || (after[1] && after[1] != '/')) return V_NONE;
		*root = rd ? &rd_root : &tmp_root;
		*rest = after[1] ? after + 2 : after + 1;
		return V_TREE;
	}
#ifdef __aarch64__
	if (name_is("linux", s, l)) {
		*rest = *e ? e : "/";
		return V_LINUX;
	}
	*rest = abs;
	return V_LINUX;
#else
	return V_NONE;
#endif
}

/* --- the Linux personality's tree (kernel/linux/file.c) -------------------- */
#ifdef __aarch64__
long lx_kfs_read(const char *path, void *buf, u64 len, u64 off, u64 *size);
long lx_kfs_write(const char *path, const void *buf, u64 len, u64 off, bool rewrite);
long lx_kfs_mkdir(const char *path);
long lx_kfs_info(const char *path, u64 *size, bool *dir);
/* 0: the entry index, 1: past the last one, < 0: no such folder */
long lx_kfs_readdir(const char *path, u32 index, char *name, u32 namelen, bool *dir, u64 *size, u64 *mtime);
long lx_kfs_delete(const char *path);
long lx_kfs_rename(const char *from, const char *to);
long lx_kfs_truncate(const char *path, u64 size);

static u32 lx_error(long e)
{
	switch (-e) {
	case 2: case 20: return E_NOT_FOUND;   /* ENOENT, ENOTDIR */
	case 28: return E_DISK_FULL;           /* ENOSPC */
	case 12: return E_MEMORY;              /* ENOMEM */
	default: return E_ACCESS;              /* EROFS, EISDIR, ENOTEMPTY, ... */
	}
}
#endif

/* --- folder entries (BDFE) ------------------------------------------------ */

void civil(u32 t, int *y, int *mo, int *d, int *h, int *mi, int *s);

static void put_time(u8 *p, u32 t)
{
	if (!t) { memset(p, 0, 8); return; }
	int y, mo, d, h, mi, s;
	civil(t, &y, &mo, &d, &h, &mi, &s);
	p[0] = s; p[1] = mi; p[2] = h; p[3] = 0;
	p[4] = d; p[5] = mo; p[6] = y & 255; p[7] = y >> 8;
}

static u32 get_time(const u8 *p)
{
	int y = p[6] | p[7] << 8, mo = p[5], d = p[4];
	if (!d || !mo) return 0;
	/* days from the civil date (the inverse of civil) */
	y -= mo <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - era * 400;
	int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	s32 days = era * 146097 + doe - 719468;
	return (u32)days * 86400 + p[2] * 3600 + p[1] * 60 + p[0];
}

/* an entry of 40 bytes, the name after it (enc 2: UTF-16, 520 bytes;
 * else 264 bytes) */
static void bdfe(u8 *e, u32 attr, u64 size, u32 ct, u32 at, u32 mt, const char *name, u32 enc)
{
	memset(e, 0, 40);
	put32(e, attr);
	put32(e + 4, enc ? enc : 1);
	put_time(e + 8, ct);
	put_time(e + 16, at);
	put_time(e + 24, mt);
	memcpy(e + 32, &size, 8);
	if (!name) return;
	if (enc == 2) {
		u16 *w = (u16 *)(e + 40);
		u32 i = 0;
		for (const u8 *p = (const u8 *)name; *p && i < 259;) {
			u32 c = *p++;
			if (c >= 0xc0 && (*p & 0xc0) == 0x80) {          /* UTF-8 */
				if (c < 0xe0) c = (c & 31) << 6 | (*p++ & 63);
				else if ((p[1] & 0xc0) == 0x80) c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63), p += 2;
			}
			w[i++] = c;
		}
		w[i] = 0;
	} else {
		strncpy0((char *)e + 40, name, 264);
	}
}

/* --- function 70 ----------------------------------------------------------- */

#define EAX r[0]
#define EBX r[1]

/* the virtual folders' entries */
static const char *virtual_entry(int vol, u32 i)
{
	static const char *const root[] = { "rd", "tmp0",
#ifdef __aarch64__
		"linux",
#endif
	};
	if (vol == V_ROOT) return i < ARRAY_SIZE(root) ? root[i] : NULL;
	return i == 0 ? "1" : NULL;
}

static void read_folder(struct task *t, u32 *r, int vol, struct knode *dirn, const char *lpath,
			u32 first, u32 enc, u32 count, u32 buf)
{
	u32 esize = enc == 2 ? 560 : 304;
	if (count > (1u << 20) / esize) count = (1u << 20) / esize;
	u8 *hd = uptr(t, buf, 32 + count * esize);
	if (!hd) { EAX = E_POINTER; EBX = 0; return; }
	memset(hd, 0, 32);
	u32 total = 0, got = 0;
	bool dots = vol == V_TREE ? dirn->parent != NULL :
#ifdef __aarch64__
		    vol == V_LINUX ? strcmp(lpath, "/") != 0 :
#endif
		    false;
	if (dots) {
		for (int i = 0; i < 2; i++, total++)
			if (total >= first && got < count)
				bdfe(hd + 32 + esize * got++, A_DIR, 0, 0, 0, 0, i ? ".." : ".", enc);
	}
	if (vol == V_TREE) {
		for (struct knode *c = dirn->child; c; c = c->next, total++)
			if (total >= first && got < count)
				bdfe(hd + 32 + esize * got++, c->attr, c->attr & A_DIR ? 0 : c->size,
				     c->ctime, c->atime, c->mtime, c->name, enc);
#ifdef __aarch64__
	} else if (vol == V_LINUX) {
		char name[NAME_MAX];
		bool dir;
		u64 size, mt;
		for (u32 i = 0;; i++, total++) {
			long e = lx_kfs_readdir(lpath, i, name, sizeof(name), &dir, &size, &mt);
			if (e < 0) { EAX = lx_error(e); EBX = 0; return; }
			if (e > 0) break;              /* past the last one */
			if (total >= first && got < count)
				bdfe(hd + 32 + esize * got++, dir ? A_DIR : A_ARCH, dir ? 0 : size, mt, mt, mt, name, enc);
		}
#endif
	} else {
		for (const char *n; (n = virtual_entry(vol, total - (dots ? 2 : 0))); total++)
			if (total >= first && got < count)
				bdfe(hd + 32 + esize * got++, A_DIR, 0, 0, 0, 0, n, enc);
	}
	put32(hd, 1);
	put32(hd + 4, got);
	put32(hd + 8, total);
	EAX = got < count ? E_EOF : E_OK;
	EBX = got;
}

/* subfunction 10: the new name is relative to the old one's folder, or,
 * with a '/' first, to the volume's root */
static int node_rename(struct knode *root, struct knode *n, const char *to)
{
	struct knode *base = to[0] == '/' ? root : n->parent;
	char rel[PATH_MAX];
	/* the path from base, "." and ".." resolved inside the volume */
	char basepath[PATH_MAX] = "";
	{
		struct knode *chain[64];
		int k = 0;
		for (struct knode *p = base; p && p != root && k < 64; p = p->parent) chain[k++] = p;
		u32 o = 0;
		while (k--) {
			u32 l = strlen(chain[k]->name);
			if (o + l + 2 >= sizeof(basepath)) return E_NOT_FOUND;
			basepath[o++] = '/';
			memcpy(basepath + o, chain[k]->name, l);
			o += l;
		}
		basepath[o] = 0;
	}
	if (absolute(basepath[0] ? basepath : "/", to[0] == '/' ? to + 1 : to, rel)) return E_NOT_FOUND;
	struct knode *parent;
	const char *last;
	if (walk(root, rel + 1, &parent, &last)) return E_ACCESS;     /* there already */
	if (!parent) return E_NOT_FOUND;
	for (struct knode *p = parent; p; p = p->parent)
		if (p == n) return E_ACCESS;                              /* into itself */
	u32 l = strlen(last);
	if (!l || l >= NAME_MAX) return E_NOT_FOUND;
	node_unlink(n);
	memcpy(n->name, last, l + 1);
	node_link(parent, n);
	return E_OK;
}

#ifdef __aarch64__
static void linux_file(struct task *t, u32 *r, u32 sub, const char *lp, u64 off, u32 size, u32 buf)
{
	long e;
	switch (sub) {
	case 0: {
		u8 *d = uptr(t, buf, size);
		u64 total = 0;
		if (!d) { EAX = E_POINTER; return; }
		e = lx_kfs_read(lp, d, size, off, &total);
		if (e < 0) { EAX = lx_error(e); EBX = -1; return; }
		EAX = (u32)e < size ? E_EOF : E_OK;
		EBX = e;
		return;
	}
	case 2: case 3: {
		const u8 *d = size ? uptr(t, buf, size) : (const u8 *)"";
		if (!d) { EAX = E_POINTER; return; }
		e = lx_kfs_write(lp, d, size, off, sub == 2);
		if (e < 0) { EAX = lx_error(e); EBX = 0; return; }
		EAX = (u32)e < size ? E_DISK_FULL : E_OK;
		EBX = e;
		return;
	}
	case 4:
		e = lx_kfs_truncate(lp, off);
		EAX = e < 0 ? lx_error(e) : E_OK;
		return;
	case 5: {
		u8 *d = uptr(t, buf, 40);
		u64 sz;
		bool dir;
		if (!d) { EAX = E_POINTER; return; }
		e = lx_kfs_info(lp, &sz, &dir);
		if (e < 0) { EAX = lx_error(e); return; }
		bdfe(d, dir ? A_DIR : A_ARCH, sz, 0, 0, 0, NULL, 1);
		EAX = E_OK;
		return;
	}
	case 6: EAX = E_OK; return;            /* attributes: not kept there */
	case 8:
		e = lx_kfs_delete(lp);
		EAX = e < 0 ? lx_error(e) : E_OK;
		return;
	case 9:
		e = lx_kfs_mkdir(lp);
		EAX = e < 0 && e != -17 ? lx_error(e) : E_OK;
		return;
	case 10: {
		const char *to = ustr(t, buf);
		char dst[PATH_MAX], base[PATH_MAX];
		if (!to) { EAX = E_POINTER; return; }
		strncpy0(base, lp, sizeof(base));
		u32 bl = strlen(base);
		while (bl > 1 && base[bl - 1] != '/') bl--;
		base[bl > 1 ? bl - 1 : 1] = 0;          /* the folder it is in */
		if (absolute(to[0] == '/' ? "/" : base, to[0] == '/' ? to + 1 : to, dst)) { EAX = E_NOT_FOUND; return; }
		e = lx_kfs_rename(lp, dst);
		EAX = e < 0 ? lx_error(e) : E_OK;
		return;
	}
	}
	EAX = E_UNSUPPORTED;
}
#endif

/* function 70 */
void kfs_syscall(struct task *t, u32 *r)
{
	u8 *info = uptr(t, EBX, 21);
	if (!info) { EAX = E_POINTER; return; }
	u32 sub = get32(info), off_lo = get32(info + 4), flags = get32(info + 8);
	u32 size = get32(info + 12), buf = get32(info + 16);
	u64 off = off_lo | (u64)flags << 32;
	static char in[PATH_MAX], abs[PATH_MAX];
	u32 pva = EBX + 20;
	if (!info[20]) {
		const u32 *pp = uptr(t, EBX + 21, 4);
		if (!pp) { EAX = E_POINTER; return; }
		memcpy(&pva, pp, 4);
	}
	int e = user_path(t, pva, in);
	if (!e) e = absolute(t->cwd, in, abs);
	if (e) { EAX = e; EBX = sub == 0 ? (u32)-1 : 0; return; }

	if (sub == 7) {                        /* start a program */
		const char *params = flags ? ustr(t, flags) : NULL;
		char p[256];
		if (params) strncpy0(p, params, sizeof(p));
		struct task *nt = NULL;
		int pid = load_program(abs, params ? p : NULL, &nt);
		if (pid > 0 && nt) strncpy0(nt->cwd, t->cwd[0] ? t->cwd : "/sys", sizeof(nt->cwd));
		EAX = pid;
		return;
	}

	struct knode *root = NULL;
	const char *rest = "";
	int vol = volume(abs, &root, &rest);
	if (vol == V_NONE) { EAX = E_NOT_FOUND; EBX = sub == 0 ? (u32)-1 : 0; return; }
#ifdef __aarch64__
	if (vol == V_LINUX) {
		if (sub == 1) read_folder(t, r, vol, NULL, rest, off_lo, flags, size, buf);
		else linux_file(t, r, sub, rest, off, size, buf);
		return;
	}
#endif
	if (vol != V_TREE) {                   /* "/", "/rd", "/tmp0" */
		if (sub == 1) read_folder(t, r, vol, NULL, NULL, off_lo, flags, size, buf);
		else if (sub == 5) {
			u8 *d = uptr(t, buf, 40);
			if (!d) { EAX = E_POINTER; return; }
			bdfe(d, A_DIR, 0, 0, 0, 0, NULL, 1);
			EAX = E_OK;
		} else EAX = sub == 0 ? E_ACCESS : E_UNSUPPORTED;
		return;
	}

	struct knode *parent;
	const char *last;
	struct knode *n = walk(root, rest, &parent, &last);
	switch (sub) {
	case 0: {                              /* read a file */
		if (!n) { EAX = E_NOT_FOUND; EBX = -1; return; }
		if (n->attr & A_DIR) { EAX = E_ACCESS; EBX = -1; return; }
		u8 *d = uptr(t, buf, size);
		if (!d && size) { EAX = E_POINTER; EBX = 0; return; }
		u32 got = node_read(n, d, size, off);
		EAX = got < size ? E_EOF : E_OK;
		EBX = got;
		return;
	}
	case 1:                                /* read a folder */
		if (!n) { EAX = E_NOT_FOUND; EBX = 0; return; }
		if (!(n->attr & A_DIR)) { EAX = E_ACCESS; EBX = 0; return; }
		read_folder(t, r, vol, n, NULL, off_lo, flags, size, buf);
		return;
	case 2:                                /* create or rewrite */
	case 3: {                              /* write into one that is there */
		const u8 *d = size ? uptr(t, buf, size) : (const u8 *)"";
		EBX = 0;
		if (!d) { EAX = E_POINTER; return; }
		if (!n) {
			if (sub == 3) { EAX = E_NOT_FOUND; return; }
			if (!parent) { EAX = E_NOT_FOUND; return; }
			n = node_new(parent, last, strlen(last), false);
			if (!n) { EAX = strlen(last) >= NAME_MAX ? E_NOT_FOUND : E_DISK_FULL; return; }
		} else if (n->attr & A_DIR) {
			EAX = E_ACCESS;
			return;
		} else if (sub == 2) {
			if (n->ro) n->ro = NULL;
			else node_drop(n, 0);
			n->size = 0;
		}
		u32 done;
		EAX = node_write(n, d, size, sub == 2 ? 0 : off, &done);
		EBX = done;
		return;
	}
	case 4:                                /* the size */
		if (!n) { EAX = E_NOT_FOUND; return; }
		if (n->attr & A_DIR) { EAX = E_ACCESS; return; }
		EAX = node_resize(n, off);
		return;
	case 5: {                              /* information */
		if (!n) { EAX = E_NOT_FOUND; return; }
		u8 *d = uptr(t, buf, 40);
		if (!d) { EAX = E_POINTER; return; }
		bdfe(d, n->attr, n->attr & A_DIR ? 0 : n->size, n->ctime, n->atime, n->mtime, NULL, 1);
		EAX = E_OK;
		return;
	}
	case 6: {                              /* set the attributes and times */
		if (!n) { EAX = E_NOT_FOUND; return; }
		if (!n->parent) { EAX = E_ACCESS; return; }
		const u8 *d = uptr(t, buf, 32);
		if (!d) { EAX = E_POINTER; return; }
		n->attr = (n->attr & (A_DIR | 0x08)) | (get32(d) & ~(u32)(A_DIR | 0x08));
		u32 ct = get_time(d + 8), at = get_time(d + 16), mt = get_time(d + 24);
		if (ct) n->ctime = ct;
		if (at) n->atime = at;
		if (mt) n->mtime = mt;
		EAX = E_OK;
		return;
	}
	case 8:                                /* delete a file or an empty folder */
		if (!n) { EAX = E_NOT_FOUND; return; }
		if (!n->parent || n->child) { EAX = E_ACCESS; return; }
		node_free(n);
		EAX = E_OK;
		return;
	case 9: {                              /* a folder, with the ones above it */
		if (n) { EAX = n->attr & A_DIR ? E_OK : E_ACCESS; return; }
		struct knode *d = root;
		for (const char *s = rest; *s;) {
			const char *q = s;
			while (*q && *q != '/') q++;
			struct knode *c = child(d, s, q - s);
			if (!c) c = node_new(d, s, q - s, true);
			else if (!(c->attr & A_DIR)) { EAX = E_ACCESS; return; }
			if (!c) { EAX = E_DISK_FULL; return; }
			d = c;
			s = *q ? q + 1 : q;
		}
		EAX = E_OK;
		return;
	}
	case 10: {                             /* rename or move */
		if (!n) { EAX = E_NOT_FOUND; return; }
		if (!n->parent) { EAX = E_ACCESS; return; }
		char to[PATH_MAX];
		int pe = user_path(t, buf, to);
		if (pe) { EAX = pe; return; }
		EAX = node_rename(root, n, to);
		return;
	}
	default:
		EAX = E_UNSUPPORTED;
		return;
	}
}

/* --- function 30: the current folder ---------------------------------------- */
#define ECX r[2]
#define EDX r[3]

void kfs_cur_dir(struct task *t, u32 *r)
{
	switch (EBX) {
	case 1:                                /* set */
	case 4: {                              /* set, edx the encoding */
		char in[PATH_MAX], abs[PATH_MAX];
		if (user_path(t, ECX, in) || absolute(t->cwd, in, abs)) return;
		strncpy0(t->cwd, abs, sizeof(t->cwd));
		return;
	}
	case 2:                                /* get */
	case 5: {                              /* get, esi the encoding */
		const char *c = t->cwd[0] ? t->cwd : "/sys";
		u32 len = strlen(c) + 1;
		char *d = EDX ? uptr(t, ECX, EDX) : NULL;
		if (d) {
			strncpy0(d, c, EDX);
		}
		EAX = len;
		return;
	}
	default:
		return;                            /* 30.3: the system folder's second key */
	}
}

/* --- the loader's way in ------------------------------------------------------ */

/* a file by its absolute path (from the kernel: /sys, /rd/1, /tmp0/1):
 * its bytes when they are the kernel's (*data), else the node to read */
int kfs_file(const char *path, const u8 **data, void **node, u32 *size)
{
	char abs[PATH_MAX];
	if (absolute("/sys", path, abs)) return -E_NOT_FOUND;
	struct knode *root;
	const char *rest;
	if (volume(abs, &root, &rest) != V_TREE) return -E_NOT_FOUND;
	struct knode *n = walk(root, rest, NULL, NULL);
	if (!n || (n->attr & A_DIR)) return -E_NOT_FOUND;
	*data = n->ro;
	*node = n;
	*size = n->size > 0xffffffffu ? 0xffffffffu : (u32)n->size;
	return 0;
}

u32 kfs_read(void *node, void *buf, u32 len, u32 off)
{
	return node_read(node, buf, len, off);
}
