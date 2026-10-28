/* KolibriOS for ARM, AArch64: QEMU's virt machine, the development
 * platform of the AArch64 kernel (the Pro1X's CPU cores run the same
 * code):
 *
 *   qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -m 512 \
 *       -device ramfb -kernel build/arm64/kernel.bin
 *
 * PL011 for the debug output, GICv3 and the virtual timer, ramfb (set up
 * through fw_cfg) as the display, PL031 as the clock. No input. */
#include "kernel.h"
#include "fdt.h"

#define UART 0x09000000ul
#define RTC 0x09010000ul
#define FWCFG 0x09020000ul
#define GICD 0x08000000ul
#define GICR 0x080a0000ul

void gic3_init(addr_t dist_pa, addr_t redist_pa, void (*irq)(u32 id));
void gic3_irq(void);

static addr_t io(addr_t pa)
{
	return (SYSREG_R(sctlr_el1) & 1) ? mmu_io_va(pa) : pa;
}

static void v_putc(char c)
{
	addr_t u = io(UART);
	while (rd32(u + 0x18) & (1 << 5))
		;
	wr32(u, (u8)c);
}

static u64 gicd = GICD, gicr = GICR;

static void v_init(void)
{
	gic3_init(gicd, gicr, NULL);
}

static void v_irq(void)
{
	gic3_irq();
}

/* fw_cfg: big-endian registers; the DMA interface writes ramfb's setup */
static u16 be16(u16 v) { return v << 8 | v >> 8; }
static u32 be32(u32 v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }
static u64 be64(u64 v) { return (u64)be32(v) << 32 | be32(v >> 32); }

static u16 fwcfg_find(const char *name)
{
	addr_t f = io(FWCFG);
	*(volatile u16 *)(f + 8) = be16(0x19);           /* the file directory */
	u32 n = 0;
	for (int i = 0; i < 4; i++) n = n << 8 | rd8(f);
	for (u32 i = 0; i < n; i++) {
		u8 e[64];
		for (int j = 0; j < 64; j++) e[j] = rd8(f);
		if (!strcmp((const char *)e + 8, name)) return e[4] << 8 | e[5];
	}
	return 0;
}

struct ramfb_cfg { u64 addr; u32 fourcc, flags, width, height, stride; } __attribute__((packed));
struct fwcfg_dma { u32 control, length; u64 address; } __attribute__((packed));

#define FB_W 800
#define FB_H 480

static int v_display_init(u32 **fb, int *w, int *h, int *stride)
{
	u16 sel = fwcfg_find("etc/ramfb");
	if (!sel) {
		kprintf("virt: no ramfb (-device ramfb)\n");
		return -1;
	}
	addr_t pa = mm_alloc_mb_aligned(2, 2);
	if (!pa) return -1;
	mmu_set_uncached(pa, 2u << 20);
	static struct ramfb_cfg cfg __attribute__((aligned(16)));
	static struct fwcfg_dma dma __attribute__((aligned(16)));
	cfg.addr = be64(pa);
	cfg.fourcc = be32(0x34325258);                    /* XR24: 0x00RRGGBB */
	cfg.flags = 0;
	cfg.width = be32(FB_W);
	cfg.height = be32(FB_H);
	cfg.stride = be32(FB_W * 4);
	dma.control = be32((u32)sel << 16 | 0x08 | 0x10); /* select, write */
	dma.length = be32(sizeof(cfg));
	dma.address = be64((u64)&cfg);
	asm volatile("dsb sy");
	addr_t f = io(FWCFG);
	u64 a = (u64)&dma;
	wr32(f + 0x10, be32(a >> 32));
	wr32(f + 0x14, be32((u32)a));
	for (int i = 0; i < 1000000 && (be32(*(volatile u32 *)&dma.control) & ~1u); i++)
		;
	*fb = (u32 *)pa;
	*w = FB_W;
	*h = FB_H;
	*stride = FB_W;
	return 0;
}

static bool v_display_flush(int y0, int y1)
{
	(void)y0; (void)y1;
	return true;
}

static u32 v_rtc_time(void)
{
	return rd32(io(RTC));
}

static const struct mem_region v_io[] = {
	{ 0, UART, 0x30000, MAP_DEVICE },        /* UART, RTC, fw_cfg */
	{ 0, GICD, 0x200000, MAP_DEVICE },       /* distributor and redistributors */
};

struct platform platform_virt = {
	.name = "QEMU virt (AArch64)",
	.io = v_io,
	.nio = ARRAY_SIZE(v_io),
	.init = v_init,
	.putc = v_putc,
	.irq = v_irq,
	.display_init = v_display_init,
	.display_flush = v_display_flush,
	.rtc_time = v_rtc_time,
	.cached_data = true,
};

extern u8 __kernel_end[];
extern addr_t boot_args[3];

/* RAM: from the kernel's 2 MiB block on, 256 MiB, within /memory */
bool virt_probe(const void *fdt)
{
	addr_t base = boot_args[1] & ~((2ul << 20) - 1);
	u64 mem_end = base + (512ul << 20);
	int ac, sc;
	int m = fdt_find_name(fdt, "memory", &ac, &sc);
	u64 a, s;
	if (m >= 0 && fdt_reg(fdt, m, ac, sc, 0, &a, &s)) mem_end = a + s;
	int g = fdt_find_compatible(fdt, "arm,gic-v3", -1, &ac, &sc);
	if (g >= 0) {
		fdt_reg(fdt, g, ac, sc, 0, &gicd, NULL);
		fdt_reg(fdt, g, ac, sc, 1, &gicr, NULL);
	}
	platform_virt.ram_base = base;
	platform_virt.ram_size = MIN(256ul << 20, mem_end - base);
	return true;
}
