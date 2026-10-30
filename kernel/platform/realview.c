/* KolibriOS for ARM: QEMU's realview-pb-a8 (a Cortex-A8 like the N950's):
 * PL011 UART, SP804 timer, GIC, PL111 display, PL050 keyboard and mouse,
 * PL031 clock, PL181 card slot (storage tests: -drive if=sd, or
 * -device emmc,bus=sd-bus). The development platform:
 *
 *   qemu-system-arm -M realview-pb-a8 -m 256 -kernel build/realview/kernel.elf */
#include "kernel.h"
#include "../storage/storage.h"
#include "../storage/pl181.h"

#define IO_PA 0x10000000u
#define IO_VA 0xf0000000u
#define GIC_PA 0x1e000000u
#define GIC_VA 0xf1000000u

#define UART0 0x10009000u
#define KMI_KBD 0x10006000u
#define KMI_MOUSE 0x10007000u
#define TIMER0 0x10011000u
#define RTC 0x10017000u
#define CLCD 0x10020000u
#define GICC 0x1e000000u
#define GICD 0x1e001000u
#define MMCI 0x10005000u
#define SYS_24MHZ 0x1000005cu

#define IRQ_TIMER0 (32 + 4)
#define IRQ_KMI_KBD (32 + 20)
#define IRQ_KMI_MOUSE (32 + 21)

/* the devices' addresses: physical until the MMU is on, then the
 * kernel's mapping of them (above 1 GiB) */
static addr_t io(addr_t pa)
{
	u32 sctlr;
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	return (sctlr & 1) ? mmu_io_va(pa) : pa;
}

static void rv_putc(char c)
{
	addr_t u = io(UART0);
	while (rd32(u + 0x18) & (1 << 5))   /* TXFF */
		;
	wr32(u, (u8)c);
}

static void rv_early_init(void)
{
}

static void gic_enable(int id)
{
	addr_t d = io(GICD);
	wr8(d + 0x400 + id, 0xa0);           /* priority */
	wr8(d + 0x800 + id, 1);              /* to CPU 0 */
	wr32(d + 0x100 + 4 * (id / 32), 1u << (id % 32));
}

static void kmi_send(addr_t base, u8 b)
{
	for (int i = 0; i < 100000 && !(rd32(base + 0x04) & (1 << 6)); i++)
		;
	wr32(base + 0x08, b);
}

/* the 24 MHz counter as microseconds, its wraps counted */
static u64 now_us(void)
{
	static u32 last;
	static u64 high;
	u32 v = rd32(io(SYS_24MHZ));
	if (v < last) high += 1ull << 32;
	last = v;
	return (high | v) / 24;
}

bool storage_off(void);
int storage_home_mode(void);

static void rv_init(void)
{
	addr_t d = io(GICD), c = io(GICC);
	wr32(d + 0x000, 1);
	wr32(c + 0x004, 0xf0);               /* priority mask */
	wr32(c + 0x000, 1);
	/* the 100 Hz tick: timer 0 of the first SP804, 1 MHz */
	addr_t t = io(TIMER0);
	wr32(t + 0x08, 0);
	wr32(t + 0x00, 10000);
	wr32(t + 0x08, 0x80 | 0x40 | 0x20 | 0x02);
	gic_enable(IRQ_TIMER0);
	/* keyboard and mouse (the mouse reports only once told to) */
	wr32(io(KMI_KBD) + 0x00, 0x14);      /* enable, receive interrupt */
	wr32(io(KMI_MOUSE) + 0x00, 0x14);
	kmi_send(io(KMI_MOUSE), 0xf4);
	gic_enable(IRQ_KMI_KBD);
	gic_enable(IRQ_KMI_MOUSE);
	/* the card slot (kernel/storage) */
	if (!storage_off()) {
		static struct pl181 mmci;
		mmci.base = io(MMCI);
		mmci.now_us = now_us;
		pl181_start(&mmci, "mmc0", storage_home_mode() == HOME_RW);
	}
}

static u8 mpkt[3];
static int mpos = -1;    /* -1: the mouse's acknowledgement first */

static void mouse_byte(u8 b)
{
	if (mpos < 0) { if (b == 0xfa) mpos = 0; return; }
	if (mpos == 0 && !(b & 0x08)) return; /* out of step */
	mpkt[mpos++] = b;
	if (mpos < 3) return;
	mpos = 0;
	int dx = mpkt[1] - ((mpkt[0] << 4) & 0x100);
	int dy = mpkt[2] - ((mpkt[0] << 3) & 0x100);
	input_mouse_rel(dx, -dy, mpkt[0] & 7, 0);
}

static void rv_irq(void)
{
	addr_t c = io(GICC);
	for (;;) {
		u32 iar = rd32(c + 0x0c);
		u32 id = iar & 0x3ff;
		if (id >= 1020) break;
		switch (id) {
		case IRQ_TIMER0:
			wr32(io(TIMER0) + 0x0c, 1);
			timer_tick();
			break;
		case IRQ_KMI_KBD:
			while (rd32(io(KMI_KBD) + 0x04) & (1 << 4))
				input_key_set2(rd32(io(KMI_KBD) + 0x08));
			break;
		case IRQ_KMI_MOUSE:
			while (rd32(io(KMI_MOUSE) + 0x04) & (1 << 4))
				mouse_byte(rd32(io(KMI_MOUSE) + 0x08));
			break;
		default:
			kprintf("irq %u?\n", id);
			break;
		}
		wr32(c + 0x10, iar);
	}
}

#define FB_W 800
#define FB_H 480

static int rv_display_init(u32 **fb, int *w, int *h, int *stride)
{
	u32 mb = ALIGN_UP(FB_W * FB_H * 4, 1u << 20) >> 20;
	addr_t pa = mm_alloc_mb(mb);
	if (!pa) return -1;
	mmu_set_uncached(pa, mb << 20);
	addr_t l = io(CLCD);
	/* the PL111 has its control register at 0x18, the PL110 at 0x1c */
	bool pl111 = (rd32(l + 0xfe0) & 0xff) == 0x11;
	wr32(l + 0x00, ((FB_W / 16 - 1) << 2));
	wr32(l + 0x04, FB_H - 1);
	wr32(l + 0x08, 0);
	wr32(l + 0x10, pa);
	wr32(l + (pl111 ? 0x18 : 0x1c), 1 | (5 << 1) | (1 << 5) | (1 << 8) | (1 << 11)); /* 24 bpp in 32, TFT, BGR (0x00RRGGBB in memory), on */
	*fb = (u32 *)pa;
	*w = FB_W;
	*h = FB_H;
	*stride = FB_W;
	return 0;
}

static bool rv_display_flush(int y0, int y1)
{
	(void)y0; (void)y1;     /* the controller reads the (uncached) buffer itself */
	return true;
}

static u32 rv_rtc_time(void)
{
	return rd32(io(RTC));
}

static const struct mem_region rv_io[] = {
	{ IO_VA, IO_PA, 1u << 20, MAP_DEVICE },
	{ GIC_VA, GIC_PA, 1u << 20, MAP_DEVICE },
};

struct platform platform_realview = {
	.name = "realview-pb-a8 (QEMU)",
	.ram_base = 0x70000000,
	.ram_size = 256u << 20,
	.io = rv_io,
	.nio = ARRAY_SIZE(rv_io),
	.early_init = rv_early_init,
	.init = rv_init,
	.putc = rv_putc,
	.irq = rv_irq,
	.display_init = rv_display_init,
	.display_flush = rv_display_flush,
	.rtc_time = rv_rtc_time,
	.cached_data = true,
};
