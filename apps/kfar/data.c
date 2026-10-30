/* kfar for KolibriOS for ARM: the data of kfar.asm (from keybar_panels
 * to the end: the key bars, the tables, the configuration, the dialogs'
 * templates, the strings, en_US).
 *
 * Translated to C from kfar's kfar.asm, diamond (Evgeny Grechnikov) and
 * the KolibriOS team, GPL-2.0. */
#include "kfar.h"

#define BLANK12 "      " "      " "      " "      " "      " "      " \
		"      " "      " "      " "      " "      " "      "

/* kfar.asm: keybar_panels */
char keybar_panels[8 * 12 * 6] =
	/* no modifiers */
	"Help  " "UserMn" "View  " "Edit  " "Copy  " "RenMov" "MkFold" "Delete" "ConfMn" "Quit  " "Plugin" "Screen"
	/* Shift */
	"Add   " "Extrct" "ArcCmd" "Edit.." "Copy  " "Rename" "      " "Delete" "Save  " "Last  " "Group " "SelUp "
	/* Ctrl */
	"Left  " "Right " "Name  " "Extens" "Modifn" "Size  " "Unsort" "Creatn" "Access" "Descr " "Owner " "Sort  "
	/* Ctrl+Shift */
	"      " "      " "View  " "Edit  " "      " "      " "      " "      " "      " "      " "      " "      "
	/* Alt */
	"Left  " "Right " "View.." "      " "Print " "MkLink" "Find  " "Histry" "Video " "Tree  " "ViewHs" "FoldHs"
	/* Alt+Shift */
	"      " "      " "      " "      " "      " "      " "      " "      " "ConfPl" "      " "      " "      "
	/* Alt+Ctrl, Alt+Ctrl+Shift */
	BLANK12 BLANK12;

/* kfar.asm: keybar_viewer (keybar_cp at F8) */
char keybar_viewer[8 * 12 * 6] =
	"Help  " "Unwrap" "Quit  " "Hex   " "      " "Edit  " "Search" "cp1251" "      " "Quit  " "Plugin" "Screen"
	"      " "WWrap " "      " "      " "      " "      " "Next  " "Table " "      " "      " "      " "      "
	"      " "      " "      " "      " "      " "      " "      " "      " "      " "GoFile" "      " "      "
	BLANK12
	"      " "      " "      " "      " "Print " "      " "Prev  " "Goto  " "Video " "      " "ViewHs" "      "
	"      " "      " "      " "      " "      " "      " "      " "      " "Config" "      " "      " "      "
	BLANK12 BLANK12;

/* kfar.asm: keybar_editor (keybar_cp2 at F8) */
char keybar_editor[8 * 12 * 6] =
	"Help  " "Save  " "      " "      " "      " "View  " "Search" "cp1251" "      " "Quit  " "Plugin" "Screen"
	"      " "SaveAs" "      " "Edit.." "      " "      " "Next  " "Table " "      " "SaveQ " "      " "      "
	"      " "      " "      " "      " "      " "      " "Replac" "      " "      " "GoFile" "      " "      "
	BLANK12
	"      " "      " "      " "      " "Print " "      " "      " "Goto  " "Video " "      " "ViewHs" "      "
	"      " "      " "      " "      " "      " "      " "      " "      " "Config" "      " "      " "      "
	BLANK12 BLANK12;

int cur_width = 80, cur_height = 25;
int saved_width = -1, saved_height = -1;
int fill_width, fill_height;   /* the remainders of the window's size by the font's */
u16 *console_data_ptr;

int cursor_x = -1, cursor_y = -1;
int cursor_size = cursor_normal_size;

int idle_interval = -1;

PanelData panel1, panel2;
PanelData *active_panel = &panel1;

char *associations;
u32 associations_size = 1;              /* terminating zero */
u32 associations_allocated = 0x1000;    /* start with one page */

/* 16 colours; function 65 takes a palette of 256 for 8 bits a pixel */
const u32 console_colors[256] = {
	0x000000, 0x000080, 0x008000, 0x008080,
	0x800000, 0x800080, 0x808000, 0xC0C0C0,
	0x808080, 0x0000FF, 0x00FF00, 0x00FFFF,
	0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
};

const screen_vtable panels_vtable = {
	panels_OnRedraw, 0, 0, panels_OnKey, keybar_panels, panels_getname, panels_OnExit, panels_IsHandleUsed,
};
const screen_vtable viewer_vtable = {
	viewer_OnRedraw, 0, 0, viewer_OnKey, keybar_viewer, viewer_getname, viewer_OnExit, viewer_IsHandleUsed,
};
const screen_vtable editor_vtable = {
	editor_OnRedraw, 0, 0, editor_OnKey, keybar_editor, editor_getname, editor_OnExit, editor_IsHandleUsed,
};
const screen_vtable filesearch_vtable = {
	filesearch_OnRedraw, filesearch_OnActivate, filesearch_OnIdle, filesearch_OnKey, 0,
	filesearch_getname, filesearch_OnExit, filesearch_IsHandleUsed,
};

/* kfar.asm: encodings */
const char encodings_names[4][8] = { "cp866   ", "cp1251  ", "koi8-r  ", "Unicode " };
const u8 encodings_tables[3][128] = {
	/* cp866 - trivial map */
	{
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
		0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
		0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
		0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
	},
	/* cp1251 */
	{
		0x3F, 0x3F, 0x27, 0x3F, 0x22, 0x3A, 0xC5, 0xD8, 0x3F, 0x25, 0x3F, 0x3C, 0x3F, 0x3F, 0x3F, 0x3F,
		0x3F, 0x27, 0x27, 0x22, 0x22, 0x07, 0x2D, 0x2D, 0x3F, 0x54, 0x3F, 0x3E, 0x3F, 0x3F, 0x3F, 0x3F,
		0xFF, 0xF6, 0xF7, 0x3F, 0xFD, 0x3F, 0xB3, 0x15, 0xF0, 0x63, 0xF2, 0x3C, 0xBF, 0x2D, 0x52, 0xF4,
		0xF8, 0x2B, 0x49, 0x69, 0x3F, 0xE7, 0x14, 0xFA, 0xF1, 0xFC, 0xF3, 0x3E, 0x3F, 0x3F, 0x3F, 0xF5,
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
		0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
	},
	/* koi8-r */
	{
		0xC4, 0xB3, 0xDA, 0xBF, 0xC0, 0xD9, 0xC3, 0xB4, 0xC2, 0xC1, 0xC5, 0xDF, 0xDC, 0xDB, 0xDD, 0xDE,
		0xB0, 0xB1, 0xB2, 0xF4, 0xFE, 0xF9, 0xFB, 0xF7, 0xF3, 0xF2, 0xFF, 0xF5, 0xF8, 0xFD, 0xFA, 0xF6,
		0xCD, 0xBA, 0xD5, 0xF1, 0xD6, 0xC9, 0xB8, 0xB7, 0xBB, 0xD4, 0xD3, 0xC8, 0xBE, 0xBD, 0xBC, 0xC6,
		0xC7, 0xCC, 0xB5, 0xF0, 0xB6, 0xB9, 0xD1, 0xD2, 0xCB, 0xCF, 0xD0, 0xCA, 0xD8, 0xD7, 0xCE, 0xFC,
		0xEE, 0xA0, 0xA1, 0xE6, 0xA4, 0xA5, 0xE4, 0xA3, 0xE5, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE,
		0xAF, 0xEF, 0xE0, 0xE1, 0xE2, 0xE3, 0xA6, 0xA2, 0xEC, 0xEB, 0xA7, 0xE8, 0xED, 0xE9, 0xE7, 0xEA,
		0x9E, 0x80, 0x81, 0x96, 0x84, 0x85, 0x94, 0x83, 0x95, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
		0x8F, 0x9F, 0x90, 0x91, 0x92, 0x93, 0x86, 0x82, 0x9C, 0x9B, 0x87, 0x98, 0x9D, 0x99, 0x97, 0x9A,
	},
};
/* encodings.menu: a list for menu(); the original keeps each item's
 * encoding in the byte before it, here encodings_menu_code[] */
menuitem32 encodings_menu[4] = {
	{ (listitem *)&encodings_menu[1], 0, "&DOS text (CP866)" },
	{ (listitem *)&encodings_menu[2], (listitem *)&encodings_menu[0], "&Windows text (CP1251)" },
	{ (listitem *)&encodings_menu[3], (listitem *)&encodings_menu[1], "&Linux text (KOI8-R)" },
	{ 0, (listitem *)&encodings_menu[2], "&Unicode" },
};
const u8 encodings_menu_code[4] = { encodings_cp866, encodings_cp1251, encodings_koi8r, encodings_unicode };

int active_screen;
int viewer_tabsize = 8;
int editor_tabsize = 8;

/* kfar.asm: ascii2scan (cp866: the scan codes of the US and Russian
 * layouts' letters) */
const u8 ascii2scan[256] = {
	[32] =
	0x39, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x28, 0x0A, 0x0B, 0x09, 0x0D, 0x33, 0x0C, 0x34, 0x35,
	0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x27, 0x27, 0x33, 0x0D, 0x34, 0x35,
	0x03, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
	0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x07, 0x0C,
	0x29, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
	0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x29, 0x00,
	0x21, 0x33, 0x20, 0x16, 0x26, 0x14, 0x27, 0x19, 0x30, 0x10, 0x13, 0x25, 0x2F, 0x15, 0x24, 0x22,
	0x23, 0x2E, 0x31, 0x12, 0x1E, 0x1A, 0x11, 0x2D, 0x17, 0x18, 0x1B, 0x1F, 0x32, 0x28, 0x34, 0x2C,
	0x21, 0x33, 0x20, 0x16, 0x26, 0x14, 0x27, 0x19, 0x30, 0x10, 0x13, 0x25, 0x2F, 0x15, 0x24, 0x22,
	[0xE0] =
	0x23, 0x2E, 0x31, 0x12, 0x1E, 0x1A, 0x11, 0x2D, 0x17, 0x18, 0x1B, 0x1F, 0x32, 0x28, 0x34, 0x2C,
	0x29, 0x29,
};
/* kfar.asm: scan2ascii */
const u8 scan2ascii[0x40] =
	"\0\0" "1234567890-=" "\0\0"
	"qwertyuiop[]" "\0\0" "as"
	"dfghjkl;'`" "\0" "\\zxcv"
	"bnm,./" "\0\0\0" " " "\0\0\0\0\0";

/* kfar.asm: mouse_panels, mouse_viewer, mouse_editor: the key bar's
 * F1-F12 for no modifier, Shift, Ctrl, Ctrl+Shift, Alt, Alt+Shift,
 * Alt+Ctrl, Alt+Ctrl+Shift */
const key_handler mouse_panels[96] = {
	0, 0, panels_OnKey_f3, panels_OnKey_f4, panels_OnKey_f5, panels_OnKey_f6, panels_OnKey_f7, panels_OnKey_f8,
	0, exit_handler, 0, F12,
	/* Shift (F6, Shift+F6: an addition, panels.c) */
	[12 + 4] = panels_OnKey_shift_f5, panels_OnKey_shift_f6,
	/* Ctrl */
	[24 + 2] = panels_OnKey_ctrl_f3_9, panels_OnKey_ctrl_f3_9, panels_OnKey_ctrl_f3_9,
	panels_OnKey_ctrl_f3_9, panels_OnKey_ctrl_f3_9, panels_OnKey_ctrl_f3_9, panels_OnKey_ctrl_f3_9,
	/* Alt */
	[48] = panels_OnKey_alt_f1_2, panels_OnKey_alt_f1_2,
	[48 + 6] = panels_OnKey_alt_f7,
};
const key_handler mouse_viewer[96] = {
	0, viewer_OnKey_f2, viewer_OnKey_exit, viewer_OnKey_f4, 0, 0, viewer_OnKey_f7, viewer_OnKey_f8,
	0, viewer_OnKey_exit, 0, F12,
	[12 + 6] = viewer_OnKey_shift_f7,
};
const key_handler mouse_editor[96] = {
	0, editor_OnKey_f2, 0, 0, 0, 0, editor_OnKey_f7, 0, 0, editor_OnKey_exit_confirm, 0, F12,
	[12 + 6] = editor_OnKey_shift_f7,
};

/* kfar.asm: the function 70 blocks */
fileinfo dirinfo = { 1, 0, 0, 300, 0, 0, 0 };
fileinfo readinfo = { 0, 0, 0, 0, 0, 0, 0 };
fileinfo writeinfo = { 2, 0, 0, 0, 0, 0, 0 };
u8 attrinfo_attr[40];
fileinfo attrinfo = { 5, 0, 0, 0, attrinfo_attr, 0, 0 };
fileinfo delinfo = { 8, 0, 0, 0, 0, 0, execdata };
fileinfo mkdirinfo = { 9, 0, 0, 0, 0, 0, CopyDestEditBuf.data };
fileinfo execinfo = { 7, 0, 0, 0, 0, 0, 0 };

const char compare_names[] = "nNxXmMsSuUcCaA";

/* the configuration data (built in, as in the original) */
/* the panels */
u8 panel_normal_color = 0x1B;
u8 panel_selected_color = 0x1E;
u8 panel_border_color = 0x1B;
u8 panel_cursor_color = 0x30;
u8 panel_selected_cursor_color = 0x3E;
u8 panel_header_color = 0x1B;
u8 panel_active_header_color = 0x30;
u8 column_header_color = 0x1E;
u8 panel_number_color = 0x1B;
u8 panel_numselected_color = 0x3E;
u8 panel_nscreens_color = 0x0B;
/* the dialogs */
struct dlgcolors dialog_colors = { 0x70, 0x70, 0x70, 0x30, 0x38, 0x70, 0x30, 0x70, 0x0F, 0x70 };
/* the warnings and errors */
struct dlgcolors warning_colors = { 0x4F, 0x4F, 0x4F, 0x30, 0x38, 0x4F, 0x70, 0x3F, 0x70, 0x3F };
/* the menus */
u8 menu_normal_color = 0x3F;
u8 menu_selected_color = 0x0F;
u8 menu_highlight_color = 0x3E;
u8 menu_selected_highlight_color = 0x0E;
u8 menu_border_color = 0x3F;
u8 menu_header_color = 0x3F;
u8 menu_scrollbar_color = 0x3F;
/* the key bar */
u8 keybar_number_color = 7;
u8 keybar_name_color = 0x30;
u8 keybar_bgr_color = 7;
/* the command line */
u8 cmdbar_normal_color = 7;
u8 cmdbar_prefix_color = 7;
/* the viewer */
u8 view_normal_color = 0x1B;
u8 view_selected_color = 0x30;
u8 view_status_color = 0x30;
u8 view_arrows_color = 0x1E;
/* the editor */
u8 edit_normal_color = 0x1B;
u8 edit_status_color = 0x30;

/* the highlighting of files: all masks in lower case */
static const highlight highlight_group0 = { 2, 0x13, 0x38, 0, 0, "*" };
static const highlight highlight_group1 = { 4, 0x13, 0x38, 0, 0, "*" };
static const highlight highlight_group2 = { 0x10, 0x1F, 0x3F, 0, 0, "*|.." };
static const highlight highlight_group3 = { 0x10, 0, 0, 0, 0, ".." };
static const highlight highlight_group4 = { 0, 0x1A, 0x3A, 0, 0, "*.exe,*.com,*.bat,*.cmd,*.kex" };
static const highlight highlight_group5 = { 0, 0x1A, 0x3A, 0, 0, "*|*.*,readme,makefile" };
static const highlight highlight_group6 = { 0, 0x1D, 0x3D, 0, 0,
	"*.rar,*.zip,*.[zj],*.[bg7]z,*.[bg]zip,*.tar,*.t[ag]z,*.ar[cj],*.r[0-9][0-9],"
	"*.a[0-9][0-9],*.bz2,*.cab,*.msi,*.jar,*.lha,*.lzh,*.ha,*.ac[bei],*.pa[ck],"
	"*.rk,*.cpio,*.rpm,*.zoo,*.hqx,*.sit,*.ice,*.uc2,*.ain,*.imp,*.777,*.ufa,*.boa,"
	"*.bs[2a],*.sea,*.hpk,*.ddi,*.x2,*.rkv,*.[lw]sz,*.h[ay]p,*.lim,*.sqz,*.chz" };
static const highlight highlight_group7 = { 0, 0x16, 0x36, 0, 0, "*.bak,*.tmp" };
static const highlight highlight_group8 = { 0, 0x17, 0x37, 0, 0, "*.asm,*.inc" };
static const highlight highlight_group9 = { 0x10, 0x1F, 0x3F, 0, 0, "*" };
int highlight_num_groups = 10;
const highlight *const highlight_groups[10] = {
	&highlight_group0, &highlight_group1, &highlight_group2, &highlight_group3, &highlight_group4,
	&highlight_group5, &highlight_group6, &highlight_group7, &highlight_group8, &highlight_group9,
};

u8 bConfirmDelete = 1;
u8 bConfirmDeleteIncomplete = 0;

char *FolderShortcuts[10];

/* the end of the configuration data */

u8 bWasE0;
u8 ctrlstate;
u8 mousestate;
u8 *MemForImage;
char *restore_semicolon;
u8 bForHex;

/* the message on a failed allocation */
struct nomem_dlgdata nomem_dlgdata = {
	{ { 2, -1, -1, 12, 2, 1, 1, aError, 0, 0, 0, 0, 0 }, 0, 0, 2 },
	/* the string "No memory" */
	DlgLbl(1, 0, 10, 0, aNoMemory, 1),
	/* the button "Ok" */
	DlgBtn(4, 1, 7, 1, aOk, 0xD),
};

/* the copy dialog (the widths set by panels_OnKey_f5) */
struct copy_dlgdata copy_dlgdata = {
	{ { 1, -1, -1, 0, 4, 4, 2, aCopyCaption, 0, 0, 0, 0, 0 }, 0, 0, 4 },
	/* the string 'Copy "%s" to:' */
	DlgLbl(1, 0, 0, 0, CopySourceTextBuf, 0),
	/* the destination's name */
	DlgEdit(1, 1, 0, 1, &CopyDestEditBuf, 0),
	/* the button "copy" */
	DlgBtn(0, 3, 0, 3, aCopy, 0x18),
	/* the button "cancel" */
	DlgBtn(0, 3, 0, 3, aCancelB, 8),
};

MkDir_Dlgdata mkdir_dlgdata = {
	{ { 1, -1, -1, 0, 4, 4, 2, aMkDirCaption, 0, 0, 0, 0, 0 }, 0, 0, 4 },
	/* the string "Create the folder" */
	DlgLbl(1, 0, aMkDirLen, 0, aMkDir, 0),
	/* the new folder's name */
	DlgEdit(1, 1, 0, 1, &CopyDestEditBuf, 0xC),
	/* the button "Continue" */
	DlgBtn(0, 3, 0, 3, a_Continue, 0x18),
	/* the button "cancel" */
	DlgBtn(0, 3, 0, 3, aCancelB, 8),
};

/* select/deselect (grey + and -) */
struct mark_dlgdata mark_dlgdata = {
	{ { 1, -1, -1, 37, 1, 4, 2, 0, 0, 0, 0, 0, 0 }, 0, 0, 1 },
	DlgEdit(1, 0, 35, 0, &enter_string_buf, 0),
};

/* the quick search in the panel (Alt+letters) */
struct QuickSearchDlg QuickSearchDlg = {
	{ { 5, 0, 0, 20, 1, 1, 1, aSearch, 0, 0, 0, 0, 0 }, 0, 0, 1 },
	DlgEdit(1, 0, 18, 0, &quick_search_buf, 0x1C),
};

/* the search in a file, for the viewer and the editor */
FindInFileDlgdata find_in_file_dlgdata = {
	{ { 1, -1, -1, 0, 7, 4, 2, aSearch, 0, 0, 0, 0, 0 }, 0, 0, 8 },
	/* the string "Search for" */
	DlgLbl(1, 0, aSearchForLen, 0, aSearchFor, 0),
	/* the text to search for */
	DlgEdit(1, 1, 0, 1, &SearchStringEditBuf, 0xC),
	DlgLine(-1, 2, -1, 2),
	/* "Case sensitive": by default, the search is case sensitive */
	DlgCheck(1, 3, -1, 3, aCaseSensitive, 0x18),
	/* "Whole words": by default, not whole words only */
	DlgCheck(1, 4, -1, 4, aWholeWords, 8),
	DlgLine(-1, 5, -1, 5),
	/* the button "Search" */
	DlgBtn(0, 6, 0, 6, aSearchB, 0x18),
	/* the button "cancel" */
	DlgBtn(0, 6, 0, 6, aCancelB, 8),
};

/* the first dialog of the file search (the query) */
FileSearch_QueryTemplate filesearch_query_template = {
	{ { 1, 0, 0, 0, 9, 4, 2, aFileSearch, 0, 0, 0, 0, 0 }, 0, 0, 10 },
	/* the prompt for the mask */
	DlgLbl(1, 0, aFileMasksLen, 0, aFileMasks, 0),
	/* the mask */
	DlgEdit(1, 1, 0, 1, 0, 0xC),
	/* the prompt for the text */
	DlgLbl(1, 2, aContainingTextLen, 2, aContainingText, 0),
	/* the text */
	DlgEdit(1, 3, 0, 3, 0, 8),
	DlgLine(-1, 4, -1, 4),
	/* "Case sensitive", "Whole words": from find_in_file_dlgdata */
	DlgCheck(1, 5, -1, 5, aCaseSensitive, 0),
	DlgCheck(1, 6, -1, 6, aWholeWords, 0),
	DlgLine(-1, 7, -1, 7),
	/* the button "Search" */
	DlgBtn(0, 8, 0, 8, aSearchB, 0x18),
	/* the button "cancel" */
	DlgBtn(0, 8, 0, 8, aCancelB, 8),
};

/* the second dialog of the file search (the scan) */
FileSearch_SearchTemplate filesearch_search_template = {
	{ { 1, 0, 0, 0, 0, 4, 2, 0, 0, 0, 0, 0, 0 }, 0, 0, 9 },
	/* the files found */
	DlgList(0, 0, 0, 0, 0, 0),
	DlgLine(-1, 0, -1, 0),
	/* "Searching <string> in:" or "Search done..." */
	DlgLbl(1, 0, 0, 0, 0, 0),
	/* the current folder */
	DlgLbl(0, 0, 0, 0, 0, 0),
	DlgLine(-1, 0, -1, 0),
	/* the buttons */
	DlgBtn(0, 0, 0, 0, aNewSearch, 8),
	DlgBtn(0, 0, 0, 0, aGoto, 0x1C),
	DlgBtn(0, 0, 0, 0, aView, 8),
	DlgBtn(0, 0, 0, 0, aCancelB2, 8),
};

const char *RetryOrCancelBtn[2] = { aRetry, a_Cancel };
const char *DeleteOrKeepBtn[2] = { a_Delete, aKeep };
const char *RetryOrIgnoreBtn[2] = { aRetry, aIgnore };
const char *DeleteOrCancelBtn[2] = { a_Delete, a_Cancel };
/* DeleteErrorBtn: Retry, then SkipOrCancelBtn: Skip, Skip all, Cancel */
const char *DeleteErrorBtn[4] = { aRetry, aSkip, aSkipAll, a_Cancel };
const char *ContinueBtn[1] = { aContinue };
const char *EditorExitBtn[3] = { aSave, aDontSave, aContinueEdit };
const char *YesOrNoBtn[2] = { aYes, aNo };

const char *aCannotOpenFile_ptr[1] = { aCannotOpenFile };
const char *aCannotCreateThread_ptr[1] = { aCannotCreateThread };
const char *ConfirmCancelMsg[1] = { aConfirmCancel };
const char *EditConfigErr_ptr[2] = { aEditConfigErr1, aEditConfigErr2 };

/* the strings, en_US */
const char aDeleteCaption[] = "Delete";
const char aConfirmDeleteText[] = "Do you wish to delete";
const char aDeleteFolder[] = " the folder";
const char aDeleteFile[] = " the file";
const char aDelete[] = " Delete ";
const char aCancel[] = " Cancel ";
const char aCancelB[] = "[ Cancel ]";
const char aCopyCaption[] = "Copy";
const char aCopy[] = "[ Copy ]";
const char a_Continue[] = "[ Continue ]";
const char aCopy1[] = "Copy \"";
const char aCopy2[] = "\" to:";
const char aError[] = "Error";
const char aContinue[] = "Continue";
const char aRetry[] = "Retry";
const char a_Cancel[] = "Cancel";
const char a_Delete[] = "Delete";
const char aSkip[] = "Skip";
const char aSkipAll[] = "Skip all";
const char aYes[] = "Yes";
const char aNo[] = "No";
const char error0msg[] = "Strange... No error";
const char error1msg[] = "Strange... Hard disk base and/or partition not defined";
const char error2msg[] = "The file system does not support this function";
const char error3msg[] = "Unknown file system";
const char error4msg[] = "Strange... Error 4";
const char error5msg[] = "File not found";
const char error6msg[] = "End of file";
const char error7msg[] = "Strange... Pointer is outside of application memory";
const char error8msg[] = "Disk is full";
const char error9msg[] = "File structure is destroyed";
const char error10msg[] = "Access denied";
const char error11msg[] = "Device error";
const char error30msg[] = "Not enough memory";
const char error31msg[] = "File is not executable";
const char error32msg[] = "Too many processes";
const char aUnknownError[] = "Unknown error code: ";
const char aCannotReadFolder[] = "Cannot read folder";
const char aRunError[] = "Cannot execute program:";
const char aFileNameTooBig[] = "Full file name is too long";
const char aFolderNameTooBig[] = "Full folder name is too long";
const char aCmdLineTooBig[] = "Command line is too long (OS limit is 256 symbols)";
const char aCannotCopyToSelf[] = "File cannot be copied onto itself";
const char aCannotReadFile[] = "Cannot read file";
const char aIncompleteFile[] = "Incomplete file was retrieved. Delete it?";
const char aKeep[] = "Keep";
const char aCannotWriteFile[] = "Cannot write to the file";
const char aCannotDeleteFile[] = "Cannot delete the file";
const char aCannotDeleteFolder[] = "Cannot delete the folder";
const char aNotFolder[] = "is not a folder";
const char aIgnore[] = "Ignore";
const char aMkDirCaption[] = "Make folder";
const char aMkDir[] = "Create the folder";
const char aCannotMakeFolder[] = "Cannot create folder";
/* the column headers: the length, then the text */
const char aName[] = "\4Name";
const char aSize[] = "\4Size";
const char aDate[] = "\4Date";
const char aTime[] = "\4Time";
const char aCannotLoadDLL[] = "Cannot load DLL";
const char aCannotLoadPlugin[] = "Cannot load plugin";
const char aInvalidDLL[] = "File is not found or invalid";
const char aMissingExport[] = "Required function is not present";
const char aInitFailed[] = "Initialization failed";
const char aIncompatibleVersion[] = "Incompatible version";
const char aTables[] = "Tables";
const char aSelect[] = "Select";
const char aDeselect[] = "Deselect";
const char aCannotOpenFile[] = "Cannot open the file";
const char aCannotCreateThread[] = "Cannot create a thread";
const char aCannotSetFolder[] = "Cannot enter to the folder";
const char aSearch[] = "Search";
const char aSearchB[] = "[ Search ]";
const char aSearchFor[] = "Search for";
const char aCaseSensitive[] = "Case sensitive";
const char aWholeWords[] = "Whole words";
const char aReverseSearch[] = "Reverse search";
const char aStringNotFound[] = "Could not find the string";
const char aFileSearch[] = "Find file";
const char aFileMasks[] = "A file mask or several file masks:";
const char aContainingText[] = "Containing text:";
const char aSearchingIn[] = "Searching \"\" in:";
const char aSearchDone[] = "Search done. Found ? file(s)";
const char aNewSearch[] = "[ New search ]";
const char aGoto[] = "[ Go to ]";
const char aView[] = "[ View ]";
const char aEditConfigErr1[] = "Error in configuration of plugins for the editor.";
const char aEditConfigErr2[] = "Try to remove unnecessary plugins.";
const char aEditNoMemory[] = "The file is too big for the editor.";
const char aLine[] = "    Line";             /* aLine_size, no zero needed */
const char aCol[] = "   Col ";
const char aEditorTitle[] = "Editor";
const char aFileModified[] = "File has been modified";
const char aSave[] = "Save";
const char aDontSave[] = "Do not save";
const char aContinueEdit[] = "Continue editing";
const char aCannotSaveToPlugin[] = "Saving is not supported for plugin panels";
const char aCannotSearchOnPlugin[] = "The search on plugin panels is not supported yet";
const char aCancelled[] = "Operation has been interrupted";
const char aConfirmCancel[] = "Do you really want to cancel it?";

const char aOk[] = "OK";
const char aNoMemory[] = "No memory!";
const char nullstr[] = "";
const char aUntitled[] = "untitled";
const char aDotDot[] = "..\0";

/* the error messages (tools.c: get_error_msg) */
const char *const errors1[12] = {
	error0msg, error1msg, error2msg, error3msg, error4msg, error5msg,
	error6msg, error7msg, error8msg, error9msg, error10msg, error11msg,
};
const char *const errors2[3] = { error30msg, error31msg, error32msg };

/* plugins: none (see kfar.h) */
PluginInfo *plugins;
int num_plugins;
int EditPlugInfo;
u8 EditEOLStyle = edit_eol_unix;

/* kfar.asm: colmodes, the panels' view modes */
const PanelMode colmodes[5] = {
	/* mode 0: NM,SC,D */
	{ 3, { COLUMN_TYPE_NAME + COLUMN_NAME_MARK, COLUMN_TYPE_SIZE + COLUMN_SIZE_COMMA, COLUMN_TYPE_DATE },
	  { 0, 10, 8 }, 0, 1, { 0 } },
	/* mode 1: N,N,N */
	{ 3, { COLUMN_TYPE_NAME, COLUMN_TYPE_NAME, COLUMN_TYPE_NAME }, { 0 }, 0, 1, { 0 } },
	/* mode 2: N,N */
	{ 2, { COLUMN_TYPE_NAME, COLUMN_TYPE_NAME }, { 0 }, 0, 0, { 0 } },
	/* mode 3: N,S,D,T */
	{ 4, { COLUMN_TYPE_NAME, COLUMN_TYPE_SIZE, COLUMN_TYPE_DATE, COLUMN_TYPE_TIME }, { 0, 6, 8, 5 }, 0, 1, { 0 } },
	/* mode 4: N,S */
	{ 2, { COLUMN_TYPE_NAME, COLUMN_TYPE_SIZE }, { 0, 6 }, 0, 0, { 0 } },
	/* mode 5: N,S,P,DM,DC,DA,A; 6: N,Z; 7: N,S,Z; 8: N,S,O; 9: N,S,LN
	 * (not in the original either) */
};

char execdata[1024];
PanelData panel1, panel2;

char cur_header[max_width + 1];
u32 tmp;

int skinh;
int wnd_width, wnd_height;

u64 scrpos, viewer_right_side;

int EditDataSize, EditBlockStart, EditBlockSize;

u8 procinfo[1024];             /* saved_file_name, lower_file_name */
char app_path[1100];
char error_msg[128];
char prev_dir[1024];
u8 driveinfo[32 + 304];
char tmpname[32];

screen *screens;
int num_screens;
const screen_vtable *active_screen_vtable;
void *active_screen_data;
char *active_screen_keybar;

u8 *default_attr;
u8 left_dotdot_entry[40 + 4];  /* 40 bytes for the attributes + '..' */
u8 right_dotdot_entry[40 + 4];

char aConfirmDeleteTextBuf[aConfirmDeleteTextMax + 1];
char CopySourceTextBuf[512];
editbuf512 CopyDestEditBuf;
searchbuf_t SearchStringEditBuf;
editbuf512 enter_string_buf;
quicksearchbuf_t quick_search_buf;

u8 bMemForImageValidData;

u8 identical_table[256], tolower_table[256], isspace_table[256], composite_table[256];
u8 layout[128];

u8 copy_buffer[copy_buffer_size];
u8 filedata_buffer[filedata_buffer_size];

void *source_hModule, *source_hPlugin, *source_hFile;
/* the data for deleting folders: a folder nested deeper than 1024 has
 * too long a name, which the code checks */
u32 del_dir_stack[1024];
u32 *del_dir_stack_ptr;
u8 del_dir_query_area[32 + 304 * del_dir_query_size];
u8 del_bSkipAll;               /* for folder errors (copy_bSkipAll) */
u8 copy_bSkipAll2;             /* for file read/write errors */
u8 copy_bSkipAll3;             /* for SetFolder errors */

u8 bEndSlash, bDestIsFolder, bNeedRestoreName;

const char header[] = "Kolibri Far " version;
const char nomem_draw[] = "No memory for redraw.";

/* the folders at the start: the original's right panel is /hd0/1, which
 * KolibriOS for ARM has not; its RAM disk instead */
const char def_left_dir[] = "/sys";
const char def_right_dir[] = "/tmp0/1";

u8 bSilentFolderMode = 1;
u8 bQuickSearchMode = 0;
u8 bDisplayQuickSearch = 0;

const char aFolder[] = "Folder";               /* aFolder_size */
const char aUp[] = "Up";
const char aDrive[] = "Drive";
const char aScreens[] = "Screens";
