/* KolibriOS for ARM: a test of the storage through function 70
 * (tests/storage): the disks' volumes read only, the container (/hd0/1)
 * written and kept across resets. Its lines go to the debug board
 * ("storetest: ..."), the last one "storetest: done N failed":
 *
 *   storetest tree PATH           every folder and file below PATH: "D path",
 *                                 "F path size crc32" (the host compares them
 *                                 with the files it put there)
 *   storetest ro PATH FILE        every writing subfunction on the volume of
 *                                 PATH (FILE a file on it) refused (10)
 *   storetest rw PATH             files and folders written, read back, cut,
 *                                 grown, renamed, moved, deleted on PATH; it
 *                                 leaves PATH/keep (storetest keep's files)
 *   storetest keep PATH           PATH/keep as rw left it (after a reset)
 *   storetest expect PATH TEXT    the file starts with TEXT (the words joined
 *                                 by blanks); TEXT "-": it is not there
 *   storetest wait N CMD ...      the same after N hundredths of a second
 *   storetest script NAME DISK    the commands of /sys/tests/st-NAME, one a
 *                                 line, "$D" in them replaced by DISK ("vd0",
 *                                 "ufs0", "mmc0"); the kernel starts it with
 *                                 kolibri.run=/sys/storetest
 *                                 kolibri.args=script,NAME,DISK */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kos.h>

static int failed, checks;

static void check(int ok, const char *what, int a, int b)
{
	checks++;
	if (!ok) {
		failed++;
		printf("storetest: FAIL %s (%d %d)\n", what, a, b);
	}
}

struct fi {
	u32 sub, off, flags, size, buf;
	u8 zero;
	const char *path;
} __attribute__((packed));

static struct kos_r2 f70(u32 sub, u32 off, u32 flags, u32 size, const void *buf, const char *path)
{
	struct fi f = { sub, off, flags, size, (u32)buf, 0, path };
	return kos_call2(70, (u32)&f, 0, 0, 0, 0);
}

static u32 crc_table[256];

static u32 crc32(u32 c, const u8 *p, u32 n)
{
	if (!crc_table[1])
		for (u32 i = 0; i < 256; i++) {
			u32 v = i;
			for (int k = 0; k < 8; k++) v = v >> 1 ^ (0xedb88320u & -(v & 1));
			crc_table[i] = v;
		}
	c = ~c;
	while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ c >> 8;
	return ~c;
}

/* the test's data: bytes from a seed (tests/storage/run.py makes the same) */
static void pattern(u8 *b, u32 n, u32 seed, u32 at)
{
	for (u32 i = 0; i < n; i++) {
		u32 x = (at + i) * 2654435761u ^ seed * 40503u;
		b[i] = (u8)(x >> 13 ^ x >> 5);
	}
}

#define BUF (64 * 1024)
static u8 buf[BUF], buf2[BUF];

/* --- tree ------------------------------------------------------------------- */

static int ndirs, nfiles;

static void tree(const char *path, const char *rel)
{
	static u8 dir[32 + 64 * 304];
	for (u32 first = 0;;) {
		struct kos_r2 r = f70(1, first, 1, 64, dir, path);
		if (r.eax && r.eax != 6) {
			printf("storetest: FAIL tree %s: %u\n", path, r.eax);
			failed++;
			return;
		}
		u32 got = r.ebx;
		/* the entries copied first: the recursion reuses dir */
		char names[64][264];
		u32 attrs[64];
		for (u32 i = 0; i < got; i++) {
			strncpy(names[i], (char *)dir + 32 + i * 304 + 40, 263);
			names[i][263] = 0;
			attrs[i] = *(u32 *)(dir + 32 + i * 304);
		}
		for (u32 i = 0; i < got; i++) {
			const char *n = names[i];
			if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
			char p[1024], q[1024];
			snprintf(p, sizeof(p), "%s/%s", path, n);
			snprintf(q, sizeof(q), "%s%s%s", rel, *rel ? "/" : "", n);
			if (attrs[i] & 0x10) {
				printf("storetest: D %s\n", q);
				ndirs++;
				tree(p, q);
				continue;
			}
			u32 c = 0, size = 0;
			for (;;) {
				struct kos_r2 rr = f70(0, size, 0, BUF, buf, p);
				if (rr.eax && rr.eax != 6) {
					printf("storetest: FAIL read %s at %u: %u\n", p, size, rr.eax);
					failed++;
					break;
				}
				c = crc32(c, buf, rr.ebx);
				size += rr.ebx;
				if (rr.eax == 6 || rr.ebx < BUF) break;
			}
			printf("storetest: F %s %u %08x\n", q, size, c);
			nfiles++;
		}
		if (got < 64) break;
		first += got;
	}
}

/* --- read only ------------------------------------------------------------ */

static void ro(const char *path, const char *file)
{
	char p[512];
	u8 attr[32] = { 0x20 }, info[40];
	int there = f70(5, 0, 0, 0, info, file).eax == 0;
	snprintf(p, sizeof(p), "%s/storetest-new", path);
	check(f70(2, 0, 0, 5, "hello", p).eax == 10, "ro: 70.2 a new file refused", 0, 0);
	check(f70(2, 0, 0, 5, "hello", file).eax == 10, "ro: 70.2 over a file refused", 0, 0);
	check(f70(3, 0, 0, 5, "hello", file).eax == 10, "ro: 70.3 refused", 0, 0);
	check(f70(4, 1, 0, 0, 0, file).eax == 10, "ro: 70.4 refused", 0, 0);
	check(f70(6, 0, 0, 0, attr, file).eax == 10, "ro: 70.6 refused", 0, 0);
	check(f70(8, 0, 0, 0, 0, file).eax == 10, "ro: 70.8 refused", 0, 0);
	check(f70(9, 0, 0, 0, 0, p).eax == 10, "ro: 70.9 refused", 0, 0);
	struct fi ren = { 10, 0, 0, 0, (u32)"storetest-renamed", 0, file };
	check(kos_call(70, (u32)&ren, 0, 0, 0, 0) == 10, "ro: 70.10 refused", 0, 0);
	struct kos_r2 r = f70(0, 0, 0, 16, buf, file);
	check(!there || r.eax == 0 || r.eax == 6, "ro: the file still reads", r.eax, 0);
	printf("storetest: ro %s done\n", path);
}

/* --- read-write ----------------------------------------------------------- */

static int write_pattern(const char *p, u32 size, u32 seed)
{
	for (u32 at = 0; at < size || !at; at += BUF) {
		u32 n = size - at < BUF ? size - at : BUF;
		pattern(buf, n, seed, at);
		struct kos_r2 r = f70(at ? 3 : 2, at, 0, n, buf, p);
		if (r.eax || r.ebx != n) return r.eax ? (int)r.eax : -1;
		if (!size) break;
	}
	return 0;
}

static int read_pattern(const char *p, u32 size, u32 seed)
{
	u8 info[40];
	if (f70(5, 0, 0, 0, info, p).eax) return -5;
	if (*(u32 *)(info + 32) != size) return -2;
	for (u32 at = 0; at < size; at += BUF) {
		u32 n = size - at < BUF ? size - at : BUF;
		struct kos_r2 r = f70(0, at, 0, n, buf, p);
		pattern(buf2, n, seed, at);
		if ((r.eax && r.eax != 6) || r.ebx != n || memcmp(buf, buf2, n)) return -3;
	}
	return 0;
}

static int names(const char *path, char *out)
{
	static u8 dir[32 + 32 * 304];
	struct kos_r2 r = f70(1, 0, 1, 32, dir, path);
	out[0] = 0;
	if (r.eax && r.eax != 6) return -(int)r.eax;
	for (u32 i = 0; i < r.ebx; i++) {
		const char *n = (char *)dir + 32 + i * 304 + 40;
		if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
		if (*out) strcat(out, " ");
		strcat(out, n);
		if (*(u32 *)(dir + 32 + i * 304) & 0x10) strcat(out, "/");
	}
	return r.ebx;
}

static const u32 sizes[] = { 0, 1, 4095, 4096, 4097, 100000, 3 * 1024 * 1024 + 17 };

static void rw(const char *path)
{
	char p[512], q[512], s[1024];
	snprintf(p, sizeof(p), "%s/t/a/b", path);
	check(f70(9, 0, 0, 0, 0, p).eax == 0, "rw: 70.9 t/a/b with the folders above", 0, 0);
	for (u32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(p, sizeof(p), "%s/t/f%u.bin", path, i);
		int e = write_pattern(p, sizes[i], i + 1);
		check(e == 0, "rw: write a file", (int)sizes[i], e);
		e = read_pattern(p, sizes[i], i + 1);
		check(e == 0, "rw: read it back", (int)sizes[i], e);
	}
	/* a long name, a name in UTF-8 */
	snprintf(p, sizeof(p), "%s/t/A file with a rather long name, longer than 8.3.txt", path);
	check(f70(2, 0, 0, 3, "abc", p).eax == 0, "rw: a long name", 0, 0);
	snprintf(p, sizeof(p), "%s/t/\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xc3\xa4.txt", path);
	check(f70(2, 0, 0, 3, "xyz", p).eax == 0, "rw: a name in UTF-8", 0, 0);
	/* 70.3 past the end: zeros in the gap */
	snprintf(p, sizeof(p), "%s/t/gap", path);
	f70(2, 0, 0, 2, "ab", p);
	struct kos_r2 r = f70(3, 10000, 0, 2, "cd", p);
	check(r.eax == 0 && r.ebx == 2, "rw: 70.3 past the end", r.eax, r.ebx);
	r = f70(0, 0, 0, 10002, buf, p);
	int zeros = 1;
	for (u32 i = 2; i < 10000; i++) zeros &= buf[i] == 0;
	check(r.ebx == 10002 && !memcmp(buf, "ab", 2) && !memcmp(buf + 10000, "cd", 2) && zeros, "rw: the gap is zeros", r.ebx, 0);
	/* 70.4: cut, grown */
	check(f70(4, 1, 0, 0, 0, p).eax == 0, "rw: 70.4 cut to 1", 0, 0);
	check(f70(4, 70000, 0, 0, 0, p).eax == 0, "rw: 70.4 grown to 70000", 0, 0);
	r = f70(0, 0, 0, BUF, buf, p);
	zeros = 1;
	for (u32 i = 1; i < r.ebx; i++) zeros &= buf[i] == 0;
	check(r.ebx == BUF && buf[0] == 'a' && zeros, "rw: grown with zeros", r.ebx, 0);
	/* 70.2 over a file: rewritten */
	check(f70(2, 0, 0, 3, "new", p).eax == 0, "rw: 70.2 over a file", 0, 0);
	r = f70(0, 0, 0, 100, buf, p);
	check(r.ebx == 3 && !memcmp(buf, "new", 3), "rw: rewritten to 3 bytes", r.ebx, 0);
	/* 70.10: renamed, moved into a folder, a folder moved */
	struct fi ren = { 10, 0, 0, 0, (u32)"gap2", 0, p };
	check(kos_call(70, (u32)&ren, 0, 0, 0, 0) == 0, "rw: 70.10 renamed", 0, 0);
	snprintf(p, sizeof(p), "%s/t/gap2", path);
	struct fi mv = { 10, 0, 0, 0, (u32)"a/b/gap3", 0, p };
	check(kos_call(70, (u32)&mv, 0, 0, 0, 0) == 0, "rw: 70.10 moved into a/b", 0, 0);
	snprintf(p, sizeof(p), "%s/t/a/b", path);
	struct fi mvd = { 10, 0, 0, 0, (u32)"/t/b2", 0, p };
	check(kos_call(70, (u32)&mvd, 0, 0, 0, 0) == 0, "rw: 70.10 a folder moved", 0, 0);
	snprintf(p, sizeof(p), "%s/t/b2", path);
	check(names(p, s) >= 0 && !strcmp(s, "gap3"), "rw: the moved folder has gap3", 0, 0);
	snprintf(q, sizeof(q), "%s/t/f1.bin", path);
	struct fi clash = { 10, 0, 0, 0, (u32)"f2.bin", 0, q };
	check(kos_call(70, (u32)&clash, 0, 0, 0, 0) == 10, "rw: 70.10 onto a file refused", 0, 0);
	/* 70.6 attributes */
	u8 attr[32] = { 0x21 }, info[40];
	check(f70(6, 0, 0, 0, attr, q).eax == 0, "rw: 70.6", 0, 0);
	check(f70(5, 0, 0, 0, info, q).eax == 0 && (info[0] & 1), "rw: 70.5 read only now", info[0], 0);
	attr[0] = 0x20;
	f70(6, 0, 0, 0, attr, q);
	/* 70.8: a folder with something in it refused, then emptied */
	snprintf(p, sizeof(p), "%s/t/b2", path);
	check(f70(8, 0, 0, 0, 0, p).eax == 10, "rw: 70.8 a full folder refused", 0, 0);
	snprintf(q, sizeof(q), "%s/t/b2/gap3", path);
	check(f70(8, 0, 0, 0, 0, q).eax == 0, "rw: 70.8 a file", 0, 0);
	check(f70(8, 0, 0, 0, 0, p).eax == 0, "rw: 70.8 the empty folder", 0, 0);
	check(f70(5, 0, 0, 0, info, q).eax == 5, "rw: gone", 0, 0);
	/* many files in one folder */
	snprintf(p, sizeof(p), "%s/t/many", path);
	f70(9, 0, 0, 0, 0, p);
	int ok = 1;
	for (int i = 0; i < 200 && ok; i++) {
		snprintf(q, sizeof(q), "%s/t/many/file number %03d.txt", path, i);
		ok = f70(2, 0, 0, 4, &i, q).eax == 0;
	}
	check(ok, "rw: 200 files in a folder", 0, 0);
	static u8 dir[32 + 300 * 304];
	r = f70(1, 0, 1, 300, dir, p);
	check(r.ebx == 202 && *(u32 *)(dir + 8) == 202, "rw: 70.1 lists them (and . ..)", r.ebx, *(u32 *)(dir + 8));
	for (int i = 0; i < 200; i += 2) {
		snprintf(q, sizeof(q), "%s/t/many/file number %03d.txt", path, i);
		f70(8, 0, 0, 0, 0, q);
	}
	r = f70(1, 0, 1, 300, dir, p);
	check(r.ebx == 102, "rw: 100 of them deleted", r.ebx, 0);
	/* a program started from the container */
	u32 esz = 0;
	{
		struct kos_r2 rr = f70(0, 0, 0, BUF, buf, "/sys/example");
		esz = rr.ebx;
	}
	snprintf(p, sizeof(p), "%s/t/example", path);
	check(esz > 0 && f70(2, 0, 0, esz, buf, p).eax == 0, "rw: a program written", esz, 0);
	struct fi run = { 7, 0, 0, 0, 0, 0, p };
	int pid = kos_call(70, (u32)&run, 0, 0, 0, 0);
	check(pid > 0, "rw: 70.7 starts it from there", pid, 0);
	/* what is to be there after the reset */
	snprintf(p, sizeof(p), "%s/keep/sub/deep", path);
	check(f70(9, 0, 0, 0, 0, p).eax == 0, "rw: keep/sub/deep", 0, 0);
	snprintf(p, sizeof(p), "%s/keep/a.txt", path);
	check(f70(2, 0, 0, 16, "persistent hello", p).eax == 0, "rw: keep/a.txt", 0, 0);
	snprintf(p, sizeof(p), "%s/keep/big.bin", path);
	check(write_pattern(p, 3000000, 7) == 0, "rw: keep/big.bin", 0, 0);
	snprintf(p, sizeof(p), "%s/keep/sub/deep/c.txt", path);
	check(f70(2, 0, 0, 4, "deep", p).eax == 0, "rw: keep/sub/deep/c.txt", 0, 0);
	printf("storetest: rw %s done\n", path);
}

static void keep(const char *path)
{
	char p[512], s[1024];
	snprintf(p, sizeof(p), "%s/keep/a.txt", path);
	struct kos_r2 r = f70(0, 0, 0, 100, buf, p);
	check(r.ebx == 16 && !memcmp(buf, "persistent hello", 16), "keep: a.txt", r.eax, r.ebx);
	snprintf(p, sizeof(p), "%s/keep/big.bin", path);
	check(read_pattern(p, 3000000, 7) == 0, "keep: big.bin", 0, 0);
	snprintf(p, sizeof(p), "%s/keep/sub/deep/c.txt", path);
	r = f70(0, 0, 0, 100, buf, p);
	check(r.ebx == 4 && !memcmp(buf, "deep", 4), "keep: sub/deep/c.txt", r.eax, r.ebx);
	snprintf(p, sizeof(p), "%s/keep", path);
	check(names(p, s) >= 0 && !strcmp(s, "sub/ a.txt big.bin"), "keep: the folder", 0, 0);
	printf("storetest: keep %s: %s\n", path, s);
}

static int run(int argc, char **argv);

/* a script's lines as commands */
static int script(const char *name, const char *disk)
{
	static char text[4096];
	char p[256];
	snprintf(p, sizeof(p), "/sys/tests/st-%s", name);
	struct kos_r2 r = f70(0, 0, 0, sizeof(text) - 1, text, p);
	if (r.eax && r.eax != 6) {
		printf("storetest: no script %s (%u)\n", p, r.eax);
		return 1;
	}
	text[r.ebx] = 0;
	for (char *l = text, *e; *l; l = e) {
		for (e = l; *e && *e != '\n'; e++)
			;
		if (*e) *e++ = 0;
		char line[512], *o = line, *argv[8];
		for (const char *c = l; *c && o < line + sizeof(line) - 16; c++) {
			if (c[0] == '$' && c[1] == 'D') {
				for (const char *d = disk; *d; d++) *o++ = *d;
				c++;
			} else {
				*o++ = *c;
			}
		}
		*o = 0;
		int argc = 1;
		argv[0] = "storetest";
		for (char *w = line; *w && argc < 8;) {
			while (*w == ' ') w++;
			if (!*w || *w == '#') break;
			argv[argc++] = w;
			while (*w && *w != ' ') w++;
			if (*w) *w++ = 0;
		}
		if (argc > 1) run(argc, argv);
	}
	printf("storetest: script %s on %s: %d failed of %d\n", name, disk, failed, checks);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "script")) return script(argv[2], argv[3]);
	return run(argc, argv);
}

static int run(int argc, char **argv)
{
	int a = 1;
	if (argc > 3 && !strcmp(argv[1], "wait")) {
		kos_delay(atoi(argv[2]));
		a = 3;
	}
	const char *cmd = a < argc ? argv[a] : "", *path = a + 1 < argc ? argv[a + 1] : "/hd0/1";
	if (!strcmp(cmd, "tree")) {
		ndirs = nfiles = 0;
		tree(path, "");
		printf("storetest: tree %s: %d folders, %d files\n", path, ndirs, nfiles);
	} else if (!strcmp(cmd, "ro") && a + 2 < argc) {
		ro(path, argv[a + 2]);
	} else if (!strcmp(cmd, "rw")) {
		rw(path);
	} else if (!strcmp(cmd, "keep")) {
		keep(path);
	} else if (!strcmp(cmd, "expect") && a + 2 < argc) {
		static char want[256];
		want[0] = 0;
		for (int i = a + 2; i < argc; i++) {
			if (i > a + 2) strcat(want, " ");
			strcat(want, argv[i]);
		}
		struct kos_r2 r = f70(0, 0, 0, sizeof(buf), buf, path);
		int ok = !strcmp(want, "-") ? r.eax == 5 :
			 (r.eax == 0 || r.eax == 6) && r.ebx >= strlen(want) && !memcmp(buf, want, strlen(want));
		check(ok, path, r.eax, r.ebx);
		printf("storetest: expect %s %s \"%s\"\n", ok ? "ok" : "FAIL", path, want);
	} else {
		printf("storetest: tree|ro|rw|keep PATH\n");
		return 1;
	}
	printf("storetest: done %d failed of %d (%s %s)\n", failed, checks, cmd, path);
	return 0;
}
