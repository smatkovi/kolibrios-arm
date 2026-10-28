/* KolibriOS for ARM: starting programs. The header is KolibriOS's
 * MENUET01 header with ARM code, so its magic says so:
 *
 *   "MENUETA1", u32 version (1), entry, end of the image, memory size,
 *   initial stack pointer, parameters buffer (or 0), path buffer (or 0)
 *
 * The program gets its memory from address 0 (a whole number of MiB), the
 * file at the start, the rest zero. */
#include "gui.h"

struct kex_header {
	char magic[8];
	u32 version, entry, img_end, mem, stack, params, path;
};

int load_program(const char *path, const char *params, struct task **out)
{
	const struct rd_file *f = fs_find(path);
	if (!f) return -5;                         /* file not found */
	const struct kex_header *h = (const void *)f->data;
	if (f->size < sizeof(*h) || memcmp(h->magic, "MENUETA1", 8)) {
		kprintf("%s: not an ARM KolibriOS program\n", path);
		return -31;                        /* not an executable */
	}
	u32 size = ALIGN_UP(MAX(MAX(h->mem, h->img_end), f->size), 1u << 20);
	if (h->entry >= size || h->stack > size) return -31;
	struct task *t = task_alloc();
	if (!t) return -30;                        /* no slot */
	u32 pa = mm_alloc_mb(size >> 20);
	if (!pa) return -30;                       /* no memory */
	memset((void *)pa, 0, size);
	memcpy((void *)pa, f->data, f->size);
	t->mem_pa = pa;
	t->mem_size = size;
	t->img_end = h->img_end;
	t->l1 = mmu_new_user_table();
	mmu_map_user(t->l1, 0, pa, size);
	cache_sync_code(pa, f->size);
	if (h->params && params && h->params < size - 256)
		strncpy0((char *)(pa + h->params), params, 256);
	if (h->path && h->path < size - 1024)
		strncpy0((char *)(pa + h->path), path, 1024);
	/* the name function 9 reports: the file name, without a path */
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/') base = p + 1;
	strncpy0(t->name, base, sizeof(t->name));
	memset(&t->tf, 0, sizeof(t->tf));
	t->tf.pc = h->entry;
	t->tf.usr_sp = h->stack ? h->stack : size;
	t->tf.cpsr = PSR_USR | PSR_F;              /* ARM state, interrupts on */
	t->heap_start = t->heap_brk = ALIGN_UP(h->img_end, 4096);
	t->heap_end = (t->tf.usr_sp > 65536 ? t->tf.usr_sp - 65536 : t->tf.usr_sp) & ~15u;
	t->events = 1u << (EV_REDRAW - 1);         /* the first event, as in KolibriOS */
	t->state = T_READY;
	kprintf("started %s: pid %u, slot %d, %u KiB\n", t->name, t->pid, t->slot, size >> 10);
	if (out) *out = t;
	return t->pid;
}
