/* KolibriOS for ARM: the C library (lib/libc): malloc and free.
 *
 * The kernel's function 68.12 hands out memory of the program's heap but
 * does not take it back (68.13 keeps it), so the heap is managed here: a
 * list of free blocks in address order, joined with their neighbours when
 * freed (as K&R's malloc), more memory from 68.12 in steps of 256 KiB.
 * free() of memory the kernel handed out itself (function 54.1's
 * clipboard data, 68.12 called directly) goes to 68.13.
 *
 * The threads of a program (function 51) share it: a lock around each
 * call, taken by ldrex/strex, waited for by giving up the time slice
 * (68.1) while another thread holds it. Libraries loaded by 68.19 get
 * these functions from the program (lib_init, lib/dllrt.c). */
#include <stdlib.h>
#include <string.h>

typedef struct hdr {
	struct hdr *next;      /* the next free block (free blocks only) */
	size_t size;           /* in units of the header, the header included */
} hdr;

static hdr base;
static hdr *freep;

/* the blocks from 68.12 this allocator manages, joined when adjacent */
#define MAX_AREAS 64
static struct { char *lo, *hi; } areas[MAX_AREAS];
static int nareas;

static int ours(void *p)
{
	for (int i = 0; i < nareas; i++)
		if ((char *)p > areas[i].lo && (char *)p < areas[i].hi) return 1;
	return 0;
}

static inline unsigned kos68(unsigned sub, unsigned arg)
{
	register unsigned r0 asm("r0") = 68, r1 asm("r1") = sub, r2 asm("r2") = arg;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2) :: "r3", "r4", "r5", "memory");
	return r0;
}

static int lock;

static void take(void)
{
	while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) kos68(1, 0);
}

static void give(void)
{
	__atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

static void free_locked(void *ap);

static hdr *morecore(size_t units)
{
	size_t n = units < 256 * 1024 / sizeof(hdr) ? 256 * 1024 / sizeof(hdr) : units;
	hdr *p = (hdr *)kos68(12, n * sizeof(hdr));
	if (!p && n > units) p = (hdr *)kos68(12, (n = units) * sizeof(hdr));
	if (!p) return NULL;
	char *lo = (char *)p, *hi = lo + n * sizeof(hdr);
	int i;
	for (i = 0; i < nareas && areas[i].hi != lo; i++)
		;
	if (i < nareas) areas[i].hi = hi;
	else if (nareas < MAX_AREAS) areas[nareas].lo = lo, areas[nareas++].hi = hi;
	p->size = n;
	free_locked(p + 1);
	return freep;
}

static void *malloc_locked(size_t n)
{
	if (!freep) {
		kos68(11, 0);          /* the heap, once */
		base.next = freep = &base;
		base.size = 0;
	}
	size_t units = (n + sizeof(hdr) - 1) / sizeof(hdr) + 1;
	for (hdr *prev = freep, *p = prev->next;; prev = p, p = p->next) {
		if (p->size >= units) {
			if (p->size == units) {
				prev->next = p->next;
			} else {
				p->size -= units;
				p += p->size;
				p->size = units;
			}
			freep = prev;
			return p + 1;
		}
		if (p == freep && !(p = morecore(units))) return NULL;
	}
}

static void free_locked(void *ap)
{
	if (!ap) return;
	if (!ours(ap)) {
		kos68(13, (unsigned)ap);
		return;
	}
	hdr *bp = (hdr *)ap - 1, *p;
	for (p = freep; !(bp > p && bp < p->next); p = p->next)
		if (p >= p->next && (bp > p || bp < p->next)) break;
	if (bp + bp->size == p->next) {
		bp->size += p->next->size;
		bp->next = p->next->next;
	} else {
		bp->next = p->next;
	}
	if (p + p->size == bp) {
		p->size += bp->size;
		p->next = bp->next;
	} else {
		p->next = bp;
	}
	freep = p;
}

void *malloc(size_t n)
{
	take();
	void *p = malloc_locked(n);
	give();
	return p;
}

void free(void *ap)
{
	take();
	free_locked(ap);
	give();
}

void *calloc(size_t n, size_t size)
{
	if (size && n > (size_t)-1 / size) return NULL;
	void *p = malloc(n * size);
	if (p) memset(p, 0, n * size);
	return p;
}

void *realloc(void *p, size_t n)
{
	if (!p) return malloc(n);
	if (!n) { free(p); return NULL; }
	hdr *h = (hdr *)p - 1;
	take();
	size_t have = ours(p) ? (h->size - 1) * sizeof(hdr) : *((unsigned *)p - 2) - 8;
	give();
	if (have >= n) return p;
	void *q = malloc(n);
	if (!q) return NULL;
	memcpy(q, p, have);
	free(p);
	return q;
}
