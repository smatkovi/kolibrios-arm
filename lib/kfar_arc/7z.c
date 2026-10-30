/* kfar_arc for KolibriOS for ARM: the loader of 7z archives and the
 * streams of the decoders (7z.inc) */
#include "kfar_arc.h"

const u32 handle_7z_basesize = sizeof(handle_common), file_in_7z_size = sizeof(file_common);

/* 7z.inc: fillBuf */
void fillBuf(streamInfo *eax)
{
	eax->bufDataLen = 0;
	fillBufNozero(eax);
}

/* 7z.inc: fillBufNozero */
void fillBufNozero(streamInfo *eax)
{
	u32 edx = eax->bufSize - eax->bufDataLen;
	u32 ecx = eax->fullSize < edx ? eax->fullSize : edx;
	eax->fullSize -= ecx;
	u8 *edi = eax->bufPtr + eax->bufDataLen;
	eax->bufDataLen += ecx;
	eax->fillBuf(eax, edi, ecx);
}

/* 7z.inc: read_7z_to_buf */
u8 *read_7z_to_buf(streamInfo *eax, u8 *edi)
{
	do {
		u32 esi = eax->bufDataLen;
		if (esi == eax->bufSize) {
			esi = 0;
			eax->bufDataLen = 0;
		}
		fillBufNozero(eax);
		u32 ecx = eax->bufDataLen - esi;
		memcpy(edi, eax->bufPtr + esi, ecx);
		edi += ecx;
	} while ((u32)eax->fullSize);
	return edi;
}

/* 7z.inc: skip_7z */
void skip_7z(streamInfo *eax)
{
	do {
		if (eax->bufDataLen == eax->bufSize) eax->bufDataLen = 0;
		fillBufNozero(eax);
	} while (eax->fullSize);
}

HANDLE open_7z(HANDLE basefile, const u8 *data, int datasize, const void *attr) { return 0; }
void close_7z(handle_common *hPlugin) {}
void getattr_7z(handle_common *ebp, file_common *eax, u8 *edi) {}
void *open_file_7z(handle_common *ebp, file_common *eax, int edi) { return 0; }
unsigned read_7z(void *ebx, void *buf, unsigned size) { return -1; }
void setpos_7z(void *ebx, u64 pos) {}
void close_file_7z(void *ebx) {}
void init_aes(void) {}
void init_ppmd(void) {}
