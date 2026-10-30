/* KolibriOS for ARM: t_edit's window of the key words' search,
 * translated to C from programs/other/t_edit/wnd_k_words.inc (IgorA,
 * the KolibriOS team; GPL-2.0): a word typed, the syntax file's key
 * words beginning with it listed with their help, one copied to the
 * clipboard.
 *
 * As there, the window is a thread of t_edit's (but_find_key_w starts it
 * with 51.1 on the stack thread_coords, its TID in wnd_k_words_run;
 * ted_Exit ends it by 18.18), with its own window, events and keys (a
 * new thread's: ASCII). It reads the editor's key words and its icons,
 * shares the focus variable el_focus with the editor's elements, and
 * writes into the editor's buffer for the clipboard, as the original.
 *
 * What differs: the original takes its window's place from t_edit's
 * procinfo (the editor's window at its last redraw) and then fills that
 * procinfo with its own window's (function 9), which the editor's thread
 * uses at the same time; here the place is read once at the start and
 * the window has its own buffer for function 9. */
#include <string.h>
#include "t_edit.h"

#define prop_wnd_width 350             /* the window's width */
#define IMAGE_TOOLBAR_ICON_SIZE (20 * 20 * 3)

volatile u32 wnd_k_words_run;          /* the window's TID: no more than one window */
static const char txt_caption[] = "Search in keywords";
static char string1[MAX_COLOR_WORD_LEN + 2];
static u8 prop_procinfo[1024];         /* its function 9 */
static s32 prop_left, prop_top;        /* the editor's window at the start */

scrollbar w_scr_t3 = SCROLLBAR(16, 0, 3, 0, 15, 100, 0, 0, 0xeeeeee, 0xbbddff, 0, 1);
/* the list of the words found */
static tree_list tree3 = TREE_LIST(MAX_COLOR_WORD_LEN, 3, tl_key_no_edit, 16, 16, 0x8080ff, 0x0000ff, 0xffffff,
				   5, 30, 300, 160, 16, 0, 0, &el_focus, &w_scr_t3, 0);
static edit_box edit3 = EDIT_BOX(80, 70, 8, 0xffffff, 0xff, 0x80ff, 0, 0x8000, MAX_COLOR_WORD_LEN + 2, string1,
				 &mouse_dd, ed_focus, 0, 0);

static void prop_red_win(void)
{
	kos_begin_draw();
	kos_call(0, (u32)(prop_left + 5) << 16 | prop_wnd_width, (u32)(prop_top + 42) << 16 | 250, 0x33ffffd0,
		 0, (u32)txt_caption);
	kos_call(8, 5 << 16 | 19, 5 << 16 | 19, 3, sc.work_button, 0);
	kos_call(8, 30 << 16 | 19, 5 << 16 | 19, 4, sc.work_button, 0);
	const u8 *img = bmp_icon + (ID_BUT_FIND - ID_BUT_0) * IMAGE_TOOLBAR_ICON_SIZE;
	kos_call(7, (u32)img, 20 << 16 | 20, 5 << 16 | 5, 0, 0);       /* find */
	img -= 2 * IMAGE_TOOLBAR_ICON_SIZE;
	kos_call(7, (u32)img, 20 << 16 | 20, 30 << 16 | 5, 0, 0);      /* copy */
	kos_call(9, (u32)prop_procinfo, -1, 0, 0, 0);
	s32 h = *(s32 *)(prop_procinfo + 46);  /* box.height */
	if (h < 90) h = 90;                    /* the least size */
	h -= 58;
	if ((s32)tree3.box_height != h) {
		tree3.box_height = h;
		w_scr_t3.y_size = h;
	}
	w_scr_t3.all_redraw = 1;
	scrollbar_v_draw(&w_scr_t3);
	tl_draw(&tree3);
	edit_box_draw(&edit3);
	kos_end_draw();
}

static void prop_button(void)
{
	u32 ah = kos_get_button() >> 8;
	text_editor *edi = &tedit0;
	if (ah == 1) {                         /* .exit */
		tree3.data_img = 0;                /* the icons are the other lists' too */
		tree3.data_img_sys = 0;
		tl_data_clear(&tree3);
		if (el_focus == &tree3) el_focus = &tedit0;   /* (an addition: the list is gone) */
		wnd_k_words_run = 0;
		kos_exit();
	}
	if (ah == 3) {
		/* the words found */
		if (!edi->syntax_file) return;
		tl_info_clear(&tree3);
		tree3.style &= ~tl_cursor_pos_limited;
		TexColViv *k = edi->key_words_data;
		for (u32 i = 0; i < edi->key_words_count; i++, k++) {
			if (str_instr(k->Text, edit3.text)) continue;
			tl_node_add(&tree3, 1 << 16, k);           /* the key word */
			tl_cur_next(&tree3);
			if (k->f1) {
				tl_node_add(&tree3, 1, edi->help_text_f1 + k->f1);   /* its help */
				tl_cur_next(&tree3);
			}
		}
		tl_cur_beg(&tree3);
		tree3.style |= tl_cursor_pos_limited;   /* the cursor on the nodes */
		tl_draw(&tree3);
	} else if (ah == 4) {
		/* copied to the clipboard */
		const char *d = tl_node_get_data(&tree3);
		if (!d) return;
		char *b = tedit0.buffer;
		*(u32 *)b = MAX_COLOR_WORD_LEN;
		*(u32 *)(b + 4) = 0;               /* text */
		*(u32 *)(b + 8) = 1;               /* cp866 */
		memcpy(b + 12, d, MAX_COLOR_WORD_LEN);   /* the system buffer's header: 12 bytes */
		b[12 + MAX_COLOR_WORD_LEN] = 0;
		kos_call(54, 2, 12 + MAX_COLOR_WORD_LEN, (u32)b, 0, 0);
	}
}

/* prop_start: the thread (but_find_key_w) */
void prop_start(void)
{
	kos_event_mask(0xc0000027);
	prop_left = *(s32 *)(procinfo + 34);   /* the editor's box.left, box.top */
	prop_top = *(s32 *)(procinfo + 38);
	/* the list: as many nodes as key words (and its start and end) */
	tree3.info_max_count = 2 + tedit0.key_words_count;
	tl_data_init(&tree3);
	tree3.data_img = icon_tl_sys;
	tree3.data_img_sys = tree1.data_img_sys;
	edit_boxes_set_sys_color(&edit3, 1, &sc);
	for (;;) {                             /* prop_still */
		int ev = kos_wait_event();
		if (ev == 1) {
			prop_red_win();
		} else if (ev == 2) {              /* prop_key */
			u32 k = kos_get_key();
			edit_box_key(&edit3, k);
			tl_key(&tree3, k);
		} else if (ev == 3) {
			prop_button();
		} else if (ev == 6) {              /* prop_mouse */
			tl_mouse(&tree3);
			edit_box_mouse(&edit3);
		}
	}
}
