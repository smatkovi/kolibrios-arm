/* KolibriOS for ARM: box_lib's TreeList, a tree or a list box, translated
 * to C from programs/develop/libraries/box_lib/tree_list.asm (Copyright
 * (C) 2009-2025 IgorA <aie85playm@gmail.com>, GPL-2.0-only).
 *
 * The nodes are a doubly linked list in an array; node 0 is the head and
 * node 1 the end, so a walk ends at a node at or below node 1 (the
 * original's "cmp edx,ecx; jle"). Nodes are not removed: deleting and
 * undoing are times (t_create, t_delete against ch_tim and tim_undo).
 * The internal procedures keep their names; edi (the list) and edx (a
 * node) become arguments. Where the original slips, the C does what it
 * meant, with a note: tl_move_par restoring the node, tl_iterat_perv
 * with undone changes, tl_node_add's loop running one node too far,
 * tl_save_mem's size. The
 * captions "Строка"/"Отмены" are English here, as the fonts are not
 * Cyrillic. */
#include <stdlib.h>
#include <string.h>
#include "bl_sys.h"

#define N0(tl) ((tl)->data_nodes)
#define N1(tl) ((tl)->data_nodes + 1)

static void tl_draw_cursor(tree_list *tl, u32 bottom);
static void tl_draw_null_cursor(tree_list *tl, u32 bottom);
static void tl_draw_caption_cur_pos(tree_list *tl);
static void tb_scrol_resize(tree_list *tl);

/* the font's height and width from a colour of function 4 */
static u32 get_font_h(u32 c)
{
	u32 a = c >> 24;
	return ((a & 7) + 1) * ((a & 0x30) ? 16 : 9);
}

static u32 get_font_w(u32 c)
{
	u32 a = c >> 24;
	return ((a & 7) + 1) * ((a & 0x30) ? 8 : 6);
}

static u32 tl_get_node_index(tree_list *tl, TreeNode *n) { return n - tl->data_nodes; }
static TreeNode *tl_move_next(tree_list *tl, TreeNode *n) { return tl->data_nodes + n->next; }
static TreeNode *tl_move_perv(tree_list *tl, TreeNode *n) { return tl->data_nodes + n->perv; }

/* 1 if the node is not seen, after the additions, deletions and undos:
 * (td + tim_undo <= ch_tim && td) || tc > ch_tim - tim_undo */
static int tl_node_not_vis(tree_list *tl, TreeNode *n)
{
	if (n->t_delete && (s32)(n->t_delete + tl->tim_undo) <= (s32)tl->ch_tim) return 1;
	if ((s32)n->t_create > (s32)(tl->ch_tim - tl->tim_undo)) return 1;
	return 0;
}

/* the next node seen, skipping the closed ones' children */
static TreeNode *tl_iterat_next(tree_list *tl, TreeNode *n)
{
	s8 bl = n->close == 1 ? (s8)n->level : 0x7f;
	for (;;) {
		n = tl_move_next(tl, n);
		if (n <= N1(tl)) return n;
		if (tl->tim_undo ? tl_node_not_vis(tl, n) : n->t_delete != 0) continue;
		if (bl < (s8)n->level) continue;
		return n;
	}
}

/* the next node seen, the closed ones' children too */
static TreeNode *tl_iterat_next_all(tree_list *tl, TreeNode *n)
{
	for (;;) {
		n = tl_move_next(tl, n);
		if (n <= N1(tl)) return n;
		if (tl->tim_undo ? tl_node_not_vis(tl, n) : n->t_delete != 0) continue;
		return n;
	}
}

/* the parent node, or the node itself */
static TreeNode *tl_move_par(tree_list *tl, TreeNode *n)
{
	if (!n->level) return n;
	TreeNode *e = n;
	u8 lev = n->level;
	for (;;) {
		e = tl_move_perv(tl, e);
		if (e <= N1(tl)) return n;            /* no parent above */
		if (tl_node_not_vis(tl, e)) continue;
		if (e->level < lev) return e;
	}
}

/* the closed parent node of the highest level */
static TreeNode *tl_move_max_clo_par(tree_list *tl, TreeNode *n)
{
	TreeNode *r = n, *prev = NULL;
	for (;;) {
		n = tl_move_par(tl, n);
		if (n->close == 1) r = n;
		if (prev == n) return r;
		prev = n;
	}
}

/* the previous node seen */
static TreeNode *tl_iterat_perv(tree_list *tl, TreeNode *n)
{
	for (;;) {
		n = tl_move_perv(tl, n);
		if (n <= N1(tl)) break;
		if (tl->tim_undo ? tl_node_not_vis(tl, n) : n->t_delete != 0) continue;
		break;
	}
	return tl_move_max_clo_par(tl, n);
}

/* the node under the cursor (its index; below 2: none) */
static u32 tl_get_cur_node_index(tree_list *tl)
{
	TreeNode *n = N0(tl);
	for (u32 i = 0;; i++) {
		n = tl_iterat_next(tl, n);
		if (n <= N1(tl) || i == tl->cur_pos) break;
	}
	return tl_get_node_index(tl, n);
}

static u32 tl_get_node_count(tree_list *tl)
{
	u32 c = 0;
	for (TreeNode *n = tl_iterat_next(tl, N0(tl)); n > N1(tl); n = tl_iterat_next(tl, n)) c++;
	return c;
}

static u32 tl_get_node_count_all(tree_list *tl)
{
	u32 c = 0;
	for (TreeNode *n = tl_iterat_next_all(tl, N0(tl)); n > N1(tl); n = tl_iterat_next_all(tl, n)) c++;
	return c;
}

static u32 tl_get_rows_count(tree_list *tl)
{
	return (u16)(tl->box_height - tl->capt_cy) / tl->img_cy;
}

static s32 tl_get_display_cur_pos(tree_list *tl)
{
	s32 p = tl->cur_pos;
	if (tl->p_scroll) p -= tl->p_scroll->position;
	return p;
}

/* the cursor's count limited to the nodes (tl_cursor_pos_limited) */
static u32 tl_limited_count(tree_list *tl)
{
	u32 n = tl_get_node_count(tl);
	if ((tl->style & tl_cursor_pos_limited) && n) n--;
	return n;
}

static void tl_info_set_undo(tree_list *tl)
{
	if ((s32)tl->tim_undo < 1) return;
	TreeNode *n = tl_move_next(tl, N0(tl));
	tl->ch_tim -= tl->tim_undo;
	while (n > N1(tl)) {
		if ((s32)n->t_create > (s32)tl->ch_tim) {
			/* the node's creation was undone: out of the list */
			n->t_create = 0;
			n->t_delete = 0;
			tl->data_nodes[n->perv].next = n->next;
			tl->data_nodes[n->next].perv = n->perv;
		}
		if ((s32)n->t_delete > (s32)tl->ch_tim) n->t_delete = 0;   /* its deletion undone */
		n = tl_move_next(tl, n);
	}
	tl->tim_undo = 0;
}

/* the memory of the nodes and their data (the constructor) */
void tl_data_init(tree_list *tl)
{
	tl->data_info = calloc(tl->info_max_count, tl->info_size);
	tl->data_img = 0;
	tl->data_img_sys = 0;
	tl->data_nodes = calloc(tl->info_max_count, sizeof(TreeNode));
	tl_info_clear(tl);
	scrollbar *sb = tl->p_scroll;
	if (sb) {
		/* the child scroll bar's colours and place */
		sb->bg_color = tl->col_bkg;
		sb->front_color = tl->col_zag;
		sb->line_color = tl->col_txt;
		sb->x_pos = tl->box_left + tl->box_width;
		sb->y_size = tl->box_height;
		sb->y_pos = tl->box_top;
	}
}

/* the destructor; it frees the icons too */
void tl_data_clear(tree_list *tl)
{
	if (tl->data_img) free(tl->data_img);
	if (tl->data_img_sys) free(tl->data_img_sys);
	free(tl->data_info);
	free(tl->data_nodes);
}

void tl_info_clear(tree_list *tl)
{
	tl->ch_tim = 0;
	tl->tim_undo = 0;
	tl->cur_pos = 0;
	memset(tl->data_nodes, 0, sizeof(TreeNode) * tl->info_max_count);
	tl->data_nodes[0].next = 1;
	if (tl->p_scroll) {
		tl->p_scroll->position = 0;
		tb_scrol_resize(tl);
	}
}

/* Enter, Space, Up, Down, Left, Right, Delete, Page Up, Page Down */
static const u8 tl_key_ascii[9] = { 13, 32, 178, 177, 176, 179, 182, 184, 183 };
static const u8 tl_key_scan[9] = { 28, 57, 72, 80, 75, 77, 83, 73, 81 };

void tl_key(tree_list *tl, u32 key)
{
	if (*tl->el_focus != tl) return;
	const u8 *k = kos_call(SF_KEYBOARD, 2, 0, 0, 0, 0) == 1 ? tl_key_scan : tl_key_ascii;
	u8 ah = key >> 8;
	int redraw = 0;
	if (ah == k[0] && tl->on_press) {
		tl->on_press(tl);
		return;
	}
	if (ah == k[1]) tl_node_close_open(tl);
	if (ah == k[2]) tl_cur_perv(tl);
	if (ah == k[3]) tl_cur_next(tl);
	if (ah == k[7]) tl_cur_page_up(tl);
	if (ah == k[8]) tl_cur_page_down(tl);
	if (!(tl->style & tl_key_no_edit)) {
		if (ah == k[4]) { tl_node_lev_dec(tl); redraw = 1; }
		if (ah == k[5]) { tl_node_lev_inc(tl); redraw = 1; }
		if (ah == k[6]) { tl_node_delete(tl); redraw = 1; }
	}
	if (redraw) tl_draw(tl);
}

void tl_mouse(tree_list *tl)
{
	u32 bottom = tl->box_top + tl->box_height;
	s32 pos = bl_mouse_pos();
	s32 ebx = tl->box_left << 16;
	if (pos < ebx) goto no_in_wnd;            /* left of the list */
	ebx = (tl->box_left + tl->box_width) << 16;
	if (pos > ebx) goto test_scroll;          /* right of it */
	ebx = tl->box_top + tl->box_height;
	if ((s16)pos > (s16)ebx) goto test_scroll;    /* below */
	ebx -= tl->box_height;
	ebx = (ebx & 0xffff0000) | (u16)(ebx + tl->capt_cy);
	if ((s16)pos < (s16)ebx) goto test_scroll;    /* above, or on the caption */
	{
		/* the wheel */
		u32 w = bl_mouse_scroll();
		if (w) {
			s16 v = w, h = w >> 16;
			if (v < 0) for (int i = -v; i > 0; i--) tl_cur_perv(tl);
			else if (h < 0) for (int i = -h; i > 0; i--) tl_cur_perv(tl);
			else for (int i = v ? v : h; i > 0; i--) tl_cur_next(tl);
		}
		u32 b = bl_mouse_buttons_ext();
		if ((b & 0x01000000) && tl->on_press) {   /* a double click */
			tl->on_press(tl);
			return;
		}
		if (!(b & 1)) return;                 /* the left button */
		*tl->el_focus = tl;
		u32 mx = ((u32)pos >> 16) - tl->box_left;
		u32 row = ((u32)pos & 0xffff) - (u32)ebx;
		row /= tl->img_cy;
		if (tl->p_scroll) row += tl->p_scroll->position;
		s32 c = tl_limited_count(tl);
		if (!(c < (s32)row)) c = row;
		if ((u32)c != tl->cur_pos) {
			tl_draw_null_cursor(tl, bottom);
			tl->cur_pos = c;
		}
		/* a click on the node's plus or minus */
		u32 idx = tl_get_cur_node_index(tl);
		if (idx >= 2) {
			u16 lev = tl->data_nodes[idx].level + 1;
			if ((s16)mx >= (s16)tl->img_cx && (u16)(mx / tl->img_cx) == lev)
				tl_node_close_open(tl);
		}
		tl_draw_cursor(tl, bottom);
		tl_draw_caption_cur_pos(tl);
		return;
	}
test_scroll:
	/* the child scroll bar's mouse */
	{
		scrollbar *sb = tl->p_scroll;
		if (!sb) goto no_in_wnd;
		ebx = (((u32)ebx >> 16) + sb->x_size) << 16;
		if (pos > ebx) goto no_in_wnd;
		if (sb->max_area <= sb->cur_area) goto no_in_wnd;    /* all the nodes seen */
		scrollbar_v_mouse(sb);
		if (sb->redraw) {
			sb->redraw = 0;
			tl_draw(tl);
		}
		/* the original tests the dword at delta2: delta2 and r_size_x */
		if (sb->delta2 || sb->r_size_x) return;
	}
no_in_wnd:
	/* a click elsewhere: the focus lost */
	if (!bl_mouse_buttons()) return;
	if (*tl->el_focus != tl) return;
	*tl->el_focus = 0;
	tl_draw_cursor(tl, bottom);
}

void tl_info_undo(tree_list *tl)
{
	if (tl->ch_tim > tl->tim_undo) {
		tl->tim_undo++;
		tb_scrol_resize(tl);
	}
}

void tl_info_redo(tree_list *tl)
{
	if ((s32)tl->tim_undo >= 1) {
		tl->tim_undo--;
		tb_scrol_resize(tl);
	}
}

/* the number into a field of len characters, a blank after it */
static void tl_convert_to_str(u32 v, char *buf, int len)
{
	char d[12];
	int n = 0;
	do d[n++] = '0' + v % 10; while ((v /= 10));
	char *end = buf + len - 1;
	while (n && buf < end) {
		*buf++ = d[--n];
		buf[0] = ' ';
		buf[1] = 0;
	}
}

static char txt_capt_cur[] = "Row        ";
static char txt_capt_otm[] = "Undo       ";
#define CAPT_V 4                               /* the number's place */

static void tl_draw_caption_cur_pos(tree_list *tl)
{
	if (tl->capt_cy < 9) return;
	memcpy(txt_capt_cur + CAPT_V, "     ", 6);
	tl_convert_to_str(tl->cur_pos + 1, txt_capt_cur + CAPT_V, 5);
	bl_text_bg(((tl->box_left << 16) + (5 << 16 | 3)) + tl->box_top,
		   (tl->col_txt & 0xffffff) | 0xc0000000u, txt_capt_cur, 0, tl->col_zag);
}

/* the node's caption, right of its icon, cut at the list's right */
static void tl_draw_node_caption(tree_list *tl, u32 ebx, u32 ecx, TreeNode *n)
{
	if ((s16)tl->info_size <= (s16)tl->info_capt_offs) return;
	const char *s = (const char *)tl->data_info + tl->info_size * tl_get_node_index(tl, n) + tl->info_capt_offs;
	u32 x = (u16)((ebx >> 16) + tl->img_cx + 3);
	u32 len = strlen(s);
	/* tl_get_draw_text_len */
	u32 fw = get_font_w(tl->col_txt), right = tl->box_left + tl->box_width;
	if ((s32)right <= (s32)x) len = 0;
	else if (len >= (right - x) / fw) len = (right - x) / fw;
	u32 y = (u16)((ecx >> 16) + tl->img_cy - get_font_h(tl->col_txt));
	bl_text(x << 16 | y, tl->col_txt & 0x37ffffff, s, len);
}

/* an icon of a picture of icons img_cx x img_cy, 3 bytes a pixel */
static void tl_put_icon(tree_list *tl, const u8 *img, u32 index, u32 ebx, u32 ecx)
{
	/* ebx = x << 16 | img_cx, ecx = y << 16 | height */
	u32 size = (u16)(3 * tl->img_cx * tl->img_cy);
	kos_call(SF_PUT_IMAGE, (u32)(img + size * index), (ebx & 0xffff) << 16 | (ecx & 0xffff),
		 (ebx & 0xffff0000) | (ecx >> 16), 0, 0);
}

/* the plus or minus of a node with children; ah: its icon */
static void tl_draw_node_icon_opn_clo(tree_list *tl, u8 lev, u8 icon, TreeNode *n, u32 ebx, u32 rect)
{
	lev++;
	n = tl_iterat_next_all(tl, n);
	if (n <= N1(tl) || lev != n->level) return;
	ebx -= (u32)tl->img_cx << 16;
	if (!tl->data_img_sys) bl_rect(ebx, rect, tl->col_txt);
	else tl_put_icon(tl, tl->data_img_sys, icon, ebx, rect);
}

/* the line to the parent node */
static void tl_draw_node_icon_par_lin(tree_list *tl, u8 lev, TreeNode *n, u32 ebx, u32 rect)
{
	if (n->close == 1 || !lev) return;
	TreeNode *nx = tl_iterat_next_all(tl, n);
	u32 icon = nx > N1(tl) && lev == nx->level ? 3 : 6;   /* in the middle, at the end */
	ebx -= (u32)tl->img_cx << 16;
	if (!tl->data_img_sys) bl_rect(ebx, rect, tl->col_txt);
	else tl_put_icon(tl, tl->data_img_sys, icon, ebx, rect);
}

/* the vertical line up to the parent node */
static void tl_draw_node_icon_par_lin_up(tree_list *tl, u8 lev, TreeNode *n, u32 ebx, u32 rect)
{
	if (!tl->data_img_sys || !lev) return;
	u32 count = 0;
	for (;;) {
		n = tl_iterat_perv(tl, n);
		if (n <= N1(tl)) return;              /* the list's start */
		if (n->level <= lev) break;
		count++;
	}
	if (!count) return;
	ebx -= (u32)tl->img_cx << 16;
	s16 y = rect >> 16, top = tl->box_top + tl->capt_cy;
	const u8 *img = tl->data_img_sys + (u16)(3 * tl->img_cx * tl->img_cy);   /* icon 1 */
	for (; count; count--) {
		y -= tl->img_cy;
		if (y < top) return;
		kos_call(SF_PUT_IMAGE, (u32)img, (u32)tl->img_cx << 16 | tl->img_cy,
			 (ebx & 0xffff0000) | (u16)y, 0, 0);
	}
}

/* a node: its icon, caption, plus or minus, lines to the parent */
static void tl_draw_node(tree_list *tl, u32 pos, TreeNode *n, u32 bottom)
{
	u32 ebx = 1, ecx;
	if (!(tl->style & tl_list_box_mode)) ebx = 2 + n->level;   /* the cursor's and the plus's room */
	ebx = (u16)(ebx * tl->img_cx);
	ebx += tl->box_left;
	ebx = ebx << 16 | tl->img_cx;
	ecx = tl->img_cy * pos;
	if ((u16)ecx + tl->capt_cy > 0xffff) return;   /* many nodes: past the coordinates */
	ecx = (ecx & 0xffff0000) | (u16)(ecx + tl->capt_cy);
	ecx += tl->box_top;
	if ((s32)bottom < (s32)ecx) return;           /* below the list */
	u32 room = bottom - ecx;
	ecx = ecx << 16 | tl->img_cy;
	if ((s16)room < (s16)tl->img_cy) ecx = (ecx & 0xffff0000) | (u16)room;   /* cut at the bottom */
	else tl_draw_node_caption(tl, ebx, ecx, n);
	u32 rect = ecx;
	if (!tl->data_img) bl_rect(ebx, ecx, tl->col_txt);
	else tl_put_icon(tl, tl->data_img, n->type, ebx, ecx);
	tl_draw_node_icon_opn_clo(tl, n->level, n->close == 1 ? 9 : 10, n, ebx, rect);
	if (tl->style & tl_draw_par_line) {
		tl_draw_node_icon_par_lin(tl, n->level, n, ebx, rect);
		tl_draw_node_icon_par_lin_up(tl, n->level, n, ebx, rect);
	}
}

void tl_draw(tree_list *tl)
{
	u32 ebx = tl->box_left << 16 | (u16)tl->box_width;
	bl_rect(ebx, tl->box_top << 16 | tl->capt_cy, tl->col_zag);      /* the caption */
	bl_rect(ebx, (tl->box_top + tl->capt_cy) << 16 | (u16)(tl->box_height - tl->capt_cy), tl->col_bkg);
	if (tl->capt_cy >= 9) {
		tl_draw_caption_cur_pos(tl);
		if (tl->tim_undo) {
			memcpy(txt_capt_otm + CAPT_V + 1, "     ", 6);
			tl_convert_to_str(tl->tim_undo, txt_capt_otm + CAPT_V + 1, 5);
			bl_text(((tl->box_left << 16) + (100 << 16 | 3)) + tl->box_top,
				(tl->col_txt & 0xffffff) | 0x80000000u, txt_capt_otm, 0);
		}
	}
	/* the nodes scrolled away skipped */
	TreeNode *n = N0(tl);
	if (tl->p_scroll && tl->p_scroll->position) {
		for (u32 i = 0;;) {
			n = tl_iterat_next(tl, n);
			if (n <= N1(tl)) return;
			if (++i >= tl->p_scroll->position) break;
		}
	}
	u32 bottom = tl->box_top + tl->box_height;
	for (u32 i = 0;; i++) {
		n = tl_iterat_next(tl, n);
		if (n <= N1(tl)) break;
		tl_draw_node(tl, i, n, bottom);
	}
	tl_draw_cursor(tl, bottom);
	if (tl->p_scroll) scrollbar_v_draw(tl->p_scroll);   /* the child scroll bar */
}

static void tl_draw_cursor(tree_list *tl, u32 bottom)
{
	s32 p = tl_get_display_cur_pos(tl);
	if (p < 0) return;                         /* scrolled away above */
	if (!tl->data_img_sys) {
		u32 ebx = tl->box_left << 16 | tl->img_cx;
		u32 ecx = tl->img_cy * p + tl->box_top;
		ecx = (ecx & 0xffff0000) | (u16)(ecx + tl->capt_cy);
		if ((s32)bottom < (s32)ecx) return;
		u32 room = bottom - ecx;
		ecx = ecx << 16 | tl->img_cy;
		if ((s16)room < (s16)tl->img_cy) ecx = (ecx & 0xffff0000) | (u16)room;
		bl_rect(ebx, ecx, tl->col_txt);
		return;
	}
	const u8 *img = tl->data_img_sys;
	u32 edx = tl->box_left << 16 | (u16)(p * tl->img_cy);
	edx += tl->box_top;
	edx = (edx & 0xffff0000) | (u16)(edx + tl->capt_cy);
	if (*tl->el_focus != tl) img += tl->img_cx * tl->img_cy * 4 * 3;   /* icon 4: not in focus */
	u32 ecx = (u32)tl->img_cx << 16 | tl->img_cy;
	if ((s16)bottom < (s16)edx) return;
	u16 room = bottom - edx;
	if ((s16)room < (s16)tl->img_cy) ecx = (ecx & 0xffff0000) | room;
	kos_call(SF_PUT_IMAGE, (u32)img, ecx, edx, 0, 0);
}

static void tl_draw_null_cursor(tree_list *tl, u32 bottom)
{
	s32 p = tl_get_display_cur_pos(tl);
	if (p < 0) return;
	u32 ebx = tl->box_left << 16 | tl->img_cx;
	u32 ecx = tl->img_cy * p + tl->box_top;
	ecx = (ecx & 0xffff0000) | (u16)(ecx + tl->capt_cy);
	if ((s32)bottom < (s32)ecx) return;
	u32 room = bottom - ecx;
	ecx = ecx << 16 | tl->img_cy;
	if ((s16)room < (s16)tl->img_cy) ecx = (ecx & 0xffff0000) | (u16)room;
	bl_rect(ebx, ecx, tl->col_bkg);
}

/* the node's data with its caption ended by a 0 */
static void tl_copy_info(tree_list *tl, u8 *dest, const void *src, int check_len)
{
	memcpy(dest, src, tl->info_size);
	if (tl->info_capt_offs >= tl->info_size) return;
	u8 *end = dest + tl->info_size;
	if (tl->info_capt_len) {
		u32 e = (u16)(tl->info_capt_offs + tl->info_capt_len);
		if (!check_len || tl->info_size >= e) end = dest + e;
	}
	end[-1] = 0;
}

static void tb_scrol_resize(tree_list *tl)
{
	scrollbar *sb = tl->p_scroll;
	if (!sb) return;
	s32 count = tl_get_node_count(tl), rows = tl_get_rows_count(tl);
	if (!(count > rows)) count = 0;
	sb->cur_area = rows;
	sb->max_area = count;
	scrollbar_v_draw(sb);
}

void tl_node_add(tree_list *tl, u32 n_opt, const void *n_info)
{
	tl_info_set_undo(tl);
	/* after the node before the cursor's (po_t) */
	TreeNode *cur = tl->data_nodes + tl_get_cur_node_index(tl);
	u32 po_t = tl_get_node_index(tl, tl_move_perv(tl, cur));
	/* the original looks up to node max_count, one past the memory */
	for (u32 i = 2; i < tl->info_max_count; i++) {
		TreeNode *n = tl->data_nodes + i;
		if (n->t_create || n->t_delete) continue;
		tl->ch_tim++;
		n->type = n_opt >> 16;
		n->close = n_opt >> 8;
		n->level = (tl->style & tl_list_box_mode) ? 0 : (u8)n_opt;
		n->t_create = tl->ch_tim;
		n->perv = po_t;
		tl_copy_info(tl, tl->data_info + tl->info_size * i, n_info, 0);
		n->next = tl->data_nodes[po_t].next;
		tl->data_nodes[po_t].next = i;        /* the links turned to it */
		tl->data_nodes[n->next].perv = i;
		tb_scrol_resize(tl);
		return;
	}
}

void tl_node_set_data(tree_list *tl, const void *n_info)
{
	u32 i = tl_get_cur_node_index(tl);
	if (i < 2) return;
	tl_copy_info(tl, tl->data_info + tl->info_size * i, n_info, 1);
}

/* the data of the node under the cursor, or NULL */
void *tl_node_get_data(tree_list *tl)
{
	u32 i = tl_get_cur_node_index(tl);
	return i < 2 ? NULL : tl->data_info + tl->info_size * i;
}

/* the node at a position (0: the first), or NULL */
TreeNode *tl_node_poi_get_info(tree_list *tl, u32 node_ind)
{
	TreeNode *n = N0(tl);
	for (s32 k = node_ind + 1;;) {
		n = tl_iterat_next(tl, n);
		if (n <= N1(tl)) return NULL;
		if (--k <= 0) return n;
	}
}

TreeNode *tl_node_poi_get_next_info(tree_list *tl, TreeNode *node_p)
{
	TreeNode *n = tl_iterat_next_all(tl, node_p);
	return n > N1(tl) ? n : NULL;
}

void *tl_node_poi_get_data(tree_list *tl, TreeNode *node_p)
{
	u32 i = tl_get_node_index(tl, node_p);
	return i < 2 ? NULL : tl->data_info + tl->info_size * i;
}

void tl_node_delete(tree_list *tl)
{
	u32 i = tl_get_cur_node_index(tl);
	if (i < 2) return;
	tl->data_nodes[i].t_delete = ++tl->ch_tim;
	tb_scrol_resize(tl);
}

void tl_cur_beg(tree_list *tl)
{
	tl->cur_pos = 0;
	if (tl->p_scroll) {
		tl->p_scroll->position = 0;
		scrollbar_v_draw(tl->p_scroll);
	}
}

void tl_cur_next(tree_list *tl)
{
	if (!((s32)tl->cur_pos < (s32)tl_limited_count(tl))) return;
	u32 bottom = tl->box_top + tl->box_height;
	tl_draw_null_cursor(tl, bottom);
	tl->cur_pos++;
	scrollbar *sb = tl->p_scroll;
	if (sb && !((s32)tl->cur_pos < (s32)(tl_get_rows_count(tl) + sb->position))) {
		sb->position++;
		scrollbar_v_draw(sb);
		tl_draw(tl);                           /* the whole list again */
		return;
	}
	tl_draw_cursor(tl, bottom);
	tl_draw_caption_cur_pos(tl);
}

void tl_cur_perv(tree_list *tl)
{
	if (!tl->cur_pos) return;
	u32 bottom = tl->box_top + tl->box_height;
	tl_draw_null_cursor(tl, bottom);
	tl->cur_pos--;
	scrollbar *sb = tl->p_scroll;
	if (sb && sb->position && (s32)tl->cur_pos < (s32)sb->position) {
		sb->position--;
		scrollbar_v_draw(sb);
		tl_draw(tl);
		return;
	}
	tl_draw_cursor(tl, bottom);
	tl_draw_caption_cur_pos(tl);
}

void tl_cur_page_up(tree_list *tl)
{
	scrollbar *sb = tl->p_scroll;
	if (!sb) return;
	s32 rows = tl_get_rows_count(tl);
	if (!tl->cur_pos && !sb->position) return;
	if ((s32)tl->cur_pos < rows) tl->cur_pos = 0;
	else tl->cur_pos -= rows;
	if ((s32)sb->position < rows) sb->position = 0;
	else sb->position -= rows;
	tl_draw(tl);
	scrollbar_v_draw(sb);
}

void tl_cur_page_down(tree_list *tl)
{
	scrollbar *sb = tl->p_scroll;
	if (!sb) return;
	s32 maxc = tl_limited_count(tl), rows = tl_get_rows_count(tl);
	s32 maxs = maxc + 1;                       /* the last node seen half */
	if (maxs < rows) maxs = 0;
	else maxs -= rows;
	if ((s32)tl->cur_pos >= maxc && (s32)sb->position >= maxs) return;
	tl->cur_pos += rows;
	if (!((s32)tl->cur_pos < maxc)) tl->cur_pos = maxc;
	sb->position += rows;
	if (!((s32)sb->position < maxs)) sb->position = maxs;
	tl_draw(tl);
	scrollbar_v_draw(sb);
}

/* opens or closes the node under the cursor, if it has children */
void tl_node_close_open(tree_list *tl)
{
	u32 i = tl_get_cur_node_index(tl);
	if (i < 2) return;
	TreeNode *n = tl->data_nodes + i, *nx = tl_node_poi_get_next_info(tl, n);
	if (!nx || n->level >= nx->level) return;
	n->close ^= 1;
	tb_scrol_resize(tl);
	tl_draw(tl);
}

void tl_node_lev_inc(tree_list *tl)
{
	if (tl->style & tl_list_box_mode) return;
	u32 i = tl_get_cur_node_index(tl);
	if (i < 2) return;
	TreeNode *n = tl->data_nodes + i, *p = tl_iterat_perv(tl, n);
	if (p <= N1(tl)) return;                   /* no node above */
	if ((s8)n->level < (s8)(p->level + 1)) n->level++;
}

void tl_node_lev_dec(tree_list *tl)
{
	u32 i = tl_get_cur_node_index(tl);
	if (i < 2) return;
	if (tl->data_nodes[i].level) tl->data_nodes[i].level--;
}

/* the links of two nodes in a row exchanged (6 links) */
static void tl_node_move_po6(tree_list *tl, u32 eax, u32 ebx, TreeNode *edx, TreeNode *esi)
{
	tl_move_perv(tl, edx)->next = ebx;
	tl_move_next(tl, esi)->perv = eax;
	esi->perv = edx->perv;
	edx->next = esi->next;
	edx->perv = ebx;
	esi->next = eax;
}

/* ... of two nodes apart (8 links) */
static void tl_node_move_po8(tree_list *tl, u32 eax, u32 ebx, TreeNode *edx, TreeNode *esi)
{
	tl_move_perv(tl, edx)->next = ebx;
	tl_move_next(tl, edx)->perv = ebx;
	tl_move_perv(tl, esi)->next = eax;
	tl_move_next(tl, esi)->perv = eax;
	u32 t = edx->perv; edx->perv = esi->perv; esi->perv = t;
	t = edx->next; edx->next = esi->next; esi->next = t;
}

void tl_node_move_up(tree_list *tl)
{
	u32 ebx = tl_get_cur_node_index(tl);
	if (ebx < 2) return;
	TreeNode *esi = tl->data_nodes + ebx, *edx = tl_iterat_perv(tl, esi);
	u32 eax = tl_get_node_index(tl, edx);
	if (edx <= N1(tl)) return;
	if (edx->next == ebx) tl_node_move_po6(tl, eax, ebx, edx, esi);
	else tl_node_move_po8(tl, eax, ebx, edx, esi);
	tl_cur_perv(tl);
	tl_draw(tl);
}

void tl_node_move_down(tree_list *tl)
{
	u32 ebx = tl_get_cur_node_index(tl);
	if (ebx < 2) return;
	TreeNode *esi = tl->data_nodes + ebx, *edx = tl_iterat_next(tl, esi);
	u32 eax = tl_get_node_index(tl, edx);
	if (edx <= N1(tl)) return;
	if (esi->next == eax) tl_node_move_po6(tl, ebx, eax, esi, edx);   /* the order turned */
	else tl_node_move_po8(tl, eax, ebx, edx, esi);
	tl_cur_next(tl);
	tl_draw(tl);
}

/* the nodes into memory: a header of 26 bytes ('tree', the data's size,
 * the count, the style, the cursor, the caption's offset and length, the
 * scroll position), then per node its index, type/level/close and data;
 * opt 1 adds after what is there. 0, or tl_err_save_memory_size */
int tl_save_mem(tree_list *tl, u32 opt, void *h_mem, u32 mem_size)
{
	u8 *p = h_mem;
	if (opt) p += tl_get_mem_size(tl, h_mem);
	u32 isz = tl->info_size, count = tl_get_node_count_all(tl);
	/* the original leaves the nodes' 8 bytes out of the size */
	if ((s32)(count * (isz + 8) + tl_save_load_heder_size + 1 + (p - (u8 *)h_mem)) > (s32)mem_size)
		return tl_err_save_memory_size;
	memcpy(p, "tree", 4);
	*(u16 *)(p + 4) = isz;
	*(u32 *)(p + 6) = count;
	*(u32 *)(p + 10) = tl->style;
	*(u32 *)(p + 14) = tl->cur_pos;
	*(u16 *)(p + 18) = tl->info_capt_offs;
	*(u16 *)(p + 20) = tl->info_capt_len;
	*(u32 *)(p + 22) = tl->p_scroll ? tl->p_scroll->position : 0;
	p += tl_save_load_heder_size;
	for (TreeNode *n = tl_iterat_next_all(tl, N0(tl)); n > N1(tl); n = tl_iterat_next_all(tl, n)) {
		*(u32 *)p = tl_get_node_index(tl, n);
		memcpy(p + 4, n, 4);                   /* type, level, close */
		p += 8;
		memcpy(p, tl_node_poi_get_data(tl, n), isz);
		p += isz;
	}
	*p = 0;                                    /* the end */
	return 0;
}

/* opt: the element's index | (2 * add mode + init mode) << 16 */
int tl_load_mem(tree_list *tl, u32 opt, const void *h_mem, u32 mem_size)
{
	const u8 *p = h_mem;
	int err = 0;
	for (u32 k = opt & 0xffff; k; k--) {      /* the k-th element */
		if (memcmp(p, "tree", 4)) return tl_err_load_caption;
		p += (u16)(*(const u16 *)(p + 4) + 8) * *(const u32 *)(p + 6) + tl_save_load_heder_size;
	}
	if (memcmp(p, "tree", 4)) return tl_err_load_caption;
	if (!(opt & tl_load_mode_add)) tl_info_clear(tl);
	u32 isz = *(const u16 *)(p + 4);
	if (isz != tl->info_size) err |= tl_err_load_info_size;
	s32 count = *(const u32 *)(p + 6);
	if (count < 1) return err;
	const u8 *head = p;
	p += tl_save_load_heder_size;
	for (; count; count--) {
		u32 o = *(const u32 *)(p + 4);
		p += 8;
		tl_node_add(tl, o >> 16 | o << 16, p);
		tl_cur_next(tl);
		p += isz;
	}
	if (opt & tl_load_mode_add) return tl_err_load_caption;   /* as the original */
	tl->cur_pos = *(const u32 *)(head + 14);
	if (tl->p_scroll) {
		tl->p_scroll->position = *(const u32 *)(head + 22);
		scrollbar_v_draw(tl->p_scroll);
	}
	return err;
}

/* the memory tl_save_mem has filled */
u32 tl_get_mem_size(tree_list *tl, const void *h_mem)
{
	const u8 *p = h_mem;
	while (!memcmp(p, "tree", 4))
		p += (u16)(*(const u16 *)(p + 4) + 8) * *(const u32 *)(p + 6) + tl_save_load_heder_size;
	return p - (const u8 *)h_mem;
}
