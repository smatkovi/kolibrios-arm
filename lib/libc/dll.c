/* KolibriOS for ARM: the C library (lib/libc): libraries, as KolibriOS's
 * programs/dll.inc loads them (dll.Load, dll.Link, dll.Init,
 * dll.LoadLibrary, dll.GetProcAddress). __libc_start_main calls dll_load
 * on the program's import list (the .kos_import section, lib/app.ld)
 * before main; a program may call it with lists of its own.
 *
 * An import list: { table, file name } pairs, ended by 0, 0; a table:
 * a word per function, the function's name until dll_link puts its
 * address there, ended by 0. A name without a "/" is a file in /sys/lib.
 * lib_init, a library's first export when its name starts with "lib_",
 * gets the program's malloc, free, realloc and dll_load. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kos_dll.h>

/* dll.GetProcAddress */
void *dll_get_proc_address(const struct kos_export *exp, const char *name)
{
	for (; exp && exp->name; exp++)
		if (!strcmp(exp->name, name)) return exp->value == (void *)-1 ? NULL : exp->value;
	return NULL;
}

/* dll.Link: false when a name is not in the export table (the rest of
 * the table is then left as it is) */
int dll_link(const struct kos_export *exp, void **imp)
{
	for (; imp && *imp; imp++) {
		void *f = dll_get_proc_address(exp, *imp);
		if (!f) {
			printf("dll: %s not exported\n", (const char *)*imp);
			return 0;
		}
		*imp = f;
	}
	return 1;
}

/* dll.Init */
void dll_init(void *entry)
{
	((int (*)(void *, void *, void *, void *))entry)(malloc, free, realloc, dll_load);
}

/* dll.LoadLibrary: the export table, lib_init called; NULL if none */
struct kos_export *dll_load_library(const char *name)
{
	char path[256];
	if (name[0] == '/') {
		strncpy(path, name, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
	} else {
		snprintf(path, sizeof(path), "/sys/lib/%s", name);
	}
	struct kos_export *e = kos_load_dll(path);
	if (!e) {
		printf("dll: %s cannot be loaded\n", path);
		return NULL;
	}
	if (e->name && !strncmp(e->name, "lib_", 4)) dll_init(e->value);
	return e;
}

/* dll.Load: 0 when all of them are loaded and linked, else 1 */
int dll_load(const struct kos_import *imp)
{
	for (; imp && imp->table; imp++) {
		struct kos_export *e = dll_load_library(imp->name);
		if (!e || !dll_link(e, imp->table)) return 1;
	}
	return 0;
}
