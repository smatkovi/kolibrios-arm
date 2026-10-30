/* KolibriOS for ARM: physical memory. Tasks and frame buffers get whole
 * MiB blocks (mapped with 1 MiB sections on ARMv7, 2 MiB blocks on
 * AArch64); small kernel objects come from a heap that is never freed.
 *
 * The RAM is the platform's banks (the Pro1X: 8 GiB in pieces between the
 * firmware's reserved memory), or its kernel window alone. One block map
 * for all of them; an allocation stays within one bank. */
#include "kernel.h"

#define MB (1u << 20)
#define MAX_BLOCKS 8192       /* 8 GiB */

static struct { addr_t base; u32 first, n; } banks[MAX_BANKS];
static int nbanks;
static u32 nblocks;
static u8 used[MAX_BLOCKS];
static addr_t heap_ptr, heap_end;

static void add_bank(addr_t base, u64 size)
{
	if (nbanks == MAX_BANKS || nblocks == MAX_BLOCKS) return;
	u32 n = MIN(size / MB, (u64)(MAX_BLOCKS - nblocks));
	if (!n) return;
	banks[nbanks].base = base;
	banks[nbanks].first = nblocks;
	banks[nbanks].n = n;
	nbanks++;
	nblocks += n;
}

/* the block of an address, or -1 */
static int block_of(addr_t pa)
{
	for (int b = 0; b < nbanks; b++)
		if (pa >= banks[b].base && (pa - banks[b].base) / MB < banks[b].n)
			return banks[b].first + (pa - banks[b].base) / MB;
	return -1;
}

void mm_init(addr_t kernel_end)
{
	if (plat->nbank)
		for (int i = 0; i < plat->nbank; i++)
			add_bank(plat->bank[i].base, plat->bank[i].size);
	else
		add_bank(plat->ram_base, plat->ram_size);
	/* the kernel and its heap, from ram_base on */
	heap_ptr = ALIGN_UP(kernel_end, (addr_t)MB);
	heap_end = heap_ptr + HEAP_MB * MB;
	int k0 = block_of(plat->ram_base), k1 = block_of(heap_end - 1);
	if (k0 < 0 || k1 < 0 || (addr_t)(k1 - k0) != (heap_end - 1 - plat->ram_base) / MB)
		panic("the kernel (%p-%p) is not in one bank of RAM", plat->ram_base, heap_end);
	mm_reserve(plat->ram_base, heap_end - plat->ram_base);
}

void *kmalloc_aligned(u32 size, u32 align)
{
	addr_t p = ALIGN_UP(heap_ptr, (addr_t)align);
	if (p + size > heap_end)
		panic("kernel heap full (%u bytes wanted)", size);
	heap_ptr = p + size;
	memset((void *)p, 0, size);
	return (void *)p;
}

void *kmalloc(u32 size)
{
	return kmalloc_aligned(size, 8);
}

/* from the top down: the memory near the kernel stays free the longest,
 * and the programs' memory lies above 4 GiB where there is some */
addr_t mm_alloc_mb_aligned(u32 mb, u32 align_mb)
{
	/* the address aligned to align_mb MiB */
	for (int b = nbanks - 1; b >= 0; b--) {
		for (s32 i = (s32)banks[b].n - (s32)mb; i >= 0; i--) {
			addr_t pa = banks[b].base + (addr_t)i * MB;
			if ((pa >> 20) % align_mb) continue;
			u32 j;
			for (j = 0; j < mb && !used[banks[b].first + i + j]; j++)
				;
			if (j == mb) {
				for (j = 0; j < mb; j++) used[banks[b].first + i + j] = 1;
				return pa;
			}
		}
	}
	return 0;
}

/* from the bottom up, the whole range below limit (DMA for a device that
 * reaches only so far) */
addr_t mm_alloc_mb_below(u32 mb, u32 align_mb, u64 limit)
{
	for (int b = 0; b < nbanks; b++)
		for (u32 i = 0; i + mb <= banks[b].n; i++) {
			addr_t pa = banks[b].base + (addr_t)i * MB;
			if ((u64)pa + (u64)mb * MB > limit) return 0;
			if ((pa >> 20) % align_mb) continue;
			u32 j;
			for (j = 0; j < mb && !used[banks[b].first + i + j]; j++)
				;
			if (j == mb) {
				for (j = 0; j < mb; j++) used[banks[b].first + i + j] = 1;
				return pa;
			}
		}
	return 0;
}

addr_t mm_alloc_mb(u32 mb)
{
	return mm_alloc_mb_aligned(mb, 1);
}

void mm_free_mb(addr_t pa, u32 mb)
{
	for (u32 j = 0; j < mb; j++) {
		int i = block_of(pa + (addr_t)j * MB);
		if (i >= 0) used[i] = 0;
	}
}

void mm_reserve(addr_t pa, u32 size)
{
	for (addr_t a = pa & ~(addr_t)(MB - 1); a < pa + size; a += MB) {
		int i = block_of(a);
		if (i >= 0) used[i] = 1;
	}
}

u32 mm_free_kb(void)
{
	u32 n = 0;
	for (u32 i = 0; i < nblocks; i++)
		if (!used[i]) n++;
	return n * 1024;
}

u32 mm_total_kb(void)
{
	return nblocks * 1024;
}
