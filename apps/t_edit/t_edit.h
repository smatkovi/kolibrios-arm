/* KolibriOS for ARM: t_edit, KolibriOS's text editor with syntax
 * colouring, translated to C from programs/other/t_edit (t_edit.asm and
 * its .inc files) by IgorA and the KolibriOS team, GPL-2.0.
 *
 * The C files follow the assembler's: t_edit.c (t_edit.asm: the start,
 * the event loop, the mouse and the keys), t_data.c (t_data.inc: the
 * data, the editor's and the panels' structures), t_button.c
 * (t_button.inc: the buttons' functions, files, panels), t_menu.c
 * (t_menu.inc: the menu bar), t_draw.c (t_draw.inc: the window, the
 * toolbar, the panels), wnd_k_words.c (wnd_k_words.inc: the window of
 * the key words' search), strlen.c (strlen.inc: string functions). The
 * labels are the functions' names, the variables keep theirs. */
#ifndef T_EDIT_H
#define T_EDIT_H

#include <kos.h>
#include <box_lib.h>
#include <libini.h>
#include <msgbox.h>
#include <proc_lib.h>
#include <kmenu.h>

typedef short s16;
typedef signed char s8;

#define MAX_COLOR_WORD_LEN 40
#define maxChars 100002              /* the characters of a new document + 2 */
#define BUF_SIZE 4096                /* the copy and paste buffer */
#define maxSyntaxFileSize 410000
#define TOOLBAR_ICONS_SIZE (1200 * 21)

/* t_data.inc */
#define TED_PANEL_NULL    0
#define TED_PANEL_FIND    1
#define TED_PANEL_REPLACE 2
#define TED_PANEL_SYNTAX  3
#define TED_PANEL_GOTO    4
#define TED_PANEL_WIDTH 150
#define count_of_dir_list_files 15
#define max_synt_auto_open 10

/* t_button.inc: the buttons */
#define ID_BUT_0 3
#define ID_BUT_NEW 3
#define ID_BUT_OPEN 4
#define ID_BUT_SAVE 5
#define ID_BUT_SAVE_AS 6
#define ID_BUT_SELECT 7
#define ID_BUT_CUT 8
#define ID_BUT_COPY 9
#define ID_BUT_PASTE 10
#define ID_BUT_FIND 11
#define ID_BUT_REPLACE 12
#define ID_BUT_KEY_WORDS 13
#define ID_BUT_UPPER 14
#define ID_BUT_LOWER 15
#define ID_BUT_REVERSE 16
#define ID_BUT_UNDO 17
#define ID_BUT_REDO 18
#define ID_BUT_INVISIBLE 19
#define ID_BUT_SYNTAX_LIST 20
#define ID_BUT_SYNTAX_MODE 21
#define ID_BUT_CONVERT_1251_866 22
#define ID_BUT_CONVERT_866_1251 23

/* t_edit.c */
extern u8 *icon_tl_sys;
extern u8 run_file_70[25];
extern edit_box edit_replace, edit_find, edit_goto;
extern u8 *unpac_mem;
extern char hed[];
extern char openfile_path[4096];
extern u8 dir_mem[32 + 304 * count_of_dir_list_files];
extern u16 wnd_s_pos[4];
extern char last_open_synt_file[32];
extern char buf[BUF_SIZE], buf_find[302], buf_replace[302], buf_goto[12];
extern system_colors sc;
extern u8 procinfo[1024];
extern char sys_path[4096], file_name[4096], syntax_path[4096], plugin_path[4096];
extern char filename_area[256];
/* the threads' stacks (t_edit.asm's thread: the message boxes', and
 * thread_coords: the key words' window's; their tops are passed) */
#define THREAD_STACK 16384
extern u8 thread_stack[THREAD_STACK], thread_coords_stack[THREAD_STACK];
#define thread ((void *)(thread_stack + THREAD_STACK))
#define thread_coords ((void *)(thread_coords_stack + THREAD_STACK))

/* t_data.c */
extern const char *fn_col_option;
extern const char fn_syntax_dir[], def_col_file[];
extern u32 exit_code;
extern u8 panel_but[21];
extern const char ini_name[], ini_sec_window[], ini_sec_options[];
extern char key_synt_file[], key_synt_ext[];
extern char synt_auto_open[64 * max_synt_auto_open + 1];
extern scrollbar wScr, hScr, ws_dir_lbox;
extern open_dialog OpenDialog_data;
extern const char default_dir[];
extern const char open_dialog_name[], communication_area_default_path[];
extern text_editor tedit0;
extern u8 conv_tabl[128];
extern void *el_focus;
extern u32 mouse_dd;
extern tree_list tree1;
extern option_box *opt_gr1;
extern option_box *opt_grlist1[];
extern option_box opt1, opt2, opt3;
extern check_box2 ch1;
extern char msgbox_1[], msgbox_2[], msgbox_3[], msgbox_4[], msgbox_6[], msgbox_7[], msgbox_9[];
extern char *msgbox_1_z, *msgbox_3_err, *msgbox_4_err, *msgbox_6_err, *msgbox_9_fdp;
extern const char txtFindCapt[], txtFindNext[], txtRerlaceCapt[], txtRerlace[], txtRerlaceAll[];
extern const char txtFormatCapt[], txtFormatApply[], txtGotoCapt[], txtGoto[], txt_Info[];
extern const char ch_text_1[];
extern mb_function msgbox_2N_funct[], msgbox_2O_funct[], msgbox_2E_funct[];
extern const unsigned char te_icon[], tl_sys_16[], tl_nod_16[];   /* tools/png2bgr.py */
extern const unsigned te_icon_w, te_icon_h, tl_sys_16_w, tl_sys_16_h, tl_nod_16_w, tl_nod_16_h;

/* t_button.c */
extern u8 *bmp_icon;
extern const u8 tbl_1251_866[256], tbl_866_1251[256];
void button(void);
void ted_Exit(void);
u32 get_find_options(void);
void but_replace_all(void);
void but_goto_line(void);
void ted_on_init_synt_err(int al);
void ted_but_new_file(void);
void On_NewFile(void);
void On_SaveAndNewFile(void);
void On_SaveAndOpenFile(void);
void On_SaveAndExit(void);
void On_Exit(void);
void but_no_msg_OpenFile(void);
void ted_but_ctrl_all(u32 opt_key);
void ted_but_open_file(void);
void ted_but_open_file_no_msg(void);
void auto_open_syntax(const char *of_path);
void ted_but_save_file(int dialog);
void ted_messages_after_open_file(int err);
void ted_save_err_msg(int al);
void ted_but_open_syntax(text_editor *edit);
s32 open_unpac_synt_file(const char *f_name);
void but_panel_find(void);
void but_panel_goto(void);
void but_panel_replace(void);
void but_panel_choose_syntax(void);
void on_panel_change(text_editor *edit, int new_id);
void ted_on_find_err(void);
void but_find_key_w(void);
void but_sumb_invis(text_editor *edit);
void but_use_syntax(text_editor *edit);
void notify_window_run(const char *text);
void copy_path(const char *name, const char *cur_dir_path, char *out);

/* t_menu.c */
extern ksubmenu_t *main_menu;
void tedit_menu_init(void);

/* t_draw.c */
void draw_window(void);
void draw_but_toolbar(void);
void draw_panels(text_editor *edit);
void EvSize(void);
void img_to_gray(const u8 *buf_rgb, u8 *buf_g24, u32 pixels);

/* wnd_k_words.c */
extern volatile u32 wnd_k_words_run;
void prop_start(void);

/* strlen.c */
u32 str_len(const char *s);
void str_cat(char *str1, const char *str2);
int str_instr(const char *str0, const char *str1);
void convert_int_to_str(u32 v, char *edi, int len);
s32 conv_str_to_int(const char *buf);

#endif
