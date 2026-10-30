/* KolibriOS for ARM: the block layer. The disks the drivers add, their
 * partitions (GPT, MBR with its logical partitions, or the whole disk) as
 * volumes with the file system found on them, a block cache, and the
 * fence: the only way to a driver's write entry.
 *
 * The volumes are named as KolibriOS names partitions, disk and number
 * from 1: "ufs0/13" (UFS LUN 0's 13th GPT entry, Linux's sda13),
 * "mmc0/1", "vd0/2"; the numbers are the table's (GPT entry, MBR 1-4,
 * logical ones from 5), as Linux counts them. A disk without a partition
 * table is one volume, "vd1/1".
 *
 * Writing: a disk's write entry (blk_add) is kept here and called only
 * by disk_write, which lets through only blocks inside the fence's
 * ranges. The fence is empty until blk_container adds the extents of the
 * container that home.c has checked; the container's volume is the only
 * one vol_wr writes. Everything else, every partition, every file system
 * on them (ext4 has no write code at all, FAT's is refused here), is read
 * only below the file systems as well.
 *
 * The cache: 4 KiB pages of the disks (absolute, aligned to 4 KiB, so a
 * partition's alignment does not matter), never dirty: what is written
 * goes to the disk at once and into the page. */
#include "storage.h"

#ifndef STORAGE_HOST
addr_t mm_alloc_mb(u32 mb);
#endif

#define BLK_MAX 16
#define VOL_MAX 160
#define FENCE_MAX FMAP_MAX
#define PAGE 4096u
#define CACHE_PAGES 512                /* 2 MiB */
#define BYPASS (64u << 10)             /* reads this large go around the cache */

static struct {
	struct blkdev *d;
	blk_write_fn write;            /* the driver's write entry, only for disk_write */
} disks[BLK_MAX];
static int ndisks;

/* the blocks that may be written: the container's extents (blk_container) */
static struct {
	const struct blkdev *d;
	u64 lba, n;
} fence[FENCE_MAX];
static int nfence;

static struct vol *vols[VOL_MAX];
static int nvols;

int blk_add(struct blkdev *d, blk_write_fn write)
{
	if (ndisks == BLK_MAX || !d->bsize || (d->bsize & (d->bsize - 1)) || d->bsize > PAGE || !d->read) return -1;
	if (!d->max_blocks) d->max_blocks = 1;
	d->index = ndisks;
	disks[ndisks].d = d;
	disks[ndisks].write = write;
	ndisks++;
	slog("blk: %s: %s, %u MiB in %u-byte blocks%s\n", d->name, d->what,
	     (u32)((d->nblocks * d->bsize) >> 20), d->bsize, write ? "" : ", read only");
	return 0;
}

int blk_count(void) { return ndisks; }
struct blkdev *blk_get(int i) { return i >= 0 && i < ndisks ? disks[i].d : NULL; }

bool blk_fence_allows(const struct blkdev *d, u64 lba, u32 n)
{
	if (!n || lba + n < lba) return false;
	for (int i = 0; i < nfence; i++)
		if (fence[i].d == d && lba >= fence[i].lba && lba + n <= fence[i].lba + fence[i].n) return true;
	return false;
}

/* --- the cache ------------------------------------------------------------ */

struct page {
	struct blkdev *d;              /* NULL: free */
	u64 pg;                        /* the page's number on the disk (byte offset / 4096) */
	u8 *data;
	u32 used;                      /* the clock's mark */
};
static struct page cache[CACHE_PAGES];
static u8 *cache_mem;
static u32 hand;

static bool cache_init(void)
{
	if (cache_mem) return true;
#ifdef STORAGE_HOST
	cache_mem = malloc(CACHE_PAGES * PAGE);
#else
	cache_mem = (u8 *)mm_alloc_mb(CACHE_PAGES * PAGE >> 20);
#endif
	if (!cache_mem) return false;
	for (int i = 0; i < CACHE_PAGES; i++) cache[i].data = cache_mem + (u32)i * PAGE;
	return true;
}

void blk_cache_drop(void)
{
	for (int i = 0; i < CACHE_PAGES; i++) cache[i].d = NULL;
}

static struct page *cache_find(struct blkdev *d, u64 pg)
{
	for (int i = 0; i < CACHE_PAGES; i++)
		if (cache[i].d == d && cache[i].pg == pg) {
			cache[i].used = 1;
			return &cache[i];
		}
	return NULL;
}

static struct page *cache_victim(void)
{
	for (;;) {
		struct page *p = &cache[hand];
		hand = (hand + 1) % CACHE_PAGES;
		if (!p->d || !p->used) return p;
		p->used = 0;
	}
}

/* reads of the driver, cut to what it moves at once */
static int dev_read(struct blkdev *d, u64 lba, u64 n, void *buf)
{
	if (lba >= d->nblocks || n > d->nblocks - lba) return E_DEVICE;
	while (n) {
		u32 k = (u32)MIN(n, (u64)d->max_blocks);
		if (d->read(d, lba, k, buf)) {
			slog("blk: %s: read error at block %u (+%u)\n", d->name, (u32)lba, k);
			return E_DEVICE;
		}
		lba += k;
		n -= k;
		buf = (u8 *)buf + (u64)k * d->bsize;
	}
	return E_OK;
}

/* a page of the disk in the cache (the last one of a disk may be short:
 * the rest zeros) */
static struct page *cache_get(struct blkdev *d, u64 pg)
{
	struct page *p = cache_find(d, pg);
	if (p) return p;
	if (!cache_init()) return NULL;
	p = cache_victim();
	u32 per = PAGE / d->bsize;
	u64 lba = pg * per;
	if (lba >= d->nblocks) return NULL;
	u32 n = (u32)MIN((u64)per, d->nblocks - lba);
	p->d = NULL;
	if (n < per) memset(p->data, 0, PAGE);
	if (dev_read(d, lba, n, p->data)) return NULL;
	p->d = d;
	p->pg = pg;
	p->used = 1;
	return p;
}

/* bytes of a disk, at any offset */
static int disk_read(struct blkdev *d, u64 off, u32 len, void *buf)
{
	u8 *b = buf;
	u64 size = d->nblocks * d->bsize;
	if (off > size || len > size - off) return E_DEVICE;
	/* large and block aligned: straight from the disk (the cache holds
	 * nothing the disk does not) */
	if (len >= BYPASS && !(off % d->bsize) && !(len % d->bsize)) {
		/* what was written lately may be in the cache only as it is on
		 * the disk too: nothing to merge */
		return dev_read(d, off / d->bsize, len / d->bsize, b);
	}
	while (len) {
		u64 pg = off / PAGE;
		u32 o = (u32)(off % PAGE), k = MIN(len, PAGE - o);
		struct page *p = cache_get(d, pg);
		if (!p) return E_DEVICE;
		memcpy(b, p->data + o, k);
		b += k;
		off += k;
		len -= k;
	}
	return E_OK;
}

/* the fence: whole blocks of d from lba, every one inside the container's
 * ranges, then the driver's write; the cache's pages follow */
static int disk_write_blocks(struct blkdev *d, u64 lba, u32 n, const u8 *buf)
{
	blk_write_fn w = disks[d->index].d == d ? disks[d->index].write : NULL;
	if (!w || !blk_fence_allows(d, lba, n)) {
		slog("blk: %s: write of blocks %u+%u refused (outside the container)\n", d->name, (u32)lba, n);
		return E_ACCESS;
	}
	for (u32 done = 0; done < n;) {
		u32 k = MIN(n - done, d->max_blocks);
		if (w(d, lba + done, k, buf + (u64)done * d->bsize)) {
			slog("blk: %s: write error at block %u (+%u)\n", d->name, (u32)(lba + done), k);
			blk_cache_drop();
			return E_DEVICE;
		}
		done += k;
	}
	/* the pages these blocks are in, as the disk has them now */
	u64 b0 = lba * d->bsize, b1 = (lba + n) * d->bsize;
	for (u64 pg = b0 / PAGE; pg * PAGE < b1; pg++) {
		struct page *p = cache_find(d, pg);
		if (!p) continue;
		u64 s = MAX(b0, pg * PAGE), e = MIN(b1, (pg + 1) * PAGE);
		memcpy(p->data + (s - pg * PAGE), buf + (s - b0), e - s);
	}
	return E_OK;
}

/* bytes of a disk; blocks written only in part are read first */
static int disk_write(struct blkdev *d, u64 off, u32 len, const u8 *buf)
{
	static u8 tmp[PAGE];
	u32 bs = d->bsize;
	while (len) {
		u64 lba = off / bs;
		u32 o = (u32)(off % bs);
		if (o || len < bs) {
			u32 k = MIN(len, bs - o);
			if (!blk_fence_allows(d, lba, 1)) {
				slog("blk: %s: write of block %u refused (outside the container)\n", d->name, (u32)lba);
				return E_ACCESS;
			}
			int e = disk_read(d, lba * bs, bs, tmp);
			if (e) return e;
			memcpy(tmp + o, buf, k);
			if ((e = disk_write_blocks(d, lba, 1, tmp))) return e;
			off += k, buf += k, len -= k;
			continue;
		}
		u32 n = len / bs;
		int e = disk_write_blocks(d, lba, n, buf);
		if (e) return e;
		off += (u64)n * bs, buf += (u64)n * bs, len -= n * bs;
	}
	return E_OK;
}

/* --- volumes -------------------------------------------------------------- */

int vol_count(void) { return nvols; }
struct vol *vol_get(int i) { return i >= 0 && i < nvols ? vols[i] : NULL; }

struct vol *vol_find(const char *name, u32 len)
{
	for (int i = 0; i < nvols; i++)
		if (strlen(vols[i]->name) == len && !memcmp(vols[i]->name, name, len)) return vols[i];
	return NULL;
}

/* a container's bytes from off on, len of them, on its host volume:
 * the piece in one extent (*hoff there, its length returned) */
static u32 cont_piece(const struct vol *v, u64 off, u32 len, u64 *hoff)
{
	for (int i = 0; i < v->next; i++) {
		if (off < v->ext[i].len) {
			*hoff = v->ext[i].off + off;
			return (u32)MIN((u64)len, v->ext[i].len - off);
		}
		off -= v->ext[i].len;
	}
	return 0;
}

int vol_rd(struct vol *v, u64 off, u32 len, void *buf)
{
	u64 size = v->nblocks * v->bsize;
	if (off > size || len > size - off) return E_DEVICE;
	if (v->host) {
		u8 *b = buf;
		while (len) {
			u64 h;
			u32 k = cont_piece(v, off, len, &h);
			if (!k) return E_DEVICE;
			int e = vol_rd(v->host, h, k, b);
			if (e) return e;
			off += k, b += k, len -= k;
		}
		return E_OK;
	}
	return disk_read(v->disk, v->start * v->bsize + off, len, buf);
}

/* only the container is written, through its extents, through the fence */
int vol_wr(struct vol *v, u64 off, u32 len, const void *buf)
{
	u64 size = v->nblocks * v->bsize;
	if (!v->writable || !v->host) return E_ACCESS;
	if (off > size || len > size - off) return E_DEVICE;
	const u8 *b = buf;
	while (len) {
		u64 h;
		u32 k = cont_piece(v, off, len, &h);
		if (!k) return E_DEVICE;
		struct vol *hv = v->host;
		if (!hv->disk || h > hv->nblocks * hv->bsize || k > hv->nblocks * hv->bsize - h) return E_DEVICE;
		int e = disk_write(hv->disk, hv->start * hv->bsize + h, k, b);
		if (e) return e;
		off += k, b += k, len -= k;
	}
	return E_OK;
}

/* --- partitions --------------------------------------------------------- */

static u32 crc32(const u8 *p, u32 n)
{
	u32 c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static void u32_str(char *o, u32 v)
{
	char t[12];
	int n = 0;
	do t[n++] = '0' + v % 10; while (v /= 10);
	while (n) *o++ = t[--n];
	*o = 0;
}

static struct vol *vol_new(struct blkdev *d, u32 index, u64 start, u64 count, const char *label)
{
	if (nvols == VOL_MAX || !count || start >= d->nblocks) return NULL;
	if (count > d->nblocks - start) count = d->nblocks - start;
	struct vol *v = kmalloc(sizeof(*v));
	if (!v) return NULL;
	memset(v, 0, sizeof(*v));
	u32 l = strlen(d->name);
	memcpy(v->name, d->name, l);
	v->name[l] = '/';
	u32_str(v->name + l + 1, index);
	if (label) {
		u32 i = 0;
		for (; label[i] && i + 1 < sizeof(v->label); i++) v->label[i] = label[i];
		v->label[i] = 0;
	}
	v->disk = d;
	v->start = start;
	v->bsize = d->bsize;
	v->nblocks = count;
	vols[nvols++] = v;
	return v;
}

/* a GPT's header at lba: its entries as volumes */
static int gpt_scan(struct blkdev *d, u64 lba)
{
	static u8 h[PAGE], e[PAGE];
	u32 bs = d->bsize;
	if (disk_read(d, lba * bs, bs, h) || memcmp(h, "EFI PART", 8)) return -1;
	u32 hsize = le32(h + 12), hcrc = le32(h + 16);
	if (hsize < 92 || hsize > bs) return -1;
	put_le32(h + 16, 0);
	if (crc32(h, hsize) != hcrc) {
		slog("blk: %s: GPT at block %u: wrong header checksum\n", d->name, (u32)lba);
		return -1;
	}
	u64 first = le64(h + 40), last = le64(h + 48), ents = le64(h + 72);
	u32 count = le32(h + 80), esize = le32(h + 84), ecrc = le32(h + 88);
	if (esize < 128 || esize > 1024 || (esize & 7) || count > 1024 || last >= d->nblocks || ents >= d->nblocks)
		return -1;
	/* the entries' checksum, then the entries */
	u32 c = 0xffffffffu;
	for (u32 i = 0; i < count; i++) {
		if (disk_read(d, ents * bs + (u64)i * esize, esize, e)) return -1;
		for (u32 j = 0; j < esize; j++) {
			c ^= e[j];
			for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
		}
	}
	if (~c != ecrc) {
		slog("blk: %s: GPT at block %u: wrong entries checksum\n", d->name, (u32)lba);
		return -1;
	}
	int found = 0;
	for (u32 i = 0; i < count; i++) {
		disk_read(d, ents * bs + (u64)i * esize, esize, e);
		static const u8 zero[16];
		if (!memcmp(e, zero, 16)) continue;
		u64 s = le64(e + 32), l = le64(e + 40);
		if (s < first || l < s || l > last) continue;
		char name[37];
		for (int j = 0; j < 36; j++) {
			u16 ch = le16(e + 56 + 2 * j);
			name[j] = ch < 0x80 ? (char)ch : '?';
			if (!ch) break;
		}
		name[36] = 0;
		if (vol_new(d, i + 1, s, l - s + 1, name)) found++;
	}
	return found;
}

/* an MBR's partitions, its extended one's logical ones from 5 */
static int mbr_scan(struct blkdev *d, const u8 *mbr)
{
	static u8 ebr[PAGE];
	int found = 0;
	for (int i = 0; i < 4; i++) {
		const u8 *p = mbr + 446 + 16 * i;
		u8 type = p[4];
		u64 s = le32(p + 8), n = le32(p + 12);
		if (!type || !n) continue;
		if (type == 0x05 || type == 0x0f || type == 0x85) {
			u64 ext = s, at = s;
			for (u32 k = 5; k < 64; k++) {
				if (disk_read(d, at * d->bsize, 512, ebr) || ebr[510] != 0x55 || ebr[511] != 0xaa) break;
				const u8 *q = ebr + 446;
				if (q[4] && le32(q + 12) && vol_new(d, k, at + le32(q + 8), le32(q + 12), NULL)) found++;
				if (!q[16 + 4] || !le32(q + 16 + 8)) break;
				at = ext + le32(q + 16 + 8);
			}
			continue;
		}
		if (vol_new(d, i + 1, s, n, NULL)) found++;
	}
	return found;
}

static void mount(struct vol *v)
{
	static const struct fsops *const fss[] = { &fs_fat, &fs_ext4 };
	for (u32 i = 0; i < ARRAY_SIZE(fss); i++)
		if (fss[i]->mount(v) == E_OK) {
			v->fs = fss[i];
			return;
		}
}

int blk_scan(void)
{
	static u8 b[PAGE];
	for (int i = 0; i < ndisks; i++) {
		struct blkdev *d = disks[i].d;
		int first = nvols, n = -1;
		if (disk_read(d, 0, d->bsize, b)) {
			slog("blk: %s: block 0 not read\n", d->name);
			continue;
		}
		bool mbr = b[510] == 0x55 && b[511] == 0xaa;
		bool protective = mbr && (b[446 + 4] == 0xee || b[462 + 4] == 0xee);
		if (!mbr || protective) {
			n = gpt_scan(d, 1);
			if (n < 0) n = gpt_scan(d, d->nblocks - 1);       /* the backup */
			if (n >= 0) slog("blk: %s: GPT, %d partitions\n", d->name, n);
		}
		/* an MBR, unless block 0 is a FAT boot sector (a disk without a
		 * table: its boot sector has the signature too) */
		if (n < 0 && mbr && !protective && !(b[0] == 0xeb || b[0] == 0xe9)) {
			n = mbr_scan(d, b);
			slog("blk: %s: MBR, %d partitions\n", d->name, n);
		}
		if (n <= 0 && nvols == first) vol_new(d, 1, 0, d->nblocks, NULL);
		for (int k = first; k < nvols; k++) {
			mount(vols[k]);
			struct vol *v = vols[k];
			if (v->fs) slog("blk: %s (%s): %s, %u MiB\n", v->name, v->label[0] ? v->label : "-",
					v->fs->name, (u32)((v->nblocks * v->bsize) >> 20));
		}
	}
	return nvols;
}

/* the container's volume: after home.c's checks, its extents (bytes on
 * host, in whole blocks of host's disk); writable (rw) only with them as
 * the fence's ranges */
struct vol *blk_container(struct vol *host, const struct extent *ext, int n, u64 size, bool rw)
{
	struct blkdev *d = host->disk;
	if (!d || n <= 0 || n > FMAP_MAX || nfence + n > FENCE_MAX || nvols == VOL_MAX || (size % PAGE)) return NULL;
	u64 sum = 0, hsize = host->nblocks * host->bsize;
	for (int i = 0; i < n; i++) {
		if ((ext[i].off % d->bsize) || (ext[i].len % d->bsize) || !ext[i].len ||
		    ext[i].off > hsize || ext[i].len > hsize - ext[i].off)
			return NULL;
		sum += ext[i].len;
	}
	if (sum != size) return NULL;
	struct vol *v = kmalloc(sizeof(*v));
	struct extent *e = kmalloc(n * sizeof(*e));
	if (!v || !e) return NULL;
	memset(v, 0, sizeof(*v));
	v->ext = e;
	memcpy(v->name, "hd0/1", 6);
	v->host = host;
	v->bsize = PAGE;
	v->nblocks = size / PAGE;
	v->next = n;
	for (int i = 0; i < n; i++) v->ext[i] = ext[i];
	for (int i = 0; rw && i < n; i++) {
		fence[nfence].d = d;
		fence[nfence].lba = host->start + ext[i].off / d->bsize;
		fence[nfence].n = ext[i].len / d->bsize;
		nfence++;
	}
	v->writable = rw;
	vols[nvols++] = v;
	return v;
}
