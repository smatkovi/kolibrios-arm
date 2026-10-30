/* KolibriOS for ARM: virtio-rng over virtio-mmio, version 2 (QEMU:
 * -device virtio-rng-device): one queue; a buffer handed over comes back
 * filled with the host's random bytes. */
#include "kernel.h"
#include "rng.h"

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

struct desc { u64 addr; u32 len; u16 flags, next; };

static addr_t base;
static struct desc *desc;
static volatile u16 *avail;
static volatile u32 *used;
static u16 aidx, last;
static u8 *buf;

static u32 read(void *out, u32 len)
{
	u32 done = 0;
	while (done < len) {
		u32 n = MIN(len - done, 256u);
		desc[0].addr = (u64)(addr_t)buf;
		desc[0].len = n;
		desc[0].flags = 2;                     /* the device writes */
		avail[2] = 0;
		asm volatile("dmb ishst" ::: "memory");
		avail[1] = ++aidx;
		asm volatile("dsb sy" ::: "memory");
		wr32(base + QUEUE_NOTIFY, 0);
		u32 i;
		for (i = 0; i < 10000000 && last == (u16)(used[0] >> 16); i++)
			;
		if (last == (u16)(used[0] >> 16)) break;
		asm volatile("dmb ishld" ::: "memory");
		u32 got = MIN(used[2 + 2 * (last % 4)], n);
		last++;
		wr32(base + INT_ACK, rd32(base + INT_STATUS));
		memcpy((u8 *)out + done, buf, got);
		done += got;
		if (!got) break;
	}
	return done;
}

bool virtio_rng_probe(addr_t va)
{
	if (rd32(va) != 0x74726976 || rd32(va + 4) != 2 || rd32(va + DEVICE_ID) != 4) return false;
	base = va;
	wr32(va + STATUS, 0);
	wr32(va + STATUS, 3);                          /* acknowledge, driver */
	wr32(va + DEV_FEATURES_SEL, 1);
	u32 f1 = rd32(va + DEV_FEATURES);
	wr32(va + DRV_FEATURES_SEL, 0);
	wr32(va + DRV_FEATURES, 0);
	wr32(va + DRV_FEATURES_SEL, 1);
	wr32(va + DRV_FEATURES, f1 & 1);               /* VERSION_1 */
	wr32(va + STATUS, 3 | 8);
	wr32(va + QUEUE_SEL, 0);
	if (!(rd32(va + STATUS) & 8) || !(f1 & 1) || rd32(va + QUEUE_NUM_MAX) < 4) {
		wr32(va + STATUS, 0);
		return false;
	}
	u8 *m = kmalloc_aligned(3 * 4096, 4096);
	memset(m, 0, 3 * 4096);
	desc = (struct desc *)m;
	avail = (volatile u16 *)(m + 4096);
	used = (volatile u32 *)(m + 2 * 4096);
	buf = kmalloc_aligned(256, 64);
	wr32(va + QUEUE_NUM, 4);
	wr32(va + QUEUE_DESC, (u32)(addr_t)desc);
	wr32(va + QUEUE_DESC + 4, (u32)((addr_t)desc >> 32));
	wr32(va + QUEUE_DRIVER, (u32)(addr_t)avail);
	wr32(va + QUEUE_DRIVER + 4, (u32)((addr_t)avail >> 32));
	wr32(va + QUEUE_DEVICE, (u32)(addr_t)used);
	wr32(va + QUEUE_DEVICE + 4, (u32)((addr_t)used >> 32));
	wr32(va + QUEUE_READY, 1);
	wr32(va + STATUS, 3 | 8 | 4);
	rng_register("virtio-rng", read, RNG_FULL);
	return true;
}
