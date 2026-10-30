/* kfar_arc for KolibriOS for ARM: the LZMA decoder for *.7z archives
 * (lzma.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/lzma.inc,
 * based on the C decoder of Igor Pavlov's LZMA SDK, portions by diamond
 * (Evgeny Grechnikov), 2006, 2007, GPL-2.0 (kfar_arc.h).
 *
 * A stream (7z.c): its buffer is the dictionary, a ring (a distance
 * before its start is taken from its end); fillBuf stops when the
 * request is filled and goes on there (.continue: the main loop, or in
 * the middle of a match). The probabilities are words, as the
 * original's.
 *
 * Differences: the literal's position bits come from the position
 * itself (the original takes them from the position state, which loses
 * them when lp > pb); rep3 starts at 1 as rep0-rep2 (the original leaves
 * it when a decoder starts again); a match from before the buffer wraps
 * by the buffer's size (the original by the dictionary's, the same size
 * for the archives 7-Zip makes). */
#include "kfar_arc.h"

#define kNumPosBitsMax 4
#define kNumPosStatesMax (1 << kNumPosBitsMax)
#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumMidBits 3
#define kLenNumMidSymbols (1 << kLenNumMidBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)
#define LenChoice 0
#define LenChoice2 1
#define LenLow 2
#define LenMid (LenLow + (kNumPosStatesMax << kLenNumLowBits))
#define LenHigh (LenMid + (kNumPosStatesMax << kLenNumMidBits))
#define kNumLenProbs (LenHigh + kLenNumHighSymbols)
#define kNumStates 12
#define kNumLitStates 7
#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex / 2))
#define kNumPosSlotBits 6
#define kNumLenToPosStates 4
#define kNumAlignBits 4
#define kAlignTableSize (1 << kNumAlignBits)
#define kMatchMinLen 2
#define IsMatch 0
#define IsRep (IsMatch + (kNumStates << kNumPosBitsMax))
#define IsRepG0 (IsRep + kNumStates)
#define IsRepG1 (IsRepG0 + kNumStates)
#define IsRepG2 (IsRepG1 + kNumStates)
#define IsRep0Long (IsRepG2 + kNumStates)
#define PosSlot (IsRep0Long + (kNumStates << kNumPosBitsMax))
#define SpecPos (PosSlot + (kNumLenToPosStates << kNumPosSlotBits))
#define Align_ (SpecPos + kNumFullDistances - kEndPosModelIndex)
#define Lencoder (Align_ + kAlignTableSize)
#define RepLencoder (Lencoder + kNumLenProbs)
#define Literal (RepLencoder + kNumLenProbs)
#define LZMA_BASE_SIZE 1846            /* must be == Literal */
#define LZMA_LIT_SIZE 768
#define kNumTopBits 24
#define kTopValue (1u << kNumTopBits)
#define kNumBitModelTotalBits 11
#define kBitModelTotal (1u << kNumBitModelTotalBits)
#define kNumMoveBits 5

_Static_assert(Literal == LZMA_BASE_SIZE, "LZMA_BASE_SIZE");

typedef struct lzma_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	/* RangeDecoder data */
	u32 inLen;
	const u8 *inPtr;
	u32 code;
	u32 range;
	/* parameters */
	u8 pb;                  /* pos state bits (0 - 4) */
	u8 lp;                  /* literal pos state bits (0 - 4) */
	u8 lc;                  /* literal context bits (0 - 8) */
	u8 previousByte;
	u32 posStateMask;       /* (1 shl .pb)-1 */
	u32 literalPosMask;     /* (1 shl .lp)-1 */
	/* variables */
	u32 cont;               /* .continue */
	u32 ecx;                /* a match's rest */
	u8 *outEnd;
	u32 dictSize;
	u32 state;
	u32 rep0, rep1, rep2, rep3;
	u32 p[LZMA_BASE_SIZE];  /* then LZMA_LIT_SIZE shl (.lc+.lp) */
} lzma_decoder;

enum { cont_start, cont_main_loop, cont_restart_repmovsb };

/* lzma.inc: .refill: the input's next bytes (none left is an error) */
static void refill(lzma_decoder *ebp)
{
	streamInfo *eax = ebp->inStream;
	if (!eax->fullSize) return_err();
	fillBuf(eax);
	ebp->inPtr = eax->bufPtr;
	if (!eax->bufDataLen) return_err();
	ebp->inLen = eax->bufDataLen - 1;
}

/* the range coder's next byte into code */
static inline void next_code_byte(lzma_decoder *ebp)
{
	ebp->range <<= 8;
	ebp->code <<= 8;
	if (ebp->inLen-- == 0) refill(ebp);     /* .refill1 */
	ebp->code |= *ebp->inPtr++;
}

/* lzma.inc: .RangeDecoderBitDecode: in: eax -> prob; out: the bit (CF) */
static u32 RangeDecoderBitDecode(lzma_decoder *ebp, u32 *eax)
{
	u32 edx = (ebp->range >> kNumBitModelTotalBits) * *eax;
	u32 cf;
	if (ebp->code < edx) {
		ebp->range = edx;
		*eax += (kBitModelTotal - *eax) >> kNumMoveBits;
		cf = 0;
	} else {
		/* .ae */
		ebp->range -= edx;
		ebp->code -= edx;
		*eax -= *eax >> kNumMoveBits;
		cf = 1;
	}
	/* .n */
	if (ebp->range < kTopValue) next_code_byte(ebp);
	return cf;
}

/* lzma.inc: .RangeDecoderDecodeDirectBits: ecx bits, the first the
 * highest */
static u32 RangeDecoderDecodeDirectBits(lzma_decoder *ebp, u32 ecx)
{
	u32 eax = 0;
	for (; ecx; ecx--) {
		ebp->range >>= 1;
		eax <<= 1;
		if (ebp->code >= ebp->range) {
			ebp->code -= ebp->range;
			eax |= 1;
		}
		if (ebp->range < kTopValue) next_code_byte(ebp);
	}
	return eax;
}

/* lzma.inc: .RangeDecoderBitTreeDecode: in: eax -> probs, ecx =
 * numLevels */
static u32 RangeDecoderBitTreeDecode(lzma_decoder *ebp, u32 *eax, u32 ecx)
{
	u32 edx = 1;
	for (u32 i = 0; i < ecx; i++) edx = edx << 1 | RangeDecoderBitDecode(ebp, eax + edx);
	return edx - (1u << ecx);
}

/* lzma.inc: .RangeDecoderReverseBitTreeDecode */
static u32 RangeDecoderReverseBitTreeDecode(lzma_decoder *ebp, u32 *eax, u32 ecx)
{
	u32 edx = 1, ebx = 0;
	for (u32 i = 0; i < ecx; i++) {
		u32 bit = RangeDecoderBitDecode(ebp, eax + edx);
		edx = edx << 1 | bit;
		ebx |= bit << i;
	}
	return ebx;
}

/* lzma.inc: .LzmaLiteralDecode: in: eax -> probs; out: the byte */
static u8 LzmaLiteralDecode(lzma_decoder *ebp, u32 *eax)
{
	u32 ecx = 1;
	while (ecx < 0x100) ecx = ecx << 1 | RangeDecoderBitDecode(ebp, eax + ecx);
	return ecx;
}

/* lzma.inc: .LzmaLiteralDecodeMatch: in: eax -> probs, dl = matchByte */
static u8 LzmaLiteralDecodeMatch(lzma_decoder *ebp, u32 *eax, u32 dl)
{
	u32 ecx = 1;
	do {
		u32 matchBit = dl >> 7 & 1;
		dl <<= 1;
		u32 bit = RangeDecoderBitDecode(ebp, eax + 0x100 + (matchBit << 8) + ecx);
		ecx = ecx << 1 | bit;
		if (matchBit != bit) {
			while (ecx < 0x100) ecx = ecx << 1 | RangeDecoderBitDecode(ebp, eax + ecx);
			break;
		}
	} while (ecx < 0x100);
	return ecx;
}

/* lzma.inc: .LzmaLenDecode: in: eax -> prob, edx = posState; out: len */
static u32 LzmaLenDecode(lzma_decoder *ebp, u32 *eax, u32 edx)
{
	if (!RangeDecoderBitDecode(ebp, eax + LenChoice))
		return RangeDecoderBitTreeDecode(ebp, eax + LenLow + (edx << kLenNumLowBits), kLenNumLowBits);
	if (!RangeDecoderBitDecode(ebp, eax + LenChoice2))
		return RangeDecoderBitTreeDecode(ebp, eax + LenMid + (edx << kLenNumMidBits), kLenNumMidBits) +
		       kLenNumLowSymbols;
	return RangeDecoderBitTreeDecode(ebp, eax + LenHigh, kLenNumHighBits) + kLenNumLowSymbols +
	       kLenNumMidSymbols;
}

/* the byte rep0 back from edi, in the ring */
static u8 *lzma_back(lzma_decoder *ebp, u8 *edi, u32 dist)
{
	u8 *edx = edi - dist;
	if (dist > (u32)(edi - ebp->outStream.bufPtr)) {
		/* .before_buf */
		edx += ebp->outStream.bufSize;
		if (edx < ebp->outStream.bufPtr) return_err();
	}
	return edx;
}

/* lzma.inc: lzma_decoder.fillBuf */
static void lzma_fillBuf(streamInfo *s, u8 *edi, u32 ecx)
{
	lzma_decoder *ebp = (lzma_decoder *)s;
	u32 ebx = ebp->state;
	u32 len;
	if (!ecx) return;                       /* .nodata */
	ebp->outEnd = edi + ecx;
	switch (ebp->cont) {
	case cont_main_loop: goto main_loop;
	case cont_restart_repmovsb:
		len = ebp->ecx;
		goto repmovsbz;
	}
	/* .start */
	{
		streamInfo *eax = ebp->inStream;
		fillBuf(eax);
		if (eax->bufDataLen < 5) return_err();
		ebp->inLen = eax->bufDataLen - 5;
		const u8 *esi = eax->bufPtr + 1;
		ebp->code = (u32)esi[0] << 24 | esi[1] << 16 | esi[2] << 8 | esi[3];
		ebp->inPtr = esi + 4;
		ebp->range = (u32)-1;
	}
main_loop:
	while (edi < ebp->outEnd) {
		/* the position's low bits: the buffer is aligned and a multiple
		 * of 16 bytes (7z.c) */
		u32 pos = edi - ebp->outStream.bufPtr;
		u32 edx = pos & ebp->posStateMask;
		if (!RangeDecoderBitDecode(ebp, &ebp->p[IsMatch + (ebx << kNumPosBitsMax) + edx])) {
			/* a literal */
			u32 *probs = &ebp->p[Literal] + LZMA_LIT_SIZE *
				(((pos & ebp->literalPosMask) << ebp->lc) + (ebp->previousByte >> (8 - ebp->lc)));
			u8 al;
			if (ebx >= kNumLitStates)
				al = LzmaLiteralDecodeMatch(ebp, probs, *lzma_back(ebp, edi, ebp->rep0));
			else
				al = LzmaLiteralDecode(ebp, probs);      /* .literal */
			/* .got_byte */
			ebp->previousByte = al;
			*edi++ = al;
			ebx = ebx < 4 ? 0 : ebx < 10 ? ebx - 3 : ebx - 6;
			continue;
		}
		/* .1 */
		if (RangeDecoderBitDecode(ebp, &ebp->p[IsRep + ebx])) {
			if (!RangeDecoderBitDecode(ebp, &ebp->p[IsRepG0 + ebx])) {
				if (!RangeDecoderBitDecode(ebp, &ebp->p[IsRep0Long + (ebx << kNumPosBitsMax) + edx])) {
					/* a short rep: one byte from rep0 */
					ebx = ebx < 7 ? 9 : 11;
					u8 al = *lzma_back(ebp, edi, ebp->rep0);   /* .copy_byte */
					*edi++ = al;
					ebp->previousByte = al;
					continue;
				}
				/* .1101 */
			} else {
				/* .111 */
				u32 eax;
				if (!RangeDecoderBitDecode(ebp, &ebp->p[IsRepG1 + ebx])) {
					eax = ebp->rep1;                        /* .l3 */
				} else {
					/* .l1 */
					if (!RangeDecoderBitDecode(ebp, &ebp->p[IsRepG2 + ebx])) {
						eax = ebp->rep2;                /* .l2 */
					} else {
						eax = ebp->rep3;
						ebp->rep3 = ebp->rep2;
					}
					ebp->rep2 = ebp->rep1;                  /* .l2 */
				}
				ebp->rep1 = ebp->rep0;                          /* .l3 */
				ebp->rep0 = eax;
			}
			/* .1101 */
			len = LzmaLenDecode(ebp, &ebp->p[RepLencoder], edx);
			ebx = ebx < 7 ? 8 : 11;
		} else {
			/* .10: a match */
			ebp->rep3 = ebp->rep2;
			ebp->rep2 = ebp->rep1;
			ebp->rep1 = ebp->rep0;
			ebx = ebx < 7 ? 7 : 10;
			len = LzmaLenDecode(ebp, &ebp->p[Lencoder], edx);
			u32 lps = len < kNumLenToPosStates - 1 ? len : kNumLenToPosStates - 1;
			u32 slot = RangeDecoderBitTreeDecode(ebp, &ebp->p[PosSlot + (lps << kNumPosSlotBits)], kNumPosSlotBits);
			ebp->rep0 = slot;
			if (slot >= kStartPosModelIndex) {
				u32 numDirectBits = (slot >> 1) - 1;
				ebp->rep0 = (2 | (slot & 1)) << numDirectBits;
				if (slot < kEndPosModelIndex) {
					ebp->rep0 += RangeDecoderReverseBitTreeDecode(
						ebp, &ebp->p[SpecPos + ebp->rep0 - slot - 1], numDirectBits);
				} else {
					/* .l5 */
					ebp->rep0 += RangeDecoderDecodeDirectBits(ebp, numDirectBits - kNumAlignBits)
						     << kNumAlignBits;
					ebp->rep0 += RangeDecoderReverseBitTreeDecode(ebp, &ebp->p[Align_], kNumAlignBits);
				}
			}
			/* .l6 */
			if (++ebp->rep0 == 0) {
				/* the end marker */
				break;                          /* .main_loop_done */
			}
		}
		/* .repmovsb */
		len += kMatchMinLen;
	repmovsbz:
		while (len) {
			/* .repmovsbr */
			u32 eax = ebp->rep0;
			if (eax > ebp->dictSize) return_err();
			u8 *esi = edi - eax;
			u32 n = len;
			if (eax > (u32)(edi - ebp->outStream.bufPtr)) {
				/* .repmovsb0: from the end of the ring, up to its end */
				u32 before = ebp->outStream.bufPtr - esi;
				esi += ebp->outStream.bufSize;
				if (esi < ebp->outStream.bufPtr) return_err();
				if (n > before) n = before;
			}
			u32 room = ebp->outEnd - edi;
			if (!room) {
				/* .done1 */
				ebp->state = ebx;
				ebp->ecx = len;
				ebp->cont = cont_restart_repmovsb;
				return;
			}
			if (n > room) n = room;
			len -= n;
			while (n--) *edi++ = *esi++;    /* rep movsb */
		}
		/* .repmovsb1 */
		ebp->previousByte = edi[-1];
	}
	/* .main_loop_done */
	ebp->state = ebx;
	ebp->cont = cont_main_loop;
}

/* lzma.inc: lzma_get_buf_size: the properties: db lc + 9 * (lp + 5 *
 * pb), dd dictionarySize */
void lzma_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx)
{
	if (params_size < 5) return_err();
	u32 b = esi[0], lc = b % 9, lp = b / 9 % 5;
	*eax = sizeof(lzma_decoder) + (LZMA_LIT_SIZE << (lc + lp)) * 4;
	*edx = esi[1] | esi[2] << 8 | esi[3] << 16 | (u32)esi[4] << 24;
}

/* lzma.inc: lzma_init_decoder */
void lzma_init_decoder(streamInfo *s, const u8 *esi, u32 params_size, int ebx)
{
	lzma_decoder *ebp = (lzma_decoder *)s;
	u32 b = esi[0];
	ebp->lc = b % 9;
	ebp->lp = b / 9 % 5;
	ebp->pb = b / 45;
	if (ebp->pb > kNumPosBitsMax) return_err();
	ebp->dictSize = esi[1] | esi[2] << 8 | esi[3] << 16 | (u32)esi[4] << 24;
	ebp->posStateMask = (1u << ebp->pb) - 1;
	ebp->literalPosMask = (1u << ebp->lp) - 1;
	s->fillBuf = lzma_fillBuf;
	ebp->cont = cont_start;
	ebp->previousByte = 0;
	ebp->state = 0;
	ebp->rep0 = ebp->rep1 = ebp->rep2 = ebp->rep3 = 1;
	u32 ecx = Literal + (LZMA_LIT_SIZE << (ebp->lc + ebp->lp));
	for (u32 i = 0; i < ecx; i++) ebp->p[i] = kBitModelTotal / 2;
}
