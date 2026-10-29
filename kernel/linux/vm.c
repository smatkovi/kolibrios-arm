/* KolibriOS for ARM: the Linux personality's memory. 4 KiB pages from a
 * pool (1 MiB blocks of the kernel's allocator cut up), a three-level
 * table tree for each process (the kernel's L1 entries copied in), the
 * mappings as a sorted list of areas; a page is allocated, zeroed, when it
 * is first touched (an abort from EL0, or the kernel copying to or from the
 * process). */
#include "lx.h"

#define PAGE 4096ul
#define PTE_PAGE 3ul                    /* level 3: a page; levels 1, 2: a table */
#define PTE_NORMAL (1ul << 2)           /* MAIR index 1: write-back */
#define PTE_USER (1ul << 6)
#define PTE_RO (1ul << 7)
#define PTE_SH (3ul << 8)
#define PTE_AF (1ul << 10)
#define PTE_PXN (1ul << 53)
#define PTE_UXN (1ul << 54)
#define PTE_ADDR 0x0000fffffffff000ul

extern const u64 *mmu_kernel_table(void);

static addr_t free_list;
static u32 free_count;                  /* pages in the pool */

u32 lx_pages_free(void) { return free_count; }

addr_t lx_page_alloc(void)
{
	if (!free_list) {
		addr_t b = mm_alloc_mb(1);
		if (!b) return 0;
		for (addr_t p = b; p < b + (1u << 20); p += PAGE) {
			*(addr_t *)p = free_list;
			free_list = p;
			free_count++;
		}
	}
	addr_t p = free_list;
	free_list = *(addr_t *)p;
	free_count--;
	memset((void *)p, 0, PAGE);
	return p;
}

void lx_page_free(addr_t pa)
{
	*(addr_t *)pa = free_list;
	free_list = pa;
	free_count++;
}

static void tlb_flush_all(void)
{
	asm volatile("dsb ishst\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
}

bool lx_user_range(u64 va, u64 len)
{
	u64 end = va + len;
	if (end < va) return false;
	return end <= LX_LOW_END || (va >= LX_MAP_START && end <= LX_MAP_END);
}

static bool user_l1(int i)
{
	return i == 0 || (i >= (int)(LX_MAP_START >> 30) && i < (int)(LX_MAP_END >> 30));
}

struct lx_proc *lx_proc_new(void)
{
	addr_t m = mm_alloc_mb(1);
	if (!m) return NULL;
	memset((void *)m, 0, sizeof(struct lx_proc));
	struct lx_proc *p = (struct lx_proc *)m;
	p->root = (u64 *)lx_page_alloc();
	if (!p->root) {
		mm_free_mb(m, 1);
		return NULL;
	}
	const u64 *k = mmu_kernel_table();
	for (int i = 0; i < 512; i++)
		if (!user_l1(i)) p->root[i] = k[i];
	p->map_next = LX_MAP_START;
	strncpy0(p->cwd, "/", sizeof(p->cwd));
	return p;
}

/* the level 3 entry of a page, the tables made on the way if asked */
static u64 *pte_of(struct lx_proc *p, u64 va, bool make)
{
	u64 *t = p->root;
	for (int level = 1; level < 3; level++) {
		u64 *e = &t[(va >> (39 - 9 * level)) & 511];
		if (!(*e & 1)) {
			if (!make) return NULL;
			addr_t n = lx_page_alloc();
			if (!n) return NULL;
			*e = n | 3;
		}
		t = (u64 *)(*e & PTE_ADDR);
	}
	return &t[(va >> 12) & 511];
}

static u64 pte_bits(u32 prot)
{
	u64 b = PTE_PAGE | PTE_NORMAL | PTE_SH | PTE_AF | PTE_USER | PTE_PXN;
	if (!(prot & PROT_WRITE)) b |= PTE_RO;
	if (!(prot & PROT_EXEC)) b |= PTE_UXN;
	return b;
}

/* the pages of a range back to the pool (the areas stay) */
static void unmap_pages(struct lx_proc *p, u64 start, u64 end)
{
	bool any = false;
	for (u64 va = start; va < end; va += PAGE) {
		u64 *e = pte_of(p, va, false);
		if (!e) {
			/* no level 3 table here: on to the next 2 MiB */
			va = (va | ((2ul << 20) - 1)) + 1 - PAGE;
			continue;
		}
		if (*e & PTE_ADDR) {            /* present, or kept by mprotect */
			lx_page_free(*e & PTE_ADDR);
			*e = 0;
			any = true;
		}
	}
	if (any) tlb_flush_all();
}

void lx_proc_free_memory(struct lx_proc *p)
{
	for (int i = 0; i < 512; i++) {
		if (!user_l1(i) || !(p->root[i] & 1)) continue;
		u64 *l2 = (u64 *)(p->root[i] & PTE_ADDR);
		for (int j = 0; j < 512; j++) {
			if (!(l2[j] & 1)) continue;
			u64 *l3 = (u64 *)(l2[j] & PTE_ADDR);
			for (int k = 0; k < 512; k++)
				if (l3[k] & PTE_ADDR) lx_page_free(l3[k] & PTE_ADDR);
			lx_page_free((addr_t)l3);
		}
		lx_page_free((addr_t)l2);
	}
	lx_page_free((addr_t)p->root);
	p->root = NULL;
	mm_free_mb((addr_t)p, 1);
}

/* the areas: sorted, not overlapping; vma_clear cuts a range out */
static struct vma *vma_find(struct lx_proc *p, u64 va)
{
	int lo = 0, hi = p->nvma - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (va < p->vma[m].start) hi = m - 1;
		else if (va >= p->vma[m].end) lo = m + 1;
		else return &p->vma[m];
	}
	return NULL;
}

static bool vma_insert_at(struct lx_proc *p, int i, u64 start, u64 end, u32 prot)
{
	if (p->nvma >= LX_MAX_VMAS) return false;
	memmove(&p->vma[i + 1], &p->vma[i], (p->nvma - i) * sizeof(struct vma));
	p->vma[i] = (struct vma){ start, end, prot };
	p->nvma++;
	return true;
}

static bool vma_clear(struct lx_proc *p, u64 start, u64 end)
{
	for (int i = 0; i < p->nvma; i++) {
		struct vma *v = &p->vma[i];
		if (v->end <= start || v->start >= end) continue;
		if (v->start < start && v->end > end) {         /* split in two */
			if (!vma_insert_at(p, i + 1, end, v->end, v->prot)) return false;
			p->vma[i].end = start;
			return true;
		}
		if (v->start < start) { v->end = start; continue; }
		if (v->end > end) { v->start = end; continue; }
		memmove(v, v + 1, (p->nvma - i - 1) * sizeof(struct vma));
		p->nvma--;
		i--;
	}
	return true;
}

bool lx_vma_add(struct lx_proc *p, u64 start, u64 end, u32 prot)
{
	if (!vma_clear(p, start, end)) return false;
	int i = 0;
	while (i < p->nvma && p->vma[i].start < start) i++;
	/* joined to its neighbours when they are alike */
	if (i > 0 && p->vma[i - 1].end == start && p->vma[i - 1].prot == prot) {
		p->vma[i - 1].end = end;
		if (i < p->nvma && p->vma[i].start == end && p->vma[i].prot == prot) {
			p->vma[i - 1].end = p->vma[i].end;
			memmove(&p->vma[i], &p->vma[i + 1], (p->nvma - i - 1) * sizeof(struct vma));
			p->nvma--;
		}
		return true;
	}
	if (i < p->nvma && p->vma[i].start == end && p->vma[i].prot == prot) {
		p->vma[i].start = start;
		return true;
	}
	return vma_insert_at(p, i, start, end, prot);
}

/* a free range of len bytes in the mmap area, from the hint on */
static u64 find_free(struct lx_proc *p, u64 hint, u64 len)
{
	for (int pass = 0; pass < 2; pass++) {
		u64 a = pass ? LX_MAP_START : MAX(hint, LX_MAP_START);
		for (int i = 0; i < p->nvma && a + len <= LX_MAP_END; i++) {
			struct vma *v = &p->vma[i];
			if (v->end <= a) continue;
			if (v->start >= a + len) break;
			a = ALIGN_UP(v->end, PAGE);
		}
		if (a + len <= LX_MAP_END) return a;
	}
	return 0;
}

#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

long lx_mmap(struct lx_proc *p, u64 addr, u64 len, u32 prot, u32 flags, struct lx_file *f, u64 off)
{
	if (!len || (addr & (PAGE - 1)) || (off & (PAGE - 1))) return -EINVAL;
	len = ALIGN_UP(len, PAGE);
	u64 a;
	if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
		if (!lx_user_range(addr, len)) return -ENOMEM;
		if (flags & MAP_FIXED_NOREPLACE)
			for (u64 v = addr; v < addr + len; v += PAGE)
				if (vma_find(p, v)) return -EEXIST;
		a = addr;
	} else {
		/* a hint is taken when it is free and ours */
		a = 0;
		if (addr && lx_user_range(addr, len)) {
			a = addr;
			for (int i = 0; i < p->nvma; i++)
				if (p->vma[i].start < addr + len && p->vma[i].end > addr) { a = 0; break; }
		}
		if (!a) a = find_free(p, p->map_next, len);
		if (!a) return -ENOMEM;
		if (a >= LX_MAP_START) p->map_next = a + len;
	}
	unmap_pages(p, a, a + len);
	if (!lx_vma_add(p, a, a + len, prot)) return -ENOMEM;
	if (f && !(flags & MAP_ANONYMOUS) && prot) {
		/* a file: read in now, a private copy (MAP_SHARED too) */
		u64 size = lx_file_size(f);
		for (u64 o = 0; o < len && off + o < size; o += PAGE) {
			u64 *e = pte_of(p, a + o, true);
			addr_t pg = e ? lx_page_alloc() : 0;
			if (!pg) return -ENOMEM;
			lx_file_pread(f, (void *)pg, MIN(PAGE, size - off - o), off + o);
			*e = pg | pte_bits(prot);
			if (prot & PROT_EXEC) cache_sync_code(pg, PAGE);
		}
	}
	return (long)a;
}

long lx_munmap(struct lx_proc *p, u64 addr, u64 len)
{
	if ((addr & (PAGE - 1)) || !len) return -EINVAL;
	len = ALIGN_UP(len, PAGE);
	if (!lx_user_range(addr, len)) return -EINVAL;
	unmap_pages(p, addr, addr + len);
	return vma_clear(p, addr, addr + len) ? 0 : -ENOMEM;
}

long lx_mprotect(struct lx_proc *p, u64 addr, u64 len, u32 prot)
{
	if (addr & (PAGE - 1)) return -EINVAL;
	len = ALIGN_UP(len, PAGE);
	if (!lx_user_range(addr, len)) return -ENOMEM;
	for (u64 v = addr; v < addr + len; v += PAGE)
		if (!vma_find(p, v)) return -ENOMEM;
	if (!lx_vma_add(p, addr, addr + len, prot)) return -ENOMEM;
	for (u64 v = addr; v < addr + len; v += PAGE) {
		u64 *e = pte_of(p, v, false);
		if (!e || !(*e & PTE_ADDR)) continue;
		if (!prot) {
			/* no access: the page stays, unmapped; it is kept in the
			 * table without its valid bit */
			*e &= ~1ul;
			continue;
		}
		*e = (*e & PTE_ADDR) | pte_bits(prot);
		if (prot & PROT_EXEC) cache_sync_code(*e & PTE_ADDR, PAGE);
	}
	tlb_flush_all();
	return 0;
}

#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_REMOVE 9

long lx_madvise(struct lx_proc *p, u64 addr, u64 len, int advice)
{
	if (addr & (PAGE - 1)) return -EINVAL;
	len = ALIGN_UP(len, PAGE);
	if (!lx_user_range(addr, len)) return -EINVAL;
	if (advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_REMOVE)
		unmap_pages(p, addr, addr + len);
	return 0;
}

long lx_brk(struct lx_proc *p, u64 addr)
{
	if (!addr || addr < p->brk_start) return p->brk;
	u64 top = ALIGN_UP(addr, PAGE), old = ALIGN_UP(p->brk, PAGE);
	if (top > LX_STACK_TOP - LX_STACK_SIZE - (1u << 20)) return p->brk;
	if (top > old) {
		for (u64 v = old; v < top; v += PAGE)
			if (vma_find(p, v)) return p->brk;
		if (!lx_vma_add(p, old, top, PROT_READ | PROT_WRITE)) return p->brk;
	} else if (top < old) {
		unmap_pages(p, top, old);
		vma_clear(p, top, old);
	}
	p->brk = addr;
	return p->brk;
}

/* a page of the process, made present if its area allows; NULL if it may
 * not be touched that way */
static u8 *page_at(struct lx_proc *p, u64 va, bool write, bool exec)
{
	if (!lx_user_range(va, 1)) return NULL;
	u64 *e = pte_of(p, va, false);
	if (e && (*e & 1)) {
		if (write && (*e & PTE_RO)) return NULL;
		return (u8 *)((*e & PTE_ADDR) + (va & (PAGE - 1)));
	}
	struct vma *v = vma_find(p, va);
	if (!v || !v->prot || (write && !(v->prot & PROT_WRITE)) || (exec && !(v->prot & PROT_EXEC)))
		return NULL;
	if (e && (*e & PTE_ADDR)) {
		/* kept by mprotect(PROT_NONE), allowed again */
		*e = (*e & PTE_ADDR) | pte_bits(v->prot);
		tlb_flush_all();
		return (u8 *)((*e & PTE_ADDR) + (va & (PAGE - 1)));
	}
	e = pte_of(p, va, true);
	addr_t pg = e ? lx_page_alloc() : 0;
	if (!pg) {
		kprintf("lx: out of memory (pid %u)\n", p->pid);
		return NULL;
	}
	asm volatile("dsb ishst" ::: "memory");
	*e = pg | pte_bits(v->prot);
	if (v->prot & PROT_EXEC) cache_sync_code(pg, PAGE);
	return (u8 *)(pg + (va & (PAGE - 1)));
}

bool lx_fault(struct lx_proc *p, u64 va, bool write, bool exec)
{
	return page_at(p, va & ~(PAGE - 1), write, exec) != NULL;
}

int lx_copy_in(struct lx_proc *p, void *dst, u64 va, u64 len)
{
	u8 *d = dst;
	while (len) {
		u8 *s = page_at(p, va, false, false);
		if (!s) return -EFAULT;
		u64 n = MIN(len, PAGE - (va & (PAGE - 1)));
		memcpy(d, s, n);
		d += n, va += n, len -= n;
	}
	return 0;
}

int lx_copy_out(struct lx_proc *p, u64 va, const void *src, u64 len)
{
	const u8 *s = src;
	while (len) {
		u8 *d = page_at(p, va, true, false);
		if (!d) return -EFAULT;
		u64 n = MIN(len, PAGE - (va & (PAGE - 1)));
		memcpy(d, s, n);
		s += n, va += n, len -= n;
	}
	return 0;
}

int lx_str_in(struct lx_proc *p, char *dst, u64 va, u32 size)
{
	for (u32 i = 0; i < size; i++) {
		u8 *s = page_at(p, va + i, false, false);
		if (!s) return -EFAULT;
		dst[i] = *s;
		if (!*s) return i;
	}
	return -ENAMETOOLONG;
}
