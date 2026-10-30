/* kfar_arc for KolibriOS for ARM: the loader of 7z archives and the
 * streams of the decoders (7z.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/7z.inc,
 * diamond (Evgeny Grechnikov), 2007, GPL-2.0 (kfar_arc.h); the loading
 * was ported there from 7-Zip 4.42's sources (7zip/Archive/7z/7zIn.cpp,
 * (C) 1999-2006 Igor Pavlov).
 *
 * open_7z reads the header (an encoded one decoded first), the streams'
 * information (the folders with their coders in one block, pointers
 * from its start, built with offsets while it grows) and the files into
 * the handle's records. A file is read through the decoders of its folder
 * (_7z.decode.init: each coder a stream, streamInfo, with a buffer, the
 * packed streams read from the archive), from the folder's start; the
 * last folder's decoders are kept for the next file after it (7-Zip's
 * solid archives).
 *
 * Differences: an empty file in the middle of a folder's files does not
 * take a stream of it (as 7-Zip's FillFolderStartFileIndex; the original
 * counts it); a folder without its attribute is a folder (0x10) all the
 * same; setpos forward adds to the position correctly (the original adds
 * the high dword to the low one); the archive's host is only closed when
 * it is there; open_file_7z sets password_size with the handle's
 * password; the buffers are aligned to 16 bytes, the decoders' data to 8.
 * Coders 7-Zip has and the original has not (ARM, PPC, Delta, BZip2,
 * LZMA2, ...) are refused with the original's "Unknown method". */
#include "kfar_arc.h"

enum { kEnd = 0, kHeader = 1, kArchiveProperties = 2, kAdditionalStreamsInfo = 3, kMainStreamsInfo = 4,
       kFilesInfo = 5, kPackInfo = 6, kUnPackInfo = 7, kSubStreamsInfo = 8, kSize = 9, kCRC = 10,
       kFolder = 11, kCodersUnPackSize = 12, kNumUnPackStream = 13, kEmptyStream = 14, kEmptyFile = 15,
       kAnti = 16, kName = 17, kCreationTime = 18, kLastAccessTime = 19, kLastWriteTime = 20,
       kWinAttributes = 21, kComment = 22, kEncodedHeader = 23, kStartPos = 24 };

typedef struct file_in_7z {
	file_common c;
	u32 attr;
	u32 FileCRC;
	u64 startPos;
	u8 CreationTime[8];             /* (70.1's time and date) */
	u8 LastAccessTime[8];
	u8 LastWriteTime[8];
	u64 UnPackSize;
	u32 folder;
	u32 folder_index;
	u64 folderStart;
	u8 bAttrDefined;
	u8 bStartPosDefined;
	u8 bCRCDefined;
	u8 bCreationTimeDefined;
	u8 bLastAccessTimeDefined;
	u8 bLastWriteTimeDefined;
	u8 bHasStream;
	u8 bAnti;
} file_in_7z;

/* a folder (_7z.StreamsInfo's virtual part); its coders' info: a list of
 * records (dd NextCoder; db MethodID.IDSize; times IDSize db
 * MethodID.ID; dd NumInStreams; dd NumOutStreams; dd PropSize; times
 * PropSize db Properties), the alternatives through NextCoder */
typedef struct folder_7z {
	u32 numCoders;
	u32 *bindPairs;                 /* { inIndex, outIndex } */
	u32 numBindPairs;
	u32 *packedStreams;
	u32 numPackedStreams;
	u32 startPackedStream;
	u32 numOutStreams;
	u64 *unpackSizes;
	u32 mainOutStream;
	u32 numUnPackStreams;
	u64 packOffset;
	u8 *fsz[];
} folder_7z;
_Static_assert(sizeof(folder_7z) % 8 == 0, "the coders' heads follow");

typedef struct handle_7z {
	handle_common c;
	char *names_buf;
	HANDLE host;
	u32 host_datetime[6];
	/* streams info */
	u32 numPackStreams;
	u64 *packSizes;
	u32 numFolders;
	u8 *unpackCRCsDefined;
	u32 *unpackCRCs;
	folder_7z **folders;            /* the folders' block */
	u32 last_folder;
	u64 last_pos;
	u8 *last_context;
	streamInfo *last_main_stream;
	int password_len;               /* -1 if no password defined; in characters */
	u16 password[password_maxlen];
} handle_7z;

const u32 handle_7z_basesize = sizeof(handle_7z), file_in_7z_size = sizeof(file_in_7z);

static handle_common empty_7z_handle = { .type = type_7z };

/* the coder's record */
static inline u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static inline void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#define CODER_NEXT(c) get32(c)
#define CODER_IDSIZE(c) ((c)[4])
#define CODER_NUMIN(c) get32((c) + 5 + (c)[4])
#define CODER_NUMOUT(c) get32((c) + 9 + (c)[4])
#define CODER_PROPSIZE(c) get32((c) + 13 + (c)[4])
#define CODER_PROPS(c) ((c) + 17 + (c)[4])

/* ---- the header's reading: kfar_arc.asm's bufptr, bufend; the
 * original's edi is cur ---- */

static u8 *bufptr = buffer, *bufend;
static u32 bufsize;
static const u8 *cur;

/* _7z.StreamsInfo */
static struct {
	u32 numPackStreams;
	u64 *packSizes;
	u32 numFolders;
	u8 *unpackCRCsDefined;
	u32 *unpackCRCs;
	u8 *folders;
	u32 folders_size, folders_alloc;
	u64 dataOffset;
	u8 *packCRCsDefined;
	u32 *packCRCs;
	u32 numSubStreams;
	u64 *SubStreamsSizes;
	u8 *SubStreamsCRCsDefined;
	u32 *SubStreamsCRCs;
	u8 *tmpSubStreamsCRCsDefined;
	u32 *tmpSubStreamsCRCs;
} SI;
/* SI's memory is ReadStreamsInfo's until the handle takes it (its
 * .error and .clear, the error_proc after it returns, free it) */
static bool si_pending;

static struct { u32 size; u8 *ptr; } _7z_unpbuf[8], *_7z_unpacked;
static u32 _7z_CurInStream;
static void *_7z_dataVector;            /* the unpacked additional streams */
static u32 _7z_dataVectorSize;
static u64 *_7z_FreeSubStreams;
static u8 *_7z_tmpVector, *_7z_emptyStreams, *_7z_emptyFiles, *_7z_antiFiles;
static u32 _7z_numEmptyStreams;
static const u8 *_7z_saved_edi, *_7z_saved_edi_end;

/* _7z.decode's */
static u8 *_7z_decode_mainbuf;
static streamInfo **_7z_decode_pStreams, *_7z_decode_pStreams_def[8];

/* 7z.inc: _7z.ReadByte, _7z.ReadWord, _7z.ReadDword */
static u8 _7z_ReadByte(void)
{
	if (cur >= bufend) return_err();
	return *cur++;
}

static u32 _7z_ReadWord(void)
{
	if (bufend - cur < 2) return_err();
	cur += 2;
	return cur[-2] | cur[-1] << 8;
}

static u32 _7z_ReadDword(void)
{
	if (bufend - cur < 4) return_err();
	cur += 4;
	return get32(cur - 4);
}

/* 7z.inc: _7z.SkipSize */
static void _7z_SkipSize(u64 n)
{
	if (n > (u64)(bufend - cur)) return_err();
	cur += n;
}

/* 7z.inc: _7z.ReadNumber: 7z's NUMBER (the first byte's leading 1 bits
 * say how many bytes follow, its other bits are the top) */
static u64 _7z_ReadNumber(void)
{
	u32 al = _7z_ReadByte(), ecx = 0;
	while (ecx < 8 && (al << ecx & 0x80)) ecx++;
	u64 r = ecx < 8 ? (u64)(al & (0xFF >> (ecx + 1))) << (8 * ecx) : 0;
	for (u32 i = 0; i < ecx; i++) r |= (u64)_7z_ReadByte() << (8 * i);
	return r;
}

/* 7z.inc: _7z.ReadNum: a NUMBER below 2^31 */
static u32 _7z_ReadNum(void)
{
	u64 n = _7z_ReadNumber();
	if (n > 0x7FFFFFFF) return_err();
	return n;
}

/* 7z.inc: _7z.SkipData */
static void _7z_SkipData(void)
{
	_7z_SkipSize(_7z_ReadNumber());
}

/* 7z.inc: _7z.WaitAttribute: up to the property ecx */
static void _7z_WaitAttribute(u32 ecx)
{
	for (;;) {
		u64 eax = _7z_ReadNumber();
		if (eax == ecx) return;
		if (!eax) return_err();
		_7z_SkipData();
	}
}

/* 7z.inc: _7z.ReadBoolVector: ecx bits, the first the highest */
static void _7z_ReadBoolVector(u8 *ebx, u32 ecx)
{
	u32 al = 0, n = 0;
	for (u32 i = 0; i < ecx; i++) {
		if (!n) {
			al = _7z_ReadByte();
			n = 8;
		}
		ebx[i] = al >> --n & 1;
	}
}

/* 7z.inc: _7z.ReadBoolVector2: all set, or the bits */
static void _7z_ReadBoolVector2(u8 *ebx, u32 ecx)
{
	if (!_7z_ReadByte()) {
		_7z_ReadBoolVector(ebx, ecx);
		return;
	}
	memset(ebx, 1, ecx);
}

/* 7z.inc: _7z.ReadHashDigests: in: ebx -> bool vector, esi -> crc, ecx
 * = size */
static void _7z_ReadHashDigests(u8 *ebx, u32 *esi, u32 ecx)
{
	_7z_ReadBoolVector2(ebx, ecx);
	for (u32 i = 0; i < ecx; i++) esi[i] = ebx[i] ? _7z_ReadDword() : 0;
}

/* 7z.inc: _7z.SwitchStream: the data in an additional stream (its
 * "external" byte), _7z.UnswitchStream back */
static bool _7z_SwitchStream(void)
{
	_7z_saved_edi_end = bufend;
	if (!_7z_ReadByte()) return false;
	u32 eax = _7z_ReadNum();
	_7z_saved_edi = cur;
	if (eax >= _7z_dataVectorSize) return_err();
	typeof(_7z_unpbuf[0]) *v = _7z_dataVector;
	cur = v[eax].ptr;
	bufend = v[eax].ptr + v[eax].size;
	return true;
}

static void _7z_UnswitchStream(void)
{
	if (_7z_saved_edi_end == bufend) return;
	bufend = (u8 *)_7z_saved_edi_end;
	cur = _7z_saved_edi;
}

/* _7z.ReadStreamsInfo.folders_alloc: ecx bytes more of the folders'
 * block (aligned to align); their offset */
static u32 folders_alloc(u32 ecx, u32 align)
{
	u32 eax = (SI.folders_size + align - 1) & ~(align - 1);
	if (eax + ecx > SI.folders_alloc) {
		u32 n = ((eax + ecx) | 0xFFF) + 1;
		u8 *p = pgrealloc(SI.folders, n);
		if (!p) return_clear();
		memset(p + SI.folders_alloc, 0, n - SI.folders_alloc);
		SI.folders = p;
		SI.folders_alloc = n;
	}
	SI.folders_size = eax + ecx;
	return eax;
}

#define FOLDER_AT(off) ((folder_7z *)(SI.folders + (off)))
#define PTR_OFF(type, off) ((type)(uintptr_t)(off))

/* ReadStreamsInfo's memory freed (_7z.ReadStreamsInfo.error, .clear) */
static void _7z_ReadStreamsInfo_free(void)
{
	if (!si_pending) return;
	si_pending = false;
	if (SI.packSizes) pgfree(SI.packSizes);
	if (SI.folders) pgfree(SI.folders);
	if (SI.SubStreamsSizes) pgfree(SI.SubStreamsSizes);
	if (SI.tmpSubStreamsCRCsDefined) pgfree(SI.tmpSubStreamsCRCsDefined);
	SI.packSizes = 0;
	SI.folders = 0;
	SI.SubStreamsSizes = 0;
	SI.tmpSubStreamsCRCsDefined = 0;
}

static u64 _7z_GetUnPackSize(folder_7z *ebx);

/* 7z.inc: _7z.ReadStreamsInfo.PackInfo */
static void ReadStreamsInfo_PackInfo(void)
{
	if (SI.numPackStreams) return_err();
	SI.dataOffset = _7z_ReadNumber() + 0x20;
	SI.numPackStreams = _7z_ReadNum();
	_7z_WaitAttribute(kSize);
	u32 ecx = SI.numPackStreams;
	if (ecx) {
		/* the sizes, the CRCs, their flags */
		u8 *eax = pgalloc(ecx * (8 + 4 + 1));
		if (!eax) return_clear();
		SI.packSizes = (u64 *)eax;
		SI.packCRCs = (u32 *)(eax + ecx * 8);
		SI.packCRCsDefined = eax + ecx * 12;
		memset(SI.packCRCsDefined, 0, ecx);
		for (u32 i = 0; i < ecx; i++) SI.packSizes[i] = _7z_ReadNumber();
	}
	for (;;) {
		/* .noloop1 */
		u32 eax = _7z_ReadNum();
		if (!eax) return;
		if (eax == kCRC) _7z_ReadHashDigests(SI.packCRCsDefined, SI.packCRCs, SI.numPackStreams);
		else _7z_SkipData();
	}
}

/* 7z.inc: _7z.ReadStreamsInfo.UnpackInfo: the folders */
static void ReadStreamsInfo_UnpackInfo(void)
{
	if (SI.folders) return_err();
	_7z_WaitAttribute(kFolder);
	u32 ecx = _7z_ReadNum();
	SI.numFolders = ecx;
	_7z_SwitchStream();
	if (ecx) {
		SI.folders_size = 0;
		SI.folders_alloc = 0x1000;
		SI.folders = pgalloc(0x1000);
		if (!SI.folders) return_clear();
		memset(SI.folders, 0, 0x1000);
		/* the folders' pointers, their CRCs and the CRCs' flags */
		folders_alloc(ecx * 9, 8);
		for (u32 i = 0; i < ecx; i++) {
			/* .folderloop */
			u32 f = folders_alloc(sizeof(folder_7z), 8);
			((u32 *)SI.folders)[i] = f;
			u32 numCoders = _7z_ReadNum();
			FOLDER_AT(f)->numCoders = numCoders;
			if (!numCoders) return_err();
			folders_alloc(numCoders * 4, 4);        /* (fsz, just after) */
			u32 numIn = 0, numOut = 0;
			u32 esi = 0;
			do {
				/* .coders_loop */
				u32 dl = _7z_ReadByte();
				u32 idsize = dl & 0xF;
				u32 c = folders_alloc(17 + idsize, 4);
				/* to the end of the list of the coder esi */
				u8 *ebx = (u8 *)&FOLDER_AT(f)->fsz[esi];
				while (get32(ebx)) ebx = SI.folders + get32(ebx);
				put32(ebx, c);
				u8 *r = SI.folders + c;
				put32(r, 0);
				r[4] = idsize;
				for (u32 k = 0; k < idsize; k++) r[5 + k] = _7z_ReadByte();
				u8 *p = r + 5 + idsize;
				if (dl & 0x10) {
					/* .coder_multiple_streams */
					u32 in = _7z_ReadNum();
					put32(p, in);
					numIn += in;
					u32 out = _7z_ReadNum();
					put32(p + 4, out);
					numOut += out;
					/* all currently defined 7z coders have 1 unpacked
					 * stream: assume 1 output stream for unpacker */
					if (out != 1) return_err();
				} else {
					put32(p, 1);
					put32(p + 4, 1);
					numIn++;
					numOut++;
				}
				put32(p + 8, 0);
				if (dl & 0x20) {
					u32 ps = _7z_ReadNum();
					if (ps) {
						put32(p + 8, ps);
						u32 po = folders_alloc(ps, 1);
						for (u32 k = 0; k < ps; k++) SI.folders[po + k] = _7z_ReadByte();
					}
				}
				/* .coder_no_properties: bit 7: an alternative follows */
				if (!(dl & 0x80)) esi++;
			} while (esi < numCoders);
			folder_7z *ebp = FOLDER_AT(f);
			ebp->numUnPackStreams = 1;
			if (!numOut) return_err();
			u32 nbp = numOut - 1;
			ebp->numBindPairs = nbp;
			u32 bp = folders_alloc(nbp * 8, 4);
			FOLDER_AT(f)->bindPairs = PTR_OFF(u32 *, bp);
			for (u32 k = 0; k < nbp; k++) {
				u32 *q = (u32 *)(SI.folders + bp) + 2 * k;
				q[0] = _7z_ReadNum();
				if (q[0] >= numIn) return_err();
				q[1] = _7z_ReadNum();
				if (q[1] >= numOut) return_err();
			}
			/* .noloop3 */
			FOLDER_AT(f)->numOutStreams = numOut;
			if (numIn < numOut) return_err();
			u32 npk = numIn - numOut + 1;
			FOLDER_AT(f)->numPackedStreams = npk;
			u32 ps = folders_alloc(npk * 4, 4);
			FOLDER_AT(f)->packedStreams = PTR_OFF(u32 *, ps);
			u32 *pks = (u32 *)(SI.folders + ps);
			if (npk == 1) {
				/* the input that no pair binds */
				u32 *bpp = (u32 *)(SI.folders + bp), k = 0;
				for (u32 j = 0; j < nbp; j++)
					if (bpp[2 * j] == k) {
						k++;
						j = (u32)-1;
					}
				pks[0] = k;
			} else {
				/* .multiplePackedStreams */
				for (u32 j = 0; j < npk; j++) pks[j] = _7z_ReadNum();
			}
			/* .commonPackedStreams */
			u32 us = folders_alloc(numOut * 8, 8);
			FOLDER_AT(f)->unpackSizes = PTR_OFF(u64 *, us);
			u32 sp = 0;
			if (i) {
				folder_7z *prev = FOLDER_AT(((u32 *)SI.folders)[i - 1]);
				sp = prev->numPackedStreams + prev->startPackedStream;
			}
			FOLDER_AT(f)->startPackedStream = sp;
		}
	}
	/* .nofolders */
	_7z_UnswitchStream();
	_7z_WaitAttribute(kCodersUnPackSize);
	for (u32 i = 0; i < SI.numFolders; i++) {
		folder_7z *ebp = FOLDER_AT(((u32 *)SI.folders)[i]);
		u64 *us = (u64 *)(SI.folders + (uintptr_t)ebp->unpackSizes);
		for (u32 j = 0; j < ebp->numOutStreams; j++) us[j] = _7z_ReadNumber();
	}
	/* .7: convert all relative addresses to pointers */
	u8 *edx = SI.folders;
	for (u32 i = 0; i < SI.numFolders; i++) {
		folder_7z **pp = (folder_7z **)edx + i;
		*pp = (folder_7z *)(edx + *(u32 *)pp);
		folder_7z *eax = *pp;
		eax->bindPairs = (u32 *)(edx + (uintptr_t)eax->bindPairs);
		eax->packedStreams = (u32 *)(edx + (uintptr_t)eax->packedStreams);
		eax->unpackSizes = (u64 *)(edx + (uintptr_t)eax->unpackSizes);
		/* .11: the coders (their alternatives stay offsets in
		 * NextCoder: only the first of each is used) */
		for (u32 j = 0; j < eax->numCoders; j++) eax->fsz[j] = edx + *(u32 *)&eax->fsz[j];
	}
	/* .9 */
	SI.unpackCRCs = (u32 *)(edx + SI.numFolders * 4);
	SI.unpackCRCsDefined = edx + SI.numFolders * 8;
	for (;;) {
		u32 eax = _7z_ReadNum();
		if (!eax) return;
		if (eax == kCRC) _7z_ReadHashDigests(SI.unpackCRCsDefined, SI.unpackCRCs, SI.numFolders);
		else _7z_SkipData();
	}
}

/* 7z.inc: _7z.ReadStreamsInfo.SubStreamsInfo: the files in the folders */
static void ReadStreamsInfo_SubStreamsInfo(void)
{
	if (!SI.numFolders || SI.numSubStreams) return_err();
	folder_7z **ebx = (folder_7z **)SI.folders;
	u32 eax;
	for (;;) {
		/* .ssloop1 */
		eax = _7z_ReadNum();
		if (eax == kNumUnPackStream) {
			for (u32 i = 0; i < SI.numFolders; i++) ebx[i]->numUnPackStreams = _7z_ReadNum();
			continue;
		}
		if (eax == kCRC || eax == kSize || !eax) break;
		_7z_SkipData();
	}
	/* .break1: calculate number of substreams and number of defined
	 * digests */
	u32 total = 0, notone = 0, ndigests = 0;
	for (u32 i = 0; i < SI.numFolders; i++) {
		u32 edx = ebx[i]->numUnPackStreams;
		if (edx != 1) notone++;
		if (total + edx < total) return_err();
		total += edx;
		if (edx == 1 && SI.unpackCRCsDefined[i]) edx = 0;
		ndigests += edx;
	}
	SI.numSubStreams = total;
	if (total) {
		u8 *p = pgalloc(total * (8 + 4 + 1));
		if (!p) return_clear();
		SI.SubStreamsSizes = (u64 *)p;
		SI.SubStreamsCRCs = (u32 *)(p + total * 8);
		SI.SubStreamsCRCsDefined = p + total * 12;
		memset(SI.SubStreamsCRCsDefined, 0, total);
	}
	/* .noss1 */
	if (ndigests) {
		u8 *p = pgalloc(ndigests * (4 + 1));
		if (!p) return_clear();
		SI.tmpSubStreamsCRCs = (u32 *)p;
		SI.tmpSubStreamsCRCsDefined = p + ndigests * 4;
	}
	if (notone && eax != kSize) return_err();
	u64 *ebp = SI.SubStreamsSizes;
	for (u32 i = 0; i < SI.numFolders; i++) {
		/* .ssloop3: the sizes, the last the rest of the folder */
		u32 n = ebx[i]->numUnPackStreams;
		if (!n) continue;
		u64 sum = 0;
		for (u32 k = 1; k < n; k++) {
			u64 s = _7z_ReadNumber();
			*ebp++ = s;
			if (sum + s < sum) return_err();
			sum += s;
		}
		u64 all = _7z_GetUnPackSize(ebx[i]);
		if (all < sum) return_err();
		*ebp++ = all - sum;
	}
	if (eax == kSize) eax = _7z_ReadNum();
	for (;;) {
		if (eax == kCRC) {
			/* .sscrc */
			_7z_ReadHashDigests(SI.tmpSubStreamsCRCsDefined, SI.tmpSubStreamsCRCs, ndigests);
			u32 ecx = 0, edx = 0;           /* index in out, in tmp */
			for (u32 f = 0; f < SI.numFolders; f++) {
				/* .ssloop5 */
				u32 n = ebx[f]->numUnPackStreams;
				if (n == 1 && SI.unpackCRCsDefined[f]) {
					SI.SubStreamsCRCsDefined[ecx] = 1;
					SI.SubStreamsCRCs[ecx++] = SI.unpackCRCs[f];
					continue;
				}
				for (; n; n--, edx++, ecx++) {
					SI.SubStreamsCRCsDefined[ecx] = SI.tmpSubStreamsCRCsDefined[edx];
					SI.SubStreamsCRCs[ecx] = SI.tmpSubStreamsCRCs[edx];
				}
			}
		} else if (!eax) {
			break;                          /* .ssend */
		} else {
			_7z_SkipData();
		}
		eax = _7z_ReadNum();                    /* .ssloop4 */
	}
	/* .ssend */
	if (SI.tmpSubStreamsCRCsDefined) pgfree(SI.tmpSubStreamsCRCs);
	SI.tmpSubStreamsCRCsDefined = 0;
}

/* without SubStreamsInfo each folder is one file (7-Zip's reading; the
 * original expects the record) */
static void _7z_one_substream_each(void)
{
	u32 n = SI.numFolders;
	u8 *p = pgalloc(n * (8 + 4 + 1));
	if (!p) return_clear();
	SI.SubStreamsSizes = (u64 *)p;
	SI.SubStreamsCRCs = (u32 *)(p + n * 8);
	SI.SubStreamsCRCsDefined = p + n * 12;
	SI.numSubStreams = n;
	for (u32 i = 0; i < n; i++) {
		folder_7z *f = ((folder_7z **)SI.folders)[i];
		f->numUnPackStreams = 1;
		SI.SubStreamsSizes[i] = _7z_GetUnPackSize(f);
		SI.SubStreamsCRCsDefined[i] = SI.unpackCRCsDefined[i];
		SI.SubStreamsCRCs[i] = SI.unpackCRCs[i];
	}
}

/* 7z.inc: _7z.ReadStreamsInfo */
static void _7z_ReadStreamsInfo(void)
{
	SI.numPackStreams = 0;
	SI.packSizes = 0;
	SI.numFolders = 0;
	SI.folders = 0;
	SI.numSubStreams = 0;
	SI.SubStreamsSizes = 0;
	SI.tmpSubStreamsCRCsDefined = 0;
	si_pending = true;
	for (;;) {
		/* .mainloop */
		u32 eax = _7z_ReadNum();
		if (!eax) return;
		if (eax == kSubStreamsInfo) ReadStreamsInfo_SubStreamsInfo();
		else if (eax == kUnPackInfo) ReadStreamsInfo_UnpackInfo();
		else if (eax == kPackInfo) ReadStreamsInfo_PackInfo();
		else return_err();
	}
}

/* 7z.inc: _7z.GetUnPackSize: the output no pair binds is the folder's
 * (mainOutStream); its size */
static u64 _7z_GetUnPackSize(folder_7z *ebx)
{
	u32 eax = ebx->numOutStreams;
	if (!eax) return 0;
	for (;;) {
		/* .scan */
		if (!eax--) return_err();
		u32 k;
		for (k = 0; k < ebx->numBindPairs; k++)
			if (ebx->bindPairs[2 * k + 1] == eax) break;
		if (k == ebx->numBindPairs) break;      /* .found */
	}
	ebx->mainOutStream = eax;
	return ebx->unpackSizes[eax];
}

/* ---- the decoders (_7z.decode) ---- */

/* 7z.inc: copy_get_buf_size, copy_init_decoder, copy_fillBuf: the copy
 * decoder; its input's position after streamInfo.size */
typedef struct copy_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	u32 pos;
} copy_decoder;

static void copy_fillBuf(streamInfo *s, u8 *edi, u32 ecx)
{
	copy_decoder *d = (copy_decoder *)s;
	streamInfo *eax = d->inStream;
	u32 esi = d->pos;
	for (;;) {
		/* .redo */
		u32 n = eax->bufDataLen - esi;
		if (n > ecx) n = ecx;
		memcpy(edi, eax->bufPtr + esi, n);
		edi += n;
		esi += n;
		ecx -= n;
		if (!ecx) break;
		if (eax->fullSize < ecx) return_err();
		fillBuf(eax);
		esi = 0;
	}
	/* .done */
	d->pos = esi;
}

static void copy_get_buf_size7(const u8 *esi, u32 n, u32 *eax, u32 *edx)
{
	*eax = sizeof(copy_decoder);
	*edx = 0x10000;
}

static void copy_init_decoder7(streamInfo *ebp, const u8 *esi, u32 n, int ebx)
{
	ebp->fillBuf = copy_fillBuf;
	((copy_decoder *)ebp)->pos = 0;
}

static void deflate_get_buf_size7(const u8 *esi, u32 n, u32 *eax, u32 *edx)
{
	deflate_get_buf_size(eax, edx);
}

static void deflate_init_decoder7(streamInfo *ebp, const u8 *esi, u32 n, int ebx)
{
	deflate_init_decoder(ebp);
}

static void deflate64_init_decoder7(streamInfo *ebp, const u8 *esi, u32 n, int ebx)
{
	deflate64_init_decoder(ebp);
}

typedef void get_buf_size_t(const u8 *esi, u32 params_size, u32 *eax, u32 *edx);
typedef void init_decoder_t(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
static get_buf_size_t *const _7z_GetBufSizeTable[8] = {
	copy_get_buf_size7, lzma_get_buf_size, ppmd_get_buf_size, bcj_get_buf_size,
	bcj2_get_buf_size, aes7z_get_buf_size, deflate_get_buf_size7, deflate_get_buf_size7,
};
static init_decoder_t *const _7z_InitTable[8] = {
	copy_init_decoder7, lzma_init_decoder, ppmd_init_decoder, bcj_init_decoder,
	bcj2_init_decoder, aes7z_init_decoder, deflate_init_decoder7, deflate64_init_decoder7,
};

/* 7z.inc: _7z.GetCoder: the coder's type (the tables' index), -1 if it is
 * unknown */
static int _7z_GetCoder(const u8 *eax)
{
	u32 n = eax[4], in = CODER_NUMIN(eax);
	const u8 *id = eax + 5;
	/* Copy decoder: method ID = [size=01] 00, NumInStreams = NumOutStreams = 1 */
	if (n == 1 && id[0] == 0 && in == 1) return 0;
	if (n == 3 && in == 1) {
		/* LZMA decoder: 03 01 01; PPMD decoder: 03 04 01; Deflate,
		 * Deflate64: 04 01 08, 04 01 09 */
		if (id[0] == 3 && id[1] == 1 && id[2] == 1) return 1;
		if (id[0] == 3 && id[1] == 4 && id[2] == 1) return 2;
		if (id[0] == 4 && id[1] == 1 && id[2] == 8) return 6;
		if (id[0] == 4 && id[1] == 1 && id[2] == 9) return 7;
	}
	if (n == 4) {
		/* BCJ filter: 03 03 01 03; BCJ2 filter: 03 03 01 1B,
		 * NumInStreams = 4; 7z-AES cryptor: 06 F1 07 01 */
		if (id[0] == 3 && id[1] == 3 && id[2] == 1 && id[3] == 3 && in == 1) return 3;
		if (id[0] == 3 && id[1] == 3 && id[2] == 1 && id[3] == 0x1B && in == 4) return 4;
		if (id[0] == 6 && id[1] == 0xF1 && id[2] == 7 && id[3] == 1 && in == 1) return 5;
	}
	return -1;
}

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

/* a packed stream: the archive's bytes from its position (streamInfo.size) */
typedef struct in_stream {
	streamInfo s;
	u64 pos;
} in_stream;

/* 7z.inc: fillBufFromInStream */
static void fillBufFromInStream(streamInfo *eax, u8 *edi, u32 ecx)
{
	in_stream *p = (in_stream *)eax;
	seek(inStream, 0, p->pos);
	if (read(inStream, edi, ecx) != ecx) {
		/* .error */
		SayErr(1, aReadError_ptr, 1, ContinueBtn);
		return_clear();
	}
	p->pos += ecx;
}

/* "Unknown method:" and the method's ID in hex */
static void _7z_unknown_method(const u8 *eax)
{
	char *ebx = aUnknownMethod_z;
	for (u32 i = 0; i < eax[4]; i++) {
		static const char hex[] = "0123456789ABCDEF";
		*ebx++ = ' ';
		*ebx++ = hex[eax[5 + i] >> 4];
		*ebx++ = hex[eax[5 + i] & 0xF];
	}
	*ebx = 0;
	SayErr(1, aUnknownMethod_ptr, 1, ContinueBtn);
	return_clear();
}

/* a coder's sizes: its data (rounded to 8), its buffer (its output's
 * size if smaller, rounded to 16) */
static void coder_sizes(folder_7z *ebx, u32 i, u32 *data, u32 *buf)
{
	u8 *c = ebx->fsz[i];
	int t = _7z_GetCoder(c);
	if (t < 0) _7z_unknown_method(c);
	u32 eax, edx;
	_7z_GetBufSizeTable[t](CODER_PROPS(c), CODER_PROPSIZE(c), &eax, &edx);
	*data = (eax + 7) & ~7u;
	if (ebx->unpackSizes[i] < edx) edx = ebx->unpackSizes[i];
	*buf = (edx + 0xF) & ~0xFu;
}

/* _7z.decode.init_links: the inputs of the coder edx: bound to another
 * coder's output (linked in turn), or a packed stream */
static void _7z_link_coder(folder_7z *ebx, u32 edx)
{
	streamInfo **pStreams = _7z_decode_pStreams;
	u8 *c = ebx->fsz[edx];
	u32 nin = CODER_NUMIN(c), base = 0;
	for (u32 j = 0; j < edx; j++) base += CODER_NUMIN(ebx->fsz[j]);
	streamInfo **inputs = (streamInfo **)(pStreams[edx] + 1);
	for (u32 ecx = 0; ecx < nin; ecx++) {
		u32 in = base + ecx, k;
		for (k = 0; k < ebx->numBindPairs; k++)
			if (ebx->bindPairs[2 * k] == in) break;
		if (k < ebx->numBindPairs) {
			u32 out = ebx->bindPairs[2 * k + 1];
			streamInfo *src = pStreams[out];
			inputs[ecx] = src;
			if (*(streamInfo **)(src + 1)) return_err();    /* (linked twice) */
			_7z_link_coder(ebx, out);
			continue;
		}
		/* .link_to_in */
		for (k = ebx->numPackedStreams; k--;)
			if (ebx->packedStreams[k] == in) break;
		if (k == (u32)-1) return_err();
		inputs[ecx] = pStreams[ebx->numCoders + k];
	}
}

/* 7z.inc: _7z.decode.init: the folder's decoders and its packed streams
 * in mainbuf, linked, initialized; pStreams[i] the coder i, then the
 * packed streams */
static void _7z_decode_init(folder_7z *ebx)
{
	/* (the stack keeps no information on streams here; the original's
	 * limit of the number of coders stays) */
	if (ebx->numCoders > 128) return_err();
	/* allocate space for streams info array */
	u32 ecx = ebx->numPackedStreams + ebx->numCoders;
	streamInfo **pStreams = _7z_decode_pStreams_def;
	if (ecx > 8) {
		pStreams = pgalloc(ecx * 4);
		if (!pStreams) return_clear();
	}
	_7z_decode_pStreams = pStreams;
	/* calculate size of buffers required for decoders */
	u64 total = 0;
	for (u32 i = 0; i < ebx->numCoders; i++) {
		/* .calcbufsz */
		u32 data, buf;
		coder_sizes(ebx, i, &data, &buf);
		total += data + buf;
	}
	/* 16 KiB for each of not binded input streams */
	total += (u64)ebx->numPackedStreams << 14;
	if (total > 0x7FFFFFFF) {
		/* .nomem */
		SayErr(1, aNoFreeRam_ptr, 1, ContinueBtn);
		return_clear();
	}
	/* .memok: allocate streamInfos */
	u8 *eax = pgalloc(total);
	if (!eax) return_clear();
	memset(eax, 0, total);
	_7z_decode_mainbuf = eax;
	/* initialize streamInfos for decoders */
	for (u32 i = 0; i < ebx->numCoders; i++) {
		/* .init1 */
		streamInfo *s = (streamInfo *)eax;
		pStreams[i] = s;
		s->fullSize = ebx->unpackSizes[i];
		*(streamInfo **)(s + 1) = 0;
		u32 data, buf;
		coder_sizes(ebx, i, &data, &buf);
		s->bufPtr = eax + data;
		s->bufSize = buf;
		s->bufDataLen = 0;
		eax += data + buf;
	}
	/* initialize streamInfos for input streams */
	for (u32 k = 0; k < ebx->numPackedStreams; k++) {
		in_stream *s = (in_stream *)eax;
		pStreams[ebx->numCoders + k] = &s->s;
		s->s.fullSize = SI.packSizes[_7z_CurInStream++];
		s->s.fillBuf = fillBufFromInStream;
		s->s.bufPtr = (u8 *)(s + 1);
		s->s.bufSize = 0x4000 - sizeof(in_stream);
		s->s.bufDataLen = 0;
		s->pos = SI.dataOffset;
		SI.dataOffset += s->s.fullSize;
		eax += 0x4000;
	}
	/* initialize links between streams */
	_7z_GetUnPackSize(ebx);
	_7z_link_coder(ebx, ebx->mainOutStream);
	/* .inited_links: initialize all decoders */
	for (u32 i = 0; i < ebx->numCoders; i++) {
		/* .init_decoders */
		u8 *c = ebx->fsz[i];
		streamInfo *s = pStreams[i];
		if (CODER_NUMIN(c) && !*(streamInfo **)(s + 1)) return_err();
		_7z_InitTable[_7z_GetCoder(c)](s, CODER_PROPS(c), CODER_PROPSIZE(c), 0);
	}
}

/* the decoders' memory freed (_7z.decode.error, .clear) */
static void _7z_decode_free(void)
{
	if (_7z_decode_pStreams && _7z_decode_pStreams != _7z_decode_pStreams_def) pgfree(_7z_decode_pStreams);
	_7z_decode_pStreams = _7z_decode_pStreams_def;
	if (_7z_decode_mainbuf) pgfree(_7z_decode_mainbuf);
	_7z_decode_mainbuf = 0;
}

/* 7z.inc: _7z.decode: in: ebx -> folder, outBufferSize, outBufferPtr: the
 * folder unpacked (a header's streams) */
static void _7z_decode(folder_7z *ebx, u32 outBufferSize, u8 *outBufferPtr)
{
	_7z_decode_mainbuf = 0;
	_7z_decode_pStreams = _7z_decode_pStreams_def;
	RETURN_CATCH(saved) {
		_7z_decode_free();
		return_again();
	}
	_7z_decode_init(ebx);
	u32 outBufferRead = 0;
	do {
		streamInfo *eax = _7z_decode_pStreams[ebx->mainOutStream];
		fillBuf(eax);
		if (!eax->bufDataLen) return_err();
		memcpy(outBufferPtr + outBufferRead, eax->bufPtr, eax->bufDataLen);
		outBufferRead += eax->bufDataLen;
	} while (outBufferRead < outBufferSize);
	RETURN_UNCATCH(saved);
	_7z_decode_free();
}

/* _7z.ReadAndDecodePackedStreams.free2: the unpacked streams of ebx
 * folders */
static void _7z_free2(u32 ebx)
{
	while (ebx--) pgfree(_7z_unpacked[ebx].ptr);
	if (_7z_unpacked != _7z_unpbuf) pgfree(_7z_unpacked);
}

/* 7z.inc: _7z.ReadAndDecodePackedStreams: a header's streams (an encoded
 * header, additional streams) read and unpacked into _7z.unpacked */
static void _7z_ReadAndDecodePackedStreams(void)
{
	_7z_ReadStreamsInfo();
	u32 ecx = SI.numFolders;
	if (!ecx) {
		/* .ret2 */
		if (SI.packSizes) pgfree(SI.packSizes);
		if (SI.folders) pgfree(SI.folders);
		si_pending = false;
		return;
	}
	if (SI.SubStreamsSizes) {
		pgfree(SI.SubStreamsSizes);
		SI.SubStreamsSizes = 0;
	}
	typeof(_7z_unpbuf[0]) *eax = _7z_unpbuf;
	if (ecx > 8) {
		eax = pgalloc(ecx * 8);
		if (!eax) return_clear();
	}
	memset(eax, 0, ecx * 8);
	_7z_unpacked = eax;
	_7z_CurInStream = 0;
	RETURN_CATCH(saved) {
		/* .error, .clear: .free, then ReadStreamsInfo's */
		_7z_free2(SI.numFolders);
		return_again();
	}
	for (u32 i = 0; i < ecx; i++) {
		/* .loop */
		folder_7z *ebx = ((folder_7z **)SI.folders)[i];
		u64 size = _7z_GetUnPackSize(ebx);
		if (size > 0x7FFFFFFF) return_err();
		eax[i].size = size;
		eax[i].ptr = pgalloc(size);
		if (!eax[i].ptr) return_err();
		_7z_decode(ebx, size, eax[i].ptr);
		if (SI.unpackCRCsDefined[i] && crc(eax[i].ptr, size) != SI.unpackCRCs[i]) return_err();
	}
	RETURN_UNCATCH(saved);
	/* .ret */
	pgfree(SI.folders);
	pgfree(SI.packSizes);
	SI.folders = 0;
	SI.packSizes = 0;
	si_pending = false;
}

/* 7z.inc: _7z.CloseHandle */
static void _7z_CloseHandle(handle_7z *esi)
{
	if (esi->host) close(esi->host);
	if (esi->packSizes) pgfree(esi->packSizes);
	if (esi->folders) pgfree(esi->folders);
	if (esi->names_buf) pgfree(esi->names_buf);
	if (esi->last_context) pgfree(esi->last_context);
	pgfree(esi);
}

/* 7z.inc: close_7z */
void close_7z(handle_common *hPlugin)
{
	if (hPlugin != &empty_7z_handle) _7z_CloseHandle((handle_7z *)hPlugin);
}

/* open_7z.clear: what the reading holds */
static void open_7z_clear(void)
{
	if (_7z_tmpVector) pgfree(_7z_tmpVector);
	_7z_tmpVector = 0;
	if (hOut) _7z_CloseHandle((handle_7z *)hOut);
	hOut = 0;
	if (_7z_dataVectorSize) _7z_free2(_7z_dataVectorSize);
	_7z_dataVectorSize = 0;
	if (_7z_FreeSubStreams) pgfree(_7z_FreeSubStreams);
	_7z_FreeSubStreams = 0;
	_7z_ReadStreamsInfo_free();
	if (bufptr != buffer) pgfree(bufptr);
	bufptr = buffer;
}

/* open_7z's ends without files (.free_empty, .empty2) */
static HANDLE open_7z_empty(void)
{
	open_7z_clear();
	close(inStream);
	return &empty_7z_handle;
}

/* 7z.inc: open_7z: CInArchive::ReadDatabase() */
HANDLE open_7z(HANDLE ebp, const u8 *esi, int datasize, const void *attr)
{
	/* major version must be 0 (minor version does not be checked) */
	if (esi[6] != 0) return 0;
	hOut = 0;
	_7z_FreeSubStreams = 0;
	_7z_tmpVector = 0;
	_7z_dataVectorSize = 0;
	si_pending = false;
	bufptr = buffer;
	RETURN_CATCH(saved) {
		if (return_kind == RETURN_ERR) {
			/* .header_error */
			SayErr(1, HeaderError_ptr, 1, ContinueBtn);
		}
		/* .clear */
		open_7z_clear();
		return (HANDLE)-1;              /* .reterr */
	}
	if (crc(esi + 12, 20) != get32(esi + 8)) return_err();         /* .header_crc_error */
	if (get32(esi + 24)) return_err();
	bufsize = get32(esi + 20);
	u8 *edi = buffer;
	if (!bufsize) {
		RETURN_UNCATCH(saved);
		close(ebp);
		return &empty_7z_handle;
	}
	/* .notempty */
	if (bufsize > 1024) {
		edi = pgalloc(bufsize);
		if (!edi) return_clear();
	}
	bufptr = edi;
	bufend = edi + bufsize;
	seek(ebp, 0, (get32(esi + 12) | (u64)get32(esi + 16) << 32) + 0x20);  /* end of main header */
	if (read(ebp, edi, bufsize) != bufsize) return_err();
	if (crc(edi, bufsize) != get32(esi + 28)) return_err();
	/* .header_ok */
	cur = edi;
	inStream = ebp;
	u32 eax = _7z_ReadNum();
	if (eax != kHeader) {
		if (eax != kEncodedHeader) return_err();
		_7z_ReadAndDecodePackedStreams();
		if (!SI.numFolders) {
			/* .free_empty */
			RETURN_UNCATCH(saved);
			return open_7z_empty();
		}
		if (SI.numFolders != 1) return_err();
		if (bufptr != buffer) pgfree(bufptr);
		bufptr = _7z_unpbuf[0].ptr;
		bufend = bufptr + _7z_unpbuf[0].size;
		cur = bufptr;
		if (_7z_ReadNum() != kHeader) return_err();
	}
	/* .found_header */
	eax = _7z_ReadNum();
	if (eax == kArchiveProperties) {
		while (_7z_ReadNum()) _7z_SkipData();
		eax = _7z_ReadNum();
	}
	/* .no_archive_props */
	if (eax == kAdditionalStreamsInfo) {
		_7z_ReadAndDecodePackedStreams();
		_7z_dataVector = _7z_unpacked;
		_7z_dataVectorSize = SI.numFolders;
		eax = _7z_ReadNum();
	}
	/* .no_additional_streams */
	if (eax != kMainStreamsInfo) return_err();
	_7z_ReadStreamsInfo();
	if (!SI.SubStreamsSizes && SI.numFolders) _7z_one_substream_each();
	eax = _7z_ReadNum();
	if (!eax) {
		/* .empty2 */
		RETURN_UNCATCH(saved);
		return open_7z_empty();
	}
	/* .notempty2 */
	if (eax != kFilesInfo) return_err();
	u32 ebx = _7z_ReadNum();                /* number of files in archive */
	if (!ebx) {
		RETURN_UNCATCH(saved);
		return open_7z_empty();
	}
	u64 sz = (u64)ebx * sizeof(file_in_7z) + sizeof(handle_7z);
	if (sz > 0x7FFFFFFF) return_err();
	hOut_allocated = sz;
	handle_7z *h = pgalloc(hOut_allocated);
	if (!h) return_clear();
	memset(h, 0, hOut_allocated);
	hOut = (u8 *)h;
	h->c.type = type_7z;
	h->c.NumFiles = ebx;
	h->numPackStreams = SI.numPackStreams;
	h->packSizes = SI.packSizes;
	h->numFolders = SI.numFolders;
	h->unpackCRCsDefined = SI.unpackCRCsDefined;
	h->unpackCRCs = SI.unpackCRCs;
	h->folders = (folder_7z **)SI.folders;
	h->password_len = -1;
	h->last_folder = (u32)-1;
	if (bPasswordDefined) {
		h->password_len = password_size;
		memcpy(h->password, password_unicode, password_size * 2);
	}
	_7z_FreeSubStreams = SI.SubStreamsSizes;
	si_pending = false;                     /* (the handle and FreeSubStreams have them now) */
	_7z_tmpVector = pgalloc(ebx * 4);
	if (!_7z_tmpVector) return_clear();
	memset(_7z_tmpVector, 0, ebx * 4);
	_7z_emptyStreams = _7z_tmpVector + ebx;
	_7z_emptyFiles = _7z_emptyStreams + ebx;
	_7z_antiFiles = _7z_emptyFiles + ebx;
	_7z_numEmptyStreams = 0;
	file_in_7z *files = (file_in_7z *)(h + 1);
	for (;;) {
		/* .parse_header */
		u32 type = _7z_ReadNum();
		if (!type) break;
		u32 size = _7z_ReadNum();
		if (type == kName) {
			/* .parse_names: UNICODE to cp866 (a half of the size) */
			if (_7z_SwitchStream()) size = bufend - cur;
			u32 ecx = size >> 1;
			char *ebp2 = pgalloc(ecx ? ecx : 1);
			if (!ebp2) return_clear();
			h = (handle_7z *)hOut;
			h->names_buf = ebp2;
			char *limit = ebp2 + ecx;
			bWasWarning = 0;
			for (u32 i = 0; i < ebx; i++) {
				/* .parse_names_loop */
				files[i].c.fullname = ebp2;
				u8 al;
				do {
					/* .parse_names_loop_int */
					if (ebp2 >= limit) return_err();
					u32 ax = _7z_ReadWord();
					if (ax < 0x80) al = ax;                 /* .id */
					else if (ax == 0x401) al = 0xF0;        /* .yo1: Ё */
					else if (ax == 0x451) al = 0xF1;        /* .yo2: ё */
					else if (ax >= 0x410 && ax < 0x440) al = ax + 0x70;     /* .rus1 */
					else if (ax >= 0x440 && ax < 0x450) al = ax + 0xA0;     /* .rus2 */
					else {
						/* .unk */
						if (!bWasWarning) {
							Message(aEncodingProblem, 3, aEncodingProblem_str_ptr, 1, ContinueBtn);
							bWasWarning++;
						}
						al = '_';
					}
					*ebp2++ = al;
				} while (al);
			}
			_7z_UnswitchStream();
		} else if (type == kWinAttributes || type == kStartPos) {
			/* .parse_winAttr, .parse_startPos */
			_7z_ReadBoolVector2(_7z_tmpVector, ebx);
			_7z_SwitchStream();
			for (u32 i = 0; i < ebx; i++) {
				u8 al = _7z_tmpVector[i];
				if (type == kWinAttributes) {
					files[i].bAttrDefined = al;
					if (al) files[i].attr = _7z_ReadDword();
				} else {
					files[i].bStartPosDefined = al;
					if (al) {
						files[i].startPos = _7z_ReadDword();
						files[i].startPos |= (u64)_7z_ReadDword() << 32;
					}
				}
			}
			_7z_UnswitchStream();
		} else if (type == kEmptyStream) {
			/* .parse_emptyStream */
			_7z_ReadBoolVector(_7z_emptyStreams, ebx);
			_7z_numEmptyStreams = 0;
			for (u32 i = 0; i < ebx; i++) _7z_numEmptyStreams += _7z_emptyStreams[i] != 0;
		} else if (type == kEmptyFile) {
			_7z_ReadBoolVector(_7z_emptyFiles, _7z_numEmptyStreams);
		} else if (type == kAnti) {
			_7z_ReadBoolVector(_7z_antiFiles, _7z_numEmptyStreams);
		} else if (type == kCreationTime || type == kLastWriteTime || type == kLastAccessTime) {
			/* .parse_time */
			_7z_ReadBoolVector2(_7z_tmpVector, ebx);
			_7z_SwitchStream();
			for (u32 i = 0; i < ebx; i++) {
				u8 al = _7z_tmpVector[i];
				(&files[i].bCreationTimeDefined)[type - kCreationTime] = al;
				if (al) {
					u64 t = _7z_ReadDword();
					t |= (u64)_7z_ReadDword() << 32;
					ntfs_datetime_to_bdfe(t, files[i].CreationTime + (type - kCreationTime) * 8);
				}
			}
			_7z_UnswitchStream();
		} else {
			_7z_SkipSize(size);
		}
	}
	/* .parse_header_done */
	u32 ecx = 0, edx = 0;                   /* index in emptyFiles, in subStreams */
	for (u32 i = 0; i < ebx; i++) {
		file_in_7z *f = &files[i];
		f->bHasStream = !_7z_emptyStreams[i];
		if (f->bHasStream) {
			f->c.bIsDirectory = 0;
			f->bAnti = 0;
			if (edx >= SI.numSubStreams) return_err();
			f->UnPackSize = SI.SubStreamsSizes[edx];
			f->bCRCDefined = SI.SubStreamsCRCsDefined[edx];
			if (f->bCRCDefined) f->FileCRC = SI.SubStreamsCRCs[edx];
			edx++;
		} else {
			/* .nostream */
			f->c.bIsDirectory = !_7z_emptyFiles[ecx];
			f->bAnti = _7z_antiFiles[ecx] != 0;
			f->UnPackSize = 0;
			f->bCRCDefined = 0;
			ecx++;
		}
		if (f->c.bIsDirectory) f->attr |= 0x10;
	}
	pgfree(_7z_tmpVector);
	_7z_tmpVector = 0;
	if (_7z_dataVectorSize) {
		_7z_free2(_7z_dataVectorSize);
		_7z_dataVectorSize = 0;
	}
	if (_7z_FreeSubStreams) {
		pgfree(_7z_FreeSubStreams);
		_7z_FreeSubStreams = 0;
	}
	if (bufptr != buffer) {
		pgfree(bufptr);
		bufptr = buffer;
	}
	/* the files' folders, their indices in them and their starts */
	h = (handle_7z *)hOut;
	u32 esi2 = 0;                           /* index of folder */
	u32 idx = 0;                            /* index in folder */
	u64 pos = 0;                            /* position in folder */
	for (u32 i = 0; i < ebx; i++) {
		/* .getfoldersloop */
		file_in_7z *f = &files[i];
		if (!f->bHasStream && !idx) {
			/* .nofolder */
			f->folder = f->folder_index = (u32)-1;
			f->folderStart = (u64)-1;
			continue;
		}
		if (!idx) {
			/* skip empty folders */
			for (;; esi2++) {
				if (esi2 >= h->numFolders) return_err();
				if (h->folders[esi2]->numUnPackStreams) break;
			}
		}
		/* .notfirst */
		f->folder = esi2;
		f->folder_index = idx;
		f->folderStart = pos;
		if (!f->bHasStream) continue;   /* (7-Zip's: no stream of the folder) */
		pos += f->UnPackSize;
		if (++idx >= h->folders[esi2]->numUnPackStreams) {
			esi2++;
			idx = 0;
			pos = 0;
		}
	}
	/* .getfoldersdone: the folders' packed data's offsets */
	for (u32 k = 0; k < h->numFolders; k++) {
		/* .getoffsloop */
		folder_7z *eax2 = h->folders[k];
		if (!k) {
			eax2->packOffset = SI.dataOffset;
			continue;
		}
		/* .getoffs2 */
		folder_7z *p = h->folders[k - 1];
		eax2->packOffset = p->packOffset;
		if (p->startPackedStream + p->numPackedStreams > h->numPackStreams) return_err();
		for (u32 j = 0; j < p->numPackedStreams; j++) eax2->packOffset += h->packSizes[p->startPackedStream + j];
	}
	/* .getoffsdone */
	if (!init_file_links(sizeof(handle_7z), ebx, sizeof(file_in_7z), offsetof(handle_common, root))) {
		RETURN_UNCATCH(saved);
		open_7z_clear();
		return (HANDLE)-1;
	}
	RETURN_UNCATCH(saved);
	h = (handle_7z *)hOut;
	hOut = 0;
	h->c.curdir = 0;                        /* set root directory */
	h->host = inStream;
	memcpy(h->host_datetime, (const u8 *)attr + 8, 24);
	return h;
}

/* 7z.inc: getattr_7z: ebp = hPlugin, eax -> item, edi -> info */
void getattr_7z(handle_common *ebp, file_common *eax, u8 *edi)
{
	memset(edi, 0, 40);
	if (eax->bPseudoFolder) {
		/* .pseudo: attributes: folder; the archive's times */
		*(u32 *)edi = 0x10;
		memcpy(edi + 8, ((handle_7z *)ebp)->host_datetime, 24);
		return;
	}
	file_in_7z *esi = (file_in_7z *)eax;
	memcpy(edi, &esi->attr, 4);
	memcpy(edi + 8, esi->CreationTime, 24);
	memcpy(edi + 32, &esi->UnPackSize, 8);
}

/* a file's handle */
typedef struct file_handle_7z {
	u32 type;                       /* type_7z */
	u8 *context;                    /* the decoders (mainbuf) */
	u64 pos;
	handle_7z *base;                /* handle of archive */
	file_in_7z *item;
	streamInfo *mainStream;         /* (pointer in context) */
	u8 bError;
} file_handle_7z;

/* a file without data: reads nothing (the original hands out
 * empty_7z_handle, which reads as one) */
static file_handle_7z empty_7z_file = { .type = type_7z };

/* open_file_7z.done: the password of the archive kept */
static void open_file_7z_done(handle_7z *ebp)
{
	if (!bPasswordDefined) return;
	ebp->password_len = password_size;
	memcpy(ebp->password, password_unicode, password_size * 2);
}

/* 7z.inc: open_file_7z: ebp = hPlugin, eax -> item, edi = mode */
void *open_file_7z(handle_common *p, file_common *eax, int edi)
{
	handle_7z *ebp = (handle_7z *)p;
	file_in_7z *esi = (file_in_7z *)eax;
	if (!esi->bHasStream) return &empty_7z_file;
again:;
	bPasswordDefined = ebp->password_len != -1;
	if (bPasswordDefined) {
		password_size = ebp->password_len;
		memcpy(password_unicode, ebp->password, password_size * 2);
	}
	mem_stream *volatile ms = 0;           /* the original's hOut */
	_7z_decode_mainbuf = 0;
	inStream = ebp->host;
	_7z_decode_pStreams = _7z_decode_pStreams_def;
	RETURN_CATCH(saved) {
		bool retry = false;
		if (return_kind == RETURN_ERR) {
			if (bPasswordDefined) {
				/* the user wants to re-enter password? */
				retry = SayErr(1, aArchiveDataErrorPass_ptr, 2, CancelPassBtn) == 1;
			} else {
				/* .realerror */
				SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
			}
		}
		/* .clear */
		_7z_decode_free();
		if (ms) pgfree(ms);
		if (retry) goto again;
		return 0;
	}
	streamInfo *mainStream;
	if (ebp->last_folder == esi->folder && esi->folderStart >= ebp->last_pos) {
		/* the last folder's decoders, from where they stopped */
		_7z_decode_mainbuf = ebp->last_context;
		ebp->last_context = 0;
		mainStream = ebp->last_main_stream;
		mainStream->fullSize = esi->folderStart - ebp->last_pos;
		ebp->last_folder = (u32)-1;
	} else {
		/* .nolast */
		folder_7z *ebx = ebp->folders[esi->folder];
		_7z_CurInStream = ebx->startPackedStream;
		SI.dataOffset = ebx->packOffset;
		SI.packSizes = ebp->packSizes;
		_7z_decode_init(ebx);
		mainStream = _7z_decode_pStreams[ebx->mainOutStream];
		mainStream->fullSize = esi->folderStart;
	}
	/* .commonl: up to the file's start */
	skip_7z(mainStream);
	mainStream->fullSize = esi->UnPackSize;
	u32 ecx = esi->UnPackSize;
	if (!(esi->UnPackSize >> 32) && ((ecx + 0x3FF) >> 10) <= getfreemem() >> 2) {
		/* create memory stream and unpack to memory */
		ms = pgalloc(ecx + sizeof(mem_stream));
		if (!ms) return_clear();
		ms->type = type_mem_stream;
		ms->size = ecx;
		ms->pos = 0;
		read_7z_to_buf(mainStream, ms->buf);
		if (esi->bCRCDefined && crc(ms->buf, ecx) != esi->FileCRC) return_err();
		RETURN_UNCATCH(saved);
		/* the decoders kept for the next file of the folder */
		if (ebp->last_context) pgfree(ebp->last_context);
		ebp->last_context = _7z_decode_mainbuf;
		_7z_decode_mainbuf = 0;
		ebp->last_folder = esi->folder;
		ebp->last_main_stream = mainStream;
		ebp->last_pos = esi->folderStart + esi->UnPackSize;
		open_file_7z_done(ebp);
		_7z_decode_free();
		return ms;
	}
	/* .nomemstream */
	file_handle_7z *h = alloc_handle(0, sizeof(file_handle_7z));
	if (!h) return_clear();
	RETURN_UNCATCH(saved);
	h->type = type_7z;
	h->pos = 0;
	h->bError = 0;
	h->context = _7z_decode_mainbuf;
	_7z_decode_mainbuf = 0;
	h->mainStream = mainStream;
	h->base = ebp;
	h->item = esi;
	open_file_7z_done(ebp);
	_7z_decode_free();
	return h;
}

/* 7z.inc: read_7z_to_buf: the stream's buffer is a ring: filled on
 * after its data, from its start again when it is full */
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

/* unsigned __stdcall read(HANDLE hFile, void* buf, unsigned size) */
unsigned read_7z(void *p, void *buf, unsigned size)
{
	file_handle_7z *volatile ebx = p;
	streamInfo *eax = ebx->mainStream;
	if (!eax) return 0;                     /* empty stream - return 0 bytes read */
	if (ebx->bError) return (unsigned)-1;   /* .reterr */
	inStream = ebx->base->host;
	eax->fullSize = size;
	if (!size) return 0;                    /* .nodata */
	RETURN_CATCH(saved) {
		/* .error, .clear */
		ebx->bError = 1;
		SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
		return (unsigned)-1;
	}
	u64 rest = ebx->item->UnPackSize - ebx->pos;
	if (rest < size) eax->fullSize = rest;
	u8 *edi = read_7z_to_buf(eax, buf);
	RETURN_UNCATCH(saved);
	u32 n = edi - (u8 *)buf;
	ebx->pos += n;
	return n;
}

/* void __stdcall setpos(HANDLE hFile, __int64 pos) */
void setpos_7z(void *p, u64 pos)
{
	file_handle_7z *volatile ebx = p;
	if (!ebx->context) return;
	handle_7z *edi = ebx->base;
	inStream = edi->host;
	RETURN_CATCH(saved) {
		ebx->bError = 1;
		SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
		return;
	}
	if (!ebx->bError && pos >= ebx->pos) {
		/* move forward - skip some data */
		ebx->mainStream->fullSize = pos - ebx->pos;
		skip_7z(ebx->mainStream);
		ebx->pos = pos;
		RETURN_UNCATCH(saved);
		return;
	}
	/* .backward: move backward - reinitialize and skip start data */
	ebx->bError = 0;
	u8 *ebp = ebx->context;
	folder_7z *f = edi->folders[ebx->item->folder];
	/* initialize streamInfos for decoders (where _7z.decode.init put them) */
	for (u32 i = 0; i < f->numCoders; i++) {
		streamInfo *s = (streamInfo *)ebp;
		s->fullSize = f->unpackSizes[i];
		s->bufDataLen = 0;
		u8 *c = f->fsz[i];
		_7z_InitTable[_7z_GetCoder(c)](s, CODER_PROPS(c), CODER_PROPSIZE(c), 1);
		ebp = s->bufPtr + s->bufSize;
	}
	/* initialize streamInfos for input streams */
	u64 off = f->packOffset;
	for (u32 k = 0; k < f->numPackedStreams; k++) {
		in_stream *s = (in_stream *)ebp;
		s->pos = off;
		s->s.bufDataLen = 0;
		s->s.fullSize = edi->packSizes[f->startPackedStream + k];
		off += s->s.fullSize;
		ebp += 0x4000;
	}
	streamInfo *eax = ebx->mainStream;
	eax->fullSize = ebx->item->folderStart + pos;
	ebx->pos = 0;
	eax->bufDataLen = 0;
	skip_7z(eax);
	ebx->pos = pos;
	RETURN_UNCATCH(saved);
}

/* 7z.inc: close_file_7z: the decoders stay as the archive's last ones
 * (unless they failed) */
void close_file_7z(void *p)
{
	file_handle_7z *ebx = p;
	u8 *ecx = ebx->context;
	if (!ecx) return;
	if (!ebx->bError) {
		handle_7z *ebp = ebx->base;
		u8 *t = ebp->last_context;
		ebp->last_context = ecx;
		ecx = t;
		ebp->last_folder = ebx->item->folder;
		ebp->last_main_stream = ebx->mainStream;
		ebp->last_pos = ebx->item->folderStart + ebx->pos;
	}
	if (ecx) pgfree(ecx);
	free_handle(ebx);
}

/* (until ppmd.inc and the AES are here) */
__attribute__((weak)) void ppmd_get_buf_size(const u8 *esi, u32 n, u32 *eax, u32 *edx) { return_err(); }
__attribute__((weak)) void ppmd_init_decoder(streamInfo *ebp, const u8 *esi, u32 n, int ebx) { return_err(); }
__attribute__((weak)) void init_ppmd(void) {}
__attribute__((weak)) void aes7z_get_buf_size(const u8 *esi, u32 n, u32 *eax, u32 *edx) { return_err(); }
__attribute__((weak)) void aes7z_init_decoder(streamInfo *ebp, const u8 *esi, u32 n, int ebx) { return_err(); }
__attribute__((weak)) void init_aes(void) {}
