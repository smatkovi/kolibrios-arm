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
 *  - the APSS watchdog, fed every third of its bark time (10 s with
 *    pro1x-kexec.sh's 32 s); kolibri.lifetime=N on the command line
 *    stops that after N s, so that an unattended test ends in the boot
 *    menu again;
 *  - the keyboard: an 8x8 matrix behind an AW9523B GPIO expander on a GENI
 *    I2C engine (polled, its FIFOs), the modifier keys as TLMM GPIOs;
 *  - the touch screen, a Goodix GT9286 on bit-banged GPIO I2C, as the mouse;
 *  - their interrupts (the expander's INT, the modifier keys, the touch
 *    screen's INT) through the TLMM's summary interrupt, so that an idle
 *    kernel sleeps until a key (tickless idle, power.h); polled while keys
 *    are held, and every 50 ms (250 ms the touch screen) without it.
 *
 *  - the power key (the PMIC's PON over SPMI): the standby, the display
 *    off and the SoC asleep, ended by the power key or any key.
 *
 *  - the UFS (kernel/storage/ufs.c) on the link Linux left up, its SMMU
 *    context's translation off as the display's: the partitions read
 *    only, the container on userdata (docs/storage.md); kolibri.storage=off
 *    leaves it alone.
 *
 * kolibri.noinput leaves the keyboard and the touch screen alone,
 * kolibri.nopowerkey the power key. The clock (the PMIC's RTC, over
 * SPMI) is not driven: the time starts at 0. */
#include "kernel.h"
#include "fdt.h"
#include "../modem/phone.h"
#include "../modem/glink.h"
#include "../modem/callaudio.h"
#include "../modem/rpm.h"
#include "../modem/rpmsleep.h"
#include "../modem/pmic.h"
#include "../modem/wlfw.h"
#include "net.h"
#include "rng.h"
#include "qcom_prng.h"
#include "power.h"
#include "powerdt.h"
#include "blit.h"
#include "../storage/storage.h"
#include "../storage/ufs.h"
#include "dsi_ulps.h"

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

static struct mem_region io[32];
static addr_t gicd, gicr, wdt, mdp, smmu, geni, tlmm[3];
static u32 streams[8][2];              /* the display's SMMU stream ids, masks */
static int nstreams;
static addr_t fb_pa;
static int fb_w, fb_h, fb_stride;      /* the panel, portrait; stride in bytes */
static u32 lifetime;                   /* s, 0: forever */
static u32 powertest;                  /* kolibri.powertest=N: min */
static bool rpmsleep;                  /* kolibri.rpmsleep: the sleep set, the MPM */
static bool displayon, battery_arg;    /* kolibri.displayon, kolibri.battery */
static bool dsiulps;                    /* kolibri.dsiulps: the DSI lanes to ULPS with the display off */
/* the SPMI arbiter's regions (core, chnls, obsrvr, cnfg, intr), its EE,
 * its interrupt (an INTID) and that interrupt's MPM pin; the battery's
 * gauge and ADC channel (the device tree's fxtec,pro1x-adc-battery); the
 * DSI controller */
static addr_t spmi_pa[5], dsi_pa;
static u32 spmi_size[5], spmi_ee, spmi_intid, spmi_mpm_pin = 0xffffffffu;
static struct battery bat;
static bool bat_found;
static u32 tlmm_irq, modem_irq;        /* the TLMM's summary interrupt, the modem's GLINK edge's */
static u32 reserved[8][2];             /* the TLMM's gpio-reserved-ranges: not ours to touch */
static int nreserved;
static bool noinput;
static bool standby;                   /* the display off, the SoC asleep: a key ends it */
/* the modem (docs/pro1x-modem.md): SMEM, the rmtfs buffer, the APCS IPC
 * register and the bit that kicks its GLINK edge, the EFS images (initrd) */
static addr_t smem_pa, rmtfs_pa, apcs_pa, efs_pa;
static u32 smem_size, rmtfs_size, apcs_bit, efs_size;
/* the ADSP's GLINK edge: its IPC bit (the call's sound, callaudio.c) */
static u32 adsp_bit = 0xffffffffu;
static bool callaudio;                 /* kolibri.callaudio: Linux left the voice path running */
static bool nomodem, fakemodem, adopt_ipcrtr;
static bool wifi;                       /* kolibri.wifi: the WLAN's rails, the WLFW service, the driver */
static bool wifi_irq;                   /* kolibri.wifi=irq: the copy engines by interrupt */
static addr_t wifi_pa;                  /* the WCN3990's membase */
static u32 wifi_size, wifi_intid[12], wifi_nirq, wifi_streams[2][2];
static int wifi_nstreams;
static addr_t prng_pa, gcc_pa, rpmram_pa;
static addr_t ufs_pa;                   /* the UFS host controller (qcom,ufshc), its SMMU stream */
static u32 ufs_streams[2][2];
static int ufs_nstreams;
static u32 rpmram_size, rpm_bit = 32;

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
static u32 wdt_every = 100, wdt_next;  /* ticks: the feeding interval, the next one */

/* first thing (the probe, MMU off): kept running, or started */
static void wdt_start(void)
{
	/* none (QEMU): nothing to feed, a look every 10 s as on the phone
	 * (the lifetime, the power test's battery samples) */
	if (!wdt) {
		wdt_every = 1000;
		return;
	}
	addr_t w = io_va(wdt);
	wdt_was_running = rd32(w + WDT_EN) & 1;
	wr32(w + WDT_RST, 1);
	if (!wdt_was_running) {
		wr32(w + WDT_BARK, WDT_MAX);
		wr32(w + WDT_BITE, WDT_MAX);
		wr32(w + WDT_EN, 1);
	}
	/* fed every third of the bark time (32768 Hz counts), 1-10 s */
	u32 bark = rd32(w + WDT_BARK) & WDT_MAX;
	wdt_every = MAX(100u, MIN(1000u, bark / 3 * 100 / 32768));
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
static void smmu_bypass_streams(u32 (*streams)[2], int nstreams)
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

static void smmu_bypass(void)
{
	smmu_bypass_streams(streams, nstreams);
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
 * of the screen is a pair of the panel's columns (blit.h's rot2x: C or
 * NEON), the alpha opaque. */
static bool display_off;               /* the power test's standby: nothing to scan out */

static bool p_display_flush(int y0, int y1)
{
	if (display_off) return true;
	y1 = MIN(y1, lh);
	y0 = MAX(y0, 0);
	if (y0 >= y1) return true;
	blit_begin();
	blit.rot2x((void *)fb_pa, fb_stride, lfb, lw, lw, y0, y1, fb_w);
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
static bool input_irq;                 /* the TLMM interrupts are on (tlmm_irq_init) */
/* the next polls: when, and whether at all (else: at an interrupt) */
static u32 kbd_due, ts_due;
static bool kbd_sched = true, ts_sched = true;
#define KBD_IDLE_POLL 5                /* without the interrupt: 50 ms */
#define TS_IDLE_POLL 25

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

static bool standby_key(int pos, bool pressed);

static void key(int pos, bool pressed, u16 lc)
{
	if (standby_key(pos, pressed)) return;
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

/* ticks from now until t */
static u32 until(u32 t)
{
	s32 d = (s32)(t - ticks);
	return d > 0 ? (u32)d : 0;
}

static void kbd_scan_now(void);

/* the next poll: while something is changing or held, by the tick */
static void kbd_plan(bool held)
{
	u32 n = input_irq || (!kbd_ok && !ngkeys) ? 0xffffffffu : KBD_IDLE_POLL;
	for (int i = 0; i < ngkeys; i++)
		if (gkey[i].last != gkey[i].down) n = 1;       /* not stable yet */
	if (kconfirm) n = 1;
	if (held) n = MIN(n, 3u);
	if (rep_sc) n = MIN(n, MAX(until(rep_tick), 1u));
	kbd_sched = n != 0xffffffffu;
	kbd_due = ticks + n;
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
		if (gkey[i].code == KEY_FN) {
			if (!standby_key(64 + i, d)) fn = d;
		} else {
			key(64 + i, d, gkey[i].code);
		}
	}
	if (rep_sc && (s32)(ticks - rep_tick) >= 0) {
		input_key(rep_sc, false);
		rep_tick = ticks + 3;
	}
	bool held = false;
	for (int c = 0; c < 8; c++) held |= kdown[c] != 0;
	if (!kbd_ok) {
		kbd_plan(false);
		return;
	}
	bool changed;
	if (gpio_ok(kbd_int)) {
		changed = !gpio_in(kbd_int);
	} else {
		u8 rows = 0xff;
		changed = aw_rd(AW_IN_P0, &rows) && rows != 0xff;
	}
	/* a held key hides others on its lines from INT: polled meanwhile */
	if (!changed && !kconfirm && !held) {
		kbd_plan(false);
		return;
	}
	kbd_scan_now();
}

static void kbd_scan_now(void)
{
	u8 st[8];
	bool ok = kbd_scan(st);
	kbd_idle();
	bool held = false;
	for (int c = 0; c < 8; c++) held |= kdown[c] != 0;
	if (!ok) {
		if (++kerrors == 50) {
			kprintf("pro1x: keyboard: I2C errors, given up\n");
			kbd_ok = false;
		}
		kbd_plan(held);
		return;
	}
	/* taken when two scans in a row agree */
	if (memcmp(st, kraw, 8)) {
		memcpy(kraw, st, 8);
		kconfirm = true;
		kbd_plan(held);
		return;
	}
	kconfirm = false;
	kbd_report(st);
	held = false;
	for (int c = 0; c < 8; c++) held |= kdown[c] != 0;
	kbd_plan(held);
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
	/* without a pulse: every 50 ms while touched, every 250 ms else
	 * (with the interrupt: only while touched) */
	if (!latched && (!ts_sched || (s32)(ts_due - ticks) > 0)) return;
	ts_sched = ts_down || !input_irq;
	ts_due = ticks + (ts_down ? 5 : TS_IDLE_POLL);
	u8 b[9];
	if (!gt_read(GT_COOR, b, sizeof(b)) || !(b[0] & 0x80)) return;
	gt_write(GT_COOR, 0);
	if (standby && !ts_down) {             /* the display off: touches read, not passed on */
		ts_sched = !input_irq;
		return;
	}
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
		ts_sched = true;
		ts_due = ticks + 5;
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
	/* no edge, no voice session: not the speaker switch alone either */
	if (callaudio && !h.adsp) {
		kprintf("pro1x: kolibri.callaudio without the ADSP's edge: calls without sound\n");
		h.prepared = false;
	}
	kcallaudio_start(&h);
}

/* WLFW says the firmware is ready (stage 2): the driver (stages 4-6,
 * kernel/net/wifi_dev.c) on the register window, with the stream's SMMU
 * translation off as the display's, the network from the initrd */
bool wifi_dev_start(addr_t membase_va, u32 dma_mb, bool use_irqs, const char *conf, u32 conf_len);
const char *wifi_dev_conf(const u8 *b, u32 size, u32 *len);
void wifi_dev_irq(void);
void wifi_dev_report(void);

static void wifi_ready(void)
{
	if (!wifi_pa) {
		kprintf("pro1x: no qcom,wcn3990-wifi in the device tree\n");
		return;
	}
	u32 cl = 0;
	const char *conf = efs_pa ? wifi_dev_conf((const u8 *)efs_pa, efs_size, &cl) : NULL;
	if (!conf) {
		kprintf("pro1x: Wi-Fi: no network in the initrd (pro1x-kexec.sh -w FILE)\n");
		return;
	}
	kprintf("pro1x: Wi-Fi at %p (%u KiB), %u interrupts from INTID %u, stream 0x%x\n", wifi_pa, wifi_size >> 10,
		wifi_nirq, wifi_nirq ? wifi_intid[0] : 0, wifi_nstreams ? wifi_streams[0][0] : 0);
	if (smmu && wifi_nstreams) smmu_bypass_streams(wifi_streams, wifi_nstreams);
	if (!wifi_dev_start(io_va(wifi_pa), 4, wifi_irq && wifi_nirq == 12, conf, cl)) return;
	if (wifi_irq && wifi_nirq == 12)
		for (u32 i = 0; i < wifi_nirq; i++) gic3_enable_spi_edge(wifi_intid[i], false);
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
		if (wifi) kmodem_fake_wlfw();
		kmodem_fake();
		if (wifi) wlfw_on_ready(wifi_ready);
		return;
	}
	if (nomodem || !smem_pa || !apcs_pa) {
		kprintf("pro1x: no modem%s\n", nomodem ? " (kolibri.nomodem)" : " in the device tree");
		return;
	}
	struct modem_hw hw = {
		.smem = (volatile u8 *)smem_pa, .smem_size = smem_size, .kick = modem_kick,
		.rmtfs_mem = (volatile u8 *)rmtfs_pa, .rmtfs_pa = rmtfs_pa, .rmtfs_size = rmtfs_size,
		.efs = (u8 *)efs_pa, .efs_size = efs_size, .wlfw = wifi,
		.adopt_ipcrtr = adopt_ipcrtr,
	};
	kprintf("pro1x: modem: SMEM %p (%u KiB), rmtfs %p, IPC %p bit %u, EFS images %p (%u KiB)\n",
		smem_pa, smem_size >> 10, rmtfs_pa, apcs_pa, apcs_bit, efs_pa, efs_size >> 10);
	kmodem_start(&hw);
	if (wifi) wlfw_on_ready(wifi_ready);
	callaudio_start();
	/* its GLINK edge's interrupt: what it sends wakes the kernel */
	if (modem_irq && modem_present()) {
		gic3_enable_spi_edge(modem_irq, true);
		kprintf("pro1x: modem: its interrupt INTID %u\n", modem_irq);
	}
}

/* the PRNG (qcom,prng-ee: TrustZone set it up), read as Linux's qcom-rng
 * reads it, its AHB clock voted on in the GCC around each read
 * (qcom_prng.c); a source of the kernel's generator (rng.c) */
static void prng_wr32(addr_t a, u32 v) { wr32(a, v); }
static struct qprng prng = { .rd = rd32, .wr = prng_wr32, .udelay = udelay };

static u32 prng_read(void *out, u32 len)
{
	u32 z = prng.zeros, t = prng.timeouts, c = prng.clock_stuck, n = qprng_read(&prng, out, len);
	static u32 said;
	if ((prng.zeros != z || prng.timeouts != t || prng.clock_stuck != c) && (said++ < 5 || said % 1000 == 0))
		kprintf("pro1x: the PRNG: %s (%u words so far, %u zero words, %u timeouts, the clock stuck %u times)\n",
			prng.zeros != z ? "a zero word" : prng.timeouts != t ? "no word for 10 ms" : "its clock does not come on",
			prng.words, prng.zeros, prng.timeouts, prng.clock_stuck);
	return n;
}

static void prng_init(void)
{
	if (!prng_pa || !gcc_pa) return;
	prng.base = io_va(prng_pa);
	prng.gcc = io_va(gcc_pa);
	rng_register("the SoC's PRNG", prng_read, RNG_FULL);
}

/* the RPM: kicked through the IPC register, waited for by the counter */
static void rpm_kick(void)
{
	wr32(io_va(apcs_pa) + 8, 1u << rpm_bit);
}

static void delay_us(u32 us)
{
	u64 end = SYSREG_R(cntvct_el0) + SYSREG_R(cntfrq_el0) / 1000000 * us;
	while (SYSREG_R(cntvct_el0) < end)
		;
}

/* kolibri.wifi: the rails of the WLAN part (the device tree's wifi node:
 * vdd-0.8-cx-mx L8, vdd-1.8-xo L16, vdd-1.3-rfa L17, vdd-3.3-ch0 L23 of
 * the PM6125), which Linux's ath10k switched off on its way out, on
 * again; their voltages as the RPM has them from Linux */
/* the RPM's GLINK channel, once (the WLAN's rails, the sleep set) */
static bool rpm_up(void)
{
	static bool tried;
	if (tried) return rpm_ready();
	tried = true;
	if (!rpmram_pa || !apcs_pa || rpm_bit >= 32) {
		kprintf("pro1x: no RPM edge in the device tree\n");
		return false;
	}
	kprintf("pro1x: RPM: message RAM %p (%u KiB), IPC bit %u\n", rpmram_pa, rpmram_size >> 10, rpm_bit);
	return rpm_init((volatile u8 *)io_va(rpmram_pa), rpmram_size, rpm_kick, delay_us) == 0;
}

static void wifi_power(void)
{
	static const u32 rails[] = { 8, 16, 17, 23 };
	if (!rpm_up()) return;
	for (u32 i = 0; i < ARRAY_SIZE(rails); i++) rpm_ldo(rails[i], 0, true);
}

/* --- the SoC's sleep (docs/power.md, stage 3) ------------------------ *
 *
 * kolibri.rpmsleep (and the power test with the display off): the sleep
 * set at 0 (rpmsleep.c), and before the cluster's power collapse the
 * vMPM (its timer: the next deadline, no pin) handed to the RPM through
 * the MPM's IPC bit, as pro1x-maemo-leste's Linux patch 0011 does when
 * the last CPU goes down. The RPM's statistics (vmin, vlow) in the power
 * test's lines. */
static bool soc_sleep_on, handed;
static addr_t vmpm_pa, stats_pa;
static u32 vmpm_pins, mpm_bit, stats_size, handovers;
static struct mpm_wake wake_set;       /* the pins that wake the SoC (the standby's keys) */
static void wake_set_build(void);
static void mpm_woken(void);

static void mpm_setup(void)
{
	stats_pa = pdt.stats_pa;
	stats_size = pdt.stats_size;
	if (pdt.mpm && pdt.msgram_pa == rpmram_pa) {
		vmpm_pa = pdt.vmpm_pa;
		vmpm_pins = pdt.mpm_pins;
		mpm_bit = pdt.mpm_bit;
	} else if (rpmram_pa && rpmram_size >= 0x1b8 + 0x48) {
		/* a tree without the MPM (mainline's sm6115.dtsi has none):
		 * pro1x-maemo-leste's patch 0006, after QCM2290's and
		 * downstream bengal's: 0x1b8 in the message RAM, 96 pins,
		 * the APCS IPC bit 1 */
		vmpm_pa = rpmram_pa + 0x1b8;
		vmpm_pins = 96;
		mpm_bit = 1;
		kprintf("pro1x: MPM: not in the device tree, patch 0006's place (%p)\n", vmpm_pa);
	}
}

/* psci.c, before the cluster's power collapse: wake at that virtual count */
static void p_soc_sleep(u64 wake)
{
	if (!soc_sleep_on || !vmpm_pa || !apcs_pa) return;
	mpm_prepare((volatile u8 *)io_va(vmpm_pa), vmpm_pins, wake, &wake_set);
	wr32(io_va(apcs_pa) + 8, 1u << mpm_bit);
	handovers++;
	handed = true;
}

/* back: what the MPM saw (its pins' status); the keyboard's INT, no MPM
 * pin, is still low when a key went down while the SoC slept */
static void p_soc_wake(void)
{
	if (!handed) return;
	handed = false;
	mpm_woken();
	if (standby && kbd_ok && gpio_ok(kbd_int) && !gpio_in(kbd_int)) {
		kbd_sched = true;
		kbd_due = ticks;
	}
}

static void soc_sleep_start(void)
{
	mpm_setup();
	if (!rpm_up()) {
		kprintf("pro1x: the SoC's sleep: no RPM\n");
		return;
	}
	rpm_sleep_votes(wifi);
	soc_sleep_on = vmpm_pa != 0;
	kprintf("pro1x: the SoC's sleep: the sleep set voted, %s\n",
		soc_sleep_on ? "the MPM handed over before the cluster's collapse" : "no MPM");
	if (!soc_sleep_on) return;
	wake_set_build();
	/* the RPM's interrupt when the SoC wakes by an MPM pin (the mpm
	 * node's, rising edge): the pins' status read then */
	if (pdt.mpm && pdt.mpm_irq) gic3_enable_spi_edge(pdt.mpm_irq, true);
}

static void rpm_stats_line(const char *what)
{
	static u32 last[2];
	static bool have;
	struct rpm_stat st[2];
	if (!stats_pa) return;
	int n = rpm_stats_read((const volatile u8 *)io_va(stats_pa), stats_size, st);
	if (n < 2) {
		kprintf("rpm: %s: no statistics at %p\n", what, stats_pa);
		return;
	}
	u64 f = timer_freq() ? timer_freq() : 19200000;
	kprintf("rpm: %s: %s %u (+%u, %u s in all), %s %u (+%u, %u s), votes %x %x; MPM hand-overs %u\n", what,
		st[0].name, st[0].count, have ? st[0].count - last[0] : 0, (u32)(st[0].accumulated / f),
		st[1].name, st[1].count, have ? st[1].count - last[1] : 0, (u32)(st[1].accumulated / f),
		st[0].votes, st[1].votes, handovers);
	last[0] = st[0].count;
	last[1] = st[1].count;
	have = true;
}

/* --- the PMICs: the battery (docs/power.md, stage 4) ---------------- */

static u32 spmi_rd(addr_t a) { return rd32(a); }
static void spmi_wr(addr_t a, u32 v) { wr32(a, v); }

static bool pmic_up(void)
{
	static bool tried, ok;
	if (tried) return ok;
	tried = true;
	if (!spmi_pa[0] || !spmi_pa[1] || !spmi_pa[2] || !spmi_pa[3]) {
		kprintf("pro1x: no SPMI arbiter in the device tree\n");
		return false;
	}
	struct spmi_hw h = { io_va(spmi_pa[0]), io_va(spmi_pa[1]), io_va(spmi_pa[2]), io_va(spmi_pa[3]),
			     spmi_pa[4] ? io_va(spmi_pa[4]) : 0, spmi_size[0], spmi_ee, spmi_rd, spmi_wr,
			     delay_us };
	u32 ver = spmi_init(&h), ours;
	u32 n = spmi_channels(&ours);
	if (!ver) {
		kprintf("pro1x: SPMI: the arbiter is not version 5\n");
		return false;
	}
	kprintf("pro1x: SPMI: arbiter %u.%u, EE %u, %u channels, %u of them ours\n", ver >> 28,
		(ver >> 16) & 0xfff, spmi_ee, n, ours);
	if (!bat_found) {
		kprintf("pro1x: battery: no fxtec,pro1x-adc-battery in the device tree\n");
	} else if (battery_init(&bat) == 0) {
		kprintf("pro1x: battery: gauge at %u:%x (%s), ADC at %u:%x, channel %x (%u/%u)\n", bat.sid, bat.qg,
			bat.na_per_lsb == 152588 ? "5 A" : "10 A", bat.sid, bat.adc, bat.chan, bat.pre_num, bat.pre_den);
		ok = true;
	} else {
		kprintf("pro1x: battery: the gauge does not answer\n");
	}
	return ok;
}

/* samples during the power test, with the watchdog's (every 10 s): the
 * minute's and the test's sums */
static struct { s64 ma, mv; u32 n, errors; s32 lo, hi; } bmin, ball;
static u32 bfirst_mv;

static void battery_sample(void)
{
	s32 ma;
	u32 mv;
	if (!bat.ok) return;
	if (battery_current(&bat, &ma) || battery_voltage(&bat, &mv)) {
		bmin.errors++;
		return;
	}
	if (!bfirst_mv) bfirst_mv = mv;
	if (!bmin.n || ma < bmin.lo) bmin.lo = ma;
	if (!bmin.n || ma > bmin.hi) bmin.hi = ma;
	bmin.ma += ma, bmin.mv += mv, bmin.n++;
	ball.ma += ma, ball.mv += mv, ball.n++;
}

static void battery_line(int when)
{
	if (!bat.ok) return;
	if (when == 0) {
		battery_sample();
		if (bmin.n) kprintf("battery: at the start %u mV, %d mA (positive: discharging)\n", (u32)bmin.mv,
				    (int)bmin.ma);
		bmin.n = 0, bmin.ma = bmin.mv = 0;
		return;
	}
	if (when == 1) {
		if (bmin.n)
			kprintf("battery: %u mV, %d mA (%u samples, %d to %d)%s\n", (u32)(bmin.mv / bmin.n),
				(int)(bmin.ma / bmin.n), bmin.n, bmin.lo, bmin.hi, bmin.errors ? ", read errors" : "");
		else
			kprintf("battery: no samples this minute (%u read errors)\n", bmin.errors);
		bmin.n = bmin.errors = 0, bmin.ma = bmin.mv = 0;
		return;
	}
	if (ball.n)
		kprintf("battery: the test's mean %d mA over %u samples, %u mV to %u mV\n", (int)(ball.ma / ball.n),
			ball.n, bfirst_mv, (u32)(ball.mv / ball.n));
}

/* --- the display off: the panel's DCS commands, the scan-out stopped -- *
 *
 * As FreeBSD's qcom_dpu (pro1x-maemo-leste freebsd/patches/0022, after
 * Linux's msm dsi_cmd_dma_tx): the packet in msm's format in a buffer
 * below 4 GiB (the display's streams bypass the SMMU), the controller in
 * command mode as well, the DMA started after a frame's video, done when
 * CMD_DMA_DONE latches; the panel's OFF commands in high speed, as
 * Linux's panel-boe-bf060y8m-aj0 sends them (display off, 20 ms, sleep
 * in). Then the interface's timing engine stops (INTF_1 at the DPU's
 * 0x6a800, INTF_TIMING_ENGINE_EN: Linux's dpu_6_3_sm6115.h,
 * dpu_hw_intf.c): no more frames read from RAM. */
#define DSI_REG 4                       /* the 6G controller's register shift */
#define DSI_CTRL 0x000
#define DSI_CTRL_ENABLE (1u << 0)
#define DSI_CTRL_CMD_MODE_EN (1u << 2)
#define DSI_STATUS0 0x004
#define DSI_STATUS0_VIDEO_BUSY (1u << 3)
#define DSI_CMD_DMA_CTRL 0x038
#define DSI_CMD_DMA_LOW_POWER (1u << 26)
#define DSI_DMA_BASE 0x044
#define DSI_DMA_LEN 0x048
#define DSI_TRIG_DMA 0x08c
#define DSI_INTR_CTRL 0x10c
#define DSI_IRQ_CMD_DMA_DONE (1u << 0)
#define DSI_IRQ_VIDEO_DONE (1u << 16)
#define DSI_IRQ_MASKS 0x02220202u
#define INTF1_TIMING_ENGINE_EN 0x6a800
#define DCS_ENTER_SLEEP 0x10
#define DCS_EXIT_SLEEP 0x11
#define DCS_DISPLAY_OFF 0x28
#define DCS_DISPLAY_ON 0x29
#define DCS_BRIGHTNESS 0x51

static u8 *dsi_buf;

static u32 dsi_rd(u32 off) { return rd32(io_va(dsi_pa) + DSI_REG + off); }
static void dsi_wr(u32 off, u32 v) { wr32(io_va(dsi_pa) + DSI_REG + off, v); }

static bool dsi_dcs(const u8 *p, u32 n, bool hs)
{
	if (!dsi_pa || !dsi_buf || !n || n > 64) return false;
	u32 ctrl = dsi_rd(DSI_CTRL);
	if (!(ctrl & DSI_CTRL_ENABLE)) return false;
	u8 *d = dsi_buf;
	u32 len;
	if (n <= 2) {
		d[0] = p[0];
		d[1] = n == 2 ? p[1] : 0;
		d[2] = n == 2 ? 0x15 : 0x05;           /* DCS short write, with a parameter */
		d[3] = 0x80;                          /* the last */
		len = 4;
	} else {
		d[0] = (u8)n;
		d[1] = 0;
		d[2] = 0x39;                          /* DCS long write */
		d[3] = 0x80 | 0x40;
		memcpy(d + 4, p, n);
		len = ALIGN_UP(4 + n, 4);
		memset(d + 4 + n, 0xff, len - 4 - n);
	}
	for (addr_t a = (addr_t)d & ~63ul; a < (addr_t)d + len; a += 64)
		asm volatile("dc cvac, %0" :: "r"(a) : "memory");
	asm volatile("dsb sy");
	u32 dma = dsi_rd(DSI_CMD_DMA_CTRL);
	dsi_wr(DSI_CMD_DMA_CTRL, hs ? dma & ~DSI_CMD_DMA_LOW_POWER : dma | DSI_CMD_DMA_LOW_POWER);
	dsi_wr(DSI_CTRL, ctrl | DSI_CTRL_CMD_MODE_EN | DSI_CTRL_ENABLE);
	u32 masks = dsi_rd(DSI_INTR_CTRL) & DSI_IRQ_MASKS;
	dsi_wr(DSI_INTR_CTRL, masks | DSI_IRQ_CMD_DMA_DONE << 1 | DSI_IRQ_VIDEO_DONE << 1 | DSI_IRQ_CMD_DMA_DONE |
	       DSI_IRQ_VIDEO_DONE);
	if (dsi_rd(DSI_STATUS0) & DSI_STATUS0_VIDEO_BUSY) {
		for (int i = 0; i < 70 && !(dsi_rd(DSI_INTR_CTRL) & DSI_IRQ_VIDEO_DONE); i++) delay_us(1000);
		delay_us(3000);                       /* past the blanking's first line, as msm */
	}
	dsi_wr(DSI_DMA_BASE, (u32)(addr_t)d);
	dsi_wr(DSI_DMA_LEN, len);
	dsi_wr(DSI_TRIG_DMA, 1);
	int i;
	for (i = 0; i < 2000 && !(dsi_rd(DSI_INTR_CTRL) & DSI_IRQ_CMD_DMA_DONE); i++) delay_us(100);
	bool ok = dsi_rd(DSI_INTR_CTRL) & DSI_IRQ_CMD_DMA_DONE;
	dsi_wr(DSI_INTR_CTRL, masks | DSI_IRQ_CMD_DMA_DONE | DSI_IRQ_VIDEO_DONE);
	dsi_wr(DSI_CTRL, ctrl);
	dsi_wr(DSI_CMD_DMA_CTRL, dma);
	if (!ok) kprintf("pro1x: DCS %x: no CMD_DMA_DONE\n", p[0]);
	return ok;
}

static bool dcs1(u8 cmd, bool hs)
{
	return dsi_dcs(&cmd, 1, hs);
}

static bool dcs_brightness(u32 v)
{
	u8 c[3] = { DCS_BRIGHTNESS, (u8)v, (u8)(v >> 8) };
	return dsi_dcs(c, 3, false);
}

/* the DSI lanes to ULPS with the display off (kolibri.dsiulps, untried
 * on the phone: off unless asked): dsi_ulps.c, Linux's msm sequence */
static u32 ulps_rd(void *ctx, u32 off) { return dsi_rd(off); }
static void ulps_wr(void *ctx, u32 off, u32 v) { dsi_wr(off, v); }
static const struct dsi_regs ulps_regs = { NULL, ulps_rd, ulps_wr, delay_us };
static struct dsi_ulps ulps;

static const char *ulps_why(int e)
{
	switch (e) {
	case 0: return "done";
	case DSI_ULPS_FIFO: return "a FIFO not empty: left";
	case DSI_ULPS_NOT_IDLE: return "no lane in its stop state: left";
	case DSI_ULPS_REFUSED: return "not every lane went in: out again";
	default: return "a lane stuck in ULPS";
	}
}

/* off: brightness 0, display off, sleep in, the frames stopped, the
 * lanes to ULPS (kolibri.dsiulps); on: the reverse (the panel keeps its
 * registers through sleep; brightness 127, the panel driver's default) */
static void display_power(bool on)
{
	if (!dsi_pa || !mdp) {
		kprintf("pro1x: display %s: no DSI controller or DPU in the device tree\n", on ? "on" : "off");
		return;
	}
	if (!dsi_buf) dsi_buf = kmalloc_aligned(256, 64);
	if ((addr_t)dsi_buf >> 32) {
		kprintf("pro1x: display: no buffer below 4 GiB for the DSI's DMA\n");
		return;
	}
	addr_t te = io_va(mdp) + INTF1_TIMING_ENGINE_EN;
	if (!on) {
		bool b = dcs_brightness(0);
		bool o = dcs1(DCS_DISPLAY_OFF, true);
		delay_us(20000);
		bool sl = dcs1(DCS_ENTER_SLEEP, true);
		delay_us(2000);
		u32 was = rd32(te);
		wr32(te, 0);
		delay_us(20000);                       /* the last frame out */
		display_off = true;
		kprintf("pro1x: display off: brightness %s, display off %s, sleep in %s, timing engine %u -> 0\n",
			b ? "0" : "failed", o ? "done" : "failed", sl ? "done" : "failed", was);
		if (dsiulps) {
			int e = dsi_ulps_enter(&ulps_regs, &ulps);
			kprintf("pro1x: DSI lanes to ULPS (lanes %x, LANE_CTRL %x): %s, status %x\n", ulps.lanes,
				ulps.lane_ctrl, ulps_why(e), ulps.status);
		}
	} else {
		/* the lanes out of ULPS first: the DCS commands need them */
		if (ulps.in) {
			int e = dsi_ulps_exit(&ulps_regs, &ulps);
			kprintf("pro1x: DSI lanes out of ULPS: %s, status %x, LANE_CTRL %x\n", ulps_why(e), ulps.status,
				dsi_rd(0xa8));
		}
		wr32(te, 1);
		delay_us(20000);
		bool sl = dcs1(DCS_EXIT_SLEEP, true);
		delay_us(30000);
		bool o = dcs1(DCS_DISPLAY_ON, true);
		delay_us(50000);
		bool b = dcs_brightness(127);
		display_off = false;
		extern void gfx_dirty(int y0, int y1);
		gfx_dirty(0, lh);
		kprintf("pro1x: display on: sleep out %s, display on %s, brightness %s\n", sl ? "done" : "failed",
			o ? "done" : "failed", b ? "127" : "failed");
	}
}

/* --- the power test on the Pro1X ------------------------------------- *
 *
 * kolibri.powertest=N: the battery sampled every 10 s and shown a
 * minute, the RPM's counts; the standby: 10 s after the start the
 * display off and the SoC's sleep (the sleep set, the MPM; as
 * kolibri.rpmsleep), with kolibri.displayon the display stays on and the
 * sleep set as it is. At the end the display on again, then the
 * lifetime's reset. */
static u32 standby_at;
static bool standby_done;

static void p_power_report(int when)
{
	rpm_stats_line(when == 0 ? "at the start" : when == 2 ? "at the end" : "this minute");
	battery_line(when);
	if (when == 2 && display_off) display_power(true);
}

static void standby_enter(const char *why);

static void standby_start(void)
{
	standby_done = true;
	if (displayon) {
		kprintf("pro1x: power test: the display stays on (kolibri.displayon)\n");
		return;
	}
	standby_enter("the power test");
}

/* --- the standby and the power key (docs/power.md, stage 5) ----------- *
 *
 * The power key: the PM6125's PON (qcom,pm8998-pon at 0x800 of slave 0;
 * its qcom,pm8941-pwrkey child: KPDPWR_N, interrupt 0, RT_STS 0x10 bit 0;
 * qcom,pm8941-resin: the volume-down key, interrupt 1, bit 1; Linux's
 * pm8941-pwrkey.c) through the SPMI arbiter's interrupt (sm6115.dtsi:
 * <&mpm 86 IRQ_TYPE_LEVEL_HIGH>, the MPM's pin 86, GIC SPI 183 by its pin
 * map). Every other PMIC interrupt Linux left on for this processor is
 * taken out of the arbiter's line first (spmi_irq_quiet), as the TLMM's.
 *
 * The power key starts the standby: the display off, the programs'
 * timeouts on whole seconds, the SoC's sleep (the sleep set, the MPM).
 * The power key again, the volume-down key, any key of the keyboard or
 * Fn end it (that key is not passed on), the display on; touches are
 * read and dropped meanwhile. While the SoC sleeps (vlow, vmin) only the
 * MPM's pins wake it: the arbiter's (the power and volume keys), and the
 * TLMM GPIOs with an MPM pin (Linux's sm6115_mpm_map, pinctrl-sm6115.c):
 * of the Pro1X's keys the modifiers Home (GPIO 93), right Shift (83),
 * left Ctrl (84), left Alt (19); not the two Fn keys (106, 112) nor the
 * expander's INT (107): a letter key is seen at the next wake-up by time
 * (the shell's, a second in the standby), by its INT still low. */
static const u8 sm6115_mpm_gpio[][2] = {
	{ 0, 84 }, { 3, 75 }, { 4, 16 }, { 6, 59 }, { 8, 63 }, { 11, 17 }, { 13, 18 },
	{ 14, 51 }, { 17, 20 }, { 18, 52 }, { 19, 53 }, { 24, 6 }, { 25, 71 }, { 27, 73 },
	{ 28, 41 }, { 31, 27 }, { 32, 54 }, { 33, 55 }, { 34, 56 }, { 35, 57 }, { 36, 58 },
	{ 39, 28 }, { 46, 29 }, { 62, 60 }, { 63, 61 }, { 64, 62 }, { 65, 30 }, { 66, 31 },
	{ 67, 32 }, { 69, 33 }, { 70, 34 }, { 72, 72 }, { 75, 35 }, { 79, 36 }, { 80, 21 },
	{ 81, 38 }, { 83, 9 }, { 84, 39 }, { 85, 40 }, { 86, 19 }, { 87, 42 }, { 88, 43 },
	{ 89, 45 }, { 91, 74 }, { 93, 46 }, { 94, 47 }, { 95, 48 }, { 96, 49 }, { 97, 50 },
};

static int mpm_pin_of_gpio(int gpio)
{
	for (u32 i = 0; i < ARRAY_SIZE(sm6115_mpm_gpio); i++)
		if (sm6115_mpm_gpio[i][0] == gpio) return sm6115_mpm_gpio[i][1];
	return -1;
}

/* a PON key: its peripheral, its interrupt, its RT_STS bit */
struct ponkey {
	bool found, down;
	u32 sid, base, irq, bit;
};
static struct ponkey pwrkey, resin;
static bool pon_on, nopowerkey;
static u32 standby_since, keytest, keytest_at, mpm_key_wakes, mpm_spmi_wakes;
static u8 swallowed[64 + 8];           /* keys that ended the standby: their release dropped too */

static void standby_enter(const char *why)
{
	if (standby) return;
	standby = true;
	standby_since = ticks;
	mpm_key_wakes = mpm_spmi_wakes = 0;
	kprintf("pro1x: standby (%s)\n", why);
	display_power(false);
	power_set_standby(true);
	power_hold_display(false);
	if (!soc_sleep_on) soc_sleep_start();
	if (keytest) keytest_at = ticks + keytest * 100;
}

static void standby_leave(const char *why)
{
	if (!standby) return;
	standby = false;
	power_set_standby(false);
	/* the sleep set stays at 0 (Linux's votes are not known to put them
	 * back): the scan-out needs the XO and the buses, so no SoC sleep
	 * while the display is on, unless kolibri.rpmsleep asks for it */
	if (!rpmsleep) power_hold_display(true);
	kprintf("pro1x: awake (%s) after %u s; MPM wake-ups: %u by keys, %u by the SPMI arbiter\n", why,
		(ticks - standby_since) / 100, mpm_key_wakes, mpm_spmi_wakes);
	if (display_off) display_power(true);
}

/* key() and Fn in the standby: the press ends it, press and release are
 * not passed on */
static bool standby_key(int pos, bool pressed)
{
	if (pressed && standby) {
		swallowed[pos] = 1;
		standby_leave("a key");
		return true;
	}
	if (!pressed && swallowed[pos]) {
		swallowed[pos] = 0;
		return true;
	}
	return false;
}

static void powerkey_press(const char *how)
{
	if (standby) standby_leave(how);
	else standby_enter(how);
}

/* the PON's interrupts: the keys' state from RT_STS; a release without
 * its press (both edges between two looks) a short press, as Linux */
static void pon_key(struct ponkey *k, u8 rt, bool power)
{
	bool d = rt & k->bit;
	bool press = d && !k->down;
	if (!d && !k->down) press = true;
	k->down = d;
	if (!press) return;
	if (power) powerkey_press("the power key");
	else if (standby) standby_leave("volume down");
}

static void pon_isr(void)
{
	static bool said;
	static u32 empty;
	struct spmi_irq ev[8];
	int n = spmi_irq_take(ev, 8);
	/* the line up with nothing to take, again and again: off, said */
	if (!n && ++empty == 1000) {
		gic3_disable_spi(spmi_intid);
		pon_on = false;
		kprintf("pro1x: power key: the SPMI arbiter's interrupt stays up with nothing pending: off\n");
	}
	if (n) empty = 0;
	for (int i = 0; i < n; i++) {
		if (ev[i].sid != pwrkey.sid || ev[i].addr != pwrkey.base) {
			if (!said) kprintf("pro1x: SPMI: an interrupt of %u:%x (%x), not ours\n", ev[i].sid,
					   ev[i].addr, ev[i].bits);
			said = true;
			continue;
		}
		u8 rt = 0;
		if (spmi_read(pwrkey.sid, pwrkey.base + 0x10, &rt, 1)) continue;
		if (ev[i].bits & 1u << pwrkey.irq) pon_key(&pwrkey, rt, true);
		if (resin.found && resin.sid == pwrkey.sid && resin.base == pwrkey.base && (ev[i].bits & 1u << resin.irq))
			pon_key(&resin, rt, false);
	}
}

static void pon_init(void)
{
	if (nopowerkey) {
		kprintf("pro1x: power key: off (kolibri.nopowerkey)\n");
		return;
	}
	if (!pwrkey.found) {
		kprintf("pro1x: power key: no qcom,pm8941-pwrkey in the device tree\n");
		return;
	}
	if (!spmi_intid || !spmi_pa[4]) {
		kprintf("pro1x: power key: the SPMI arbiter has no %s in the device tree\n",
			spmi_intid ? "intr region" : "interrupt");
		return;
	}
	if (!pmic_up()) return;
	u32 quiet = spmi_irq_quiet();
	int r = spmi_irq_enable(pwrkey.sid, pwrkey.base, pwrkey.irq, SPMI_IRQ_EDGE_BOTH);
	if (r) {
		kprintf("pro1x: power key: its interrupt not set up (%d: %s)\n", r,
			r == -5 ? "not this EE's" : r == -2 ? "not ours to write" : r == -1 ? "no channel" : "refused");
		return;
	}
	int r2 = resin.found && resin.sid == pwrkey.sid && resin.base == pwrkey.base ?
		 spmi_irq_enable(resin.sid, resin.base, resin.irq, SPMI_IRQ_EDGE_BOTH) : -1;
	if (r2) resin.found = false;
	/* PON's other interrupts (Linux's driver uses these two) off */
	u8 others = (u8)~(1u << pwrkey.irq | (resin.found ? 1u << resin.irq : 0));
	spmi_write(pwrkey.sid, pwrkey.base + 0x16, &others, 1);
	u8 rt = 0;
	spmi_read(pwrkey.sid, pwrkey.base + 0x10, &rt, 1);
	pwrkey.down = rt & pwrkey.bit;
	resin.down = rt & resin.bit;
	gic3_enable_spi_edge(spmi_intid, false);
	pon_on = true;
	kprintf("pro1x: power key: PON at %u:%x, interrupt %u%s, through INTID %u (MPM pin %d); "
		"%u of Linux's PMIC interrupts quieted\n", pwrkey.sid, pwrkey.base, pwrkey.irq,
		resin.found ? ", volume down 1" : "", spmi_intid, (int)spmi_mpm_pin, quiet);
}

/* the MPM's wake set: the arbiter's pin (a high level), the keys' GPIOs
 * that have a pin (both edges) */
static void wake_set_build(void)
{
	int none[10], nn = 0;
	bool any = false;
	mzero(&wake_set, sizeof(wake_set));
	kprintf("pro1x: MPM wake-ups:");
	if (pon_on && spmi_mpm_pin < vmpm_pins) {
		mpm_wake_pin(&wake_set, spmi_mpm_pin, false, false, true);
		kprintf(" SPMI (pin %u)", spmi_mpm_pin);
		any = true;
	}
	for (int i = 0; i < ngkeys; i++) {
		int pin = mpm_pin_of_gpio(gkey[i].pin);
		if (pin >= 0 && (u32)pin < vmpm_pins) {
			mpm_wake_pin(&wake_set, (u32)pin, true, true, false);
			kprintf(" GPIO %d (pin %d)", gkey[i].pin, pin);
			any = true;
		} else if (nn < (int)ARRAY_SIZE(none)) {
			none[nn++] = gkey[i].pin;
		}
	}
	if (kbd_ok && gpio_ok(kbd_int)) {
		int pin = mpm_pin_of_gpio(kbd_int);
		if (pin >= 0 && (u32)pin < vmpm_pins) mpm_wake_pin(&wake_set, (u32)pin, false, true, false);
		else if (nn < (int)ARRAY_SIZE(none)) none[nn++] = kbd_int;
	}
	if (!any) kprintf(" the timer only");
	if (nn) kprintf("; no MPM pin: GPIO");
	for (int i = 0; i < nn; i++) kprintf(" %d", none[i]);
	kprintf("\n");
}

static void mpm_woken(void)
{
	u32 st[MPM_BANKS];
	if (!vmpm_pa) return;
	mzero(st, sizeof(st));
	mpm_status((const volatile u8 *)io_va(vmpm_pa), vmpm_pins, st);
	bool keys = false;
	for (u32 b = 0; b < MPM_BANKS; b++) {
		u32 hit = st[b] & wake_set.enable[b];
		for (u32 i = 0; hit && i < 32; i++) {
			if (!(hit & 1u << i)) continue;
			if (32 * b + i == spmi_mpm_pin) mpm_spmi_wakes++;
			else keys = true, mpm_key_wakes++;
		}
	}
	/* the modifiers: looked at now (their TLMM edge may be lost); the
	 * arbiter's level interrupt comes by itself */
	if (keys) {
		kbd_sched = true;
		kbd_due = ticks;
	}
}

/* the UFS: as Linux left it (kexec), its SMMU context's translation off,
 * DMA memory below 4 GiB (kernel/storage/ufs.c) */
bool storage_off(void);
int storage_home_mode(void);
bool storage_dma(struct dma_mem *m, u32 mb, u64 limit);
static u32 ufs_rd(void *c, u32 off) { (void)c; return rd32(io_va(ufs_pa) + off); }
static void ufs_wr(void *c, u32 off, u32 v) { (void)c; wr32(io_va(ufs_pa) + off, v); }
static u64 now_us(void)
{
	u64 c = SYSREG_R(cntvct_el0), f = SYSREG_R(cntfrq_el0);
	return c / f * 1000000 + c % f * 1000000 / f;     /* no overflow after days up */
}
static void dsb(void) { asm volatile("dsb sy" ::: "memory"); }

static void ufs_init(void)
{
	if (storage_off()) {
		kprintf("pro1x: storage left alone (kolibri.storage=off)\n");
		return;
	}
	if (!ufs_pa) {
		kprintf("pro1x: no qcom,ufshc in the device tree\n");
		return;
	}
	static struct ufs_hw hw = { .rd = ufs_rd, .wr = ufs_wr, .now_us = now_us, .barrier = dsb, .keep_link = true };
	if (!storage_dma(&hw.dma, 2, 1ull << 32)) {
		kprintf("pro1x: no DMA memory below 4 GiB for the UFS\n");
		return;
	}
	if (smmu && ufs_nstreams) smmu_bypass_streams(ufs_streams, ufs_nstreams);
	else kprintf("pro1x: UFS: no SMMU stream in the device tree\n");
	hw.writable = storage_home_mode() == HOME_RW;
	/* Auto-Hibernate found off: ufshcd's 150 ms; kolibri.ufsahit=keep
	 * leaves it off, kolibri.ufsahit=N sets N ms (1-1023) */
	const char *ah = boot_arg("kolibri.ufsahit");
	hw.ahit = UFS_AHIT_LINUX;
	if (ah && !strncmp(ah, "keep", 4)) hw.ahit = 0;
	else if (ah && *ah >= '1' && *ah <= '9') {
		u32 ms = 0;
		for (; *ah >= '0' && *ah <= '9'; ah++) ms = ms * 10 + *ah - '0';
		hw.ahit = 3u << 10 | MIN(ms, 1023u);
	}
	kprintf("pro1x: UFS at %p, DMA %p\n", ufs_pa, (addr_t)hw.dma.pa);
	ufs_start(&hw);
}

static void p_other_irq(u32 id);
static void tlmm_irq_init(void);

static void p_init(void)
{
	extern void (*plat_power_report)(int when);
	if (!stats_pa) stats_pa = pdt.stats_pa, stats_size = pdt.stats_size;
	plat_power_report = p_power_report;
	/* the battery before the power test's first line (timer_start) */
	if (powertest || battery_arg) pmic_up();
	gic3_init(gicd, gicr, p_other_irq);
	if (lifetime) kprintf("pro1x: lifetime %u s\n", lifetime);
	ufs_init();
	pon_init();
	prng_init();
	if (wifi) {
		net_init();
		wifi_power();
	}
	if (rpmsleep) soc_sleep_start();
	else if (stats_pa && !powertest) rpm_stats_line("left by Linux");   /* (the test's start line has it) */
	standby_at = ticks + 1000;
	modem_init();
	if (noinput) {
		kprintf("pro1x: no input (kolibri.noinput)\n");
		return;
	}
	kbd_init();
	ts_init();
	tlmm_irq_init();
}

static void p_irq(void)
{
	gic3_irq();
}

static bool due(bool sched, u32 at)
{
	return sched && (s32)(ticks - at) >= 0;
}

static void p_tick(void)
{
	if (wdt_feeding && (s32)(ticks - wdt_next) >= 0) {
		if (lifetime && ticks >= lifetime * 100) {
			shot();
			if (wifi) wifi_dev_report();
			kprintf("pro1x: lifetime over: the watchdog resets the phone\n");
			wdt_feeding = false;
		} else {
			if (wdt) wr32(io_va(wdt) + WDT_RST, 1);
			if (powertest) battery_sample();
			wdt_next = ticks + wdt_every;
			if (lifetime && (s32)(wdt_next - lifetime * 100) > 0) wdt_next = lifetime * 100;
		}
	}
	if (!noinput) {
		if (due(kbd_sched, kbd_due)) kbd_poll();
		if (due(ts_sched, ts_due)) ts_poll();
	}
	if (powertest && !standby_done && (s32)(ticks - standby_at) >= 0) standby_start();
	/* kolibri.keytest=S: a press of the power key played S s into each
	 * standby (the way back without hands; QEMU) */
	if (keytest && standby && (s32)(ticks - keytest_at) >= 0) powerkey_press("the power key, played");
}

/* tickless idle: the watchdog, the lifetime, the input's polls */
static u32 p_next_tick(void)
{
	u32 n = wdt_feeding ? until(wdt_next) : 0xffffffffu;
	if (powertest && !standby_done) n = MIN(n, until(standby_at));
	if (keytest && standby) n = MIN(n, until(keytest_at));
	if (!noinput) {
		if (kbd_sched) n = MIN(n, until(kbd_due));
		if (ts_sched && ts_ok) n = MIN(n, until(ts_due));
	}
	return n;
}

/* --- the TLMM's interrupts: the keyboard's, the touch screen's -------- */

#define INTR_ENABLE (1u << 0)
#define INTR_POL_HIGH (1u << 1)
#define INTR_DETECT_SHIFT 2            /* 0 level, 1 rising, 2 falling, 3 both */
#define INTR_RAW_STATUS (1u << 4)
#define INTR_TARGET_SHIFT 5
#define INTR_TARGET_KPSS 3u            /* (Linux's pinctrl-sm6115.c) */

static bool pin_reserved(int pin)
{
	for (int i = 0; i < nreserved; i++)
		if ((u32)pin >= reserved[i][0] && (u32)pin < reserved[i][0] + reserved[i][1]) return true;
	return false;
}

/* routed to this processor (the KPSS): other pins' interrupts belong to
 * the modem's or the DSPs' side and are not touched */
static bool pin_kpss(int pin)
{
	return (rd32(gpio_reg(pin, TLMM_INTR_CFG)) >> INTR_TARGET_SHIFT & 7) == INTR_TARGET_KPSS;
}

static bool pin_ours(int pin)
{
	if (pin == kbd_int || pin == ts_irq) return true;
	for (int i = 0; i < ngkeys; i++)
		if (gkey[i].pin == pin) return true;
	return false;
}

/* an edge interrupt to this processor (pinctrl-msm.c's
 * msm_gpio_irq_set_type with intr_detection_width 2), unmasked */
static void pin_irq(int pin, u32 detect)
{
	addr_t c = gpio_reg(pin, TLMM_INTR_CFG);
	u32 v = rd32(c) & ~(0x7u << INTR_TARGET_SHIFT | 3u << INTR_DETECT_SHIFT | INTR_POL_HIGH);
	v |= INTR_TARGET_KPSS << INTR_TARGET_SHIFT | detect << INTR_DETECT_SHIFT | INTR_POL_HIGH |
	     INTR_RAW_STATUS;
	wr32(c, v);
	wr32(gpio_reg(pin, TLMM_INTR_STATUS), 0);
	wr32(c, v | INTR_ENABLE);
}

/* every GPIO interrupt Linux left on for this processor masked but ours
 * (an unknown one would hold the summary interrupt for ever), ours on:
 * the expander's INT falling, the modifiers' and the touch screen's
 * either way */
static void tlmm_irq_init(void)
{
	if (noinput || !tlmm_irq || !timer_tickless()) return;
	int off = 0;
	for (int pin = 0; pin < NPINS; pin++) {
		if (!gpio_ok(pin) || pin_reserved(pin) || pin_ours(pin)) continue;
		addr_t c = gpio_reg(pin, TLMM_INTR_CFG);
		u32 v = rd32(c);
		if (!(v & INTR_ENABLE) || (v >> INTR_TARGET_SHIFT & 7) != INTR_TARGET_KPSS) continue;
		wr32(c, v & ~INTR_ENABLE);
		wr32(gpio_reg(pin, TLMM_INTR_STATUS), 0);
		off++;
	}
	int on = 0;
	if (kbd_ok && gpio_ok(kbd_int) && !pin_reserved(kbd_int)) {
		pin_irq(kbd_int, 2);
		on++;
	}
	for (int i = 0; i < ngkeys; i++)
		if (!pin_reserved(gkey[i].pin)) {
			pin_irq(gkey[i].pin, 3);
			on++;
		}
	if (ts_ok && gpio_ok(ts_irq) && !pin_reserved(ts_irq)) {
		pin_irq(ts_irq, 3);
		on++;
	}
	/* without the keyboard's INT a key is only seen by polling */
	if (!kbd_ok || !gpio_ok(kbd_int)) {
		kprintf("pro1x: input interrupts: not without the keyboard's INT\n");
		return;
	}
	gic3_enable_spi(tlmm_irq);
	input_irq = true;
	kbd_sched = ts_sched = true;           /* a first look, then by interrupt */
	kbd_due = ts_due = ticks;
	kprintf("pro1x: input interrupts: %d GPIOs through INTID %u (%d of Linux's masked)\n", on, tlmm_irq, off);
}

static void tlmm_isr(void)
{
	for (int pin = 0; pin < NPINS; pin++) {
		if (!gpio_ok(pin) || pin_reserved(pin)) continue;
		if (!(rd32(gpio_reg(pin, TLMM_INTR_STATUS)) & 1) || !pin_kpss(pin)) continue;
		wr32(gpio_reg(pin, TLMM_INTR_STATUS), 0);
		if (pin == ts_irq) {
			ts_sched = true;
			ts_due = ticks;
		} else if (pin_ours(pin)) {
			kbd_sched = true;
			kbd_due = ticks;
		} else {
			addr_t c = gpio_reg(pin, TLMM_INTR_CFG);
			wr32(c, rd32(c) & ~INTR_ENABLE);
		}
	}
	if (due(kbd_sched, kbd_due)) kbd_poll();
	if (due(ts_sched, ts_due)) ts_poll();
}

static void p_other_irq(u32 id)
{
	if (id == tlmm_irq && input_irq) tlmm_isr();
	else if (id == modem_irq && modem_irq) kmodem_irq();
	else if (id == spmi_intid && pon_on) pon_isr();
	else if (id == pdt.mpm_irq && soc_sleep_on) mpm_woken();
	else if (wifi_irq && wifi_nirq && id >= wifi_intid[0] && id < wifi_intid[0] + wifi_nirq) wifi_dev_irq();
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
	.next_tick = p_next_tick,
	.soc_sleep = p_soc_sleep,
	.soc_wake = p_soc_wake,
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

	/* the Wi-Fi (qcom,wcn3990-wifi): its register window, the copy
	 * engines' interrupts (12 SPIs), its SMMU stream */
	int wn = fdt_find_compatible(fdt, "qcom,wcn3990-wifi", -1, &ac, &sc);
	if (wn >= 0 && fdt_reg(fdt, wn, ac, sc, 0, &a, &s) && s <= 16 * MB) {
		wifi_pa = a;
		wifi_size = (u32)s;
		add_io(a, s, MAP_DEVICE);
		p = fdt_prop(fdt, wn, "interrupts", &len);
		for (int i = 0; p && i + 3 <= len / 4 && wifi_nirq < 12; i += 3)
			if (cell(p, i) == 0) wifi_intid[wifi_nirq++] = 32 + cell(p, i + 1);
		p = fdt_prop(fdt, wn, "iommus", &len);
		for (int i = 0; p && i + 3 <= len / 4 && wifi_nstreams < 2; i += 3) {
			wifi_streams[wifi_nstreams][0] = cell(p, i + 1);
			wifi_streams[wifi_nstreams][1] = cell(p, i + 2);
			wifi_nstreams++;
		}
	}

	/* the UFS host controller (its "std" registers) and its SMMU stream */
	int un = fdt_find_compatible(fdt, "qcom,ufshc", -1, &ac, &sc);
	if (un >= 0 && fdt_reg(fdt, un, ac, sc, 0, &a, &s) && s <= 0x10000) {
		ufs_pa = a;
		add_io(a, s, MAP_DEVICE);
		p = fdt_prop(fdt, un, "iommus", &len);
		if (p && len >= 12 && !smmu) {          /* its SMMU, if the display's did not say */
			int sm = fdt_find_phandle(fdt, cell(p, 0));
			int sac = 2, ssc = 2;
			if (sm >= 0 && fdt_find_compatible(fdt, "arm,mmu-500", sm - 1, &sac, &ssc) == sm)
				reg_io(fdt, sm, sac, ssc, 0, &smmu, MAP_DEVICE);
		}
		for (int i = 0; p && i + 3 <= len / 4 && ufs_nstreams < 2; i += 3) {
			ufs_streams[ufs_nstreams][0] = cell(p, i + 1);
			ufs_streams[ufs_nstreams][1] = cell(p, i + 2);
			ufs_nstreams++;
		}
	}

	/* the GPIOs */
	int t = fdt_find_compatible(fdt, "qcom,sm6115-tlmm", -1, &ac, &sc);
	u32 tlmm_ph = t >= 0 ? fdt_u32(fdt, t, "phandle", 0) : 0;
	for (int i = 0; i < 3; i++) reg_io(fdt, t, ac, sc, i, &tlmm[i], MAP_DEVICE);
	/* its summary interrupt (an SPI), the GPIOs that are not Linux's */
	p = t >= 0 ? fdt_prop(fdt, t, "interrupts", &len) : NULL;
	if (p && len >= 12 && cell(p, 0) == 0) tlmm_irq = 32 + cell(p, 1);
	p = t >= 0 ? fdt_prop(fdt, t, "gpio-reserved-ranges", &len) : NULL;
	for (int i = 0; p && i + 1 < len / 4 && nreserved < (int)ARRAY_SIZE(reserved); i += 2) {
		reserved[nreserved][0] = cell(p, i);
		reserved[nreserved++][1] = cell(p, i + 1);
	}

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
	p = edge >= 0 ? fdt_prop(fdt, edge, "interrupts", &len) : NULL;
	if (p && len >= 12 && cell(p, 0) == 0) modem_irq = 32 + cell(p, 1);
	p = edge >= 0 ? fdt_prop(fdt, edge, "mboxes", &len) : NULL;
	if (p && len >= 8) {
		int mb = fdt_find_phandle(fdt, cell(p, 0));
		int mac = 2, msc = 2;
		apcs_bit = cell(p, 1);
		if (mb >= 0 && fdt_find_compatible(fdt, "qcom,sm6115-apcs-hmss-global", mb - 1, &mac, &msc) == mb)
			reg_io(fdt, mb, mac, msc, 0, &apcs_pa, MAP_DEVICE);
	}

	/* the PRNG and its clock (in the GCC); the RPM's GLINK edge: its
	 * message RAM, its bit in the IPC register */
	reg_io(fdt, fdt_find_compatible(fdt, "qcom,prng-ee", -1, &ac, &sc), ac, sc, 0, &prng_pa, MAP_DEVICE);
	reg_io(fdt, fdt_find_compatible(fdt, "qcom,gcc-sm6115", -1, &ac, &sc), ac, sc, 0, &gcc_pa, MAP_DEVICE);
	int rr = fdt_find_compatible(fdt, "qcom,rpm-msg-ram", -1, &ac, &sc);
	if (rr >= 0 && fdt_reg(fdt, rr, ac, sc, 0, &a, &s) && s <= 0x10000) {
		rpmram_pa = a;
		rpmram_size = s;
		add_io(a, s, MAP_DEVICE);              /* below 1 GiB: moved up, 32-bit accesses */
	}
	int gr = fdt_find_compatible(fdt, "qcom,glink-rpm", -1, &ac, &sc);
	p = gr >= 0 ? fdt_prop(fdt, gr, "mboxes", &len) : NULL;
	if (p && len >= 8) rpm_bit = cell(p, 1);

	/* the PMICs: the SPMI arbiter's regions by name, and the battery: a
	 * PMIC (qcom,spmi-pmic, reg <sid type>) with the gauge, the ADC's
	 * channel its io-channels name (pmi632.dtsi: VBAT_SNS, 1/3) */
	int sp = fdt_find_compatible(fdt, "qcom,spmi-pmic-arb", -1, &ac, &sc);
	const char *rnames = sp >= 0 ? fdt_prop(fdt, sp, "reg-names", &len) : NULL;
	static const char *const spmi_names[5] = { "core", "chnls", "obsrvr", "cnfg", "intr" };
	for (int k = 0, idx = 0, o = 0; rnames && o < len; idx++) {
		for (k = 0; k < 5; k++)
			if (!strcmp(rnames + o, spmi_names[k]) && fdt_reg(fdt, sp, ac, sc, idx, &a, &s)) {
				spmi_pa[k] = a;
				spmi_size[k] = (u32)s;
				add_io(a, s, MAP_DEVICE);
			}
		o += strlen(rnames + o) + 1;
	}
	if (sp >= 0) spmi_ee = fdt_u32(fdt, sp, "qcom,ee", 0);
	/* its interrupt: through the MPM (a pin, the GIC's SPI by the pin
	 * map), or the GIC's own */
	p = sp >= 0 ? fdt_prop(fdt, sp, "interrupts-extended", &len) : NULL;
	if (p && len >= 12) {
		int ic = fdt_find_phandle(fdt, cell(p, 0));
		int ml = 0;
		const void *map = ic >= 0 ? fdt_prop(fdt, ic, "qcom,mpm-pin-map", &ml) : NULL;
		if (map && fdt_is_compatible(fdt, ic, "qcom,mpm")) {
			for (int i = 0; i + 1 < ml / 4; i += 2)
				if (cell(map, i) == cell(p, 1)) {
					spmi_mpm_pin = cell(p, 1);
					spmi_intid = 32 + cell(map, i + 1);
				}
		} else if (ic >= 0 && fdt_is_compatible(fdt, ic, "arm,gic-v3") && len >= 16 && cell(p, 1) == 0) {
			spmi_intid = 32 + cell(p, 2);
		}
	} else if (sp >= 0 && (p = fdt_prop(fdt, sp, "interrupts", &len)) && len >= 12 && cell(p, 0) == 0) {
		spmi_intid = 32 + cell(p, 1);
	}
	/* the power key and the volume-down key: PON's children (pm6125.dtsi:
	 * qcom,pm8941-pwrkey, qcom,pm8941-resin; interrupts <sid peripheral
	 * irq type>, RT_STS bits 0 and 1) */
	for (int pm = sp >= 0 ? fdt_next_child(fdt, sp, -1) : -1; pm >= 0; pm = fdt_next_child(fdt, sp, pm))
		for (int pon = fdt_next_child(fdt, pm, -1); pon >= 0; pon = fdt_next_child(fdt, pm, pon))
			for (int k = fdt_next_child(fdt, pon, -1); k >= 0; k = fdt_next_child(fdt, pon, k)) {
				bool pw = fdt_is_compatible(fdt, k, "qcom,pm8941-pwrkey");
				if (!pw && !fdt_is_compatible(fdt, k, "qcom,pm8941-resin")) continue;
				const char *st = fdt_prop(fdt, k, "status", &len);
				if (st && strcmp(st, "okay") && strcmp(st, "ok")) continue;
				p = fdt_prop(fdt, k, "interrupts", &len);
				if (!p || len < 16) continue;
				struct ponkey *pk = pw ? &pwrkey : &resin;
				*pk = (struct ponkey){ .found = true, .sid = cell(p, 0), .base = cell(p, 1) << 8,
						       .irq = cell(p, 2), .bit = pw ? 1u : 2u };
			}
	for (int pm = sp >= 0 ? fdt_next_child(fdt, sp, -1) : -1; pm >= 0; pm = fdt_next_child(fdt, sp, pm)) {
		int g = fdt_child_compatible(fdt, pm, "fxtec,pro1x-adc-battery");
		p = g >= 0 ? fdt_prop(fdt, g, "io-channels", &len) : NULL;
		if (!p || len < 8) continue;
		int adc = fdt_find_phandle(fdt, cell(p, 0));
		u32 chan = cell(p, 1);
		bool same = false;
		for (int c = fdt_next_child(fdt, pm, -1); c >= 0; c = fdt_next_child(fdt, pm, c))
			if (c == adc) same = true;
		if (!same || !fdt_is_compatible(fdt, adc, "qcom,spmi-adc5")) continue;
		bat = (struct battery){ .sid = fdt_u32(fdt, pm, "reg", 0), .qg = fdt_u32(fdt, g, "reg", 0),
					.adc = fdt_u32(fdt, adc, "reg", 0), .chan = chan, .pre_num = 1, .pre_den = 1 };
		for (int c = fdt_next_child(fdt, adc, -1); c >= 0; c = fdt_next_child(fdt, adc, c)) {
			const void *ps = fdt_prop(fdt, c, "qcom,pre-scaling", &len);
			if (fdt_u32(fdt, c, "reg", 0xffffffffu) == chan && ps && len >= 8 && cell(ps, 0)) {
				bat.pre_num = cell(ps, 0);
				bat.pre_den = cell(ps, 1);
			}
		}
		bat_found = true;
		break;
	}
	/* the DSI controller (its DCS commands: the panel off) */
	reg_io(fdt, fdt_find_compatible(fdt, "qcom,sm6115-dsi-ctrl", -1, &ac, &sc), ac, sc, 0, &dsi_pa, MAP_DEVICE);

	/* the command line: kolibri.lifetime=N, kolibri.noinput */
	int ch = fdt_find_name(fdt, "chosen", &ac, &sc);
	const char *args = ch >= 0 ? fdt_prop(fdt, ch, "bootargs", &len) : NULL;
	if (args) {
		word(args, len, "kolibri.lifetime=", &lifetime);
		/* a power test ends with a reset into the boot menu (as a
		 * lifetime), half a minute after its last line */
		if (word(args, len, "kolibri.powertest", NULL)) {
			word(args, len, "kolibri.powertest=", &powertest);
			if (!powertest) powertest = 10;
			if (!lifetime) lifetime = powertest * 60 + 30;
		}
		noinput = word(args, len, "kolibri.noinput", NULL);
		nomodem = word(args, len, "kolibri.nomodem", NULL);
		fakemodem = word(args, len, "kolibri.fakemodem", NULL);
		callaudio = word(args, len, "kolibri.callaudio", NULL);
		wifi = word(args, len, "kolibri.wifi", NULL);
		wifi_irq = word(args, len, "kolibri.wifi=irq", NULL);
		rpmsleep = word(args, len, "kolibri.rpmsleep", NULL);
		displayon = word(args, len, "kolibri.displayon", NULL);
		dsiulps = word(args, len, "kolibri.dsiulps", NULL);
		battery_arg = word(args, len, "kolibri.battery", NULL);
		nopowerkey = word(args, len, "kolibri.nopowerkey", NULL);
		word(args, len, "kolibri.keytest=", &keytest);
		adopt_ipcrtr = word(args, len, "kolibri.ipcrtr=adopt", NULL);
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
