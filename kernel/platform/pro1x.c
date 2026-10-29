/* KolibriOS for ARM, AArch64: the F(x)tec Pro1X (Qualcomm SM6115).
 *
 * Started with kexec from Linux, with Linux's device tree: from the boot
 * menu of pro1x-maemo-leste or from a running Maemo Leste
 * (tools/pro1x-kexec.sh). Linux leaves the display running (its msm
 * driver is patched for kexec), the keyboard's I2C engine clocked and the
 * SoC's watchdog armed. After FreeBSD's Pro1X port
 * (pro1x-maemo-leste/freebsd/patches), this platform drives:
 *
 *  - the display: the bootloader's framebuffer (/chosen's
 *    simple-framebuffer, 1080x2160 portrait, in reserved memory) made the
 *    display's scanout again: its pipes pointed at it, the SMMU context of
 *    the display bypassed. The kernel draws a landscape screen of half the
 *    resolution, 1080x540, which the flush doubles and turns clockwise
 *    (the keyboard below, as X's "Rotate CW" on the phone);
 *  - traces that Linux reads after the next boot (tools/pro1x-read.py):
 *    PRO1X-KOS:ENTERED at 0x5ca00000, the kernel's output from 0x5ca01000
 *    (the RAM console), a screenshot at 0x5cc00000 (a PPM, at the end of
 *    the lifetime and at a panic): the spare part of the splash memory,
 *    which nobody else writes and which survives a reset;
 *  - the APSS watchdog, fed every second; kolibri.lifetime=N on the
 *    command line stops that after N s, so that an unattended test ends
 *    in the boot menu again;
 *  - the keyboard: an 8x8 matrix behind an AW9523B GPIO expander on a GENI
 *    I2C engine (polled, its FIFOs), the modifier keys as TLMM GPIOs;
 *  - the touch screen, a Goodix GT9286 on bit-banged GPIO I2C, as the mouse.
 *
 * kolibri.noinput leaves the keyboard and the touch screen alone. The
 * clock (the PMIC's RTC, over SPMI) is not driven: the time starts at 0. */
#include "kernel.h"
#include "fdt.h"
#include "../modem/phone.h"
#include "../modem/glink.h"
#include "../modem/callaudio.h"

void gic3_init(addr_t dist_pa, addr_t redist_pa, void (*irq)(u32 id));
void gic3_irq(void);
extern addr_t boot_args[3];

#define MB (1ul << 20)
#define SPLASH 0x5c000000ul            /* the splash memory, 16 MiB, reserved */
#define MARKER (SPLASH + 0xa00000)
#define RAMCONS (SPLASH + 0xa01000)
#define RAMCONS_SIZE (1u << 20)
#define SHOT (SPLASH + 0xc00000)
#define SHOT_SIZE (3u << 20)

static struct mem_region io[24];
static addr_t gicd, gicr, wdt, mdp, smmu, geni, tlmm[3];
static u32 streams[8][2];              /* the display's SMMU stream ids, masks */
static int nstreams;
static addr_t fb_pa;
static int fb_w, fb_h, fb_stride;      /* the panel, portrait; stride in bytes */
static u32 lifetime;                   /* s, 0: forever */
static bool noinput;
/* the modem (docs/pro1x-modem.md): SMEM, the rmtfs buffer, the APCS IPC
 * register and the bit that kicks its GLINK edge, the EFS images (initrd) */
static addr_t smem_pa, rmtfs_pa, apcs_pa, efs_pa;
static u32 smem_size, rmtfs_size, apcs_bit, efs_size;
/* the ADSP's GLINK edge: its IPC bit (the call's sound, callaudio.c) */
static u32 adsp_bit = 0xffffffffu;
static bool callaudio;                 /* kolibri.callaudio: Linux left the voice path running */
static bool nomodem, fakemodem;

static addr_t io_va(addr_t pa)
{
	return (SYSREG_R(sctlr_el1) & 1) ? mmu_io_va(pa) : pa;
}

static void add_io(addr_t pa, u64 size, u32 attr)
{
	if (!pa || platform_pro1x.nio == (int)ARRAY_SIZE(io)) return;
	io[platform_pro1x.nio++] = (struct mem_region){ 0, pa, (u32)size, attr };
}

static void udelay(u32 us)
{
	u64 f = SYSREG_R(cntfrq_el0), t0 = SYSREG_R(cntvct_el0);
	while ((SYSREG_R(cntvct_el0) - t0) * 1000000 < (u64)us * f)
		;
}

/* --- the RAM console ------------------------------------------------- */

static u32 cons_pos;

static void cons_init(void)
{
	static const char marker[] = "PRO1X-KOS:ENTERED\n";
	static const char head[] = "PRO1X-KOS-CONSOLE\n";
	volatile char *m = (volatile char *)MARKER, *c = (volatile char *)RAMCONS;
	for (u32 i = 0; i < sizeof(marker); i++) m[i] = marker[i];
	for (cons_pos = 0; head[cons_pos]; cons_pos++) c[cons_pos] = head[cons_pos];
	c[cons_pos] = 0;
	asm volatile("dsb sy");
}

static void p_putc(char ch)
{
	volatile char *c = (volatile char *)RAMCONS;
	if (cons_pos + 1 >= RAMCONS_SIZE) return;
	c[cons_pos++] = ch;
	c[cons_pos] = 0;
	if (ch == '\n') asm volatile("dsb sy");
}

/* --- the watchdog (kpss layout, the 32 kHz sleep clock) ------------- */

#define WDT_RST 0x04
#define WDT_EN 0x08
#define WDT_BARK 0x10
#define WDT_BITE 0x14
#define WDT_MAX 0xfffff                /* about 32 s */

static bool wdt_feeding = true, wdt_was_running;

/* first thing (the probe, MMU off): kept running, or started */
static void wdt_start(void)
{
	if (!wdt) return;
	addr_t w = io_va(wdt);
	wdt_was_running = rd32(w + WDT_EN) & 1;
	wr32(w + WDT_RST, 1);
	if (!wdt_was_running) {
		wr32(w + WDT_BARK, WDT_MAX);
		wr32(w + WDT_BITE, WDT_MAX);
		wr32(w + WDT_EN, 1);
	}
}

/* --- the display ----------------------------------------------------- */

#define CTL0_FLUSH (0x1000 + 0x18)
#define FLUSH_CTL (1u << 17)
#define SSPP_SRC_SIZE 0x00
#define SSPP_SRC0_ADDR 0x14
#define SSPP_SRC_YSTRIDE0 0x24
#define SSPP_SRC_FORMAT 0x30
#define SSPP_SRC_OP_MODE 0x38

static const struct { const char *name; u32 base, flush; } pipes[] = {
	{ "VIG0", 0x4000, 1u << 0 },
	{ "DMA0", 0x24000, 1u << 11 },
};

/* translation off in the context banks of the display's streams: the
 * pipes then read the framebuffer's physical address (ARM MMU-500) */
static void smmu_bypass(void)
{
	addr_t s = io_va(smmu);
	u32 nsmr = rd32(s + 0x20) & 0xff, idr1 = rd32(s + 0x24);
	u32 pgshift = (idr1 & (1u << 31)) ? 16 : 12;
	u32 numpage = 1u << (((idr1 >> 28) & 7) + 1);
	for (int k = 0; k < nstreams; k++) {
		for (u32 i = 0; i < nsmr; i++) {
			u32 smr = rd32(s + 0x800 + 4 * i);
			if (!(smr & (1u << 31)) ||
			    ((smr ^ streams[k][0]) & 0x7fff & ~(streams[k][1] | (smr >> 16))))
				continue;
			u32 s2cr = rd32(s + 0xc00 + 4 * i);
			if ((s2cr >> 16) & 3) {
				kprintf("pro1x: stream 0x%x: S2CR 0x%x, no translation\n", streams[k][0], s2cr);
				continue;
			}
			addr_t cb = s + ((addr_t)(numpage + (s2cr & 0xff)) << pgshift);
			kprintf("pro1x: stream 0x%x: context bank %u, SCTLR 0x%x -> 0\n",
				streams[k][0], s2cr & 0xff, rd32(cb));
			wr32(cb, 0);
		}
	}
}

/* the display back on the bootloader's framebuffer (MMU off) */
static bool display_takeover(void)
{
	if (!mdp || !fb_pa) {
		kprintf("pro1x: no display controller or framebuffer in the device tree\n");
		return false;
	}
	addr_t m = io_va(mdp);
	u32 flush = 0;
	for (u32 i = 0; i < ARRAY_SIZE(pipes); i++) {
		addr_t p = m + pipes[i].base;
		u32 a = rd32(p + SSPP_SRC0_ADDR), size = rd32(p + SSPP_SRC_SIZE);
		kprintf("pro1x: %s: source 0x%x, %ux%u, stride %u, format 0x%x, mode 0x%x\n", pipes[i].name,
			a, size & 0xffff, size >> 16, rd32(p + SSPP_SRC_YSTRIDE0) & 0xffff,
			rd32(p + SSPP_SRC_FORMAT), rd32(p + SSPP_SRC_OP_MODE));
		if (a) flush |= pipes[i].flush;
	}
	if (!flush) {
		kprintf("pro1x: no pipe scans out: the display is off\n");
		return false;
	}
	if (smmu) smmu_bypass();
	for (u32 i = 0; i < ARRAY_SIZE(pipes); i++) {
		if (!(flush & pipes[i].flush)) continue;
		addr_t p = m + pipes[i].base;
		wr32(p + SSPP_SRC0_ADDR, (u32)fb_pa);
		wr32(p + SSPP_SRC_YSTRIDE0, (rd32(p + SSPP_SRC_YSTRIDE0) & 0xffff0000u) | (u32)fb_stride);
	}
	wr32(m + CTL0_FLUSH, flush | FLUSH_CTL);
	kprintf("pro1x: the display scans out from %p now\n", fb_pa);
	return true;
}

static u32 *lfb;                       /* the kernel's screen, landscape */
static int lw, lh;

static int p_display_init(u32 **fb, int *w, int *h, int *stride)
{
	if (!fb_pa) return -1;
	lw = fb_h / 2;
	lh = fb_w / 2;
	addr_t pa = mm_alloc_mb(ALIGN_UP((addr_t)lw * lh * 4, MB) / MB);
	if (!pa) return -1;
	lfb = (u32 *)pa;
	memset(lfb, 0, (size_t)lw * lh * 4);
	*fb = lfb;
	*w = lw;
	*h = lh;
	*stride = lw;
	return 0;
}

/* lines y0..y1 of the landscape screen, each pixel doubled, turned
 * clockwise: landscape (X, Y) is the panel's (fb_w - 1 - Y, X), so a line
 * of the screen is a pair of the panel's columns. Panel line by panel
 * line, two pixels in a 64-bit store, the alpha opaque. */
static bool p_display_flush(int y0, int y1)
{
	static u64 span[1080];
	y1 = MIN(y1, lh);
	if (y0 >= y1) return true;
	int n = MIN(y1 - y0, (int)ARRAY_SIZE(span));
	y0 = y1 - n;
	addr_t px0 = (addr_t)(fb_w - 2 * y1) * 4;
	for (int lx = 0; lx < lw; lx++) {
		const u32 *src = lfb + lx;
		for (int k = 0; k < n; k++) {
			u64 p = src[(addr_t)(y1 - 1 - k) * lw] | 0xff000000u;
			span[k] = p | p << 32;
		}
		for (int r = 0; r < 2; r++) {
			volatile u64 *dst = (volatile u64 *)(fb_pa + (addr_t)(2 * lx + r) * fb_stride + px0);
			for (int k = 0; k < n; k++) dst[k] = span[k];
		}
	}
	asm volatile("dsb st");
	return true;
}

/* the screen as a PPM where Linux finds it after the reset: "PRO1X-SHOT",
 * the length at offset 12, the data from 16 (FreeBSD's /dev/pro1xshot) */
static void shot(void)
{
	if (!lfb) return;
	volatile u8 *s = (volatile u8 *)SHOT;
	static const char magic[] = "PRO1X-SHOT\n";
	char hdr[24];
	int hl = 0;
	const char *h1 = "P6\n";
	while (*h1) hdr[hl++] = *h1++;
	int v[2] = { lw, lh };
	for (int j = 0; j < 2; j++) {
		char d[8];
		int nd = 0;
		for (int x = v[j]; x || !nd; x /= 10) d[nd++] = '0' + x % 10;
		while (nd) hdr[hl++] = d[--nd];
		hdr[hl++] = j ? '\n' : ' ';
	}
	for (const char *h2 = "255\n"; *h2; ) hdr[hl++] = *h2++;
	for (int i = 0; i < 16; i++) s[i] = i < (int)sizeof(magic) - 1 ? magic[i] : 0;
	u32 len = 0;
	for (int i = 0; i < hl; i++) s[16 + len++] = hdr[i];
	for (int y = 0; y < lh && 16 + len + (u32)lw * 3 <= SHOT_SIZE; y++)
		for (int x = 0; x < lw; x++) {
			u32 p = lfb[(addr_t)y * lw + x];
			s[16 + len++] = p >> 16;
			s[16 + len++] = p >> 8;
			s[16 + len++] = p;
		}
	for (int i = 0; i < 4; i++) s[12 + i] = len >> (8 * i);
	asm volatile("dsb sy");
	kprintf("pro1x: screenshot at %p, %u bytes\n", SHOT, len);
}

/* --- TLMM: the SoC's GPIOs, in three register tiles ------------------ */

#define TLMM_CTL 0x0
#define CTL_OE (1u << 9)
#define TLMM_IO 0x4
#define TLMM_INTR_CFG 0x8
#define TLMM_INTR_STATUS 0xc
#define NPINS 113

enum { WEST, SOUTH, EAST };
/* each GPIO's tile (Linux, drivers/pinctrl/qcom/pinctrl-sm6115.c) */
static const u8 tile[NPINS] = {
	WEST, WEST, WEST, WEST, WEST, WEST, WEST, WEST,
	EAST, EAST, EAST, EAST, WEST, WEST, WEST, WEST,
	WEST, WEST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, EAST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, EAST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, EAST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, EAST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, EAST, EAST, EAST, EAST, EAST, EAST, EAST,
	EAST, WEST, WEST, WEST, WEST, WEST, WEST, WEST,
	SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH,
	WEST, WEST, WEST, WEST, WEST, WEST, WEST, EAST,
	EAST, WEST, EAST, EAST, WEST, WEST, WEST, WEST,
	WEST, WEST, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH,
	SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH,
	SOUTH,
};

static addr_t gpio_reg(int pin, u32 off)
{
	return io_va(tlmm[tile[pin]]) + 0x1000u * pin + off;
}

static bool gpio_in(int pin)
{
	return rd32(gpio_reg(pin, TLMM_IO)) & 1;
}

/* open drain: low driven, high released (the pull-up) */
static void gpio_od(int pin, bool high)
{
	addr_t c = gpio_reg(pin, TLMM_CTL);
	u32 v = rd32(c);
	wr32(c, high ? v & ~CTL_OE : v | CTL_OE);
}

static bool gpio_ok(int pin)
{
	return pin >= 0 && pin < NPINS && tlmm[tile[pin]];
}

/* --- the GENI I2C engine, polled, its FIFOs (as Linux left it) ------- */

#define GENI_IF_DISABLE 0x064
#define GENI_FW_REVISION 0x068
#define GENI_DMA_MODE_EN 0x258
#define GENI_I2C_TX_LEN 0x26c
#define GENI_I2C_RX_LEN 0x270
#define GENI_M_CMD0 0x600
#define GENI_M_CMD_CTRL 0x604
#define GENI_M_IRQ_STATUS 0x610
#define GENI_M_IRQ_EN 0x614
#define GENI_M_IRQ_CLEAR 0x618
#define GENI_TX_FIFO 0x700
#define GENI_RX_FIFO 0x780
#define GENI_RX_FIFO_STATUS 0x804
#define M_CMD_DONE (1u << 0)
/* overrun, illegal command, failure, cancel, abort; GP_IRQ 0-5 (NACK,
 * bus protocol, arbitration lost among them) */
#define M_ERRORS (0x3eu | 0x3fu << 9)

static bool geni_wait(addr_t g, u8 *rx, int rxlen)
{
	u64 f = SYSREG_R(cntfrq_el0), t0 = SYSREG_R(cntvct_el0);
	int got = 0;
	for (;;) {
		u32 st = rd32(g + GENI_M_IRQ_STATUS);
		for (u32 w = rd32(g + GENI_RX_FIFO_STATUS) & 0x1ffffff; w; w--) {
			u32 d = rd32(g + GENI_RX_FIFO);
			for (int b = 0; b < 4 && got < rxlen; b++) rx[got++] = d >> (8 * b);
		}
		if (st & (M_CMD_DONE | M_ERRORS)) {
			for (u32 w = rd32(g + GENI_RX_FIFO_STATUS) & 0x1ffffff; w; w--) {
				u32 d = rd32(g + GENI_RX_FIFO);
				for (int b = 0; b < 4 && got < rxlen; b++) rx[got++] = d >> (8 * b);
			}
			wr32(g + GENI_M_IRQ_CLEAR, st);
			return !(st & M_ERRORS) && got == rxlen;
		}
		if ((SYSREG_R(cntvct_el0) - t0) * 1000 > 5 * f) {        /* 5 ms */
			wr32(g + GENI_M_CMD_CTRL, 1u << 2);               /* cancel */
			udelay(100);
			wr32(g + GENI_M_IRQ_CLEAR, 0xffffffffu);
			return false;
		}
	}
}

/* a write, then (repeated start) a read */
static bool geni_xfer(u8 addr, const u8 *tx, int txlen, u8 *rx, int rxlen)
{
	addr_t g = io_va(geni);
	if (txlen) {
		wr32(g + GENI_M_IRQ_CLEAR, 0xffffffffu);
		wr32(g + GENI_I2C_TX_LEN, txlen);
		/* opcode 1: write; the stop held back when a read follows */
		wr32(g + GENI_M_CMD0, 1u << 27 | (u32)addr << 9 | (rxlen ? 1u << 2 : 0));
		for (int i = 0; i < txlen; i += 4) {
			u32 w = 0;
			for (int b = 0; b < 4 && i + b < txlen; b++) w |= (u32)tx[i + b] << (8 * b);
			wr32(g + GENI_TX_FIFO, w);
		}
		if (!geni_wait(g, NULL, 0)) return false;
	}
	if (rxlen) {
		wr32(g + GENI_M_IRQ_CLEAR, 0xffffffffu);
		wr32(g + GENI_I2C_RX_LEN, rxlen);
		wr32(g + GENI_M_CMD0, 2u << 27 | (u32)addr << 9);       /* opcode 2: read */
		if (!geni_wait(g, rx, rxlen)) return false;
	}
	return true;
}

/* --- the keyboard: AW9523B, port 1 the columns, port 0 the rows ------ */

#define AW_IN_P0 0x00
#define AW_IN_P1 0x01
#define AW_OUT_P1 0x03
#define AW_CFG_P0 0x04                 /* 1: input */
#define AW_CFG_P1 0x05
#define AW_INT_P0 0x06                 /* 0: interrupt on */
#define AW_INT_P1 0x07
#define AW_ID 0x10
#define AW_RESET 0x7f

#define KEY_FN 0x1d0

static u8 kbd_addr;
static int kbd_int = -1;               /* its INT, a TLMM GPIO (low: a change) */
static u16 keycode[64];                /* Linux's codes, row * 8 + col */
static bool kbd_ok;
static u8 kdown[8], kraw[8];           /* per column: the rows down */
static u8 ksent[64 + 8];               /* the scan code a key went down with:
				        * the matrix's, then the GPIO keys' */
static bool kconfirm;
static int kerrors;
static u8 rep_sc;
static u32 rep_tick;
static bool fn;

static struct { int pin; u16 code; bool low; bool down, last; } gkey[8];
static int ngkeys;

static bool aw_wr(u8 reg, u8 v)
{
	u8 b[2] = { reg, v };
	return geni_xfer(kbd_addr, b, 2, NULL, 0);
}

static bool aw_rd(u8 reg, u8 *v)
{
	return geni_xfer(kbd_addr, &reg, 1, v, 1);
}

/* all columns low: any key pulls its row low and INT goes low */
static void kbd_idle(void)
{
	u8 d;
	aw_wr(AW_OUT_P1, 0);
	aw_wr(AW_CFG_P1, 0);
	aw_rd(AW_IN_P0, &d);                 /* reading the ports releases INT */
	aw_rd(AW_IN_P1, &d);
}

/* one column low at a time, the others pre-charged high and floating */
static bool kbd_scan(u8 st[8])
{
	if (!aw_wr(AW_OUT_P1, 0xff) || !aw_wr(AW_CFG_P1, 0)) return false;
	for (int col = 0; col < 8; col++) {
		u8 rows;
		if (!aw_wr(AW_OUT_P1, 0xff) || !aw_wr(AW_CFG_P1, (u8)~(1u << col)) ||
		    !aw_wr(AW_OUT_P1, (u8)~(1u << col)))
			return false;
		udelay(20);
		if (!aw_rd(AW_IN_P0, &rows)) return false;
		st[col] = ~rows;
	}
	return true;
}

/* another column has this row down and shares a second row with this
 * one: a ghost, the fourth corner of a rectangle */
static bool ghost(const u8 st[8], int col, int row)
{
	for (int k = 0; k < 8; k++)
		if (k != col && (st[k] & (1u << row)) && (st[col] & st[k] & ~(1u << row)))
			return true;
	return false;
}

/* Linux's key codes as KolibriOS's (PC set 1) scan codes; Fn (the keys
 * with the arrows on them) gives F1-F10, Page Up/Down, Home, End */
static u8 to_set1(u16 lc)
{
	if (fn) {
		if (lc >= 2 && lc <= 11) return 0x3b + lc - 2;      /* 1-0: F1-F10 */
		switch (lc) {
		case 103: return 0x49;  /* up: page up */
		case 108: return 0x51;  /* down: page down */
		case 105: return 0x47;  /* left: home */
		case 106: return 0x4f;  /* right: end */
		case 14: return 0x53;   /* backspace: delete */
		}
	}
	if (lc < 0x59) return lc;               /* the same below F12 */
	switch (lc) {
	case 97: return 0x1d;   /* right ctrl */
	case 100: return 0x38;  /* right alt */
	case 102: return 0x47;  /* home */
	case 103: return 0x48;  /* up */
	case 104: return 0x49;  /* page up */
	case 105: return 0x4b;  /* left */
	case 106: return 0x4d;  /* right */
	case 107: return 0x4f;  /* end */
	case 108: return 0x50;  /* down */
	case 109: return 0x51;  /* page down */
	case 110: return 0x52;  /* insert */
	case 111: return 0x53;  /* delete */
	case 125: case 126: case 172: return 0x5b;  /* meta, home page: the Windows key */
	}
	return 0;
}

static void key(int pos, bool pressed, u16 lc)
{
	u8 sc = pressed ? to_set1(lc) : ksent[pos];
	if (!sc) return;
	ksent[pos] = pressed ? sc : 0;
	input_key(sc, !pressed);
	if (pressed) {
		rep_sc = sc;
		rep_tick = ticks + 25;           /* 250 ms, then every 30 ms */
	} else if (sc == rep_sc) {
		rep_sc = 0;
	}
}

static void kbd_report(const u8 st[8])
{
	for (int col = 0; col < 8; col++)
		for (int row = 0; row < 8; row++) {
			bool pressed = st[col] & (1u << row);
			if (pressed == !!(kdown[col] & (1u << row))) continue;
			if (pressed && ghost(st, col, row)) continue;
			kdown[col] ^= 1u << row;
			if (keycode[row * 8 + col]) key(row * 8 + col, pressed, keycode[row * 8 + col]);
		}
}

static void kbd_init(void)
{
	if (!geni || !kbd_addr) {
		kprintf("pro1x: no keyboard in the device tree\n");
		return;
	}
	/* the first access hangs the bus if the engine's clocks are off */
	addr_t g = io_va(geni);
	u32 fw = rd32(g + GENI_FW_REVISION);
	if (((fw >> 8) & 0xff) != 3 || (rd32(g + GENI_IF_DISABLE) & 1)) {
		kprintf("pro1x: keyboard: the I2C engine runs protocol %u%s\n", (fw >> 8) & 0xff,
			(rd32(g + GENI_IF_DISABLE) & 1) ? ", FIFOs off" : "");
		return;
	}
	wr32(g + GENI_DMA_MODE_EN, rd32(g + GENI_DMA_MODE_EN) & ~1u);
	wr32(g + GENI_M_IRQ_EN, rd32(g + GENI_M_IRQ_EN) | M_CMD_DONE | M_ERRORS);
	u8 id = 0;
	if (!aw_rd(AW_ID, &id) || id != 0x23) {
		kprintf("pro1x: keyboard: no AW9523B at 0x%x (id 0x%x)\n", kbd_addr, id);
		return;
	}
	aw_wr(AW_RESET, 0);
	udelay(2000);
	if (!aw_wr(AW_CFG_P0, 0xff) || !aw_wr(AW_INT_P0, 0) || !aw_wr(AW_INT_P1, 0xff)) {
		kprintf("pro1x: keyboard: the expander does not take its setup\n");
		return;
	}
	kbd_idle();
	kbd_ok = true;
	kprintf("pro1x: keyboard: AW9523B at 0x%x, INT on GPIO %d, %d modifier keys\n",
		kbd_addr, kbd_int, ngkeys);
}

static void kbd_poll(void)
{
	/* the modifiers, debounced over two ticks */
	for (int i = 0; i < ngkeys; i++) {
		bool d = gpio_in(gkey[i].pin) != gkey[i].low;
		bool stable = d == gkey[i].last;
		gkey[i].last = d;
		if (!stable || d == gkey[i].down) continue;
		gkey[i].down = d;
		if (gkey[i].code == KEY_FN) fn = d;
		else key(64 + i, d, gkey[i].code);
	}
	if (rep_sc && (s32)(ticks - rep_tick) >= 0) {
		input_key(rep_sc, false);
		rep_tick = ticks + 3;
	}
	if (!kbd_ok) return;
	bool held = false;
	for (int c = 0; c < 8; c++) held |= kdown[c] != 0;
	bool changed;
	if (gpio_ok(kbd_int)) {
		changed = !gpio_in(kbd_int);
	} else {
		u8 rows = 0xff;
		changed = !(ticks & 1) && aw_rd(AW_IN_P0, &rows) && rows != 0xff;
	}
	/* a held key hides others on its lines from INT: polled meanwhile */
	if (!changed && !kconfirm && !(held && ticks % 3 == 0)) return;
	u8 st[8];
	bool ok = kbd_scan(st);
	kbd_idle();
	if (!ok) {
		if (++kerrors == 50) {
			kprintf("pro1x: keyboard: I2C errors, given up\n");
			kbd_ok = false;
		}
		return;
	}
	/* taken when two scans in a row agree */
	if (memcmp(st, kraw, 8)) {
		memcpy(kraw, st, 8);
		kconfirm = true;
		return;
	}
	kconfirm = false;
	kbd_report(st);
}

/* --- the touch screen: Goodix on bit-banged I2C ---------------------- */

static int ts_sda = -1, ts_scl = -1, ts_irq = -1;
static u32 ts_delay = 5;
static u8 ts_addr;
static bool ts_ok, ts_down;
static int ts_xmax, ts_ymax, ts_x, ts_y;

static void half(void) { udelay(ts_delay); }

static void scl_high(void)
{
	gpio_od(ts_scl, true);
	for (int i = 0; i < 1000 && !gpio_in(ts_scl); i++) udelay(1);  /* clock stretching */
}

static void i2c_start(void)
{
	gpio_od(ts_sda, true);
	scl_high();
	half();
	gpio_od(ts_sda, false);
	half();
	gpio_od(ts_scl, false);
	half();
}

static void i2c_stop(void)
{
	gpio_od(ts_sda, false);
	half();
	scl_high();
	half();
	gpio_od(ts_sda, true);
	half();
}

static bool i2c_wbyte(u8 b)
{
	for (int i = 7; i >= 0; i--) {
		gpio_od(ts_sda, (b >> i) & 1);
		half();
		scl_high();
		half();
		gpio_od(ts_scl, false);
	}
	gpio_od(ts_sda, true);
	half();
	scl_high();
	half();
	bool ack = !gpio_in(ts_sda);
	gpio_od(ts_scl, false);
	half();
	return ack;
}

static u8 i2c_rbyte(bool ack)
{
	u8 b = 0;
	gpio_od(ts_sda, true);
	for (int i = 0; i < 8; i++) {
		half();
		scl_high();
		half();
		b = b << 1 | gpio_in(ts_sda);
		gpio_od(ts_scl, false);
	}
	gpio_od(ts_sda, !ack);
	half();
	scl_high();
	half();
	gpio_od(ts_scl, false);
	gpio_od(ts_sda, true);
	half();
	return b;
}

/* Goodix registers: 16-bit addresses, big endian */
static bool gt_read(u16 reg, u8 *buf, int n)
{
	i2c_start();
	bool ok = i2c_wbyte(ts_addr << 1) && i2c_wbyte(reg >> 8) && i2c_wbyte(reg);
	if (ok) {
		i2c_start();
		ok = i2c_wbyte(ts_addr << 1 | 1);
		for (int i = 0; ok && i < n; i++) buf[i] = i2c_rbyte(i < n - 1);
	}
	i2c_stop();
	return ok;
}

static bool gt_write(u16 reg, u8 v)
{
	i2c_start();
	bool ok = i2c_wbyte(ts_addr << 1) && i2c_wbyte(reg >> 8) && i2c_wbyte(reg) && i2c_wbyte(v);
	i2c_stop();
	return ok;
}

#define GT_ID 0x8140
#define GT_CONFIG 0x8050               /* the GT9286's (GT1x generation) */
#define GT_COOR 0x814e                 /* status, then 8 bytes per contact */

static void ts_init(void)
{
	if (!gpio_ok(ts_sda) || !gpio_ok(ts_scl) || !ts_addr) {
		kprintf("pro1x: no touch screen in the device tree\n");
		return;
	}
	wr32(gpio_reg(ts_sda, TLMM_IO), 0);   /* driven: low */
	wr32(gpio_reg(ts_scl, TLMM_IO), 0);
	gpio_od(ts_sda, true);
	gpio_od(ts_scl, true);
	u8 id[4];
	if (!gt_read(GT_ID, id, 4)) {
		kprintf("pro1x: touch: no answer at 0x%x\n", ts_addr);
		return;
	}
	u8 cfg[5];
	if (gt_read(GT_CONFIG, cfg, 5)) {
		ts_xmax = cfg[1] | cfg[2] << 8;
		ts_ymax = cfg[3] | cfg[4] << 8;
	}
	if (ts_xmax <= 0 || ts_ymax <= 0 || ts_xmax > 8192 || ts_ymax > 8192) {
		ts_xmax = fb_w;
		ts_ymax = fb_h;
	}
	/* its interrupt pulses latched by the TLMM (either edge), not
	 * forwarded: the status polled */
	if (gpio_ok(ts_irq)) {
		addr_t c = gpio_reg(ts_irq, TLMM_INTR_CFG);
		wr32(c, (rd32(c) & ~0xfu) | 1u << 4 | 3u << 2 | 1u << 1);
		wr32(gpio_reg(ts_irq, TLMM_INTR_STATUS), 0);
	}
	ts_ok = true;
	kprintf("pro1x: touch: Goodix %c%c%c%c, %dx%d, INT on GPIO %d\n",
		id[0], id[1], id[2], id[3], ts_xmax, ts_ymax, ts_irq);
}

static void ts_poll(void)
{
	if (!ts_ok) return;
	bool latched = false;
	if (gpio_ok(ts_irq)) {
		latched = rd32(gpio_reg(ts_irq, TLMM_INTR_STATUS)) & 1;
		if (latched) wr32(gpio_reg(ts_irq, TLMM_INTR_STATUS), 0);
	}
	/* without a pulse: every 50 ms while touched, every 250 ms else */
	if (!latched && ticks % (ts_down ? 5 : 25)) return;
	u8 b[9];
	if (!gt_read(GT_COOR, b, sizeof(b)) || !(b[0] & 0x80)) return;
	gt_write(GT_COOR, 0);
	if (b[0] & 0x0f) {
		int tx = b[2] | b[3] << 8, ty = b[4] | b[5] << 8, xm = ts_xmax, ym = ts_ymax;
		if (xm > ym) {                   /* the long side first: swapped */
			int t = tx; tx = ty; ty = t;
			t = xm; xm = ym; ym = t;
		}
		/* the panel's (px, py) is the landscape (py, fb_w - 1 - px) */
		int px = tx * fb_w / xm, py = ty * fb_h / ym;
		ts_x = MAX(0, MIN(py / 2, lw - 1));
		ts_y = MAX(0, MIN((fb_w - 1 - px) / 2, lh - 1));
		input_mouse_abs(ts_x, ts_y, 1);
		ts_down = true;
	} else if (ts_down) {
		input_mouse_abs(ts_x, ts_y, 0);
		ts_down = false;
	}
}

/* --- the platform ---------------------------------------------------- */

static void p_early_init(void)
{
	kprintf("pro1x: kernel at %p, device tree at %p\n", boot_args[1], boot_args[0]);
	for (int i = 0; i < platform_pro1x.nbank; i++)
		kprintf("pro1x: RAM %p-%p\n", platform_pro1x.bank[i].base,
			platform_pro1x.bank[i].base + platform_pro1x.bank[i].size);
	if (wdt)
		kprintf("pro1x: watchdog %s, bites after %u s unfed\n",
			wdt_was_running ? "left running" : "started", rd32(io_va(wdt) + WDT_BITE) / 32768);
	if (display_takeover() && fb_pa) {
		/* proof of life: a blue stripe at the left (the panel's top) */
		for (int y = 0; y < 48; y++)
			for (int x = 0; x < fb_w; x++)
				wr32(fb_pa + (addr_t)y * fb_stride + 4 * x, 0xff1e64c8u);
		asm volatile("dsb sy");
	}
}

static void modem_kick(void)
{
	asm volatile("dsb sy" ::: "memory");
	wr32(io_va(apcs_pa) + 8, 1u << apcs_bit);
}

/* --- the call's sound (kernel/modem/callaudio.c) --------------------- */

static void adsp_kick(void)
{
	asm volatile("dsb sy" ::: "memory");
	wr32(io_va(apcs_pa) + 8, 1u << adsp_bit);
}

static bool amp_write(u8 addr, u8 reg, u8 val)
{
	u8 b[2] = { reg, val };
	return geni && geni_xfer(addr, b, 2, NULL, 0);
}

static bool amp_read(u8 addr, u8 reg, u8 *val)
{
	return geni && geni_xfer(addr, &reg, 1, val, 1);
}

/* push-pull: the value, then the output on (as Linux's pinctrl left
 * the amplifiers' enable lines: GPIO function, an output) */
static void gpio_out(int pin, bool high)
{
	if (!gpio_ok(pin)) return;
	addr_t io = gpio_reg(pin, TLMM_IO), c = gpio_reg(pin, TLMM_CTL);
	wr32(io, high ? 2u : 0u);
	wr32(c, rd32(c) | CTL_OE);
}

/* the ADSP's GLINK edge (SMEM host 2), its APR channel taken over from
 * Linux in callaudio.c */
static void callaudio_start(void)
{
	static struct smem adsp_smem;
	struct callaudio_hw h = {
		.i2c_write = amp_write, .i2c_read = amp_read, .gpio_set = gpio_out, .delay_us = udelay,
		.prepared = callaudio,
	};
	if (callaudio && adsp_bit < 32 && smem_init(&adsp_smem, (volatile u8 *)smem_pa, smem_size, SMEM_HOST_APPS, 2) == 0) {
		struct glink *g = kmalloc_aligned(sizeof(*g), 64);
		if (glink_init(g, &adsp_smem, adsp_kick) == 0) {
			h.adsp = g;
			kprintf("pro1x: ADSP edge: tx FIFO %u bytes, rx FIFO %u bytes, IPC bit %u\n",
				g->tx_len, g->rx_len, adsp_bit);
		} else {
			kprintf("pro1x: ADSP edge: no GLINK FIFOs in SMEM\n");
		}
	}
	kcallaudio_start(&h);
}

static void modem_init(void)
{
	if (efs_pa) {
		/* written with the caches off (kexec): what the caches hold is stale */
		for (addr_t p = efs_pa & ~63ul; p < efs_pa + efs_size; p += 64)
			asm volatile("dc ivac, %0" :: "r"(p) : "memory");
		asm volatile("dsb sy");
		char magic[9];
		memcpy(magic, (const void *)efs_pa, 8);
		magic[8] = 0;
		for (int i = 0; i < 8; i++)
			if (magic[i] < 32 || magic[i] > 126) magic[i] = '.';
		kprintf("pro1x: initrd at %p, %u KiB (%s)\n", efs_pa, efs_size >> 10, magic);
	}
	if (fakemodem) {
		kmodem_fake();
		return;
	}
	if (nomodem || !smem_pa || !apcs_pa) {
		kprintf("pro1x: no modem%s\n", nomodem ? " (kolibri.nomodem)" : " in the device tree");
		return;
	}
	struct modem_hw hw = {
		.smem = (volatile u8 *)smem_pa, .smem_size = smem_size, .kick = modem_kick,
		.rmtfs_mem = (volatile u8 *)rmtfs_pa, .rmtfs_pa = rmtfs_pa, .rmtfs_size = rmtfs_size,
		.efs = (u8 *)efs_pa, .efs_size = efs_size,
	};
	kprintf("pro1x: modem: SMEM %p (%u KiB), rmtfs %p, IPC %p bit %u, EFS images %p (%u KiB)\n",
		smem_pa, smem_size >> 10, rmtfs_pa, apcs_pa, apcs_bit, efs_pa, efs_size >> 10);
	kmodem_start(&hw);
	callaudio_start();
}

static void p_init(void)
{
	gic3_init(gicd, gicr, NULL);
	if (lifetime) kprintf("pro1x: lifetime %u s\n", lifetime);
	modem_init();
	if (noinput) {
		kprintf("pro1x: no input (kolibri.noinput)\n");
		return;
	}
	kbd_init();
	ts_init();
}

static void p_irq(void)
{
	gic3_irq();
}

static void p_tick(void)
{
	if (wdt_feeding && ticks % 100 == 0) {
		if (lifetime && ticks >= lifetime * 100) {
			shot();
			kprintf("pro1x: lifetime over: the watchdog resets the phone\n");
			wdt_feeding = false;
		} else if (wdt) {
			wr32(io_va(wdt) + WDT_RST, 1);
		}
	}
	if (!noinput) {
		kbd_poll();
		ts_poll();
	}
}

static void p_panic(void)
{
	p_display_flush(0, lh);
	shot();
}

static u32 p_rtc_time(void)
{
	return 0;
}

struct platform platform_pro1x = {
	.name = "F(x)tec Pro1X (SM6115)",
	.io = io,
	.early_init = p_early_init,
	.init = p_init,
	.putc = p_putc,
	.irq = p_irq,
	.display_init = p_display_init,
	.display_flush = p_display_flush,
	.rtc_time = p_rtc_time,
	.tick = p_tick,
	.panic_hook = p_panic,
	.cached_data = true,
};

/* --- the device tree ------------------------------------------------- */

static u32 cell(const void *p, int i)
{
	return (u32)fdt_cells((const u8 *)p + 4 * i, 1);
}

static void reg_io(const void *fdt, int node, int ac, int sc, int index, addr_t *pa, u32 attr)
{
	u64 a, s;
	if (node < 0 || !fdt_reg(fdt, node, ac, sc, index, &a, &s)) return;
	*pa = a;
	add_io(a, s, attr);
}

static bool word(const char *s, int len, const char *w, u32 *val)
{
	int n = strlen(w);
	for (int i = 0; i + n <= len; i++) {
		if ((i && s[i - 1] != ' ') || strncmp(s + i, w, n)) continue;
		if (val) {
			u32 v = 0;
			for (int j = i + n; j < len && s[j] >= '0' && s[j] <= '9'; j++) v = v * 10 + s[j] - '0';
			*val = v;
		}
		return true;
	}
	return false;
}

/* [lo, hi) out of the RAM banks (2 MiB aligned) */
static void bank_cut(u64 lo, u64 hi)
{
	struct platform *pl = &platform_pro1x;
	for (int i = 0; i < pl->nbank; i++) {
		u64 b = pl->bank[i].base, e = b + pl->bank[i].size;
		if (hi <= b || lo >= e) continue;
		if (lo > b && hi < e && pl->nbank < MAX_BANKS) {   /* split */
			for (int j = pl->nbank; j > i + 1; j--) pl->bank[j] = pl->bank[j - 1];
			pl->nbank++;
			pl->bank[i + 1].base = hi;
			pl->bank[i + 1].size = e - hi;
			pl->bank[i].size = lo - b;
			i++;
		} else if (lo <= b && hi >= e) {
			for (int j = i; j + 1 < pl->nbank; j++) pl->bank[j] = pl->bank[j + 1];
			pl->nbank--;
			i--;
		} else if (lo <= b) {
			pl->bank[i].base = hi;
			pl->bank[i].size = e - hi;
		} else {
			pl->bank[i].size = lo - b;
		}
	}
}

bool pro1x_probe(const void *fdt)
{
	cons_init();
	int ac, sc, len;
	const void *p;

	/* the RAM: what /memory has without the reserved memory (the
	 * firmware's: not even to be mapped), from 1 GiB on */
	int n = fdt_usable_ram(fdt, platform_pro1x.bank, MAX_BANKS, 2 * MB), m = 0;
	for (int i = 0; i < n; i++)
		if (platform_pro1x.bank[i].base >= (1ul << 30)) platform_pro1x.bank[m++] = platform_pro1x.bank[i];
	platform_pro1x.nbank = m;
	platform_pro1x.ram_base = boot_args[1] & ~(2 * MB - 1);

	/* the framebuffer, the splash memory around it (the console, the
	 * screenshot) not cached */
	int f = fdt_find_compatible(fdt, "simple-framebuffer", -1, &ac, &sc);
	u64 a, s;
	if (f >= 0 && fdt_reg(fdt, f, ac, sc, 0, &a, &s)) {
		fb_pa = a;
		fb_w = fdt_u32(fdt, f, "width", 0);
		fb_h = fdt_u32(fdt, f, "height", 0);
		fb_stride = fdt_u32(fdt, f, "stride", fb_w * 4);
		if (fb_w <= 0 || fb_h <= 0 || fb_w > 2160 || fb_h > 4320 || (fb_w | fb_h) & 1)
			fb_pa = 0;
	}
	add_io(SPLASH, 16 * MB, MAP_UNCACHED);
	if (fb_pa && (fb_pa < SPLASH || fb_pa + (u64)fb_h * fb_stride > SPLASH + 16 * MB))
		add_io(fb_pa, (u64)fb_h * fb_stride, MAP_UNCACHED);

	int g = fdt_find_compatible(fdt, "arm,gic-v3", -1, &ac, &sc);
	reg_io(fdt, g, ac, sc, 0, &gicd, MAP_DEVICE);
	reg_io(fdt, g, ac, sc, 1, &gicr, MAP_DEVICE);
	reg_io(fdt, fdt_find_compatible(fdt, "qcom,kpss-wdt", -1, &ac, &sc), ac, sc, 0, &wdt, MAP_DEVICE);
	wdt_start();

	/* the display controller, the SMMU in front of it */
	reg_io(fdt, fdt_find_compatible(fdt, "qcom,sm6115-dpu", -1, &ac, &sc), ac, sc, 0, &mdp, MAP_DEVICE);
	int md = fdt_find_compatible(fdt, "qcom,sm6115-mdss", -1, &ac, &sc);
	p = md >= 0 ? fdt_prop(fdt, md, "iommus", &len) : NULL;
	if (p && len >= 12) {
		int sm = fdt_find_phandle(fdt, cell(p, 0));
		int sac = 2, ssc = 2;
		if (sm >= 0 && fdt_find_compatible(fdt, "arm,mmu-500", sm - 1, &sac, &ssc) == sm)
			reg_io(fdt, sm, sac, ssc, 0, &smmu, MAP_DEVICE);
		for (int i = 0; i + 3 <= len / 4 && nstreams < (int)ARRAY_SIZE(streams); i += 3) {
			streams[nstreams][0] = cell(p, i + 1);
			streams[nstreams][1] = cell(p, i + 2);
			nstreams++;
		}
	}

	/* the GPIOs */
	int t = fdt_find_compatible(fdt, "qcom,sm6115-tlmm", -1, &ac, &sc);
	u32 tlmm_ph = t >= 0 ? fdt_u32(fdt, t, "phandle", 0) : 0;
	for (int i = 0; i < 3; i++) reg_io(fdt, t, ac, sc, i, &tlmm[i], MAP_DEVICE);

	/* the keyboard: its I2C engine (the expander's parent), its map */
	for (int i2c = -1; (i2c = fdt_find_compatible(fdt, "qcom,geni-i2c", i2c, &ac, &sc)) >= 0; ) {
		int k = fdt_child_compatible(fdt, i2c, "fxtec,pro1x-aw9523-keypad");
		if (k < 0) continue;
		reg_io(fdt, i2c, ac, sc, 0, &geni, MAP_DEVICE);
		kbd_addr = fdt_u32(fdt, k, "reg", 0);
		p = fdt_prop(fdt, k, "interrupts", &len);
		if (p && len >= 4 && tlmm_ph && fdt_u32(fdt, k, "interrupt-parent", 0) == tlmm_ph)
			kbd_int = cell(p, 0);
		p = fdt_prop(fdt, k, "linux,keymap", &len);
		for (int j = 0; p && j < len / 4; j++) {
			u32 e = cell(p, j);             /* MATRIX_KEY(row, col, code) */
			if ((e >> 24) < 8 && ((e >> 16) & 0xff) < 8)
				keycode[(e >> 24) * 8 + ((e >> 16) & 0xff)] = e & 0xffff;
		}
		break;
	}
	/* the modifier keys: gpio-keys on TLMM pins */
	for (int gk = -1; (gk = fdt_find_compatible(fdt, "gpio-keys", gk, &ac, &sc)) >= 0; ) {
		for (int c = fdt_next_child(fdt, gk, -1); c >= 0 && ngkeys < (int)ARRAY_SIZE(gkey);
		     c = fdt_next_child(fdt, gk, c)) {
			p = fdt_prop(fdt, c, "gpios", &len);
			if (!p || len < 12 || cell(p, 0) != tlmm_ph || fdt_u32(fdt, c, "linux,input-type", 1) != 1)
				continue;
			u32 code = fdt_u32(fdt, c, "linux,code", 0);
			if (code != KEY_FN && !to_set1(code)) continue;
			gkey[ngkeys].pin = cell(p, 1);
			gkey[ngkeys].code = code;
			gkey[ngkeys].low = cell(p, 2) & 1;
			if (gpio_ok(gkey[ngkeys].pin)) ngkeys++;
		}
	}
	/* the touch screen: its GPIO I2C bus */
	for (int ig = -1; (ig = fdt_find_compatible(fdt, "i2c-gpio", ig, &ac, &sc)) >= 0; ) {
		int ts = fdt_child_compatible(fdt, ig, "goodix,gt9286");
		if (ts < 0) continue;
		p = fdt_prop(fdt, ig, "sda-gpios", &len);
		if (p && len >= 8 && cell(p, 0) == tlmm_ph) ts_sda = cell(p, 1);
		p = fdt_prop(fdt, ig, "scl-gpios", &len);
		if (p && len >= 8 && cell(p, 0) == tlmm_ph) ts_scl = cell(p, 1);
		ts_delay = fdt_u32(fdt, ig, "i2c-gpio,delay-us", 5);
		ts_addr = fdt_u32(fdt, ts, "reg", 0);
		p = fdt_prop(fdt, ts, "irq-gpios", &len);
		if (p && len >= 8 && cell(p, 0) == tlmm_ph) ts_irq = cell(p, 1);
		break;
	}

	/* the modem: SMEM and the rmtfs buffer (reserved memory, not
	 * cached, never read speculatively), the IPC register of the MPSS's
	 * GLINK edge */
	int sn = fdt_find_compatible(fdt, "qcom,smem", -1, &ac, &sc);
	if (sn >= 0 && fdt_reg(fdt, sn, ac, sc, 0, &a, &s)) {
		smem_pa = a;
		smem_size = s;
		add_io(a, s, MAP_UNCACHED);
	}
	int rn = fdt_find_compatible(fdt, "qcom,rmtfs-mem", -1, &ac, &sc);
	if (rn >= 0 && fdt_reg(fdt, rn, ac, sc, 0, &a, &s)) {
		rmtfs_pa = a;
		rmtfs_size = s;
		add_io(a, s, MAP_UNCACHED);
	}
	int ap = fdt_find_compatible(fdt, "qcom,sm6115-adsp-pas", -1, &ac, &sc);
	int aedge = ap >= 0 ? fdt_next_child(fdt, ap, -1) : -1;
	while (aedge >= 0 && strcmp(fdt_node_name(fdt, aedge), "glink-edge"))
		aedge = fdt_next_child(fdt, ap, aedge);
	p = aedge >= 0 ? fdt_prop(fdt, aedge, "mboxes", &len) : NULL;
	if (p && len >= 8) adsp_bit = cell(p, 1);
	int mp = fdt_find_compatible(fdt, "qcom,sm6115-mpss-pas", -1, &ac, &sc);
	int edge = mp >= 0 ? fdt_next_child(fdt, mp, -1) : -1;
	while (edge >= 0 && strcmp(fdt_node_name(fdt, edge), "glink-edge"))
		edge = fdt_next_child(fdt, mp, edge);
	p = edge >= 0 ? fdt_prop(fdt, edge, "mboxes", &len) : NULL;
	if (p && len >= 8) {
		int mb = fdt_find_phandle(fdt, cell(p, 0));
		int mac = 2, msc = 2;
		apcs_bit = cell(p, 1);
		if (mb >= 0 && fdt_find_compatible(fdt, "qcom,sm6115-apcs-hmss-global", mb - 1, &mac, &msc) == mb)
			reg_io(fdt, mb, mac, msc, 0, &apcs_pa, MAP_DEVICE);
	}

	/* the command line: kolibri.lifetime=N, kolibri.noinput */
	int ch = fdt_find_name(fdt, "chosen", &ac, &sc);
	const char *args = ch >= 0 ? fdt_prop(fdt, ch, "bootargs", &len) : NULL;
	if (args) {
		word(args, len, "kolibri.lifetime=", &lifetime);
		noinput = word(args, len, "kolibri.noinput", NULL);
		nomodem = word(args, len, "kolibri.nomodem", NULL);
		fakemodem = word(args, len, "kolibri.fakemodem", NULL);
		callaudio = word(args, len, "kolibri.callaudio", NULL);
	}
	/* the initrd (pro1x-kexec.sh -m: the modem's EFS images): kept out of
	 * the programs' memory, mapped as RAM */
	if (ch >= 0) {
		const void *st = fdt_prop(fdt, ch, "linux,initrd-start", &len);
		int len2;
		const void *en = fdt_prop(fdt, ch, "linux,initrd-end", &len2);
		if (st && en && (len == 4 || len == 8) && (len2 == 4 || len2 == 8)) {
			u64 b = fdt_cells(st, len / 4), e = fdt_cells(en, len2 / 4);
			if (e > b && e - b < 64 * MB) {
				efs_pa = b;
				efs_size = e - b;
				bank_cut(b & ~(2 * MB - 1), ALIGN_UP(e, 2 * MB));
				add_io(b, e - b, MAP_NORMAL);
			}
		}
	}
	return true;
}
