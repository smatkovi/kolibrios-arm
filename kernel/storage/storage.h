/* KolibriOS for ARM: storage. Block devices (the Pro1X's UFS, the N950's
 * eMMC, QEMU's virtio-blk, UFS and eMMC models), their partitions (GPT,
 * MBR), the file systems on them (ext2/3/4 read only, FAT12/16/32), and
 * the one place that may be written: a container file prepared from Linux
 * (home.c, docs/storage.md).
 *
 * Plain C without the kernel's services beyond the few below: the kernel
 * builds it, and tests/storage on the PC (STORAGE_HOST).
 *
 * The rule the layers keep: a driver's write entry is handed to the block
 * layer once (blk_add's write argument), kept where nothing else reaches
 * it, and called only by the fence (blk.c), which lets through nothing
 * outside the extents home.c has verified and added. A driver checks the
 * fence again itself before it sends a write command. */
#ifndef STORAGE_H
#define STORAGE_H

#ifdef STORAGE_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef unsigned long addr_t;
#define slog(...) printf(__VA_ARGS__)
#define kmalloc(n) calloc(1, (n))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
u32 rtc_seconds(void);
#else
#include "kernel.h"
#define slog(...) kprintf(__VA_ARGS__)
#endif

/* KolibriOS's file system error codes (function 70) */
enum { E_OK = 0, E_UNSUPPORTED = 2, E_UNKNOWN_FS = 3, E_NOT_FOUND = 5, E_EOF = 6, E_POINTER = 7,
       E_DISK_FULL = 8, E_FS = 9, E_ACCESS = 10, E_DEVICE = 11, E_MEMORY = 12 };

/* KolibriOS's attributes (those of FAT) */
#define A_RO 0x01
#define A_HIDDEN 0x02
#define A_SYSTEM 0x04
#define A_LABEL 0x08
#define A_DIR 0x10
#define A_ARCH 0x20

/* little and big endian fields of on-disk structures, at any alignment */
static inline u16 le16(const void *p) { const u8 *b = p; return b[0] | b[1] << 8; }
static inline u32 le32(const void *p) { const u8 *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }
static inline u64 le64(const void *p) { return le32(p) | (u64)le32((const u8 *)p + 4) << 32; }
static inline u16 be16_at(const void *p) { const u8 *b = p; return b[0] << 8 | b[1]; }
static inline u32 be32_at(const void *p) { const u8 *b = p; return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static inline u64 be64_at(const void *p) { return (u64)be32_at(p) << 32 | be32_at((const u8 *)p + 4); }
static inline void put_le16(void *p, u16 v) { u8 *b = p; b[0] = v; b[1] = v >> 8; }
static inline void put_le32(void *p, u32 v) { u8 *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static inline void put_le64(void *p, u64 v) { put_le32(p, (u32)v); put_le32((u8 *)p + 4, (u32)(v >> 32)); }
static inline void put_be16(void *p, u16 v) { u8 *b = p; b[0] = v >> 8; b[1] = v; }
static inline void put_be32(void *p, u32 v) { u8 *b = p; b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static inline void put_be64(void *p, u64 v) { put_be32(p, (u32)(v >> 32)); put_be32((u8 *)p + 4, (u32)v); }

/* --- block devices (blk.c) --------------------------------------------- */

struct blkdev;
typedef int (*blk_read_fn)(struct blkdev *d, u64 lba, u32 n, void *buf);
typedef int (*blk_write_fn)(struct blkdev *d, u64 lba, u32 n, const void *buf);

struct blkdev {
	char name[12];          /* its name in paths: "ufs0", "mmc0", "vd0" */
	char what[48];          /* for the log: "UFS LUN 0, SAMSUNG KLUDG4U1EA" */
	u32 bsize;              /* bytes a block: 512 or 4096 */
	u64 nblocks;
	u32 max_blocks;         /* the most blocks one read or write may move */
	blk_read_fn read;       /* n blocks from lba to buf (any kernel address); 0 or -1 */
	void *priv;             /* the driver's */
	int index;              /* in the block layer's list */
};

/* a driver adds its disk once it reads; write may be NULL (read only).
 * The write entry stays inside blk.c: only the fence calls it */
int blk_add(struct blkdev *d, blk_write_fn write);
/* the fence: may n blocks at lba of d be written? A driver asks again
 * before it sends the command */
bool blk_fence_allows(const struct blkdev *d, u64 lba, u32 n);
int blk_count(void);
struct blkdev *blk_get(int i);

/* --- volumes: a partition (or a whole disk) with its file system ---------- */

struct vol;
struct vdirent {
	char name[768];         /* UTF-8 (a FAT long name, an ext4 name's bytes) */
	u32 attr;               /* A_* */
	u64 size;               /* 0 for folders */
	u32 ctime, atime, mtime;   /* Unix times (FAT's local times taken as they are) */
};

#define FMAP_MAX 64
struct extent { u64 off, len; };   /* bytes, from the volume's start */
/* where a file's data lies (home.c checks the container with it) */
struct fmap {
	u64 id;                 /* ext4: the inode's number; FAT: the first cluster */
	u64 size;               /* the file's size in bytes */
	u32 flags;              /* FMAP_* */
	int n;
	struct extent ext[FMAP_MAX];   /* in file order, adjacent ones joined */
};
#define FMAP_IMMUTABLE 1        /* ext4: EXT4_IMMUTABLE_FL */
#define FMAP_RO 2               /* FAT: A_RO */
#define FMAP_SYSTEM 4           /* FAT: A_SYSTEM */

/* A file system's functions, paths relative to the volume's root:
 * "dir/file", "" the root; no "." or ".." in them, no '/' at either
 * end. Names compare as the file system does (FAT: without regard to
 * case, ext4: exactly). Return E_OK or a KolibriOS error code. */
struct fsops {
	const char *name;       /* "ext4", "fat32" ... */
	/* is it this file system? Then v->fs and v->fsd are set; read only
	 * unless v->writable (and then only through the fence anyway) */
	int (*mount)(struct vol *v);
	/* 70.5: attributes, size, times (e->name unused) */
	int (*info)(struct vol *v, const char *path, struct vdirent *e);
	/* 70.0: up to len bytes at off; *got the bytes read (fewer at the end) */
	int (*read)(struct vol *v, const char *path, u64 off, u32 len, void *buf, u32 *got);
	/* 70.1: the folder's entries from index first on, at most count of
	 * them to emit, *total all there are; "." and ".." left out (kfs.c
	 * adds them), volume labels and deleted entries too */
	int (*readdir)(struct vol *v, const char *path, u32 first, u32 count,
		       void (*emit)(void *ctx, const struct vdirent *e), void *ctx, u32 *total);
	/* the file's place on the volume (home.c) */
	int (*map)(struct vol *v, const char *path, struct fmap *m);
	/* writing: NULL for a read-only file system; E_ACCESS on a volume
	 * that is not writable */
	/* 70.2 (rewrite: created, or cut to 0 first, then written at 0) and
	 * 70.3 (at off; E_NOT_FOUND if not there); the file grows as needed,
	 * zeros in a gap; *done the bytes written (E_DISK_FULL: fewer) */
	int (*write)(struct vol *v, const char *path, u64 off, u32 len, const void *buf, bool rewrite, u32 *done);
	int (*truncate)(struct vol *v, const char *path, u64 size);           /* 70.4, zeros when it grows */
	int (*mkdir)(struct vol *v, const char *path);    /* one folder, its parent there; E_OK if it is there */
	int (*remove)(struct vol *v, const char *path);   /* 70.8: a file or an empty folder */
	int (*rename)(struct vol *v, const char *from, const char *to);   /* 70.10, E_ACCESS if to is there */
	int (*setattr)(struct vol *v, const char *path, const struct vdirent *e);   /* 70.6 (0 times: kept) */
	void (*sync)(struct vol *v);                      /* what is kept in memory to the disk (FSInfo) */
};

struct vol {
	char name[24];          /* in paths after '/': "ufs0/13", "hd0/1" */
	char label[40];         /* the GPT's name of the partition, the volume label */
	struct blkdev *disk;    /* NULL for the container (its blocks go through the fence) */
	u64 start;              /* the first block on disk */
	u32 bsize;              /* bytes a block (the disk's; the container's: 4096) */
	u64 nblocks;
	bool writable;          /* the container only (home.c) */
	const struct fsops *fs; /* the mounted file system, or NULL */
	void *fsd;              /* its state */
	/* the container: its blocks by its extents on the volume it is on */
	struct vol *host;
	int next;
	struct extent ext[FMAP_MAX];
};

/* bytes of a volume through the block cache: 0, E_DEVICE, E_ACCESS (a
 * write to a volume that is not writable, or one the fence refuses) */
int vol_rd(struct vol *v, u64 off, u32 len, void *buf);
int vol_wr(struct vol *v, u64 off, u32 len, const void *buf);
/* the volumes the block layer found (blk_scan) and the container */
int vol_count(void);
struct vol *vol_get(int i);
struct vol *vol_find(const char *name, u32 len);   /* "ufs0/13" */
/* the partitions of every disk added, their file systems mounted (read
 * only); returns the number of volumes */
int blk_scan(void);
/* the container's volume over its extents on host, after home.c's
 * checks; rw: writable, its extents added to the fence */
struct vol *blk_container(struct vol *host, const struct extent *ext, int n, u64 size, bool rw);
/* the block cache: everything written is on the disk (write through), so
 * this only drops it (tests) */
void blk_cache_drop(void);

extern const struct fsops fs_ext4, fs_fat;

/* --- the container (home.c) ------------------------------------------- */

/* after blk_scan: the container found, checked, mounted read-write as
 * hd0/1 if the command line says kolibri.home=rw (home_mode) */
enum { HOME_OFF, HOME_RO, HOME_RW };
void home_init(int mode);
struct vol *home_vol(void);             /* the mounted container, or NULL */

/* --- the drivers ---------------------------------------------------------- */

/* the DMA memory the drivers get: uncached, physically contiguous, its
 * bus address (the controllers see physical addresses: the SMMU's
 * context of their stream has its translation off) */
struct dma_mem {
	u8 *va;
	u64 pa;
	u32 size;
};

#endif
