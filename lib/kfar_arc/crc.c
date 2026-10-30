/* kfar_arc for KolibriOS for ARM: CRC-32 (crc.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/crc.inc,
 * diamond (Evgeny Grechnikov), GPL-2.0 (kfar_arc.h). */
#include "kfar_arc.h"

/* crc.inc: init_crc_table (the polynomial 0xEDB88320) */
void init_crc_table(void)
{
	for (u32 edx = 0; edx < 256; edx++) {
		u32 eax = edx;
		for (int ecx = 8; ecx; ecx--) eax = eax & 1 ? eax >> 1 ^ 0xEDB88320 : eax >> 1;
		crc_table[edx] = eax;
	}
}

/* crc.inc: crc_continue: in: eax = previous crc, ecx = size, esi ->
 * buffer; out: the crc */
u32 crc_continue(u32 eax, const u8 *esi, u32 ecx)
{
	eax = ~eax;
	while (ecx--) eax = eax >> 8 ^ crc_table[(u8)eax ^ *esi++];     /* crc.loop */
	return ~eax;
}

/* crc.inc: crc: in: ecx = size, esi -> buffer; out: the crc */
u32 crc(const u8 *esi, u32 ecx)
{
	return crc_continue(0, esi, ecx);
}
