/* KolibriOS for ARM: t_edit's buttons and their functions, translated to
 * C from programs/other/t_edit/t_button.inc (IgorA, the KolibriOS team;
 * GPL-2.0): the toolbar's and the panels' buttons, new, open and save
 * with their messages, the syntax files, the panels, the code pages.
 *
 * The message boxes are msgbox's threads, as there: the functions of
 * their buttons (On_SaveAndExit, On_NewFile, ...) run in the box's
 * thread, which ends after them; t_edit's window is drawn again when the
 * box's window goes (the window manager's redraw), and On_Exit's
 * exit_code ends t_edit there. The open and save dialogs are the program
 * opendial (proc_lib's OpenDialog_start waits for it).
 *
 * Differences, and why: the messages of @notify (the replacements made,
 * the text not found) are message boxes, as there is no @notify here;
 * "Save" in the message "Save changes?" saves to the file's name if it
 * has one (the original passes the message box's register to
 * ted_but_save_file, which then always asks for a name). */
#include <string.h>
#include <stdlib.h>
#include "t_edit.h"

u8 *bmp_icon;

void button(void)
{
	u32 id = kos_get_button() >> 8;
	switch (id) {
	case ID_BUT_NEW: ted_but_new_file(); return;
	case ID_BUT_OPEN: ted_but_open_file(); return;
	case ID_BUT_SAVE: ted_but_save_file(0); return;        /* without the dialog */
	case ID_BUT_SAVE_AS: ted_but_save_file(1); return;     /* with it */
	case ID_BUT_SELECT: ted_but_select_word(&tedit0); return;
	case ID_BUT_CUT: ted_but_cut(&tedit0); return;
	case ID_BUT_COPY: ted_but_copy(&tedit0); return;
	case ID_BUT_PASTE: ted_but_paste(&tedit0); return;
	case ID_BUT_FIND: but_panel_find(); return;
	case ID_BUT_REPLACE: but_panel_replace(); return;
	case ID_BUT_KEY_WORDS: but_find_key_w(); return;
	case ID_BUT_UPPER: ted_but_sumb_upper(&tedit0); return;
	case ID_BUT_LOWER: ted_but_sumb_lover(&tedit0); return;
	case ID_BUT_REVERSE: ted_but_reverse(&tedit0); return;
	case ID_BUT_UNDO: ted_but_undo(&tedit0); return;
	case ID_BUT_REDO: ted_but_redo(&tedit0); return;
	case ID_BUT_INVISIBLE: but_sumb_invis(&tedit0); return;
	case ID_BUT_SYNTAX_LIST: but_use_syntax(&tedit0); return;
	case ID_BUT_SYNTAX_MODE: but_panel_choose_syntax(); return;
	case ID_BUT_CONVERT_1251_866: ted_but_convert_by_table(&tedit0, tbl_1251_866); return;
	case ID_BUT_CONVERT_866_1251: ted_but_convert_by_table(&tedit0, tbl_866_1251); return;
	case 200: ted_but_open_syntax(&tedit0); return;
	case 201:                              /* find next */
		ted_but_find(&tedit0, get_find_options());
		return;
	case 202:                              /* replace */
		if (ted_but_replace(&tedit0, buf_replace, get_find_options(), 1)) ted_draw(&tedit0);
		return;
	case 203: but_replace_all(); return;   /* replace all */
	case 204: but_goto_line(); return;     /* go to the line */
	case 1:
	case 199:
		ted_Exit();
		return;
	}
}

void ted_Exit(void)
{
	if (wnd_k_words_run) {                 /* the key words' window ended (18.18) */
		kos_kill_pid(wnd_k_words_run);
		wnd_k_words_run = 0;
	}
	if (exit_code != 1 && ted_can_save(&tedit0)) {
		mb_create(msgbox_2, thread);       /* save the buffer in the file? */
		mb_setfunctions(msgbox_2E_funct);
		return;
	}
	free(bmp_icon);
	if (unpac_mem) free(unpac_mem);
	ted_delete(&tedit0);
	tl_data_clear(&tree1);
	kos_exit();
}

/* the find panel's options: 0 down, 1 up, 2 from the start; bit 30:
 * any case */
u32 get_find_options(void)
{
	u32 o = 0;
	if (opt_gr1 != &opt1) {
		o++;
		if (opt_gr1 != &opt2) o++;
	}
	if (!(ch1.flags & ch_flag_en)) o |= 1u << 30;
	return o;
}

void but_replace_all(void)
{
	u32 o = get_find_options(), n = 0;
	if (ted_but_replace(&tedit0, buf_replace, o, 1)) {
		n++;
		for (int i = 0; i < 0x1000; i++) {
			if (!ted_but_replace(&tedit0, buf_replace, o, 0)) break;
			n++;
		}
	}
	if (n) ted_draw(&tedit0);
	convert_int_to_str(n, msgbox_1_z, 8);
	str_cat(msgbox_1_z, txt_Info);
	notify_window_run(msgbox_1);
}

void but_goto_line(void)
{
	ted_go_to_position(&tedit0, conv_str_to_int(buf_goto), 1);
}

/* al: the error's code */
void ted_on_init_synt_err(int al)
{
	*msgbox_3_err = '0' + al;
	mb_create(msgbox_3, thread);           /* the colours' file did not open */
}

void ted_but_new_file(void)
{
	if (ted_can_save(&tedit0)) {
		mb_create(msgbox_2, thread);       /* save the changes? */
		mb_setfunctions(msgbox_2N_funct);
	} else {
		On_NewFile();
	}
	openfile_path[0] = 0;
	kos_set_caption(hed);
}

void On_NewFile(void)
{
	ted_clear(&tedit0, 1);
	draw_but_toolbar();
	ted_draw(&tedit0);
	memset(openfile_path, 0, 4);
}

void On_SaveAndNewFile(void)
{
	ted_but_save_file(0);
	if (!tedit0.err_save) On_NewFile();
}

void On_SaveAndOpenFile(void)
{
	ted_but_save_file(0);
	if (!tedit0.err_save) ted_but_open_file_no_msg();
}

void On_SaveAndExit(void)
{
	ted_but_save_file(0);
	if (!tedit0.err_save) exit_code = 1;
}

void On_Exit(void)
{
	exit_code = 1;
}

/* the file opened without asking */
void but_no_msg_OpenFile(void)
{
	ted_messages_after_open_file(ted_open_file(&tedit0, run_file_70, openfile_path));
	const char *s = openfile_path + str_len(openfile_path);
	while (s > openfile_path && s[-1] != '/' && s[-1] != '\\') s--;   /* the name's start */
	u32 n = str_len(s);
	if (n > 255) n = 255;
	memcpy(filename_area, s, n);           /* into the save dialog */
	filename_area[n] = 0;
}

/* Ctrl+N, O, S, F, G, H ('S' | 0x100: Ctrl+Shift+S, save as) */
void ted_but_ctrl_all(u32 opt_key)
{
	switch (opt_key & 0xff) {
	case 'N': ted_but_new_file(); break;
	case 'O': ted_but_open_file(); break;
	case 'S': ted_but_save_file(opt_key >> 8); break;
	case 'F': but_panel_find(); break;
	case 'G': but_panel_goto(); break;
	case 'H': but_panel_replace(); break;
	}
}

/* open a file, asking first if the changes are to be saved */
void ted_but_open_file(void)
{
	if (ted_can_save(&tedit0)) {
		mb_create(msgbox_2, thread);       /* save the changes? */
		mb_setfunctions(msgbox_2O_funct);
		return;
	}
	ted_but_open_file_no_msg();
}

/* .no_msg: also the function of "Discard" in the message */
void ted_but_open_file_no_msg(void)
{
	copy_path(open_dialog_name, communication_area_default_path, file_name);   /* the dialog's path */
	OpenDialog_data.type = 0;
	OpenDialog_start(&OpenDialog_data);
	if (OpenDialog_data.status == 2) {    /* the dialog did not start */
		memset(msgbox_9_fdp, ' ', 100);
		u32 n = str_len(file_name);
		memcpy(msgbox_9_fdp, file_name, n > 100 ? 100 : n);
		mb_create(msgbox_9, thread);
		return;
	}
	if (OpenDialog_data.status == 1) {
		auto_open_syntax(OpenDialog_data.openfile_path);
		ted_messages_after_open_file(ted_open_file(&tedit0, run_file_70, openfile_path));
	}
}

/* the syntax file for the file's extension (t_edit.ini's ext_N) */
void auto_open_syntax(const char *of_path)
{
	const char *e = of_path + str_len(of_path);
	while (e > of_path && *e != '.') e--;
	e++;                                   /* the extension */
	for (char *s = synt_auto_open; *s; s += 64)
		if (!strcmp(s + 32, e)) {
			open_unpac_synt_file(s);
			return;
		}
}

/* dialog 0: to the file's name, the dialog if it has none; 1: the dialog */
void ted_but_save_file(int dialog)
{
	if (dialog || !openfile_path[0]) {
		copy_path(open_dialog_name, communication_area_default_path, file_name);
		OpenDialog_data.type = 1;
		OpenDialog_start(&OpenDialog_data);
		if (OpenDialog_data.status != 1) return;
	}
	ted_save_file(&tedit0, run_file_70, openfile_path);
}

/* err: function 70's error of opening */
void ted_messages_after_open_file(int err)
{
	if (!err) return;
	*msgbox_4_err = err < 10 ? '0' + err : '?';
	mb_create(msgbox_4, thread);           /* the file did not open */
}

void ted_save_err_msg(int al)
{
	*msgbox_6_err = al;
	mb_create(msgbox_6, thread);           /* the file was not saved */
}

/* [Apply] on the syntax panel */
void ted_but_open_syntax(text_editor *edit)
{
	if (edit->panel_id != TED_PANEL_SYNTAX) return;
	const char *n = tl_node_get_data(&tree1);
	if (!n) return;
	fn_col_option = n;
	if (open_unpac_synt_file(n) == -1) return;
	ted_text_colored(edit);
	ted_draw(edit);
}

/* a syntax file (its name in the folder info) read; the bytes read, -1
 * if not. The original unpacks files packed by kpack; the syntax files
 * here are built unpacked (apps/t_edit/app.mk) */
s32 open_unpac_synt_file(const char *f_name)
{
	if (!f_name[0]) return 0;
	if (!strcmp(f_name, last_open_synt_file)) return 0;   /* read already */
	strncpy(last_open_synt_file, f_name, 31);
	copy_path(f_name, fn_syntax_dir, syntax_path);
	copy_path(syntax_path, sys_path, file_name);
	struct kos_r2 r = kos_read_file(file_name, tedit0.syntax_file, tedit0.syntax_file_size, 0);
	if ((s32)r.ebx == -1 || (r.eax && r.eax != 6)) {
		ted_on_init_synt_err(r.eax);
		return -1;
	}
	if (!memcmp(tedit0.syntax_file, "KPCK", 4)) {
		ted_on_init_synt_err(9);           /* packed: not unpacked here */
		return -1;
	}
	ted_init_syntax_file(&tedit0);
	return r.ebx;
}

void but_panel_find(void) { on_panel_change(&tedit0, TED_PANEL_FIND); }
void but_panel_goto(void) { on_panel_change(&tedit0, TED_PANEL_GOTO); }
void but_panel_replace(void) { on_panel_change(&tedit0, TED_PANEL_REPLACE); }
void but_panel_choose_syntax(void) { on_panel_change(&tedit0, TED_PANEL_SYNTAX); }

void on_panel_change(text_editor *edit, int new_id)
{
	if (edit->panel_id == new_id) {
		/* the same panel: closed, all drawn */
		edit->panel_id = TED_PANEL_NULL;
		edit->wnd.left = 0;
	} else if (edit->panel_id == TED_PANEL_NULL) {
		/* none open: opened, all drawn */
		edit->panel_id = new_id;
		edit->wnd.left = TED_PANEL_WIDTH;
	} else {
		/* another one: only the panel drawn */
		edit->panel_id = new_id;
		draw_panels(edit);
		return;
	}
	EvSize();
	ted_draw(edit);
}

/* the code pages' tables */
const u8 tbl_1251_866[256] = {
	[168] = 240, [184] = 241,
	[192] = 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
	144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
	160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
	224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
};

const u8 tbl_866_1251[256] = {
	[128] = 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
	208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
	224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
	[224] = 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
	168, 184,
};

void ted_on_find_err(void)
{
	notify_window_run(msgbox_7);           /* the text not found */
}

/* the key words' search: its window, a thread (one at a time) */
void but_find_key_w(void)
{
	if (wnd_k_words_run) return;
	int tid = kos_thread_create(prop_start, thread_coords);
	wnd_k_words_run = tid == -1 ? 0 : tid;
}

/* the invisible characters shown or not */
void but_sumb_invis(text_editor *edit)
{
	edit->mode_invis ^= 1;
	draw_but_toolbar();
	ted_draw(edit);
}

/* the key words coloured or not */
void but_use_syntax(text_editor *edit)
{
	edit->mode_color ^= 1;
	if (edit->mode_color) ted_text_colored(edit);
	draw_but_toolbar();
	ted_draw(edit);
}

/* @notify's message ("title\r\ntext" -tI) as a message box; a few
 * buffers, as a box reads its text while it is shown */
void notify_window_run(const char *text)
{
	static char mbs[4][512];
	static int next;
	char *mb = mbs[next++ & 3];
	const char *s = text;
	if (*s == '"') s++;
	const char *nl = strstr(s, "\r\n"), *end = strrchr(s, '"');
	if (!end || end < s) end = s + strlen(s);
	size_t o = 0;
	mb[o++] = 1;
	mb[o++] = 0;
	size_t tl = nl && nl < end ? (size_t)(nl - s) : 0;
	memcpy(mb + o, s, tl);                 /* the caption */
	o += tl;
	mb[o++] = 0;
	if (tl) s = nl + 2;
	for (; s < end && o < sizeof(mbs[0]) - 16; s++)
		if (*s != '\n') mb[o++] = *s;      /* the text, its lines ended by 13 */
	mb[o++] = 0;
	memcpy(mb + o, "Close\0", 7);
	mb_create(mb, thread);
}
