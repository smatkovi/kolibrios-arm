/* kfar (Kolibri Far) for KolibriOS for ARM: the shared declarations.
 *
 * Translated to C from kfar's kfar.asm, dialogs.inc, tools.inc,
 * viewer.inc, editor.inc, filetool.inc, search.inc and sort.inc
 * (programs/fs/kfar), by diamond (Evgeny Grechnikov) and the KolibriOS
 * team, GPL-2.0.
 *
 * The C keeps the original's structure and names: a procedure label is a
 * C function of the same name (a local label .x of a procedure p is
 * p_x), the globals and structures have the original's names, and each
 * function says where it comes from. Registers that carry the state
 * between the original's procedures are kept where they make the
 * correspondence easier to follow: "ebp" is the panel or the screen's
 * data, "al"/"eax" the key's scan code, as in the original.
 *
 * Text is in CP866, as the original's; the strings are the English
 * (en_US) ones of the original. kfar's plugins (kfar_arc, api.txt) are
 * not supported: there are no shared libraries (function 68.19) in
 * KolibriOS for ARM; the panels keep their hPlugin fields and checks
 * (always 0), the calls into plugins are left out. */
#ifndef KFAR_H
#define KFAR_H

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include "kos.h"
#include "font.h"              /* generated: font_width, font_height, font[] */

typedef unsigned long long u64;
typedef long long s64;
typedef short s16;

#define version "0.65"
#define version_dword (0 * 0x10000 + 65)

#define min_width 54
#define max_width 255
#define min_height 8
#define max_height 255

#define cursor_normal_size ((font_height * 15 + 50) / 100)
#define cursor_big_size font_height

/* ---- system functions (KOSfuncs.inc names where kfar uses them) ---- */

/* function 70's information block (packed, as KolibriOS has it) */
typedef struct __attribute__((packed)) fileinfo {
	u32 code;
	u32 first;          /* the offset (low dword) or the first entry */
	u32 first_hi;       /* the offset's high dword, or flags (folders) */
	u32 size;
	void *data;
	u8 zero;
	const char *name;
} fileinfo;

/* function 70: eax (the error), ebx (the bytes or entries) */
static inline u32 fs_call(fileinfo *fi, u32 *ebx)
{
	struct kos_r2 r = kos_call2(70, (u32)fi, 0, 0, 0, 0);
	if (ebx) *ebx = r.ebx;
	return r.eax;
}

/* KolibriOS's BDFE (a folder entry, 304 bytes with the name in cp866):
 * kfar keeps its own data in it, as the original: bytes 4-7 the colours
 * of the highlighting (normal, cursor, selected, selected under the
 * cursor), byte 303 the selection */
#define BDFE_SIZE 304
#define bdfe_attr(e) (*(u32 *)(e))
#define bdfe_name(e) ((char *)(e) + 40)
#define bdfe_sel(e) (((u8 *)(e))[303])
static inline u64 bdfe_size(const u8 *e) { return *(const u32 *)(e + 32) | (u64)*(const u32 *)(e + 36) << 32; }
static inline bool bdfe_is_dotdot(const u8 *e) { return e[40] == '.' && e[41] == '.' && e[42] == 0; }

/* ---- the panels ---- */

typedef struct PanelData {
	int left, top, width, height;
	int index, start, colsz, colst;
	u8 sortmode;
	u8 pad1[3];
	int nfa;                /* entries allocated in files */
	int numfiles;
	/* nfa pointers to the entries, then the folder's 32-byte header and
	 * nfa entries of 304 bytes (read_folder) */
	u8 **files;
	void *hPlugin;
	void *hFile;
	u8 *parents;
	u32 parents_sz, parents_alloc;
	u32 colmode;
	u32 colwidths[16 + 1];
	u32 total_num;
	u64 total_size;
	u32 selected_num;
	u64 selected_size;
	u32 plugin_flags;       /* plugin_info */
	char dir[1024];
} PanelData;

#define PANEL_OF_DIR(p) ((PanelData *)((char *)(p) - offsetof(PanelData, dir)))

extern PanelData panel1, panel2;
extern PanelData *active_panel;
/* the other panel (the original's "xor ebp, panel1 xor panel2") */
#define other_panel(p) ((p) == &panel1 ? &panel2 : &panel1)

/* PanelMode: the columns of a view mode */
enum {
	COLUMN_TYPE_NONE = 0, COLUMN_TYPE_NAME = 1, COLUMN_NAME_MARK = 0x10,
	COLUMN_NAME_NOPATH = 0x20, COLUMN_NAME_RIGHTALIGN = 0x40,
	COLUMN_TYPE_SIZE = 2, COLUMN_SIZE_COMMA = 0x10, COLUMN_TYPE_PACKED_SIZE = 3,
	COLUMN_TYPE_DATE = 4, COLUMN_TYPE_TIME = 5, COLUMN_TYPE_DATETIME = 6,
	COLUMN_TYPE_DATETIME_CREATION = 7, COLUMN_TYPE_DATETIME_ACCESS = 8,
	COLUMN_TYPE_ATTRIBUTES = 9, COLUMN_TYPE_DESCRIPTION = 10, COLUMN_TYPE_OWNER = 11,
	COLUMN_TYPE_NUMLINKS = 12, COLUMN_TYPE_CUSTOM = 13,
};
typedef struct PanelMode {
	u32 NumColumns;
	u8 ColumnTypes[16];
	u8 ColumnWidths[16];
	u8 bFullScreen;
	u8 bAlignExtensions;
	u8 pad[2];
} PanelMode;
extern const PanelMode colmodes[5];

/* the highlighting of files (kfar.asm: highlight) */
typedef struct highlight {
	u8 IncludeAttributes;
	u8 NormalColor, CursorColor, SelectedColor, SelectedCursorColor;
	const char *Mask;       /* in lower case */
} highlight;
extern int highlight_num_groups;
extern const highlight *const highlight_groups[];

/* plugins: kfar.asm's PluginInfo (not loaded here, see above) */
typedef struct PluginInfo {
	void *unload, *getattr, *open, *read, *setpos, *close, *OpenFilePlugin, *ClosePlugin,
		*ReadFolder, *SetFolder, *GetOpenPluginInfo, *GetPanelTitle, *GetFiles;
	u32 EditInfoSize, EditInfoOffs;
} PluginInfo;
extern PluginInfo *plugins;
extern int num_plugins;
extern int EditPlugInfo;

/* ---- the screens: panels, viewers, editors, the file search ---- */

typedef struct screen_vtable {
	void (*OnRedraw)(void *ebp);
	void (*OnActivate)(void *ebp);
	void (*OnIdle)(void *ebp);
	void (*OnKey)(void *ebp, u32 al);       /* al: the scan code (bit 7: released) */
	char *keybar;                           /* 8 x 12 x 6 characters, or NULL */
	/* writes the screen's name for the F12 menu: up to ecx characters at edi */
	void (*getname)(void *ebp, char *edi, int ecx);
	void (*OnExit)(void *ebp);
	/* true if the screen uses the plugin's handle (ZF in the original) */
	bool (*IsHandleUsed)(void *ebp, void *hPlugin, void *hFile);
} screen_vtable;

typedef struct screen { const screen_vtable *vtable; void *data; } screen;

extern const screen_vtable panels_vtable, viewer_vtable, editor_vtable, filesearch_vtable;
extern screen *screens;
extern int num_screens, active_screen;
extern const screen_vtable *active_screen_vtable;
extern void *active_screen_data;
extern char *active_screen_keybar;
extern int idle_interval;

/* kfar.c: in: ecx = the size of the screen's data, edx = its vtable;
 * the new screen is the active one; returns its data (zeroed) or NULL */
void *new_screen(u32 ecx, const screen_vtable *edx);
void next_screen(void);
void delete_active_screen(void);
void change_screen(void);

/* ---- keys ---- */

/* the keys a screen handles (kfar.asm: panels_ctrlkeys ...): the scan
 * code, the modifiers (4 bits each for Shift, Ctrl, Alt: 0 none, 1
 * exactly one, 2 both, 3 the left one, 4 the right one), the handler */
typedef void (*key_handler)(void *ebp, u32 eax);
typedef struct ctrlkey {
	u8 scancode;
	u8 reserved;
	u16 ctrlstate;
	key_handler handler;
} ctrlkey;

/* kfar.c: true (CF clear) if the key was in the table and handled */
bool process_ctrl_keys(const ctrlkey *esi, u32 al, void *ebp);

extern u8 ctrlstate;           /* 1 LShift, 2 RShift, 4 LCtrl, 8 RCtrl, 0x10 LAlt, 0x20 RAlt */
extern u8 bWasE0;
extern u8 mousestate;
extern const u8 ascii2scan[256];
extern const u8 scan2ascii[0x40];
extern u8 layout[128];

/* the handlers shared by all screens */
void F12(void *ebp, u32 eax);
void alt_f9(void *ebp, u32 eax);
void exit_handler(void *ebp, u32 eax);  /* kfar.asm's exit, as a key */
void dummy_key(void *ebp, u32 eax);
void kfar_exit(void) __attribute__((noreturn));    /* kfar.asm: exit */

extern const ctrlkey panels_ctrlkeys[], viewer_ctrlkeys[], editor_ctrlkeys[];
/* the key bar's functions under the mouse (F1-F12 x 8 modifier states) */
extern const key_handler mouse_panels[96], mouse_viewer[96], mouse_editor[96];

/* the key bars: writable, the viewer and the editor show the encoding
 * at F8 (keybar_cp, keybar_cp2) */
extern char keybar_panels[8 * 12 * 6], keybar_viewer[8 * 12 * 6], keybar_editor[8 * 12 * 6];
#define keybar_cp (keybar_viewer + 7 * 6)
#define keybar_cp2 (keybar_editor + 7 * 6)

/* ---- the console: cur_width x cur_height cells (char | attr << 8) ---- */

extern int cur_width, cur_height, saved_width, saved_height, fill_width, fill_height;
extern int wnd_width, wnd_height, skinh;
/* the cells, then (cur_width * cur_height later) the cells last drawn */
extern u16 *console_data_ptr;
extern int cursor_x, cursor_y, cursor_size;
extern const u32 console_colors[256];

static inline u16 *get_console_ptr(int eax, int edx)
{
	return console_data_ptr + edx * cur_width + eax;
}
#define CELL(c, a) ((u16)((u8)(c) | (u16)(u8)(a) << 8))

void draw_window(void);
/* the window's frame width (5 classic, 0 in Hildon's look; kfar.c) */
int kfar_wnd_border(void);
void draw_image(void);
/* draw_image does nothing while set (the original patches its first
 * byte with ret: filesearch_OnRedraw) */
extern u8 bDrawImageDisabled;
void draw_keybar(void);
void draw_cmdbar(void);
void init_console(void);
/* kfar.c: function 26.2 (the keyboard layout), the scan code in ASCII */
u8 get_ascii_char(u32 eax);

/* ---- colours (kfar.asm: the configuration data) ---- */

extern u8 panel_normal_color, panel_selected_color, panel_border_color, panel_cursor_color,
	panel_selected_cursor_color, panel_header_color, panel_active_header_color,
	column_header_color, panel_number_color, panel_numselected_color, panel_nscreens_color;
struct dlgcolors {
	u8 main_color, border_color, header_color, edit_color, unmodified_edit_color,
		normal_btn_color, selected_btn_color, list_color, selected_list_color, scroll_list_color;
};
extern struct dlgcolors dialog_colors, warning_colors;
#define dialog_main_color dialog_colors.main_color
#define dialog_border_color dialog_colors.border_color
#define dialog_header_color dialog_colors.header_color
#define dialog_edit_color dialog_colors.edit_color
#define dialog_unmodified_edit_color dialog_colors.unmodified_edit_color
#define dialog_normal_btn_color dialog_colors.normal_btn_color
#define dialog_selected_btn_color dialog_colors.selected_btn_color
#define dialog_list_color dialog_colors.list_color
#define dialog_selected_list_color dialog_colors.selected_list_color
#define dialog_scroll_list_color dialog_colors.scroll_list_color
#define warning_main_color warning_colors.main_color
#define warning_border_color warning_colors.border_color
#define warning_header_color warning_colors.header_color
extern u8 menu_normal_color, menu_selected_color, menu_highlight_color, menu_selected_highlight_color,
	menu_border_color, menu_header_color, menu_scrollbar_color;
extern u8 keybar_number_color, keybar_name_color, keybar_bgr_color;
extern u8 cmdbar_normal_color, cmdbar_prefix_color;
extern u8 view_normal_color, view_selected_color, view_status_color, view_arrows_color;
extern u8 edit_normal_color, edit_status_color;

/* ---- dialogs (dialogs.inc) ---- */

/* flags: 1 the dialog's colours, 2 the warning's colours, 4 no shadow */
typedef struct dlgtemplate {
	u32 flags;
	int x, y, width, height;
	int border_size_x, border_size_y;
	const char *title;
	u8 main_color, border_color, header_color, pad;
	void *dataptr;          /* used internally */
} dlgtemplate;

/* int __stdcall DlgProc(DLGTEMPLATE* dlg, int msg, int param1, int param2):
 * msg 1 = draw, 2 = a key (param1 = the scan code) */
typedef int (*DlgProc_t)(void *dlg, int msg, u32 param1, u32 param2);

/* item types: 1 static text, 2 button, 3 edit box, 4 horizontal line,
 * 5 check box, 6 list; flags: 0/1/2 aligned left/centre/right, 4 the
 * focus, 8 can have the focus, 0x10 default button / modified edit box /
 * checked check box, 0x20 password edit box */
typedef struct dlgitemtemplate {
	u32 type;
	int x1, y1, x2, y2;
	void *data;
	u32 flags;
} dlgitemtemplate;

/* DLGDATA: the template, then num_items items */
typedef struct DLGDATA {
	dlgtemplate dialog;
	DlgProc_t DlgProc;
	void *user_data;
	u32 num_items;
} DLGDATA;
#define DLG_ITEMS(d) ((dlgitemtemplate *)((DLGDATA *)(d) + 1))

#define quicksearch_maxlen 64
/* an edit box's data: struct {unsigned maxlength; unsigned pos;
 * unsigned start; char data[maxlength+1];} */
typedef struct editdata { u32 maxlength, pos, start; char data[]; } editdata;
#define EDITBUF(n) struct { u32 maxlength, pos, start; char data[(n) + 1]; }
typedef EDITBUF(512) editbuf512;
typedef EDITBUF(253 + 1) searchbuf_t;
typedef EDITBUF(quicksearch_maxlen) quicksearchbuf_t;   /* 12 + 64 bytes, and the original's byte beyond */
/* a list's items and data */
typedef struct listitem { struct listitem *next, *prev; char text[]; } listitem;
typedef struct listdata { listitem *curitemptr; u32 numitems; listitem *head; u32 curitem; } listdata;

int GenericBox(void *dlg, DlgProc_t DlgProc);
int ShowGenericBox(void *dlg, DlgProc_t DlgProc);
void DrawGenericBox(void *dlg, DlgProc_t DlgProc);
void HideGenericBox(void *dlg, int bRedrawWindow);
void HideDialogBox(void *dlg, int bRedrawWindow);
void draw_dialog_shadow(dlgtemplate *ebx);
/* returns the item that ended it (a button), -1 (Esc) */
int DialogBox(void *dlg);
int ShowDialogBox(void *dlg);
void DrawDialogBox(void *dlg);
int ManagerDlgProc(void *dlg, int msg, u32 param1, u32 param2);
void ManagerDlgProc_dodraw(DLGDATA *ebx);
void draw_listbox(DLGDATA *ebp, dlgitemtemplate *ebx);
void listbox_key(dlgitemtemplate *ebx, u32 al);
/* int __stdcall menu(void* variants, const char* title, unsigned flags):
 * returns the listitem chosen or -1; flags & 1: a wide border */
int menu(listitem *variants, const char *title, u32 flags);
int menu_centered_in(int left, int top, int width, int height, listitem *variants,
		     const char *title, u32 flags);
/* returns the index of the button pressed, -1 for Esc */
int SayErr(int num_strings, const char **strings, int num_buttons, const char **buttons);
int SayErrTitle(const char *title, int num_strings, const char **strings, int num_buttons,
		const char **buttons);
int Message(const char *title, int num_strings, const char **strings, int num_buttons,
	    const char **buttons);
void SayNoMem(void);
/* 0: the user is sure, nonzero: the user wants to go on */
int ConfirmCancel(void);

#define DLGITEM(type, x1, y1, x2, y2, data, flags) { type, x1, y1, x2, y2, (void *)(data), flags }
#define DlgLbl(x1, y1, x2, y2, text, flags) DLGITEM(1, x1, y1, x2, y2, text, flags)
#define DlgBtn(x1, y1, x2, y2, text, flags) DLGITEM(2, x1, y1, x2, y2, text, flags)
#define DlgEdit(x1, y1, x2, y2, data, flags) DLGITEM(3, x1, y1, x2, y2, data, flags)
#define DlgLine(x1, y1, x2, y2) DLGITEM(4, x1, y1, x2, y2, 0, 0)
#define DlgCheck(x1, y1, x2, y2, text, flags) DLGITEM(5, x1, y1, x2, y2, text, flags)
#define DlgList(x1, y1, x2, y2, data, flags) DLGITEM(6, x1, y1, x2, y2, data, flags)

/* the dialogs of kfar.asm's data */
extern struct nomem_dlgdata { DLGDATA h; dlgitemtemplate lbl, btn; } nomem_dlgdata;
extern struct copy_dlgdata { DLGDATA h; dlgitemtemplate lcopy, ename, copy, cnl; } copy_dlgdata;
typedef struct MkDir_Dlgdata { DLGDATA h; dlgitemtemplate lbl, dnam, cont, cnl; } MkDir_Dlgdata;
extern MkDir_Dlgdata mkdir_dlgdata;
extern struct mark_dlgdata { DLGDATA h; dlgitemtemplate edit; } mark_dlgdata;
extern struct QuickSearchDlg { DLGDATA h; dlgitemtemplate edit; } QuickSearchDlg;
typedef struct FindInFileDlgdata {
	DLGDATA h;
	dlgitemtemplate lbl, efind, line1, case_, whole, line2, search, cnl;
} FindInFileDlgdata;
extern FindInFileDlgdata find_in_file_dlgdata;
typedef struct FileSearch_QueryTemplate {
	DLGDATA h;
	dlgitemtemplate lbl1, edit1, lbl2, edit2, line1, case_, whole, line2, search, cnl;
} FileSearch_QueryTemplate;
extern FileSearch_QueryTemplate filesearch_query_template;
typedef struct FileSearch_SearchTemplate {
	DLGDATA h;
	dlgitemtemplate list1, lin1, lbl1, lbl2, lin2, btn1, btn2, btn3, btn4;
} FileSearch_SearchTemplate;
extern FileSearch_SearchTemplate filesearch_search_template;

/* the buttons of messages */
extern const char *RetryOrCancelBtn[2], *DeleteOrKeepBtn[2], *RetryOrIgnoreBtn[2],
	*DeleteOrCancelBtn[2], *DeleteErrorBtn[4], *ContinueBtn[1], *EditorExitBtn[3], *YesOrNoBtn[2];
#define SkipOrCancelBtn (DeleteErrorBtn + 1)
extern const char *aCannotOpenFile_ptr[1], *aCannotCreateThread_ptr[1], *ConfirmCancelMsg[1],
	*EditConfigErr_ptr[2];

/* ---- strings (kfar.asm, en_US) ---- */

extern const char aDeleteCaption[], aConfirmDeleteText[], aDeleteFolder[], aDeleteFile[],
	aDelete[], aCancel[], aCancelB[], aCopyCaption[], aCopy[], a_Continue[], aCopy1[], aCopy2[],
	aError[], aContinue[], aRetry[], a_Cancel[], a_Delete[], aSkip[], aSkipAll[], aYes[], aNo[],
	aUnknownError[], aCannotReadFolder[], aRunError[], aFileNameTooBig[], aFolderNameTooBig[],
	aCmdLineTooBig[], aCannotCopyToSelf[], aCannotReadFile[], aIncompleteFile[], aKeep[],
	aCannotWriteFile[], aCannotDeleteFile[], aCannotDeleteFolder[], aNotFolder[], aIgnore[],
	aMkDirCaption[], aMkDir[], aCannotMakeFolder[], aName[], aSize[], aDate[], aTime[],
	aCannotLoadDLL[], aCannotLoadPlugin[], aInvalidDLL[], aMissingExport[], aInitFailed[],
	aIncompatibleVersion[], aTables[], aSelect[], aDeselect[], aCannotOpenFile[],
	aCannotCreateThread[], aCannotSetFolder[], aSearch[], aSearchB[], aSearchFor[],
	aCaseSensitive[], aWholeWords[], aReverseSearch[], aStringNotFound[], aFileSearch[],
	aFileMasks[], aContainingText[], aSearchingIn[], aSearchDone[], aNewSearch[], aGoto[],
	aView[], aEditConfigErr1[], aEditConfigErr2[], aEditNoMemory[], aLine[], aCol[],
	aEditorTitle[], aFileModified[], aSave[], aDontSave[], aContinueEdit[],
	aCannotSaveToPlugin[], aCannotSearchOnPlugin[], aCancelled[], aConfirmCancel[], aOk[],
	aNoMemory[], nullstr[], aUntitled[], aDotDot[], header[], nomem_draw[],
	aFolder[], aUp[], aDrive[], aScreens[];
#define aCancelB2 aCancelB
/* lengths (kfar.asm's "= $ - label - 1") */
#define aDeleteLength 8
#define aCancelLength 8
#define aCancelBLength 10
#define aCancelB2Length aCancelBLength
#define aCopyLength 8
#define a_ContinueLength 12
#define aMkDirLen 17
#define aSearchBLength 10
#define aSearchForLen 6         /* as the original's "$ - aSearch - 1" */
#define aFileMasksLen 34
#define aContainingTextLen 16
#define aSearchingInLen 16
#define aSearchDoneLen 28
#define aNewSearchLen 14
#define aGotoLen 9
#define aViewLen 8
#define aConfirmDeleteTextMax (21 + 11)
#define aFolder_size 6
#define aUp_size 2
#define aLine_size 8
#define aCol_size 7

/* ---- the other globals of kfar.asm ---- */

extern char app_path[1100];     /* argv[0]; kfar.ini beside it */
extern char execdata[1024];
#define execdataend (execdata + 1024)
extern char prev_dir[1024];
extern u8 procinfo[1024];
#define saved_file_name ((char *)procinfo)
#define lower_file_name ((char *)procinfo + 512)
extern char error_msg[128];
extern char cur_header[max_width + 1];
extern u8 driveinfo[32 + 304];
extern char tmpname[32];
extern u32 tmp;

extern fileinfo dirinfo, readinfo, writeinfo, attrinfo, delinfo, mkdirinfo, execinfo;
extern u8 attrinfo_attr[40];
#define execparams execinfo.first_hi
#define execptr execinfo.name

extern u8 identical_table[256], tolower_table[256], isspace_table[256], composite_table[256];

extern char *associations;
extern u32 associations_size, associations_allocated;
extern char *FolderShortcuts[10];
extern u8 bConfirmDelete, bConfirmDeleteIncomplete;
extern u8 bSilentFolderMode, bQuickSearchMode, bDisplayQuickSearch;
extern u8 *MemForImage;
extern u8 bMemForImageValidData;
extern char *restore_semicolon;
extern u8 bForHex;
extern u8 EditEOLStyle;
extern int EditDataSize, EditBlockStart, EditBlockSize;
extern int viewer_tabsize, editor_tabsize;
extern u64 scrpos, viewer_right_side;
extern const char def_left_dir[], def_right_dir[];

/* encodings (viewer, editor) */
enum { encodings_cp866 = 0, encodings_cp1251 = 1, encodings_koi8r = 2, encodings_unicode = 3 };
extern const char encodings_names[4][8];
extern const u8 encodings_tables[3][128];
typedef struct menuitem32 { listitem *next, *prev; char text[32]; } menuitem32;
extern menuitem32 encodings_menu[4];
extern const u8 encodings_menu_code[4];      /* the byte before each item */

/* the edit boxes' buffers */
extern editbuf512 CopyDestEditBuf;
#define CopyDestEditBuf_length 512
extern char CopySourceTextBuf[512];
extern searchbuf_t SearchStringEditBuf;   /* one more byte for "string" */
#define SearchStringEditBuf_length 253
#define SearchString (SearchStringEditBuf.data)
extern editbuf512 enter_string_buf;
extern quicksearchbuf_t quick_search_buf;
extern char aConfirmDeleteTextBuf[aConfirmDeleteTextMax + 1];

/* the copy and the delete (filetool.c) */
#define copy_buffer_size 65536
extern u8 copy_buffer[copy_buffer_size];
#define filedata_buffer_size 1024
extern u8 filedata_buffer[filedata_buffer_size];
extern void *source_hModule, *source_hPlugin, *source_hFile;
extern u8 *default_attr;
extern u8 left_dotdot_entry[40 + 4], right_dotdot_entry[40 + 4];
extern u8 bEndSlash, bDestIsFolder, bNeedRestoreName;
extern u8 del_bSkipAll, copy_bSkipAll2, copy_bSkipAll3;
#define copy_bSkipAll del_bSkipAll
#define del_dir_query_size 32
extern u8 del_dir_query_area[32 + 304 * del_dir_query_size];
extern u32 del_dir_stack[1024];
extern u32 *del_dir_stack_ptr;
#define copy_dir_stack del_dir_stack
#define copy_dir_stack_ptr del_dir_stack_ptr
#define copy_dir_query_size del_dir_query_size
#define copy_dir_query_area del_dir_query_area

/* ---- procedures of kfar.asm (kfar.c, panels.c) ---- */

void redraw(void);
/* kfar.asm: get_event: function 10, or 23 with the screen's OnIdle */
int get_event(void);
void read_folder(PanelData *ebp);
void sort_files(PanelData *ebp);
void calc_colwidths(PanelData *ebp);
void draw_panel(PanelData *ebp);
void close_plugin_panel(PanelData *ebp);
void close_plugin_panels(PanelData *ebp);
void close_handle_if_unused(void *hPlugin, void *hFile);
u8 *get_curfile_folder_entry(PanelData *ebp);
char *get_curfile_name(PanelData *ebp);
/* flags as 'cmp al,[edi]' after the case folding: <0, 0, >0 */
int match_symbol(u8 al, u8 edi);
/* the names compared without case: the sign of the first difference */
int strcmpi(const char *esi, const char *edi);
/* esi -> name (lower case), edx -> its end, edi -> mask; true <=> match */
bool match_mask_rev_lowercase(const char *esi, const char *edx, const char *edi);
/* the association (the value in [Associations]) of the extension of the
 * entry ecx, or NULL (CF in the original) */
char *find_extension(const u8 *ecx);
/* kfar.asm: draw_border: a double frame of ebx x edx cells at edi */
void draw_border(u16 *edi, u8 ah, int ebx, int edx);
void panels_OnKey_run_association2(PanelData *ebp, const u8 *ecx, char *edi);
void panels_OnKey_ctrl_r(void *ebp, u32 eax);
/* kfar.asm: panels_OnKey.ctrl_r.doread: the folder again, the cursor on
 * saved_file_name (or at the index); the file search's Go to */
void panels_OnKey_ctrl_r_doread(PanelData *ebp, int index);
void panels_OnKey_done_redraw(PanelData *ebp);
void panels_OnKey_reread(PanelData *ebp);
void panels_OnKey_enter(void *ebp, u32 eax);

void panels_OnRedraw(void *ebp);
void panels_OnKey(void *ebp, u32 al);
void panels_getname(void *ebp, char *edi, int ecx);
void panels_OnExit(void *ebp);
bool panels_IsHandleUsed(void *ebp, void *hPlugin, void *hFile);
void panels_OnKey_f3(void *ebp, u32 eax);
void panels_OnKey_f4(void *ebp, u32 eax);
void panels_OnKey_f5(void *ebp, u32 eax);
void panels_OnKey_shift_f5(void *ebp, u32 eax);
void panels_OnKey_f7(void *ebp, u32 eax);
void panels_OnKey_f8(void *ebp, u32 eax);
void panels_OnKey_ctrl_f3_9(void *ebp, u32 eax);
void panels_OnKey_alt_f1_2(void *ebp, u32 eax);

/* tools.c (tools.inc): the memory (malloc here: function 68.13 keeps
 * the memory, 68.20 does not exist), the error messages */
void *pgalloc(u32 ecx);
void pgfree(void *ecx);
void *pgrealloc(void *edx, u32 ecx);
void *xpgalloc(u32 ecx);
void *xpgrealloc(void *edx, u32 ecx);
u32 getfreemem(void);
const char *get_error_msg(u32 eax);

/* sort.c: sort.inc's heap sort of ecx dwords at edx, ebx compares
 * (>0: esi above edi) */
typedef int (*compare_fn)(const u8 *esi, const u8 *edi);
void sort(u8 **edx, u32 ecx, compare_fn ebx);
extern const compare_fn compare_fns[14];
extern const char compare_names[];

/* search.c (search.inc): the finite automaton for a pattern: ebx the
 * pattern, esi the translation table, the encoding, whole words only;
 * returns the automaton (to be pgfree'd) or NULL, *ecx its final state */
u8 *search_string_pre(const char *ebx, const u8 *esi, int encoding, int whole, u32 *ecx);

/* viewer.c (viewer.inc): eax = 0: from the panel, esi = its dir
 * (PANEL_OF_DIR(esi) is the panel), ecx its BDFE; eax != 0: esi = the
 * full name */
void view_file(int eax, const char *esi, const u8 *ecx);
void viewer_OnRedraw(void *ebp);
void viewer_OnKey(void *ebp, u32 al);
void viewer_getname(void *ebp, char *edi, int ecx);
void viewer_OnExit(void *ebp);
bool viewer_IsHandleUsed(void *ebp, void *hPlugin, void *hFile);
void viewer_OnKey_f2(void *ebp, u32 eax);
void viewer_OnKey_exit(void *ebp, u32 eax);
void viewer_OnKey_f4(void *ebp, u32 eax);
void viewer_OnKey_f7(void *ebp, u32 eax);
void viewer_OnKey_f8(void *ebp, u32 eax);
void viewer_OnKey_shift_f7(void *ebp, u32 eax);

/* viewer_data and editor_data begin with hPlugin, hFile and have
 * filename[1024], hostname[1024] at offset 0x200 (editor.inc checks
 * this); viewer.c's helpers below work on both */
#define VIEWEDIT_HPLUGIN(ebp) (*(void **)(ebp))
#define VIEWEDIT_FILENAME(ebp) ((char *)(ebp) + 0x200)
#define VIEWEDIT_HOSTNAME(ebp) ((char *)(ebp) + 0x600)
/* viewer.inc: viewedit_draw_filename: the file's name at edi (cells of
 * the colour ah), at most *ecx cells (*ecx decreased); returns edi */
u16 *viewedit_draw_filename(void *ebp, u16 *edi, int *ecx, u8 ah);
/* viewer.inc: viewedit_getname: getname after the 13 characters of the
 * prefix ("View         "): edi after them, ecx the getname's ecx */
void viewedit_getname(void *ebp, char *edi, int ecx);
/* viewer.inc: search_failed: frees edx (search_string_pre's), says so */
void search_failed(u8 *edx);
/* editor.inc: find_in_file_dlg: true if the user chose [ Search ] */
bool find_in_file_dlg(void);

/* editor.c (editor.inc): the panel's file at the index ecx */
void edit_file(PanelData *ebp, int ecx);
void editor_OnRedraw(void *ebp);
void editor_OnKey(void *ebp, u32 al);
void editor_getname(void *ebp, char *edi, int ecx);
void editor_OnExit(void *ebp);
bool editor_IsHandleUsed(void *ebp, void *hPlugin, void *hFile);
void editor_OnKey_f2(void *ebp, u32 eax);
void editor_OnKey_f7(void *ebp, u32 eax);
void editor_OnKey_exit_confirm(void *ebp, u32 eax);
void editor_OnKey_shift_f7(void *ebp, u32 eax);
/* kfar.asm start's EditDataSize: the pages of an editor's data for
 * max_height lines (editor_data, editor_line are editor.c's) */
void editor_calc_datasize(void);
/* editor.inc's edit.eol_* (EditEOLStyle) */
enum { edit_eol_dos = 1, edit_eol_unix = 2, edit_eol_mac = 3 };

/* filetool.c (filetool.inc): the delete, the copy, the folders; true:
 * the job is cancelled (the original's "ja cancel_label") */
bool delete_file(PanelData *ebp, u8 *eax);
bool copy_file(PanelData *ebp, u8 *eax);
/* the folder CopyDestEditBuf.data with the buttons for an error;
 * 0 (ZF): made */
int makedir(int num_buttons, const char **buttons);
void delete_last_name_from_end(char *esi);
void delete_last_name(char *esi);

/* filetool.c: the file layer (_FILE, open, read, ...) */
enum { O_READ = 1, O_WRITE = 2, O_CREATE = 4, O_TRUNCATE = 8, O_SEQUENTIAL_ONLY = 0x10 };
typedef struct _FILE _FILE;
_FILE *open(const char *name, int mode);
_FILE *open2(void *plugin_id, void *plugin_instance, const char *name, int mode);
u32 read(_FILE *hFile, void *buf, u32 size);
void seek(_FILE *hFile, int method, s64 newpos);
s64 tell(_FILE *hFile);
s64 filesize(_FILE *hFile);
void close(_FILE *hFile);

/* filesearch.c (filetool.inc): Alt+F7, the file search screen */
void panels_OnKey_alt_f7(void *ebp, u32 eax);
void filesearch_OnRedraw(void *ebp);
void filesearch_OnActivate(void *ebp);
void filesearch_OnIdle(void *ebp);
void filesearch_OnKey(void *ebp, u32 al);
void filesearch_getname(void *ebp, char *edi, int ecx);
void filesearch_OnExit(void *ebp);
bool filesearch_IsHandleUsed(void *ebp, void *hPlugin, void *hFile);

/* settings (libini, linked in) */
#include "libini.h"

#endif
