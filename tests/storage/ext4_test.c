/* tests/storage: kernel/storage/ext4.c, the ext2/3/4 driver, on the PC.
 *
 * Makes a tree of files in /tmp/kolibri-ext4-test, file system images of
 * it with mkfs.ext2/3/4 -d (block sizes 1K to 64K, the feature sets of
 * ext2, ext3, the N950's ext4, today's ext4, inline_data, meta_bg,
 * bigalloc, sparse_super2), and checks through the driver:
 *   - every folder's listing against the tree (names, attributes, sizes,
 *     times), info() of every path, readdir() windows
 *   - every file and symlink read back byte for byte: whole, in pieces of
 *     odd sizes, at random places, around its holes, past its end
 *   - the error codes (E_NOT_FOUND, E_ACCESS, E_FS)
 *   - map() against debugfs's block map; refusals of holes, unwritten
 *     extents, freed blocks, recovery, unclean state, encrypted and
 *     inline files; inodes made to point at every group's metadata (the
 *     places dumpe2fs lists), at the journal, at free blocks, at their
 *     own tree blocks
 * then a 300 GB volume (many groups), a 17 TiB one with a file and an
 * inode table above block 2^32, and a fuzz pass over damaged copies of
 * small images (no crash, no hang, no ASan report; errors are fine).
 * Everything is deleted at the end.
 *
 * make -f tests/storage/ext4.mk ext4-test   (from the repository's root)
 * ext4_test quick: without the fuzz pass and the big volumes */
#define _GNU_SOURCE
#include "storage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WORK "/tmp/kolibri-ext4-test"
#define SRC WORK "/src"
#define IMG WORK "/img"
#define BIG (64u << 20)         /* files larger: read in windows only */

static int passed, failed;
static const char *cur = "";    /* the image being checked */

static void fail_at(int line, const char *fmt, ...)
{
	va_list ap;
	if (++failed > 80) return;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL [%s] line %d: ", cur, line);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
#define CHECK(c, ...) do { if (c) passed++; else fail_at(__LINE__, __VA_ARGS__); } while (0)

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void die(const char *what)
{
	fprintf(stderr, "ext4_test: %s: %s\n", what, strerror(errno));
	exit(2);
}

/* --- the shell ------------------------------------------------------------ */

static int sh(const char *fmt, ...)
{
	char cmd[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	fflush(stdout);
	int rc = system(cmd);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static char *sh_out(const char *fmt, ...)
{
	char cmd[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	fflush(stdout);
	FILE *f = popen(cmd, "r");
	if (!f) die("popen");
	size_t n = 0, cap = 1 << 16;
	char *b = malloc(cap);
	for (size_t k; (k = fread(b + n, 1, cap - n - 1, f)) > 0;) {
		n += k;
		if (cap - n < 4096) b = realloc(b, cap *= 2);
	}
	b[n] = 0;
	pclose(f);
	return b;
}

static u64 num_after(const char *text, const char *key)
{
	const char *p = strstr(text, key);
	return p ? strtoull(p + strlen(key), NULL, 0) : (u64)-1;
}

/* --- the driver's log (its slog is printf: stdout goes to a file) --------- */

static long log_mark(void)
{
	fflush(stdout);
	return ftell(stdout);
}

static char logbuf[1 << 16];
static const char *log_since(long mark)
{
	fflush(stdout);
	FILE *f = fopen(WORK "/driver.log", "r");
	size_t n = 0;
	if (f) {
		fseek(f, mark, SEEK_SET);
		n = fread(logbuf, 1, sizeof(logbuf) - 1, f);
		fclose(f);
	}
	logbuf[n] = 0;
	return logbuf;
}

/* --- the volume: an image file, or (fuzzing) bytes in memory --------------- */

static struct vol vol;
static int img_fd = -1;
static u8 *img_mem;
static long bad_reads;          /* reads past the volume */

int vol_rd(struct vol *v, u64 off, u32 len, void *buf)
{
	u64 size = v->nblocks * v->bsize;
	if (off > size || len > size - off) {
		bad_reads++;
		return E_DEVICE;
	}
	off += v->start * v->bsize;
	if (img_mem) {
		memcpy(buf, img_mem + off, len);
		return 0;
	}
	for (u8 *p = buf; len;) {
		ssize_t n = pread(img_fd, p, len, off);
		if (n <= 0) return E_DEVICE;
		p += n, off += n, len -= n;
	}
	return 0;
}

u32 rtc_seconds(void) { return time(NULL); }

static void umount(void)
{
	free(vol.fsd);  /* the driver's one allocation of the mount */
	memset(&vol, 0, sizeof(vol));
}

static int mount_file(const char *path, u32 bsize)
{
	struct stat st;
	umount();
	if (img_fd >= 0) close(img_fd);
	img_mem = NULL;
	if ((img_fd = open(path, O_RDONLY)) < 0 || fstat(img_fd, &st)) die(path);
	snprintf(vol.name, sizeof(vol.name), "img/%u", bsize);
	vol.bsize = bsize;
	vol.nblocks = st.st_size / bsize;
	return fs_ext4.mount(&vol);
}

static int mount_mem(u8 *mem, u64 size)
{
	umount();
	img_mem = mem;
	strcpy(vol.name, "fuzz");
	vol.bsize = 512;
	vol.nblocks = size / 512;
	return fs_ext4.mount(&vol);
}

/* --- making the tree ------------------------------------------------------- */

/* a file's bytes: a hash of the seed and the offset */
static u8 pat(u32 seed, u64 o)
{
	u32 h = (u32)(o >> 2) * 0x9e3779b1u ^ seed * 0x85ebca77u ^ (u32)(o >> 34);
	h ^= h >> 15, h *= 0x2c1b3c6du, h ^= h >> 12, h *= 0x297a2d39u, h ^= h >> 15;
	return h >> ((o & 3) * 8);
}

static u32 seed_of(const char *s)
{
	u32 h = 2166136261u;
	while (*s) h = (h ^ (u8)*s++) * 16777619u;
	return h;
}

static char *path2(const char *root, const char *rel)
{
	static char b[4][4096];
	static int k;
	char *p = b[k = (k + 1) & 3];
	snprintf(p, 4096, "%s%s%s", root, *root && *rel ? "/" : "", rel);
	return p;
}

/* a file's bytes [off, off + len) at at in fd */
static void write_pat_at(int fd, u32 seed, u64 off, u64 len, u64 at)
{
	static u8 b[1 << 16];
	while (len) {
		u32 n = len < sizeof(b) ? len : sizeof(b);
		for (u32 i = 0; i < n; i++) b[i] = pat(seed, off + i);
		if (pwrite(fd, b, n, at) != n) die("pwrite");
		off += n, at += n, len -= n;
	}
}

static void write_pat(int fd, u32 seed, u64 off, u64 len)
{
	write_pat_at(fd, seed, off, len, off);
}

/* a file of size bytes, data in segments (pairs of offset, length; none:
 * all of it), holes between them */
static void mkf(const char *root, const char *rel, u64 size, const u64 *seg, int nseg)
{
	const char *p = path2(root, rel);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) die(p);
	if (ftruncate(fd, size)) die("ftruncate");
	if (!nseg) write_pat(fd, seed_of(rel), 0, size);
	for (int i = 0; i < nseg; i++) write_pat(fd, seed_of(rel), seg[2 * i], seg[2 * i + 1]);
	close(fd);
}

static void mkd(const char *root, const char *rel)
{
	const char *p = path2(root, rel);
	if (mkdir(p, 0755) && errno != EEXIST) die(p);
}

static void mkl(const char *root, const char *rel, const char *target)
{
	if (symlink(target, path2(root, rel))) die("symlink");
}

static char *rep(const char *s, int n, const char *tail)
{
	static char b[1024];
	b[0] = 0;
	for (int i = 0; i < n; i++) strcat(b, s);
	strcat(b, tail);
	return b;
}

static void make_tree(const char *r)
{
	mkd(r, "");
	mkf(r, "empty", 0, NULL, 0);
	mkf(r, "one", 1, NULL, 0);
	mkf(r, "b4095", 4095, NULL, 0);
	mkf(r, "b4096", 4096, NULL, 0);
	mkf(r, "b4097", 4097, NULL, 0);
	mkf(r, "mega", (1 << 20) + 13, NULL, 0);
	mkf(r, "big", (20 << 20) + 12288, NULL, 0);
	mkf(r, "container.img", 8 << 20, NULL, 0);
	mkf(r, "falloc_target", 0, NULL, 0);
	mkf(r, "sparse", 5 << 20, (u64[]){ 0, 0 }, 1);
	mkf(r, "holes", 10 << 20, (u64[]){ 0, 65536, 3 << 20, 65536, (10 << 20) - 1, 1 }, 3);
	static u64 fr[2000];
	for (int i = 0; i < 1000; i++) fr[2 * i] = i * 8192, fr[2 * i + 1] = 4096;
	mkf(r, "frag", 1000 * 8192 - 4096, fr, 1000);
	mkf(r, "far", (70 << 20) + 4099, (u64[]){ 0, 4096, 70 << 20, 4099 }, 2);
	mkf(r, "far5g", (5ull << 30) + 4096, (u64[]){ 0, 100, 5ull << 30, 4096 }, 2);
	mkf(r, ".hidden", 5, NULL, 0);
	mkf(r, "ro.txt", 10, NULL, 0);
	chmod(path2(r, "ro.txt"), 0444);
	mkf(r, "\xc3\xa4rger.txt", 300, NULL, 0);                       /* ärger.txt */
	mkd(r, "\xd1\x88\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0");               /* шапка */
	mkf(r, "\xd1\x88\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb.txt", 2000, NULL, 0);
	mkf(r, rep("L", 255, ""), 17, NULL, 0);
	mkf(r, rep("\xc3\xa4", 127, "x"), 33, NULL, 0);                  /* 255 bytes of UTF-8 */
	mkd(r, "a"), mkd(r, "a/b"), mkd(r, "a/b/c"), mkd(r, "a/b/c/d"), mkd(r, "a/b/c/d/e");
	mkf(r, "a/b/c/d/e/deep.txt", 12345, NULL, 0);
	mkf(r, "a/b/sib.txt", 77, NULL, 0);
	mkd(r, "emptydir");
	mkd(r, "tiny");
	mkf(r, "tiny/x", 3, NULL, 0);
	mkf(r, "tiny/y", 4, NULL, 0);
	mkf(r, "small60", 60, NULL, 0);
	mkf(r, "small100", 100, NULL, 0);
	mkf(r, "small150", 150, NULL, 0);
	mkd(r, "rodir");
	mkf(r, "rodir/inside.txt", 9, NULL, 0);
	chmod(path2(r, "rodir"), 0555);
	mkd(r, "many");
	for (int i = 0; i < 3000; i++) {
		char n[32];
		snprintf(n, sizeof(n), "many/f%04d", i);
		mkf(r, n, i % 10 ? 0 : i % 300 + 1, NULL, 0);
	}
	mkl(r, "lnk_short", "b4096");
	mkl(r, "lnk_long", rep("long/", 20, ""));                         /* 100 bytes: slow */
	mkl(r, "lnk_dir", "a");
	mkl(r, "lnk_59", rep("x", 59, ""));
	mkl(r, "lnk_60", rep("y", 60, ""));
	mkl(r, "lnk_dangling", "does/not/exist");
	if (mkfifo(path2(r, "fifo"), 0644)) die("mkfifo");
}

/* --- checking a tree ------------------------------------------------------- */

struct ents {
	struct vdirent *e;
	int n, cap;
};

static void collect(void *ctx, const struct vdirent *e)
{
	struct ents *l = ctx;
	if (l->n == l->cap) l->e = realloc(l->e, (l->cap = l->cap ? l->cap * 2 : 64) * sizeof(*e));
	l->e[l->n++] = *e;
}

static u32 want_attr(const struct stat *st, const char *name)
{
	u32 a = S_ISDIR(st->st_mode) ? A_DIR : S_ISREG(st->st_mode) || S_ISLNK(st->st_mode) ? A_ARCH : A_SYSTEM;
	if (!(st->st_mode & 0200)) a |= A_RO;
	if (name[0] == '.') a |= A_HIDDEN;
	return a;
}

static u64 want_size(const struct stat *st)
{
	return S_ISREG(st->st_mode) || S_ISLNK(st->st_mode) ? (u64)st->st_size : 0;
}

static void check_ent(const char *rel, const struct vdirent *e, const struct stat *st, const char *name)
{
	CHECK(e->attr == want_attr(st, name), "%s: attr 0x%x, want 0x%x", rel, e->attr, want_attr(st, name));
	CHECK(e->size == want_size(st), "%s: size %llu, want %llu", rel, (unsigned long long)e->size,
	      (unsigned long long)want_size(st));
	CHECK(e->mtime == (u32)st->st_mtime && e->ctime == (u32)st->st_ctime && e->atime,
	      "%s: times %u %u %u, want m %u c %u", rel, e->ctime, e->atime, e->mtime, (u32)st->st_mtime,
	      (u32)st->st_ctime);
}

/* what a file or symlink holds: the host's file, or the target */
struct src {
	int fd;
	const char *text;
	u64 size;
};

static void src_bytes(const struct src *s, u64 off, u32 len, u8 *out)
{
	if (s->text) memcpy(out, s->text + off, len);
	else if (pread(s->fd, out, len, off) != len) die("pread src");
}

static u8 *rbuf, *ebuf;         /* a read's bytes, the bytes wanted */

static void window(const char *rel, const struct src *s, u64 off, u32 len)
{
	u32 got = 0xdeadbeef;
	u32 want = off >= s->size ? 0 : (u32)MIN((u64)len, s->size - off);
	memset(rbuf, 0xa5, MIN(len, want + 64));
	int rc = fs_ext4.read(&vol, rel, off, len, rbuf, &got);
	CHECK(rc == E_OK && got == want, "%s: read %llu+%u: rc %d got %u want %u", rel, (unsigned long long)off,
	      len, rc, got, want);
	if (rc || got != want || !want) return;
	src_bytes(s, off, want, ebuf);
	if (memcmp(rbuf, ebuf, want)) {
		u32 i = 0;
		while (rbuf[i] == ebuf[i]) i++;
		CHECK(0, "%s: read %llu+%u differs at byte %u", rel, (unsigned long long)off, len, i);
	} else
		passed++;
}

static u32 rng = 12345;
static u32 rnd(void)
{
	rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
	return rng;
}

static void check_content(const char *rel, const char *host, const struct stat *st)
{
	struct src s = { -1, NULL, st->st_size };
	char target[4096];
	if (S_ISLNK(st->st_mode)) {
		ssize_t n = readlink(host, target, sizeof(target) - 1);
		target[n < 0 ? 0 : n] = 0;
		s.text = target;
	} else if ((s.fd = open(host, O_RDONLY)) < 0)
		die(host);
	if (s.size <= BIG) {
		window(rel, &s, 0, s.size + 17);
		static const u32 piece[] = { 1, 7, 511, 4095, 4097, 65537, 1048579, 3, 12288 };
		u64 off = 0;
		for (int i = 0; off < s.size; i++) {
			u32 len = piece[i % ARRAY_SIZE(piece)];
			window(rel, &s, off, len);
			off += len;
		}
	}
	for (int i = 0; i < 8; i++) window(rel, &s, rnd() % (s.size + 8192), rnd() % 300000);
	if (s.fd >= 0) {
		/* around the holes */
		for (off_t o = 0; o < (off_t)s.size;) {
			off_t d = lseek(s.fd, o, SEEK_DATA), h;
			if (d < 0) break;
			h = lseek(s.fd, d, SEEK_HOLE);
			window(rel, &s, d > 3000 ? d - 3000 : 0, 6000);
			window(rel, &s, h > 3000 ? h - 3000 : 0, 6000);
			if (h <= o) break;
			o = h;
		}
	}
	window(rel, &s, s.size, 100);
	window(rel, &s, s.size + 1000000000ull, 100);
	if (s.fd >= 0) close(s.fd);
}

static int by_name(const struct ents *l, const char *name)
{
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->e[i].name, name)) return i;
	return -1;
}

static void check_dir(const char *root, const char *rel)
{
	struct ents got = { 0 };
	u32 total = 12345;
	int rc = fs_ext4.readdir(&vol, rel, 0, 1u << 30, collect, &got, &total);
	CHECK(rc == E_OK && total == got.n, "readdir %s: rc %d total %u n %d", rel, rc, total, got.n);
	DIR *d = opendir(path2(root, rel));
	if (!d) die(path2(root, rel));
	int hn = 0;
	char *seen = calloc(got.n + 1, 1);
	for (struct dirent *de; (de = readdir(d));) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		hn++;
		char sub[4096], host[4096];
		struct stat st;
		snprintf(sub, sizeof(sub), "%s", path2(rel, de->d_name));
		snprintf(host, sizeof(host), "%s", path2(root, sub));
		if (lstat(host, &st)) die(host);
		int k = by_name(&got, de->d_name);
		CHECK(k >= 0, "readdir %s: %s missing", rel, de->d_name);
		if (k < 0) continue;
		seen[k] = 1;
		check_ent(sub, &got.e[k], &st, de->d_name);
		struct vdirent e;
		rc = fs_ext4.info(&vol, sub, &e);
		CHECK(rc == E_OK, "info %s: %d", sub, rc);
		if (!rc) check_ent(sub, &e, &st, de->d_name);
		if (S_ISDIR(st.st_mode)) check_dir(root, sub);
		else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) check_content(sub, host, &st);
	}
	closedir(d);
	/* the root has lost+found besides */
	int extra = 0;
	for (int i = 0; i < got.n; i++)
		if (!seen[i]) {
			extra++;
			CHECK(!*rel && !strcmp(got.e[i].name, "lost+found") && got.e[i].attr == A_DIR,
			      "readdir %s: %s not in the tree", rel, got.e[i].name);
		}
	CHECK(got.n == hn + extra && extra == (*rel ? 0 : 1), "readdir %s: %d entries, the tree has %d", rel, got.n, hn);
	free(seen);
	free(got.e);
}

/* readdir windows over the 3000 entries */
static void check_windows(const char *rel, int want)
{
	struct ents all = { 0 }, w;
	u32 total;
	int rc = fs_ext4.readdir(&vol, rel, 0, 1u << 30, collect, &all, &total);
	CHECK(rc == E_OK && all.n == want && total == want, "%s: %d entries, want %d", rel, all.n, want);
	for (int first = 0; first < want + 10; first += want / 40 + 3) {
		u32 count = first % 2 ? 7 : 113;
		memset(&w, 0, sizeof(w));
		total = 0;
		rc = fs_ext4.readdir(&vol, rel, first, count, collect, &w, &total);
		int n = first >= want ? 0 : MIN((int)count, want - first);
		CHECK(rc == E_OK && total == want && w.n == n, "%s: window %d+%u: rc %d total %u n %d", rel, first,
		      count, rc, total, w.n);
		for (int i = 0; i < w.n && i < n; i++)
			CHECK(!strcmp(w.e[i].name, all.e[first + i].name) && w.e[i].size == all.e[first + i].size,
			      "%s: window %d: entry %d", rel, first, i);
		free(w.e);
	}
	memset(&w, 0, sizeof(w));
	rc = fs_ext4.readdir(&vol, rel, 3, 0, collect, &w, &total);
	CHECK(rc == E_OK && w.n == 0 && total == want, "%s: count 0", rel);
	free(all.e);
}

static void check_refused(const char *rel, int want_rc, const char *why);

static void check_errors(bool map, bool inl)
{
	struct vdirent e;
	struct ents l = { 0 };
	u32 got, total;
	u8 b[64];
	CHECK(fs_ext4.read(&vol, "a", 0, 10, b, &got) == E_ACCESS, "read a folder");
	CHECK(fs_ext4.read(&vol, "", 0, 10, b, &got) == E_ACCESS, "read the root");
	CHECK(fs_ext4.readdir(&vol, "b4096", 0, 10, collect, &l, &total) == E_ACCESS, "readdir a file");
	CHECK(fs_ext4.readdir(&vol, "lnk_dir", 0, 10, collect, &l, &total) == E_ACCESS, "readdir a symlink");
	CHECK(fs_ext4.readdir(&vol, "fifo", 0, 10, collect, &l, &total) == E_ACCESS, "readdir a fifo");
	CHECK(fs_ext4.readdir(&vol, "nope", 0, 10, collect, &l, &total) == E_NOT_FOUND, "readdir nope");
	CHECK(fs_ext4.info(&vol, "nope", &e) == E_NOT_FOUND, "info nope");
	CHECK(fs_ext4.info(&vol, "B4096", &e) == E_NOT_FOUND, "names are case sensitive");
	CHECK(fs_ext4.info(&vol, "b4096/x", &e) == E_NOT_FOUND, "a file as a folder");
	CHECK(fs_ext4.info(&vol, "lnk_dir/b", &e) == E_NOT_FOUND, "a symlink as a folder");
	CHECK(fs_ext4.info(&vol, "a/b/c/nope/x", &e) == E_NOT_FOUND, "deep nope");
	CHECK(fs_ext4.read(&vol, "nope", 0, 10, b, &got) == E_NOT_FOUND, "read nope");
	got = 99;
	CHECK(fs_ext4.read(&vol, "fifo", 0, 10, b, &got) == E_OK && got == 0, "read a fifo");
	CHECK(fs_ext4.info(&vol, "", &e) == E_OK && e.attr == A_DIR && e.size == 0, "info root");
	struct fmap m;
	if (!map) return;
	CHECK(fs_ext4.map(&vol, "a", &m) == E_FS, "map a folder");
	CHECK(fs_ext4.map(&vol, "lnk_short", &m) == E_FS, "map a symlink");
	CHECK(fs_ext4.map(&vol, "fifo", &m) == E_FS, "map a fifo");
	CHECK(fs_ext4.map(&vol, "nope", &m) == E_NOT_FOUND, "map nope");
	if (inl) check_refused("empty", E_FS, "inline");     /* mkfs -d made it inline */
	else CHECK(fs_ext4.map(&vol, "empty", &m) == E_OK && m.n == 0 && m.size == 0, "map empty");
	free(l.e);
}

/* --- map() against debugfs ------------------------------------------------- */

struct fsinfo {
	u64 blocks;
	u32 bs, fdb, bpg, isz, ipg;
};

static struct fsinfo fs_info(const char *img)
{
	char *o = sh_out("dumpe2fs -h %s 2>/dev/null", img);
	struct fsinfo f = { num_after(o, "Block count:"), num_after(o, "Block size:"), num_after(o, "First block:"),
			    num_after(o, "Blocks per group:"), num_after(o, "Inode size:"), num_after(o, "Inodes per group:") };
	free(o);
	if (f.isz > 4096) f.isz = 128;          /* revision 0: no "Inode size" line */
	return f;
}

/* debugfs's physical block of each logical one, 0 for a hole */
static u64 *dbg_blocks(const char *img, const char *rel, u64 n)
{
	FILE *f = fopen(WORK "/cmds", "w");
	for (u64 i = 0; i < n; i++) fprintf(f, "bmap \"/%s\" %llu\n", rel, (unsigned long long)i);
	fclose(f);
	char *o = sh_out("debugfs -f " WORK "/cmds %s 2>/dev/null", img);
	u64 *b = calloc(n + 1, sizeof(u64)), k = 0;
	for (char *line = strtok(o, "\n"); line && k < n; line = strtok(NULL, "\n"))
		if (line[0] >= '0' && line[0] <= '9') b[k++] = strtoull(line, NULL, 10);
	CHECK(k == n, "debugfs bmap %s: %llu of %llu", rel, (unsigned long long)k, (unsigned long long)n);
	free(o);
	return b;
}

static u32 dbg_inode(const char *img, const char *rel)
{
	char *o = sh_out("debugfs -R 'stat \"/%s\"' %s 2>/dev/null", rel, img);
	u32 n = num_after(o, "Inode:");
	free(o);
	return n;
}

static u32 dbg_flags(const char *img, const char *rel)
{
	char *o = sh_out("debugfs -R 'stat \"/%s\"' %s 2>/dev/null", rel, img);
	u32 n = num_after(o, "Flags:");
	free(o);
	return n;
}

/* a refusal, and the reason in the driver's log */
static void check_refused(const char *rel, int want_rc, const char *why)
{
	struct fmap m;
	long mark = log_mark();
	int rc = fs_ext4.map(&vol, rel, &m);
	const char *log = log_since(mark);
	CHECK(rc == want_rc && m.n == 0 && (!why || strstr(log, why)), "map %s: rc %d, want %d (%s); log: %s", rel, rc,
	      want_rc, why ? why : "", log);
}

static void check_map(const char *img, const struct fsinfo *fi, const char *rel)
{
	struct fmap m;
	struct stat st;
	if (lstat(path2(SRC, rel), &st)) die(rel);
	u64 nb = (st.st_size + fi->bs - 1) / fi->bs, *b = dbg_blocks(img, rel, nb);
	static struct extent want[1 << 16];
	int n = 0;
	for (u64 i = 0; i < nb && n < 1 << 16; i++) {
		u64 off = b[i] * fi->bs;
		if (n && want[n - 1].off + want[n - 1].len == off) want[n - 1].len += fi->bs;
		else want[n].off = off, want[n++].len = fi->bs;
	}
	if (n) want[n - 1].len -= nb * fi->bs - st.st_size;
	free(b);
	if (n > FMAP_MAX) {
		/* ext2/3 with 1 KiB blocks: an indirect block every 256 */
		say("  [%s] %s: %d extents\n", cur, rel, n);
		check_refused(rel, E_FS, "more than");
		return;
	}
	long mark = log_mark();
	int rc = fs_ext4.map(&vol, rel, &m);
	CHECK(rc == E_OK, "map %s: %d (%s)", rel, rc, log_since(mark));
	if (rc) return;
	CHECK(m.size == (u64)st.st_size && m.id == dbg_inode(img, rel), "map %s: size %llu id %llu", rel,
	      (unsigned long long)m.size, (unsigned long long)m.id);
	CHECK(n == m.n, "map %s: %d extents, debugfs %d", rel, m.n, n);
	u64 sum = 0;
	for (int i = 0; i < m.n && i < n; i++) {
		CHECK(m.ext[i].off == want[i].off && m.ext[i].len == want[i].len, "map %s: extent %d: %llu+%llu, want %llu+%llu",
		      rel, i, (unsigned long long)m.ext[i].off, (unsigned long long)m.ext[i].len,
		      (unsigned long long)want[i].off, (unsigned long long)want[i].len);
		sum += m.ext[i].len;
	}
	CHECK(sum == m.size, "map %s: extents sum to %llu", rel, (unsigned long long)sum);
}

/* --- crafted inodes: container.img made to point elsewhere ------------------ */

static int rw_fd = -1;
static u64 ino_at;              /* container.img's inode in the image */
static u8 ino_orig[1024];

static void craft_begin(const char *img, const struct fsinfo *fi)
{
	char *o = sh_out("debugfs -R 'imap /container.img' %s 2>/dev/null", img);
	ino_at = num_after(o, "located at block") * fi->bs + num_after(o, "offset");
	free(o);
	if ((rw_fd = open(img, O_RDWR)) < 0) die(img);
	if (pread(rw_fd, ino_orig, fi->isz, ino_at) != fi->isz) die("pread inode");
}

static void craft_end(const struct fsinfo *fi)
{
	if (pwrite(rw_fd, ino_orig, fi->isz, ino_at) != fi->isz) die("pwrite inode");
	close(rw_fd);
	rw_fd = -1;
}

/* the inode: one extent of n blocks at start, its size theirs */
static void craft_extent(const struct fsinfo *fi, u64 start, u32 n)
{
	u8 b[1024];
	memcpy(b, ino_orig, fi->isz);
	memset(b + 0x28, 0, 60);
	put_le16(b + 0x28, 0xf30a), put_le16(b + 0x2a, 1), put_le16(b + 0x2c, 4), put_le16(b + 0x2e, 0);
	put_le32(b + 0x34, 0), put_le16(b + 0x38, n), put_le16(b + 0x3a, start >> 32), put_le32(b + 0x3c, start);
	put_le32(b + 0x20, le32(b + 0x20) | 0x80000);
	put_le32(b + 0x04, n * fi->bs), put_le32(b + 0x6c, (u64)n * fi->bs >> 32);
	if (pwrite(rw_fd, b, fi->isz, ino_at) != fi->isz) die("pwrite inode");
}

struct range {
	u64 a, b;
	int meta;               /* 1: superblock or descriptors, 2: bitmaps or inode table */
};

/* the metadata dumpe2fs lists, the groups whose block bitmap is not
 * initialised, a free block in an initialised group */
static int meta_ranges(const char *img, const struct fsinfo *fi, struct range **out, char **uninit, u64 *free_blk)
{
	char *o = sh_out("dumpe2fs %s 2>/dev/null", img);
	int n = 0, cap = 64;
	u32 ng = (fi->blocks - fi->fdb + fi->bpg - 1) / fi->bpg, g = 0;
	struct range *r = malloc(cap * sizeof(*r));
	*uninit = calloc(ng + 1, 1);
	*free_blk = 0;
	static const struct { const char *key; int meta, pair; } k[] = {
		{ "superblock at ", 1, 0 }, { "Group descriptors at ", 1, 1 }, { "Group descriptor at ", 1, 0 },
		{ "Reserved GDT blocks at ", 1, 1 },
		{ "Block bitmap at ", 2, 0 }, { "Inode bitmap at ", 2, 0 }, { "Inode table at ", 2, 1 },
	};
	for (char *line = strtok(o, "\n"); line; line = strtok(NULL, "\n")) {
		if (!strncmp(line, "Group ", 6)) {
			g = strtoul(line + 6, NULL, 10);
			if (g < ng && strstr(line, "BLOCK_UNINIT")) (*uninit)[g] = 1;
			continue;
		}
		const char *fb = strstr(line, "Free blocks: ");
		if (fb && !*free_blk && g < ng && !(*uninit)[g] && fb[13] >= '0' && fb[13] <= '9')
			*free_blk = strtoull(fb + 13, NULL, 10);
		for (u32 i = 0; i < ARRAY_SIZE(k); i++) {
			const char *p = strstr(line, k[i].key);
			if (!p) continue;
			char *e;
			u64 a = strtoull(p + strlen(k[i].key), &e, 10), b = a;
			if (k[i].pair && *e == '-') b = strtoull(e + 1, NULL, 10);
			if (n == cap) r = realloc(r, (cap *= 2) * sizeof(*r));
			r[n].a = a, r[n].b = b, r[n++].meta = k[i].meta;
		}
	}
	free(o);
	*out = r;
	return n;
}

static void check_crafted(const char *img, const struct fsinfo *fi, int limit)
{
	struct range *r;
	char *uninit;
	u64 free_blk;
	int n = meta_ranges(img, fi, &r, &uninit, &free_blk);
	CHECK(n >= 4, "dumpe2fs: %d metadata ranges", n);
	craft_begin(img, fi);
	/* the control: its first block, as it is */
	u64 *b0 = dbg_blocks(img, "container.img", 1), first = b0[0];
	free(b0);
	craft_extent(fi, first, 1);
	struct fmap m;
	int rc = fs_ext4.map(&vol, "container.img", &m);
	CHECK(rc == E_OK && m.n == 1 && m.ext[0].off == first * fi->bs && m.ext[0].len == fi->bs,
	      "crafted control: rc %d n %d", rc, m.n);
	int step = n > limit ? n / limit : 1, tried = 0;
	for (int i = 0; i < n; i += step) {
		u64 t[2] = { r[i].a, r[i].b };
		for (int j = 0; j < 2; j++) {
			if (j && t[1] == t[0]) break;
			u32 g = (t[j] - fi->fdb) / fi->bpg;
			if (t[j] < fi->fdb) continue;   /* 1 KiB blocks: block 0 */
			craft_extent(fi, t[j], 1);
			const char *why = uninit[g] ? "BLOCK_UNINIT" : r[i].meta == 1 ? "superblock or descriptors" :
					  "bitmaps or inode table";
			/* the superblock's block (s_first_data_block or below): the
			 * extent itself is refused, before any check that logs */
			if (t[j] <= fi->fdb) why = NULL;
			check_refused("container.img", E_FS, why);
			tried++;
		}
	}
	say("  [%s] %d crafted extents into metadata refused\n", cur, tried);
	/* a free block, the journal, past the end */
	if (free_blk) {
		craft_extent(fi, free_blk, 1);
		check_refused("container.img", E_FS, "free in the bitmap");
	}
	char *o = sh_out("debugfs -R 'bmap <8> 0' %s 2>/dev/null", img);
	u64 jb = strtoull(o, NULL, 10);
	free(o);
	if (jb) {
		craft_extent(fi, jb, 1);
		check_refused("container.img", E_FS, "journal");
	}
	craft_extent(fi, fi->blocks + 3, 1);
	check_refused("container.img", E_FS, NULL);
	u32 got;
	CHECK(fs_ext4.read(&vol, "container.img", 0, 100, rbuf, &got) == E_FS, "read past the volume");
	/* two extents, one block */
	craft_extent(fi, first, 1);
	u8 two[24];
	put_le16(two, 0xf30a), put_le16(two + 2, 2), put_le16(two + 4, 4), put_le16(two + 6, 0);
	put_le32(two + 8, 0);
	if (pwrite(rw_fd, two, 12, ino_at + 0x28) != 12) die("pwrite");
	put_le32(two, 1), put_le16(two + 4, 1), put_le16(two + 6, 0), put_le32(two + 8, first);
	if (pwrite(rw_fd, two, 12, ino_at + 0x28 + 24) != 12) die("pwrite");
	put_le32(two, 2 * fi->bs);
	if (pwrite(rw_fd, two, 4, ino_at + 0x04) != 4) die("pwrite");
	check_refused("container.img", E_FS, "twice");
	/* its own tree: depth 1, the leaf in its own second data block */
	u8 leaf[64], save[64];
	u64 lb = first + 1;
	if (pread(rw_fd, save, 64, lb * fi->bs) != 64) die("pread");
	memset(leaf, 0, sizeof(leaf));
	put_le16(leaf, 0xf30a), put_le16(leaf + 2, 1), put_le16(leaf + 4, (fi->bs - 12) / 12), put_le16(leaf + 6, 0);
	put_le32(leaf + 12, 0), put_le16(leaf + 16, 4), put_le16(leaf + 18, 0), put_le32(leaf + 20, first);
	if (pwrite(rw_fd, leaf, 64, lb * fi->bs) != 64) die("pwrite");
	craft_extent(fi, first, 4);
	u8 ib[60] = { 0 };
	put_le16(ib, 0xf30a), put_le16(ib + 2, 1), put_le16(ib + 4, 4), put_le16(ib + 6, 1);
	put_le32(ib + 12, 0), put_le32(ib + 16, lb), put_le16(ib + 20, lb >> 32);
	if (pwrite(rw_fd, ib, 60, ino_at + 0x28) != 60) die("pwrite");
	check_refused("container.img", E_FS, "tree in the file");
	/* and as ext2 does it: 12 direct blocks, the indirect one the 13th */
	u8 ind[4] = { 0 };
	u8 sv4[4];
	if (pread(rw_fd, sv4, 4, (first + 12) * fi->bs) != 4) die("pread");
	put_le32(ind, first + 12);
	if (pwrite(rw_fd, ind, 4, (first + 12) * fi->bs) != 4) die("pwrite");
	u8 ii[1024];
	memcpy(ii, ino_orig, fi->isz);
	memset(ii + 0x28, 0, 60);
	for (int i = 0; i < 12; i++) put_le32(ii + 0x28 + 4 * i, first + i);
	put_le32(ii + 0x28 + 48, first + 12);
	put_le32(ii + 0x20, le32(ii + 0x20) & ~0x80000u);
	put_le32(ii + 0x04, 13 * fi->bs), put_le32(ii + 0x6c, 0);
	if (pwrite(rw_fd, ii, fi->isz, ino_at) != fi->isz) die("pwrite");
	check_refused("container.img", E_FS, "tree in the file");
	if (pwrite(rw_fd, sv4, 4, (first + 12) * fi->bs) != 4) die("pwrite");
	if (pwrite(rw_fd, save, 64, lb * fi->bs) != 64) die("pwrite");
	craft_end(fi);
	check_map(img, fi, "container.img");     /* all back as it was */
	free(r);
	free(uninit);
}

/* an inline folder whose entries go on in the system.data attribute (as
 * Linux makes them; e2fsprogs turns a full one into blocks instead): tiny's
 * empty attribute given two more entries, z1 and z2, to x's and y's inodes */
static void check_inline_spill(const char *img, const struct fsinfo *fi)
{
	char *o = sh_out("debugfs -R 'imap /tiny' %s 2>/dev/null", img);
	u64 at = num_after(o, "located at block") * fi->bs + num_after(o, "offset");
	free(o);
	u8 ino[1024], orig[1024];
	int fd = open(img, O_RDWR);
	if (fd < 0 || pread(fd, orig, fi->isz, at) != fi->isz) die("pread tiny");
	memcpy(ino, orig, fi->isz);
	u32 start = 128 + le16(ino + 0x80), pos = start + 4, e = 0;
	CHECK(le32(ino + start) == 0xea020000, "tiny: no attributes in the inode");
	while (pos + 20 <= fi->isz && le32(ino + pos)) {
		if (ino[pos] == 4 && ino[pos + 1] == 7 && !memcmp(ino + pos + 16, "data", 4)) e = pos;
		pos += (16 + ino[pos] + 3) & ~3u;
	}
	CHECK(e != 0, "tiny: no system.data");
	if (!e) {
		close(fd);
		return;
	}
	u32 val = fi->isz - 24;                 /* the value at the inode's end */
	put_le16(ino + e + 2, val - (start + 4));
	put_le32(ino + e + 8, 24);
	u8 *d = ino + val;
	put_le32(d, dbg_inode(img, "tiny/x")), put_le16(d + 4, 12), d[6] = 2, d[7] = 1, memcpy(d + 8, "z1", 2);
	put_le32(d + 12, dbg_inode(img, "tiny/y")), put_le16(d + 16, 12), d[18] = 2, d[19] = 1, memcpy(d + 20, "z2", 2);
	if (pwrite(fd, ino, fi->isz, at) != fi->isz) die("pwrite tiny");
	struct ents l = { 0 };
	u32 total;
	int rc = fs_ext4.readdir(&vol, "tiny", 0, 100, collect, &l, &total);
	CHECK(rc == E_OK && l.n == 4 && total == 4 && by_name(&l, "x") >= 0 && by_name(&l, "y") >= 0 &&
	      by_name(&l, "z1") >= 0 && by_name(&l, "z2") >= 0, "inline folder with its attribute: rc %d n %d", rc, l.n);
	free(l.e);
	struct vdirent ent;
	CHECK(fs_ext4.info(&vol, "tiny/z1", &ent) == E_OK && ent.size == 3, "info tiny/z1");
	struct stat st;
	lstat(path2(SRC, "tiny/y"), &st);
	check_content("tiny/z2", path2(SRC, "tiny/y"), &st);
	if (pwrite(fd, orig, fi->isz, at) != fi->isz) die("pwrite tiny");
	close(fd);
}

/* --- changes with debugfs ------------------------------------------------ */

static void dbg_w(const char *img, const char *cmd)
{
	CHECK(sh("debugfs -w -R '%s' %s >/dev/null 2>&1", cmd, img) == 0, "debugfs -w %s", cmd);
}

static void check_changes(const char *img, const struct fsinfo *fi, u32 bsize, bool extents)
{
	struct fmap m;
	char cmd[256];
	u32 got;
	/* immutable */
	u32 fl = dbg_flags(img, "container.img");
	snprintf(cmd, sizeof(cmd), "sif /container.img flags 0x%x", fl | 0x10);
	dbg_w(img, cmd);
	mount_file(img, bsize);
	CHECK(fs_ext4.map(&vol, "container.img", &m) == E_OK && (m.flags & FMAP_IMMUTABLE), "immutable");
	snprintf(cmd, sizeof(cmd), "sif /container.img flags 0x%x", fl);
	dbg_w(img, cmd);
	mount_file(img, bsize);
	CHECK(fs_ext4.map(&vol, "container.img", &m) == E_OK && !(m.flags & FMAP_IMMUTABLE), "not immutable");
	/* encrypted: a file, a folder */
	fl = dbg_flags(img, "b4097");
	snprintf(cmd, sizeof(cmd), "sif /b4097 flags 0x%x", fl | 0x800);
	dbg_w(img, cmd);
	u32 dfl = dbg_flags(img, "tiny");
	snprintf(cmd, sizeof(cmd), "sif /tiny flags 0x%x", dfl | 0x800);
	dbg_w(img, cmd);
	mount_file(img, bsize);
	struct vdirent e;
	struct ents l = { 0 };
	u32 total;
	CHECK(fs_ext4.read(&vol, "b4097", 0, 10, rbuf, &got) == E_ACCESS, "read encrypted");
	CHECK(fs_ext4.info(&vol, "b4097", &e) == E_OK && e.size == 4097, "info encrypted");
	check_refused("b4097", E_FS, "encrypted");
	CHECK(fs_ext4.readdir(&vol, "tiny", 0, 10, collect, &l, &total) == E_ACCESS, "readdir encrypted");
	CHECK(fs_ext4.info(&vol, "tiny/x", &e) == E_ACCESS, "into encrypted");
	snprintf(cmd, sizeof(cmd), "sif /b4097 flags 0x%x", fl);
	dbg_w(img, cmd);
	snprintf(cmd, sizeof(cmd), "sif /tiny flags 0x%x", dfl);
	dbg_w(img, cmd);
	free(l.e);
	/* unwritten extents: zeros to read, no map */
	if (extents) {
		dbg_w(img, "fallocate /falloc_target 0 255");
		snprintf(cmd, sizeof(cmd), "sif /falloc_target size %u", 256 * fi->bs);
		dbg_w(img, cmd);
		char *o = sh_out("debugfs -R 'ex /falloc_target' %s 2>/dev/null", img);
		CHECK(strstr(o, "Uninit") != NULL, "debugfs made no unwritten extent: %s", o);
		free(o);
		mount_file(img, bsize);
		u32 n = 256 * fi->bs;
		u8 *z = malloc(n + 10);
		memset(z, 0x5a, n + 10);
		CHECK(fs_ext4.read(&vol, "falloc_target", 0, n + 10, z, &got) == E_OK && got == n, "read unwritten");
		u32 nz = 0;
		for (u32 i = 0; i < got; i++) nz += z[i] != 0;
		CHECK(nz == 0, "unwritten read: %u bytes not zero", nz);
		free(z);
		check_refused("falloc_target", E_FS, "unwritten");
	}
	/* a block freed in the bitmap */
	u64 *b = dbg_blocks(img, "container.img", 6), blk = b[5];
	free(b);
	snprintf(cmd, sizeof(cmd), "freeb %llu 1", (unsigned long long)blk);
	dbg_w(img, cmd);
	mount_file(img, bsize);
	check_refused("container.img", E_FS, "free in the bitmap");
	snprintf(cmd, sizeof(cmd), "setb %llu 1", (unsigned long long)blk);
	dbg_w(img, cmd);
	mount_file(img, bsize);
	CHECK(fs_ext4.map(&vol, "container.img", &m) == E_OK, "map after setb");
	/* the state: not clean, errors */
	dbg_w(img, "ssv state 0");
	long mark = log_mark();
	CHECK(mount_file(img, bsize) == E_OK && strstr(log_since(mark), "WARNING"), "unclean mount");
	check_refused("container.img", E_FS, "not cleanly unmounted");
	dbg_w(img, "ssv state 3");
	mount_file(img, bsize);
	check_refused("container.img", E_FS, "errors");
	dbg_w(img, "ssv state 1");
	/* the journal needs recovery: mounted with a warning, read, no map */
	dbg_w(img, "feature needs_recovery");
	mark = log_mark();
	CHECK(mount_file(img, bsize) == E_OK && strstr(log_since(mark), "journal needs recovery"), "recovery mount");
	check_refused("container.img", E_FS, "needs recovery");
	struct stat st;
	lstat(path2(SRC, "mega"), &st);
	check_content("mega", path2(SRC, "mega"), &st);
	dbg_w(img, "feature -needs_recovery");
	mount_file(img, bsize);
	CHECK(fs_ext4.map(&vol, "container.img", &m) == E_OK, "map after recovery cleared");
}

/* --- the images ------------------------------------------------------------- */

enum { EXT = 1, INLINE = 2, BIGALLOC = 4, LABEL = 8 };
static const struct cfg {
	const char *name, *mkfs;
	u32 mb;
	int flags;
} cfgs[] = {
	{ "ext4-4k", "mkfs.ext4 -b 4096 -L kolibri-\xc3\xa4", 160, EXT | LABEL },
	{ "ext4-1k-encrypt", "mkfs.ext4 -b 1024 -O encrypt", 160, EXT },
	{ "n950", "mkfs.ext4 -b 4096 -O none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,"
		  "sparse_super,large_file,huge_file,uninit_bg,dir_nlink,extra_isize", 160, EXT },
	{ "ext3-4k", "mkfs.ext3 -b 4096", 160, 0 },
	{ "ext3-1k", "mkfs.ext3 -b 1024", 160, 0 },
	{ "ext2-1k", "mkfs.ext2 -b 1024", 160, 0 },
	{ "inline", "mkfs.ext4 -b 4096 -O inline_data", 160, EXT | INLINE },
	{ "meta_bg-1k", "mkfs.ext4 -b 1024 -O meta_bg,^resize_inode", 160, EXT },
	{ "2k-no-flex", "mkfs.ext4 -b 2048 -O ^flex_bg", 160, EXT },
	{ "64k", "mkfs.ext4 -b 65536", 256, EXT },
	{ "sparse_super2-casefold", "mkfs.ext4 -b 4096 -O sparse_super2,casefold -E num_backup_sb=1", 160, EXT },
	{ "bigalloc", "mkfs.ext4 -b 4096 -O bigalloc -C 65536", 256, EXT | BIGALLOC },
};

static void check_image(const struct cfg *c, int idx)
{
	const char *img = IMG;
	cur = c->name;
	int before = failed;
	sh("rm -f " IMG);
	CHECK(sh("truncate -s %uM %s && %s -q -F -d " SRC " %s >/dev/null 2>&1", c->mb, img, c->mkfs, img) == 0, "mkfs");
	/* mkfs -d makes no htree: e2fsck -D indexes the big folders */
	CHECK(sh("e2fsck -fyD %s >/dev/null 2>&1", img) <= 1, "e2fsck -D");
	struct fsinfo fi = fs_info(img);
	u32 bsize = idx & 1 ? 4096 : 512;
	long mark = log_mark();
	int rc = mount_file(img, bsize);
	CHECK(rc == E_OK, "mount: %d", rc);
	if (rc) return;
	say("  [%s] %s", c->name, log_since(mark));
	CHECK(!strcmp(vol.label, c->flags & LABEL ? "kolibri-\xc3\xa4" : ""), "label \"%s\"", vol.label);
	bad_reads = 0;
	/* that the tree has what it should exercise */
	CHECK(fi.bs > 16384 || dbg_flags(img, "many") & 0x1000, "many is no htree");   /* 64K: one block */
	char *o = sh_out("debugfs -R 'stat /frag' %s 2>/dev/null", img);
	CHECK(fi.bs > 4096 || strstr(o, c->flags & EXT ? "(ETB0)" : "(DIND)"), "frag has no tree blocks");
	free(o);
	o = sh_out("debugfs -R 'stat /far5g' %s 2>/dev/null", img);
	CHECK((c->flags & EXT) || strstr(o, "(TIND)"), "far5g has no triple indirect block");
	free(o);
	check_dir(SRC, "");
	check_windows("many", 3000);
	check_errors(!(c->flags & BIGALLOC), c->flags & INLINE);
	if (c->flags & BIGALLOC) {
		check_refused("container.img", E_UNSUPPORTED, "bigalloc");
		check_refused("sparse", E_UNSUPPORTED, "bigalloc");
	} else {
		/* inline_data: mkfs -d makes the all-hole file inline */
		check_refused("sparse", E_FS, c->flags & INLINE ? "inline" : "a hole");
		check_refused("holes", E_FS, "a hole");
		if (fi.bs <= 4096) check_refused("frag", E_FS, "a hole");
		else check_map(img, &fi, "frag");      /* its 4 KiB holes inside blocks */
		check_refused("far", E_FS, "a hole");
		check_map(img, &fi, "container.img");
		check_map(img, &fi, "big");
		check_map(img, &fi, "mega");
		check_map(img, &fi, "b4096");
		check_map(img, &fi, "a/b/c/d/e/deep.txt");
		if (c->flags & INLINE) {
			CHECK(dbg_flags(img, "one") & 0x10000000, "one is not inline");
			CHECK(dbg_flags(img, "small100") & 0x10000000, "small100 is not inline");
			CHECK(dbg_flags(img, "tiny") & 0x10000000, "tiny is not inline");
			check_inline_spill(img, &fi);
			check_refused("small100", E_FS, "inline");
		} else
			check_map(img, &fi, "small100");
		/* the other block size of the disk */
		mount_file(img, bsize ^ (512 ^ 4096));
		check_map(img, &fi, "container.img");
		struct stat st;
		lstat(path2(SRC, "b4097"), &st);
		check_content("b4097", path2(SRC, "b4097"), &st);
		check_crafted(img, &fi, 400);
		/* (inline_data: falloc_target is inline, so no unwritten test) */
		check_changes(img, &fi, bsize, (c->flags & EXT) && !(c->flags & INLINE));
	}
	CHECK(bad_reads == 0, "%ld reads past the volume", bad_reads);
	umount();
	sh("rm -f " IMG);
	say("  [%s] %s\n", c->name, failed == before ? "ok" : "FAILED");
}

/* --- the big volumes -------------------------------------------------------- */

static void make_small_tree(const char *r)
{
	mkd(r, "");
	mkf(r, "container.img", 8 << 20, NULL, 0);
	mkf(r, "falloc_target", 0, NULL, 0);
	mkf(r, "b4096", 4096, NULL, 0);
	mkf(r, "b4097", 4097, NULL, 0);
	mkf(r, "mega", (1 << 20) + 13, NULL, 0);
	mkf(r, "holes", 1 << 20, (u64[]){ 0, 4096, 500000, 1000 }, 2);
	mkd(r, "tiny");
	mkf(r, "tiny/x", 3, NULL, 0);
	mkd(r, "d");
	for (int i = 0; i < 60; i++) {
		char n[32];
		snprintf(n, sizeof(n), "d/f%02d", i);
		mkf(r, n, i * 37, NULL, 0);
	}
	mkl(r, "lnk", "d/f01");
}

/* an image of the small tree (SRC by then): size, mkfs's options */
static void check_small(const char *name, const char *size, const char *mkfs, bool extents, int craft_limit)
{
	const char *img = WORK "/small.img";
	cur = name;
	int before = failed;
	CHECK(sh("truncate -s %s %s && %s -q -F -d " SRC " %s >/dev/null 2>&1", size, img, mkfs, img) == 0, "mkfs");
	char *du = sh_out("du -k %s", img);
	say("  [%s] %s KiB on the disk\n", name, strtok(du, "\t"));
	free(du);
	struct fsinfo fi = fs_info(img);
	long mark = log_mark();
	CHECK(mount_file(img, 512) == E_OK, "mount");
	say("  [%s] %s", name, log_since(mark));
	check_dir(SRC, "");
	check_map(img, &fi, "container.img");
	check_map(img, &fi, "mega");
	check_refused("holes", E_FS, "a hole");
	check_crafted(img, &fi, craft_limit);
	check_changes(img, &fi, 4096, extents);
	umount();
	sh("rm -f %s", img);
	say("  [%s] %s\n", name, failed == before ? "ok" : "FAILED");
}

/* 17 TiB: blocks past 2^32. A file's blocks moved up there (its extent's
 * start_hi), an inode in a group whose inode table is up there (the
 * descriptor's _hi halves) */
static void check_17t(void)
{
	const char *img = WORK "/huge.img";
	cur = "17T";
	int before = failed;
	CHECK(sh("truncate -s 17T %s && mkfs.ext4 -q -F -b 4096 -O ^metadata_csum,uninit_bg,sparse_super2 "
		 "-E num_backup_sb=1,lazy_itable_init=1,lazy_journal_init=1 -d " SRC " %s >/dev/null 2>&1", img, img) == 0,
	      "mkfs");
	char *du = sh_out("du -k %s", img);
	say("  [17T] %s KiB on the disk\n", strtok(du, "\t"));
	free(du);
	struct fsinfo fi = fs_info(img);
	u32 g = 135000;                         /* its first block is above 2^32 */
	u64 high = (u64)g * fi.bpg + 100, n = ((1 << 20) + 13 + fi.bs - 1) / fi.bs;
	CHECK(high >> 32 && g * (u64)fi.bpg < fi.blocks, "the group is not above 2^32");
	/* "mega" moved to high: its data there, marked in use, the extent */
	int fd = open(img, O_RDWR);
	if (fd < 0) die(img);
	write_pat_at(fd, seed_of("mega"), 0, (1 << 20) + 13, high * fi.bs);
	close(fd);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "set_bg %u flags 0", g);
	dbg_w(img, cmd);
	snprintf(cmd, sizeof(cmd), "setb %llu %llu", (unsigned long long)high, (unsigned long long)n);
	dbg_w(img, cmd);
	char *o = sh_out("debugfs -R 'imap /mega' %s 2>/dev/null", img);
	u64 at = num_after(o, "located at block") * fi.bs + num_after(o, "offset");
	free(o);
	u8 ino[1024];
	fd = open(img, O_RDWR);
	if (pread(fd, ino, fi.isz, at) != fi.isz) die("pread");
	CHECK(le32(ino + 0x20) & 0x80000, "mega has no extents");
	memset(ino + 0x28, 0, 60);
	put_le16(ino + 0x28, 0xf30a), put_le16(ino + 0x2a, 1), put_le16(ino + 0x2c, 4);
	put_le16(ino + 0x38, n), put_le16(ino + 0x3a, high >> 32), put_le32(ino + 0x3c, high);
	if (pwrite(fd, ino, fi.isz, at) != fi.isz) die("pwrite");
	/* the same inode once more, as the first of group g: hi_ino */
	u32 num = g * fi.ipg + 1;
	snprintf(cmd, sizeof(cmd), "debugfs -R 'imap <%u>' %s 2>/dev/null", num, img);
	o = sh_out("%s", cmd);
	u64 at2 = num_after(o, "located at block") * fi.bs + num_after(o, "offset");
	u64 itb = num_after(o, "located at block");
	CHECK(itb != (u64)-1 && itb >> 32, "inode %u's table not above 2^32: %s", num, o);
	free(o);
	if (pwrite(fd, ino, fi.isz, at2) != fi.isz) die("pwrite");
	close(fd);
	snprintf(cmd, sizeof(cmd), "seti <%u>", num);
	dbg_w(img, cmd);
	snprintf(cmd, sizeof(cmd), "link <%u> /hi_ino", num);
	dbg_w(img, cmd);
	long mark = log_mark();
	CHECK(mount_file(img, 4096) == E_OK, "mount");
	say("  [17T] %s", log_since(mark));
	struct stat st;
	lstat(path2(SRC, "mega"), &st);
	check_content("mega", path2(SRC, "mega"), &st);
	check_content("hi_ino", path2(SRC, "mega"), &st);
	struct vdirent e;
	CHECK(fs_ext4.info(&vol, "hi_ino", &e) == E_OK && e.size == (u64)st.st_size, "info hi_ino");
	struct fmap m;
	int rc = fs_ext4.map(&vol, "mega", &m);
	CHECK(rc == E_OK && m.n == 1 && m.ext[0].off == high * fi.bs && m.ext[0].len == (u64)st.st_size,
	      "map mega up there: rc %d n %d off %llx", rc, m.n, (unsigned long long)m.ext[0].off);
	rc = fs_ext4.map(&vol, "hi_ino", &m);
	CHECK(rc == E_OK && m.id == num && m.ext[0].off == high * fi.bs, "map hi_ino: rc %d id %llu", rc,
	      (unsigned long long)m.id);
	check_map(img, &fi, "container.img");
	struct stat s2;
	lstat(path2(SRC, "container.img"), &s2);
	check_content("container.img", path2(SRC, "container.img"), &s2);
	/* a block up there freed: refused */
	snprintf(cmd, sizeof(cmd), "freeb %llu 1", (unsigned long long)(high + 7));
	dbg_w(img, cmd);
	mount_file(img, 4096);
	check_refused("mega", E_FS, "free in the bitmap");
	umount();
	sh("rm -f %s", img);
	say("  [17T] %s\n", failed == before ? "ok" : "FAILED");
}

/* --- fuzzing ------------------------------------------------------------- */

static int fuzz_iter;
static const char *fuzz_name;
static void on_alarm(int sig)
{
	fprintf(stderr, "ext4_test: HANG in fuzz %s, iteration %d\n", fuzz_name, fuzz_iter);
	abort();
}

static int ops;
static void explore(const char *path, int depth)
{
	struct ents l = { 0 };
	u32 total, got;
	struct vdirent e;
	struct fmap m;
	if (depth > 6 || ++ops > 1500) return;
	fs_ext4.readdir(&vol, path, 0, 200, collect, &l, &total);
	struct ents w = { 0 };
	fs_ext4.readdir(&vol, path, 3, 5, collect, &w, &total);
	free(w.e);
	for (int i = 0; i < l.n && ops < 1500; i++, ops++) {
		char sub[2048];
		snprintf(sub, sizeof(sub), "%s", path2(path, l.e[i].name));
		if (strlen(sub) > 1500) continue;
		fs_ext4.info(&vol, sub, &e);
		if (l.e[i].attr & A_DIR) {
			explore(sub, depth + 1);
			continue;
		}
		fs_ext4.read(&vol, sub, 0, 65536, rbuf, &got);
		fs_ext4.read(&vol, sub, l.e[i].size / 2, 4096, rbuf, &got);
		fs_ext4.read(&vol, sub, rnd() % (l.e[i].size + 1), 70000, rbuf, &got);
		fs_ext4.map(&vol, sub, &m);
	}
	fs_ext4.info(&vol, path2(path, "nope"), &e);
	free(l.e);
}

static bool fi_htree(const char *img)
{
	return dbg_flags(img, "many") & 0x1000;
}

static void fuzz(const char *name, const char *mkfs, int iters)
{
	const char *img = WORK "/fuzz.img", *src = WORK "/fsrc";
	cur = fuzz_name = name;
	sh("rm -rf %s %s", src, img);
	mkd(src, "");
	mkd(src, "d1"), mkd(src, "d1/d2"), mkd(src, "d1/d2/d3"), mkd(src, "many");
	mkf(src, "e", 0, NULL, 0), mkf(src, "one", 1, NULL, 0), mkf(src, "h", 100, NULL, 0);
	mkf(src, "d1/f5000", 5000, NULL, 0), mkf(src, "d1/d2/f70000", 70000, NULL, 0);
	mkf(src, "d1/d2/d3/c.img", 65536, NULL, 0);
	static u64 sg[40];
	for (int i = 0; i < 20; i++) sg[2 * i] = i * 4096, sg[2 * i + 1] = 1024;
	mkf(src, "holes", 20 * 4096, sg, 20);
	for (int i = 0; i < 400; i++) {
		char n[32];
		snprintf(n, sizeof(n), "many/entry-%03d", i);
		mkf(src, n, i % 5 ? 0 : 200, NULL, 0);
	}
	mkl(src, "s", "one"), mkl(src, "l", rep("q", 80, ""));
	mkfifo(path2(src, "p"), 0644);
	CHECK(sh("truncate -s 4M %s && %s -q -F -d %s %s >/dev/null 2>&1 && e2fsck -fyD %s >/dev/null 2>&1; [ $? -le 1 ]",
		 img, mkfs, src, img, img) == 0, "mkfs");
	CHECK(fi_htree(img), "%s: many is no htree", name);
	struct fsinfo fi = fs_info(img);
	/* where to damage: the superblock, descriptors, bitmaps, inode
	 * tables (their start), folders' blocks, tree and indirect blocks */
	struct range *r, z[256];
	char *uninit;
	u64 free_blk;
	int nr = meta_ranges(img, &fi, &r, &uninit, &free_blk), nz = 0;
	free(uninit);
	for (int i = 0; i < nr && nz < 200; i++) z[nz].a = r[i].a, z[nz++].b = MIN(r[i].b, r[i].a + 7);
	free(r);
	static const char *const objs[] = { "/", "/d1", "/d1/d2", "/d1/d2/d3", "/many", "/holes", "/d1/d2/f70000",
					    "/l", "/h" };
	for (u32 i = 0; i < ARRAY_SIZE(objs); i++) {
		char *o = sh_out("debugfs -R 'stat %s' %s 2>/dev/null", objs[i], img);
		/* "(0):1234", "(ETB0):1234", "(IND):1234", "(0-3):1234-1237" */
		for (char *p = strstr(o, "BLOCKS:") ? strstr(o, "BLOCKS:") : strstr(o, "EXTENTS:"); p && *p && nz < 250;) {
			char *q = strstr(p, "):");
			if (!q) break;
			u64 a = strtoull(q + 2, &p, 10), b = a;
			if (*p == '-') b = strtoull(p + 1, &p, 10);
			if (a) z[nz].a = a, z[nz++].b = MIN(b, a + 3);
		}
		free(o);
	}
	int fd = open(img, O_RDONLY);
	struct stat st;
	fstat(fd, &st);
	u8 *pristine = malloc(st.st_size), *work = malloc(st.st_size);
	if (pread(fd, pristine, st.st_size, 0) != st.st_size) die("pread");
	close(fd);
	memcpy(work, pristine, st.st_size);
	signal(SIGALRM, on_alarm);
	int mounted = 0;
	for (fuzz_iter = 0; fuzz_iter < iters; fuzz_iter++) {
		u64 at[16];
		int k = 1 + rnd() % 8;
		for (int i = 0; i < k; i++) {
			u32 w = rnd() % 100;
			if (w < 8) at[i] = 1024 + rnd() % 1024;          /* the superblock */
			else if (w < 18) at[i] = rnd() % (st.st_size / 2);
			else {
				int j = rnd() % nz;
				at[i] = z[j].a * fi.bs + rnd() % ((z[j].b - z[j].a + 1) * fi.bs);
			}
			if (at[i] >= (u64)st.st_size) at[i] = rnd() % st.st_size;
			u32 how = rnd() % 10;
			work[at[i]] = how < 5 ? rnd() : how < 8 ? work[at[i]] ^ 1 << rnd() % 8 : how < 9 ? 0 : 0xff;
		}
		alarm(10);
		if (mount_mem(work, st.st_size) == E_OK) {
			mounted++;
			ops = 0;
			explore("", 0);
		}
		alarm(0);
		for (int i = 0; i < k; i++) work[at[i]] = pristine[at[i]];
	}
	umount();
	free(pristine);
	free(work);
	passed++;
	say("  [%s] %d damaged images, %d of them mounted: no crash, no hang\n", name, iters, mounted);
	sh("rm -rf %s %s", src, img);
}

/* --- main ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	bool quick = argc > 1 && !strcmp(argv[1], "quick");
	setenv("LC_ALL", "C", 1);
	sh("chmod -R u+w " WORK " 2>/dev/null; rm -rf " WORK);
	if (mkdir(WORK, 0755)) die(WORK);
	if (!freopen(WORK "/driver.log", "w", stdout)) die("driver.log");
	rbuf = malloc(BIG + 1024 * 1024);
	ebuf = malloc(BIG + 1024 * 1024);
	char *df = sh_out("df -k --output=avail /tmp | tail -1");
	say("ext4_test: %s KiB free in /tmp\n", strtok(df, " \n"));
	free(df);
	make_tree(SRC);
	for (u32 i = 0; i < ARRAY_SIZE(cfgs); i++) check_image(&cfgs[i], i);
	sh("chmod -R u+w " SRC "; rm -rf " SRC);
	make_small_tree(SRC);
	/* revision 0: 128-byte inodes, no features (16-bit name lengths, a
	 * superblock copy in every group) */
	check_small("ext2-rev0", "64M", "mkfs.ext2 -b 1024 -E revision=0", false, 100);
	if (!quick) {
		check_small("300G", "300G", "mkfs.ext4 -b 4096 -E lazy_itable_init=1,lazy_journal_init=1", true, 60);
		check_17t();
		/* the refusals' lines of thousands of damaged images: not kept */
		if (!freopen("/dev/null", "w", stdout)) die("/dev/null");
		fuzz("fuzz ext4-1k", "mkfs.ext4 -b 1024", 1500);
		fuzz("fuzz ext2-1k", "mkfs.ext2 -b 1024", 1500);
		fuzz("fuzz ext4-4k inline", "mkfs.ext4 -b 4096 -O inline_data,^has_journal", 1500);
	}
	sh("chmod -R u+w " WORK " 2>/dev/null; rm -rf " WORK);
	say("ext4_test: %d checks passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
