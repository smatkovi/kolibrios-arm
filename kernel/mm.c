/* KolibriOS for ARM: physical memory. Tasks and frame buffers get whole
 * MiB blocks (they are mapped with 1 MiB sections); small kernel objects
 * come from a heap that is never freed. */
#include "kernel.h"

#define MB (1u << 20)
#define HEAP_MB 8
#define MAX_BLOCKS 1024       /* 1 GiB */

static u32 ram_base, nblocks;
static u8 used[MAX_BLOCKS];
static u32 heap_ptr, heap_end;

void mm_init(u32 base, u32 size, u32 kernel_end)
{
	ram_base = base;
	nblocks = MIN(size / MB, MAX_BLOCKS);
	u32 first = ALIGN_UP(kernel_end - base, MB) / MB;
	for (u32 i = 0; i < first + HEAP_MB && i < nblocks; i++)
		used[i] = 1;
	heap_ptr = base + first * MB;
	heap_end = heap_ptr + HEAP_MB * MB;
}

void *kmalloc_aligned(u32 size, u32 align)
{
	u32 p = ALIGN_UP(heap_ptr, align);
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

u32 mm_alloc_mb(u32 mb)
{
	for (u32 i = 0; i + mb <= nblocks; i++) {
		u32 j;
		for (j = 0; j < mb && !used[i + j]; j++)
			;
		if (j == mb) {
			for (j = 0; j < mb; j++) used[i + j] = 1;
			return ram_base + i * MB;
		}
		i += j;
	}
	return 0;
}

void mm_free_mb(u32 pa, u32 mb)
{
	u32 i = (pa - ram_base) / MB;
	for (u32 j = 0; j < mb && i + j < nblocks; j++)
		used[i + j] = 0;
}

void mm_reserve(u32 pa, u32 size)
{
	if (pa < ram_base) return;
	for (u32 a = pa & ~(MB - 1); a < pa + size; a += MB) {
		u32 i = (a - ram_base) / MB;
		if (i < nblocks) used[i] = 1;
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
