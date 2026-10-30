/* kfar_arc for KolibriOS for ARM: the Deflate and Deflate64 decoders for
 * *.zip and *.7z archives (deflate.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/
 * deflate.inc, diamond (Evgeny Grechnikov), 2007, GPL-2.0 (kfar_arc.h).
 *
 * The decoder is a stream (streamInfo): fillBuf writes the next bytes
 * into its buffer of 64 KiB, which is the window too (a ring: a distance
 * before the buffer's start is taken from its end). It stops when the
 * request is filled and goes on there the next time (.continue: here
 * ebp->cont, one of the labels below). The bits come from the input
 * stream a byte at a time, dl the rest of the byte with a 1 above them.
 *
 * Differences: the fixed Huffman trees are built by
 * construct_huffman_tree from the fixed code lengths (the original
 * writes the same tables by hand); the output position (the original's
 * edi) lives in the decoder, so that deflate_unpack2 knows where the
 * data ended. */
#include "kfar_arc.h"

enum { cont_start, cont_block_loop, cont_continue_stored, cont_block_h, cont_restart_repmovsb };

static const u8 CodeLengthOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
static const u8 LengthCodesStart[28] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
					 67, 83, 99, 115, 131, 163, 195, 227 };
static const u8 LengthCodesExtra[28] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
					 5, 5, 5, 5 };
static const u32 DistCodesStart[32] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
					769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
					32769, 49153 };
static const u8 DistCodesExtra[32] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
				       11, 11, 12, 12, 13, 13, 14, 14 };

/* deflate.inc: .refill: the input's next bytes; none is an error */
static void refill(deflate_decoder *ebp)
{
	streamInfo *eax = ebp->inStream;
	fillBuf(eax);
	ebp->inPtr = eax->bufPtr;
	if (!eax->bufDataLen) return_err();
	ebp->inLen = eax->bufDataLen - 1;       /* (the byte about to be read) */
}

/* deflate.inc: .get_bit: the next bit (CF) */
static u32 get_bit(deflate_decoder *ebp)
{
	u32 cf = ebp->dl & 1;
	ebp->dl >>= 1;
	if (ebp->dl) return cf;
	if (ebp->inLen-- == 0) refill(ebp);     /* .gb_refill */
	u8 b = *ebp->inPtr++;
	ebp->dl = 0x80 | b >> 1;
	return b & 1;
}

/* deflate.inc: .get_bits: ecx bits, the first in bit 0 */
static u32 get_bits(deflate_decoder *ebp, u32 ecx)
{
	u32 eax = 0;
	for (u32 i = 0; i < ecx; i++) eax |= get_bit(ebp) << i;
	return eax;
}

/* deflate.inc: .get_word: two bytes of the input (the bits before them
 * dropped) */
static u32 get_word(deflate_decoder *ebp)
{
	if (ebp->inLen-- == 0) refill(ebp);
	u32 eax = *ebp->inPtr++;
	if (ebp->inLen-- == 0) refill(ebp);
	return eax | *ebp->inPtr++ << 8;
}

/* deflate.inc: .construct_huffman_tree: edi -> bit lengths array, ecx =
 * number of items, ebx -> tree root, size = size of tree (in words). The
 * tree: pairs of words, each a leaf (the symbol, < 0x1000), the address
 * of the next pair, or -1 (no code); the depths one after the other,
 * the leaves of a depth first (the canonical codes of Deflate) */
static void construct_huffman_tree(const u8 *edi, u32 ecx, u32 *ebx, u32 size)
{
	u32 *end = ebx + size;
	ebx[0] = ebx[1] = 0;
	u32 maxlen = 0;
	for (u32 edx = 0; edx < ecx; edx++)
		if (edi[edx] > maxlen) maxlen = edi[edx];       /* .cht1 */
	if (!maxlen) return;                    /* .chtd: no codes */
	u32 *eax = ebx + 2;                     /* the next pair */
	s32 slots = 2;
	for (u32 edx = 1; edx <= maxlen; edx++) {
		/* .cht2, .cht3: the leaves of this length */
		for (u32 i = 0; i < ecx; i++)
			if (edi[i] == edx) {
				if (--slots < 0 || ebx >= end) return_err();
				*ebx++ = i;
			}
		if (!slots) continue;           /* .cht4 */
		/* .cht5: the other slots go one level deeper */
		for (s32 n = slots; n; n--) {
			if (ebx >= end) return_err();
			if (eax >= end) {
				*ebx++ = (u32)-1;
			} else {
				*ebx++ = (u32)(uintptr_t)eax;
				eax += 2;
			}
		}
		slots *= 2;
	}
	/* the rest: no codes */
	if (slots)
		while (ebx < end) *ebx++ = (u32)-1;
}

/* deflate.inc: .get_huffman_code: ebx -> tree root; the symbol, or -1 */
static u32 get_huffman_code(deflate_decoder *ebp, const u32 *ebx)
{
	if (!ebx[1]) return 0;                  /* .ghcret */
	for (;;) {
		u32 eax = ebx[get_bit(ebp)];
		if (eax == (u32)-1 || eax < 0x1000) return eax;
		ebx = (const u32 *)(uintptr_t)eax;
	}
}

/* deflate.inc: deflate_decoder.fillBuf */
static void deflate_fillBuf(streamInfo *eax, u8 *edi, u32 ecx)
{
	deflate_decoder *ebp = (deflate_decoder *)eax;
	if (!ecx) return;                       /* .nodata */
	ebp->edi = edi;
	ebp->outEnd = edi + ecx;
	u32 len, dist;
	switch (ebp->cont) {
	case cont_block_loop: goto block_loop;
	case cont_continue_stored: goto continue_stored;
	case cont_block_h: goto block_h;
	case cont_restart_repmovsb: goto restart_repmovsb;
	}
	/* .start */
	ebp->dl = 0;
	ebp->inLen = 0;
	ebp->bLast = 0;
block_loop:
	for (;;) {
		if (ebp->edi >= ebp->outEnd) {
			/* .block_loop_done */
			ebp->cont = cont_block_loop;
			return;
		}
		if (ebp->bLast) return_err();
		ebp->bLast = get_bit(ebp);
		if (get_bit(ebp)) {
			/* .test_block_fh */
			if (get_bit(ebp)) return_err();
			/* .block_fh: Fixed Huffman */
			u8 *l = ebp->lengths;
			memset(l, 8, 144);
			memset(l + 144, 9, 112);
			memset(l + 256, 7, 24);
			memset(l + 280, 8, 8);
			memset(l + 288, 5, 32);
			construct_huffman_tree(l + 288, 32, ebp->huff_dist, 31 * 2);
			construct_huffman_tree(l, 288, ebp->huff_lit, 287 * 2);
			goto block_h;
		}
		if (!get_bit(ebp)) {
			/* .block_stored: Stored */
			ebp->dl = 0;
			u32 ecx2 = get_word(ebp);
			if ((~get_word(ebp) & 0xFFFF) != ecx2) return_err();
			ebp->blockLen = ecx2;
			goto continue_stored;
		}
		/* .block_dh: Dynamic Huffman */
		memset(ebp->lit_len, 0, sizeof(ebp->lit_len));
		u32 hlit = get_bits(ebp, 5);
		u32 hdist = get_bits(ebp, 5);
		u32 hclen = get_bits(ebp, 4) + 4;
		for (u32 i = 0; i < hclen; i++) ebp->lit_len[CodeLengthOrder[i]] = get_bits(ebp, 3);
		construct_huffman_tree(ebp->lit_len, 19, ebp->huff_bl, 18 * 2);
		u32 ecx = hlit + hdist + 258;
		u8 *d = ebp->lengths;
		while (ecx) {
			/* .dhl */
			u32 c = get_huffman_code(ebp, ebp->huff_bl);
			if (c < 16) {
				*d++ = c;
				ecx--;
				continue;
			}
			/* .dh_special */
			u32 n;
			u8 v = 0;
			if (c == 16) {
				n = get_bits(ebp, 2) + 3;
				v = d[-1];
			} else if (c == 17) {
				n = get_bits(ebp, 3) + 3;       /* .dh_0 */
			} else if (c == 18) {
				n = get_bits(ebp, 7) + 11;
			} else {
				return_err();
			}
			if (n > ecx) return_err();
			memset(d, v, n);
			d += n;
			ecx -= n;
		}
		/* .dhd */
		construct_huffman_tree(ebp->lengths + hlit + 257, hdist + 1, ebp->huff_dist, 31 * 2);
		construct_huffman_tree(ebp->lengths, hlit + 257, ebp->huff_lit, 287 * 2);
		goto block_h;
	}
continue_stored:
	{
		u32 ecx = ebp->blockLen;
		u32 room = ebp->outEnd - ebp->edi;
		if (room < ecx) ecx = room;
		ebp->blockLen -= ecx;
		while (ecx) {
			/* .bs_loop */
			if (!ebp->inLen) {
				refill(ebp);
				ebp->inLen++;           /* (no byte read yet) */
			}
			u32 n = ecx < ebp->inLen ? ecx : ebp->inLen;
			ebp->inLen -= n;
			memcpy(ebp->edi, ebp->inPtr, n);
			ebp->edi += n;
			ebp->inPtr += n;
			ecx -= n;
		}
		if (!ebp->blockLen) goto block_loop;
		ebp->cont = cont_continue_stored;
		return;
	}
block_h:
	for (;;) {
		u32 sym = get_huffman_code(ebp, ebp->huff_lit);
		if (sym < 256) {
			*ebp->edi++ = sym;
		} else {
			/* .not_char */
			sym -= 256;
			if (!sym) goto block_loop;
			if (sym > 285 - 256) return_err();
			if (sym == 285 - 256) {
				/* .h_max */
				len = 258;
				if (ebp->bDeflate64) len = get_bits(ebp, 16) + 3;
			} else {
				len = LengthCodesStart[sym - 1] + get_bits(ebp, LengthCodesExtra[sym - 1]);
			}
			/* .length_known */
			u32 d = get_huffman_code(ebp, ebp->huff_dist);
			if (d >= 32) return_err();
			dist = DistCodesStart[d] + get_bits(ebp, DistCodesExtra[d]);
			/* ecx = length, ebx = distance */
		repmovsbz:
			while (len) {
				/* .repmovsbr: from the window, the buffer's end before its start */
				u8 *esi = ebp->edi - dist;
				u32 n = len;
				if (esi < ebp->outStream.bufPtr) {
					/* .repmovsb0 */
					u32 before = ebp->outStream.bufPtr - esi;
					esi += 0x10000;
					if (n > before) n = before;
				}
				u32 room = ebp->outEnd - ebp->edi;
				if (n > room) n = room;
				len -= n;
				u8 *edi = ebp->edi;
				while (n--) *edi++ = *esi++;    /* rep movsb (overlapping) */
				ebp->edi = edi;
				if (len && ebp->edi >= ebp->outEnd) {
					/* .repmovsbc */
					ebp->blockLen = len;
					*(u32 *)ebp->lit_len = dist;
					ebp->cont = cont_restart_repmovsb;
					return;
				}
			}
			/* .repmovsb1 */
		}
		/* .bhc */
		if (ebp->edi >= ebp->outEnd) {
			ebp->cont = cont_block_h;
			return;
		}
	}
restart_repmovsb:
	len = ebp->blockLen;
	dist = *(u32 *)ebp->lit_len;
	goto repmovsbz;
}

/* deflate.inc: deflate_get_buf_size: the decoder's data, its buffer */
void deflate_get_buf_size(u32 *eax, u32 *edx)
{
	*eax = sizeof(deflate_decoder);
	*edx = 0x10000;
}

/* deflate.inc: deflate_init_decoder, deflate64_init_decoder */
void deflate_init_decoder(streamInfo *ebp)
{
	((deflate_decoder *)ebp)->bDeflate64 = 0;
	ebp->fillBuf = deflate_fillBuf;
	((deflate_decoder *)ebp)->cont = cont_start;
}

void deflate64_init_decoder(streamInfo *ebp)
{
	deflate_init_decoder(ebp);
	((deflate_decoder *)ebp)->bDeflate64 = 1;
}
