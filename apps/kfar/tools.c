/* kfar for KolibriOS for ARM: tools.inc: the memory, the error messages.
 *
 * Translated to C from kfar's tools.inc, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0.
 *
 * The original takes its memory from function 68 (12 allocate, 13 free,
 * 20 reallocate); here from the C library's malloc: KolibriOS for ARM's
 * 68.13 keeps the memory and 68.20 does not exist. The memory is zeroed,
 * as fresh pages of 68.12 are. The loading of DLLs (load_dll_and_import,
 * libini_dllload) is left out: libini is linked in, plugins are not
 * supported (kfar.h). */
#include "kfar.h"

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
