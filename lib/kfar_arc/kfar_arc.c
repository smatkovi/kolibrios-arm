/* kfar_arc for KolibriOS for ARM: the interface for kfar, the auxiliary
 * procedures, the strings (kfar_arc.asm).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/
 * kfar_arc.asm (0.17), diamond (Evgeny Grechnikov), GPL-2.0
 * (kfar_arc.h).
 *
 * Differences from the original:
 * - the file handles (alloc_handle) come from kfar's pgalloc one by one,
 *   not from a table of pages;
 * - init_file_links keeps the parent of a folder whose record replaces
 *   its pseudo-folder (the original leaves the record's parent as it
 *   was, the root for zip's first records), and zeroes the new records;
 * - deflate_unpack, deflate_unpack2 take their memory from the C
 *   library (the caller frees the result with free), not 68.12/68.20. */
#include "kfar_arc.h"

/* ---- import from kfar (kfar_arc.asm's .udata) ---- */

kfar_info *kfar_info_ptr;
HANDLE (*open2)(void *plugin_id, HANDLE plugin_instance, const char *name, int mode);
long long (*filesize)(HANDLE hFile);
unsigned (*read)(HANDLE hFile, void *buf, unsigned size);
void (*seek)(HANDLE hFile, int method, long long newpos);
void (*close)(HANDLE hFile);
void *(*pgalloc)(unsigned size);
void *(*pgrealloc)(void *pointer, unsigned size);
void (*pgfree)(void *pointer);
unsigned (*getfreemem)(void);
int (*DialogBox)(void *dlg);
int (*SayErr)(int num_strings, const char **strings, int num_buttons, const char **buttons);
int (*Message)(const char *title, int num_strings, const char **strings, int num_buttons,
	       const char **buttons);
struct kfar_console_size *cur_console_size;

u32 crc_table[256];
u8 buffer[1024];
HANDLE inStream;
u8 *hOut;
u32 hOut_allocated;
u8 bWasWarning;

void *return_jmp[5];
int return_kind;

/* data for editbox in kfar dialog */
static struct {
	u32 maxlen, pos, start;
	char ansi[password_maxlen + 1];
} password_data;
char password_ansi[password_maxlen + 1];
u8 bPasswordDefined;
/* converted password */
u16 password_unicode[password_maxlen + 1];
u32 password_size;

/* ---- strings ---- */

const char aContinue[] = "Continue";
const char aCancel[] = "Cancel";
const char aHeaderError[] = "Invalid archive header";
const char aReadError[] = "Read error";
const char aNoFreeRam[] = "There is not enough of free RAM";
const char aEncodingProblem[] = "Encoding problem";
static const char aEncodingProblem_str[] = "The names of some files in the archive contain";
static const char aEncodingProblem_str_2[] = "characters which can not be represented in cp866.";
static const char aEncodingProblem_str_3[] = "Such characters will be replaced to underscores.";
const char aEnterPassword[] = "Enter password:";
const char aEnterPasswordTitle[] = "Get password";
const char aArchiveDataError[] = "Error in archive data";
const char aArchiveDataErrorPass[] = "Error in archive data or incorrect password";
const char aChangePass[] = "Enter password";
const char aNameTooLong[] = "Name is too long";
const char aCannotOpenFile[] = "Cannot open file";

/* pointers for SayErr and Message */
const char *ContinueBtn[1] = { aContinue };
const char *HeaderError_ptr[1] = { aHeaderError };
const char *aReadError_ptr[1] = { aReadError };
const char *aNoFreeRam_ptr[1] = { aNoFreeRam };
const char *aEncodingProblem_str_ptr[3] = { aEncodingProblem_str, aEncodingProblem_str_2, aEncodingProblem_str_3 };
const char *aNameTooLong_ptr[1] = { aNameTooLong };
const char *aArchiveDataError_ptr[1] = { aArchiveDataError };
const char *aArchiveDataErrorPass_ptr[1] = { aArchiveDataErrorPass };
const char *CancelPassBtn[2] = { aCancel, aChangePass };

/* 7z.inc: aUnknownMethod, its .z filled with the method */
char aUnknownMethod[16 + 16 * 3 + 1] = "Unknown method:";
const char *aUnknownMethod_ptr[1] = { aUnknownMethod };

/* "enter password" dialog for KFar */
static struct {
	DLGDATA h;
	dlgitemtemplate lbl, edit;
} password_dlg = {
	{ { 1,                          /* use standard dialog colors */
	    -1, -1,                     /* center window by x and y */
	    0,                          /* width (filled according to the current console width) */
	    2,                          /* height */
	    4, 2,                       /* border size */
	    aEnterPasswordTitle, 0, 0, 0, 0,    /* title; the colors (set by KFar) */
	    0 },                        /* used internally by dialog manager, ignored */
	  0, 0,                         /* reserved for DlgProc */
	  2 },                          /* 2 controls */
	/* the string "enter password": static */
	{ 1, 1, 0, 0, 0, (void *)aEnterPassword, 0 },
	/* editbox for password */
	{ 3, 1, 1, 0, 0, &password_data, 0x2C },
};

/* ---- the archive types' functions (types start from type_7z) ---- */

static void (*const ClosePluginTable[])(handle_common *) = { close_7z, close_zip };
static void (*const getattrTable[])(handle_common *, file_common *, u8 *) = { getattr_7z, getattr_zip };
static void *(*const openTable[])(handle_common *, file_common *, int) = { open_file_7z, open_file_zip };
static const u32 *const basesizes[][2] = {
	{ &handle_7z_basesize, &file_in_7z_size },
	{ &handle_zip_basesize, &file_in_zip_size },
};

/* file functions (types start from type_mem_stream) */
static unsigned read_mem_stream(void *ebx, void *buf, unsigned size);
static void setpos_mem_stream(void *ebx, u64 pos);
static void close_mem_stream(void *ebx);
static unsigned (*const readTable[])(void *, void *, unsigned) = { read_mem_stream, read_7z, read_zip };
static void (*const setposTable[])(void *, u64) = { setpos_mem_stream, setpos_7z, setpos_zip };
static void (*const closeTable[])(void *) = { close_mem_stream, close_file_7z, close_file_zip };

/* ---- Interface for KFar ---- */

/* int __stdcall plugin_load(kfar_info* info): initialization of plugin
 * + save used KFar functions */
int plugin_load(kfar_info *info)
{
	kfar_info_ptr = info;
	open2 = info->open2;
	filesize = info->filesize;
	read = info->read;
	seek = info->seek;
	close = info->close;
	DialogBox = info->DialogBox;
	SayErr = info->SayErr;
	Message = info->Message;
	cur_console_size = info->cur_console_size;
	pgalloc = info->pgalloc;
	pgrealloc = info->pgrealloc;
	pgfree = info->pgfree;
	getfreemem = info->getfreemem;
	init_crc_table();
	init_aes();
	init_ppmd();
	return 0;                               /* success */
}

/* HANDLE __stdcall OpenFilePlugin(HANDLE basefile, const void* attr,
 * const void* data, int datasize, int baseplugin_id, HANDLE
 * baseplugin_instance, const char* name): called when the user presses
 * Enter (or Ctrl+PgDn) on a file; the plugin tests whether the file is
 * of a supported type and if so, loads information and returns the
 * handle for ReadFolder, SetFolder and so on */
HANDLE OpenFilePlugin(HANDLE basefile, const void *attr, const void *data, int datasize,
		      void *baseplugin_id, HANDLE baseplugin_instance, const char *name)
{
	const u8 *esi = data;
	bPasswordDefined = 0;
	/* test for 7z archive: minimal size of 7z archive is 20h bytes; the
	 * signature '7z' 0xBC 0xAF 0x27 0x1C */
	if (datasize >= 0x20 && esi[0] == '7' && esi[1] == 'z' && esi[2] == 0xBC && esi[3] == 0xAF &&
	    esi[4] == 0x27 && esi[5] == 0x1C)
		return open_7z(basefile, esi, datasize, attr);
	/* .no_7z: test for zip archive: minimal size of zip archive is 22
	 * bytes; PK 03 04, 01 02, 06 06, 07 06, 05 06 */
	if (datasize >= 22 && esi[0] == 'P' && esi[1] == 'K') {
		u16 w = esi[2] | esi[3] << 8;
		if (w == 0x0403 || w == 0x0201 || w == 0x0606 || w == 0x0706 || w == 0x0605)
			return open_zip(basefile, attr, baseplugin_id, baseplugin_instance, name);
	}
	/* .no_zip */
	return 0;
}

/* void __stdcall ClosePlugin(HANDLE hPlugin): frees all resources
 * allocated in OpenFilePlugin (the first word of the handle is the
 * archive's type, they start from 1) */
void ClosePlugin(HANDLE hPlugin)
{
	handle_common *eax = hPlugin;
	ClosePluginTable[eax->type - 1](eax);
}

/* the current folder's lists */
static dirlists *curdir_lists(handle_common *ebp)
{
	return ebp->curdir ? &ebp->curdir->sub : &ebp->root;
}

/* int __stdcall ReadFolder(HANDLE hPlugin, unsigned dirinfo_start,
 * unsigned dirinfo_size, void* dirdata): as function 70.1 */
int ReadFolder(HANDLE hPlugin, unsigned dirinfo_start, unsigned dirinfo_size, void *dirdata)
{
	/* init header */
	u8 *edi = dirdata;
	memset(edi, 0, 32);
	edi[0] = 1;                             /* version */
	handle_common *ebp = hPlugin;
	/* get current directory */
	dirlists *ebx = curdir_lists(ebp);
	u32 ecx = ebx->NumSubItems;
	*(u32 *)(edi + 8) = ecx;                /* number of files */
	edi += 32;                              /* edi points to BDFE */
	int eax = 6;                            /* assume EOF */
	if (ecx <= dirinfo_start) {
		*(u32 *)(edi - 28) = 0;         /* number of files read */
		return eax;
	}
	ecx -= dirinfo_start;
	if (ecx >= dirinfo_size) {
		ecx = dirinfo_size;
		eax = 0;                        /* OK */
	}
	*(u32 *)(edi - 28) = ecx;
	if (!ecx) return eax;
	/* seek to required item: the subfolders, then the files (each list
	 * ends with next = 0) */
	u32 o = ebx->subfolders ? ebx->subfolders : ebx->subfiles;
	file_common *esi = ITEM(ebp, o);
	for (u32 n = dirinfo_start; n; n--) {
		o = esi->next;
		if (!o) o = ebx->subfiles;
		esi = ITEM(ebp, o);
	}
	/* .copy: copy files data */
	for (;;) {
		getattrTable[ebp->type - 1](ebp, esi, edi);
		memcpy(edi + 40, esi->name, esi->namelen);
		edi[40 + esi->namelen] = 0;
		edi += 304;
		if (!--ecx) break;
		o = esi->next;
		if (!o) o = ebx->subfiles;
		esi = ITEM(ebp, o);
	}
	/* .done */
	return eax;
}

/* bool __stdcall SetFolder(HANDLE hPlugin, const char* relative_path,
 * const char* absolute_path): ".." or a subfolder of the current one */
bool SetFolder(HANDLE hPlugin, const char *relative_path, const char *absolute_path)
{
	handle_common *ebp = hPlugin;
	file_common *edx = ebp->curdir, *ebx;
	const char *esi = relative_path;
	if (!strcmp(esi, "..")) {
		/* .toparent */
		if (!edx) return false;         /* .err */
		ebx = edx->parent ? ITEM(ebp, edx->parent) : 0;
	} else {
		u32 ecx = strlen(esi);
		u32 o = edx ? edx->sub.subfolders : ebp->root.subfolders;
		for (;;) {
			/* .scan */
			if (!o) return false;   /* .err */
			ebx = ITEM(ebp, o);
			if (ebx->namelen == ecx && !memcmp(esi, ebx->name, ecx)) break;
			o = ebx->next;          /* .cont */
		}
	}
	/* .set */
	ebp->curdir = ebx;
	return true;
}

static u32 cur_stamp;
static u8 tmp_bdfe[304] __attribute__((aligned(8)));

/* the record's name ended for a moment: a pseudo-folder's fullname is
 * another item's, not null-terminated after it */
static bool call_adddir(handle_common *ebp, file_common *edx, const char *name, adddir_t *adddir)
{
	char *ecx = edx->name + edx->namelen;
	char saved = *ecx;
	*ecx = 0;
	getattrTable[ebp->type - 1](ebp, edx, tmp_bdfe);
	bool al = adddir(name, tmp_bdfe);
	*ecx = saved;
	return al;
}

/* void __stdcall GetFiles(HANDLE hPlugin, int NumItems, void* items[],
 * void* addfile, void* adddir): all items of the current folder and its
 * subfolders that are (or are in) one of items[] (BDFEs; NumItems = -1:
 * all), each folder once (the stamps) and before what it holds */
void GetFiles(HANDLE hPlugin, int NumItems, void *items[], addfile_t *addfile, adddir_t *adddir)
{
	handle_common *ebp = hPlugin;
	u32 ecx = ebp->NumFiles;
	if (!ecx) return;
	u8 *ebx_p = (u8 *)ebp + *basesizes[ebp->type - 1][0];
	u32 size = *basesizes[ebp->type - 1][1];
	cur_stamp++;
	for (; ecx; ecx--, ebx_p += size) {
		/* .loop */
		file_common *ebx = (file_common *)ebx_p;
		const char *esi = ebx->fullname;
		file_common *edx = ebp->curdir;
		if (edx) {
			edx->stamp = cur_stamp;
			u32 n = edx->name + edx->namelen - edx->fullname;
			if (memcmp(esi, edx->fullname, n)) continue;    /* .cont */
			esi += n;
		}
		/* .incur */
		if (*esi == '/') esi++;
		if (NumItems != -1) {
			int i;
			for (i = 0; i < NumItems; i++) {
				/* .check: the name, or a name in the folder items[i] */
				const char *s = esi, *t = (const char *)items[i] + 40;
				for (; *s == *t && *s; s++, t++);
				if (*s == *t) break;                    /* .ok2 */
				if (*s == '/' && !*t) break;            /* .ok */
			}
			if (i == NumItems) continue;
		}
		/* .ok: add all parents directories if needed */
		for (;;) {
			/* .parloope */
			if (!ebx->parent) break;
			file_common *p = ITEM(ebp, ebx->parent);
			if (p->stamp == cur_stamp) break;
			/* .parloopi: the topmost not added yet */
			file_common *top = p;
			while (top->parent && ITEM(ebp, top->parent)->stamp != cur_stamp) top = ITEM(ebp, top->parent);
			top->stamp = cur_stamp;
			if (!call_adddir(ebp, top, esi - ebx->fullname + top->fullname, adddir)) return;
		}
		/* .pardone */
		if (ebx->bIsDirectory) {
			if (ebx->stamp == cur_stamp) continue;
			ebx->stamp = cur_stamp;
			if (!call_adddir(ebp, ebx, esi, adddir)) return;
			continue;
		}
		/* .addfile */
		void *eax = openTable[ebp->type - 1](ebp, ebx, O_READ + O_SEQUENTIAL_ONLY);
		if (!eax) continue;
		getattrTable[ebp->type - 1](ebp, ebx, tmp_bdfe);
		bool al = addfile(esi, tmp_bdfe, eax);
		myclose(eax);
		if (!al) return;                /* .forced_exit */
	}
}

/* void __stdcall GetOpenPluginInfo(HANDLE hPlugin, OpenPluginInfo* info) */
void GetOpenPluginInfo(HANDLE hPlugin, OpenPluginInfo *info)
{
	/* flags: add non-existing '..' entry automatically, use GetFiles for
	 * copying (the original writes the low byte) */
	*(u8 *)&info->flags = 3;
}

/* int __stdcall getattr(HANDLE hPlugin, const char* filename, void* info) */
int mygetattr(HANDLE hPlugin, const char *filename, void *info)
{
	handle_common *ebp = hPlugin;
	file_common *eax = lookup_file_name(ebp, filename);
	if (!eax) return 5;                     /* ERROR_FILE_NOT_FOUND */
	getattrTable[ebp->type - 1](ebp, eax, info);
	return 0;
}

/* HANDLE __stdcall open(HANDLE hPlugin, const char* filename, int mode) */
HANDLE myopen(HANDLE hPlugin, const char *filename, int mode)
{
	handle_common *ebp = hPlugin;
	file_common *eax = lookup_file_name(ebp, filename);
	if (!eax) return 0;
	return openTable[ebp->type - 1](ebp, eax, mode);
}

/* unsigned __stdcall read(HANDLE hFile, void* buf, unsigned size) */
unsigned myread(HANDLE hFile, void *buf, unsigned size)
{
	return readTable[((file_handle_common *)hFile)->type](hFile, buf, size);
}

/* void __stdcall setpos(HANDLE hFile, __int64 pos) */
void mysetpos(HANDLE hFile, long long pos)
{
	setposTable[((file_handle_common *)hFile)->type](hFile, pos);
}

/* void __stdcall close(HANDLE hFile) */
void myclose(HANDLE hFile)
{
	closeTable[((file_handle_common *)hFile)->type](hFile);
}

/* ---- Auxiliary procedures ---- */

/* return.err and return.clear are labels to jmp if something is
 * invalid; the caller must previously define [_esp], [_ebp] and
 * [error_proc], [clear_proc]: here RETURN_CATCH */
void return_err(void)
{
	return_kind = RETURN_ERR;
	__builtin_longjmp(return_jmp, 1);
}

void return_clear(void)
{
	return_kind = RETURN_CLEAR;
	__builtin_longjmp(return_jmp, 1);
}

static const u32 months[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const u32 months2[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
#define days400year (365 * 400 + 100 - 4 + 1)
#define days100year (365 * 100 + 25 - 1)
#define days4year (365 * 4 + 1)
#define days1year 365

/* kfar_arc.asm: ntfs_datetime_to_bdfe: convert QWORD FILETIME to BDFE
 * format; t = number of 100-nanosecond intervals since January 1, 1601,
 * in UTC */
u8 *ntfs_datetime_to_bdfe(u64 t, u8 *edi)
{
	u64 sec = t / 10000000;                 /* seconds since January 1, 1601 */
	edi[0] = sec % 60;
	u64 min = sec / 60;                     /* minutes */
	edi[1] = min % 60;
	u32 hours = min / 60;                   /* (2^64/(10^7*60*60) < 2^32) */
	edi[2] = hours % 24;
	edi[3] = 0;
	u32 eax = hours / 24;                   /* days since January 1, 1601 */
	u32 edx = eax % days400year;
	u32 year = eax / days400year * 400 + 1601;
	eax = edx / days100year;
	edx %= days100year;
	if (eax == 4) {
		eax--;
		edx += days100year;
	}
	year += eax * 100;
	year += edx / days4year * 4;
	edx %= days4year;
	eax = edx / days1year;
	edx %= days1year;
	if (eax == 4) {
		eax--;
		edx += days1year;
	}
	year += eax;
	edi[6] = year;
	edi[7] = year >> 8;
	const u32 *esi = (year % 4 == 0 && (year % 400 == 0 || year % 100 != 0)) ? months2 : months;
	u32 month = 1;
	while (edx >= *esi) {
		edx -= *esi++;
		month++;
	}
	edi[4] = edx + 1;
	edi[5] = month;
	return edi + 8;
}

#define LISTS(off) ((dirlists *)(hOut + (off)))
#define REC(off) ITEM(hOut, off)

/* kfar_arc.asm: init_file_links: by given array of files information,
 * initialize links between them ("[folder] contains [item]" relations) */
bool init_file_links(u32 edx, u32 ebx, u32 size, u32 edi)
{
	u32 free_list = 0;                      /* init_file_links.free */
	memset(LISTS(edi), 0, sizeof(dirlists));
	/* loop through all files */
	for (; ebx; ebx--, edx += size) {
		/* .mainloop: parse the file name; ecx: the current folder's lists */
		const char *esi = REC(edx)->fullname;
		u32 ecx = edi;
		for (;;) {
			/* .parseloop: the next component of the name */
			const char *start = esi;
			while (*esi && *esi != '/') esi++;
			u32 len = esi - start;
			if (len > 259) {
				SayErr(1, aNameTooLong_ptr, 1, ContinueBtn);
				return_clear();
			}
			/* look for it in the current folder */
			u32 eax = LISTS(ecx)->subfolders;
			while (eax && !(REC(eax)->namelen == len && !memcmp(start, REC(eax)->name, len)))
				eax = REC(eax)->next;           /* .scansubfolders */
			if (eax) {
				/* .subfound: the subfolder is the current one */
				ecx = eax + offsetof(file_common, sub);
				if (*esi) {
					esi++;
					continue;
				}
				/* that was the last component of the name, and we
				 * have found the subfolder: it is a virtual one and
				 * the current record takes its place */
				file_common *old = REC(eax), *cur = REC(edx);
				cur->namelen = old->namelen;
				cur->name = (char *)start;
				cur->parent = old->parent;
				/* replace item in L2-list */
				dirlists *pl = old->parent ? &REC(old->parent)->sub : LISTS(edi);
				if (old->prev) REC(old->prev)->next = edx;
				else pl->subfolders = edx;
				if (old->next) REC(old->next)->prev = edx;
				else pl->subfolders_end = edx;
				/* correct parent links in childrens */
				for (u32 c = old->sub.subfolders; c; c = REC(c)->next) REC(c)->parent = edx;
				for (u32 c = old->sub.subfiles; c; c = REC(c)->next) REC(c)->parent = edx;
				/* set children links, prev/next links */
				cur->sub = old->sub;
				cur->next = old->next;
				cur->prev = old->prev;
				/* add old item to list of free items */
				*(u32 *)old = free_list;
				free_list = eax;
				break;                          /* .mainloopcont */
			}
			/* .nofolder */
			u32 item = edx;
			if (*esi) {
				/* the current item is as 'dir1/item1' and 'dir1' has
				 * not been found: allocate virtual subfolder 'dir1' */
				if (free_list) {
					item = free_list;
					free_list = *(u32 *)REC(item);
				} else {
					/* .realloc: there is no free space, so reallocate
					 * the hOut block */
					u8 *n = pgrealloc(hOut, hOut_allocated + size);
					if (!n) {
						pgfree(hOut);
						hOut = 0;
						return false;
					}
					hOut = n;
					item = hOut_allocated;
					hOut_allocated += size;
				}
				/* .allocated */
				memset(REC(item), 0, size);
				REC(item)->bIsDirectory = 1;
				REC(item)->bPseudoFolder = 1;
			}
			/* .newitem (in this case .fullname is not null-terminated) */
			file_common *e = REC(item);
			e->namelen = len;
			e->fullname = REC(edx)->fullname;
			e->parent = 0;
			memset(&e->sub, 0, sizeof(e->sub));
			e->next = 0;
			e->name = (char *)start;
			LISTS(ecx)->NumSubItems++;      /* new item in parent folder */
			/* add new item to end of L2-list */
			u32 *list = e->bIsDirectory ? &LISTS(ecx)->subfolders : &LISTS(ecx)->subfiles;
			if (!list[0]) list[0] = item;
			u32 prev = list[1];
			list[1] = item;
			e->prev = prev;
			if (prev) REC(prev)->next = item;
			/* set parent link */
			if (ecx != edi) e->parent = ecx - offsetof(file_common, sub);
			/* set current directory to current item */
			ecx = item + offsetof(file_common, sub);
			/* if that was not last component, continue parse name */
			if (!*esi) break;
			esi++;
		}
		/* .mainloopcont */
	}
	return true;
}

/* kfar_arc.asm: lookup_file_name: called by getattr and open: the file
 * info record of the name, or NULL */
file_common *lookup_file_name(handle_common *ebp, const char *esi)
{
	dirlists *edi = &ebp->root;
	/* KFar operates with absolute names, skip first '/' */
	if (*esi != '/') return 0;
	esi++;
	for (;;) {
		/* .mainloop: get next component of name */
		u32 ecx = 0;
		while (esi[ecx] && esi[ecx] != '/') ecx++;
		/* scan for required item in subfolders list; if this is the last
		 * component in file name, in subfiles list then */
		file_common *eax = 0;
		for (int pass = 0; pass < 2 && !eax; pass++) {
			if (pass && esi[ecx]) return 0;         /* .notfound */
			for (u32 o = pass ? edi->subfiles : edi->subfolders; o; o = ITEM(ebp, o)->next)
				if (ITEM(ebp, o)->namelen == ecx && !memcmp(esi, ITEM(ebp, o)->name, ecx)) {
					eax = ITEM(ebp, o);     /* .found1 */
					break;
				}
		}
		if (!eax) return 0;
		/* item is found, go to next component */
		edi = &eax->sub;
		esi += ecx + 1;
		if (!esi[-1]) return eax;               /* this was the last component */
	}
}

/* ---- memory streams: archive handlers create memory stream for small
 * files, not greater than (free RAM size)/4 (LIMIT_FOR_MEM_STREAM is
 * not defined: if user has many physical memory, why not to use it?) */

/* unsigned __stdcall read(ebx = HANDLE hFile, void* buf, unsigned size) */
static unsigned read_mem_stream(void *p, void *buf, unsigned size)
{
	mem_stream *ebx = p;
	u32 ecx = ebx->size > ebx->pos ? ebx->size - ebx->pos : 0;
	u32 eax = size < ecx ? size : ecx;
	memcpy(buf, ebx->buf + ebx->pos, eax);
	ebx->pos += eax;
	return eax;
}

/* void __stdcall setpos(ebx = HANDLE hFile, __int64 pos) */
static void setpos_mem_stream(void *p, u64 pos)
{
	((mem_stream *)p)->pos = pos;
}

/* void __stdcall close(ebx = HANDLE hFile) */
static void close_mem_stream(void *p)
{
	pgfree(p);
}

/* kfar_arc.asm: alloc_handle: esi -> handle table, ecx = size of handle */
void *alloc_handle(void *esi, u32 ecx)
{
	return pgalloc(ecx);
}

/* kfar_arc.asm: free_handle: esi = handle */
void free_handle(void *esi)
{
	pgfree(esi);
}

/* kfar_arc.asm: query_password: ask user to enter password; false
 * (ZF set) <=> user pressed Esc */
bool query_password(void)
{
	if (bPasswordDefined) return true;
	password_data.maxlen = password_maxlen; /* maximum length */
	password_data.pos = 0;                  /* start of visible part */
	password_data.start = 0;                /* position of cursor */
	password_data.ansi[0] = 0;              /* initial state: empty string */
	int eax = cur_console_size->width - 12; /* get current console width */
	password_dlg.h.dialog.width = eax;
	password_dlg.lbl.x2 = eax - 2;
	password_dlg.edit.x2 = eax - 2;
	if (DialogBox(&password_dlg) == -1) return false;
	strcpy(password_ansi, password_data.ansi);
	/* convert ANSI-cp866 to UNICODE string; also calculate
	 * 'password_size' */
	const u8 *esi = (const u8 *)password_ansi;
	u16 *edi = password_unicode;
	password_size = (u32)-1;
	for (;;) {
		password_size++;
		u8 al = *esi++;
		u16 ax;
		if (al < 0x80) ax = al;                         /* 0x00-0x7F - trivial map */
		else if (al < 0xB0) ax = al + 0x410 - 0x80;     /* 0x80-0xAF -> 0x410-0x43F */
		else if (al >= 0xE0 && al < 0xF0) ax = al + 0x440 - 0xE0;      /* 0xE0-0xEF -> 0x440-0x44F */
		else if (al == 0xF0) ax = 0x401;                /* 0xF0 -> 0x401 */
		else if (al == 0xF1) ax = 0x451;                /* 0xF1 -> 0x451 */
		else ax = '_';                                  /* .unk */
		*edi++ = ax;
		if (!al) break;
	}
	bPasswordDefined++;
	return true;
}

/* ---- API for other programs ---- */

/* deflate_unpack2's input: the stream, the caller's function and its
 * parameter (streamInfo.size, +4) */
typedef struct unpack_in {
	streamInfo s;
	void *(*get_next_chunk)(void *parameter, unsigned *pLength);
	void *parameter;
} unpack_in;

/* deflate_unpack2.fillBuf: the next chunk is the buffer */
static void deflate_unpack2_fillBuf(streamInfo *eax, u8 *edi, u32 ecx)
{
	unpack_in *ebp = (unpack_in *)eax;
	unsigned edx = 0;
	u8 *p = ebp->get_next_chunk(ebp->parameter, &edx);
	ebp->s.bufPtr = p;
	ebp->s.bufDataLen = p ? edx : 0;
}

/* void* __stdcall deflate_unpack2(void* get_next_chunk, void* parameter,
 * unsigned* pUnpackedLength): void* __stdcall get_next_chunk(void*
 * parameter, unsigned* pLength) gives the packed data in pieces; the
 * unpacked data (to be freed with free) or NULL */
void *deflate_unpack2(void *(*get_next_chunk)(void *parameter, unsigned *pLength), void *parameter,
		      unsigned *pUnpackedLength)
{
	struct {
		unpack_in in;
		deflate_decoder dec;
		u8 buf[0x10000];
	} *volatile eax = malloc(sizeof(*eax));
	u8 *volatile edx = 0;
	volatile u32 ecx = 0x10000;
	if (!eax) return 0;
	eax->in.s.fullSize = (u64)-1;
	eax->in.s.bufSize = 0;
	eax->in.s.bufDataLen = 0;
	eax->in.s.fillBuf = deflate_unpack2_fillBuf;
	eax->in.get_next_chunk = get_next_chunk;
	eax->in.parameter = parameter;
	eax->dec.inStream = &eax->in.s;
	eax->dec.outStream.bufPtr = eax->buf;
	deflate_init_decoder(&eax->dec.outStream);
	RETURN_CATCH(saved) {
		/* .error: the end of the data (or an error in it): the bytes
		 * of the last round, up to where the decoder stopped */
		u32 n = ecx - 0x10000 + (eax->dec.edi - eax->buf);
		if (pUnpackedLength) *pUnpackedLength = n;
		u8 *r = realloc(edx, n ? n : 1);
		if (!r) {
			free(edx);              /* .nomem */
			free(eax);
			return 0;
		}
		memcpy(r + ecx - 0x10000, eax->buf, eax->dec.edi - eax->buf);
		free(eax);
		return r;
	}
	for (;;) {
		/* .loop: 64 KiB at a time, until the decoder fails */
		eax->dec.outStream.fillBuf(&eax->dec.outStream, eax->buf, 0x10000);
		u8 *n = realloc(edx, ecx);
		if (!n) {
			RETURN_UNCATCH(saved);
			free(edx);              /* .nomem */
			free(eax);
			return 0;
		}
		edx = n;
		memcpy(edx + ecx - 0x10000, eax->buf, 0x10000);
		ecx += 0x10000;
	}
}

/* deflate_unpack_cb: the whole packed data in one chunk */
static void *deflate_unpack_cb(void *parameter, unsigned *pLength)
{
	void **p = parameter;
	*pLength = *(unsigned *)p[1];
	void *eax = p[0];
	p[0] = 0;
	return eax;
}

/* void* __stdcall deflate_unpack(void* packed_data, unsigned* pLength):
 * *pLength the packed size in, the unpacked size out */
void *deflate_unpack(void *packed_data, unsigned *pLength)
{
	void *p[2] = { packed_data, pLength };
	return deflate_unpack2(deflate_unpack_cb, p, pLength);
}
