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

/* the offset behind a node's name and properties */
static int node_body(const u8 *base, int node)
{
	const char *nn = (const char *)base + node + 4;
	int n = 0;
	while (nn[n]) n++;
	int off = ALIGN_UP(node + 4 + n + 1, 4);
	for (;;) {
		u32 tok = be32(base + off);
		if (tok == TOK_PROP) off = ALIGN_UP(off + 12 + be32(base + off + 4), 4);
		else if (tok == TOK_NOP) off += 4;
		else return off;
	}
}

/* the end of a node (behind its TOK_END) */
static int node_end(const u8 *base, int node)
{
	int off = node_body(base, node), depth = 0;
	for (;;) {
		u32 tok = be32(base + off);
		if (tok == TOK_BEGIN) {
			depth++;
			off = node_body(base, off);
		} else if (tok == TOK_END) {
			off += 4;
			if (depth-- == 0) return off;
		} else if (tok == TOK_PROP) {
			off = ALIGN_UP(off + 12 + be32(base + off + 4), 4);
		} else if (tok == TOK_NOP) {
			off += 4;
		} else {
			return -1;
		}
	}
}

int fdt_next_child(const void *fdt, int parent, int prev)
{
	if (!fdt_ok(fdt) || parent < 0) return -1;
	const u8 *base = structs(fdt);
	int off = prev < 0 ? node_body(base, parent) : node_end(base, prev);
	while (off >= 0 && be32(base + off) == TOK_NOP) off += 4;
	return off >= 0 && be32(base + off) == TOK_BEGIN ? off : -1;
}

const char *fdt_node_name(const void *fdt, int node)
{
	return (const char *)structs(fdt) + node + 4;
}

u32 fdt_totalsize(const void *fdt)
{
	return be32((const u8 *)fdt + 4);
}

/* a node's name without the unit address */
static bool name_is(const char *name, const char *want)
{
	while (*want && *name == *want) name++, want++;
	return !*want && (!*name || *name == '@');
}

/* for each reserved region: the header's reservation block, the children
 * of /reserved-memory with a reg, the device tree itself */
static void for_each_reserved(const void *fdt, void (*cb)(u64 a, u64 s, void *arg), void *arg)
{
	const u8 *rsv = (const u8 *)fdt + be32((const u8 *)fdt + 16);
	for (;; rsv += 16) {
		u64 a = fdt_cells(rsv, 2), s = fdt_cells(rsv + 8, 2);
		if (!a && !s) break;
		cb(a, s, arg);
	}
	for (int c = fdt_next_child(fdt, 0, -1); c >= 0; c = fdt_next_child(fdt, 0, c)) {
		if (!name_is(fdt_node_name(fdt, c), "reserved-memory")) continue;
		int cac = fdt_u32(fdt, c, "#address-cells", 2), csc = fdt_u32(fdt, c, "#size-cells", 2);
		for (int r = fdt_next_child(fdt, c, -1); r >= 0; r = fdt_next_child(fdt, c, r)) {
			u64 a, s;
			for (int i = 0; fdt_reg(fdt, r, cac, csc, i, &a, &s); i++)
				cb(a, s, arg);
		}
	}
	cb((addr_t)fdt, fdt_totalsize(fdt), arg);
}

struct overlap { u64 start, size; bool hit; };

static void overlap_cb(u64 a, u64 s, void *arg)
{
	struct overlap *o = arg;
	if (a < o->start + o->size && o->start < a + s) o->hit = true;
}

bool fdt_overlaps_reserved(const void *fdt, u64 start, u64 size)
{
	struct overlap o = { start, size, false };
	for_each_reserved(fdt, overlap_cb, &o);
	return o.hit;
}

struct usable { struct mem_bank *b; int n, max; u64 align; };

/* a reserved region (widened to the alignment) cut out of the banks */
static void cut_cb(u64 a, u64 s, void *arg)
{
	struct usable *u = arg;
	u64 lo = a & ~(u->align - 1), hi = ALIGN_UP(a + s, u->align);
	for (int i = 0; i < u->n; i++) {
		u64 b0 = u->b[i].base, b1 = b0 + u->b[i].size;
		if (hi <= b0 || lo >= b1) continue;
		if (lo > b0 && hi < b1 && u->n < u->max) {     /* split */
			u->b[u->n].base = hi;
			u->b[u->n].size = b1 - hi;
			u->n++;
			u->b[i].size = lo - b0;
		} else if (lo > b0) {
			u->b[i].size = lo - b0;
		} else if (hi < b1) {
			u->b[i].base = hi;
			u->b[i].size = b1 - hi;
		} else {
			u->b[i].size = 0;
		}
	}
}

int fdt_usable_ram(const void *fdt, struct mem_bank *out, int max, u64 align)
{
	struct usable u = { out, 0, max, align };
	int ac = fdt_u32(fdt, 0, "#address-cells", 2), sc = fdt_u32(fdt, 0, "#size-cells", 1);
	for (int c = fdt_next_child(fdt, 0, -1); c >= 0; c = fdt_next_child(fdt, 0, c)) {
		if (!name_is(fdt_node_name(fdt, c), "memory")) continue;
		u64 a, s;
		for (int i = 0; u.n < max && fdt_reg(fdt, c, ac, sc, i, &a, &s); i++) {
			u64 lo = ALIGN_UP(a, align), hi = (a + s) & ~(align - 1);
			if (hi > lo) {
				out[u.n].base = lo;
				out[u.n].size = hi - lo;
				u.n++;
			}
		}
	}
	for_each_reserved(fdt, cut_cb, &u);
	/* the empty ones out, in address order */
	int n = 0;
	for (int i = 0; i < u.n; i++)
		if (out[i].size) out[n++] = out[i];
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && out[j].base < out[j - 1].base; j--) {
			struct mem_bank t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}
