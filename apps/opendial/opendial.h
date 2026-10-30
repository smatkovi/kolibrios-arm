/* KolibriOS for ARM: opendial, KolibriOS's open and save dialog
 * (Marat Zakiyanov aka Mario79 and the KolibriOS team; GPL-2.0),
 * translated to C from programs/fs/opendial/opendial.asm.
 *
 * The files: opendial.c (opendial.asm: the start, the communication
 * area, the folders, the window, the keys, the mouse, the answer) and
 * fb.c (the three box_lib components opendial draws with, written for it
 * after box_lib's filebrowser.asm, menubar.asm and pathshow.asm: our
 * box_lib has the edit box and the scroll bar, which it uses as they
 * are). The structures keep the fields opendial reads and writes, by
 * their names there. */
#ifndef OPENDIAL_H
#define OPENDIAL_H

#include <kos.h>
#include <box_lib.h>
#include <proc_lib.h>

typedef short s16;

/* filebrowser.asm's file_browser_data, the fields opendial uses */
struct file_browser {
	u16 size_x, start_x, size_y, start_y;
	u16 icon_size_y, icon_size_x, line_size_y;
	const u8 *icon_raw_area;          /* the icons, 24 bits a pixel, one under the other */
	int select_flag;                  /* the mouse pressed in the list */
	u32 background_color, select_color, seclect_text_color, text_color, reduct_text_color;
	u32 max_panel_line;               /* the lines that fit */
	int select_panel_counter;         /* the cursor shown (the list in focus) */
	u32 folder_block;                 /* the entries */
	u32 start_draw_line;              /* the first one shown */
	u16 start_draw_cursor_line;       /* the cursor's line, in pixels from the top */
	u8 *folder_data;                  /* function 70.1's: a header of 32 bytes, 304 an entry */
	const char *ini_file_start, *ini_file_end;   /* "ext=ID" lines: the icons by extension */
	int draw_scroll_bar;              /* a key moved the list: its scroll bar to draw */
	u16 font_size_y, font_size_x;
	u32 mouse_keys, mouse_keys_old, mouse_keys_delta, mouse_key_delay, mouse_keys_tick;
	int all_redraw;                   /* 1 all, 2 the lines but not the cursor's bar */
	u8 *selected_BDVK_adress;         /* the entry under the cursor */
	u16 key_action, key_action_num;   /* FileBrowser_key's: 1 down ... 7 Enter */
};

/* menubar.asm's menu_bar, the fields opendial uses */
struct menu_bar {
	u32 type;                         /* 1: the bar itself is the first item (opendial's disks) */
	u16 size_x, start_x, size_y, start_y;
	const char *text_pointer;         /* the bar's text */
	const char *pos_pointer;          /* the items, each ended by 0 */
	const char *text_end;             /* behind the last */
	u32 ret_key;                      /* 1 Left, 2 Right: to the menu beside */
	u16 size_x1, start_x1, size_y1, start_y1;   /* the menu below the bar */
	u32 bckg_col, frnt_col, menu_col;
	u32 select;                       /* the bar under the mouse */
	u32 click;                        /* an item chosen */
	u32 cursor;                       /* the item under the cursor (from 0) */
	u32 interval;                     /* an item's height */
	u32 menu_sel_col, bckg_text_col, frnt_text_col;
	u32 font_height;
	u32 cursor_out;                   /* the item chosen (from 1), 0 none */
	u32 get_mouse_flag;
	u32 mouse_keys, mouse_keys_old;
};

/* pathshow.asm's PathShow_data */
struct path_show {
	u16 start_y, start_x, font_size_x, area_size_x;
	u32 font_color;
	const char *text_pointer;
	char *work_area_pointer;
};

/* fb.c */
void FileBrowser_draw(struct file_browser *fb);
void FileBrowser_key(struct file_browser *fb);
void FileBrowser_mouse(struct file_browser *fb);
void menu_bar_draw(struct menu_bar *m);
void menu_bar_mouse(struct menu_bar *m);
void menu_bar_activate(struct menu_bar *m);
void PathShow_prepare(struct path_show *p);
void PathShow_draw(struct path_show *p);

#endif
