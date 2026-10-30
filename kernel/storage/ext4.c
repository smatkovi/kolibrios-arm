/* KolibriOS for ARM: ext2, ext3 and ext4, read only.
 *
 * Written from the on-disk format as kernel.org's "ext4 Data Structures
 * and Algorithms" describes it. What is read:
 *   - the superblock at byte 1024; blocks of 1 KiB to 64 KiB; group
 *     descriptors of 32 or 64 bytes (64bit: their _hi halves too);
 *     flex_bg, meta_bg, sparse_super and sparse_super2
 *   - a file's blocks by extents (any depth; unwritten extents and holes
 *     read as zeros) or by ext2/3's direct, indirect, double and triple
 *     indirect blocks; inline data (small files and folders kept in the
 *     inode and its system.data attribute)
 *   - folders block by block: an htree's index blocks look like empty
 *     entries, so they need no reading of their own; "." and ".." left
 *     out. Names are their bytes (UTF-8), compared exactly (also in
 *     casefold folders: the name as it was created)
 *   - symlinks are not followed: a file whose content is the target
 *     (a fast one's in the inode); devices, fifos and sockets are empty
 *     files with A_SYSTEM
 * Refused at mount: a journal device, compression, dirdata and every
 * incompatible feature not known here. Encrypted files and folders:
 * E_ACCESS (their names and data are cipher text). A journal that needs
 * recovery (Linux has it mounted, or crashed), a file system not cleanly
 * unmounted or with errors recorded: mounted with a warning, the metadata
 * may be stale; map() refuses then. bigalloc: read, map() refuses.
 * Nothing is ever written: there are no write functions.
 *
 * map() is the container's safety fence (home.c): it lists a regular
 * file's blocks only if each of them is inside the volume, set in its
 * group's block bitmap, no group's metadata (superblock and descriptor
 * backups with the reserved descriptor blocks, bitmaps, inode tables, as
 * the descriptors place them), not the journal's, not the MMP block, not
 * a block of the file's own extent tree or indirect blocks, and not in
 * the file twice; the file has no hole below its size, no unwritten
 * extent, no inline data, and is neither encrypted nor verity. */
#include "storage.h"

/* the superblock: offsets in its 1024 bytes */
#define SB_INODES 0x00
#define SB_BLOCKS 0x04
#define SB_FIRST_DATA 0x14
#define SB_LOG_BSIZE 0x18
#define SB_LOG_CSIZE 0x1c
#define SB_BPG 0x20
#define SB_CPG 0x24
#define SB_IPG 0x28
#define SB_MAGIC 0x38
#define SB_STATE 0x3a
#define SB_REV 0x4c
#define SB_ISIZE 0x58
#define SB_COMPAT 0x5c
#define SB_INCOMPAT 0x60
#define SB_RO_COMPAT 0x64
#define SB_NAME 0x78
#define SB_RESERVED_GDT 0xce
#define SB_JOURNAL_INO 0xe0
#define SB_DESC_SIZE 0xfe
#define SB_FIRST_META_BG 0x104
#define SB_BLOCKS_HI 0x150
#define SB_MMP_BLOCK 0x168
#define SB_BACKUP_BGS 0x24c

#define EXT_SB_MAGIC 0xef53
#define ST_VALID 1              /* s_state: cleanly unmounted */
#define ST_ERRORS 2             /* errors detected */

/* features */
#define CO_JOURNAL 0x0004
#define CO_EXT_ATTR 0x0008
#define CO_RESIZE_INODE 0x0010
#define CO_DIR_INDEX 0x0020
#define CO_SPARSE_SUPER2 0x0200
#define RO_SPARSE_SUPER 0x0001
#define RO_LARGE_FILE 0x0002
#define RO_HUGE_FILE 0x0008
#define RO_GDT_CSUM 0x0010
#define RO_BIGALLOC 0x0200
#define RO_METADATA_CSUM 0x0400
#define IN_COMPRESSION 0x0001
#define IN_FILETYPE 0x0002
#define IN_RECOVER 0x0004
#define IN_JOURNAL_DEV 0x0008
#define IN_META_BG 0x0010
#define IN_EXTENTS 0x0040
#define IN_64BIT 0x0080
#define IN_MMP 0x0100
#define IN_FLEX_BG 0x0200
#define IN_EA_INODE 0x0400
#define IN_DIRDATA 0x1000
#define IN_CSUM_SEED 0x2000
#define IN_LARGEDIR 0x4000
#define IN_INLINE_DATA 0x8000
#define IN_ENCRYPT 0x10000
#define IN_CASEFOLD 0x20000
#define IN_KNOWN (IN_FILETYPE | IN_RECOVER | IN_META_BG | IN_EXTENTS | IN_64BIT | IN_MMP | \
		  IN_FLEX_BG | IN_EA_INODE | IN_CSUM_SEED | IN_LARGEDIR | IN_INLINE_DATA | \
		  IN_ENCRYPT | IN_CASEFOLD)

#define BG_BLOCK_UNINIT 0x0002  /* the group's block bitmap never written: all free */

/* inodes */
#define M_FMT 0xf000
#define M_DIR 0x4000
#define M_REG 0x8000
#define M_LNK 0xa000
#define FL_IMMUTABLE 0x10
#define FL_ENCRYPT 0x800
#define FL_EXTENTS 0x80000
#define FL_VERITY 0x100000
#define FL_INLINE 0x10000000
#define ROOT_INO 2

#define EXT_MAGIC 0xf30a        /* an extent tree node's header */
#define EXT_INIT_MAX 32768      /* longer: an unwritten extent of len - 32768 */
#define XATTR_MAGIC 0xea020000
#define INLINE_SIZE 60          /* inline data in i_block */

struct ext4 {
	u64 blocks;             /* s_blocks_count */
	u32 bs, bshift;         /* the block size, its log2 */
	u32 fdb;                /* s_first_data_block */
	u32 sb_blk;             /* the block the superblock is in */
	u32 bpg, ipg, isz;      /* blocks and inodes a group, an inode's bytes */
	u32 inodes, ngroups;
	u32 desc_size, dpb_shift;   /* a descriptor's bytes; log2 of descriptors a block */
	u32 gdb;                /* descriptor blocks */
	u32 first_meta_bg, reserved_gdt;
	u32 itb;                /* inode table blocks a group */
	u32 compat, incompat, ro_compat;
	u32 backup_bgs[2];      /* sparse_super2 */
	u32 journal_ino;
	u64 mmp_block;
	u32 cluster;            /* bytes a cluster (bigalloc), else a block */
	bool needs_recovery;    /* the journal has to be replayed first */
	bool unclean;           /* not cleanly unmounted, or errors recorded */
	const struct fmap *fence;   /* map(): no tree block may lie in these */
	u64 fence_hit;          /* the first that did, 0: none */
	u8 *dirbuf, *nodebuf;   /* a block each: a folder's, a tree node's */
};

/* an inode, its fields taken out */
struct inode {
	u32 num;
	u16 mode;
	u32 flags;
	u64 size;
	u32 atime, ctime, mtime;
	u64 blocks;             /* i_blocks, in 512 bytes (fast symlinks: 0) */
	u64 file_acl;           /* its attribute block */
	u32 extra;              /* i_extra_isize */
	u64 at;                 /* its bytes on the volume */
	u8 blk[60];             /* i_block */
};

/* a run of a file's blocks from a logical block on */
enum { R_DATA, R_HOLE, R_ZERO };   /* on the disk, a hole, unwritten */
struct run {
	u64 pblk;
	u64 n;
	int kind;
};

#define STOP (-1)               /* a folder walk ended by its callback */

static int rd(struct vol *v, u64 off, u32 len, void *buf)
{
	return vol_rd(v, off, len, buf) ? E_DEVICE : E_OK;
}

/* a 64-bit number for the log (two at once) */
static const char *dec(u64 n)
{
	static char b[2][24];
	static int k;
	char *p = b[k ^= 1] + 23;
	*p = 0;
	do *--p = '0' + n % 10; while (n /= 10);
	return p;
}

/* --- groups ------------------------------------------------------------- */

static u64 group_first(const struct ext4 *x, u32 g)
{
	return x->fdb + (u64)g * x->bpg;
}

static bool is_pow(u32 g, u32 b)
{
	u64 p = b;
	while (p < g) p *= b;
	return p == g;
}

/* has group g a copy of the superblock? */
static bool has_super(const struct ext4 *x, u32 g)
{
	if (g == 0) return true;
	if (x->compat & CO_SPARSE_SUPER2) return g == x->backup_bgs[0] || g == x->backup_bgs[1];
	if (g == 1 || !(x->ro_compat & RO_SPARSE_SUPER)) return true;
	if (!(g & 1)) return false;
	return is_pow(g, 3) || is_pow(g, 5) || is_pow(g, 7);
}

/* the blocks at a group's start that hold the superblock and descriptor
 * copies (with the reserved descriptor blocks) */
static u32 base_meta(const struct ext4 *x, u32 g)
{
	u32 n = has_super(x, g), dpb = 1u << x->dpb_shift;
	if (!(x->incompat & IN_META_BG) || g < (u64)x->first_meta_bg * dpb) {
		if (n) n += ((x->incompat & IN_META_BG) ? x->first_meta_bg : x->gdb) + x->reserved_gdt;
	} else {
		u32 r = g & (dpb - 1);
		if (r == 0 || r == 1 || r == dpb - 1) n++;
	}
	return n;
}

struct gdesc {
	u64 bbitmap, ibitmap, itable;
	u32 flags;
};

static int gd_get(struct vol *v, const struct ext4 *x, u32 g, struct gdesc *d)
{
	if (g >= x->ngroups) return E_FS;
	u32 i = g >> x->dpb_shift;      /* its descriptor block */
	u64 blk;
	if (!(x->incompat & IN_META_BG) || i < x->first_meta_bg)
		blk = x->sb_blk + 1 + i;
	else {
		/* meta_bg: in the first group of its meta group */
		u32 bg = i << x->dpb_shift;
		blk = group_first(x, bg) + has_super(x, bg);
		if (x->bs == 1024 && i == 0 && x->fdb == 0) blk++;
	}
	if (blk >= x->blocks) return E_FS;
	u8 b[64];
	u64 at = (blk << x->bshift) + (u64)(g & ((1u << x->dpb_shift) - 1)) * x->desc_size;
	int rc = rd(v, at, x->desc_size >= 64 ? 64 : 32, b);
	if (rc) return rc;
	d->bbitmap = le32(b + 0x00);
	d->ibitmap = le32(b + 0x04);
	d->itable = le32(b + 0x08);
	d->flags = le16(b + 0x12);
	if (x->incompat & IN_64BIT) {
		d->bbitmap |= (u64)le32(b + 0x20) << 32;
		d->ibitmap |= (u64)le32(b + 0x24) << 32;
		d->itable |= (u64)le32(b + 0x28) << 32;
	}
	return E_OK;
}

/* --- inodes ------------------------------------------------------------- */

static int ino_get(struct vol *v, const struct ext4 *x, u32 n, struct inode *in)
{
	if (n < 1 || n > x->inodes) return E_FS;
	u32 g = (n - 1) / x->ipg, i = (n - 1) % x->ipg;
	struct gdesc d;
	int rc = gd_get(v, x, g, &d);
	if (rc) return rc;
	if (d.itable <= x->fdb || d.itable >= x->blocks || x->blocks - d.itable < x->itb) return E_FS;
	u8 b[160];
	in->at = (d.itable << x->bshift) + (u64)i * x->isz;
	rc = rd(v, in->at, MIN(x->isz, 160u), b);
	if (rc) return rc;
	in->num = n;
	in->mode = le16(b + 0x00);
	in->flags = le32(b + 0x20);
	in->size = le32(b + 0x04);
	if ((in->mode & M_FMT) == M_REG || (x->incompat & IN_LARGEDIR))
		in->size |= (u64)le32(b + 0x6c) << 32;
	in->atime = le32(b + 0x08);
	in->ctime = le32(b + 0x0c);
	in->mtime = le32(b + 0x10);
	in->blocks = le32(b + 0x1c);
	if (x->ro_compat & RO_HUGE_FILE) in->blocks |= (u64)le16(b + 0x74) << 32;
	in->file_acl = le32(b + 0x68);
	if (x->incompat & IN_64BIT) in->file_acl |= (u64)le16(b + 0x76) << 32;
	in->extra = x->isz > 128 ? le16(b + 0x80) : 0;
	if (128 + in->extra > x->isz || (in->extra & 3)) in->extra = x->isz - 128;   /* no attributes then */
	memcpy(in->blk, b + 0x28, 60);
	/* a free inode (a folder entry to it is stale) */
	if (!in->mode || !le16(b + 0x1a)) return E_FS;
	return E_OK;
}

/* the system.data attribute in the inode's body: inline data past the
 * first 60 bytes; *size 0 if there is none */
static int inline_attr(struct vol *v, const struct ext4 *x, const struct inode *in, u64 *at, u32 *size)
{
	u32 start = 128 + in->extra, pos = start + 4;
	u8 e[20];
	*size = 0;
	if (pos > x->isz) return E_OK;
	int rc = rd(v, in->at + start, 4, e);
	if (rc || le32(e) != XATTR_MAGIC) return rc;
	/* entries of 16 bytes and the name (to 4 bytes), until 4 zero bytes;
	 * values from the first entry on */
	while (pos + 4 <= x->isz) {
		u32 avail = MIN(20u, x->isz - pos);
		rc = rd(v, in->at + pos, avail, e);
		if (rc) return rc;
		if (!le32(e)) break;
		u32 esz = (16 + e[0] + 3) & ~3u;
		if (avail < 16 || esz > x->isz - pos) return E_FS;
		if (e[1] == 7 && e[0] == 4 && !memcmp(e + 16, "data", 4)) {
			u32 voff = le16(e + 2), vsz = le32(e + 8);
			if (le32(e + 4)) return E_UNSUPPORTED;     /* the value in an EA inode */
			if ((u64)start + 4 + voff + vsz > x->isz) return E_FS;
			*at = in->at + start + 4 + voff;
			*size = vsz;
			return E_OK;
		}
		pos += esz;
	}
	return E_OK;
}

static bool fast_symlink(const struct ext4 *x, const struct inode *in)
{
	if ((in->mode & M_FMT) != M_LNK || (in->flags & FL_INLINE)) return false;
	return in->blocks == (in->file_acl ? x->cluster >> 9 : 0);
}

/* --- a file's blocks ---------------------------------------------------- */

/* a tree block map() is checking (x->fence): not in the file */
static void fence_node(struct ext4 *x, u64 b)
{
	const struct fmap *m = x->fence;
	if (x->fence_hit) return;
	for (int i = 0; i < m->n; i++)
		if ((b << x->bshift) < m->ext[i].off + m->ext[i].len && m->ext[i].off < (b + 1) << x->bshift)
			x->fence_hit = b;
}

/* extents: the run at lblk down the tree (root in i_block, at most 5 levels) */
static int ext_map(struct vol *v, struct ext4 *x, const struct inode *in, u32 lblk, struct run *r)
{
	const u8 *n = in->blk;
	u32 cap = 4;                    /* entries a node holds */
	u64 end = 1ull << 32;           /* where the next entry of a node above starts */
	u32 depth = 0;
	for (u32 level = 0; level <= 5; level++) {
		u32 cnt = le16(n + 2), max = le16(n + 4), d = le16(n + 6);
		if (le16(n) != EXT_MAGIC || cnt > max || max > cap) return E_FS;
		if (level == 0) {
			if (d > 5) return E_FS;
			depth = d;
		} else if (d != depth - level) return E_FS;
		if (!cnt || le32(n + 12) > lblk) {
			/* nothing here at lblk: a hole up to the first entry */
			u64 next = cnt ? MIN((u64)le32(n + 12), end) : end;
			r->kind = R_HOLE;
			r->n = next - lblk;
			return E_OK;
		}
		/* the last entry starting at or before lblk */
		u32 lo = 0, hi = cnt;
		while (hi - lo > 1) {
			u32 mid = (lo + hi) / 2;
			if (le32(n + 12 + mid * 12) <= lblk) lo = mid;
			else hi = mid;
		}
		const u8 *e = n + 12 + lo * 12;
		if (lo + 1 < cnt) {
			u64 next = le32(e + 12);
			if (next <= lblk) return E_FS;
			if (next < end) end = next;
		}
		if (d == 0) {
			u64 first = le32(e), len = le16(e + 4);
			u64 start = (u64)le16(e + 6) << 32 | le32(e + 8);
			int kind = R_DATA;
			if (len > EXT_INIT_MAX) len -= EXT_INIT_MAX, kind = R_ZERO;
			if (!len) return E_FS;
			if (lblk >= first + len) {
				r->kind = R_HOLE;
				r->n = end - lblk;
				return E_OK;
			}
			if (kind == R_DATA && (start <= x->fdb || start >= x->blocks || x->blocks - start < len))
				return E_FS;
			r->kind = kind;
			r->pblk = start + (lblk - first);
			r->n = MIN(first + len, end) - lblk;
			return E_OK;
		}
		u64 child = le32(e + 4) | (u64)le16(e + 8) << 32;
		if (child <= x->fdb || child >= x->blocks) return E_FS;
		if (x->fence) fence_node(x, child);
		int rc = rd(v, child << x->bshift, x->bs, x->nodebuf);
		if (rc) return rc;
		n = x->nodebuf;
		cap = (x->bs - 12) / 12;
	}
	return E_FS;
}

static int ptr_run(const struct ext4 *x, u32 b, u32 n, struct run *r)
{
	r->n = n;
	r->kind = b ? R_DATA : R_HOLE;
	r->pblk = b;
	if (b && (b <= x->fdb || (u64)b + n > x->blocks)) return E_FS;
	return E_OK;
}

/* ext2/3: 12 direct blocks, then an indirect, a double and a triple
 * indirect one; physically adjacent blocks are one run */
static int ind_map(struct vol *v, struct ext4 *x, const struct inode *in, u32 lblk, struct run *r)
{
	u32 pshift = x->bshift - 2, p = 1u << pshift;
	if (lblk < 12) {
		u32 b = le32(in->blk + lblk * 4), k = lblk + 1;
		while (k < 12 && le32(in->blk + k * 4) == (b ? b + (k - lblk) : 0)) k++;
		return ptr_run(x, b, k - lblk, r);
	}
	u64 rel = lblk - 12, span = p;
	u32 levels = 1;
	while (rel >= span) {
		rel -= span;
		if (++levels > 3) return E_FS;     /* past the triple indirect block */
		span <<= pshift;
	}
	u32 b = le32(in->blk + (11 + levels) * 4);
	for (u32 l = levels; ; l--) {
		/* b: a block of level l over span blocks, rel in them */
		if (!b) {
			r->kind = R_HOLE;
			r->n = span - rel;
			return E_OK;
		}
		if (b <= x->fdb || b >= x->blocks) return E_FS;
		if (x->fence) fence_node(x, b);
		u32 sh = pshift * (l - 1);
		u32 idx = rel >> sh;
		u64 at = ((u64)b << x->bshift) + idx * 4;
		rel &= (1ull << sh) - 1;
		span = 1ull << sh;
		if (l == 1) {
			u32 cnt = p - idx;
			int rc = rd(v, at, cnt * 4, x->nodebuf);
			if (rc) return rc;
			u32 d = le32(x->nodebuf), k = 1;
			while (k < cnt && le32(x->nodebuf + k * 4) == (d ? d + k : 0)) k++;
			return ptr_run(x, d, k, r);
		}
		u8 w[4];
		int rc = rd(v, at, 4, w);
		if (rc) return rc;
		b = le32(w);
	}
}

static int bmap(struct vol *v, struct ext4 *x, const struct inode *in, u64 lblk, struct run *r)
{
	if (lblk >> 32) return E_FS;
	int rc = (in->flags & FL_EXTENTS) ? ext_map(v, x, in, lblk, r) : ind_map(v, x, in, lblk, r);
	if (!rc && !r->n) rc = E_FS;
	return rc;
}

/* len bytes at off of a file or symlink, off + len within its size */
static int file_read(struct vol *v, struct ext4 *x, const struct inode *in, u64 off, u32 len, u8 *buf)
{
	if (in->flags & FL_INLINE) {
		/* i_block, then the attribute's value, then zeros (as Linux
		 * reads it: mkfs -d makes an empty sparse file so) */
		u64 at = 0;
		u32 vsz, done = 0;
		int rc = inline_attr(v, x, in, &at, &vsz);
		if (rc) return rc;
		if (off < INLINE_SIZE) {
			done = MIN(len, INLINE_SIZE - (u32)off);
			memcpy(buf, in->blk + off, done);
		}
		if (done < len && off + done < INLINE_SIZE + (u64)vsz) {
			u32 n = MIN(len - done, INLINE_SIZE + vsz - (u32)(off + done));
			if ((rc = rd(v, at + (off + done - INLINE_SIZE), n, buf + done))) return rc;
			done += n;
		}
		memset(buf + done, 0, len - done);
		return E_OK;
	}
	if (fast_symlink(x, in)) {
		if (in->size >= INLINE_SIZE) return E_FS;
		memcpy(buf, in->blk + off, len);
		return E_OK;
	}
	for (u32 done = 0; done < len;) {
		u64 pos = off + done;
		u32 in_blk = pos & (x->bs - 1);
		struct run r;
		int rc = bmap(v, x, in, pos >> x->bshift, &r);
		if (rc) return rc;
		/* the run's bytes from pos, as much as is wanted */
		u64 n = len - done;
		if (r.n < ((n + in_blk + x->bs - 1) >> x->bshift)) n = (r.n << x->bshift) - in_blk;
		if (r.kind != R_DATA) memset(buf + done, 0, n);
		else if ((rc = rd(v, (r.pblk << x->bshift) + in_blk, n, buf + done))) return rc;
		done += n;
	}
	return E_OK;
}

/* --- folders ------------------------------------------------------------ */

typedef int (*dirent_fn)(void *ctx, const u8 *name, u32 len, u32 ino);

static u32 rec_len(const struct ext4 *x, u32 v)
{
	if (x->bs < 65536) return v;
	if (v == 65535 || v == 0) return 65536;
	return (v & 65532) | (v & 3) << 16;
}

/* the entries in a block (or in inline data): fn for each but "." and ".." */
static int dir_block(const struct ext4 *x, const u8 *b, u32 size, dirent_fn fn, void *ctx)
{
	for (u32 off = 0; off < size;) {
		const u8 *e = b + off;
		if (size - off < 8) return E_FS;
		u32 ino = le32(e), rl = rec_len(x, le16(e + 4));
		u32 nl = (x->incompat & IN_FILETYPE) ? e[6] : le16(e + 6);
		if (rl < 8 || (rl & 3) || rl > size - off || nl > rl - 8 || nl > 255) return E_FS;
		if (ino && nl && !(e[8] == '.' && (nl == 1 || (nl == 2 && e[9] == '.'))))
			if (fn(ctx, e + 8, nl, ino)) return STOP;
		off += rl;
	}
	return E_OK;
}

static int dir_iter(struct vol *v, struct ext4 *x, const struct inode *in, dirent_fn fn, void *ctx)
{
	if (in->flags & FL_ENCRYPT) return E_ACCESS;
	if (in->flags & FL_INLINE) {
		/* the parent's number, entries in the rest of i_block, more in
		 * the attribute */
		int rc = dir_block(x, in->blk + 4, INLINE_SIZE - 4, fn, ctx);
		u64 at;
		u32 vsz;
		if (!rc) rc = inline_attr(v, x, in, &at, &vsz);
		if (rc || !vsz) return rc;
		if ((rc = rd(v, at, vsz, x->dirbuf))) return rc;
		return dir_block(x, x->dirbuf, vsz, fn, ctx);
	}
	u64 nblk = (in->size + x->bs - 1) >> x->bshift;
	if (nblk > x->blocks) nblk = x->blocks;
	for (u64 lb = 0; lb < nblk;) {
		struct run r;
		int rc = bmap(v, x, in, lb, &r);
		if (rc) return rc;
		if (r.kind != R_DATA) {
			lb += r.n;
			continue;
		}
		for (u64 k = 0; k < r.n && lb < nblk; k++, lb++) {
			rc = rd(v, (r.pblk + k) << x->bshift, x->bs, x->dirbuf);
			if (!rc) rc = dir_block(x, x->dirbuf, x->bs, fn, ctx);
			if (rc) return rc;
		}
	}
	return E_OK;
}

struct find {
	const char *name;
	u32 len, ino;
};

static int find_fn(void *ctx, const u8 *name, u32 len, u32 ino)
{
	struct find *f = ctx;
	if (len != f->len || memcmp(name, f->name, len)) return 0;
	f->ino = ino;
	return 1;
}

/* the inode at path ("" the root) */
static int lookup(struct vol *v, struct ext4 *x, const char *path, struct inode *in)
{
	int rc = ino_get(v, x, ROOT_INO, in);
	while (!rc && *path) {
		const char *q = path;
		while (*q && *q != '/') q++;
		struct find f = { path, (u32)(q - path), 0 };
		if ((in->mode & M_FMT) != M_DIR || !f.len || f.len > 255) return E_NOT_FOUND;
		rc = dir_iter(v, x, in, find_fn, &f);
		if (rc != STOP) return rc ? rc : E_NOT_FOUND;
		rc = ino_get(v, x, f.ino, in);
		path = *q ? q + 1 : q;
	}
	return rc;
}

static void fill(const struct inode *in, const void *name, u32 len, struct vdirent *e)
{
	switch (in->mode & M_FMT) {
	case M_DIR: e->attr = A_DIR; e->size = 0; break;
	case M_REG: case M_LNK: e->attr = A_ARCH; e->size = in->size; break;
	default: e->attr = A_SYSTEM; e->size = 0; break;   /* devices, fifos, sockets */
	}
	if (!(in->mode & 0200)) e->attr |= A_RO;
	if (len && *(const char *)name == '.') e->attr |= A_HIDDEN;
	if (len > sizeof(e->name) - 1) len = sizeof(e->name) - 1;
	memcpy(e->name, name, len);
	e->name[len] = 0;
	e->ctime = in->ctime;
	e->atime = in->atime;
	e->mtime = in->mtime;
}

/* --- the file system's functions ------------------------------------- */

static int ext4_info(struct vol *v, const char *path, struct vdirent *e)
{
	struct inode in;
	int rc = lookup(v, v->fsd, path, &in);
	if (rc) return rc;
	const char *name = path;
	for (const char *p = path; *p; p++)
		if (*p == '/') name = p + 1;
	fill(&in, name, strlen(name), e);
	return E_OK;
}

static int ext4_read(struct vol *v, const char *path, u64 off, u32 len, void *buf, u32 *got)
{
	struct ext4 *x = v->fsd;
	struct inode in;
	*got = 0;
	int rc = lookup(v, x, path, &in);
	if (rc) return rc;
	u32 type = in.mode & M_FMT;
	if (type == M_DIR) return E_ACCESS;
	if (type != M_REG && type != M_LNK) return E_OK;    /* devices and the like: empty */
	if (in.flags & FL_ENCRYPT) return E_ACCESS;
	if (off >= in.size) return E_OK;
	if (len > in.size - off) len = in.size - off;
	rc = file_read(v, x, &in, off, len, buf);
	if (!rc) *got = len;
	return rc;
}

struct list {
	struct vol *v;
	struct ext4 *x;
	u32 first, count, total;
	void (*emit)(void *ctx, const struct vdirent *e);
	void *ctx;
	int rc;
	bool logged;            /* a bad entry told already */
};

static struct vdirent ent;      /* readdir's entry */

static int list_fn(void *ctx, const u8 *name, u32 len, u32 ino)
{
	struct list *l = ctx;
	u32 i = l->total++;
	if (i < l->first || i - l->first >= l->count || !l->emit) return 0;
	struct inode in;
	int rc = ino_get(l->v, l->x, ino, &in);
	if (rc == E_DEVICE) {
		l->rc = rc;
		return 1;
	}
	if (rc) {
		/* a broken entry: shown as an empty system file */
		if (!l->logged) slog("ext4 %s: entry %u: bad inode %u\n", l->v->name, i, ino);
		l->logged = true;
		memset(&in, 0, sizeof(in));
	}
	fill(&in, name, len, &ent);
	l->emit(l->ctx, &ent);
	return 0;
}

static int ext4_readdir(struct vol *v, const char *path, u32 first, u32 count,
			void (*emit)(void *ctx, const struct vdirent *e), void *ctx, u32 *total)
{
	struct ext4 *x = v->fsd;
	struct inode in;
	struct list l = { v, x, first, count, 0, emit, ctx, E_OK, false };
	if (total) *total = 0;
	int rc = lookup(v, x, path, &in);
	if (rc) return rc;
	if ((in.mode & M_FMT) != M_DIR) return E_ACCESS;
	rc = dir_iter(v, x, &in, list_fn, &l);
	if (rc == STOP) rc = l.rc;
	if (total) *total = l.total;
	return rc;
}

/* --- map(): the container's fence ---------------------------------------- */

/* does any of m's extents touch blocks [b, b + n)? */
static bool hits(const struct ext4 *x, const struct fmap *m, u64 b, u64 n)
{
	u64 lo = b << x->bshift, hi = (b + n) << x->bshift;
	for (int i = 0; i < m->n; i++)
		if (m->ext[i].off < hi && lo < m->ext[i].off + m->ext[i].len) return true;
	return false;
}

static u8 bm[512];              /* a piece of a block bitmap */

/* are blocks [b, b + n) all in use in their groups' bitmaps? */
static int fence_bitmap(struct vol *v, struct ext4 *x, u64 b, u64 n)
{
	for (u64 end = b + n; b < end;) {
		u32 g = (b - x->fdb) / x->bpg;
		u64 first = group_first(x, g), stop = MIN(end, first + x->bpg);
		struct gdesc d;
		int rc = gd_get(v, x, g, &d);
		if (rc) return rc;
		if (d.flags & BG_BLOCK_UNINIT) {
			slog("ext4 %s: no map: group %u's blocks all free (BLOCK_UNINIT)\n", v->name, g);
			return E_FS;
		}
		if (d.bbitmap <= x->fdb || d.bbitmap >= x->blocks) return E_FS;
		u32 i = b - first, e = stop - first;    /* bits */
		while (i < e) {
			u32 byte0 = i >> 3, nb = MIN((e - 1) / 8 - byte0 + 1, (u32)sizeof(bm));
			u32 lim = MIN(e, (byte0 + nb) * 8);
			if ((rc = rd(v, (d.bbitmap << x->bshift) + byte0, nb, bm))) return rc;
			for (; i < lim; i++) {
				u8 c = bm[(i >> 3) - byte0];
				if (!(i & 7) && i + 8 <= lim && c == 0xff) {
					i += 7;
					continue;
				}
				if (!(c >> (i & 7) & 1)) {
					slog("ext4 %s: no map: block %s is free in the bitmap\n", v->name, dec(first + i));
					return E_FS;
				}
			}
		}
		b = stop;
	}
	return E_OK;
}

/* is none of m's blocks a group's metadata? */
static int fence_groups(struct vol *v, struct ext4 *x, const struct fmap *m)
{
	for (u32 g = 0; g < x->ngroups; g++) {
		u32 n = base_meta(x, g);
		struct gdesc d;
		if (n && hits(x, m, group_first(x, g), n)) {
			slog("ext4 %s: no map: superblock or descriptors of group %u in the file\n", v->name, g);
			return E_FS;
		}
		int rc = gd_get(v, x, g, &d);
		if (rc) return rc;
		if (hits(x, m, d.bbitmap, 1) || hits(x, m, d.ibitmap, 1) || hits(x, m, d.itable, x->itb)) {
			slog("ext4 %s: no map: bitmaps or inode table of group %u in the file\n", v->name, g);
			return E_FS;
		}
	}
	return E_OK;
}

/* an inode's runs over blocks [0, nblk) (tree blocks checked against
 * x->fence on the way); m: its blocks may not be in m either */
static int walk(struct vol *v, struct ext4 *x, const struct inode *in, u64 nblk, const struct fmap *m)
{
	for (u64 lb = 0; lb < nblk;) {
		struct run r;
		int rc = bmap(v, x, in, lb, &r);
		if (rc) return rc;
		u64 n = MIN(r.n, nblk - lb);
		if (m && r.kind != R_HOLE && !x->fence_hit && hits(x, m, r.pblk, n)) x->fence_hit = r.pblk;
		lb += n;
	}
	return E_OK;
}

#define REFUSE(...) do { slog(__VA_ARGS__); m->n = 0; return E_FS; } while (0)

static int ext4_map(struct vol *v, const char *path, struct fmap *m)
{
	struct ext4 *x = v->fsd;
	struct inode in;
	memset(m, 0, sizeof(*m));
	if (x->needs_recovery) REFUSE("ext4 %s: no map: the journal needs recovery\n", v->name);
	if (x->unclean) REFUSE("ext4 %s: no map: not cleanly unmounted, or errors recorded\n", v->name);
	if (x->ro_compat & RO_BIGALLOC) {
		slog("ext4 %s: no map: bigalloc\n", v->name);
		return E_UNSUPPORTED;
	}
	int rc = lookup(v, x, path, &in);
	if (rc) return rc;
	if ((in.mode & M_FMT) != M_REG) REFUSE("ext4 %s: no map: %s is no regular file\n", v->name, path);
	if (in.flags & (FL_ENCRYPT | FL_INLINE | FL_VERITY))
		REFUSE("ext4 %s: no map: %s is encrypted, inline or verity (flags 0x%x)\n", v->name, path, in.flags);
	u64 nblk = (in.size + x->bs - 1) >> x->bshift;
	if (nblk > x->blocks) REFUSE("ext4 %s: no map: %s larger than the volume\n", v->name, path);
	m->id = in.num;
	m->size = in.size;
	if (in.flags & FL_IMMUTABLE) m->flags |= FMAP_IMMUTABLE;

	/* the runs, adjacent ones joined */
	for (u64 lb = 0; lb < nblk;) {
		struct run r;
		if ((rc = bmap(v, x, &in, lb, &r))) {
			m->n = 0;
			return rc;
		}
		if (r.kind != R_DATA)
			REFUSE("ext4 %s: no map: %s at block %s\n", v->name,
			       r.kind == R_HOLE ? "a hole" : "an unwritten extent", dec(lb));
		u64 n = MIN(r.n, nblk - lb), off = r.pblk << x->bshift;
		struct extent *last = m->n ? &m->ext[m->n - 1] : NULL;
		if (last && last->off + last->len == off) last->len += n << x->bshift;
		else if (m->n == FMAP_MAX) REFUSE("ext4 %s: no map: more than %d extents\n", v->name, FMAP_MAX);
		else m->ext[m->n].off = off, m->ext[m->n++].len = n << x->bshift;
		lb += n;
	}

	/* no block twice, each one in use and no group's metadata */
	for (int i = 0; i < m->n; i++) {
		u64 b = m->ext[i].off >> x->bshift, n = m->ext[i].len >> x->bshift;
		for (int j = 0; j < i; j++)
			if (m->ext[j].off < m->ext[i].off + m->ext[i].len && m->ext[i].off < m->ext[j].off + m->ext[j].len)
				REFUSE("ext4 %s: no map: blocks in the file twice\n", v->name);
		if (b <= x->fdb || b >= x->blocks || x->blocks - b < n)
			REFUSE("ext4 %s: no map: block %s outside the volume's data\n", v->name, dec(b));
		if ((rc = fence_bitmap(v, x, b, n))) {
			m->n = 0;
			return rc;
		}
	}
	if ((rc = fence_groups(v, x, m))) {
		m->n = 0;
		return rc;
	}
	if (x->mmp_block && hits(x, m, x->mmp_block, 1)) REFUSE("ext4 %s: no map: the MMP block in the file\n", v->name);

	/* the file's own tree blocks, the journal's blocks */
	x->fence = m;
	x->fence_hit = 0;
	rc = walk(v, x, &in, nblk, NULL);
	if (!rc && x->journal_ino) {
		struct inode j;
		rc = ino_get(v, x, x->journal_ino, &j);
		if (!rc && (j.flags & FL_INLINE)) rc = E_FS;
		if (!rc) rc = walk(v, x, &j, MIN((j.size + x->bs - 1) >> x->bshift, x->blocks), m);
	}
	x->fence = NULL;
	if (rc) REFUSE("ext4 %s: no map: the journal's or the file's blocks unreadable (%d)\n", v->name, rc);
	if (x->fence_hit) REFUSE("ext4 %s: no map: block %s of the journal or the file's tree in the file\n",
				 v->name, dec(x->fence_hit));

	/* the last extent cut to the size */
	if (m->n) m->ext[m->n - 1].len -= (nblk << x->bshift) - in.size;
	return E_OK;
}

/* --- mount ------------------------------------------------------------- */

static u8 sb[1024];
static char feat[160];

static void feat_add(const char *s)
{
	u32 n = strlen(feat), k = strlen(s);
	if (n + k + 2 > sizeof(feat)) return;
	if (n) feat[n++] = ' ';
	memcpy(feat + n, s, k + 1);
}

#define BAD(what) do { slog("ext4 %s: %s, not mounted\n", v->name, what); return E_FS; } while (0)

static int ext4_mount(struct vol *v)
{
	struct ext4 t;
	u64 vbytes = v->nblocks * v->bsize;
	if (vbytes < 2048) return E_UNKNOWN_FS;
	if (rd(v, 1024, 1024, sb)) return E_DEVICE;
	if (le16(sb + SB_MAGIC) != EXT_SB_MAGIC) return E_UNKNOWN_FS;

	memset(&t, 0, sizeof(t));
	/* features even on revision 0 (as Linux, which only warns) */
	u32 rev = le32(sb + SB_REV);
	t.compat = le32(sb + SB_COMPAT);
	t.incompat = le32(sb + SB_INCOMPAT);
	t.ro_compat = le32(sb + SB_RO_COMPAT);
	if (t.incompat & (IN_JOURNAL_DEV | IN_COMPRESSION | IN_DIRDATA)) {
		slog("ext4 %s: %s, not mounted\n", v->name, (t.incompat & IN_JOURNAL_DEV) ? "an external journal" :
		     (t.incompat & IN_COMPRESSION) ? "compression" : "dirdata");
		return E_UNSUPPORTED;
	}
	if (t.incompat & ~IN_KNOWN) {
		slog("ext4 %s: unknown incompatible features 0x%x, not mounted\n", v->name, t.incompat & ~IN_KNOWN);
		return E_UNSUPPORTED;
	}

	u32 log = le32(sb + SB_LOG_BSIZE);
	if (log > 6) BAD("bad block size");
	t.bshift = 10 + log;
	t.bs = 1u << t.bshift;
	t.sb_blk = t.bs == 1024;
	t.fdb = le32(sb + SB_FIRST_DATA);
	t.blocks = le32(sb + SB_BLOCKS);
	if (t.incompat & IN_64BIT) {
		t.blocks |= (u64)le32(sb + SB_BLOCKS_HI) << 32;
		t.desc_size = le16(sb + SB_DESC_SIZE);
		if (t.desc_size < 64 || t.desc_size > 1024 || (t.desc_size & (t.desc_size - 1)))
			BAD("bad descriptor size");
	} else
		t.desc_size = 32;
	for (t.dpb_shift = t.bshift; (1u << (t.bshift - t.dpb_shift)) < t.desc_size; t.dpb_shift--);
	t.isz = rev ? le16(sb + SB_ISIZE) : 128;
	if (t.isz < 128 || t.isz > t.bs || (t.isz & (t.isz - 1))) BAD("bad inode size");
	t.bpg = le32(sb + SB_BPG);
	t.ipg = le32(sb + SB_IPG);
	t.inodes = le32(sb + SB_INODES);
	t.cluster = t.bs;
	u32 cpg = t.bpg;
	if (t.ro_compat & RO_BIGALLOC) {
		u32 clog = le32(sb + SB_LOG_CSIZE);
		if (clog < log || clog > log + 16 || 10 + clog > 30) BAD("bad cluster size");
		t.cluster = 1024u << clog;
		cpg = le32(sb + SB_CPG);
		if ((u64)cpg << (clog - log) != t.bpg) BAD("bad blocks per group");
	}
	if (!cpg || cpg > 8 * t.bs || !t.ipg || t.ipg > 8 * t.bs) BAD("bad group size");
	if (t.fdb > 1 || t.fdb >= t.blocks) BAD("bad first data block");
	if (t.blocks > vbytes >> t.bshift) BAD("larger than its partition");
	u64 ng = (t.blocks - t.fdb + t.bpg - 1) / t.bpg;
	if (ng >> 32 || (u64)ng * t.ipg != t.inodes) BAD("inode count does not match the groups");
	t.ngroups = ng;
	t.gdb = (ng + (1u << t.dpb_shift) - 1) >> t.dpb_shift;
	t.reserved_gdt = le16(sb + SB_RESERVED_GDT);
	if (t.incompat & IN_META_BG) {
		t.first_meta_bg = le32(sb + SB_FIRST_META_BG);
		if (t.first_meta_bg > t.gdb) BAD("bad first meta_bg");
	} else if ((u64)t.sb_blk + 1 + t.gdb > t.blocks)
		BAD("descriptors beyond the end");
	t.itb = ((u64)t.ipg * t.isz + t.bs - 1) >> t.bshift;
	if (t.compat & CO_JOURNAL) t.journal_ino = le32(sb + SB_JOURNAL_INO);
	if (t.incompat & IN_MMP) t.mmp_block = le64(sb + SB_MMP_BLOCK);
	t.backup_bgs[0] = le32(sb + SB_BACKUP_BGS);
	t.backup_bgs[1] = le32(sb + SB_BACKUP_BGS + 4);
	u32 state = le16(sb + SB_STATE);
	t.needs_recovery = (t.incompat & IN_RECOVER) != 0;
	t.unclean = !(state & ST_VALID) || (state & ST_ERRORS);

	struct inode root;
	int rc = ino_get(v, &t, ROOT_INO, &root);
	if (rc == E_DEVICE) return rc;
	if (rc || (root.mode & M_FMT) != M_DIR) BAD("no root folder");

	struct ext4 *x = kmalloc(sizeof(*x) + 2 * t.bs);
	if (!x) return E_MEMORY;
	*x = t;
	x->dirbuf = (u8 *)(x + 1);
	x->nodebuf = x->dirbuf + t.bs;
	v->fs = &fs_ext4;
	v->fsd = x;

	char name[17];
	u32 k = 0;
	for (; k < 16 && sb[SB_NAME + k]; k++) name[k] = sb[SB_NAME + k];
	name[k] = 0;
	if (!v->label[0]) memcpy(v->label, name, k + 1);
	/* the features for the log: which word (compat, incompat, ro_compat), bit, name */
	static const struct { u8 w; u32 bit; const char *name; } fl[] = {
		{ 0, CO_JOURNAL, "has_journal" }, { 0, CO_EXT_ATTR, "ext_attr" },
		{ 0, CO_RESIZE_INODE, "resize_inode" }, { 0, CO_DIR_INDEX, "dir_index" },
		{ 0, CO_SPARSE_SUPER2, "sparse_super2" }, { 1, IN_FILETYPE, "filetype" },
		{ 1, IN_EXTENTS, "extent" }, { 1, IN_64BIT, "64bit" }, { 1, IN_FLEX_BG, "flex_bg" },
		{ 1, IN_META_BG, "meta_bg" }, { 1, IN_MMP, "mmp" }, { 1, IN_INLINE_DATA, "inline_data" },
		{ 1, IN_ENCRYPT, "encrypt" }, { 1, IN_CASEFOLD, "casefold" }, { 1, IN_LARGEDIR, "large_dir" },
		{ 1, IN_EA_INODE, "ea_inode" }, { 1, IN_CSUM_SEED, "metadata_csum_seed" },
		{ 1, IN_RECOVER, "needs_recovery" }, { 2, RO_SPARSE_SUPER, "sparse_super" },
		{ 2, RO_LARGE_FILE, "large_file" }, { 2, RO_HUGE_FILE, "huge_file" },
		{ 2, RO_GDT_CSUM, "uninit_bg" }, { 2, RO_METADATA_CSUM, "metadata_csum" },
		{ 2, RO_BIGALLOC, "bigalloc" },
	};
	feat[0] = 0;
	for (u32 i = 0; i < ARRAY_SIZE(fl); i++)
		if ((fl[i].w == 0 ? t.compat : fl[i].w == 1 ? t.incompat : t.ro_compat) & fl[i].bit)
			feat_add(fl[i].name);
	slog("ext4 %s: \"%s\", %s, %u-byte blocks, %s blocks, %u inodes, %u groups: %s\n", v->name, name,
	     (t.incompat & (IN_EXTENTS | IN_64BIT | IN_FLEX_BG)) ? "ext4" : t.journal_ino ? "ext3" : "ext2",
	     t.bs, dec(t.blocks), t.inodes, t.ngroups, feat);
	if (t.needs_recovery)
		slog("ext4 %s: WARNING: the journal needs recovery (Linux has it mounted or did not "
		     "unmount it): what is read may be stale\n", v->name);
	else if (t.unclean)
		slog("ext4 %s: WARNING: not cleanly unmounted or errors recorded (state 0x%x)\n", v->name, state);
	if (t.incompat & IN_CASEFOLD) slog("ext4 %s: casefold: names compared exactly\n", v->name);
	return E_OK;
}

const struct fsops fs_ext4 = {
	.name = "ext4",
	.mount = ext4_mount,
	.info = ext4_info,
	.read = ext4_read,
	.readdir = ext4_readdir,
	.map = ext4_map,
};
