/* KolibriOS for ARM: the MMU (ARMv7 short descriptors, 1 MiB sections).
 *
 * TTBCR.N = 2 splits the address space: below 1 GiB is the running task's
 * (TTBR0, a table of its own, its memory from VA 0 as KolibriOS programs
 * expect), above it the kernel's (TTBR1): RAM mapped 1:1 (both platforms
 * have it above 1 GiB) and the devices, those below 1 GiB moved up. */
#include "kernel.h"

#define SECTION 0x2
#define AP_KERNEL (1u << 10)            /* PL1 read/write, PL0 none */
#define AP_USER (3u << 10)              /* both read/write */
#define XN (1u << 4)
#define A_NORMAL ((1u << 12) | (1u << 3) | (1u << 2)) /* write-back, write-allocate */
#define A_UNCACHED (1u << 12)                         /* normal, not cached */
#define A_DEVICE (1u << 2)                            /* shareable device */

static u32 kl1[4096] __attribute__((aligned(16384)));
static u32 null_l1[1024] __attribute__((aligned(4096)));
static u32 *free_tables[64];
static int nfree;

static u32 attr_of(u32 map)
{
	switch (map) {
	case MAP_DEVICE: return A_DEVICE | XN;
	case MAP_UNCACHED: return A_UNCACHED;
	default: return plat->cached_data ? A_NORMAL : A_UNCACHED;
	}
}

static void dcache_invalidate_all(void)
{
	/* by set/way, every level of the hierarchy (ARMv7 ARM B2.2.7) */
	u32 clidr;
	asm volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));
	u32 loc = (clidr >> 24) & 7;
	for (u32 level = 0; level < loc; level++) {
		u32 type = (clidr >> (level * 3)) & 7;
		if (type < 2) continue;
		asm volatile("mcr p15, 2, %0, c0, c0, 0; isb" :: "r"(level << 1));
		u32 ccsidr;
		asm volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
		u32 line = (ccsidr & 7) + 4;
		u32 ways = ((ccsidr >> 3) & 0x3ff);
		u32 sets = ((ccsidr >> 13) & 0x7fff);
		u32 wshift = __builtin_clz(ways ? ways : 1);
		for (int w = ways; w >= 0; w--)
			for (int s = sets; s >= 0; s--) {
				u32 v = (level << 1) | (s << line) | (ways ? (w << wshift) : 0);
				asm volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(v)); /* DCISW */
			}
	}
	asm volatile("dsb; isb");
}

static void tlb_flush(void)
{
	asm volatile("mcr p15, 0, %0, c8, c7, 0\n" /* TLBIALL */
		     "mcr p15, 0, %0, c7, c5, 6\n"   /* BPIALL */
		     "dsb\nisb" :: "r"(0) : "memory");
}

void mmu_init(void)
{
	u32 sctlr;
	memset(kl1, 0, sizeof(kl1));
	memset(null_l1, 0, sizeof(null_l1));
	for (u32 a = plat->ram_base; a - plat->ram_base < plat->ram_size; a += 1u << 20)
		kl1[a >> 20] = a | SECTION | AP_KERNEL | attr_of(MAP_NORMAL);
	for (int i = 0; i < plat->nio; i++) {
		const struct mem_region *r = &plat->io[i];
		for (u32 o = 0; o < r->size; o += 1u << 20)
			kl1[(r->va + o) >> 20] = ((r->pa + o) & 0xfff00000) | SECTION | AP_KERNEL | attr_of(r->attr);
	}
	asm volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(2));        /* TTBCR.N = 2 */
	asm volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"((u32)null_l1)); /* TTBR0 */
	asm volatile("mcr p15, 0, %0, c2, c0, 1" :: "r"((u32)kl1));     /* TTBR1 */
	asm volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(0x55555555)); /* all domains: client */
	if (plat->cached_data)
		dcache_invalidate_all();
	asm volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0));         /* ICIALLU */
	tlb_flush();
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	sctlr |= 1u << 0 | 1u << 11 | 1u << 12;                       /* M, Z, I */
	if (plat->cached_data)
		sctlr |= 1u << 2;                                       /* C */
	sctlr &= ~(1u << 1);                                            /* no alignment faults */
	asm volatile("mcr p15, 0, %0, c1, c0, 0\nisb" :: "r"(sctlr));
}

u32 mmu_io_va(u32 pa)
{
	for (int i = 0; i < plat->nio; i++) {
		const struct mem_region *r = &plat->io[i];
		if (pa >= r->pa && pa - r->pa < r->size)
			return r->va + (pa - r->pa);
	}
	return pa;
}

/* a RAM range not cached (frame buffers the display reads) */
void mmu_set_uncached(u32 pa, u32 size)
{
	for (u32 o = 0; o < size; o += 1u << 20)
		kl1[(pa + o) >> 20] = ((pa + o) & 0xfff00000) | SECTION | AP_KERNEL | A_UNCACHED;
	tlb_flush();
}

u32 *mmu_new_user_table(void)
{
	u32 *t = nfree ? free_tables[--nfree] : kmalloc_aligned(4096, 4096);
	memset(t, 0, 4096);
	return t;
}

void mmu_free_user_table(u32 *l1)
{
	if (nfree < (int)ARRAY_SIZE(free_tables))
		free_tables[nfree++] = l1;
}

void mmu_map_user(u32 *l1, u32 va, u32 pa, u32 size)
{
	for (u32 o = 0; o < size; o += 1u << 20)
		l1[(va + o) >> 20] = ((pa + o) & 0xfff00000) | SECTION | AP_USER | attr_of(MAP_NORMAL);
}

void mmu_switch(u32 *l1)
{
	asm volatile("dsb\nmcr p15, 0, %0, c2, c0, 0" :: "r"((u32)(l1 ? l1 : null_l1)));
	tlb_flush();
}

/* after code was written (the loader): to the point of unification and
 * out of the instruction cache */
void cache_sync_code(u32 va, u32 len)
{
	if (plat->cached_data)
		for (u32 a = va & ~31u; a < va + len; a += 32)
			asm volatile("mcr p15, 0, %0, c7, c11, 1" :: "r"(a)); /* DCCMVAU */
	asm volatile("dsb\nmcr p15, 0, %0, c7, c5, 0\n" /* ICIALLU */
		     "mcr p15, 0, %0, c7, c5, 6\ndsb\nisb" :: "r"(0));
}
