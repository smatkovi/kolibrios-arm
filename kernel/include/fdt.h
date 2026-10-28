/* KolibriOS for ARM: the device tree (fdt.c) */
#ifndef FDT_H
#define FDT_H

#include "kernel.h"

bool fdt_ok(const void *fdt);
int fdt_root_compatible(const void *fdt, const char *compat);
/* a node (its offset) after start (-1: the first), with its parent's cells */
int fdt_find_compatible(const void *fdt, const char *compat, int start, int *acells, int *scells);
int fdt_find_name(const void *fdt, const char *name, int *acells, int *scells);
const void *fdt_prop(const void *fdt, int node, const char *name, int *len);
u32 fdt_u32(const void *fdt, int node, const char *name, u32 def);
u64 fdt_cells(const void *p, int cells);
bool fdt_reg(const void *fdt, int node, int acells, int scells, int index, u64 *addr, u64 *size);
bool fdt_overlaps_reserved(const void *fdt, u64 start, u64 size);

#endif
