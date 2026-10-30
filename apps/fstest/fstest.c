/* KolibriOS for ARM: a test of the file systems (function 70: the
 * volumes /, /rd, /rd/1 = /sys, /tmp0/1, on AArch64 the Linux tree),
 * the current folder (30), the clipboard (54), the keyboard's layout
 * (26.2) and modes (66). Each check writes "fstest: ok ..." or
 * "fstest: FAIL ..." to the debug board; "fstest: done N failed" at the
 * end (tests/kfs runs it in QEMU) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kos.h>

static int failed;

static void check(int ok, const char *what, int a, int b)
{
	printf("fstest: %s %s (%d %d)\n", ok ? "ok" : "FAIL", what, a, b);
	if (!ok) failed++;
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

static u8 dir[32 + 32 * 304];

/* the names of a folder, joined with ' ' */
static int list(const char *path, char *out)
{
	struct kos_r2 r = f70(1, 0, 1, 32, dir, path);
	out[0] = 0;
	if (r.eax && r.eax != 6) return -(int)r.eax;
	for (u32 i = 0; i < r.ebx; i++) {
		if (i) strcat(out, " ");
		strcat(out, (char *)dir + 32 + i * 304 + 40);
		if (*(u32 *)(dir + 32 + i * 304) & 0x10) strcat(out, "/");
	}
	return r.ebx;
}

/* fstest expect PATH TEXT...: the file holds TEXT (the words joined by
 * blanks) at its start; fstest expect PATH -: it is not there (tests of
 * other programs: tests/apps) */
static int expect(int argc, char **argv)
{
	static char want[256], got[1024];
	for (int i = 3; i < argc; i++) {
		if (i > 3) strcat(want, " ");
		strcat(want, argv[i]);
	}
	struct kos_r2 r = f70(0, 0, 0, sizeof(got) - 1, got, argv[2]);
	int ok = !strcmp(want, "-") ? r.eax == 5 : (r.eax == 0 || r.eax == 6) && !strncmp(got, want, strlen(want));
	printf("fstest: expect %s %s \"%s\"\n", ok ? "ok" : "FAIL", argv[2], want);
	return 0;
}

int main(int argc, char **argv)
{
	char buf[1024], s[256];
	struct kos_r2 r;
	if (argc >= 4 && !strcmp(argv[1], "expect")) return expect(argc, argv);

	/* the current folder */
	int n = kos_call(30, 2, (u32)s, sizeof(s), 0, 0);
	check(!strcmp(s, "/sys") && n == 5, "30.2 /sys at the start", n, 0);

	/* the virtual folders */
	list("/", buf);
	printf("fstest: / = %s\n", buf);
	check(!strncmp(buf, "rd/ tmp0/", 9), "70.1 /", 0, 0);
	list("/rd", buf);
	check(!strcmp(buf, "1/"), "70.1 /rd", 0, 0);
	list("/tmp0/1", buf);
	check(!strcmp(buf, ""), "70.1 /tmp0/1 empty", 0, 0);
	n = list("/sys", buf);
	printf("fstest: /sys = %s\n", buf);
	check(n > 5 && strstr(buf, "fstest") != NULL, "70.1 /sys", n, 0);
	r = f70(1, 0, 1, 2, dir, "/sys");
	check(r.eax == 0 && r.ebx == 2 && *(u32 *)(dir + 8) >= (u32)n, "70.1 the total", r.eax, *(u32 *)(dir + 8));

	/* a file in /tmp0/1 */
	r = f70(2, 0, 0, 5, "hello", "/tmp0/1/a.txt");
	check(r.eax == 0 && r.ebx == 5, "70.2 create", r.eax, r.ebx);
	memset(buf, 0, sizeof(buf));
	r = f70(0, 0, 0, 100, buf, "/tmp0/1/a.txt");
	check(r.eax == 6 && r.ebx == 5 && !strcmp(buf, "hello"), "70.0 read back, EOF", r.eax, r.ebx);
	r = f70(3, 10, 0, 2, "XY", "/TMP0/1/A.TXT");
	check(r.eax == 0 && r.ebx == 2, "70.3 at 10 (any case)", r.eax, r.ebx);
	r = f70(0, 0, 0, 12, buf, "/tmp0/1/a.txt");
	check(r.eax == 0 && r.ebx == 12 && buf[5] == 0 && buf[9] == 0 && buf[10] == 'X', "70.0 zeros in the gap", r.eax, r.ebx);
	r = f70(4, 3, 0, 0, 0, "/tmp0/1/a.txt");
	check(r.eax == 0, "70.4 cut to 3", r.eax, 0);
	u8 info[40];
	r = f70(5, 0, 0, 0, info, "/tmp0/1/a.txt");
	check(r.eax == 0 && *(u32 *)(info + 32) == 3 && !(*(u32 *)info & 0x10), "70.5 size 3", r.eax, *(u32 *)(info + 32));
	/* 70.6: attributes and times as given, 70.5 and 70.1 report them */
	static u8 attr[32] = { 0x21, 0, 0, 0, 1, 0, 0, 0, 5, 4, 3, 0, 2, 1, 0xe4, 0x07,
			       5, 4, 3, 0, 2, 1, 0xe4, 0x07, 5, 4, 3, 0, 2, 1, 0xe4, 0x07 };
	r = f70(6, 0, 0, 0, attr, "/tmp0/1/a.txt");
	u8 inf6[40];
	struct kos_r2 r6 = f70(5, 0, 0, 0, inf6, "/tmp0/1/a.txt");
	check(r.eax == 0 && r6.eax == 0 && (inf6[0] & 1) && !memcmp(inf6 + 8, attr + 8, 24), "70.6 then 70.5: 03:04:05 02.01.2020",
	      inf6[24] | inf6[25] << 8, inf6[28] | inf6[29] << 8);
	r = f70(3, 0, 0, 1, "x", "/tmp0/1/none");
	check(r.eax == 5, "70.3 not there", r.eax, 0);
	r = f70(0, 0, 0, 1, buf, "/tmp0/1");
	check(r.eax == 10, "70.0 a folder: access denied", r.eax, 0);

	/* folders, rename, delete */
	r = f70(9, 0, 0, 0, 0, "/tmp0/1/d1/d2");
	check(r.eax == 0, "70.9 with the folder above", r.eax, 0);
	r = f70(9, 0, 0, 0, 0, "/tmp0/1/d1");
	check(r.eax == 0, "70.9 there already", r.eax, 0);
	struct fi ren = { 10, 0, 0, 0, (u32)"d1/b.txt", 0, "/tmp0/1/a.txt" };
	n = kos_call(70, (u32)&ren, 0, 0, 0, 0);
	check(n == 0, "70.10 into a folder (relative)", n, 0);
	list("/tmp0/1/d1", buf);
	check(!strcmp(buf, "./ ../ d2/ b.txt"), "70.1 . .. d2 b.txt", 0, 0);
	printf("fstest: d1 = %s\n", buf);
	struct fi ren2 = { 10, 0, 0, 0, (u32)"/c.txt", 0, "/tmp0/1/d1/b.txt" };
	n = kos_call(70, (u32)&ren2, 0, 0, 0, 0);
	check(n == 0, "70.10 to the volume's root (absolute)", n, 0);
	struct fi ren3 = { 10, 0, 0, 0, (u32)"d1/d2/x", 0, "/tmp0/1/d1" };
	n = kos_call(70, (u32)&ren3, 0, 0, 0, 0);
	check(n == 10, "70.10 into itself refused", n, 0);
	r = f70(8, 0, 0, 0, 0, "/tmp0/1/d1");
	check(r.eax == 10, "70.8 a full folder refused", r.eax, 0);
	r = f70(8, 0, 0, 0, 0, "/tmp0/1/d1/d2");
	n = r.eax;
	r = f70(8, 0, 0, 0, 0, "/tmp0/1/d1");
	check(n == 0 && r.eax == 0, "70.8 folders", n, r.eax);
	list("/tmp0/1", buf);
	check(!strcmp(buf, "c.txt"), "70.1 c.txt left", 0, 0);

	/* the current folder: relative paths */
	kos_call(30, 1, (u32)"/tmp0/1", 0, 0, 0);
	kos_call(30, 2, (u32)s, sizeof(s), 0, 0);
	check(!strcmp(s, "/tmp0/1"), "30.1 set", 0, 0);
	r = f70(2, 0, 0, 3, "rel", "rel.txt");
	check(r.eax == 0, "70.2 relative", r.eax, 0);
	r = f70(0, 0, 0, 3, buf, "/tmp0/1/rel.txt");
	check(r.eax == 0 && !memcmp(buf, "rel", 3), "70.0 it is in the current folder", r.eax, 0);
	kos_call(30, 1, (u32)"../../sys", 0, 0, 0);
	kos_call(30, 2, (u32)s, sizeof(s), 0, 0);
	check(!strcmp(s, "/sys"), "30.1 ../../sys", 0, 0);

	/* /sys: copy on write */
	r = f70(0, 0, 0, 8, buf, "fstest");
	check(r.eax == 0 && !memcmp(buf, "MENUETA1", 8), "70.0 /sys/fstest (relative)", r.eax, 0);
	r = f70(2, 0, 0, 4, "new!", "/rd/1/fstest.d/new.txt");
	check(r.eax == 5, "70.2 in a folder that is not there", r.eax, 0);
	r = f70(9, 0, 0, 0, 0, "/sys/fstest.d");
	r = f70(2, 0, 0, 4, "new!", "/rd/1/fstest.d/new.txt");
	check(r.eax == 0, "70.2 in a new folder of /sys", r.eax, 0);
	r = f70(0, 0, 0, 8, buf, "/sys/autorun");
	char head[9] = { 0 };
	memcpy(head, buf, 8);
	r = f70(3, 0, 0, 1, "!", "/sys/autorun");
	check(r.eax == 0, "70.3 into a ramdisk file", r.eax, 0);
	r = f70(0, 0, 0, 8, buf, "/sys/autorun");
	check(r.eax == 0 && buf[0] == '!' && !memcmp(buf + 1, head + 1, 7), "70.0 the copy changed", r.eax, 0);
	r = f70(5, 0, 0, 0, info, "/sys");
	check(r.eax == 0 && (*(u32 *)info & 0x10), "70.5 /sys a folder", r.eax, 0);

	/* starting a program written to /tmp0/1 */
	r = f70(5, 0, 0, 0, info, "/sys/example");
	u32 esz = *(u32 *)(info + 32);
	char *ex = malloc(esz);
	r = f70(0, 0, 0, esz, ex, "/sys/example");
	r = f70(2, 0, 0, esz, ex, "/tmp0/1/example2");
	struct fi run = { 7, 0, 0, 0, 0, 0, "/tmp0/1/example2" };
	n = kos_call(70, (u32)&run, 0, 0, 0, 0);
	check(n > 0, "70.7 /tmp0/1/example2 started", n, 0);
	if (n > 0) kos_call(18, 18, n, 0, 0, 0);

	/* UTF-16 */
	static const unsigned short w[] = { '/', 't', 'm', 'p', '0', '/', '1', '/', 'c', '.', 't', 'x', 't', 0 };
	static u8 wpath[64];
	wpath[0] = 2;
	memcpy(wpath + 1, w, sizeof(w));
	r = f70(0, 0, 0, 3, buf, (const char *)wpath);
	check(r.eax == 0 && !memcmp(buf, "hel", 3), "70.0 a UTF-16 path", r.eax, r.ebx);

#ifdef LINUX_TREE
#endif
	/* AArch64: the Linux tree */
	n = list("/", buf);
	if (strstr(buf, "linux/")) {
		n = list("/linux/home", buf);
		check(n >= 1 && strstr(buf, "user/") != NULL, "70.1 /linux/home", n, 0);
		r = f70(2, 0, 0, 2, "lx", "/home/user/t1");
		check(r.eax == 0, "70.2 /home/user/t1", r.eax, 0);
		struct fi lr = { 10, 0, 0, 0, (u32)"t2", 0, "/linux/home/user/t1" };
		n = kos_call(70, (u32)&lr, 0, 0, 0, 0);
		check(n == 0, "70.10 in the Linux tree", n, 0);
		list("/home/user", buf);
		check(strstr(buf, "t2") != NULL && !strstr(buf, "t1"), "70.1 /home/user has t2", 0, 0);
		r = f70(8, 0, 0, 0, 0, "/home/user/t2");
		check(r.eax == 0, "70.8 in the Linux tree", r.eax, 0);
	}

	/* the clipboard */
	int c0 = kos_call(54, 0, 0, 0, 0, 0);
	static struct { u32 size, type, enc; char text[8]; } clip = { 12 + 6, 0, 1, "clip!" };
	n = kos_call(54, 2, clip.size, (u32)&clip, 0, 0);
	check(n == 0 && kos_call(54, 0, 0, 0, 0, 0) == c0 + 1, "54.2 a slot", n, c0);
	kos_call(68, 11, 0, 0, 0, 0);
	char *p = (char *)kos_call(54, 1, c0, 0, 0, 0);
	check((u32)p > 1 && !strcmp(p + 12, "clip!"), "54.1 read it back", (int)p, 0);
	n = kos_call(54, 3, 0, 0, 0, 0);
	check(n == 0 && kos_call(54, 0, 0, 0, 0, 0) == c0, "54.3 deleted", n, 0);

	/* the keyboard */
	u8 lay[128];
	kos_call(26, 2, 1, (u32)lay, 0, 0);
	check(lay[0x10] == 'q' && lay[0x1c] == 13 && lay[0x48] == 0xb2, "26.2 normal", lay[0x10], lay[0x48]);
	kos_call(26, 2, 2, (u32)lay, 0, 0);
	check(lay[0x02] == '!' && lay[0x10] == 'Q', "26.2 shift", lay[0x02], 0);
	kos_call(66, 1, 1, 0, 0, 0);
	n = kos_call(66, 2, 0, 0, 0, 0);
	kos_call(66, 1, 0, 0, 0, 0);
	check(n == 1 && kos_call(66, 2, 0, 0, 0, 0) == 0, "66.1/66.2 scan codes and back", n, 0);

	printf("fstest: done %d failed\n", failed);
	return 0;
}
