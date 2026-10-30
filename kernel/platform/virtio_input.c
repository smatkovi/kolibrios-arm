/* KolibriOS for ARM: virtio-input over virtio-mmio, version 2 (QEMU:
 * -device virtio-keyboard-device, -device virtio-tablet-device): the
 * keyboard and the pointer of QEMU's virt, so that tests can type
 * (the monitor's sendkey) and click.
 *
 * After the virtio 1.1 specification, 5.8 "Input Device": device id 18,
 * queue 0 the events (struct virtio_input_event: le16 type, le16 code,
 * le32 value, Linux's input events), queue 1 the status (LEDs: not
 * used). The keys are Linux's key codes (linux/input-event-codes.h),
 * which for 1-88 are the PC's set 1 scan codes; the others with a set 1
 * code (E0 prefixed there: the arrows, Home, End, ...) are mapped here.
 * A tablet's EV_ABS X/Y are scaled from the axis' range (the config
 * space's ABS_INFO) to the screen. */
#include "kernel.h"
#include "gui.h"

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

#define CFG_ABS_INFO 0x12               /* virtio_input_config_select */
#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define NBUF 64

struct desc { u64 addr; u32 len; u16 flags, next; };
struct event { u16 type, code; u32 value; };

static struct vin {
	addr_t base;
	struct desc *desc;
	volatile u16 *avail;
	volatile u32 *used;
	u16 aidx, last;
	struct event *ev;
	u32 max[2];                     /* the axes' largest values (a tablet) */
	int x, y, buttons;
	bool abs;
} dev[2];
static int ndev;

/* Linux's codes above 88 with a set 1 code (input-event-codes.h) */
static u8 set1_of(u16 code)
{
	if (code >= 1 && code <= 88) return (u8)code;
	switch (code) {
	case 96: return 0x1c;           /* KEY_KPENTER */
	case 97: return 0x1d;           /* KEY_RIGHTCTRL */
	case 98: return 0x35;           /* KEY_KPSLASH */
	case 100: return 0x38;          /* KEY_RIGHTALT */
	case 102: return 0x47;          /* KEY_HOME */
	case 103: return 0x48;          /* KEY_UP */
	case 104: return 0x49;          /* KEY_PAGEUP */
	case 105: return 0x4b;          /* KEY_LEFT */
	case 106: return 0x4d;          /* KEY_RIGHT */
	case 107: return 0x4f;          /* KEY_END */
	case 108: return 0x50;          /* KEY_DOWN */
	case 109: return 0x51;          /* KEY_PAGEDOWN */
	case 110: return 0x52;          /* KEY_INSERT */
	case 111: return 0x53;          /* KEY_DELETE */
	}
	return 0;
}

static void offer(struct vin *d, u16 i)
{
	d->desc[i].addr = (u64)(addr_t)&d->ev[i];
	d->desc[i].len = sizeof(struct event);
	d->desc[i].flags = 2;                     /* the device writes */
	d->avail[2 + d->aidx % NBUF] = i;
	asm volatile("dmb ishst" ::: "memory");
	d->avail[1] = ++d->aidx;
}

static void event(struct vin *d, const struct event *e)
{
	if (e->type == EV_KEY) {
		if (e->code >= BTN_LEFT && e->code <= BTN_MIDDLE) {
			int bit = e->code == BTN_LEFT ? 1 : e->code == BTN_RIGHT ? 2 : 4;
			d->buttons = e->value ? d->buttons | bit : d->buttons & ~bit;
			return;
		}
		u8 sc = set1_of(e->code);
		if (sc && e->value <= 2) input_key(sc, e->value == 0);   /* 2: repeated */
	} else if (e->type == EV_ABS && e->code < 2) {
		u32 max = d->max[e->code] ? d->max[e->code] : 0x7fff;
		int v = (int)((u64)e->value * (u32)(e->code ? scr.h - 1 : scr.w - 1) / max);
		if (e->code) d->y = v; else d->x = v;
		d->abs = true;
	} else if (e->type == EV_SYN && d->abs) {
		input_mouse_abs(d->x, d->y, d->buttons);
	}
}

static void reap(struct vin *d)
{
	u32 st = rd32(d->base + INT_STATUS);
	if (st) wr32(d->base + INT_ACK, st);
	bool more = false;
	while (d->last != (u16)(d->used[0] >> 16)) {
		asm volatile("dmb ishld" ::: "memory");
		u32 slot = d->last++ % NBUF;
		u32 id = d->used[1 + 2 * slot];
		if (id < NBUF) {
			event(d, &d->ev[id]);
			offer(d, (u16)id);
		}
		more = true;
	}
	if (more) {
		asm volatile("dsb sy" ::: "memory");
		wr32(d->base + QUEUE_NOTIFY, 0);
	}
}

void virtio_input_irq(void)
{
	for (int i = 0; i < ndev; i++) reap(&dev[i]);
}

static u32 abs_max(addr_t va, u8 axis)
{
	*(volatile u8 *)(va + CONFIG) = CFG_ABS_INFO;
	*(volatile u8 *)(va + CONFIG + 1) = axis;
	asm volatile("dsb sy" ::: "memory");
	if (*(volatile u8 *)(va + CONFIG + 2) < 8) return 0;
	return rd32(va + CONFIG + 8 + 4);         /* virtio_input_absinfo: min, max, ... */
}

bool virtio_input_probe(addr_t va)
{
	if (rd32(va) != 0x74726976 || rd32(va + 4) != 2 || rd32(va + DEVICE_ID) != 18) return false;
	if (ndev == (int)ARRAY_SIZE(dev)) return false;
	struct vin *d = &dev[ndev];
	wr32(va + STATUS, 0);
	wr32(va + STATUS, 3);                     /* acknowledge, driver */
	wr32(va + DEV_FEATURES_SEL, 1);
	u32 f1 = rd32(va + DEV_FEATURES);
	wr32(va + DRV_FEATURES_SEL, 0);
	wr32(va + DRV_FEATURES, 0);
	wr32(va + DRV_FEATURES_SEL, 1);
	wr32(va + DRV_FEATURES, f1 & 1);          /* VERSION_1 */
	wr32(va + STATUS, 3 | 8);
	wr32(va + QUEUE_SEL, 0);
	if (!(rd32(va + STATUS) & 8) || !(f1 & 1) || rd32(va + QUEUE_NUM_MAX) < NBUF) {
		wr32(va + STATUS, 0);
		return false;
	}
	d->base = va;
	u8 *m = kmalloc_aligned(3 * 4096, 4096);
	memset(m, 0, 3 * 4096);
	d->desc = (struct desc *)m;
	d->avail = (volatile u16 *)(m + 4096);
	d->used = (volatile u32 *)(m + 2 * 4096);
	d->ev = kmalloc_aligned(NBUF * sizeof(struct event), 64);
	d->max[0] = abs_max(va, 0);
	d->max[1] = abs_max(va, 1);
	wr32(va + QUEUE_NUM, NBUF);
	wr32(va + QUEUE_DESC, (u32)(addr_t)d->desc);
	wr32(va + QUEUE_DESC + 4, (u32)((addr_t)d->desc >> 32));
	wr32(va + QUEUE_DRIVER, (u32)(addr_t)d->avail);
	wr32(va + QUEUE_DRIVER + 4, (u32)((addr_t)d->avail >> 32));
	wr32(va + QUEUE_DEVICE, (u32)(addr_t)d->used);
	wr32(va + QUEUE_DEVICE + 4, (u32)((addr_t)d->used >> 32));
	wr32(va + QUEUE_READY, 1);
	for (u16 i = 0; i < NBUF; i++) offer(d, i);
	wr32(va + STATUS, 3 | 8 | 4);
	asm volatile("dsb sy" ::: "memory");
	wr32(va + QUEUE_NOTIFY, 0);
	ndev++;
	kprintf("virtio-input: %s\n", d->max[0] ? "a tablet" : "a keyboard");
	return true;
}
