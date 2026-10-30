/* KolibriOS for ARM, AArch64: QEMU's virt machine, the development
 * platform of the AArch64 kernel (the Pro1X's CPU cores run the same
 * code):
 *
 *   qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -m 512 \
 *       -device ramfb -kernel build/arm64/kernel.bin
 *
 * PL011 for the debug output, GICv3 and the virtual timer, ramfb (set up
 * through fw_cfg) as the display, PL031 as the clock, a virtio-net card
 * on virtio-mmio as the network (make run-net), virtio-input as the
 * keyboard and the pointer (-device virtio-keyboard-device,
 * virtio-tablet-device: tests type through the monitor's sendkey);
 * storage (tests/storage): virtio-blk disks (-device virtio-blk-device),
 * and on the PCI Express bus QEMU's UFS controller (-device ufs, ufs-lu;
 * kolibri.ufstest=keeplink: first left running as Linux leaves the
 * Pro1X's, then taken over as there) and SDHCI (-device sdhci-pci,
 * -device emmc: the N950's eMMC protocol). */
#include "kernel.h"
#include "fdt.h"
#include "net.h"
#include "../storage/storage.h"
#include "../storage/ufs.h"
#include "../storage/pci.h"
#include "../storage/sdhci.h"

#define UART 0x09000000ul
#define RTC 0x09010000ul
#define FWCFG 0x09020000ul
#define GICD 0x08000000ul
#define GICR 0x080a0000ul
#define VIRTIO 0x0a000000ul             /* 32 virtio-mmio slots of 0x200 */

void gic3_init(addr_t dist_pa, addr_t redist_pa, void (*irq)(u32 id));
void gic3_irq(void);
void gic3_enable_spi(u32 id);
bool virtio_net_probe(addr_t va);
bool virtio_rng_probe(addr_t va);
bool virtio_input_probe(addr_t va);
bool virtio_blk_probe(addr_t va);
bool storage_off(void);
int storage_home_mode(void);
bool storage_dma(struct dma_mem *m, u32 mb, u64 limit);
const char *boot_arg(const char *name);
void virtio_net_irq(void);
void virtio_input_irq(void);

/* the virtio-mmio slots of the device tree (read before the MMU is on) */
static struct { addr_t pa; u32 irq; } vslot[32];
static int nvslot;
static u32 net_irq, input_irq[2];
static u64 ecam_pa;                     /* PCI Express's configuration space */
/* the BARs: in its 32-bit window, not at its start: devices below 1 GiB
 * are mapped at 256 GiB up (mmu.c's DEV_ALIAS), where 0x10000000 would
 * meet the ECAM's own address (0x4010000000) */
#define PCI_WIN 0x20000000ul
#define PCI_WIN_SIZE (16ul << 20)

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

static void v_other_irq(u32 id)
{
	if (id == net_irq) virtio_net_irq();
	if (id == input_irq[0] || id == input_irq[1]) virtio_input_irq();
}

/* QEMU's UFS controller on the PCI Express bus */
static addr_t ufs_regs;
static u32 ufs_rd(void *c, u32 off) { (void)c; return rd32(ufs_regs + off); }
static void ufs_wr(void *c, u32 off, u32 v) { (void)c; wr32(ufs_regs + off, v); }
static u64 now_us(void)
{
	u64 c = SYSREG_R(cntvct_el0), f = SYSREG_R(cntfrq_el0);
	return c / f * 1000000 + c % f * 1000000 / f;     /* no overflow after days up */
}
static void dsb(void) { asm volatile("dsb sy" ::: "memory"); }

static void v_storage(void)
{
	if (!ecam_pa) {
		kprintf("virt: no PCI Express in the device tree\n");
		return;
	}
	pci_init(mmu_io_va(ecam_pa), PCI_WIN, PCI_WIN_SIZE);
	u32 size;
	bool rw = storage_home_mode() == HOME_RW;
	u64 sd = pci_find(PCI_CLASS_SDHCI, &size);
	if (sd) {
		static struct sdhci hs;
		hs.base = mmu_io_va(sd);
		hs.now_us = now_us;
		sdhci_start(&hs, "mmc0", rw);
	}
	u64 bar = pci_find(PCI_CLASS_UFS, &size);
	if (!bar) return;
	static struct ufs_hw hw = { .rd = ufs_rd, .wr = ufs_wr, .now_us = now_us, .barrier = dsb };
	ufs_regs = mmu_io_va(bar);
	if (!storage_dma(&hw.dma, 2, 1ull << 32)) return;
	hw.writable = rw;
	const char *t = boot_arg("kolibri.ufstest");
	if (t && !strncmp(t, "keeplink", 8)) hw.keep_link = ufs_leave_running(&hw);
	ufs_start(&hw);
}

static void v_init(void)
{
	gic3_init(gicd, gicr, v_other_irq);
	net_init();
	bool storage = !storage_off();
	for (int i = 0; i < nvslot; i++) {
		addr_t va = mmu_io_va(vslot[i].pa);
		if (virtio_rng_probe(va)) continue;
		if (storage && virtio_blk_probe(va)) continue;
		if (virtio_input_probe(va)) {
			input_irq[input_irq[0] ? 1 : 0] = vslot[i].irq;
			gic3_enable_spi(vslot[i].irq);
			continue;
		}
		if (net_irq || !virtio_net_probe(va)) continue;
		net_irq = vslot[i].irq;
		gic3_enable_spi(net_irq);
	}
	if (storage) v_storage();
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
	{ 0, VIRTIO, 0x4000, MAP_DEVICE },       /* virtio-mmio */
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

/* the RAM: /memory without the device tree (QEMU puts it at the start),
 * the kernel from its 2 MiB block on */
bool virt_probe(const void *fdt)
{
	addr_t base = boot_args[1] & ~((2ul << 20) - 1);
	int ac, sc;
	int n = fdt_ok(fdt) ? fdt_usable_ram(fdt, platform_virt.bank, MAX_BANKS, 2ul << 20) : 0;
	if (!n) {
		platform_virt.bank[0].base = base;
		platform_virt.bank[0].size = 256ul << 20;
		n = 1;
	}
	platform_virt.nbank = n;
	int g = fdt_find_compatible(fdt, "arm,gic-v3", -1, &ac, &sc);
	if (g >= 0) {
		fdt_reg(fdt, g, ac, sc, 0, &gicd, NULL);
		fdt_reg(fdt, g, ac, sc, 1, &gicr, NULL);
	}
	for (int v = fdt_find_compatible(fdt, "virtio,mmio", -1, &ac, &sc);
	     v >= 0 && nvslot < (int)ARRAY_SIZE(vslot);
	     v = fdt_find_compatible(fdt, "virtio,mmio", v, &ac, &sc)) {
		u64 pa;
		int len;
		const u32 *irq = fdt_prop(fdt, v, "interrupts", &len);
		if (!fdt_reg(fdt, v, ac, sc, 0, &pa, NULL) || pa < VIRTIO || pa >= VIRTIO + 0x4000 ||
		    !irq || len < 12 || fdt_cells(irq, 1) != 0)
			continue;
		vslot[nvslot].pa = pa;
		vslot[nvslot++].irq = 32 + (u32)fdt_cells(irq + 1, 1);    /* an SPI */
	}
	/* PCI Express: bus 0's configuration space and the BARs' window
	 * mapped (storage tests) */
	int pc = fdt_find_compatible(fdt, "pci-host-ecam-generic", -1, &ac, &sc);
	u64 epa;
	if (pc >= 0 && fdt_reg(fdt, pc, ac, sc, 0, &epa, NULL)) {
		ecam_pa = epa;
		mmu_add_io(epa, 1u << 20);
		mmu_add_io(PCI_WIN, PCI_WIN_SIZE);
	}
	platform_virt.ram_base = base;
	return true;
}
