/* KolibriOS for ARM, AArch64: the MMU (4 KiB granule, 39-bit addresses,
 * 2 MiB blocks).
 *
 * One table tree for each program, from TTBR0: its first GiB (L1 entry 0,
 * a level 2 table of its own) is the program's (EL0, from VA 0), the
 * rest the kernel's, shared by all: the RAM the kernel uses, mapped 1:1
 * (it lies above 1 GiB on both platforms), and the devices, whose
 * addresses lie below 1 GiB, moved up to DEV_ALIAS. Only what is used is
 * mapped (on the Pro1X, firmware's memory must not even be speculated). */
#include "kernel.h"

#define DEV_ALIAS 0x4000000000ul        /* 256 GiB */
#define GIB (1ul << 30)
#define BLOCK (2ul << 20)

#define D_TABLE 3ul
#define D_BLOCK 1ul
#define ATTR_DEV (0ul << 2)
#define ATTR_NORMAL (1ul << 2)
#define ATTR_NC (2ul << 2)
#define AP_EL1 (0ul << 6)
#define AP_EL0 (1ul << 6)
#define SH_INNER (3ul << 8)
#define AF (1ul << 10)
#define PXN (1ul << 53)
#define UXN (1ul << 54)

static u64 *kroot;                      /* the kernel's L1 table */
static u64 *free_roots[64];
static int nfree;

static u64 *new_table(void)
{
	return kmalloc_aligned(4096, 4096);
}

static u64 *l2_of(u64 *root, addr_t va)
{
	u64 *e = &root[(va >> 30) & 511];
	if (!(*e & 1)) *e = (u64)new_table() | D_TABLE;
	return (u64 *)(*e & 0xfffffffff000ul);
}

static void map(u64 *root, addr_t va, addr_t pa, u64 size, u64 attr)
{
	for (u64 o = 0; o < size; o += BLOCK) {
		u64 *l2 = l2_of(root, va + o);
		l2[((va + o) >> 21) & 511] = ((pa + o) & ~(BLOCK - 1)) | attr | AF | D_BLOCK;
	}
}

static u64 normal(void) { return ATTR_NORMAL | SH_INNER | AP_EL1 | UXN; }

static void tlb_flush(void)
{
	asm volatile("dsb ishst\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
}

/* by address, to the point of coherency: what was written with the MMU
 * (and so the caches) off must not be hidden by stale lines */
static void dcache_inval(addr_t a, u64 n)
{
	for (addr_t p = a & ~63ul; p < a + n; p += 64)
		asm volatile("dc ivac, %0" :: "r"(p) : "memory");
	asm volatile("dsb sy");
}

void mmu_init(void)
{
	kroot = new_table();
	/* the kernel's RAM, in 2 MiB blocks */
	addr_t ram = plat->ram_base & ~(BLOCK - 1);
	map(kroot, ram, ram, ALIGN_UP(plat->ram_base + plat->ram_size - ram, BLOCK), normal());
	for (int i = 0; i < plat->nio; i++) {
		const struct mem_region *r = &plat->io[i];
		u64 attr = r->attr == MAP_DEVICE ? (ATTR_DEV | AP_EL1 | PXN | UXN) :
			   r->attr == MAP_UNCACHED ? (ATTR_NC | SH_INNER | AP_EL1 | UXN) : normal();
		addr_t pa = r->pa & ~(BLOCK - 1);
		addr_t va = r->attr == MAP_DEVICE ? mmu_io_va(pa) : pa;
		map(kroot, va, pa, ALIGN_UP(r->pa + r->size - pa, BLOCK), attr);
	}
	u64 parange = SYSREG_R(id_aa64mmfr0_el1) & 7;
	SYSREG_W(mair_el1, 0x0000000000044ff00ul);      /* 0 device, 1 normal WB, 2 normal NC */
	SYSREG_W(tcr_el1, 25ul | (1ul << 8) | (1ul << 10) | (3ul << 12) | (1ul << 23) | (parange << 32));
	SYSREG_W(ttbr0_el1, (u64)kroot);
	asm volatile("isb");
	tlb_flush();
	extern u8 __kernel_end[];
	dcache_inval(plat->ram_base, (addr_t)__kernel_end - plat->ram_base + (16u << 20));
	asm volatile("ic iallu\ndsb nsh\nisb");
	u64 sctlr = SYSREG_R(sctlr_el1);
	sctlr |= 1ul << 0 | 1ul << 2 | 1ul << 12;       /* M, C, I */
	sctlr &= ~(1ul << 1 | 1ul << 3 | 1ul << 4 | 1ul << 19); /* no alignment, SP checks, WXN */
	sctlr |= 1ul << 5;                               /* AArch32 EL0's CP15 barriers */
	SYSREG_W(sctlr_el1, sctlr);
	asm volatile("isb");
}

addr_t mmu_io_va(addr_t pa)
{
	return pa < GIB ? DEV_ALIAS + pa : pa;
}

void mmu_set_uncached(addr_t pa, u32 size)
{
	map(kroot, pa, pa, ALIGN_UP(size, BLOCK), ATTR_NC | SH_INNER | AP_EL1 | UXN);
	tlb_flush();
}

void *mmu_new_user_table(void)
{
	u64 *root = nfree ? free_roots[--nfree] : NULL;
	if (!root) {
		root = new_table();
		root[0] = (u64)new_table() | D_TABLE;
	}
	u64 *l2 = (u64 *)(root[0] & 0xfffffffff000ul);
	memset(l2, 0, 4096);
	for (int i = 1; i < 512; i++) root[i] = kroot[i];
	return root;
}

void mmu_free_user_table(void *root)
{
	if (nfree < (int)ARRAY_SIZE(free_roots))
		free_roots[nfree++] = root;
}

void mmu_map_user(void *root, addr_t va, addr_t pa, u32 size)
{
	u64 *l2 = (u64 *)(((u64 *)root)[0] & 0xfffffffff000ul);
	for (u64 o = 0; o < size; o += BLOCK)
		l2[((va + o) >> 21) & 511] = ((pa + o) & ~(BLOCK - 1)) | ATTR_NORMAL | SH_INNER |
					     AP_EL0 | PXN | AF | D_BLOCK;
}

void mmu_switch(void *root)
{
	SYSREG_W(ttbr0_el1, (u64)(root ? root : kroot));
	asm volatile("isb");
	tlb_flush();
}

void cache_sync_code(addr_t va, u32 len)
{
	for (addr_t p = va & ~63ul; p < va + len; p += 64)
		asm volatile("dc cvau, %0" :: "r"(p));
	asm volatile("dsb ish\nic iallu\ndsb ish\nisb");
}
