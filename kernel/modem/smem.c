/* KolibriOS for ARM: Qualcomm's shared memory (SMEM), as Linux's
 * drivers/soc/qcom/smem.c reads it (only reads: the items KolibriOS needs
 * were allocated by the firmware or by Linux before).
 *
 * The partition table ("$TOC") is in the last 4 KiB. Each entry names a
 * partition ("$PRT" header, 32 bytes) and the two hosts it is for. Items
 * in a partition: uncached ones from its start up (a 16-byte header, the
 * item behind it), cached ones from its end down (the item before the
 * header, the header cacheline aligned). */
#include "smem.h"

#define PTABLE_MAGIC 0x434f5424u   /* "$TOC" */
#define PART_MAGIC 0x54525024u     /* "$PRT" */
#define CANARY 0xa5a5
#define PTABLE_HDR 32
#define PTABLE_ENTRY 48
#define PART_HDR 32
#define ENTRY_HDR 16

int smem_init(struct smem *s, volatile u8 *base, u32 size, u16 local, u16 remote)
{
	s->base = base;
	s->size = size;
	s->part = 0;
	if (!base || size < 8192) return -1;
	volatile u8 *pt = base + size - 4096;
	if (sm_rd32(pt) != PTABLE_MAGIC || sm_rd32(pt + 4) != 1) return -1;
	u32 n = sm_rd32(pt + 8);
	for (u32 i = 0; i < n && PTABLE_HDR + (i + 1) * PTABLE_ENTRY <= 4096; i++) {
		volatile u8 *e = pt + PTABLE_HDR + i * PTABLE_ENTRY;
		u32 off = sm_rd32(e), sz = sm_rd32(e + 4);
		u16 h0 = sm_rd16(e + 12), h1 = sm_rd16(e + 14);
		if (!off || !sz) continue;
		if (!((h0 == local && h1 == remote) || (h0 == remote && h1 == local))) continue;
		if ((u64)off + sz > size || sz < PART_HDR) return -1;
		volatile u8 *ph = base + off;
		if (sm_rd32(ph) != PART_MAGIC || sm_rd16(ph + 4) != h0 || sm_rd16(ph + 6) != h1 ||
		    sm_rd32(ph + 8) != sz)
			return -1;
		s->part = ph;
		s->part_size = sz;
		s->cacheline = sm_rd32(e + 16);
		return 0;
	}
	return -1;
}

volatile u8 *smem_get(struct smem *s, u16 item, u32 *size)
{
	volatile u8 *p = s->part;
	if (!p) return 0;
	u32 psz = s->part_size;
	u32 free_unc = sm_rd32(p + 12), free_c = sm_rd32(p + 16);
	if (free_unc > psz || free_c > psz) return 0;
	for (u32 e = PART_HDR; e + ENTRY_HDR <= free_unc; ) {
		volatile u8 *ent = p + e;
		if (sm_rd16(ent) != CANARY) return 0;
		u32 esz = sm_rd32(ent + 4);
		u16 pad_data = sm_rd16(ent + 8), pad_hdr = sm_rd16(ent + 10);
		if (esz > psz || pad_data > esz) return 0;
		if (sm_rd16(ent + 2) == item) {
			if (size) *size = esz - pad_data;
			return ent + ENTRY_HDR + pad_hdr;
		}
		e += ENTRY_HDR + pad_hdr + esz;
	}
	u32 cl = s->cacheline ? s->cacheline : 8;
	u32 hl = M_ALIGN(ENTRY_HDR, cl);
	if (psz < hl) return 0;
	for (u32 e = psz - hl; e > free_c; ) {
		volatile u8 *ent = p + e;
		if (sm_rd16(ent) != CANARY) return 0;
		u32 esz = sm_rd32(ent + 4);
		u16 pad_data = sm_rd16(ent + 8);
		if (esz > e || pad_data > esz) return 0;
		if (sm_rd16(ent + 2) == item) {
			if (size) *size = esz - pad_data;
			return ent - esz;
		}
		if (e < esz + hl) break;
		e -= esz + hl;
	}
	return 0;
}
