/* kfar for KolibriOS for ARM: sort.inc: the heap sort of the panels.
 *
 * Translated to C from kfar's sort.inc, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0. */
#include "kfar.h"

/* sort.inc: sort.exchange: the elements esi and edi (from 1) */
static void sort_exchange(u8 **edx, u32 esi, u32 edi)
{
	u8 *eax = edx[esi - 1];
	edx[esi - 1] = edx[edi - 1];
	edx[edi - 1] = eax;
}

/* sort.inc: sort.restore: the heap below the element eax (from 1) */
static void sort_restore(u8 **edx, u32 ecx, compare_fn ebx, u32 eax)
{
	for (;;) {
		u32 esi = eax + eax;
		if (esi > ecx) return;                  /* .doner */
		if (ebx(edx[esi - 1], edx[eax - 1]) <= 0) {
			if (esi >= ecx) return;
			if (ebx(edx[esi], edx[eax - 1]) <= 0) return;
		}
		/* .need_xchg: the larger child ("sbb esi, -1") */
		if (esi != ecx && !(ebx(edx[esi], edx[esi - 1]) < 0)) esi++;
		/* .do_xchg */
		sort_exchange(edx, esi, eax);
		eax = esi;
	}
}

/* sort.inc: sort: ecx dwords at edx, the comparison ebx */
void sort(u8 **edx, u32 ecx, compare_fn ebx)
{
	if (!ecx) return;
	for (u32 eax = ecx; eax; eax--)
		sort_restore(edx, ecx, ebx, eax);
	while (ecx != 1) {
		sort_exchange(edx, 1, ecx);
		ecx--;
		sort_restore(edx, ecx, ebx, 1);
	}
}
