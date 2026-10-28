/* KolibriOS for ARM: the ramdisk (/sys = /rd/1), read-only, built into
 * the kernel image by tools/mkrd.py:
 *
 *   "KRD1", u32 count, count x { char name[48]; u32 offset, size; u32 pad[2]; },
 *   then the files (offsets from the start of the ramdisk) */
#include "task.h"

#define MAX_FILES 128
extern const u8 ramdisk_start[], ramdisk_end[];
static struct rd_file files[MAX_FILES];
static int nfiles;

void fs_init(void)
{
	const u8 *rd = ramdisk_start;
	if (ramdisk_end - rd < 8 || memcmp(rd, "KRD1", 4)) {
		kprintf("ramdisk: none\n");
		return;
	}
	u32 n = *(const u32 *)(rd + 4);
	for (u32 i = 0; i < n && nfiles < MAX_FILES; i++) {
		const u8 *e = rd + 8 + i * 64;
		struct rd_file *f = &files[nfiles++];
		f->name = (const char *)e;
		f->data = rd + *(const u32 *)(e + 48);
		f->size = *(const u32 *)(e + 52);
	}
	kprintf("ramdisk: %d files\n", nfiles);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool name_eq(const char *a, const char *b)
{
	while (*a && *b && lower(*a) == lower(*b)) a++, b++;
	return !*a && !*b;
}

/* the file name in a path on the ramdisk: /sys/x, /rd/1/x (any case) */
const char *fs_rd_name(const char *p)
{
	if (*p >= 1 && *p <= 3) p++;               /* an encoding byte */
	if (*p != '/') return p;                   /* relative: from /sys */
	p++;
	if ((lower(p[0]) == 's' && lower(p[1]) == 'y' && lower(p[2]) == 's') && (p[3] == '/' || !p[3]))
		return p[3] ? p + 4 : p + 3;
	if (lower(p[0]) == 'r' && lower(p[1]) == 'd' && p[2] == '/' && p[3] == '1' && (p[4] == '/' || !p[4]))
		return p[4] ? p + 5 : p + 4;
	return NULL;
}

const struct rd_file *fs_find(const char *path)
{
	const char *n = fs_rd_name(path);
	if (!n || !*n) return NULL;
	for (int i = 0; i < nfiles; i++)
		if (name_eq(files[i].name, n)) return &files[i];
	return NULL;
}

int fs_count(void) { return nfiles; }
const struct rd_file *fs_get(int i) { return i >= 0 && i < nfiles ? &files[i] : NULL; }
