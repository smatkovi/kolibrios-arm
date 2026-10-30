/* KolibriOS for ARM: t_edit's data, translated to C from
 * programs/other/t_edit/t_data.inc (IgorA, the KolibriOS team; GPL-2.0):
 * the settings' names and defaults, the scroll bars, the open dialog's
 * structure, the editor's (tedit0), the syntax list (tree1), the find
 * panel's options and check box, the messages (English, as t_edit's
 * en_US build) */
#include <string.h>
#include "t_edit.h"

const char *fn_col_option = "default.syn";
const char fn_syntax_dir[] = "info/";
const char def_col_file[] = "default.syn";

u32 exit_code = 0;                     /* the program ends */
u8 panel_but[21];                      /* the toolbar's buttons shown */

/* the settings' names (t_edit.ini) */
const char ini_name[] = "t_edit.ini";
const char ini_sec_window[] = "Window";
const char ini_sec_options[] = "Options";
char key_synt_file[] = "syntax_0";     /* .numb: its digit */
char key_synt_ext[] = "ext_0";

/* the syntax files by extension: pairs of 32-byte names */
char synt_auto_open[64 * max_synt_auto_open + 1];

scrollbar wScr = SCROLLBAR(16, 50, 300, 50, 15, 100, 30, 0, 0, 0, 0, 1);
scrollbar hScr = SCROLLBAR(150, 0, 16, 50, 15, 100, 30, 0, 0, 0, 0, 1);
/* the ListBox's scroll bar */
scrollbar ws_dir_lbox = SCROLLBAR(16, 0, 30, 70, 15, 100, 30, 0, 0, 0, 0, 1);

const char default_dir[] = "/sys";     /* the folder by default */
static char communication_area_name[] = "FFFFFFFF_open_dialog";
const char open_dialog_name[] = "opendial";
const char communication_area_default_path[] = "/sys/File managers/";

#define FILTER_EXT "ASM\0CPP\0INC\0MAC\0INI\0TXT\0H\0C\0HTM\0HTML\0PY\0SH\0BAT\0LUA\0INF\0LOG\0DBG\0DAT\0"
static struct { u32 size; char ext[sizeof(FILTER_EXT)]; } Filter = { sizeof(FILTER_EXT) - 1, FILTER_EXT };

open_dialog OpenDialog_data = {
	.type = 0,
	.procinfo = procinfo,
	.com_area_name = communication_area_name,
	.com_area = 0,
	.opendir_path = plugin_path,
	.dir_default_path = (char *)default_dir,
	.start_path = file_name,           /* the open dialog's path */
	.draw_window = draw_window,
	.status = 0,
	.openfile_path = openfile_path,    /* the file opened */
	.filename_area = filename_area,    /* its name */
	.filter_area = &Filter,
	.x_size = 420,
	.x_start = 10,
	.y_size = 320,
	.y_start = 10,
};

const char txt_Info[] = "\" -tI";

/* the editor */
text_editor tedit0 = {
	.wnd = { 0, 50, 440, 150 },
	.rec = { 30, 25, 7, 10 },              /* ini_def_symbol_w, ini_def_symbol_h */
	.max_chars = maxChars,
	.colors_text_count = 1,
	.color_cursor = 0x808080,
	.color_wnd_capt = 0x80,
	.color_wnd_work = 0x0,
	.color_wnd_bord = 0xd0d0d0,
	.color_select = 0x208080,
	.color_cur_text = 0xff0000,
	.color_wnd_text = 0xffff00,
	.syntax_file_size = maxSyntaxFileSize,
	.help_id = -1,
	.el_focus = &el_focus,
	.symbol_new_line = 20,
	.scr_w = &wScr,
	.scr_h = &hScr,
	.buffer = buf,
	.buffer_find = buf_find,
	.cur_ins = 1,
	.mode_color = 1,
	.fun_on_key_ctrl_all = ted_but_ctrl_all,
	.buffer_size = BUF_SIZE,
	.fun_find_err = ted_on_find_err,
	.fun_draw_panel_buttons = draw_but_toolbar,
	.fun_draw_panels = draw_panels,
	.fun_save_err = ted_save_err_msg,
	.increase_size = 200,
	.font_size = 0,
};

u8 conv_tabl[128];                     /* scan codes to ASCII */

void *el_focus = &tedit0;
u32 mouse_dd = 0;
tree_list tree1 = TREE_LIST(264, count_of_dir_list_files + 2, tl_key_no_edit | tl_draw_par_line | tl_list_box_mode,
			    16, 16, 0x8080ff, 0x0000ff, 0xffffff, 0, 70, TED_PANEL_WIDTH - 17, 120, 0, 0, 0,
			    &el_focus, &ws_dir_lbox, 0);

option_box opt1 = OPTION_BOX(&opt_gr1, 5, 90, 6, 12, 0xd0d0ff, 0xff, 0x80ff, "Down", 4, 0);
option_box opt2 = OPTION_BOX(&opt_gr1, 5, 108, 6, 12, 0xd0d0ff, 0xff, 0x80ff, "Up", 2, 0);
option_box opt3 = OPTION_BOX(&opt_gr1, 5, 126, 6, 12, 0xd0d0ff, 0xff, 0x80ff, "At first", 8, 0);
option_box *opt_gr1 = &opt1;
option_box *opt_grlist1[] = { &opt1, &opt2, &opt3, 0 };    /* the group's end */

const char ch_text_1[] = "Case sensitive";
check_box2 ch1 = CHECK_BOX2(5 << 16 | 12, 144 << 16 | 12, 5, 0xffffff, 0, 0x80ff, ch_text_1, ch_flag_en | ch_flag_middle);

/* the messages: msgbox's (the focused button, 0, the caption, the text,
 * the buttons) and @notify's ("text" -tI) */
#define MB1 "\"Replace\r\nMade replacements: "
char msgbox_1[sizeof(MB1) + 16] = MB1;
char *msgbox_1_z = msgbox_1 + sizeof(MB1) - 1;
char msgbox_2[] = "\3\0Warning\0Save changes to the file?\0Save\0Discard\0Cancel\0";
#define MB3 "Error opening the file with the color settings!\rBecause color selection of words is not possible.\r  (error code ="
char msgbox_3[] = "\1\0Warning\0" MB3 "?)\0Close\0";
char *msgbox_3_err = msgbox_3 + 10 + sizeof(MB3) - 1;
#define MB4 "Error opening file!\rMaybe the file name is not entered correctly.\r  (error code ="
char msgbox_4[] = "\1\0Warning\0" MB4 "?)\0Close\0";
char *msgbox_4_err = msgbox_4 + 10 + sizeof(MB4) - 1;
#define MB6 "Error saving file!\rMaybe the file name is not entered correctly.\r  (error code ="
char msgbox_6[] = "\1\0Warning\0" MB6 "?)\0Close\0";
char *msgbox_6_err = msgbox_6 + 10 + sizeof(MB6) - 1;
char msgbox_7[] = "\"Text not found.\r\nTry change the query and search again.\" -tI";
/* .fdp: the dialog's path, 100 characters */
#define MB9 "Error run 'open file dialog'.\r"
char msgbox_9[] = "\3\0Warning\0" MB9 "                                                  "
		  "                                                  \0Close\0";
char *msgbox_9_fdp = msgbox_9 + 10 + sizeof(MB9) - 1;

const char txtFindCapt[] = "Search:";
const char txtFindNext[] = "Find next";
const char txtRerlaceCapt[] = "Replace with:";
const char txtRerlace[] = "Replace";
const char txtRerlaceAll[] = "Replace all";
const char txtFormatCapt[] = "Format";
const char txtFormatApply[] = "Apply";
const char txtGotoCapt[] = "Go to line:";
const char txtGoto[] = "Go to";

/* the messages' buttons' functions */
mb_function msgbox_2N_funct[] = { On_SaveAndNewFile, On_NewFile, 0 };
mb_function msgbox_2O_funct[] = { On_SaveAndOpenFile, ted_but_open_file_no_msg, 0 };
mb_function msgbox_2E_funct[] = { On_SaveAndExit, On_Exit, 0 };
