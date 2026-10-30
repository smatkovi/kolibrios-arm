/* kfar_arc for KolibriOS for ARM: the branch filters for 7-Zip archives,
 * BCJ and BCJ2 (7zbranch.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/
 * 7zbranch.inc, ported there from 7-Zip's C++ sources (c) Igor Pavlov,
 * by diamond (Evgeny Grechnikov), GPL-2.0 (kfar_arc.h).
 *
 * Both are streams (7z.c) that turn the x86 code's call and jump
 * targets back from absolute to relative. A converted dword that does not
 * fit into the request is kept for the next one (numRest, dwordRest), as
 * in the original. The original's BCJ reads the four bytes after an E8
 * or E9 across the end of its input's buffer through a dword of its own
 * (tempDword) and gives them back to the input when they are not a
 * target; here they go back into a small queue of their own (pend). */
#include "kfar_arc.h"

#define kNumBitModelTotalBits 11
#define kBitModelTotal (1u << kNumBitModelTotalBits)
#define kNumMoveBits 5
#define kTopValue (1u << 24)

/* ---- BCJ ---- */

typedef struct bcj_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	u32 inPtr;              /* offset in the input's buffer */
	u32 inSize;             /* the bytes left there */
	u32 nowPos;             /* offset in stream: of the next output byte */
	u32 prevPos;            /* the position after the previous E8/E9 */
	u8 prevMask;
	u8 numRest;
	u8 bPrev;               /* prevPos is set (the original: prevPos != 0) */
	u8 npend;
	u32 dwordRest;
	u8 pend[4];             /* input given back (the original's tempDword) */
} bcj_decoder;

static fillBuf_t bcj_fillBuf;

void bcj_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx)
{
	*eax = sizeof(bcj_decoder);
	*edx = 0x4000;
}

void bcj_init_decoder(streamInfo *s, const u8 *esi, u32 params_size, int ebx)
{
	bcj_decoder *ebp = (bcj_decoder *)s;
	s->fillBuf = bcj_fillBuf;
	ebp->inPtr = 0;
	ebp->inSize = 0;
	ebp->prevPos = 0;
	ebp->bPrev = 0;
	ebp->nowPos = 0;
	ebp->numRest = 0;
	ebp->npend = 0;
}

/* the input bytes there are still: in the queue, the buffer, the stream */
static u64 bcj_available(bcj_decoder *ebx)
{
	return ebx->npend + ebx->inSize + ebx->inStream->fullSize;
}

/* the next input byte (bcj_fillBuf.refill1, .refill0) */
static u8 bcj_get(bcj_decoder *ebx)
{
	if (ebx->npend) {
		u8 al = ebx->pend[0];
		memmove(ebx->pend, ebx->pend + 1, --ebx->npend);
		return al;
	}
	if (!ebx->inSize) {
		/* .refill0 */
		streamInfo *eax = ebx->inStream;
		fillBuf(eax);
		if (!eax->bufDataLen) return_err();
		ebx->inPtr = 0;
		ebx->inSize = eax->bufDataLen;
	}
	ebx->inSize--;
	return ebx->inStream->bufPtr[ebx->inPtr++];
}

static const u8 bcj_kMaskToBitNumber[8] = { 24, 16, 8, 8, 0, 0, 0, 0 };

/* 7zbranch.inc: bcj_fillBuf */
static void bcj_fillBuf(streamInfo *eax, u8 *edi, u32 ecx)
{
	bcj_decoder *ebx = (bcj_decoder *)eax;
	u32 ebp = ecx;                          /* save output size */
	/* the bytes of a dword the last request had no room for */
	while (ebx->numRest) {
		if (!ebp--) return;
		ebx->numRest--;
		*edi++ = ebx->dwordRest;
		ebx->nowPos++;
		ebx->dwordRest >>= 8;
	}
	while (ebp) {
		/* .mainloop */
		ebp--;
		u8 al = bcj_get(ebx);
		*edi++ = al;
		ebx->nowPos++;
		if (al != 0xE8 && al != 0xE9) continue;
		/* .filter: the four bytes after it, if there are four */
		if (bcj_available(ebx) < 4) continue;
		u8 b[4];
		for (int i = 0; i < 4; i++) b[i] = bcj_get(ebx);
		/* .posok */
		u32 d = ebx->bPrev ? ebx->nowPos - ebx->prevPos : 6;
		u32 edx = 0;
		if (d <= 5) {
			edx = ebx->prevMask;
			for (; d; d--) edx = (edx & 0x77) << 1;
		}
		/* .maskok */
		ebx->prevMask = edx;
		ebx->prevPos = ebx->nowPos;
		ebx->bPrev = 1;
		if ((u8)(b[3] + 1) >= 2) {
			/* .miss */
			ebx->prevMask |= 1;
		give_back:
			/* not a target: the four bytes are input again (the
			 * queue is empty here: they took what it had) */
			memcpy(ebx->pend, b, 4);
			ebx->npend = 4;
			continue;
		}
		if (edx >= 0x20 || (edx & (edx - 1))) {
			/* .miss2 */
			ebx->prevMask |= 0x10 | 1;
			goto give_back;
		}
		/* a target: absolute to relative */
		u32 src = b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
		u32 cl = bcj_kMaskToBitNumber[edx >> 1];
		u32 dest;
		for (;;) {
			dest = src - (ebx->nowPos + 4);
			if (cl == 24) break;
			if ((u8)((dest >> cl) + 1) >= 2) break;
			src = dest ^ ((0x100u << cl) - 1);
		}
		dest = (s32)(dest << 7) >> 7;
		if (ebp < 4) {
			/* .finalize_dword: the rest next time */
			ebx->numRest = 4;
			for (; ebp; ebp--) {
				*edi++ = dest;
				ebx->nowPos++;
				ebx->numRest--;
				dest >>= 8;
			}
			ebx->dwordRest = dest;           /* .save_dword */
			return;
		}
		ebp -= 4;
		for (int i = 0; i < 4; i++, dest >>= 8) *edi++ = dest;
		ebx->nowPos += 4;
	}
	/* .mainloopdone */
}

/* ---- BCJ2 ---- */

typedef struct bcj2_decoder {
	streamInfo outStream;
	streamInfo *mainInStream;
	streamInfo *callStream;
	streamInfo *jumpStream;
	streamInfo *rangeDecoder;
	u32 dwordRest;
	u8 prevByte;
	u8 numRest;
	u8 bInited;
	u8 pad;
	const u8 *inPtr;
	u32 inSize;
	const u8 *callPtr;
	const u8 *jumpPtr;
	u32 callSize;
	u32 jumpSize;
	const u8 *rangeDecPtr;
	u32 rangeDecSize;
	u32 nowPos;
	u32 range;
	u32 code;
	u32 statusE9Decoder;
	u32 statusJccDecoder;
	u32 statusE8Decoder[256];
} bcj2_decoder;

void bcj2_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx)
{
	*eax = sizeof(bcj2_decoder);
	*edx = 0x4000;
}

static fillBuf_t bcj2_fillBuf;

void bcj2_init_decoder(streamInfo *s, const u8 *esi, u32 params_size, int ebx)
{
	bcj2_decoder *ebp = (bcj2_decoder *)s;
	s->fillBuf = bcj2_fillBuf;
	ebp->statusE9Decoder = ebp->statusJccDecoder = kBitModelTotal / 2;
	for (int i = 0; i < 256; i++) ebp->statusE8Decoder[i] = kBitModelTotal / 2;
	ebp->prevByte = ebp->numRest = ebp->bInited = 0;
	ebp->inSize = ebp->callSize = ebp->jumpSize = ebp->rangeDecSize = 0;
	ebp->nowPos = 0;
}

/* bcj2_fillBuf.RangeDecoderBitDecode (the LZMA decoder's) */
static u32 bcj2_bit(bcj2_decoder *ebp, u32 *eax)
{
	u32 edx = (ebp->range >> kNumBitModelTotalBits) * *eax;
	u32 cf;
	if (ebp->code < edx) {
		ebp->range = edx;
		*eax += (kBitModelTotal - *eax) >> kNumMoveBits;
		cf = 0;
	} else {
		ebp->range -= edx;
		ebp->code -= edx;
		*eax -= *eax >> kNumMoveBits;
		cf = 1;
	}
	if (ebp->range < kTopValue) {
		ebp->range <<= 8;
		ebp->code <<= 8;
		if (ebp->rangeDecSize-- == 0) {
			/* .refill3 */
			streamInfo *s = ebp->rangeDecoder;
			fillBuf(s);
			if (!s->bufDataLen) return_err();
			ebp->rangeDecSize = s->bufDataLen - 1;
			ebp->rangeDecPtr = s->bufPtr;
		}
		ebp->code |= *ebp->rangeDecPtr++;
	}
	return cf;
}

/* a big-endian dword of the call or the jump stream (.getptr, .refill2) */
static u32 bcj2_getptr(streamInfo *s, const u8 **ptr, u32 *size)
{
	if (*size < 4) {
		/* (the original assumes the dwords do not cross its buffers) */
		fillBuf(s);
		if (s->bufDataLen < 4) return_err();
		*ptr = s->bufPtr;
		*size = s->bufDataLen;
	}
	const u8 *p = *ptr;
	*ptr += 4;
	*size -= 4;
	return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void bcj2_fillBuf(streamInfo *s, u8 *edi, u32 ecx)
{
	bcj2_decoder *ebp = (bcj2_decoder *)s;
	if (!ebp->bInited) {
		/* .init: the range coder's first five bytes */
		streamInfo *eax = ebp->rangeDecoder;
		fillBuf(eax);
		if (eax->bufDataLen < 5) return_err();
		ebp->rangeDecSize = eax->bufDataLen - 5;
		const u8 *edx = eax->bufPtr + 5;
		ebp->rangeDecPtr = edx;
		ebp->code = (u32)edx[-4] << 24 | edx[-3] << 16 | edx[-2] << 8 | edx[-1];
		ebp->range = (u32)-1;
		ebp->bInited = 1;
	}
	/* .inited */
	while (ebp->numRest) {
		if (!ecx--) return;
		ebp->numRest--;
		u8 al = ebp->dwordRest;
		*edi++ = al;
		ebp->nowPos++;
		ebp->prevByte = al;
		ebp->dwordRest >>= 8;
	}
	while (ecx) {
		/* .mainloop */
		ecx--;
		if (ebp->inSize-- == 0) {
			/* .refill1 */
			streamInfo *eax = ebp->mainInStream;
			fillBuf(eax);
			if (!eax->bufDataLen) return_err();
			ebp->inSize = eax->bufDataLen - 1;
			ebp->inPtr = eax->bufPtr;
		}
		/* .filled1 */
		u8 al = *ebp->inPtr++;
		*edi++ = al;
		ebp->nowPos++;
		u32 hit;
		bool call = false;
		if (al == 0xE8) {
			/* .e8 */
			u8 prev = ebp->prevByte;
			ebp->prevByte = al;
			hit = bcj2_bit(ebp, &ebp->statusE8Decoder[prev]);
			call = true;
		} else if (al == 0xE9) {
			/* .e9 */
			ebp->prevByte = al;
			hit = bcj2_bit(ebp, &ebp->statusE9Decoder);
		} else {
			bool jcc = ebp->prevByte == 0xF && (al & 0xF0) == 0x80;
			ebp->prevByte = al;
			if (!jcc) continue;
			/* .jcc */
			hit = bcj2_bit(ebp, &ebp->statusJccDecoder);
		}
		if (!hit) continue;
		/* .getptr */
		u32 eax = call ? bcj2_getptr(ebp->callStream, &ebp->callPtr, &ebp->callSize)
			       : bcj2_getptr(ebp->jumpStream, &ebp->jumpPtr, &ebp->jumpSize);
		eax -= ebp->nowPos + 4;
		if (ecx < 4) {
			/* .finalize_dword */
			ebp->numRest = 4;
			for (; ecx; ecx--) {
				*edi++ = eax;
				ebp->nowPos++;
				ebp->numRest--;
				eax >>= 8;
			}
			ebp->dwordRest = eax;   /* .save_dword */
			return;
		}
		ecx -= 4;
		for (int i = 0; i < 4; i++) edi[i] = eax >> (8 * i);
		edi += 4;
		ebp->nowPos += 4;
		ebp->prevByte = eax >> 24;
	}
	/* .mainloopdone */
}
