/* KolibriOS for ARM: libraries (functions 68.18, 68.19), as KolibriOS's
 * core/dll.inc loads its COFF libraries: the file into the process, its
 * relocations applied, the address of its export table (the symbol
 * EXPORTS) returned. A library already in the process (by its path)
 * gives the same table again, to all of its threads.
 *
 * The format (docs/threads.md): an ELF32 shared object for ARM (ET_DYN,
 * EM_ARM, little endian) as the GNU linker makes it with -shared
 * -Bsymbolic --hash-style=sysv from code built -fPIC: its PT_LOAD
 * segments from VA 0, no DT_NEEDED, relocations only R_ARM_RELATIVE
 * (and R_ARM_ABS32, GLOB_DAT, JUMP_SLOT against its own symbols), the
 * export table KolibriOS's: { name, value } pairs of words, 0-terminated,
 * the first one lib_init (called by the program's dll.Load, not here).
 * Each process gets its own copy, packed into blocks of its map area. */
#include "gui.h"

int kfs_file(const char *path, const u8 **data, void **node, u32 *size);
u32 kfs_read(void *node, void *buf, u32 len, u32 off);

struct ehdr {
	u8 ident[16];
	u16 type, machine;
	u32 version, entry, phoff, shoff, flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct dyn { s32 tag; u32 val; };
struct sym { u32 name, value, size; u8 info, other; u16 shndx; };

#define ET_DYN 3
#define EM_ARM 40
#define PT_LOAD 1
#define PT_DYNAMIC 2
enum { DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6,
       DT_RELA = 7, DT_STRSZ = 10, DT_REL = 17, DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23 };
enum { R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

static inline u32 get32(const void *p) { const struct __attribute__((packed)) w32 { u32 v; } *q = p; return q->v; }
static inline void put32(void *p, u32 v) { struct __attribute__((packed)) w32 { u32 v; } *q = p; q->v = v; }

struct file {
	const u8 *data;   /* the ramdisk's bytes, or NULL: read through node */
	void *node;
	u32 size;
};

static bool fread_at(const struct file *f, void *buf, u32 len, u32 off)
{
	if (off > f->size || len > f->size - off) return false;
	if (f->data) memcpy(buf, f->data + off, len);
	else if (kfs_read(f->node, buf, len, off) != len) return false;
	return true;
}

/* a path as function 70 takes it: absolute, or from the current folder */
static bool full_path(struct task *t, const char *in, char *out, u32 n)
{
	if (in[0] == '/') {
		strncpy0(out, in, n);
	} else {
		const char *cwd = t->cwd[0] ? t->cwd : "/sys";
		u32 l = strlen(cwd);
		if (l + 1 + strlen(in) >= n) return false;
		memcpy(out, cwd, l);
		out[l] = '/';
		strncpy0(out + l + 1, in, n - l - 1);
	}
	return true;
}

/* room for size bytes in the process's libraries' blocks (a new block
 * when the last one is full); the kernel's address in *k */
static u32 dll_room(struct proc *p, u32 size, u8 **k)
{
	size = ALIGN_UP(size, 4096u);
	for (int i = 0; i < p->nmap; i++) {
		struct umap *m = &p->map[i];
		if (m->kind == UMAP_DLL && m->size - m->used >= size) {
			u32 va = m->va + m->used;
			*k = (u8 *)(m->pa + m->used);
			m->used += size;
			return va;
		}
	}
	struct umap *m = proc_map_new(p, size, UMAP_DLL);
	if (!m) return 0;
	m->used = size;
	*k = (u8 *)m->pa;
	return m->va;
}

static int reloc(const char *path, u8 *img, u32 span, u32 base, const struct sym *syms, u32 nsym,
		 const u8 *rel, u32 relsz)
{
	for (u32 o = 0; o + 8 <= relsz; o += 8) {
		u32 off = get32(rel + o), info = get32(rel + o + 4), type = info & 0xff, si = info >> 8;
		if (type == R_ARM_NONE) continue;
		if (off > span - 4) return -1;
		u32 s = 0;
		if (type != R_ARM_RELATIVE) {
			if (si >= nsym || !syms[si].shndx) {
				kprintf("%s: an undefined symbol (relocation at %x)\n", path, off);
				return -1;
			}
			s = base + syms[si].value;
		}
		switch (type) {
		case R_ARM_RELATIVE: put32(img + off, get32(img + off) + base); break;
		case R_ARM_ABS32: put32(img + off, get32(img + off) + s); break;
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT: put32(img + off, s); break;
		default:
			kprintf("%s: relocation type %u not supported (at %x)\n", path, type, off);
			return -1;
		}
	}
	return 0;
}

/* the library into p: the VA of its export table, or 0 */
static u32 dll_load(struct task *t, const char *path)
{
	struct proc *p = t->proc;
	struct file f;
	if (kfs_file(path, &f.data, &f.node, &f.size)) return 0;
	struct ehdr h;
	if (!fread_at(&f, &h, sizeof(h), 0) || memcmp(h.ident, "\177ELF\1\1\1", 7) || h.type != ET_DYN ||
	    h.machine != EM_ARM || h.phentsize != sizeof(struct phdr) || !h.phnum || h.phnum > 16) {
		kprintf("%s: not a library for ARM (an ELF32 shared object)\n", path);
		return 0;
	}
	struct phdr ph[16];
	if (!fread_at(&f, ph, h.phnum * sizeof(struct phdr), h.phoff)) return 0;
	u32 span = 0, dyn_va = 0, dyn_sz = 0;
	for (int i = 0; i < h.phnum; i++) {
		if (ph[i].type == PT_LOAD) {
			if (ph[i].vaddr + ph[i].memsz < ph[i].vaddr || ph[i].filesz > ph[i].memsz) return 0;
			span = MAX(span, ph[i].vaddr + ph[i].memsz);
		} else if (ph[i].type == PT_DYNAMIC) {
			dyn_va = ph[i].vaddr;
			dyn_sz = ph[i].memsz;
		}
	}
	if (!span || span > (64u << 20) || !dyn_va) return 0;
	span = ALIGN_UP(span, 4096u);
	u8 *img;
	u32 base = dll_room(p, span, &img);
	if (!base) {
		kprintf("%s: no room for %u KiB\n", path, span >> 10);
		return 0;
	}
	memset(img, 0, span);
	for (int i = 0; i < h.phnum; i++)
		if (ph[i].type == PT_LOAD && !fread_at(&f, img + ph[i].vaddr, ph[i].filesz, ph[i].offset))
			return 0;
	/* the dynamic section: the symbols, the relocations */
	if (dyn_va > span || dyn_sz > span - dyn_va) return 0;
	u32 symtab = 0, strtab = 0, strsz = 0, hash = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
	for (u32 o = 0; o + 8 <= dyn_sz; o += 8) {
		s32 tag = get32(img + dyn_va + o);
		u32 val = get32(img + dyn_va + o + 4);
		if (tag == DT_NULL) break;
		switch (tag) {
		case DT_NEEDED: kprintf("%s: needs another library (DT_NEEDED)\n", path); return 0;
		case DT_RELA: kprintf("%s: RELA relocations\n", path); return 0;
		case DT_SYMTAB: symtab = val; break;
		case DT_STRTAB: strtab = val; break;
		case DT_STRSZ: strsz = val; break;
		case DT_HASH: hash = val; break;
		case DT_REL: rel = val; break;
		case DT_RELSZ: relsz = val; break;
		case DT_JMPREL: jmprel = val; break;
		case DT_PLTREL:
			if (val != DT_REL) { kprintf("%s: RELA relocations\n", path); return 0; }
			break;
		case DT_PLTRELSZ: pltrelsz = val; break;
		default: break;
		}
	}
	if (!symtab || !strtab || !hash || hash > span - 8 || strtab > span || strsz > span - strtab) {
		kprintf("%s: no symbol table (link with --hash-style=sysv)\n", path);
		return 0;
	}
	u32 nsym = get32(img + hash + 4);          /* nchain: the number of symbols */
	if (symtab > span || nsym > (span - symtab) / sizeof(struct sym) || (symtab & 3)) return 0;
	const struct sym *syms = (const struct sym *)(img + symtab);
	if ((rel && (rel > span || relsz > span - rel)) || (jmprel && (jmprel > span || pltrelsz > span - jmprel)))
		return 0;
	/* the symbols' values before the relocations change the image */
	u32 exports = 0;
	for (u32 i = 1; i < nsym; i++) {
		u32 n = syms[i].name;
		if (n + 8 <= strsz && syms[i].shndx && !memcmp(img + strtab + n, "EXPORTS", 8))
			exports = base + syms[i].value;
	}
	if (!exports) {
		kprintf("%s: no EXPORTS\n", path);
		return 0;
	}
	if ((rel && reloc(path, img, span, base, syms, nsym, img + rel, relsz)) ||
	    (jmprel && reloc(path, img, span, base, syms, nsym, img + jmprel, pltrelsz)))
		return 0;
	cache_sync_code((addr_t)img, span);
	return exports;
}

/* 68.19 (68.18: with the path's encoding, ASCII here): the export table
 * or 0. The memory of a library that failed stays the process's. */
void sys_load_dll(struct task *t, u32 *r)
{
	struct proc *p = t->proc;
	const char *in = ustr(t, r[2]);
	char path[128];
	r[0] = 0;
	if (!p || !in || !full_path(t, in, path, sizeof(path))) return;
	for (int i = 0; i < p->ndll; i++)
		if (!strcmp(p->dll[i].path, path)) {
			p->dll[i].refs++;
			r[0] = p->dll[i].exports;
			return;
		}
	if (p->ndll == MAX_DLLS) return;
	u32 e = dll_load(t, path);
	if (!e) return;
	struct pdll *d = &p->dll[p->ndll++];
	strncpy0(d->path, path, sizeof(d->path));
	d->exports = e;
	d->refs = 1;
	r[0] = e;
}
