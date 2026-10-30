/* kfar_arc for KolibriOS for ARM: password handling in 7-Zip, the
 * "7zAES" filter (SHA-256 + AES-256) (7zaes.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/7zaes.inc,
 * ported there from 7-Zip's C++ sources (c) Igor Pavlov, by diamond
 * (Evgeny Grechnikov), GPL-2.0 (kfar_arc.h).
 *
 * A stream (7z.c): AES-256 in CBC mode over its input, the key from the
 * password (query_password: the user is asked once) and the salt, by
 * 2^NumCyclesPower rounds of SHA-256. The original decrypts a whole
 * block past the end of a request that is not a multiple of 16 bytes and
 * skips it the next time (bufRest); here the rest of the block waits in
 * the decoder. */
#include "kfar_arc.h"
#include "sha256.h"

typedef struct aes7z_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	u32 inLen;              /* the input's bytes left in its buffer */
	const u8 *inPtr;
	u32 bufRest;            /* the decrypted block's bytes not given yet */
	/* key data */
	u32 NumCyclesPower;
	u32 SaltSize;
	u8 Salt[16];
	/* AES data */
	u8 iv[16];
	u8 Key[32];
	aes_data aes;
	u8 rest[16];
} aes7z_decoder;

/* 7zaes.inc: aes7z_decoder.fillBuf */
static void aes7z_fillBuf(streamInfo *s, u8 *edi, u32 ecx)
{
	aes7z_decoder *ebp = (aes7z_decoder *)s;
	/* the rest of the last block */
	u32 n = ebp->bufRest < ecx ? ebp->bufRest : ecx;
	memcpy(edi, ebp->rest + 16 - ebp->bufRest, n);
	ebp->bufRest -= n;
	edi += n;
	ecx -= n;
	while (ecx) {
		/* .mainloop */
		if (ebp->inLen < 16) {
			/* .refill */
			if (ebp->inLen) {
				/* .rest: a tail shorter than a block (the stream's
				 * end): as it is */
				if (ebp->inLen < ecx) return_err();
				memcpy(edi, ebp->inPtr, ecx);
				ebp->inPtr += ecx;
				ebp->inLen -= ecx;
				return;
			}
			streamInfo *eax = ebp->inStream;
			fillBuf(eax);
			if (!eax->bufDataLen) return_err();
			ebp->inPtr = eax->bufPtr;
			ebp->inLen = eax->bufDataLen;
			continue;
		}
		/* .refilled: a block, CBC */
		u8 out[16];
		aes_decode(ebp->inPtr, out, &ebp->aes);
		for (int i = 0; i < 16; i++) out[i] ^= ebp->iv[i];
		memcpy(ebp->iv, ebp->inPtr, 16);
		ebp->inPtr += 16;
		ebp->inLen -= 16;
		if (ecx < 16) {
			/* .rest1 */
			memcpy(edi, out, ecx);
			memcpy(ebp->rest, out, 16);
			ebp->bufRest = 16 - ecx;
			return;
		}
		memcpy(edi, out, 16);
		edi += 16;
		ecx -= 16;
	}
	/* .done */
}

void aes7z_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx)
{
	*eax = sizeof(aes7z_decoder);
	*edx = 0x4000;
}

/* 7zaes.inc: aes7z_init_decoder: the properties (NumCyclesPower, the
 * salt and iv sizes, the salt, the iv), then the key (ebx = 1: again,
 * the key is there) */
void aes7z_init_decoder(streamInfo *s, const u8 *esi, u32 params_size, int ebx)
{
	aes7z_decoder *ebp = (aes7z_decoder *)s;
	/* zero all */
	ebp->inLen = 0;
	ebp->bufRest = 0;
	ebp->NumCyclesPower = 0;
	ebp->SaltSize = 0;
	memset(ebp->Salt, 0, sizeof(ebp->Salt));
	memset(ebp->iv, 0, sizeof(ebp->iv));
	s->fillBuf = aes7z_fillBuf;
	/* parse parameters */
	if (params_size) {
		u8 cl = esi[0];
		ebp->NumCyclesPower = cl & 0x3F;
		if (cl & 0xC0) {
			ebp->SaltSize = cl >> 7 & 1;
			u32 ivSize = cl >> 6 & 1;
			if (params_size < 2) return_err();
			ebp->SaltSize += esi[1] >> 4;
			ivSize += esi[1] & 0xF;
			if (params_size < 2 + ebp->SaltSize + ivSize) return_err();
			memcpy(ebp->Salt, esi + 2, ebp->SaltSize);
			memcpy(ebp->iv, esi + 2 + ebp->SaltSize, ivSize);
		}
	}
	/* .parok: if reinitializing - all calculations have been already done */
	if (ebx) return;
	if (!query_password()) return_clear();
	/* .CalculateDigest */
	if (ebp->NumCyclesPower == 0x3F) {
		/* the salt and the password themselves */
		u32 edx = 32 - ebp->SaltSize;
		memcpy(ebp->Key, ebp->Salt, ebp->SaltSize);
		u32 n = password_size * 2 < edx ? password_size * 2 : edx;
		memcpy(ebp->Key + ebp->SaltSize, password_unicode, n);
		memset(ebp->Key + ebp->SaltSize + n, 0, edx - n);
	} else {
		/* .sha: 2^NumCyclesPower rounds of the salt, the password and a
		 * counter of 8 bytes */
		u64 rounds = (u64)1 << ebp->NumCyclesPower;
		u8 counter[8] = { 0 };
		sha256_init();
		for (; rounds; rounds--) {
			/* .loop */
			sha256_update(ebp->Salt, ebp->SaltSize);
			sha256_update(password_unicode, password_size * 2);
			sha256_update(counter, 8);
			for (int i = 0; i < 8 && !++counter[i]; i++);
		}
		sha256_final(ebp->Key);
	}
	/* .setkey: 7z uses 256-bit keys */
	aes_setkey(ebp->Key, 8, &ebp->aes);
}
