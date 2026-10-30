/* KolibriOS for ARM, AArch64: the MMU (4 KiB granule, 39-bit addresses,
 * 2 MiB blocks).
 *
 * One table tree for each program, from TTBR0: its first GiB (L1 entry 0,
 * a level 2 table of its own) is the program's (EL0, from VA 0), the
 * rest the kernel's, shared by all: the RAM, mapped 1:1 (it lies above
 * 1 GiB on both platforms; the platform's banks), and the devices, whose
 * addresses lie below 1 GiB, moved up to DEV_ALIAS. Only what is used is
 * mapped (on the Pro1X, firmware's memory must not even be speculated).
 *
 * The programs' mappings are not global (nG): each tree has an address
 * space number (ASID, from the root's address; 0 is the kernel's own
 * tree), so that a task switch only loads TTBR0 and the TLB keeps the
 * kernel's entries and other programs'. An ASID's entries are thrown
 * away when it serves another tree than before, or its tree was made
 * anew (mmu_asid_forget). */
#include "kernel.h"

#define DEV_ALIAS 0x4000000000ul        /* 256 GiB */
#define GIB (1ul << 30)
#define BLOCK (2ul << 20)

#define D_TABLE 3ul
#define D_BLOCK 1ul
#define ATTR_DEV (0ul << 2)             /* MAIR: Device-nGnRnE */
#define ATTR_NORMAL (1ul << 2)          /* write-back */
#define ATTR_NC (2ul << 2)              /* normal, not cached */
#define ATTR_DEV_GRE (3ul << 2)         /* Device-GRE: gathering, no speculation */
#define AP_EL1 (0ul << 6)
#define AP_EL0 (1ul << 6)
#define AP_EL0_RO (3ul << 6)
#define SH_INNER (3ul << 8)
#define AF (1ul << 10)
#define NG (1ul << 11)
#define PXN (1ul << 53)
#define UXN (1ul << 54)

static u64 *kroot;                      /* the kernel's L1 table */
static struct mem_region extra_io[8];   /* the architecture's devices (timer.c) */
static int nextra;
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

/* the tree each ASID last served (1-255; 0: the kernel's, no entries
 * of its own) */
static void *asid_tree[256];

static u64 asid_of(const void *root)
{
	return ((addr_t)root >> 12) % 255 + 1;
}

/* a tree made anew or given up: what the TLB holds for its ASID goes at
 * its next switch */
void mmu_asid_forget(const void *root)
{
	if (asid_tree[asid_of(root)] == root) asid_tree[asid_of(root)] = NULL;
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
	/* the RAM, in 2 MiB blocks: the banks (all of them 2 MiB aligned),
	 * or the kernel's window alone */
	if (plat->nbank) {
		for (int i = 0; i < plat->nbank; i++)
			map(kroot, plat->bank[i].base, plat->bank[i].base, plat->bank[i].size, normal());
	} else {
		addr_t ram = plat->ram_base & ~(BLOCK - 1);
		map(kroot, ram, ram, ALIGN_UP(plat->ram_base + plat->ram_size - ram, BLOCK), normal());
	}
	for (int i = 0; i < plat->nio + nextra; i++) {
		const struct mem_region *r = i < plat->nio ? &plat->io[i] : &extra_io[i - plat->nio];
		u64 attr = r->attr == MAP_DEVICE ? (ATTR_DEV | AP_EL1 | PXN | UXN) :
			   r->attr == MAP_UNCACHED ? (ATTR_DEV_GRE | AP_EL1 | PXN | UXN) : normal();
		addr_t pa = r->pa & ~(BLOCK - 1);
		addr_t va = r->attr == MAP_DEVICE ? mmu_io_va(pa) : pa;
		map(kroot, va, pa, ALIGN_UP(r->pa + r->size - pa, BLOCK), attr);
	}
	u64 parange = SYSREG_R(id_aa64mmfr0_el1) & 7;
	SYSREG_W(mair_el1, 0x0c44ff00ul);    /* 0 device, 1 normal WB, 2 normal NC, 3 device GRE */
	SYSREG_W(tcr_el1, 25ul | (1ul << 8) | (1ul << 10) | (3ul << 12) | (1ul << 23) | (parange << 32));
	SYSREG_W(ttbr0_el1, (u64)kroot);
	asm volatile("isb");
	tlb_flush();
	extern u8 __kernel_end[];
	dcache_inval(plat->ram_base, (addr_t)__kernel_end - plat->ram_base + ((HEAP_MB + 1u) << 20));
	asm volatile("ic iallu\ndsb nsh\nisb");
	u64 sctlr = SYSREG_R(sctlr_el1);
	sctlr |= 1ul << 0 | 1ul << 2 | 1ul << 12;       /* M, C, I */
	sctlr &= ~(1ul << 1 | 1ul << 3 | 1ul << 4 | 1ul << 19); /* no alignment, SP checks, WXN */
	sctlr |= 1ul << 5;                               /* AArch32 EL0's CP15 barriers */
	SYSREG_W(sctlr_el1, sctlr);
	asm volatile("isb");
}

void mmu_add_io(addr_t pa, u32 size)
{
	if (nextra < (int)ARRAY_SIZE(extra_io))
		extra_io[nextra++] = (struct mem_region){ 0, pa, size, MAP_DEVICE };
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
	mmu_asid_forget(root);
	return root;
}

/* the kernel's L1 table, for the Linux personality's tables */
const u64 *mmu_kernel_table(void)
{
	return kroot;
}

void mmu_free_user_table(void *root)
{
	mmu_asid_forget(root);
	if (nfree < (int)ARRAY_SIZE(free_roots))
		free_roots[nfree++] = root;
}

/* what the TLB holds for the tree's ASID away now (the tree may be the
 * running one: another thread of the process changed it) */
static void asid_flush(const void *root)
{
	u64 asid = asid_of(root);
	asm volatile("dsb ishst" ::: "memory");
	if (asid_tree[asid] == root) asm volatile("tlbi aside1, %0" :: "r"(asid << 48) : "memory");
	asm volatile("dsb ish\nisb" ::: "memory");
}

/* ro: EL0 and EL1 read only (AP 11); the kernel reaches the memory
 * through its own mapping of the RAM */
void mmu_map_user_ro(void *root, addr_t va, addr_t pa, u32 size, bool ro)
{
	u64 *l2 = (u64 *)(((u64 *)root)[0] & 0xfffffffff000ul);
	for (u64 o = 0; o < size; o += BLOCK)
		l2[((va + o) >> 21) & 511] = ((pa + o) & ~(BLOCK - 1)) | ATTR_NORMAL | SH_INNER |
					     (ro ? AP_EL0_RO : AP_EL0) | PXN | AF | NG | D_BLOCK;
	asid_flush(root);
}

void mmu_map_user(void *root, addr_t va, addr_t pa, u32 size)
{
	u64 *l2 = (u64 *)(((u64 *)root)[0] & 0xfffffffff000ul);
	for (u64 o = 0; o < size; o += BLOCK)
		l2[((va + o) >> 21) & 511] = ((pa + o) & ~(BLOCK - 1)) | ATTR_NORMAL | SH_INNER |
					     AP_EL0 | PXN | AF | NG | D_BLOCK;
	mmu_asid_forget(root);
}

void mmu_unmap_user(void *root, addr_t va, u32 size)
{
	u64 *l2 = (u64 *)(((u64 *)root)[0] & 0xfffffffff000ul);
	for (u64 o = 0; o < size; o += BLOCK)
		l2[((va + o) >> 21) & 511] = 0;
	asid_flush(root);
}

void mmu_switch(void *root)
{
	if (!root) {
		SYSREG_W(ttbr0_el1, (u64)kroot);
		asm volatile("isb");
		return;
	}
	u64 asid = asid_of(root);
	if (asid_tree[asid] != root) {
		/* its entries of another tree away first, then the switch */
		asm volatile("dsb ishst\ntlbi aside1, %0\ndsb ish" :: "r"(asid << 48) : "memory");
		asid_tree[asid] = root;
	}
	SYSREG_W(ttbr0_el1, (u64)root | asid << 48);
	asm volatile("isb");
}

void cache_sync_code(addr_t va, u32 len)
{
	for (addr_t p = va & ~63ul; p < va + len; p += 64)
		asm volatile("dc cvau, %0" :: "r"(p));
	asm volatile("dsb ish\nic iallu\ndsb ish\nisb");
}
