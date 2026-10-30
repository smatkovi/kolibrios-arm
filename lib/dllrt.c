/* KolibriOS for ARM: the runtime of a library loaded by function 68.19
 * (docs/threads.md), linked into each of them (lib/NAME as /sys/lib/
 * FILE): lib_init, and malloc, free, realloc and calloc through the
 * program's, as KolibriOS's libraries use the mem.Alloc, mem.Free and
 * mem.ReAlloc that dll.Init hands to their lib_init. Memory then moves
 * freely between the program and its libraries. Before lib_init (a
 * program that called 68.19 itself and not lib_init) they take blocks
 * from 68.12 directly, never given back. */
#include <stddef.h>
#include <string.h>
#include <kos_dll.h>

static kos_mem_alloc mem_alloc;
static kos_mem_free mem_free;
static kos_mem_realloc mem_realloc;
kos_dll_load dll_load_fn;

int lib_init(kos_mem_alloc a, kos_mem_free f, kos_mem_realloc r, kos_dll_load d)
{
	mem_alloc = a;
	mem_free = f;
	mem_realloc = r;
	dll_load_fn = d;
	return 0;
}

/* 68.12's block, its size in front */
static void *own_alloc(size_t n)
{
	unsigned *p = (unsigned *)kos_call(68, 12, n + 8, 0, 0, 0);
	if (!p) return NULL;
	p[0] = n;
	return p + 2;
}

void *malloc(size_t n)
{
	return mem_alloc ? mem_alloc(n) : own_alloc(n);
}

void free(void *p)
{
	if (mem_free) mem_free(p);
}

void *realloc(void *p, size_t n)
{
	if (mem_realloc) return mem_realloc(p, n);
	if (!p) return own_alloc(n);
	void *q = own_alloc(n);
	if (q) memcpy(q, p, ((unsigned *)p)[-2] < n ? ((unsigned *)p)[-2] : n);
	return q;
}

void *calloc(size_t n, size_t size)
{
	if (size && n > (size_t)-1 / size) return NULL;
	void *p = malloc(n * size);
	if (p) memset(p, 0, n * size);
	return p;
}
