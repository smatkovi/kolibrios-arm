/* kernel/storage/fat.c on the PC (tests/storage/fat.mk): a volume over an
 * image file, and a shell for fat_test.py: commands on stdin, fields split
 * by tabs, the answer to each a line "r <code> ..." (fat.c's log lines and
 * "e ..." lines of listings before it). "fuzz" runs the fuzz pass. Every
 * vol_wr is counted; one on a volume that is not writable ends the test,
 * and "fail N" makes the (N+1)th from now on and all after it fail and
 * reach nothing (a reset in the middle). */
#include "storage.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

static int img = -1;
static struct vol vol;
static bool mounted;
static long nwrites, nreads, fail_at = -1;
static u32 now = 1700000000;
/* the fuzz pass: the ranges written, to be put back */
static struct range { u64 off; u32 len; } *wlog;
static int nwlog, capwlog;
static bool track;

u32 rtc_seconds(void) { return now; }

int vol_rd(struct vol *v, u64 off, u32 len, void *buf)
{
	u64 size = v->nblocks * v->bsize;
	if (off > size || len > size - off) return E_DEVICE;
	nreads++;
	return pread(img, buf, len, off) == (ssize_t)len ? E_OK : E_DEVICE;
}

int vol_wr(struct vol *v, u64 off, u32 len, const void *buf)
{
	if (!v->writable) {
		printf("FAIL: vol_wr on a read-only volume\n");
		fflush(stdout);
		abort();
	}
	u64 size = v->nblocks * v->bsize;
	if (off > size || len > size - off) return E_DEVICE;
	nwrites++;
	if (fail_at >= 0 && nwrites > fail_at) return E_DEVICE;
	if (track) {
		if (nwlog == capwlog) wlog = realloc(wlog, (capwlog = capwlog ? 2 * capwlog : 256) * sizeof(*wlog));
		wlog[nwlog].off = off, wlog[nwlog++].len = len;
	}
	return pwrite(img, buf, len, off) == (ssize_t)len ? E_OK : E_DEVICE;
}

static void umount(void)
{
	if (mounted) free(vol.fsd);
	mounted = false;
	if (img >= 0) close(img);
	img = -1;
	memset(&vol, 0, sizeof(vol));
}

static int mount(const char *path, bool rw, u32 bsize)
{
	umount();
	img = open(path, rw ? O_RDWR : O_RDONLY);
	if (img < 0) return E_DEVICE;
	struct stat st;
	fstat(img, &st);
	strcpy(vol.name, "test");
	vol.bsize = bsize;
	vol.nblocks = st.st_size / bsize;
	vol.writable = rw;
	int e = fs_fat.mount(&vol);
	mounted = !e;
	if (e) { close(img); img = -1; }
	return e;
}

/* --- listings ---------------------------------------------------------------- */

struct list { int n, cap; struct vdirent *e; };

static void emit(void *ctx, const struct vdirent *e)
{
	struct list *l = ctx;
	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 64;
		l->e = realloc(l->e, l->cap * sizeof(*l->e));
	}
	l->e[l->n++] = *e;
}

static int list(const char *path, u32 first, u32 count, struct list *l, u32 *total)
{
	l->n = 0;
	return fs_fat.readdir(&vol, path, first, count, emit, l, total);
}

static void join(char *o, const char *dir, const char *name)
{
	if (dir[0]) snprintf(o, 4096, "%s/%s", dir, name);
	else snprintf(o, 4096, "%s", name);
}

/* every entry under dir: "e path attr size mtime"; checks paging and info */
static int tree(const char *dir, int depth, int *count)
{
	struct list l = { 0 };
	u32 total;
	int e = list(dir, 0, ~0u, &l, &total);
	if (e) { free(l.e); return e; }
	if (total != (u32)l.n) { printf("FAIL: %s: total %u, %d listed\n", dir, total, l.n); e = E_FS; }
	/* paging: the same entries in pieces of 7 */
	struct list p = { 0 };
	for (u32 i = 0; !e && i < total; i += 7) {
		u32 t;
		if ((e = list(dir, i, 7, &p, &t))) break;
		for (int j = 0; j < p.n; j++)
			if (strcmp(p.e[j].name, l.e[i + j].name)) { printf("FAIL: %s: paging differs at %u\n", dir, i + j); e = E_FS; }
		if (p.n != (int)MIN(7, total - i)) { printf("FAIL: %s: page at %u has %d\n", dir, i, p.n); e = E_FS; }
	}
	free(p.e);
	for (int i = 0; !e && i < l.n; i++) {
		char path[4096];
		join(path, dir, l.e[i].name);
		struct vdirent in;
		int x = fs_fat.info(&vol, path, &in);
		if (x || in.attr != l.e[i].attr || in.size != l.e[i].size || in.mtime != l.e[i].mtime) {
			printf("FAIL: info %s: %d, attr %x/%x size %u/%u\n", path, x, in.attr, l.e[i].attr, (u32)in.size, (u32)l.e[i].size);
			e = E_FS;
			break;
		}
		printf("e\t%s\t%u\t%llu\t%u\t%u\n", path, l.e[i].attr, (unsigned long long)l.e[i].size, l.e[i].mtime, l.e[i].ctime);
		(*count)++;
		if ((l.e[i].attr & A_DIR) && depth < 32) e = tree(path, depth + 1, count);
	}
	free(l.e);
	return e;
}

/* --- the commands ------------------------------------------------------------ */

static u8 *slurp(const char *path, u32 *len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	u8 *b = malloc(n + 1);
	if (fread(b, 1, n, fp) != (size_t)n) n = 0;
	fclose(fp);
	*len = n;
	return b;
}

/* the whole file in pieces of changing sizes, into out */
static int get(const char *path, const char *out, u64 *size)
{
	struct vdirent in;
	int e = fs_fat.info(&vol, path, &in);
	if (e) return e;
	FILE *fp = fopen(out, "wb");
	static u8 buf[1 << 20];
	static const u32 sizes[] = { 1 << 20, 4095, 65536 + 13, 1, 512, 300000 };
	u64 off = 0;
	for (int i = 0;; i++) {
		u32 want = sizes[i % 6], got;
		e = fs_fat.read(&vol, path, off, want, buf, &got);
		if (e) break;
		fwrite(buf, 1, got, fp);
		off += got;
		if (got < want) break;
	}
	fclose(fp);
	*size = off;
	if (!e && off != in.size) { printf("FAIL: %s: %llu bytes read, size %llu\n", path, (unsigned long long)off, (unsigned long long)in.size); e = E_FS; }
	return e;
}

static int split_tabs(char *s, char **f, int max)
{
	int n = 0;
	f[n++] = s;
	for (; *s && n < max; s++)
		if (*s == '\t') *s = 0, f[n++] = s + 1;
	return n;
}

static void reply(int e) { printf("r\t%d\n", e); }

/* --- the fuzz pass ----------------------------------------------------------- */

static u32 rng = 1;
static u32 rnd(void)
{
	rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
	return rng;
}

static void alarmed(int s)
{
	printf("FAIL: fuzz: hang (no answer in 20 s)\n");
	fflush(stdout);
	_exit(1);
}

struct fz { int n; char path[256][300]; bool dir[256]; };

static void fz_walk(const char *dir, int depth, struct fz *z)
{
	struct list l = { 0 };
	u32 total;
	if (list(dir, 0, ~0u, &l, &total) == E_OK)
		for (int i = 0; i < l.n && i < 400; i++) {
			char path[4096];
			join(path, dir, l.e[i].name);
			struct vdirent in;
			fs_fat.info(&vol, path, &in);
			if (strlen(path) < 299 && z->n < 256) {
				strcpy(z->path[z->n], path);
				z->dir[z->n++] = l.e[i].attr & A_DIR;
			}
			if (l.e[i].attr & A_DIR) {
				if (depth < 6) fz_walk(path, depth + 1, z);
			} else {
				static u8 buf[1 << 18];
				u32 got;
				static struct fmap m;
				fs_fat.read(&vol, path, rnd() % 4096, sizeof(buf), buf, &got);
				fs_fat.map(&vol, path, &m);
			}
		}
	free(l.e);
}

static void fuzz(const char *src, u32 seed, int iters)
{
	u32 len;
	u8 *orig = slurp(src, &len);
	if (!orig || len < 4096) { printf("FAIL: fuzz: %s not read\n", src); return; }
	u32 bps = le16(orig + 11), rsvd = le16(orig + 14), nfats = orig[16], rootent = le16(orig + 17);
	u32 fatsz = le16(orig + 22) ? le16(orig + 22) : le32(orig + 36);
	u64 meta = (u64)(rsvd + nfats * fatsz) * bps + rootent * 32;
	char scratch[300];
	snprintf(scratch, sizeof(scratch), "%s.fuzz", src);
	FILE *fp = fopen(scratch, "wb");
	fwrite(orig, 1, len, fp);
	fclose(fp);
	int fd = open(scratch, O_RDWR);
	signal(SIGALRM, alarmed);
	rng = seed * 2654435761u + 1;
	int mounts = 0;
	static u8 data[70000];
	for (u32 i = 0; i < sizeof(data); i++) data[i] = i * 7;
	static u64 flipped[24];
	for (int it = 0; it < iters; it++) {
		int flips = 1 + rnd() % 24;
		for (int k = 0; k < flips; k++) {
			u32 r = rnd() % 10;
			u64 at = r < 2 ? rnd() % 512 : r < 6 ? rsvd * bps + rnd() % (meta + 1 - rsvd * bps)
				 : r < 9 ? meta + rnd() % 65536 : rnd() % len;
			if (at >= len) at = rnd() % len;
			u8 b = orig[at];
			if (rnd() & 1) b ^= 1 << (rnd() % 8);
			else b = rnd();
			pwrite(fd, &b, 1, at);
			flipped[k] = at;
		}
		bool rw = rnd() & 1;
		alarm(20);
		nwlog = 0;
		track = true;
		if (mount(scratch, rw, rnd() & 1 ? 512 : 4096) == E_OK) {
			mounts++;
			static struct fz z;
			z.n = 0;
			fz_walk("", 0, &z);
			for (int k = 0; rw && k < 40; k++) {
				char p[400], q[400];
				const char *a = z.n ? z.path[rnd() % z.n] : "x";
				u32 done;
				snprintf(p, sizeof(p), "%s%snew%u", z.n && z.dir[rnd() % z.n] ? a : "", z.n && strchr(a, '/') ? "/" : "", rnd() % 50);
				snprintf(q, sizeof(q), "%s", z.n ? z.path[rnd() % z.n] : "y");
				switch (rnd() % 8) {
				case 0: fs_fat.write(&vol, p, 0, rnd() % sizeof(data), data, true, &done); break;
				case 1: fs_fat.write(&vol, a, rnd() % 100000, rnd() % 9000, data, false, &done); break;
				case 2: fs_fat.truncate(&vol, a, rnd() % 200000); break;
				case 3: fs_fat.mkdir(&vol, p); break;
				case 4: fs_fat.rename(&vol, a, p); break;
				case 5: fs_fat.rename(&vol, a, q); break;
				case 6: fs_fat.remove(&vol, a); break;
				case 7: { struct vdirent at = { .attr = rnd(), .mtime = rnd() }; fs_fat.setattr(&vol, a, &at); break; }
				}
			}
			fs_fat.sync(&vol);
			z.n = 0;
			fz_walk("", 0, &z);
		}
		alarm(0);
		umount();
		track = false;
		/* the image as it was */
		for (int k = 0; k < nwlog; k++) pwrite(fd, orig + wlog[k].off, wlog[k].len, wlog[k].off);
		for (int k = 0; k < flips; k++) pwrite(fd, orig + flipped[k], 1, flipped[k]);
	}
	close(fd);
	unlink(scratch);
	free(orig);
	printf("fuzz: %s: %d images, %d mounted\n", src, iters, mounts);
}

int main(void)
{
	static char line[1 << 16];
	char *f[8];
	setvbuf(stdout, NULL, _IOLBF, 0);
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\n")] = 0;
		int n = split_tabs(line, f, 8);
		const char *c = f[0];
		int e = E_OK;
		if (!strcmp(c, "quit")) break;
		if (!strcmp(c, "mount") && n >= 4) {
			e = mount(f[1], !strcmp(f[2], "rw"), atoi(f[3]));
			printf("r\t%d\t%s\n", e, vol.label);
		} else if (!strcmp(c, "umount")) {
			umount();
			reply(0);
		} else if (!mounted && strcmp(c, "stats") && strcmp(c, "fail") && strcmp(c, "time") && strcmp(c, "fuzz")) {
			reply(E_UNKNOWN_FS);
		} else if (!strcmp(c, "info") && n >= 2) {
			struct vdirent in;
			e = fs_fat.info(&vol, f[1], &in);
			printf("r\t%d\t%u\t%llu\t%u\t%u\t%u\n", e, in.attr, (unsigned long long)in.size, in.ctime, in.atime, in.mtime);
		} else if (!strcmp(c, "ls") && n >= 4) {
			struct list l = { 0 };
			u32 total = 0;
			e = list(f[1], atoi(f[2]), strtoul(f[3], NULL, 0), &l, &total);
			for (int i = 0; i < l.n; i++)
				printf("e\t%s\t%u\t%llu\t%u\n", l.e[i].name, l.e[i].attr, (unsigned long long)l.e[i].size, l.e[i].mtime);
			printf("r\t%d\t%u\t%d\n", e, total, l.n);
			free(l.e);
		} else if (!strcmp(c, "tree")) {
			int count = 0;
			e = tree("", 0, &count);
			printf("r\t%d\t%d\n", e, count);
		} else if (!strcmp(c, "get") && n >= 3) {
			u64 size = 0;
			e = get(f[1], f[2], &size);
			printf("r\t%d\t%llu\n", e, (unsigned long long)size);
		} else if (!strcmp(c, "read") && n >= 4) {
			u32 len = atoi(f[3]), got = 0;
			u8 *b = malloc(len + 1);
			e = fs_fat.read(&vol, f[1], strtoull(f[2], NULL, 0), len, b, &got);
			printf("r\t%d\t%u\t", e, got);
			for (u32 i = 0; i < got; i++) printf("%02x", b[i]);
			printf("\n");
			free(b);
		} else if (!strcmp(c, "write") && n >= 5) {
			u32 len = 0, done = 0;
			u8 *b = slurp(f[4], &len);
			if (!b) { printf("r\t%d\t0\n", E_POINTER); continue; }
			e = fs_fat.write(&vol, f[1], strtoull(f[2], NULL, 0), len, b, atoi(f[3]), &done);
			printf("r\t%d\t%u\n", e, done);
			free(b);
		} else if (!strcmp(c, "trunc") && n >= 3) {
			reply(fs_fat.truncate(&vol, f[1], strtoull(f[2], NULL, 0)));
		} else if (!strcmp(c, "mkdir") && n >= 2) {
			reply(fs_fat.mkdir(&vol, f[1]));
		} else if (!strcmp(c, "rm") && n >= 2) {
			reply(fs_fat.remove(&vol, f[1]));
		} else if (!strcmp(c, "mv") && n >= 3) {
			reply(fs_fat.rename(&vol, f[1], f[2]));
		} else if (!strcmp(c, "attr") && n >= 6) {
			struct vdirent a = { .attr = strtoul(f[2], NULL, 0), .ctime = strtoul(f[3], NULL, 0),
					     .atime = strtoul(f[4], NULL, 0), .mtime = strtoul(f[5], NULL, 0) };
			reply(fs_fat.setattr(&vol, f[1], &a));
		} else if (!strcmp(c, "sync")) {
			fs_fat.sync(&vol);
			reply(0);
		} else if (!strcmp(c, "map") && n >= 2) {
			static struct fmap m;
			e = fs_fat.map(&vol, f[1], &m);
			printf("r\t%d\t%llu\t%llu\t%u\t%d\t", e, (unsigned long long)m.id, (unsigned long long)m.size, m.flags, m.n);
			for (int i = 0; i < m.n; i++)
				printf("%s%llu:%llu", i ? "," : "", (unsigned long long)m.ext[i].off, (unsigned long long)m.ext[i].len);
			printf("\n");
		} else if (!strcmp(c, "fail") && n >= 2) {
			long k = atol(f[1]);
			fail_at = k < 0 ? -1 : nwrites + k;
			reply(0);
		} else if (!strcmp(c, "stats")) {
			printf("r\t0\t%ld\t%ld\n", nwrites, nreads);
		} else if (!strcmp(c, "time") && n >= 2) {
			now = strtoul(f[1], NULL, 0);
			reply(0);
		} else if (!strcmp(c, "fuzz") && n >= 4) {
			umount();
			fuzz(f[1], atoi(f[2]), atoi(f[3]));
			reply(0);
		} else {
			printf("r\t%d\tunknown command\n", E_UNSUPPORTED);
		}
	}
	umount();
	return 0;
}
