/* KolibriOS for ARM: virtio-blk over virtio-mmio, version 2 (QEMU's
 * -device virtio-blk-device on its virt machine, with -global
 * virtio-mmio.force-legacy=false): the disks of the storage tests in
 * QEMU, "vd0", "vd1", ... One queue, one request at a time, polled: a
 * header (type, sector), the data through a bounce buffer, the status.
 * logical_block_size=4096 makes a disk of 4 KiB blocks, as the Pro1X's
 * UFS LUNs are; virtio counts in 512-byte sectors either way. */
#include "storage.h"

#define MAGIC 0x000
#define VERSION 0x004
#define DEVICE_ID 0x008
#define DEV_FEATURES 0x010
#define DEV_FEATURES_SEL 0x014
#define DRV_FEATURES 0x020
#define DRV_FEATURES_SEL 0x024
#define QUEUE_SEL 0x030
#define QUEUE_NUM_MAX 0x034
#define QUEUE_NUM 0x038
#define QUEUE_READY 0x044
#define QUEUE_NOTIFY 0x050
#define INT_STATUS 0x060
#define INT_ACK 0x064
#define STATUS 0x070
#define QUEUE_DESC 0x080
#define QUEUE_DRIVER 0x090
#define QUEUE_DEVICE 0x0a0
#define CONFIG 0x100

#define S_ACK 1
#define S_DRIVER 2
#define S_DRIVER_OK 4
#define S_FEATURES_OK 8
#define F_RO (1u << 5)
#define F_BLK_SIZE (1u << 6)
#define F_VERSION_1 1u                  /* bit 32 */

#define D_NEXT 1
#define D_WRITE 2
#define QSIZE 8
#define BOUNCE (64u << 10)

#define T_IN 0
#define T_OUT 1

struct desc { u64 addr; u32 len; u16 flags, next; };

struct vblk {
	struct blkdev dev;
	addr_t base;
	struct desc *desc;
	volatile u16 *avail;
	volatile u32 *used;
	u16 avail_idx, last_used;
	u8 *hdr, *status, *bounce;
};

#define MAX_DISKS 4
static struct vblk disks[MAX_DISKS];
static int ndisks;

static u32 reg(struct vblk *b, u32 off) { return rd32(b->base + off); }
static void set(struct vblk *b, u32 off, u32 v) { wr32(b->base + off, v); }

static void barrier(void)
{
	asm volatile("dsb sy" ::: "memory");
}

/* one request: n blocks at lba, from or into the bounce buffer */
static int request(struct vblk *b, u32 type, u64 lba, u32 n)
{
	u32 len = n * b->dev.bsize;
	put_le32(b->hdr, type);
	put_le32(b->hdr + 4, 0);
	put_le64(b->hdr + 8, lba * (b->dev.bsize / 512));
	*b->status = 0xff;
	b->desc[0] = (struct desc){ (u64)(addr_t)b->hdr, 16, D_NEXT, 1 };
	b->desc[1] = (struct desc){ (u64)(addr_t)b->bounce, len, (u16)(D_NEXT | (type == T_IN ? D_WRITE : 0)), 2 };
	b->desc[2] = (struct desc){ (u64)(addr_t)b->status, 1, D_WRITE, 0 };
	b->avail[2 + b->avail_idx % QSIZE] = 0;
	barrier();
	b->avail[1] = ++b->avail_idx;
	barrier();
	set(b, QUEUE_NOTIFY, 0);
	for (u32 i = 0; i < 100000000u; i++) {
		barrier();
		if ((u16)(b->used[0] >> 16) != b->last_used) {
			b->last_used++;
			u32 st = reg(b, INT_STATUS);
			if (st) set(b, INT_ACK, st);
			return *b->status == 0 ? 0 : -1;
		}
	}
	slog("virtio-blk: %s: no answer\n", b->dev.name);
	return -1;
}

static int vread(struct blkdev *d, u64 lba, u32 n, void *buf)
{
	struct vblk *b = d->priv;
	if (request(b, T_IN, lba, n)) return -1;
	memcpy(buf, b->bounce, n * d->bsize);
	return 0;
}

/* only the fence calls it (blk.c); asked again here */
static int vwrite(struct blkdev *d, u64 lba, u32 n, const void *buf)
{
	struct vblk *b = d->priv;
	if (!blk_fence_allows(d, lba, n)) return -1;
	memcpy(b->bounce, buf, n * d->bsize);
	return request(b, T_OUT, lba, n);
}

/* a virtio-mmio slot (its kernel address): a block device, added */
bool virtio_blk_probe(addr_t va)
{
	if (rd32(va + MAGIC) != 0x74726976 || rd32(va + DEVICE_ID) != 2 || ndisks == MAX_DISKS) return false;
	if (rd32(va + VERSION) != 2) {
		slog("virtio-blk: a legacy device (QEMU: -global virtio-mmio.force-legacy=false)\n");
		return false;
	}
	struct vblk *b = &disks[ndisks];
	b->base = va;
	set(b, STATUS, 0);
	set(b, STATUS, S_ACK);
	set(b, STATUS, S_ACK | S_DRIVER);
	set(b, DEV_FEATURES_SEL, 0);
	u32 f0 = reg(b, DEV_FEATURES);
	set(b, DEV_FEATURES_SEL, 1);
	u32 f1 = reg(b, DEV_FEATURES);
	set(b, DRV_FEATURES_SEL, 0);
	set(b, DRV_FEATURES, f0 & (F_RO | F_BLK_SIZE));
	set(b, DRV_FEATURES_SEL, 1);
	set(b, DRV_FEATURES, f1 & F_VERSION_1);
	set(b, STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
	set(b, QUEUE_SEL, 0);
	if (!(reg(b, STATUS) & S_FEATURES_OK) || !(f1 & F_VERSION_1) || reg(b, QUEUE_READY) ||
	    reg(b, QUEUE_NUM_MAX) < QSIZE) {
		slog("virtio-blk: not set up\n");
		set(b, STATUS, 0);
		return false;
	}
	u8 *m = kmalloc_aligned(3 * 4096, 4096);
	memset(m, 0, 3 * 4096);
	b->desc = (struct desc *)m;
	b->avail = (volatile u16 *)(m + 4096);
	b->used = (volatile u32 *)(m + 2 * 4096);
	b->hdr = kmalloc_aligned(64, 64);
	b->status = b->hdr + 32;
	b->bounce = kmalloc_aligned(BOUNCE, 4096);
	set(b, QUEUE_NUM, QSIZE);
	set(b, QUEUE_DESC, (u32)(addr_t)b->desc);
	set(b, QUEUE_DESC + 4, (u32)((u64)(addr_t)b->desc >> 32));
	set(b, QUEUE_DRIVER, (u32)(addr_t)b->avail);
	set(b, QUEUE_DRIVER + 4, (u32)((u64)(addr_t)b->avail >> 32));
	set(b, QUEUE_DEVICE, (u32)(addr_t)b->used);
	set(b, QUEUE_DEVICE + 4, (u32)((u64)(addr_t)b->used >> 32));
	set(b, QUEUE_READY, 1);
	set(b, STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
	u64 sectors = rd32(va + CONFIG) | (u64)rd32(va + CONFIG + 4) << 32;
	u32 bs = (f0 & F_BLK_SIZE) ? rd32(va + CONFIG + 20) : 512;
	if (bs != 512 && bs != 4096) bs = 512;
	struct blkdev *d = &b->dev;
	d->name[0] = 'v', d->name[1] = 'd', d->name[2] = '0' + ndisks, d->name[3] = 0;
	memcpy(d->what, "virtio-blk", 11);
	d->bsize = bs;
	d->nblocks = sectors / (bs / 512);
	d->max_blocks = BOUNCE / bs;
	d->read = vread;
	d->priv = b;
	ndisks++;
	blk_add(d, (f0 & F_RO) ? NULL : vwrite);
	return true;
}
