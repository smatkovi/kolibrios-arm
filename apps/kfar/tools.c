/* kfar for KolibriOS for ARM: tools.inc: the memory, the error messages.
 *
 * Translated to C from kfar's tools.inc, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0.
 *
 * The original takes its memory from function 68 (12 allocate, 13 free,
 * 20 reallocate); here from the C library's malloc: KolibriOS for ARM's
 * 68.13 keeps the memory and 68.20 does not exist. The memory is zeroed,
 * as fresh pages of 68.12 are. Of the loading of DLLs only the plugins'
 * part is here (load_dll_and_import.do with ebp = -1, its
 * find_exported_function): libini comes through the program's own
 * imports (the Makefile's DLLS, lib/libc/dll.c). */
#include "kfar.h"
#include "kos_dll.h"

/* tools.inc: pgalloc: in: ecx = size; out: a pointer or NULL */
void *pgalloc(u32 ecx)
{
	return calloc(1, ecx ? ecx : 1);
}

/* tools.inc: pgfree: in: ecx = the pointer */
void pgfree(void *ecx)
{
	free(ecx);
}

/* tools.inc: pgrealloc: in: ecx = the size, edx = the pointer */
void *pgrealloc(void *edx, u32 ecx)
{
	return realloc(edx, ecx ? ecx : 1);
}

/* tools.inc: xpgalloc: pgalloc, saying so when there is no memory */
void *xpgalloc(u32 ecx)
{
	void *eax = pgalloc(ecx);
	if (!eax) SayNoMem();
	return eax;
}

/* tools.inc: xpgrealloc */
void *xpgrealloc(void *edx, u32 ecx)
{
	void *eax = pgrealloc(edx, ecx);
	if (!eax) SayNoMem();
	return eax;
}

/* tools.inc: getfreemem: the free RAM in KiB */
u32 getfreemem(void)
{
	return kos_call(18, 16, 0, 0, 0, 0);
}

/* tools.inc: get_error_msg: in: eax = the error code; out: the message
 * (in a static buffer) */
const char *get_error_msg(u32 eax)
{
	extern const char *const errors1[12], *const errors2[3];
	if (eax <= 11) {
		strcpy(error_msg, errors1[eax]);
	} else if (eax >= 30 && eax <= 32) {
		strcpy(error_msg, errors2[eax - 30]);
	} else {
		strcpy(error_msg, aUnknownError);
		char *edi = error_msg + strlen(error_msg);
		s32 v = eax;
		if (v < 0) {
			*edi++ = '-';
			v = -v;
		}
		char buf[12];
		int n = 0;
		do buf[n++] = '0' + (u32)v % 10; while ((v = (u32)v / 10));
		while (n) *edi++ = buf[--n];
		*edi = 0;
	}
	return error_msg;
}

/* tools.inc: libini_alloc, libini_free, libini_realloc: xpgalloc and
 * its kin as stdcall functions (kfar_info's pgalloc2 ...) */
void *libini_alloc(unsigned size)
{
	return xpgalloc(size);
}

void libini_free(void *p)
{
	pgfree(p);
}

void *libini_realloc(void *p, unsigned size)
{
	return xpgrealloc(p, size);
}

/* load_dll_and_import.sayerr: edi = the reason, then "Cannot load
 * DLL" and the file's name */
static void load_dll_sayerr(const char *edi, const char *name)
{
	const char *strings[3] = { edi, aCannotLoadDLL, name };
	const char *buttons[1] = { aOk };
	SayErr(3, strings, 1, buttons);
}

/* tools.inc: load_dll_and_import.do, as enum_plugins_callback calls it
 * (ebp = -1: no import list, no version check): function 68.19 with the
 * full name eax; the export table, or NULL with the message said.
 *
 * KolibriOS for ARM: the original does not initialise a plugin (its
 * lib_init is called for libraries only); an ARM library's lib_init
 * (lib/dllrt.c) hands it the program's malloc, free and realloc, so it is
 * called here, as for the other libraries (dll.Init's arguments: the C
 * library's malloc, free, realloc, dll_load), when the first export is
 * a lib_* one. */
const struct kos_export *load_dll_and_import_do(const char *eax)
{
	const struct kos_export *edx = kos_load_dll(eax);
	if (!edx) {
		load_dll_sayerr(aInvalidDLL, eax);
		return NULL;
	}
	if (edx->name && !strncmp(edx->name, "lib_", 4)) {
		int (*init)(kos_mem_alloc, kos_mem_free, kos_mem_realloc, kos_dll_load) = edx->value;
		if (init((kos_mem_alloc)malloc, free, (kos_mem_realloc)realloc, dll_load)) {
			load_dll_sayerr(aInitFailed, eax);
			return NULL;
		}
	}
	return edx;
}

/* tools.inc: load_dll_and_import.find_exported_function: in edx's
 * table the export named eax; false (CF set) if there is none */
bool find_exported_function(const struct kos_export *edx, const char *eax, void **value)
{
	for (; edx->name; edx++)
		if (!strcmp(edx->name, eax)) {
			*value = edx->value;
			return true;
		}
	return false;
}
