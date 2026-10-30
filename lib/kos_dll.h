/* KolibriOS for ARM: libraries (function 68.19, docs/threads.md): the
 * export and import tables, dll.inc's functions (lib/libc/dll.c) */
#ifndef KOS_DLL_H
#define KOS_DLL_H

#include <kos.h>

/* a library's export table: these, ended by { 0, 0 } */
struct kos_export {
	const char *name;
	void *value;
};
/* a program's import list: these, ended by { 0, 0 } */
struct kos_import {
	void **table;
	const char *name;
};

void *dll_get_proc_address(const struct kos_export *exp, const char *name);
int dll_link(const struct kos_export *exp, void **imp);
void dll_init(void *entry);
struct kos_export *dll_load_library(const char *name);
int dll_load(const struct kos_import *imp);

/* lib_init's arguments, as dll.Init passes them */
typedef void *(*kos_mem_alloc)(unsigned size);
typedef void (*kos_mem_free)(void *p);
typedef void *(*kos_mem_realloc)(void *p, unsigned size);
typedef int (*kos_dll_load)(const struct kos_import *imp);

#endif
