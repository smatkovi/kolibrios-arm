/* KolibriOS for ARM: the block layer, the container's checks and the
 * fence on the PC (make -C tests/storage host), on mkdisk.py's images:
 *
 *   host DISK.img BSIZE NAME [expect]
 *
 * The image is a disk of BSIZE-byte blocks named NAME ("ufs0", "mmc0"),
 * every write that reaches it is logged and checked against the
 * container's ranges (DISK.img.ranges, bytes). EXPECT: "home" (the
 * container is found, checked, written through FAT, read back, and no
 * write lands outside it), or "nohome" (refused: no write at all). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../kernel/storage/storage.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

u32 rtc_seconds(void) { return (u32)time(NULL); }

static FILE *img;
static struct blkdev disk;
static struct { unsigned long long off, len; } ranges[FMAP_MAX];
static int nranges;
static long writes, outside;

static int dread(struct blkdev *d, u64 lba, u32 n, void *buf)
{
	if (fseeko(img, (off_t)lba * d->bsize, SEEK_SET) || fread(buf, d->bsize, n, img) != n) return -1;
	return 0;
}

/* the disk's write entry: what reaches it must be inside the ranges */
static int dwrite(struct blkdev *d, u64 lba, u32 n, const void *buf)
{
	unsigned long long b0 = lba * d->bsize, b1 = (lba + n) * d->bsize;
	int in = 0;
	for (int i = 0; i < nranges; i++)
		if (b0 >= ranges[i].off && b1 <= ranges[i].off + ranges[i].len) in = 1;
	writes++;
	if (!in) {
		outside++;
		printf("FAIL: a write outside the container: bytes %llu-%llu\n", b0, b1);
		return -1;
	}
	if (fseeko(img, (off_t)b0, SEEK_SET) || fwrite(buf, d->bsize, n, img) != n) return -1;
	return 0;
}

static void load_ranges(const char *path)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s.ranges", path);
	FILE *f = fopen(p, "r");
	if (!f) return;
	while (nranges < FMAP_MAX && fscanf(f, "%llu %llu", &ranges[nranges].off, &ranges[nranges].len) == 2) nranges++;
	fclose(f);
}

/* the fence, from the side: block by block around the ranges */
static void fence_edges(void)
{
	u32 bs = disk.bsize;
	for (int i = 0; i < nranges; i++) {
		u64 first = ranges[i].off / bs, n = ranges[i].len / bs;
		CHECK(blk_fence_allows(&disk, first, 1), "range %d: its first block", i);
		CHECK(blk_fence_allows(&disk, first + n - 1, 1), "range %d: its last block", i);
		CHECK(blk_fence_allows(&disk, first, (u32)n), "range %d: all of it", i);
		CHECK(!blk_fence_allows(&disk, first - 1, 1), "range %d: the block before", i);
		CHECK(!blk_fence_allows(&disk, first + n, 1), "range %d: the block after", i);
		CHECK(!blk_fence_allows(&disk, first - 1, 2), "range %d: across its start", i);
		CHECK(!blk_fence_allows(&disk, first + n - 1, 2), "range %d: across its end", i);
		CHECK(!blk_fence_allows(&disk, first, 0), "range %d: no blocks", i);
		CHECK(!blk_fence_allows(&disk, 0xffffffffffffffffull, 2), "wrapping");
		if (i + 1 < nranges) {
			u64 next = ranges[i + 1].off / bs;
			CHECK(!blk_fence_allows(&disk, first, (u32)(next - first + 1)), "ranges %d and %d together", i, i + 1);
		}
	}
	CHECK(!blk_fence_allows(&disk, 0, 1), "block 0");
	struct blkdev other = disk;
	CHECK(!blk_fence_allows(&other, ranges[0].off / bs, 1), "another disk at the same block");
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "host DISK.img BSIZE NAME home|nohome ro|rw\n");
		return 2;
	}
	img = fopen(argv[1], "r+b");
	if (!img) { perror(argv[1]); return 2; }
	load_ranges(argv[1]);
	fseeko(img, 0, SEEK_END);
	disk.bsize = atoi(argv[2]);
	disk.nblocks = (u64)ftello(img) / disk.bsize;
	strncpy(disk.name, argv[3], sizeof(disk.name) - 1);
	strcpy(disk.what, "image");
	disk.max_blocks = 64;
	disk.read = dread;
	bool want_home = !strcmp(argv[4], "home"), rw = !strcmp(argv[5], "rw");
	CHECK(blk_add(&disk, dwrite) == 0, "blk_add");
	int nv = blk_scan();
	CHECK(nv > 0, "volumes found: %d", nv);
	home_init(rw ? HOME_RW : HOME_RO);
	struct vol *h = home_vol();
	CHECK(!!h == want_home, "container %s", want_home ? "found" : "refused");
	if (h && !rw) {
		CHECK(!h->writable, "read only with kolibri.home=ro");
		u32 done = 0;
		int e = h->fs->write(h, "x.txt", 0, 3, "abc", true, &done);
		CHECK(e == E_ACCESS, "a write to the read-only container refused (%d)", e);
		CHECK(vol_wr(h, 0, 4096, (u8[4096]){0}) == E_ACCESS, "vol_wr on the read-only container");
		CHECK(!blk_fence_allows(&disk, ranges[0].off / disk.bsize, 1), "the fence is empty while read only");
	}
	/* every other volume: no write reaches the disk */
	for (int i = 0; i < vol_count(); i++) {
		struct vol *v = vol_get(i);
		if (v == h) continue;
		static u8 z[4096];
		CHECK(vol_wr(v, 0, 512, z) == E_ACCESS, "vol_wr %s refused", v->name);
		if (v->fs && v->fs->write) {
			u32 done = 0;
			int e = v->fs->write(v, "storetest.txt", 0, 3, "abc", true, &done);
			CHECK(e == E_ACCESS, "%s: the file system's write refused (%d)", v->name, e);
		}
	}
	CHECK(writes == 0, "nothing written yet (%ld)", writes);
	if (h && rw) {
		CHECK(h->writable, "writable with kolibri.home=rw");
		fence_edges();
		/* files and folders through FAT, through the fence */
		static u8 big[3 << 20];
		for (u32 i = 0; i < sizeof(big); i++) big[i] = (u8)(i * 7 + i / 4096);
		u32 done = 0;
		CHECK(h->fs->mkdir(h, "hosttest") == E_OK, "mkdir");
		CHECK(h->fs->write(h, "hosttest/big.bin", 0, sizeof(big), big, true, &done) == E_OK && done == sizeof(big),
		      "a 3 MiB file (%u)", done);
		CHECK(h->fs->write(h, "hosttest/small.txt", 0, 5, "hello", true, &done) == E_OK, "a small file");
		CHECK(writes > 0 && outside == 0, "%ld writes, %ld outside", writes, outside);
		/* read back from the disk, the cache dropped, mounted anew */
		blk_cache_drop();
		CHECK(fs_fat.mount(h) == E_OK, "mounted again");
		static u8 back[3 << 20];
		u32 got = 0;
		CHECK(h->fs->read(h, "hosttest/big.bin", 0, sizeof(back), back, &got) == E_OK && got == sizeof(big) &&
		      !memcmp(back, big, sizeof(big)), "read back (%u)", got);
		/* the container's volume refuses what lies outside it */
		CHECK(vol_wr(h, h->nblocks * h->bsize, 4096, big) != E_OK, "past the container's end");
		if (h->fs->sync) h->fs->sync(h);
	}
	printf("host %s (%s): %d passed, %d failed, %ld writes, %ld outside\n", argv[1], argv[5], passed, failed,
	       writes, outside);
	return failed != 0;
}
