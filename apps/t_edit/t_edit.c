/* KolibriOS for ARM: t_edit, KolibriOS's text editor with syntax
 * colouring, translated to C from programs/other/t_edit/t_edit.asm
 * (IgorA, "with great thanks to all who helped: with code, advice,
 * design", the KolibriOS team; GPL-2.0). The files: t_edit.h.
 *
 * KolibriOS loads box_lib, msgbox, proc_lib, libimg, libini and kmenu
 * as shared libraries; here they are linked in (lib/boxlib, lib/msgbox,
 * lib/proc_lib, lib/libini, lib/kmenu), and the pictures libimg decoded
 * at the start (include_image_file) are decoded at build time
 * (tools/png2bgr.py). As in KolibriOS the message boxes (msgbox) and the
 * key words' window run in threads of their own (51.1, on the stacks
 * thread and thread_coords), the open dialog is the program opendial
 * (/sys/File managers/opendial, talking through shared memory: proc_lib);
 * the menus (kmenu) are drawn in this window. */
#include <string.h>
#include <stdlib.h>
#include "t_edit.h"

u8 *icon_tl_sys;                       /* the tree's icons */
u8 run_file_70[25];                    /* FileInfoBlock */

edit_box edit_replace = EDIT_BOX(TED_PANEL_WIDTH - 1, 0, 20, 0xffffff, 0xff80, 0xff0000, 0xff, 0x4080, 300,
				 buf_replace, &mouse_dd, 0, 0, 0);
edit_box edit_find = EDIT_BOX(TED_PANEL_WIDTH - 1, 0, 20, 0xffffff, 0xff80, 0xff0000, 0xff, 0x4080, 300,
			      buf_find, &mouse_dd, 0, 0, 0);
edit_box edit_goto = EDIT_BOX(TED_PANEL_WIDTH - 1, 0, 20, 0xffffff, 0xff80, 0xff0000, 0xff, 0x4080, 10,
			      buf_goto, &mouse_dd, 0, 0, 0);

u8 *unpac_mem;
char hed[] = "TextEdit ";

char openfile_path[4096];              /* the file worked on */
u8 dir_mem[32 + 304 * count_of_dir_list_files];
u16 wnd_s_pos[4];                      /* the window's place at the start */
char last_open_synt_file[32];          /* the syntax file read last */
char buf[BUF_SIZE];                    /* copy and paste */
char buf_find[302];
char buf_replace[302];
char buf_goto[12];
system_colors sc;
u8 procinfo[1024];
char sys_path[4096];                   /* the program's path */
char file_name[4096];                  /* the parameters at first, then paths */
char syntax_path[4096];                /* the syntax file's name */
char plugin_path[4096];
char filename_area[256];               /* the file's name for the dialog */
/* the stacks: 4 KiB each in t_edit.asm (thread grows into procinfo's
 * spare 1 KiB there), more for C */
u8 thread_stack[THREAD_STACK] __attribute__((aligned(16)));
u8 thread_coords_stack[THREAD_STACK] __attribute__((aligned(16)));

extern char __params[], __path[];      /* lib/crt0.S: the header's */

/* copy_path (load_lib.mac): name in the folder of cur_dir_path */
void copy_path(const char *name, const char *cur_dir_path, char *out)
{
	char tmp[4096];
	strncpy(tmp, cur_dir_path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	char *s = strrchr(tmp, '/');
	if (s) s[1] = 0;
	else tmp[0] = 0;
	strcpy(out, tmp);
	strcat(out, name);
}

static void ini_panel(const char *b_key, int b_id)
{
	panel_but[b_id - ID_BUT_0] = ini_get_int(file_name, ini_sec_window, b_key, 1);
}

/* the editor's (t_edit.asm's) mouse: the menu, the text, the panels */
static void mouse(void)
{
	/* the menu's own (it is drawn over the window here) */
	if (kmainmenu_dispatch_cursorevent(main_menu)) return;
	u32 pos = kos_call(37, 1, 0, 0, 0, 0);
	if (!((s16)tedit0.wnd.top > (s16)pos) && !((s16)tedit0.wnd.left > (s16)(pos >> 16))) {
		if (kos_call(37, 3, 0, 0, 0, 0) & 0x01000000) {   /* a double click */
			ted_but_select_word(&tedit0);
			return;
		}
		ted_mouse(&tedit0);
	}
	/* the panels */
	if (tedit0.panel_id == TED_PANEL_FIND) {
		edit_box_mouse(&edit_find);
		option_box_mouse(opt_grlist1);
		check_box_mouse2(&ch1);
	}
	if (tedit0.panel_id == TED_PANEL_REPLACE) {
		edit_box_mouse(&edit_find);
		edit_box_mouse(&edit_replace);
		option_box_mouse(opt_grlist1);
	}
	if (tedit0.panel_id == TED_PANEL_SYNTAX) tl_mouse(&tree1);
	if (tedit0.panel_id == TED_PANEL_GOTO) edit_box_mouse(&edit_goto);
}

/* KeyConvertToASCII: function 2's scan code (ah) to the layout's
 * character. The original leaves the scan code in al and bits 16-23
 * zero, which the edit boxes take for the cursor keys' codes as
 * characters; the scan code goes to bits 16-23 here, as function 2
 * gives it in ASCII mode, so the edit boxes have their keys */
static u32 KeyConvertToASCII(u32 key, const u8 *table)
{
	u8 scan = key >> 8;
	return (u32)scan << 16 | (u32)table[scan & 0x7f] << 8;
}

/* the panels' edit boxes take the key when in focus */
static int key_to_box(edit_box *e, u32 k)
{
	if (!(e->flags & ed_focus)) return 0;
	u8 ah = k >> 8;
	if (ah > 0x80 || ah == 42 || ah == 54 || ah == 56 || ah == 29 || ah == 69) return 1;
	edit_box_key(e, KeyConvertToASCII(k, conv_tabl));
	return 1;
}

static void key(void)
{
	u32 m = kos_call(66, 3, 0, 0, 0, 0), esi = 0, table = 1;
	if (m & 3) { table = 2; esi |= KM_SHIFT; }
	if (m & 0x0c) esi |= KM_CTRL;
	if (m & 0x30) { table = 3; esi |= KM_ALT; }
	if (m & 0x80) esi |= KM_NUMLOCK;
	kos_call(26, 2, table, (u32)conv_tabl, 0, 0);     /* the keyboard's layout */
	u32 k = kos_get_key();
	if (kmainmenu_dispatch_key(main_menu, k)) return;  /* an open menu's */
	tl_key(&tree1, k);
	if (key_to_box(&edit_replace, k)) return;
	if (key_to_box(&edit_find, k)) return;
	if (key_to_box(&edit_goto, k)) return;
	ted_key(&tedit0, k, conv_tabl, esi);
}

/* the window's place and size from t_edit.ini, within the work area
 * (a small screen: the classic look of 800x480) */
static void fit_window(void)
{
	struct kos_r2 r = kos_call2(48, 5, 0, 0, 0, 0);
	int ww = (r.eax & 0xffff) - (r.eax >> 16) + 1, wh = (r.ebx & 0xffff) - (r.ebx >> 16) + 1;
	if (wnd_s_pos[0] > ww) wnd_s_pos[0] = ww;
	if (wnd_s_pos[2] > wh) wnd_s_pos[2] = wh;
	if (wnd_s_pos[1] + wnd_s_pos[0] > ww) wnd_s_pos[1] = ww - wnd_s_pos[0];
	if (wnd_s_pos[3] + wnd_s_pos[2] > wh) wnd_s_pos[3] = wh - wnd_s_pos[2];
}

static void start(void)
{
	kos_call(48, 3, (u32)&sc, sizeof(sc), 0, 0);
	kos_call(66, 1, 1, 0, 0, 0);           /* scan codes */
	kos_event_mask(0xc0000027);
	strncpy(sys_path, __path, sizeof(sys_path) - 1);
	strncpy(file_name, __params, 256);
	strcpy(openfile_path, file_name);      /* the file name given */

	ted_init(&tedit0);
	tl_data_init(&tree1);
	option_boxes_set_sys_color(&sc, opt_grlist1);
	init_checkboxes2(&ch1, 1);
	check_boxes_set_sys_color2(&ch1, 1, &sc);

	OpenDialog_init(&OpenDialog_data);
	tedit_menu_init();

	/* the toolbar's icons, and gray ones */
	bmp_icon = malloc(TOOLBAR_ICONS_SIZE * 2);
	memcpy(bmp_icon, te_icon, TOOLBAR_ICONS_SIZE);
	img_to_gray(bmp_icon, bmp_icon + TOOLBAR_ICONS_SIZE, TOOLBAR_ICONS_SIZE / 3);
	/* the tree's cursors and lines, the nodes' icons */
	icon_tl_sys = malloc(tl_sys_16_w * tl_sys_16_h * 3);
	memcpy(icon_tl_sys, tl_sys_16, tl_sys_16_w * tl_sys_16_h * 3);
	tree1.data_img_sys = icon_tl_sys;
	icon_tl_sys = malloc(tl_nod_16_w * tl_nod_16_h * 3);
	memcpy(icon_tl_sys, tl_nod_16, tl_nod_16_w * tl_nod_16_h * 3);
	tree1.data_img = icon_tl_sys;

	/* the syntax files' folder */
	copy_path(fn_syntax_dir, sys_path, file_name);
	size_t n = strlen(file_name);
	if (n) file_name[n - 1] = 0;           /* no '/' at its end (CDs need that) */
	u8 tree_file_struct[25] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, count_of_dir_list_files };
	*(u32 *)(tree_file_struct + 16) = (u32)dir_mem;
	char *fp = file_name;
	memcpy(tree_file_struct + 21, &fp, 4);
	struct kos_r2 r = kos_call2(70, (u32)tree_file_struct, 0, 0, 0, 0);
	u32 count = (r.eax == 0 || r.eax == 6) ? r.ebx : 0;
	if (count > count_of_dir_list_files) count = count_of_dir_list_files;
	if (count) {
		for (u32 i = 0; i < count; i++) {
			const char *name = (const char *)dir_mem + 32 + 40 + i * 304;
			if (name[0] == '.') continue;  /* '.' and '..' */
			tl_node_add(&tree1, 0x10000, name);   /* icon 1: the book */
			tl_cur_next(&tree1);
		}
		tl_cur_beg(&tree1);
		tree1.style |= tl_cursor_pos_limited;
	}

	/* t_edit.ini */
	copy_path(ini_name, sys_path, file_name);
	wnd_s_pos[1] = ini_get_int(file_name, ini_sec_window, "l", 200);
	wnd_s_pos[0] = ini_get_int(file_name, ini_sec_window, "w", 570);
	wnd_s_pos[3] = ini_get_int(file_name, ini_sec_window, "t", 20);
	wnd_s_pos[2] = ini_get_int(file_name, ini_sec_window, "h", 470);
	fit_window();
	u32 t = ini_get_int(file_name, ini_sec_window, "scroll_type", 0);
	wScr.type = hScr.type = ws_dir_lbox.type = t;
	extern scrollbar w_scr_t3;
	w_scr_t3.type = t;
	tedit0.rec.width = ini_get_int(file_name, ini_sec_window, "symbol_w", 7);
	tedit0.rec.height = ini_get_int(file_name, ini_sec_window, "symbol_h", 10);
	tedit0.font_size = (u32)ini_get_int(file_name, ini_sec_window, "font_s", 0) << 24;
	ini_panel("but_new", ID_BUT_NEW);
	ini_panel("but_open", ID_BUT_OPEN);
	ini_panel("but_save", ID_BUT_SAVE);
	ini_panel("but_save_as", ID_BUT_SAVE_AS);
	ini_panel("but_select", ID_BUT_SELECT);
	ini_panel("but_cut", ID_BUT_CUT);
	ini_panel("but_copy", ID_BUT_COPY);
	ini_panel("but_paste", ID_BUT_PASTE);
	ini_panel("but_find", ID_BUT_FIND);
	ini_panel("but_replace", ID_BUT_REPLACE);
	ini_panel("but_key_words", ID_BUT_KEY_WORDS);
	ini_panel("but_upper", ID_BUT_UPPER);
	ini_panel("but_lower", ID_BUT_LOWER);
	ini_panel("but_reverse", ID_BUT_REVERSE);
	ini_panel("but_undo", ID_BUT_UNDO);
	ini_panel("but_redo", ID_BUT_REDO);
	ini_panel("but_invisible", ID_BUT_INVISIBLE);
	ini_panel("but_syntax_list", ID_BUT_SYNTAX_LIST);
	ini_panel("but_syntax_mode", ID_BUT_SYNTAX_MODE);
	ini_panel("but_convert_1251_866", ID_BUT_CONVERT_1251_866);
	ini_panel("but_convert_866_1251", ID_BUT_CONVERT_866_1251);
	/* the files' extensions */
	char *e = synt_auto_open;
	for (int k = 0; k < max_synt_auto_open; k++) {
		ini_get_str(file_name, ini_sec_options, key_synt_file, e, 32, "");   /* the file */
		if (!e[0]) break;
		key_synt_file[7]++;
		e += 32;
		ini_get_str(file_name, ini_sec_options, key_synt_ext, e, 32, "");    /* the extension */
		key_synt_ext[4]++;
		e += 32;
	}

	/* the colours' file */
	open_unpac_synt_file(fn_col_option);

	/* the command line */
	if (openfile_path[0] && openfile_path[1] && openfile_path[2] && openfile_path[3]) {
		auto_open_syntax(openfile_path);
		but_no_msg_OpenFile();
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	start();
	draw_window();                         /* red_win */
	for (;;) {                             /* still */
		int ev = kos_wait_event();
		if (exit_code == 1) {          /* a message box's On_Exit, On_SaveAndExit */
			ted_Exit();
			continue;
		}
		if (ev == 1) draw_window();        /* the window changed */
		else if (ev == 2) key();
		else if (ev == 3) button();
		else if (ev == 6) mouse();
	}
}
