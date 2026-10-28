/* KolibriOS for ARM: the Nokia N950 (TI OMAP3630, Cortex-A8).
 *
 * NOT TRIED ON THE DEVICE YET. What is here follows the OMAP36xx
 * technical reference manual and Linux's omap3-n950.dts:
 *   - debug output on UART3 (0x49020000, 16550-like)
 *   - the interrupt controller (INTC, 0x48200000) and the 100 Hz tick
 *     from GPTIMER1 (32 kHz, IRQ 37)
 *   - the display as the boot loader left it: NOLO (or Linux before
 *     kexec) has set up the DSI panel (Himalaya, command mode, 480x864,
 *     used in landscape) and DISPC's graphics layer; the kernel draws
 *     into a landscape buffer of its own and copies it, turned, into
 *     the graphics layer's buffer, then has DISPC send one frame (stall
 *     mode: LCDENABLE starts a transfer, as for command mode panels)
 * Not yet: the touch screen (Atmel maXTouch on I2C2, 0x4b) and the
 * keyboard (the TWL4030's keypad on I2C1), the RTC (TWL4030). */
#include "kernel.h"

#define UART3 0x49020000u
#define INTC 0x48200000u
#define GPT1 0x48318000u
#define CM_FCLKEN_WKUP 0x48004c00u
#define CM_ICLKEN_WKUP 0x48004c10u
#define DISPC 0x48050400u
#define IRQ_GPT1 37

/* DISPC registers (offsets from DISPC) */
#define DISPC_CONTROL 0x40
#define DISPC_SIZE_LCD 0x7c
#define DISPC_GFX_BA0 0x80
#define DISPC_GFX_SIZE 0x8c
#define DISPC_GFX_ATTR 0xa0
#define DISPC_GFX_ROW_INC 0xac
#define DISPC_GFX_PIXEL_INC 0xb0

/* the panel: 480 x 864 in its own (portrait) orientation; of the 864
 * lines the 5 at each end are hidden. Landscape: 854 x 480 */
#define PANEL_W 480
#define PANEL_H 864
#define HIDDEN 5
#define SCR_W (PANEL_H - 2 * HIDDEN)
#define SCR_H PANEL_W

static void n9_putc(char c)
{
	for (int i = 0; i < 100000 && !(rd32(UART3 + 0x14) & 0x20); i++)
		;
	wr32(UART3, (u8)c);
}

static void n9_early_init(void)
{
}

static void n9_init(void)
{
	/* the INTC: GPTIMER1 only, for now */
	wr32(INTC + 0x88 + 0x20 * (IRQ_GPT1 / 32), 1u << (IRQ_GPT1 % 32)); /* MIR_CLEAR */
	/* GPTIMER1 on the 32 kHz clock: overflow at 100 Hz, auto reload */
	wr32(CM_FCLKEN_WKUP, rd32(CM_FCLKEN_WKUP) | 1);
	wr32(CM_ICLKEN_WKUP, rd32(CM_ICLKEN_WKUP) | 1);
	wr32(GPT1 + 0x24, 0);                         /* TCLR: stopped */
	wr32(GPT1 + 0x2c, 0xffffffffu - 328 + 1);     /* TLDR: 32768 / 100 */
	wr32(GPT1 + 0x28, 0xffffffffu - 328 + 1);     /* TCRR */
	wr32(GPT1 + 0x18, 7);                         /* TISR: clear */
	wr32(GPT1 + 0x1c, 2);                         /* TIER: overflow */
	wr32(GPT1 + 0x24, 3);                         /* TCLR: start, reload */
}

static void n9_irq(void)
{
	u32 irq = rd32(INTC + 0x40) & 0x7f;           /* SIR_IRQ */
	if (irq == IRQ_GPT1) {
		wr32(GPT1 + 0x18, 2);
		timer_tick();
	}
	wr32(INTC + 0x48, 1);                         /* CONTROL: NEWIRQAGR */
}

static u32 *shadow, *lcd;
static int lcd_bpp, lcd_w, lcd_h, lcd_stride;

static int n9_display_init(u32 **fb, int *w, int *h, int *stride)
{
	u32 ba = rd32(DISPC + DISPC_GFX_BA0);
	u32 size = rd32(DISPC + DISPC_GFX_SIZE);
	u32 fmt = (rd32(DISPC + DISPC_GFX_ATTR) >> 1) & 0xf;
	lcd_w = (size & 0x7ff) + 1;
	lcd_h = ((size >> 16) & 0x7ff) + 1;
	kprintf("DISPC: graphics at %p, %dx%d, format %u, control %x\n",
		ba, lcd_w, lcd_h, fmt, rd32(DISPC + DISPC_CONTROL));
	if (!ba || (fmt != 0x6 && fmt != 0x8 && fmt != 0xc && fmt != 0xd))
		return -1;
	lcd_bpp = fmt == 0x6 ? 16 : 32;
	lcd_stride = lcd_w;
	lcd = (u32 *)ba;
	u32 fbsize = ALIGN_UP(lcd_w * lcd_h * (lcd_bpp / 8) + (ba & 0xfffff), 1u << 20);
	mm_reserve(ba & 0xfff00000u, fbsize);
	mmu_set_uncached(ba & 0xfff00000u, fbsize);
	u32 mb = ALIGN_UP(SCR_W * SCR_H * 4, 1u << 20) >> 20;
	shadow = (u32 *)mm_alloc_mb(mb);
	if (!shadow) return -1;
	*fb = shadow;
	*w = SCR_W;
	*h = SCR_H;
	*stride = SCR_W;
	return 0;
}

/* landscape (x, y) to the panel's portrait pixel: turned by 90 degrees
 * (the keyboard side down); the direction is a guess until it is seen
 * on the device */
static bool n9_display_flush(int y0, int y1)
{
	static u32 last;
	if (ticks - last < 3) return false;           /* at most ~30 frames a second */
	last = ticks;
	for (int y = y0; y < y1; y++) {
		const u32 *src = &shadow[y * SCR_W];
		int px = PANEL_W - 1 - y;
		if (px < 0 || px >= lcd_w) continue;
		if (lcd_bpp == 32) {
			for (int x = 0; x < SCR_W; x++) {
				int py = HIDDEN + x;
				if (py < lcd_h) lcd[py * lcd_stride + px] = src[x];
			}
		} else {
			u16 *l16 = (u16 *)lcd;
			for (int x = 0; x < SCR_W; x++) {
				int py = HIDDEN + x;
				u32 c = src[x];
				if (py < lcd_h)
					l16[py * lcd_stride + px] = ((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 0x1f);
			}
		}
	}
	/* one frame to the panel: GOLCD, then LCDENABLE (stall mode) */
	u32 c = rd32(DISPC + DISPC_CONTROL);
	wr32(DISPC + DISPC_CONTROL, c | (1u << 5));
	wr32(DISPC + DISPC_CONTROL, c | (1u << 5) | 1);
	return true;
}

static const struct mem_region n9_io[] = {
	{ 0x48000000u, 0x48000000u, 16u << 20, MAP_DEVICE },  /* L4 core, L4 wakeup */
	{ 0x49000000u, 0x49000000u, 1u << 20, MAP_DEVICE },   /* L4 peripherals */
};

const struct platform platform_n950 = {
	.name = "Nokia N950 (OMAP3630)",
	.ram_base = 0x80000000u,
	.ram_size = 256u << 20,       /* of 1 GiB: enough, and clear of what NOLO keeps */
	.io = n9_io,
	.nio = ARRAY_SIZE(n9_io),
	.early_init = n9_early_init,
	.init = n9_init,
	.putc = n9_putc,
	.irq = n9_irq,
	.display_init = n9_display_init,
	.display_flush = n9_display_flush,
	.rtc_time = 0,
	.cached_data = true,
};
