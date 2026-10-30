/* KolibriOS for ARM: storage, the kernel's side: the command line's
 * switches, DMA memory for the drivers, the start after the platform's
 * drivers have added their disks (main.c).
 *
 *   kolibri.storage=off    no storage driver touches its controller
 *   kolibri.home=off       no container looked for: every volume read only
 *   kolibri.home=ro        (the default) the container checked and shown as
 *                          /hd0/1, read only: nothing at all is written
 *   kolibri.home=rw        the container writable, its blocks the only ones
 *                          the drivers write (the fence, blk.c)
 *   kolibri.mmcsleep=off   an eMMC never put to sleep (CMD5) when idle (mmc.c) */
#include "storage.h"
#include "mmc.h"

const char *boot_arg(const char *name);

bool storage_off(void)
{
	const char *a = boot_arg("kolibri.storage");
	return a && !strncmp(a, "off", 3);
}

bool storage_mmc_sleep(void)
{
	const char *a = boot_arg("kolibri.mmcsleep");
	return !a || strncmp(a, "off", 3);
}

/* the timer's tick (sched.c): the drivers' idle work (an eMMC's sleep);
 * the ticks until it is due, for the tickless idle */
void storage_tick(void)
{
	mmc_tick();
}

u32 storage_next(void)
{
	u32 ms = mmc_next_ms();
	return ms == 0xffffffffu ? ms : (ms + 9) / 10;
}

int storage_home_mode(void)
{
	const char *a = boot_arg("kolibri.home");
	if (a && !strncmp(a, "off", 3)) return HOME_OFF;
	if (a && !strncmp(a, "rw", 2)) return HOME_RW;
	return HOME_RO;
}

/* mb MiB of uncached memory for a controller's DMA, below limit */
bool storage_dma(struct dma_mem *m, u32 mb, u64 limit)
{
	addr_t pa = mm_alloc_mb_below(mb, 2, limit);
	if (!pa) return false;
#ifdef __aarch64__
	for (addr_t a = pa; a < pa + (addr_t)mb * (1u << 20); a += 64)
		asm volatile("dc civac, %0" :: "r"(a) : "memory");
	asm volatile("dsb sy");
#endif
	mmu_set_uncached(pa, mb << 20);
	m->va = (u8 *)pa;
	m->pa = pa;
	m->size = mb << 20;
	memset(m->va, 0, m->size);
	return true;
}

/* after the platform's init: the partitions, their file systems, the
 * container */
void storage_start(void)
{
	if (!blk_count()) return;
	blk_scan();
	home_init(storage_home_mode());
}
