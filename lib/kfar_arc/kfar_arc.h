/* kfar_arc for KolibriOS for ARM: kfar's plugin for archives (7z, zip),
 * a library loaded by function 68.19 (/sys/lib/kfar_arc.obj).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc
 * (kfar_arc.asm 0.17 and its includes: crc.inc, zip.inc, deflate.inc,
 * 7z.inc, lzma.inc, 7zbranch.inc, ppmd.inc, aes.inc, 7zaes.inc,
 * sha256.inc), by diamond (Evgeny Grechnikov), GPL-2.0; the 7z loader
 * and the decoders follow Igor Pavlov's 7-Zip and LZMA SDK, as the
 * original says in each file.
 *
 * The C keeps the original's structure and names, as kfar's port does
 * (apps/kfar/kfar.h): a label is a function of the same name (a local
 * label .x of p is p_x, a dot in a name an underscore: zip.read is
 * zip_read), the globals and structures are the original's, each
 * function names its file and label. The interface with kfar is
 * apps/kfar/kfar_api.h.
 *
 * The original's error exits, return.err and return.clear (esp back to
 * [_esp], then [error_proc] or [clear_proc]), are __builtin_longjmp to
 * the innermost RETURN_CATCH below: the entry functions catch them and
 * run their .error and .clear parts. Links between the records of an
 * archive (folders, files) are offsets from the handle, as in the
 * original (the block may be reallocated while it is built). */
#ifndef KFAR_ARC_H
#define KFAR_ARC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "kos.h"
#include "kfar_api.h"

typedef unsigned long long u64;
typedef long long s64;

/* ---- the exports (kfar_arc.exp; kfar_api.h has their types) ---- */

plugin_load_t plugin_load;
OpenFilePlugin_t OpenFilePlugin;
ClosePlugin_t ClosePlugin;
ReadFolder_t ReadFolder;
SetFolder_t SetFolder;
GetFiles_t GetFiles;
GetOpenPluginInfo_t GetOpenPluginInfo;
getattr_t mygetattr;
plugin_open_t myopen;
plugin_read_t myread;
setpos_t mysetpos;
plugin_close_t myclose;
void *deflate_unpack(void *packed_data, unsigned *pLength);
void *deflate_unpack2(void *(*get_next_chunk)(void *parameter, unsigned *pLength), void *parameter,
		      unsigned *pUnpackedLength);

/* ---- the imports from kfar (kfar_arc.asm: plugin_load) ---- */

extern kfar_info *kfar_info_ptr;        /* kfar_arc.asm: kfar_info */
extern HANDLE (*open2)(void *plugin_id, HANDLE plugin_instance, const char *name, int mode);
extern long long (*filesize)(HANDLE hFile);
extern unsigned (*read)(HANDLE hFile, void *buf, unsigned size);
extern void (*seek)(HANDLE hFile, int method, long long newpos);
extern void (*close)(HANDLE hFile);
extern void *(*pgalloc)(unsigned size);
extern void *(*pgrealloc)(void *pointer, unsigned size);
extern void (*pgfree)(void *pointer);
extern unsigned (*getfreemem)(void);
extern int (*DialogBox)(void *dlg);
extern int (*SayErr)(int num_strings, const char **strings, int num_buttons, const char **buttons);
extern int (*Message)(const char *title, int num_strings, const char **strings, int num_buttons,
		      const char **buttons);
extern struct kfar_console_size *cur_console_size;

/* ---- return.err, return.clear ---- */

enum { RETURN_ERR = 1, RETURN_CLEAR = 2 };
extern void *return_jmp[5];
extern int return_kind;
/* kfar_arc.asm: return.err ("invalid data") and return.clear (cancelled,
 * no memory: the user knows) */
void return_err(void) __attribute__((noreturn));
void return_clear(void) __attribute__((noreturn));
/* catches them in the function where it stands: the block after it runs
 * with return_kind set and the outer catcher back in return_jmp;
 * RETURN_UNCATCH() before the function returns normally */
#define RETURN_CATCH(saved)                                             \
	void *saved[5];                                                 \
	memcpy(saved, return_jmp, sizeof(saved));                      \
	if (__builtin_setjmp(return_jmp) && (memcpy(return_jmp, saved, sizeof(saved)), 1))
#define RETURN_UNCATCH(saved) memcpy(return_jmp, saved, sizeof(saved))
/* the error passed on to the catcher outside */
static inline void __attribute__((noreturn)) return_again(void)
{
	if (return_kind == RETURN_ERR) return_err();
	return_clear();
}

/* ---- the plugin's handles (kfar_arc.asm: handle_common, file_common) ---- */

/* a folder's lists: its subfolders and its files (doubly linked through
 * next/prev), offsets from the handle, 0 for none */
typedef struct dirlists {
	u32 subfolders, subfolders_end;
	u32 subfiles, subfiles_end;
	u32 NumSubItems;
} dirlists;

/* for each archive item one record, which begins as follows */
typedef struct file_common {
	char *fullname;         /* pointer to cp866 string */
	char *name;             /* name without path (end of .fullname) */
	u32 namelen;            /* strlen(.name) */
	u8 bIsDirectory;
	u8 bPseudoFolder;
	u8 pad[2];
	u32 parent;             /* the parent folder's record (offset), 0: the root */
	dirlists sub;           /* [for folders] */
	u32 next;               /* next item in list of subfolders/files */
	u32 prev;               /* previous item in list of subfolders/files */
	u32 stamp;              /* stamp for GetFiles */
} file_common;

/* the handle of an open archive: the handle_xxx of its type, which begins
 * as follows (the records follow it) */
typedef struct handle_common {
	u32 type;               /* type_7z, type_zip */
	dirlists root;
	file_common *curdir;    /* the current folder, NULL: the root */
	u32 NumFiles;
} handle_common;

#define ITEM(h, off) ((file_common *)((u8 *)(h) + (off)))
#define OFF(h, p) ((u32)((u8 *)(p) - (u8 *)(h)))

/* kfar_arc supports many archive types: OpenFilePlugin looks for a known
 * signature, the other functions dispatch by the handle's type */
enum { type_mem_stream = 0, type_7z = 1, type_zip = 2 };

/* every file handle (from open) begins with its type */
typedef struct file_handle_common { u32 type; } file_handle_common;

/* ---- kfar_arc.asm's globals ---- */

#define password_maxlen 512
extern u32 crc_table[256];
extern u8 buffer[1024];
extern HANDLE inStream;
extern u8 *hOut;                /* the handle being built */
extern u32 hOut_allocated;
extern u8 bPasswordDefined;
extern char password_ansi[password_maxlen + 1];
extern u16 password_unicode[password_maxlen + 1];
extern u32 password_size;
extern u8 bWasWarning;

/* the strings (en_US) and their pointers for SayErr */
extern const char aContinue[], aCancel[], aHeaderError[], aReadError[], aNoFreeRam[], aEncodingProblem[],
	aEnterPassword[], aEnterPasswordTitle[], aArchiveDataError[], aArchiveDataErrorPass[],
	aChangePass[], aNameTooLong[], aCannotOpenFile[];
extern const char *ContinueBtn[1], *HeaderError_ptr[1], *aReadError_ptr[1], *aNoFreeRam_ptr[1],
	*aEncodingProblem_str_ptr[3], *aNameTooLong_ptr[1], *aArchiveDataError_ptr[1],
	*aArchiveDataErrorPass_ptr[1], *CancelPassBtn[2];
/* "Unknown method:" and the method (zip.inc, 7z.inc) */
extern char aUnknownMethod[16 + 16 * 3 + 1];
#define aUnknownMethod_z (aUnknownMethod + 15)
extern const char *aUnknownMethod_ptr[1];

/* ---- kfar_arc.asm's procedures ---- */

/* kfar_arc.asm: ntfs_datetime_to_bdfe: a FILETIME (100 ns since 1601) to
 * 70.1's 8 bytes of time and date at edi; returns edi + 8 */
u8 *ntfs_datetime_to_bdfe(u64 t, u8 *edi);
/* kfar_arc.asm: init_file_links: the "folder contains item" links of
 * ebx records of `size` bytes from hOut + edx (records compatible with
 * file_common), the root's lists at hOut + edi; folders named only in
 * the paths are new pseudo-folder records at the end of hOut (which may
 * move). false: no memory (hOut freed, the original's "or eax, -1") */
bool init_file_links(u32 edx, u32 ebx, u32 size, u32 edi);
/* kfar_arc.asm: lookup_file_name: the record of the absolute name, or 0 */
file_common *lookup_file_name(handle_common *ebp, const char *esi);
/* kfar_arc.asm: alloc_handle, free_handle: a file handle of ecx bytes
 * (the original's table of pages: here the C library's memory) */
void *alloc_handle(void *esi, u32 ecx);
void free_handle(void *esi);
/* kfar_arc.asm: query_password: false (ZF) if the user pressed Esc;
 * password_ansi, password_unicode, password_size filled */
bool query_password(void);

/* the memory streams (kfar_arc.asm: mem_stream): small files unpacked
 * at open */
typedef struct mem_stream {
	u32 type;               /* type_mem_stream */
	u32 size;
	u32 pos;
	u8 buf[];
} mem_stream;

/* crc.inc */
void init_crc_table(void);
/* crc: the CRC-32 of ecx bytes at esi; crc_continue from a previous one */
u32 crc(const u8 *esi, u32 ecx);
u32 crc_continue(u32 eax, const u8 *esi, u32 ecx);

/* ---- the streams of the decoders (7z.inc: streamInfo) ---- */

/* a stream: its buffer filled by fillBuf, which writes exactly ecx
 * bytes at edi (bufPtr + bufDataLen, both already advanced by
 * fillBufNozero; fullSize already lowered); a decoder's data follows the
 * header, its first word its input stream */
typedef struct streamInfo streamInfo;
typedef void fillBuf_t(streamInfo *eax, u8 *edi, u32 ecx);
struct streamInfo {
	u64 fullSize;           /* the bytes still to come */
	fillBuf_t *fillBuf;     /* procedure which fills the buffer */
	u32 bufSize;
	u32 bufDataLen;
	u8 *bufPtr;
};
_Static_assert(sizeof(streamInfo) == 24, "streamInfo");
/* a decoder: the stream, then its input (streamInfo.size) */
typedef struct decoder_common {
	streamInfo outStream;
	streamInfo *inStream;
} decoder_common;

/* 7z.inc: fillBuf, fillBufNozero: the buffer (emptied first, or after
 * its data) filled with up to bufSize bytes, at most fullSize */
void fillBuf(streamInfo *eax);
void fillBufNozero(streamInfo *eax);
/* 7z.inc: read_7z_to_buf: the stream's fullSize bytes to edi (the
 * buffer used as a ring); returns edi after them */
u8 *read_7z_to_buf(streamInfo *eax, u8 *edi);
/* 7z.inc: skip_7z: the stream's fullSize bytes read and dropped */
void skip_7z(streamInfo *eax);

/* deflate.inc */
typedef struct deflate_decoder {
	streamInfo outStream;
	streamInfo *inStream;
	u8 bDeflate64;
	u8 dl;                  /* the bits of the byte being read, a 1 above them */
	u8 bLast;
	u8 pad;
	u8 *outEnd;
	u32 inLen;
	const u8 *inPtr;
	u32 cont;               /* .continue: where fillBuf goes on */
	u32 blockLen;
	u8 *edi;                /* the output's position (the original's edi; deflate_unpack2's end) */
	u8 lit_len[19];
	u8 pad2;
	u8 lengths[288 + 32];
	u32 huff_bl[18 * 2];
	u32 huff_dist[31 * 2];
	u32 huff_lit[287 * 2];
} deflate_decoder;
void deflate_get_buf_size(u32 *eax, u32 *edx);
void deflate_init_decoder(streamInfo *ebp);
void deflate64_init_decoder(streamInfo *ebp);

/* ---- the archive types ---- */

/* zip.inc */
HANDLE open_zip(HANDLE basefile, const void *attr, void *baseplugin_id, HANDLE baseplugin_instance,
		const char *name);
void close_zip(handle_common *hPlugin);
void getattr_zip(handle_common *ebp, file_common *eax, u8 *edi);
void *open_file_zip(handle_common *ebp, file_common *eax, int edi);
unsigned read_zip(void *ebx, void *buf, unsigned size);
void setpos_zip(void *ebx, u64 pos);
void close_file_zip(void *ebx);
extern const u32 handle_zip_basesize, file_in_zip_size;

/* 7z.inc */
HANDLE open_7z(HANDLE basefile, const u8 *data, int datasize, const void *attr);
void close_7z(handle_common *hPlugin);
void getattr_7z(handle_common *ebp, file_common *eax, u8 *edi);
void *open_file_7z(handle_common *ebp, file_common *eax, int edi);
unsigned read_7z(void *ebx, void *buf, unsigned size);
void setpos_7z(void *ebx, u64 pos);
void close_file_7z(void *ebx);
extern const u32 handle_7z_basesize, file_in_7z_size;

/* the decoders of 7z.inc's coders: their buffers' sizes and their init */
void copy_get_buf_size(u32 *eax, u32 *edx);
void copy_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void lzma_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx);
void lzma_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void ppmd_get_buf_size(const u8 *esi, u32 params_size, u32 *eax, u32 *edx);
void ppmd_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void init_ppmd(void);
void bcj_get_buf_size(u32 *eax, u32 *edx);
void bcj_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void bcj2_get_buf_size(u32 *eax, u32 *edx);
void bcj2_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void aes7z_get_buf_size(u32 *eax, u32 *edx);
void aes7z_init_decoder(streamInfo *ebp, const u8 *esi, u32 params_size, int ebx);
void init_aes(void);

#endif
