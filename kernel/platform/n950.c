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
 *   - the keyboard: the TWL4030's keypad matrix (I2C1, 0x4a), polled; the
 *     key map is omap3-n950.dts's (tools/n950-keymap.py)
 *   - the touch screen: Atmel maXTouch (I2C2, 0x4b), T9 messages through
 *     T5, polled; the first finger is the mouse with its left button
 *   - the eMMC (MMC2, an OMAP3 HSMMC): its clocks and supply (VEMMC, GPIO
 *     157) on, the controller reset, the card started anew (sdhci.c,
 *     mmc.c); its partitions read only, the container (docs/storage.md)
 *   - random numbers: the secure ROM's RNG through the PPA (below), the
 *     timing jitter of the cycle counter against the 32 kHz clock (rng.c)
 * Not yet: the RTC (TWL4030), the battery. */
#include "kernel.h"
#include "blit.h"
#include "rng.h"
#include "../storage/storage.h"
#include "../storage/sdhci.h"

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

static void keypad_poll(void);
static void touch_init(void);
static void touch_poll(void);

static void n9_putc(char c)
{
	for (int i = 0; i < 100000 && !(rd32(UART3 + 0x14) & 0x20); i++)
		;
	wr32(UART3, (u8)c);
}

static void n9_early_init(void)
{
}

/* --- the eMMC ----------------------------------------------------------- */

#define MMC2 0x480b4000u
#define CM_FCLKEN1_CORE 0x48004a00u
#define CM_ICLKEN1_CORE 0x48004a10u
#define CM_IDLEST1_CORE 0x48004a20u
#define CM_FCLKEN_PER 0x48005000u
#define CM_ICLKEN_PER 0x48005010u
#define GPIO5 0x49056000u
#define EN_MMC2 (1u << 25)
#define VEMMC_PIN 29                   /* GPIO 157: gpio5's 29th, VEMMC's enable */
#define SYNC32K 0x48320010u            /* the 32 kHz counter */

static u64 now_us(void)
{
	static u32 last;
	static u64 high;
	u32 v = rd32(SYNC32K);
	if (v < last) high += 1ull << 32;
	last = v;
	return ((high | v) * 15625) >> 9;     /* * 10^6 / 32768 */
}

bool storage_off(void);
int storage_home_mode(void);

static void emmc_init(void)
{
	wr32(CM_ICLKEN1_CORE, rd32(CM_ICLKEN1_CORE) | EN_MMC2);
	wr32(CM_FCLKEN1_CORE, rd32(CM_FCLKEN1_CORE) | EN_MMC2);
	for (int i = 0; i < 100000 && (rd32(CM_IDLEST1_CORE) & EN_MMC2); i++)
		;
	if (rd32(CM_IDLEST1_CORE) & EN_MMC2) {
		kprintf("n950: MMC2's clocks do not come on: no eMMC\n");
		return;
	}
	/* the supply: after a second without requests ubiboot's Linux may
	 * have switched it off */
	wr32(CM_ICLKEN_PER, rd32(CM_ICLKEN_PER) | (1u << 16));
	wr32(CM_FCLKEN_PER, rd32(CM_FCLKEN_PER) | (1u << 16));
	bool was_on = rd32(GPIO5 + 0x03c) & (1u << VEMMC_PIN);    /* DATAOUT */
	wr32(GPIO5 + 0x094, 1u << VEMMC_PIN);                     /* SETDATAOUT */
	wr32(GPIO5 + 0x034, rd32(GPIO5 + 0x034) & ~(1u << VEMMC_PIN));   /* OE: an output */
	kprintf("n950: VEMMC %s\n", was_on ? "was on" : "switched on");
	u64 t0 = now_us();
	while (now_us() - t0 < (was_on ? 1000 : 10000))
		;
	static struct sdhci hs;
	hs.base = MMC2;
	hs.omap = true;
	hs.now_us = now_us;
	sdhci_start(&hs, "mmc0", storage_home_mode() == HOME_RW);
}

const char *boot_arg(const char *name);

/* --- the secure ROM's random numbers -------------------------------------
 * The OMAP3630's RNG module sits behind the secure side's firewall on the
 * N950, an HS device (Linux's omap3-n950-n9.dtsi: "RNG not directly
 * accessible on N950/N9", rng_target disabled). Harmattan's kernel read it
 * through Nokia's PPA (arch/arm/mach-omap2/hs.c, sec_hal.c, sec_hal_entry.S:
 * SEC_HAL_RNG_GENERATE, RNG_GEN_HW): smc #1 with the service (29) in r12,
 * r6 0xff (a new call), the flag 4 (FLAG_START_HAL_CRITICAL) in r2 and in
 * r3 the physical address of { 3, buffer, length, command }; the caches
 * cleaned before and invalidated after, the interrupts masked (they are in
 * the kernel). Linux's omap3-rom-rng reads the N900's PPA the same way
 * (service 29, there with nargs + 1 and flag 0), with rng_ick on and
 * RNG_GEN_PRNG_HW_INIT first; that is the second try here.
 * NOT TRIED ON THE DEVICE: kolibri.romrng=off leaves the PPA alone. */
#define CM_ICLKEN2_CORE 0x48004a14u     /* bit 2: EN_RNG (rng_ick) */
#define PPA_RNG 29
#define PPA_FLAG 4
#define RNG_GEN_PRNG_HW_INIT 0x02
#define RNG_GEN_HW 0x08

u32 n950_smc(u32 service, u32 process, u32 flag, u32 params_pa);
asm(".text\n.arm\n.arch_extension sec\n.balign 4\n"
    ".global n950_smc\n.type n950_smc, %function\n"
    "n950_smc:\n"
    "	push	{r4-r12, lr}\n"
    "	mov	r12, r0\n"
    "	mov	r6, #0xff\n"
    "	dsb\n"
    "	smc	#1\n"
    "	mov	r1, #0\n"
    "	mcr	p15, 0, r1, c7, c5, 0\n"
    "	pop	{r4-r12, pc}\n");

/* the parameters and the buffer in cache lines of their own (the kernel's
 * RAM is mapped at its physical address) */
static struct {
	u32 p[16];
	u8 buf[64];
} ppa __attribute__((aligned(64)));

static void dcache_clean_inv(const void *a, u32 n)
{
	for (addr_t x = (addr_t)a & ~63u; x < (addr_t)a + n; x += 64)
		asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(x));  /* DCCIMVAC */
	asm volatile("dsb" ::: "memory");
}

static void dcache_inv(const void *a, u32 n)
{
	for (addr_t x = (addr_t)a & ~63u; x < (addr_t)a + n; x += 64)
		asm volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(x));   /* DCIMVAC */
	asm volatile("dsb" ::: "memory");
}

static u32 ppa_rng(u32 cmd, u32 len)
{
	ppa.p[0] = 3;
	ppa.p[1] = cmd == RNG_GEN_HW ? (u32)(addr_t)ppa.buf : 0;
	ppa.p[2] = len;
	ppa.p[3] = cmd;
	memset(ppa.buf, 0, sizeof(ppa.buf));
	dcache_clean_inv(&ppa, sizeof(ppa));
	ARCH_IRQ_OFF();
	u32 r = n950_smc(PPA_RNG, 0, PPA_FLAG, (u32)(addr_t)ppa.p);
	dcache_inv(&ppa, sizeof(ppa));
	return r;
}

static u32 rom_rng_read(void *out, u32 len)
{
	u32 done = 0;
	while (done < len) {
		u32 n = MIN(len - done, (u32)sizeof(ppa.buf));
		u32 r = ppa_rng(RNG_GEN_HW, n);
		if (r) {
			static u32 said;
			if (said++ < 3) kprintf("n950: the secure ROM's RNG: error 0x%x\n", r);
			break;
		}
		memcpy((u8 *)out + done, ppa.buf, n);
		done += n;
	}
	memset(ppa.buf, 0, sizeof(ppa.buf));
	return done;
}

static void rom_rng_init(void)
{
	const char *a = boot_arg("kolibri.romrng");
	if (a && !strncmp(a, "off", 3)) {
		kprintf("n950: the secure ROM's RNG left alone (kolibri.romrng=off)\n");
		return;
	}
	wr32(CM_ICLKEN2_CORE, rd32(CM_ICLKEN2_CORE) | 1u << 2);
	kprintf("n950: the secure ROM's RNG (PPA service 29): asking ...\n");
	u32 r = ppa_rng(RNG_GEN_HW, 16), r2 = 0;
	if (r) {
		r2 = ppa_rng(RNG_GEN_PRNG_HW_INIT, 0);
		r = r2 ? r : ppa_rng(RNG_GEN_HW, 16);
	}
	u32 any = 0;
	for (int i = 0; i < 16; i++) any |= ppa.buf[i];
	memset(ppa.buf, 0, sizeof(ppa.buf));
	if (r || !any) {
		kprintf("n950: the secure ROM's RNG: %s (0x%x, init 0x%x): not used\n",
			r ? "an error" : "only zeros", r, r2);
		return;
	}
	kprintf("n950: the secure ROM's RNG answers\n");
	/* Linux's omap3-rom-rng: quality 900 of 1024 */
	rng_register("the secure ROM's RNG", rom_rng_read, 900);
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
	touch_init();
	if (!storage_off()) emmc_init();
	rom_rng_init();
}

static void n9_irq(void)
{
	u32 irq = rd32(INTC + 0x40) & 0x7f;           /* SIR_IRQ */
	if (irq == IRQ_GPT1) {
		wr32(GPT1 + 0x18, 2);
		timer_tick();
		/* the keyboard and the touch screen, polled at 50 Hz */
		if (ticks & 1) keypad_poll();
		else touch_poll();
	}
	wr32(INTC + 0x48, 1);                         /* CONTROL: NEWIRQAGR */
}

/* --- I2C (OMAP3 I2C1-3, polled; the boot loader's clock setup kept) --- */

#define I2C1 0x48070000u
#define I2C2 0x48072000u
#define I2C_STAT 0x08
#define I2C_CNT 0x18
#define I2C_DATA 0x1c
#define I2C_CON 0x24
#define I2C_SA 0x2c
#define ST_NACK 0x0002
#define ST_AL 0x0001
#define ST_ARDY 0x0004
#define ST_RRDY 0x0008
#define ST_XRDY 0x0010
#define ST_BB 0x1000
#define CON_EN 0x8000
#define CON_MST 0x0400
#define CON_TRX 0x0200
#define CON_STP 0x0002
#define CON_STT 0x0001

static u16 i2c_r(addr_t b, u32 o) { return *(volatile u16 *)(b + o); }
static void i2c_w(addr_t b, u32 o, u16 v) { *(volatile u16 *)(b + o) = v; }

static int i2c_wait(addr_t b, u16 bits)
{
	for (int i = 0; i < 20000; i++) {
		u16 st = i2c_r(b, I2C_STAT);
		if (st & (ST_NACK | ST_AL)) {
			i2c_w(b, I2C_CON, i2c_r(b, I2C_CON) | CON_STP);
			i2c_w(b, I2C_STAT, st);
			return -1;
		}
		if (st & bits) return st;
	}
	return -2;
}

/* write wlen bytes, then (repeated start) read rlen: 0 or < 0 */
static int i2c_xfer(addr_t b, u8 addr, const u8 *w, int wlen, u8 *r, int rlen)
{
	for (int i = 0; i < 20000 && (i2c_r(b, I2C_STAT) & ST_BB); i++)
		;
	i2c_w(b, I2C_STAT, 0xffff);
	i2c_w(b, I2C_SA, addr);
	if (wlen) {
		i2c_w(b, I2C_CNT, wlen);
		i2c_w(b, I2C_CON, CON_EN | CON_MST | CON_TRX | CON_STT | (rlen ? 0 : CON_STP));
		for (int i = 0; i < wlen; i++) {
			if (i2c_wait(b, ST_XRDY) < 0) return -1;
			i2c_w(b, I2C_DATA, w[i]);  /* 16-bit accesses, as Linux makes them */
			i2c_w(b, I2C_STAT, ST_XRDY);
		}
		if (i2c_wait(b, ST_ARDY) < 0) return -1;
		i2c_w(b, I2C_STAT, ST_ARDY);
	}
	if (rlen) {
		i2c_w(b, I2C_CNT, rlen);
		i2c_w(b, I2C_CON, CON_EN | CON_MST | CON_STT | CON_STP);
		for (int i = 0; i < rlen; i++) {
			if (i2c_wait(b, ST_RRDY) < 0) return -1;
			r[i] = i2c_r(b, I2C_DATA) & 0xff;
			i2c_w(b, I2C_STAT, ST_RRDY);
		}
		if (i2c_wait(b, ST_ARDY) < 0) return -1;
		i2c_w(b, I2C_STAT, ST_ARDY);
	}
	return 0;
}

/* --- the keyboard: the TWL4030's keypad (I2C1, 0x4a, registers from 0xd2) --- */

#include "n950_keymap.h"
#define TWL_KEYPAD 0x4a
#define KEYP_FULL_CODE (0xd2 + 0x09)   /* 8 bytes: a row's columns each */
static u8 keys_down[8];
static bool keypad_ok = true;

static void keypad_poll(void)
{
	u8 reg = KEYP_FULL_CODE, now[8];
	if (!keypad_ok) return;
	if (i2c_xfer(I2C1, TWL_KEYPAD, &reg, 1, now, 8) < 0) {
		kprintf("n950: keypad does not answer\n");
		keypad_ok = false;
		return;
	}
	for (int r = 0; r < 8; r++) {
		if (now[r] == 0xff) continue;             /* ghosting: ignored */
		u8 ch = now[r] ^ keys_down[r];
		for (int c = 0; c < 8 && ch; c++)
			if (ch & (1 << c)) {
				u8 sc = n950_keymap[r][c];
				if (sc) input_key(sc, !(now[r] & (1 << c)));
			}
		keys_down[r] = now[r];
	}
}

/* --- the touch screen: Atmel maXTouch (I2C2, 0x4b) ---------------------
 * Its registers are objects listed in a table after the 7-byte information
 * block at address 0; touches come as T9 messages from T5's address. */

#define MXT 0x4b
static u16 t5_addr, t9_range_x = 1023, t9_range_y = 1023;
static u8 t5_size, t9_first_id, t9_last_id;
static bool touch_ok;

static int mxt_read(u16 reg, u8 *buf, int n)
{
	u8 a[2] = { reg & 0xff, reg >> 8 };
	return i2c_xfer(I2C2, MXT, a, 2, buf, n);
}

static void touch_init(void)
{
	u8 info[7], obj[6];
	if (mxt_read(0, info, 7) < 0) {
		kprintf("n950: maXTouch does not answer\n");
		return;
	}
	kprintf("n950: maXTouch family %x variant %x version %x, %u objects\n",
		info[0], info[1], info[2], info[6]);
	u8 id = 1;
	u16 t9_addr = 0;
	for (int i = 0; i < info[6]; i++) {
		if (mxt_read(7 + i * 6, obj, 6) < 0) return;
		u8 type = obj[0], inst = obj[4] + 1, nrep = obj[5];
		u16 addr = obj[1] | obj[2] << 8;
		if (type == 5) { t5_addr = addr; t5_size = obj[3] + 1; }
		if (type == 9) { t9_addr = addr; t9_first_id = id; t9_last_id = id + nrep * inst - 1; }
		id += nrep * inst;
	}
	if (!t5_addr || !t9_addr) {
		kprintf("n950: maXTouch without T5/T9\n");
		return;
	}
	u8 range[4];
	if (mxt_read(t9_addr + 18, range, 4) == 0) {
		t9_range_x = range[0] | range[1] << 8;
		t9_range_y = range[2] | range[3] << 8;
	}
	kprintf("n950: touch T9 reports %u-%u, range %ux%u\n", t9_first_id, t9_last_id, t9_range_x, t9_range_y);
	touch_ok = true;
}

/* the panel is portrait: its x along the short side; landscape x is its y
 * (directions to check on the device) */
static void touch_poll(void)
{
	u8 m[16];
	if (!touch_ok) return;
	for (int n = 0; n < 8; n++) {
		if (mxt_read(t5_addr, m, MIN(t5_size, sizeof(m))) < 0 || m[0] == 0xff) return;
		if (m[0] != t9_first_id) continue;      /* the first finger only */
		u32 x = (m[2] << 4) | (m[4] >> 4), y = (m[3] << 4) | (m[4] & 0xf);
		if (t9_range_x < 1024) x >>= 2;
		if (t9_range_y < 1024) y >>= 2;
		int sx = y * (SCR_W - 1) / (t9_range_y ? t9_range_y : 1);
		int sy = (SCR_H - 1) - x * (SCR_H - 1) / (t9_range_x ? t9_range_x : 1);
		bool down = (m[1] & 0x80) && !(m[1] & 0x20);   /* DETECT, not RELEASE */
		input_mouse_abs(sx, sy, down ? 1 : 0);
	}
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

/* landscape (x, y) to the panel's portrait pixel (PANEL_W - 1 - y,
 * HIDDEN + x): turned by 90 degrees (the keyboard side down); the
 * direction is a guess until it is seen on the device. A screen column
 * is a panel line, the band's bottom line first: blit.rot90 writes the
 * panel (uncached) line by line, in whole blocks of 16 lines, so that
 * each panel line's run starts 64 bytes aligned (32 at 16 bpp) */
static void n9_copy(int y0, int y1)
{
	y0 &= ~15;
	y1 = MIN((y1 + 15) & ~15, SCR_H);
	/* only the panel's own pixels: 0 <= PANEL_W - 1 - y < lcd_w,
	 * HIDDEN + x < lcd_h */
	int lo = MAX(y0, PANEL_W - lcd_w), hi = MIN(y1, PANEL_W);
	int w = MIN(SCR_W, lcd_h - HIDDEN);
	if (lo >= hi || w <= 0) return;
	u32 bytes = lcd_bpp / 8;
	char *d = (char *)lcd + (u32)HIDDEN * lcd_stride * bytes + (u32)(PANEL_W - hi) * bytes;
	blit_begin();
	blit.rot90(d, lcd_stride * bytes, shadow + lo * SCR_W, SCR_W, w, hi - lo, lcd_bpp);
	asm volatile("dsb" ::: "memory");             /* the frame in memory before DISPC goes */
}

#ifndef SIMD_C
/* kolibri.bench (bench.c): the whole screen copied as the flush did
 * before blit.rot90, each screen line a panel column, a pixel at a
 * time: the uncached frame buffer written 4 (2) bytes a store, a panel
 * line apart */
void n9_flush_by_columns(void)
{
	for (int y = 0; y < SCR_H; y++) {
		const u32 *src = &shadow[y * SCR_W];
		int px = PANEL_W - 1 - y;
		if (px < 0 || px >= lcd_w) continue;
		for (int x = 0; x < SCR_W && HIDDEN + x < lcd_h; x++) {
			u32 c = src[x];
			if (lcd_bpp == 32)
				lcd[(HIDDEN + x) * lcd_stride + px] = c;
			else
				((u16 *)lcd)[(HIDDEN + x) * lcd_stride + px] =
					((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 0x1f);
		}
	}
	asm volatile("dsb" ::: "memory");
}
#endif

static bool n9_display_flush(int y0, int y1)
{
	static u32 last;
	if (ticks - last < 3) return false;           /* at most ~30 frames a second */
	last = ticks;
	n9_copy(y0, y1);
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

struct platform platform_n950 = {
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
