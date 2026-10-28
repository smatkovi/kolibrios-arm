/* KolibriOS for ARM: reading the flattened device tree the boot loader
 * passes (AArch64): nodes found by their compatible string or name,
 * their properties, with the #address-cells and #size-cells of their
 * parent. Read before the MMU is on: aligned 4-byte reads only. */
#include "kernel.h"
#include "fdt.h"

#define FDT_MAGIC 0xd00dfeed
#define TOK_BEGIN 1
#define TOK_END 2
#define TOK_PROP 3
#define TOK_NOP 4
#define TOK_DONE 9

static u32 be32(const void *p)
{
	const u8 *b = p;
	return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

u64 fdt_cells(const void *p, int cells)
{
	u64 v = 0;
	for (int i = 0; i < cells; i++) v = v << 32 | be32((const u8 *)p + 4 * i);
	return v;
}

bool fdt_ok(const void *fdt)
{
	return fdt && be32(fdt) == FDT_MAGIC;
}

static const u8 *structs(const void *fdt) { return (const u8 *)fdt + be32((const u8 *)fdt + 8); }
static const char *strs(const void *fdt) { return (const char *)fdt + be32((const u8 *)fdt + 12); }

static bool streq(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

/* one string of a string list property */
static bool list_has(const char *list, int len, const char *s)
{
	for (int i = 0; i < len; ) {
		if (streq(list + i, s)) return true;
		while (i < len && list[i]) i++;
		i++;
	}
	return false;
}

/* walk the tree: cb(node offset, name, depth, parent cells) for each node;
 * a non-zero return ends the walk with it */
typedef int (*walk_fn)(const void *fdt, int node, const char *name, int depth,
		       int acells, int scells, const void *arg);

static int walk(const void *fdt, walk_fn cb, const void *arg)
{
	if (!fdt_ok(fdt)) return -1;
	const u8 *base = structs(fdt);
	const char *sb = strs(fdt);
	int depth = -1, off = 0;
	int ac[16] = { 2 }, sc[16] = { 1 };
	for (;;) {
		u32 tok = be32(base + off);
		off += 4;
		if (tok == TOK_BEGIN) {
			int node = off - 4;
			const char *name = (const char *)base + off;
			int n = 0;
			while (name[n]) n++;
			off = ALIGN_UP(off + n + 1, 4);
			depth++;
			if (depth < 15) { ac[depth + 1] = 2; sc[depth + 1] = 1; }
			/* this node's own #address-cells/#size-cells, for its children */
			int p = off;
			while (be32(base + p) == TOK_PROP || be32(base + p) == TOK_NOP) {
				if (be32(base + p) == TOK_NOP) { p += 4; continue; }
				u32 len = be32(base + p + 4);
				const char *pn = sb + be32(base + p + 8);
				if (depth < 15 && streq(pn, "#address-cells")) ac[depth + 1] = be32(base + p + 12);
				if (depth < 15 && streq(pn, "#size-cells")) sc[depth + 1] = be32(base + p + 12);
				p = ALIGN_UP(p + 12 + len, 4);
			}
			int d = depth < 15 ? depth : 15;
			int r = cb(fdt, node, name, depth, ac[d], sc[d], arg);
			if (r) return r;
		} else if (tok == TOK_END) {
			depth--;
		} else if (tok == TOK_PROP) {
			u32 len = be32(base + off);
			off = ALIGN_UP(off + 8 + len, 4);
		} else if (tok == TOK_NOP) {
		} else {
			return -1;
		}
	}
}

const void *fdt_prop(const void *fdt, int node, const char *name, int *len)
{
	if (!fdt_ok(fdt) || node < 0) return NULL;
	const u8 *base = structs(fdt);
	const char *sb = strs(fdt);
	int off = node + 4;
	const char *nn = (const char *)base + off;
	int n = 0;
	while (nn[n]) n++;
	off = ALIGN_UP(off + n + 1, 4);
	for (;;) {
		u32 tok = be32(base + off);
		if (tok == TOK_NOP) { off += 4; continue; }
		if (tok != TOK_PROP) return NULL;
		u32 l = be32(base + off + 4);
		if (streq(sb + be32(base + off + 8), name)) {
			if (len) *len = l;
			return base + off + 12;
		}
		off = ALIGN_UP(off + 12 + l, 4);
	}
}

struct find { const char *compat; const char *name; int start; int *acells, *scells; };

static int find_cb(const void *fdt, int node, const char *name, int depth, int ac, int sc, const void *arg)
{
	const struct find *f = arg;
	if (node <= f->start) return 0;
	if (f->name) {
		int i = 0;
		while (f->name[i] && name[i] == f->name[i]) i++;
		if (f->name[i] || (name[i] && name[i] != '@')) return 0;
	} else {
		int len;
		const char *c = fdt_prop(fdt, node, "compatible", &len);
		if (!c || !list_has(c, len, f->compat)) return 0;
	}
	if (f->acells) *f->acells = ac;
	if (f->scells) *f->scells = sc;
	return node;
}

int fdt_find_compatible(const void *fdt, const char *compat, int start, int *acells, int *scells)
{
	struct find f = { compat, NULL, start, acells, scells };
	int r = walk(fdt, find_cb, &f);
	return r > 0 ? r : -1;
}

int fdt_find_name(const void *fdt, const char *name, int *acells, int *scells)
{
	struct find f = { NULL, name, -1, acells, scells };
	int r = walk(fdt, find_cb, &f);
	return r > 0 ? r : -1;
}

int fdt_root_compatible(const void *fdt, const char *compat)
{
	if (!fdt_ok(fdt)) return 0;
	int len;
	const char *c = fdt_prop(fdt, 0, "compatible", &len);
	return c && list_has(c, len, compat);
}

/* reg's first (address, size), in the parent's cells */
bool fdt_reg(const void *fdt, int node, int acells, int scells, int index, u64 *addr, u64 *size)
{
	int len;
	const u8 *p = fdt_prop(fdt, node, "reg", &len);
	int step = 4 * (acells + scells);
	if (!p || (index + 1) * step > len) return false;
	p += index * step;
	*addr = fdt_cells(p, acells);
	if (size) *size = fdt_cells(p + 4 * acells, scells);
	return true;
}

u32 fdt_u32(const void *fdt, int node, const char *name, u32 def)
{
	int len;
	const void *p = fdt_prop(fdt, node, name, &len);
	return p && len >= 4 ? be32(p) : def;
}

/* the reserved memory a region must not overlap: /reserved-memory's
 * children (any) and the header's memory reservation block */
bool fdt_overlaps_reserved(const void *fdt, u64 start, u64 size)
{
	const u8 *rsv = (const u8 *)fdt + be32((const u8 *)fdt + 16);
	for (;; rsv += 16) {
		u64 a = fdt_cells(rsv, 2), s = fdt_cells(rsv + 8, 2);
		if (!a && !s) break;
		if (a < start + size && start < a + s) return true;
	}
	int ac, sc;
	int rm = fdt_find_name(fdt, "reserved-memory", &ac, &sc);
	if (rm < 0) return false;
	/* its children use its own cells */
	int cac = fdt_u32(fdt, rm, "#address-cells", 2), csc = fdt_u32(fdt, rm, "#size-cells", 2);
	const u8 *base = structs(fdt);
	int off = rm + 4, depth = 0;
	const char *nn = (const char *)base + off;
	int n = 0;
	while (nn[n]) n++;
	off = ALIGN_UP(off + n + 1, 4);
	for (;;) {
		u32 tok = be32(base + off);
		if (tok == TOK_PROP) { off = ALIGN_UP(off + 12 + be32(base + off + 4), 4); continue; }
		if (tok == TOK_NOP) { off += 4; continue; }
		if (tok == TOK_BEGIN) {
			if (depth == 0) {
				u64 a, s;
				if (fdt_reg(fdt, off, cac, csc, 0, &a, &s) && a < start + size && start < a + s)
					return true;
			}
			depth++;
			const char *cn = (const char *)base + off + 4;
			int m = 0;
			while (cn[m]) m++;
			off = ALIGN_UP(off + 4 + m + 1, 4);
			continue;
		}
		if (tok == TOK_END) {
			if (depth == 0) return false;
			depth--;
			off += 4;
			continue;
		}
		return false;
	}
}
