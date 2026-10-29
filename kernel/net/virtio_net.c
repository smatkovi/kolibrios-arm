/* KolibriOS for ARM: virtio-net over virtio-mmio, version 2 (QEMU's
 * network card on its virt machine, with -global
 * virtio-mmio.force-legacy=false). Two queues: receive (0), buffers
 * handed to the device, and transmit (1); every frame behind the 12-byte
 * virtio-net header. The interrupt and the tick both reap the queues. */
#include "net.h"

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

#define F_MAC (1u << 5)
#define F_VERSION_1 1u                  /* bit 32: in the second word */

#define D_WRITE 2
#define QSIZE 64
#define BUF 2048
#define HDR 12

struct desc { u64 addr; u32 len; u16 flags, next; };

struct queue {
	int index;
	u32 num;
	struct desc *desc;
	volatile u16 *avail;            /* flags, idx, ring[num] */
	volatile u32 *used;             /* flags | idx << 16, then id, len pairs */
	u16 avail_idx, last_used;
	u8 *buf;
	u32 inflight;                   /* transmit */
};

static addr_t base;
static struct queue rxq, txq;
static struct netif *nif;

static u32 reg(u32 off) { return rd32(base + off); }
static void set(u32 off, u32 v) { wr32(base + off, v); }

static bool queue_init(struct queue *q, int index)
{
	set(QUEUE_SEL, index);
	if (reg(QUEUE_READY)) return false;
	u32 max = reg(QUEUE_NUM_MAX);
	if (!max) return false;
	q->index = index;
	q->num = MIN(max, QSIZE);
	u8 *m = kmalloc_aligned(3 * 4096, 4096);
	memset(m, 0, 3 * 4096);
	q->desc = (struct desc *)m;
	q->avail = (volatile u16 *)(m + 4096);
	q->used = (volatile u32 *)(m + 2 * 4096);
	q->buf = kmalloc_aligned(q->num * BUF, 4096);
	for (u32 i = 0; i < q->num; i++) {
		q->desc[i].addr = (u64)(addr_t)(q->buf + i * BUF);
		q->desc[i].len = BUF;
		q->desc[i].flags = index == 0 ? D_WRITE : 0;
	}
	set(QUEUE_NUM, q->num);
	set(QUEUE_DESC, (u32)(addr_t)q->desc);
	set(QUEUE_DESC + 4, (u32)((addr_t)q->desc >> 32));
	set(QUEUE_DRIVER, (u32)(addr_t)q->avail);
	set(QUEUE_DRIVER + 4, (u32)((addr_t)q->avail >> 32));
	set(QUEUE_DEVICE, (u32)(addr_t)q->used);
	set(QUEUE_DEVICE + 4, (u32)((addr_t)q->used >> 32));
	set(QUEUE_READY, 1);
	return true;
}

static void offer(struct queue *q, u16 id)
{
	q->avail[2 + q->avail_idx % q->num] = id;
	asm volatile("dmb ishst" ::: "memory");
	q->avail[1] = ++q->avail_idx;
}

static void notify(struct queue *q)
{
	asm volatile("dsb sy" ::: "memory");
	set(QUEUE_NOTIFY, q->index);
}

static void reap(void *dev)
{
	(void)dev;
	if (!base) return;
	u32 st = reg(INT_STATUS);
	if (st) set(INT_ACK, st);
	bool more = false;
	while (rxq.last_used != (u16)(rxq.used[0] >> 16)) {
		asm volatile("dmb ishld" ::: "memory");
		u32 slot = rxq.last_used++ % rxq.num;
		u32 id = rxq.used[1 + 2 * slot], len = rxq.used[2 + 2 * slot];
		if (id < rxq.num && len > HDR && len <= BUF && nif)
			net_input(nif, rxq.buf + id * BUF + HDR, len - HDR);
		offer(&rxq, id);
		more = true;
	}
	if (more) notify(&rxq);
	while (txq.last_used != (u16)(txq.used[0] >> 16)) {
		txq.last_used++;
		txq.inflight--;
	}
}

static bool send(void *dev, const void *frame, u32 len)
{
	(void)dev;
	if (len + HDR > BUF) return false;
	reap(NULL);
	if (txq.inflight >= txq.num) return false;
	u16 id = txq.avail_idx % txq.num;
	u8 *b = txq.buf + id * BUF;
	memset(b, 0, HDR);
	memcpy(b + HDR, frame, len);
	txq.desc[id].len = len + HDR;
	txq.inflight++;
	offer(&txq, id);
	notify(&txq);
	return true;
}

/* a virtio-mmio slot (its kernel address): a network card set up and
 * attached, true; anything else, false */
bool virtio_net_probe(addr_t va)
{
	if (rd32(va + MAGIC) != 0x74726976 || rd32(va + DEVICE_ID) != 1) return false;
	if (rd32(va + VERSION) != 2) {
		kprintf("virtio-net: a legacy device (QEMU: -global virtio-mmio.force-legacy=false)\n");
		return false;
	}
	base = va;
	set(STATUS, 0);
	set(STATUS, S_ACK);
	set(STATUS, S_ACK | S_DRIVER);
	set(DEV_FEATURES_SEL, 0);
	u32 f0 = reg(DEV_FEATURES);
	set(DEV_FEATURES_SEL, 1);
	u32 f1 = reg(DEV_FEATURES);
	set(DRV_FEATURES_SEL, 0);
	set(DRV_FEATURES, f0 & F_MAC);
	set(DRV_FEATURES_SEL, 1);
	set(DRV_FEATURES, f1 & F_VERSION_1);
	set(STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
	if (!(reg(STATUS) & S_FEATURES_OK) || !(f1 & F_VERSION_1) ||
	    !queue_init(&rxq, 0) || !queue_init(&txq, 1)) {
		kprintf("virtio-net: not set up\n");
		set(STATUS, 0);
		base = 0;
		return false;
	}
	for (u32 i = 0; i < rxq.num; i++) offer(&rxq, i);
	set(STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
	notify(&rxq);
	struct net_dev d = { .name = "virtio-net", .mtu = 1500, .send = send, .poll = reap };
	for (int i = 0; i < 6; i++)
		d.mac[i] = (f0 & F_MAC) ? rd8(va + CONFIG + i) : (u8)(i ? net_rand() : 0x02);
	nif = net_attach(&d);
	return nif != NULL;
}

void virtio_net_irq(void)
{
	reap(NULL);
}
