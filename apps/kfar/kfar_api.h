/* kfar's plugin interface on KolibriOS for ARM: the structures and
 * functions both sides use, kfar (apps/kfar) and its plugins (lib/
 * kfar_arc, built with -Iapps/kfar).
 *
 * The interface is KolibriOS's (programs/fs/kfar/api.txt and kfar.asm,
 * diamond (Evgeny Grechnikov), GPL-2.0), interface version 3 (kfar 0.65
 * takes 3 only). A plugin is a library loaded by function 68.19, an ARM
 * ELF shared object with KolibriOS's export table (docs/threads.md),
 * exporting
 *
 *     lib_init         (the ARM libraries' start: kfar calls it first,
 *                       with its malloc, free, realloc and dll_load)
 *     version          the interface's version, a value: 3
 *     plugin_load ... GetFiles, getattr, open, read, setpos, close,
 *     EditInfoSize     as api.txt describes them (any may be missing:
 *                      kfar has defaults, kfar.asm's *_default)
 *
 * The ABI: every function and callback is a plain AAPCS C function with
 * api.txt's stdcall signature (the types below; __int64 is long long, in
 * an even register pair or on the stack as AAPCS has it, bool is C's).
 * The callbacks with register conventions in api.txt become C
 * functions: pgalloc (in ecx = size, out eax) is void *pgalloc(unsigned
 * size), pgrealloc (edx = pointer, ecx = size) void *pgrealloc(void *p,
 * unsigned size), pgfree (ecx = pointer) void pgfree(void *p). Memory
 * from them is kfar's heap (the C library's malloc); the plugin's own
 * malloc is the same one, through lib_init. Strings are cp866, names in
 * a plugin's file system absolute ("/folder/file"). */
#ifndef KFAR_API_H
#define KFAR_API_H

#include <stddef.h>
#include <stdbool.h>

/* a file's or a plugin's handle */
typedef void *HANDLE;

/* the interface's version: a plugin's export "version" */
#define KFAR_INTERFACE_VERSION 3

/* open's modes (filetool.inc) */
enum { O_READ = 1, O_WRITE = 2, O_CREATE = 4, O_TRUNCATE = 8, O_SEQUENTIAL_ONLY = 0x10 };

/* the console's size in characters (kfar_info.cur_console_size) */
struct kfar_console_size { int width, height; };

/* int __stdcall plugin_load(kfar_info* info): what kfar offers (kfar.asm:
 * kfar_info, a table of words) */
typedef struct kfar_info {
	int StructSize;                 /* sizeof(kfar_info) */
	int kfar_ver;                   /* 10000h*major + minor */
	/* the files (open: a physical file; open2(0, any, name, mode) =
	 * open(name, mode), otherwise a file on a plugin's panel) */
	HANDLE (*open)(const char *name, int mode);
	HANDLE (*open2)(void *plugin_id, HANDLE plugin_instance, const char *name, int mode);
	unsigned (*read)(HANDLE hFile, void *buf, unsigned size);
	void *write;                    /* (void *)-1: not implemented */
	void (*seek)(HANDLE hFile, int method, long long newpos);
	long long (*tell)(HANDLE hFile);
	void *flush;                    /* (void *)-1: not implemented */
	long long (*filesize)(HANDLE hFile);
	void (*close)(HANDLE hFile);
	/* the memory: they tell the user when there is none and return NULL */
	void *(*pgalloc)(unsigned size);
	void *(*pgrealloc)(void *pointer, unsigned size);
	void (*pgfree)(void *pointer);
	unsigned (*getfreemem)(void);   /* the free RAM in KiB */
	void *(*pgalloc2)(unsigned size);
	void *(*pgrealloc2)(void *pointer, unsigned size);
	void (*pgfree2)(void *pointer);
	/* the dialogs (dialogs.inc): variants -> the current item of a
	 * doubly linked list (listitem) */
	int (*menu)(void *variants, const char *title, unsigned flags);
	int (*menu_centered_in)(int left, int top, int width, int height, void *variants,
				const char *title, unsigned flags);
	int (*DialogBox)(void *dlg);    /* DLGDATA */
	int (*SayErr)(int num_strings, const char **strings, int num_buttons, const char **buttons);
	int (*Message)(const char *title, int num_strings, const char **strings, int num_buttons,
		       const char **buttons);
	struct kfar_console_size *cur_console_size;
} kfar_info;

_Static_assert(offsetof(kfar_info, open) == 8 && offsetof(kfar_info, close) == 40 &&
	       offsetof(kfar_info, pgalloc) == 44 && offsetof(kfar_info, pgfree2) == 68 &&
	       offsetof(kfar_info, menu) == 72 && offsetof(kfar_info, Message) == 88 &&
	       offsetof(kfar_info, cur_console_size) == 92 && sizeof(kfar_info) == 96,
	       "kfar_info: api.txt's table of 24 words");

/* void __stdcall GetOpenPluginInfo(HANDLE hPlugin, OpenPluginInfo* Info) */
typedef struct OpenPluginInfo {
	unsigned flags;         /* bit 0: add '..' if missing; bit 1: GetFiles copies */
} OpenPluginInfo;
_Static_assert(sizeof(OpenPluginInfo) == 4, "OpenPluginInfo");

/* a folder entry (function 70.1's BDFE, 70.5's information in its first
 * 40 bytes): attributes, the name's encoding, the times, the size, the
 * name (cp866) */
#define BDFE_SIZE 304
#define BDFE_INFO_SIZE 40

/* the plugin's exports (kfar.asm: plugin_exported) */
typedef int plugin_load_t(kfar_info *info);    /* 0 ok, 1 failed (kfar says so), 2 failed quietly */
typedef void plugin_unload_t(void);
/* basefile: kfar's file (its read and seek); attr: 70.1's entry; data:
 * the file's first datasize (min(1024, its size)) bytes; baseplugin_id:
 * the panel's plugin or 0, baseplugin_instance its handle; name: the
 * full name. A new handle, 0 (not mine) or -1 (cancelled); a plugin
 * that takes the file closes basefile (then or in ClosePlugin) */
typedef HANDLE OpenFilePlugin_t(HANDLE basefile, const void *attr, const void *data, int datasize,
				void *baseplugin_id, HANDLE baseplugin_instance, const char *name);
typedef void ClosePlugin_t(HANDLE hPlugin);
typedef void GetOpenPluginInfo_t(HANDLE hPlugin, OpenPluginInfo *Info);
typedef void GetPanelTitle_t(HANDLE hPlugin, char title[1024], const char *host_file, const char *curdir);
/* as function 70.1: the result, dirdata the 32-byte header and BDFEs */
typedef int ReadFolder_t(HANDLE hPlugin, unsigned dirinfo_start, unsigned dirinfo_size, void *dirdata);
/* relative_path: ".." or a subfolder's name; absolute_path: the folder */
typedef bool SetFolder_t(HANDLE hPlugin, const char *relative_path, const char *absolute_path);
/* GetFiles' callbacks: false stops the copy; name relative to the
 * current folder, bdfe_info 40 bytes of 70.5's information; addfile
 * only reads hFile, the plugin opens and closes it */
typedef bool addfile_t(const char *name, void *bdfe_info, HANDLE hFile);
typedef bool adddir_t(const char *name, void *bdfe_info);
/* items: NumItems pointers to BDFEs; NumItems = -1, items = NULL: all */
typedef void GetFiles_t(HANDLE hPlugin, int NumItems, void *items[], addfile_t *addfile, adddir_t *adddir);
typedef int getattr_t(HANDLE hPlugin, const char *filename, void *info);   /* as 70.5 */
typedef HANDLE plugin_open_t(HANDLE hPlugin, const char *filename, int mode);
/* size a multiple of 512; the bytes read, -1 on an error */
typedef unsigned plugin_read_t(HANDLE hFile, void *buf, unsigned size);
typedef void setpos_t(HANDLE hFile, long long pos);       /* pos a multiple of 512 */
typedef void plugin_close_t(HANDLE hFile);

/* ---- dialogs.inc's structures, as DialogBox and menu take them ---- */

/* flags: 1 the dialog's colours, 2 the warning's colours, 4 no shadow */
typedef struct dlgtemplate {
	unsigned flags;
	int x, y, width, height;        /* x, y = -1: centred */
	int border_size_x, border_size_y;
	const char *title;
	unsigned char main_color, border_color, header_color, pad;  /* set by kfar */
	void *dataptr;                  /* used internally */
} dlgtemplate;

/* int __stdcall DlgProc(DLGTEMPLATE* dlg, int msg, int param1, int param2):
 * msg 1 = draw, 2 = a key (param1 = the scan code) */
typedef int (*DlgProc_t)(void *dlg, int msg, unsigned param1, unsigned param2);

/* item types: 1 static text, 2 button, 3 edit box, 4 horizontal line,
 * 5 check box, 6 list; flags: 0/1/2 aligned left/centre/right, 4 the
 * focus, 8 can have the focus, 0x10 default button / modified edit box /
 * checked check box, 0x20 password edit box */
typedef struct dlgitemtemplate {
	unsigned type;
	int x1, y1, x2, y2;
	void *data;
	unsigned flags;
} dlgitemtemplate;

/* DLGDATA: the template, then num_items items */
typedef struct DLGDATA {
	dlgtemplate dialog;
	DlgProc_t DlgProc;
	void *user_data;
	unsigned num_items;
} DLGDATA;
#define DLG_ITEMS(d) ((dlgitemtemplate *)((DLGDATA *)(d) + 1))

_Static_assert(offsetof(dlgtemplate, title) == 28 && offsetof(dlgtemplate, main_color) == 32 &&
	       sizeof(dlgtemplate) == 40 && sizeof(DLGDATA) == 52 && sizeof(dlgitemtemplate) == 28,
	       "dialogs.inc's dialog and item templates");

/* an edit box's data: struct {unsigned maxlength; unsigned pos;
 * unsigned start; char data[maxlength+1];} */
typedef struct editdata { unsigned maxlength, pos, start; char data[]; } editdata;
/* a list's (and a menu's) items */
typedef struct listitem { struct listitem *next, *prev; char text[]; } listitem;

#endif
