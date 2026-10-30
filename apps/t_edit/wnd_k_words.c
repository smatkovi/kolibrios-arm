/* KolibriOS for ARM: t_edit's window of the key words' search,
 * translated to C from programs/other/t_edit/wnd_k_words.inc (IgorA,
 * the KolibriOS team; GPL-2.0): a word typed, the syntax file's key
 * words beginning with it listed with their help, one copied to the
 * clipboard.
 *
 * In KolibriOS the window is a thread of its own beside the editor's;
 * here (no threads) it is a box in the editor's window with its own
 * event loop until it is closed (the editor's window drawn under it on
 * a redraw), the keys read as characters meanwhile (a new thread's
 * mode in KolibriOS). Its coordinates are the original's, from the
 * box's corner. */
#include <string.h>
#include "t_edit.h"

#define prop_wnd_width 350             /* the window's width */
#define IMAGE_TOOLBAR_ICON_SIZE (20 * 20 * 3)
#define CAPT_H 20
#define ID_CLOSE 0xf301
/* the original's buttons 3 and 4, apart from the toolbar's here */
#define ID_FIND 0xf303
#define ID_COPY 0xf304

u32 wnd_k_words_run;                   /* the window is open */
static const char txt_caption[] = "Search in keywords";
static char string1[MAX_COLOR_WORD_LEN + 2];
static int bx, by, bw, bh;             /* the box in the editor's window */

scrollbar w_scr_t3 = SCROLLBAR(16, 0, 3, 0, 15, 100, 0, 0, 0xeeeeee, 0xbbddff, 0, 1);
/* the list of the words found */
static tree_list tree3 = TREE_LIST(MAX_COLOR_WORD_LEN, 3, tl_key_no_edit, 16, 16, 0x8080ff, 0x0000ff, 0xffffff,
				   5, 30, 300, 160, 16, 0, 0, &el_focus, &w_scr_t3, 0);
static edit_box edit3 = EDIT_BOX(80, 70, 8, 0xffffff, 0xff, 0x80ff, 0, 0x8000, MAX_COLOR_WORD_LEN + 2, string1,
				 &mouse_dd, ed_focus, 0, 0);

/* the box's place: the original's window at the editor's (5, 42) */
static void place(void)
{
	s32 cw = *(s32 *)(procinfo + 62) + 1, ch = *(s32 *)(procinfo + 66) + 1;
	bw = prop_wnd_width;
	bh = 250;
	bx = 5;
	by = 42;
	if (bx + bw > cw) bx = cw > bw ? cw - bw : 0, bw = bw > cw ? cw : bw;
	if (by + bh > ch) by = ch > bh ? ch - bh : 0, bh = bh > ch ? ch : bh;
	int x = bx + 1, y = by + CAPT_H;       /* the client area's corner */
	tree3.box_left = x + 5;
	tree3.box_top = y + 30;
	edit3.left = x + 70;
	edit3.top = y + 8;
	w_scr_t3.x_pos = tree3.box_left + tree3.box_width;
	w_scr_t3.y_pos = tree3.box_top;
}

static void prop_red_win(void)
{
	place();
	int x = bx + 1, y = by + CAPT_H;
	kos_rect(bx, by, bw, bh, sc.work_graph);
	kos_rect(bx + 1, by + 1, bw - 2, CAPT_H - 1, sc.work_light);
	kos_rect(x, y, bw - 2, bh - CAPT_H - 1, 0xffffd0);   /* the window's colour, 0x33ffffd0 */
	kos_call(4, (u32)(bx + 8) << 16 | (u16)(by + 6), 0x80000000u | (sc.grab_text & 0xffffff), (u32)txt_caption, 0, 0);
	int cx = bx + bw - CAPT_H, cy = by + 2;
	kos_button_delete(ID_CLOSE);
	kos_button_delete(ID_FIND);
	kos_button_delete(ID_COPY);
	kos_button(cx, cy, CAPT_H - 4, CAPT_H - 4, ID_CLOSE, sc.work_light);
	kos_line(cx + 4, cy + 4, cx + CAPT_H - 9, cy + CAPT_H - 9, sc.grab_text);
	kos_line(cx + CAPT_H - 9, cy + 4, cx + 4, cy + CAPT_H - 9, sc.grab_text);
	kos_call(8, (u32)(x + 5) << 16 | 19, (u32)(y + 5) << 16 | 19, ID_FIND, sc.work_button, 0);
	kos_call(8, (u32)(x + 30) << 16 | 19, (u32)(y + 5) << 16 | 19, ID_COPY, sc.work_button, 0);
	const u8 *img = bmp_icon + (ID_BUT_FIND - ID_BUT_0) * IMAGE_TOOLBAR_ICON_SIZE;
	kos_call(7, (u32)img, 20 << 16 | 20, (u32)(x + 5) << 16 | (u16)(y + 5), 0, 0);   /* find */
	img -= 2 * IMAGE_TOOLBAR_ICON_SIZE;
	kos_call(7, (u32)img, 20 << 16 | 20, (u32)(x + 30) << 16 | (u16)(y + 5), 0, 0);  /* copy */
	s32 h = bh - CAPT_H - 1;
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
}

static void prop_button(u32 id)
{
	text_editor *edi = &tedit0;
	if (id == ID_FIND) {
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
	} else if (id == ID_COPY) {
		/* copied to the clipboard */
		const char *d = tl_node_get_data(&tree3);
		if (!d) return;
		char *b = tedit0.buffer;
		*(u32 *)b = MAX_COLOR_WORD_LEN;
		*(u32 *)(b + 4) = 0;               /* text */
		*(u32 *)(b + 8) = 1;               /* cp866 */
		memcpy(b + 12, d, MAX_COLOR_WORD_LEN);
		b[12 + MAX_COLOR_WORD_LEN] = 0;
		kos_call(54, 2, 12 + MAX_COLOR_WORD_LEN, (u32)b, 0, 0);
	}
}

void prop_start(void)
{
	wnd_k_words_run = 1;
	u32 mask = kos_event_mask(0xc0000027);
	u32 mode = kos_call(66, 2, 0, 0, 0, 0);
	kos_call(66, 1, 0, 0, 0, 0);
	/* the list: as many nodes as key words (and its start and end) */
	tree3.info_max_count = 2 + tedit0.key_words_count;
	tl_data_init(&tree3);
	tree3.data_img = icon_tl_sys;
	tree3.data_img_sys = tree1.data_img_sys;
	edit_boxes_set_sys_color(&edit3, 1, &sc);
	prop_red_win();
	while (wnd_k_words_run) {              /* prop_still */
		int ev = kos_wait_event();
		if (ev == 1) {
			draw_window();
			prop_red_win();
		} else if (ev == 2) {
			u32 k = kos_get_key();
			edit_box_key(&edit3, k);
			tl_key(&tree3, k);
		} else if (ev == 3) {
			u32 id = kos_get_button() >> 8;
			if (id == 1 || id == ID_CLOSE) break;
			prop_button(id);
		} else if (ev == 6) {
			tl_mouse(&tree3);
			edit_box_mouse(&edit3);
		}
	}
	/* .exit: the icons belong to the other lists */
	tree3.data_img = 0;
	tree3.data_img_sys = 0;
	tl_data_clear(&tree3);
	if (el_focus == &tree3) el_focus = &tedit0;    /* the list is gone */
	kos_button_delete(ID_CLOSE);
	kos_button_delete(ID_FIND);
	kos_button_delete(ID_COPY);
	wnd_k_words_run = 0;
	kos_call(66, 1, mode, 0, 0, 0);
	kos_event_mask(mask);
	draw_window();
}
