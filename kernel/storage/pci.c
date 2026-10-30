/* KolibriOS for ARM, AArch64: PCI Express on QEMU's virt machine, as far
 * as the storage tests need it: the ECAM's bus 0, its devices' first
 * memory BAR placed in a window of the 32-bit memory range (no firmware
 * has done it: -kernel), memory decoding and bus mastering on. QEMU's
 * UFS controller (-device ufs) and SDHCI (-device sdhci-pci) sit there. */
#include "storage.h"
#include "pci.h"

static addr_t ecam;                     /* bus 0's configuration space (kernel address) */
static u64 win, win_end;                /* the BARs' window (physical) */

static u32 cfg_rd(u32 dev, u32 fn, u32 off) { return rd32(ecam + (dev << 15 | fn << 12 | off)); }
static void cfg_wr(u32 dev, u32 fn, u32 off, u32 v) { wr32(ecam + (dev << 15 | fn << 12 | off), v); }

void pci_init(addr_t ecam_va, u64 window, u64 size)
{
	ecam = ecam_va;
	win = window;
	win_end = window + size;
}

/* the first device of class (base class << 8 | subclass), its BAR0 given
 * an address; its physical address, 0 if there is none */
u64 pci_find(u32 class, u32 *bar_size)
{
	if (!ecam) return 0;
	for (u32 dev = 0; dev < 32; dev++) {
		u32 id = cfg_rd(dev, 0, 0);
		if ((id & 0xffff) == 0xffff) continue;
		u32 cl = cfg_rd(dev, 0, 8) >> 16;
		if (cl != class) continue;
		u32 b0 = cfg_rd(dev, 0, 0x10);
		if (b0 & 1) continue;                   /* an I/O BAR */
		bool b64 = ((b0 >> 1) & 3) == 2;
		cfg_wr(dev, 0, 4, cfg_rd(dev, 0, 4) & ~7u);   /* decoding off while sized */
		cfg_wr(dev, 0, 0x10, 0xffffffffu);
		u32 sz = ~(cfg_rd(dev, 0, 0x10) & ~0xfu) + 1;
		if (!sz || sz > (64u << 20)) continue;
		u64 at = (win + sz - 1) & ~(u64)(sz - 1);
		if (at + sz > win_end) {
			slog("pci: no room for %u KiB\n", sz >> 10);
			return 0;
		}
		win = at + sz;
		cfg_wr(dev, 0, 0x10, (u32)at | (b0 & 0xf));
		if (b64) cfg_wr(dev, 0, 0x14, (u32)(at >> 32));
		cfg_wr(dev, 0, 4, cfg_rd(dev, 0, 4) | 6);    /* memory, bus master */
		slog("pci: 00:%u.0 %x:%x class %x: BAR0 at %p (%u bytes)\n", dev, id & 0xffff, id >> 16, cl,
		     (addr_t)at, sz);
		if (bar_size) *bar_size = sz;
		return at;
	}
	return 0;
}
