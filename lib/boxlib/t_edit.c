/* KolibriOS for ARM: box_lib's TextEditor (TED), translated to C from
 * programs/develop/libraries/box_lib/t_edit.asm (IgorA, last changed
 * 12.01.2021; the KolibriOS team; GPL-2.0), with fb_char_toupper and
 * fb_char_todown of filebrowser.asm (Mario79).
 *
 * The text is a doubly linked list of characters (struct symbol) in an
 * array: 0 is the start, 1 the end, and a character's place in the list
 * is independent of its place in the array. A change is a time:
 * tim_ch counts the changes, a character has the time of its creation
 * (tc) and of its deletion (td), and undoing is tim_undo going up
 * (ted_symbol_not_vis); the next change makes the undone ones final
 * (ted_set_undo). Colouring marks the first character of a key word
 * with its colour and the one after it with 255 (ted_text_colored).
 *
 * The procedures keep the original's names; its registers become
 * arguments and results: edi the editor (ted), edx a character, the
 * move procedures' dl their result. The drawing keeps the original's
 * coordinates, x << 16 | y as function 4 and 13 take them, with the
 * 16-bit halves (bx, cx) as the assembler changes them. Where the
 * original slips, the C does what it meant, with a note: the line ends
 * of a file with CR LF (blank lines came out doubled), the scroll bar's
 * size after lines added past the end, fb_char_toupper on cp866's box
 * characters 0xB0-0xBF, the clipboard's text without an ending 0.
 * Ctrl+Y (redo) is added to the keys. */
#include <stdlib.h>
#include <string.h>
#include "bl_sys.h"

#define LO(r) ((u16)(r))
#define SETLO(r, v) ((r) = ((r) & 0xffff0000u) | (u16)(v))
#define ROR16(r) ((r) = (r) >> 16 | (r) << 16)

#define T(i) (ted->tex + (i))                 /* ConvertIndexToPointer */
#define IDX(p) ((u32)((p) - ted->tex))        /* ted_get_text_arr_index */
#define COL(i) (ted->key_words_data + (i))    /* ColToIndexOffset */

static const char txtRow[] = "Rows", txtCol[] = "Cols", txtOtm[] = "Undo", txtBuf[] = "Buffer:";
static const u8 ted_symbol_space = 32;        /* a blank, now and then */
static const u8 ted_symbol_tab = 26;          /* the arrow for a tab, invisible ones shown */

/* the characters typed into the text */
static const u8 EvChar[256] = {
	0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

#define KEY_F1 0x3b
#define KEY_F3 0x3d

static void ted_draw_main_cursor(text_editor *ted);
static void ted_draw_cursor_sumb(text_editor *ted);
static void ted_draw_cur_line(text_editor *ted);
static void ted_draw_buffer(text_editor *ted);
static void ted_draw_help_f1(text_editor *ted);
static void ted_show_help_f1(text_editor *ted);
static void ted_go_to_pos(text_editor *ted, int col, int row);
static int ted_get_num_lines(text_editor *ted);
static int ted_cur_move_left(text_editor *ted);

static void panel_buttons(text_editor *ted)
{
	if (ted->fun_draw_panel_buttons) ted->fun_draw_panel_buttons();
}

/* fb_char_toupper, fb_char_todown: cp866 */
static u8 fb_char_toupper(u8 c)
{
	if (c < 'a') return c;
	if (c <= 'z') return c & ~0x20;
	if (c < 0xa0) return c;
	if (c < 0xb0) return c & ~0x20;            /* 0xA0-0xAF -> 0x80-0x8F (the original also 0xB0-0xDF) */
	if (c < 0xe0) return c;
	if (c > 0xef) return c;
	return c - 0x50;                           /* 0xE0-0xEF -> 0x90-0x9F */
}

static u8 fb_char_todown(u8 c)
{
	if (c < 'A') return c;
	if (c <= 'Z') return c + 0x20;
	if (c < 0x80) return c;
	if (c < 0x90) return c + 0x20;             /* 0x80-0x8F -> 0xA0-0xAF */
	if (c > 0x9f) return c;
	return c + 0x50;                           /* 0x90-0x9F -> 0xE0-0xEF */
}

/* ------------------------------------------------------------------ */
/* the list                                                            */

/* (td + tim_undo <= tim_ch && td) || tc > tim_ch - tim_undo */
static int ted_symbol_not_vis(text_editor *ted, const symbol *p)
{
	if (p->td && (s32)(p->td + ted->tim_undo) <= (s32)ted->tim_ch) return 1;
	if ((s32)p->tc > (s32)(ted->tim_ch - ted->tim_undo)) return 1;
	return 0;
}

/* the previous character seen (code 10 skipped), or the start */
static symbol *ted_iterat_perv(text_editor *ted, symbol *p)
{
	for (;;) {
		if (!p->perv) return ted->tex;
		p = T(p->perv);
		if (ted->tim_undo ? ted_symbol_not_vis(ted, p) : p->td != 0) continue;
		if (p->c == 10) continue;
		return p;
	}
}

/* the next character seen, or the end (tex_1) */
static symbol *ted_iterat_next(text_editor *ted, symbol *p)
{
	for (;;) {
		if ((s32)p->next <= 1) return ted->tex_1;
		p = T(p->next);
		if (ted->tim_undo ? ted_symbol_not_vis(ted, p) : p->td != 0) continue;
		if (p->c == 10) continue;
		return p;
	}
}

/* the character after the next endc not escaped by escc */
static symbol *ted_iterat_next_pos_char(text_editor *ted, symbol *p, u8 endc, u8 escc)
{
	u8 prev = 1;
	for (;;) {
		if (p->c == endc && escc != prev) break;
		if (p <= ted->tex_1) break;
		prev = p->c;
		p = ted_iterat_next(ted, p);
	}
	return ted_iterat_next(ted, p);
}

static symbol *ted_iterat_perv_color_tag(text_editor *ted, symbol *p)
{
	while (!p->col) {
		p = ted_iterat_perv(ted, p);
		if (p <= ted->tex_1) break;
	}
	return p;
}

static symbol *ted_iterat_next_color_tag(text_editor *ted, symbol *p)
{
	do p = ted_iterat_next(ted, p);
	while (!p->col && p > ted->tex_1);
	return p;
}

static symbol *ted_get_text_perv_pos(text_editor *ted, symbol *p) { return T(p->perv); }
static symbol *ted_get_text_next_pos(text_editor *ted, symbol *p) { return T(p->next); }

/* the character at column x of line y. gp_opt: 2 found, 1 the line
 * found but it is shorter (the line's end returned), 0 no such line
 * (tex_1 returned) */
static symbol *ted_get_pos_by_coords(text_editor *ted, int x, int y)
{
	int row = 0, col = 0;
	symbol *p = ted->tex;
	ted->gp_opt = 0;
	for (;;) {
		p = ted_iterat_next(ted, p);
		if (p <= ted->tex_1) break;
		if (col == x) {
			ted->gp_opt = 1;
			if (row >= y) break;
		}
		ted->gp_opt = 0;
		col++;
		if (p->c != 13) continue;
		if (row >= y) break;
		row++;
		col = 0;
	}
	if (row == y) ted->gp_opt++;
	if (!ted->gp_opt) p = ted->tex_1;
	return p;
}

static symbol *ted_get_pos_by_cursor(text_editor *ted)
{
	return ted_get_pos_by_coords(ted, ted->cur_x + ted->scr_h->position, ted->cur_y + ted->scr_w->position);
}

/* the length of a line (its end not counted) */
static int ted_strlen(text_editor *ted, int row)
{
	int n = 0, r = 0;
	symbol *p = ted->tex;
	for (;;) {
		p = ted_iterat_next(ted, p);
		if (p <= ted->tex_1) break;
		n++;
		if (p->c != 13) continue;
		n--;
		if (row == r) break;
		n = 0;
		r++;
	}
	return row == r ? n : 0;
}

/* a character's line and column */
static void ted_get_text_coords(text_editor *ted, symbol *p, int *line, int *col)
{
	int l = 0, c = 0;
	for (;;) {
		p = ted_iterat_perv(ted, p);
		if (!l) c++;
		if (p <= ted->tex_1) break;
		if (p->c == 13) l++;
	}
	*line = l;
	*col = c - 1;
}

static int ted_get_num_lines(text_editor *ted)
{
	int n = 1;
	for (symbol *p = ted_iterat_next(ted, ted->tex); p > ted->tex_1; p = ted_iterat_next(ted, p))
		if (p->c == 13) n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* creation, scroll bars, the selection                                */

void ted_init(text_editor *ted)
{
	u32 n = sizeof(symbol) * ted->max_chars;
	ted->tex = malloc(n);
	ted->tex_1 = ted->tex + 1;
	ted->tex_end = (symbol *)((u8 *)ted->tex + n);
	ted_clear(ted, 1);
	u8 *m = malloc(1024 + ted->syntax_file_size);     /* 1024: ted_arr_key_pos */
	ted->arr_key_pos = (int *)m;
	ted->syntax_file = m + 1024;
	ted_init_scroll_bars(ted, 3);
}

#define MIN_W_SCRL_ARE 3
#define MIN_H_SCRL_ARE 3

void ted_init_scroll_bars(text_editor *ted, u32 opt)
{
	scrollbar *w = ted->scr_w, *h = ted->scr_h;
	if (opt & 1) {                             /* the colours */
		w->bg_color = h->bg_color = ted->color_wnd_work;
		w->front_color = h->front_color = ted->color_wnd_capt;
		w->line_color = h->line_color = ted->color_wnd_bord;
	}
	if (opt & 4) {                             /* the document's size */
		int n = ted_get_num_lines(ted);
		if (n < TED_LINES_IN_NEW_FILE) n = TED_LINES_IN_NEW_FILE;
		w->max_area = n;
	}
	if (opt & 2) {                             /* the window's size */
		h->x_pos = ted->wnd.left + ted->rec.left;
		h->y_pos = ted->wnd.top + ted->wnd.height;
		s32 lines = (ted->wnd.height - ted->rec.top) / ted->rec.height;   /* lines in a page */
		if (lines <= MIN_W_SCRL_ARE) lines = MIN_W_SCRL_ARE;
		w->cur_area = lines;
		w->x_pos = ted->wnd.left + ted->wnd.width;
		w->y_pos = ted->wnd.top;
		w->y_size = ted->wnd.height;
		u32 cols = ted->wnd.width - ted->rec.left;
		h->x_size = cols;
		s32 c = cols / ted->rec.width;          /* characters in a page */
		if (c <= MIN_H_SCRL_ARE) c = MIN_H_SCRL_ARE;
		h->cur_area = c - 1;
	}
}

void ted_delete(text_editor *ted)
{
	free(ted->tex);
	free(ted->arr_key_pos);
}

int ted_can_save(text_editor *ted)
{
	return ted->tim_ls != ted->tim_ch - ted->tim_undo;
}

int ted_is_select(text_editor *ted)
{
	if (ted->drag_m == 1) return 0;
	return !(ted->sel.x0 == ted->sel.x1 && ted->sel.y0 == ted->sel.y1);
}

static void ted_sel_normalize(text_editor *ted)
{
	TexSelect s = ted->sel;
	if (s.y0 > s.y1 || (s.y0 == s.y1 && s.x0 > s.x1)) {
		int t = s.x0; s.x0 = s.x1; s.x1 = t;
		if (s.y0 > s.y1) { t = s.y0; s.y0 = s.y1; s.y1 = t; }
	}
	ted->seln = s;
}

/* a selection starts */
static void ted_sel_start(text_editor *ted)
{
	ted->sel.x0 = ted->sel.x1 = ted->cur_x + ted->scr_h->position;
	ted->sel.y0 = ted->sel.y1 = ted->cur_y + ted->scr_w->position;
}

/* the selection ends */
static void ted_sel_end(text_editor *ted)
{
	ted->drag_k = 0;
	if (ted_is_select(ted)) {
		memset(&ted->sel, 0, sizeof(ted->sel));
		ted_draw(ted);
		return;
	}
	ted_draw_main_cursor(ted);
}

/* the selection's end to the cursor */
static void ted_sel_move(text_editor *ted)
{
	ted->sel.x1 = ted->cur_x + ted->scr_h->position;
	ted->sel.y1 = ted->cur_y + ted->scr_w->position;
	panel_buttons(ted);                        /* the toolbar's Copy */
}

static void ted_sel_all(text_editor *ted)
{
	ted->sel.x0 = ted->sel.y0 = 0;
	ted->sel.x1 = 0;
	ted->sel.y1 = ted_get_num_lines(ted);
	ted_draw(ted);
	panel_buttons(ted);
}

void ted_clear(text_editor *ted, int cl_al_mem)
{
	ted->cur_x = ted->cur_y = 0;
	ted->tim_ch = ted->tim_ls = ted->tim_co = ted->tim_undo = 0;
	ted->help_id = -1;
	ted->ptr_free_symb = ted->tex + 2;
	ted->scr_w->position = 0;
	ted->scr_w->max_area = 100;                /* the lines of a new document */
	ted->scr_w->redraw = 1;
	ted->scr_h->position = 0;
	ted->scr_h->max_area = 100;                /* its characters */
	memset(&ted->sel, 0, sizeof(ted->sel));
	if (!cl_al_mem) return;
	memset(ted->tex, 0, sizeof(symbol) * ted->max_chars);
	ted->tex[0].next = 1;
}

/* a syntax file read into syntax_file: its colours and key words */
void ted_init_syntax_file(text_editor *ted)
{
	for (int i = 0; i < 0x100; i++) ted->arr_key_pos[i] = -1;
	ted->colors_text_count = 1;
	ted->help_text_f1 = 0;
	ted->help_id = -1;
	/* colors_text_count, key_words_count, the 7 colours */
	memcpy((u8 *)ted + ted_offs_count_colors, ted->syntax_file, 9 * 4);
	ted->text_colors = (u32 *)(ted->syntax_file + 32);
	ted->key_words_data = (TexColViv *)(ted->syntax_file + 32 + 4 * ted->colors_text_count);
	for (u32 i = 0; i < ted->key_words_count; i++) {
		u8 c = COL(i)->Text[0];
		if (ted->arr_key_pos[c] == -1) ted->arr_key_pos[c] = i;   /* the first word of each character */
	}
	ted->help_text_f1 = (char *)(ted->key_words_data + ted->key_words_count);
	ted_init_scroll_bars(ted, 1);
}

/* the text's memory for count characters more (68.20 in the original,
 * whose new pages are zero; here they are cleared) */
static void ted_mem_resize_no_2(text_editor *ted, u32 count)
{
	u32 old = ted->max_chars;
	count += ted->increase_size;               /* room for editing */
	ted->max_chars = count;
	ted->tex = realloc(ted->tex, count * sizeof(symbol));
	if (count > old) memset(ted->tex + old, 0, (count - old) * sizeof(symbol));
	ted->tex_1 = ted->tex + 1;
	ted->tex_end = ted->tex + count;
	ted->ptr_free_symb = ted->tex + 2;
}

static void ted_mem_resize(text_editor *ted, u32 count)
{
	ted_mem_resize_no_2(ted, count + 2);       /* the start and end characters */
}

/* the file read into tex (size bytes) made the text */
static void ted_on_open_file(text_editor *ted, u32 size)
{
	if (!size) {
		ted_clear(ted, 1);                     /* an empty file */
	} else {
		ted_clear(ted, 0);
		/* a 10 after a 13 is kept (the iterators skip 10), another 10
		 * ends a line as 13; the original stepped over the character
		 * after a 13 10, so a blank line of a CR LF file came out twice */
		u8 *b = (u8 *)ted->tex;
		for (u32 i = 0; i < size; i++)
			if (b[i] == 10 && !(i && b[i - 1] == 13)) b[i] = 13;
		/* the bytes into characters, from the end: byte k is character k + 2 */
		symbol *s = T(size + 2);
		for (u32 k = size;; k--, s--) {
			s->c = b[k];
			s->perv = k + 1;
			s->next = k + 3;
			s->tc = -1;
			s->td = 0;
			if (!k) break;
		}
		s->perv = 0;                           /* the first character */
		ted->tex[0].perv = 0;
		ted->tex[0].next = 2;
		ted->tex[1].next = 0;                  /* the end */
		ted->tex[1].perv = size + 1;
		ted->tex[1].tc = 0;
		symbol *e = T(size + 1);
		e->next = 1;
		for (e++; e < ted->tex_end; e++)       /* after a bigger file before */
			e->tc = e->td = 0;
	}
	int n = ted_get_num_lines(ted);
	if (n < TED_LINES_IN_NEW_FILE) n = TED_LINES_IN_NEW_FILE;
	ted->scr_w->max_area = n;
	ted_text_colored(ted);
	ted_draw(ted);
	panel_buttons(ted);
}

/* ------------------------------------------------------------------ */
/* editing                                                             */

/* the undone changes made final before a new one */
static void ted_set_undo(text_editor *ted)
{
	ted->drag_k = 0;
	if ((s32)ted->tim_undo < 1) return;
	symbol *p = ted_get_text_next_pos(ted, ted->tex);
	ted->tim_ch -= ted->tim_undo;
	s32 now = ted->tim_ch;
	if ((s32)ted->tim_ls > now) ted->tim_ls = 0;
	while (p > ted->tex_1) {
		if ((s32)p->tc > now) {
			/* its creation was undone: out of the list */
			p->tc = p->td = 0;
			T(p->perv)->next = p->next;
			symbol *nx = T(p->next);
			nx->perv = p->perv;
			if (ted->ptr_free_symb > p) ted->ptr_free_symb = p;
			p = nx;
			continue;
		}
		if ((s32)p->td > now) p->td = 0;       /* its deletion undone */
		p = ted_get_text_next_pos(ted, p);
	}
	ted->tim_undo = 0;
	if ((s32)ted->tim_ch < (s32)ted->tim_co) ted->tim_co = 0;
}

void ted_text_add(text_editor *ted, const char *text, u32 t_len, u32 add_opt)
{
	if ((s32)t_len < 1) return;
	const u8 *esi = (const u8 *)text;
	symbol *p = ted_get_text_perv_pos(ted, ted_get_pos_by_cursor(ted));
	u32 po_t = IDX(p);
	s32 new_spc = 0, new_lin = 0;
	if (ted->gp_opt != 2) {
		/* the cursor right of the line's end: blanks first */
		new_spc = ted->scr_h->position + ted->cur_x;
		new_spc -= ted_strlen(ted, ted->scr_w->position + ted->cur_y);
	}
	u32 ebx = t_len;
	if (ted->gp_opt == 0) {
		/* the cursor below the text: lines first */
		new_lin = ted->scr_w->position + ted->cur_y + 1;
		new_lin -= ted_get_num_lines(ted);
		/* the original adds the number of lines there were */
		ted->scr_w->max_area += new_lin;
	}
	symbol *edx = ted->ptr_free_symb;
	for (;;) {
		if (!edx->tc && !edx->td) {
			if (add_opt & ted_opt_ed_change_time) ted->tim_ch++;
			if ((add_opt & ted_opt_ed_move_cursor) && new_lin <= 0 && new_spc <= 0) {
				ted->cur_x++;
				if (*esi == 13) {
					ted->cur_x = 0;
					ted->cur_y++;
					ted->scr_w->max_area++;
				}
			}
			add_opt &= ~ted_opt_ed_change_time;
			/* the character inserted after po_t */
			edx->c = *esi;
			edx->tc = ted->tim_ch;
			edx->perv = po_t;
			edx->next = T(po_t)->next;
			u32 i = IDX(edx);
			T(po_t)->next = i;
			T(edx->next)->perv = i;
			po_t = i;
			if (new_lin > 0) {
				new_lin--;
				edx->c = 13;
			} else if (new_spc > 0) {
				new_spc--;
				edx->c = ' ';
			} else {
				esi++;
				ebx--;
			}
		}
		edx++;
		if (edx >= ted->tex_end) {
			/* out of memory */
			if (!ted->increase_size) break;
			u32 at = IDX(edx);
			ted_mem_resize_no_2(ted, ted->max_chars);
			edx = T(at);
			ted->ptr_free_symb = edx;
			if (ebx) continue;
			break;
		}
		if (!ebx) {
			ted->ptr_free_symb = edx;          /* to find free memory sooner */
			break;
		}
	}
	ted_text_colored(ted);
}

/* a character c after p in a free cell from *cell on */
static void ted_char_add(text_editor *ted, symbol **cell, symbol *p, u8 c)
{
	symbol *s = *cell;
	while (s < ted->tex_end && (s->tc || s->td)) s++;
	*cell = s;
	if (s < ted->tex_end) {
		s->tc = ted->tim_ch;
		s->c = c;
		s->perv = IDX(p);
		s->next = p->next;
		u32 i = IDX(s);
		p->next = i;
		T(s->next)->perv = i;
	}
	ted_text_colored(ted);
}

void ted_but_convert_by_table(text_editor *ted, const u8 *table)
{
	for (symbol *p = ted->tex; (s32)p->next > 1;) {
		p = T(p->next);
		u8 c = table[p->c];
		if (c) p->c = c;                       /* a new code */
	}
	ted_draw(ted);
}

/* the selection's characters through conv (changes counted) */
static u32 ted_convert_sel_text(text_editor *ted, u8 (*conv)(u8))
{
	u32 count = 0;
	if (!ted_is_select(ted)) return 0;
	ted_set_undo(ted);
	ted_sel_normalize(ted);
	symbol *a = ted_get_pos_by_coords(ted, ted->seln.x0, ted->seln.y0);
	symbol *b = ted_get_pos_by_coords(ted, ted->seln.x1, ted->seln.y1);
	if (a == b) return 0;
	ted->tim_ch++;
	symbol *cell = ted->ptr_free_symb;
	for (symbol *p = a;;) {
		u8 c = conv(p->c);
		if (c != p->c) {
			p->td = ted->tim_ch;
			ted_char_add(ted, &cell, p, c);
			p = ted_get_text_next_pos(ted, p);     /* to the character added */
			count++;
		}
		p = ted_iterat_next(ted, p);
		if (p == ted->tex || p == b) break;
	}
	if (!count) ted->tim_ch--;
	return count;
}

/* 1 if a character was deleted */
static int ted_text_del(text_editor *ted, u32 del_opt)
{
	if ((del_opt & ted_opt_ed_move_cursor) && !ted_cur_move_left(ted)) return 0;
	symbol *p = ted_get_pos_by_cursor(ted);
	if (ted->gp_opt == 1) return 0;
	if (del_opt & ted_opt_ed_change_time) ted->tim_ch++;
	p->td = ted->tim_ch;
	return 1;
}

/* the selected text deleted: 1 if deleted */
static int ted_sel_text_del(text_editor *ted, u32 del_opt)
{
	if (!ted_is_select(ted)) return 0;
	int al = 1;
	ted_sel_normalize(ted);
	symbol *e = ted_get_pos_by_coords(ted, ted->seln.x1, ted->seln.y1);
	symbol *p = ted_get_pos_by_coords(ted, ted->seln.x0, ted->seln.y0);
	if (del_opt & ted_opt_ed_change_time) ted->tim_ch++;
	while (p != ted->tex && p != e) {
		p->td = ted->tim_ch;
		del_opt &= ~ted_opt_ed_change_time;
		p = ted_iterat_next(ted, p);
	}
	if (del_opt & ted_opt_ed_change_time) {    /* nothing deleted */
		ted->tim_ch--;
		al = 0;
	} else {
		ted_go_to_pos(ted, ted->seln.x0, ted->seln.y0);
		memset(&ted->sel, 0, sizeof(ted->sel));
	}
	return al;
}

/* the characters from p0 to p1 in the reverse order */
static void ted_revers(text_editor *ted, symbol *p0, symbol *p1)
{
	if (p0 == p1) return;
	if (p1 == ted->tex_1) p1 = ted_get_text_perv_pos(ted, p1);
	symbol *a = T(p0->perv), *b = T(p1->next);
	u32 t = a->next;
	a->next = b->perv;
	b->perv = t;
	t = p0->perv;
	p0->perv = p1->next;
	p1->next = t;
	for (symbol *p = p0;;) {
		t = p->next;
		p->next = p->perv;
		p->perv = t;
		if (p == p1) break;
		p = T(t);
	}
	ted_text_colored(ted);
}

/* ------------------------------------------------------------------ */
/* the cursor: 0 not moved, 1 or 2 moved, 3 left and up, 8 scrolled    */

static int ted_cur_move_up(text_editor *ted)
{
	if (ted->cur_y) { ted->cur_y--; return 2; }
	if (ted->scr_w->position) { ted->scr_w->position--; return 8; }
	return 0;
}

static int ted_cur_move_down(text_editor *ted)
{
	scrollbar *w = ted->scr_w;
	if (ted->cur_y < (s32)w->cur_area - 1) { ted->cur_y++; return 2; }
	if ((s32)w->max_area > (s32)(ted->cur_y + w->position + 1)) { w->position++; return 8; }
	return 0;
}

static int ted_cur_move_page_up(text_editor *ted)
{
	scrollbar *w = ted->scr_w;
	int dl = 0;
	if (!((s32)w->cur_area > (s32)w->position)) {
		w->position -= w->cur_area;
		dl = 1;
	}
	if (w->position && dl != 1) {
		w->position = 0;
		dl = 1;
	}
	return dl;
}

static int ted_cur_move_page_down(text_editor *ted)
{
	scrollbar *w = ted->scr_w;
	s32 last = w->max_area - w->cur_area;
	if (!((s32)w->position < last)) return 0;
	w->position += w->cur_area;
	w->redraw = 1;
	if ((s32)w->position > last) w->position = last;
	return 1;
}

static int ted_cur_move_x_last_char(text_editor *ted);

static int ted_cur_move_left(text_editor *ted)
{
	if (ted->cur_x) { ted->cur_x--; return 1; }
	if (ted->scr_h->position) { ted->scr_h->position--; return 8; }
	if (!ted->cur_y) {
		if (!ted->scr_w->position) return 0;
		ted->scr_w->position--;
		ted->scr_w->redraw = ted->scr_h->redraw = 1;
		ted_cur_move_x_last_char(ted);
		return 8;
	}
	ted->cur_y--;
	return ted_cur_move_x_last_char(ted) == 8 ? 8 : 3;
}

static int ted_cur_move_right(text_editor *ted)
{
	if (ted->cur_x < (s32)ted->scr_h->cur_area) { ted->cur_x++; return 1; }
	ted->scr_h->position++;
	return 8;
}

/* the cursor after the line's last character */
static int ted_cur_move_x_last_char(text_editor *ted)
{
	scrollbar *h = ted->scr_h;
	int dl = 0;
	s32 len = ted_strlen(ted, ted->cur_y + ted->scr_w->position);
	if (len < (s32)h->position) {
		dl = 8;
		h->position = len;
	}
	len -= h->position;
	if (len > (s32)h->cur_area) {              /* b---[---]---e */
		h->position += len;
		len = h->cur_area;
		h->position -= len;
		dl = 8;
	}
	ted->cur_x = len;
	return dl;
}

static int ted_cur_move_x_first_char(text_editor *ted)
{
	int dl = 0;
	if (ted->cur_x) { ted->cur_x = 0; dl = 1; }
	if (ted->scr_h->position) { ted->scr_h->position = 0; dl = 8; }
	return dl;
}

void ted_go_to_position(text_editor *ted, int row, int col)
{
	int n = ted_get_num_lines(ted);
	if (row > n) row = n;
	row--;
	if (row < 0) row = 0;
	col--;
	if (col < 0) col = 0;
	ted_go_to_pos(ted, col, row);
	ted_draw(ted);
}

/* the cursor to a column and line, the scroll bars after it */
static void ted_go_to_pos(text_editor *ted, int col, int row)
{
	scrollbar *h = ted->scr_h, *w = ted->scr_w;
	col -= h->position;
	if (col < 0) {                             /* scrolled left */
		h->position += col;
		col = 0;
	}
	int m = 5;                                 /* the room wanted on the left */
	if (col < m) {
		m -= col;
		if ((s32)h->position < m) {
			col += h->position;
			h->position = 0;
		} else {
			h->position -= m;
			col += m;
		}
	}
	if (col >= (s32)h->cur_area) {             /* scrolled right */
		m = col - h->cur_area + 1;
		h->position += m;
		col -= m;
	}
	ted->cur_x = col;
	row -= w->position;
	if (row < 0) {                             /* scrolled up */
		w->position += row;
		row = 0;
	} else if (row >= (s32)w->cur_area) {      /* scrolled down */
		m = row - w->cur_area + 1;
		w->position += m;
		row -= m;
	}
	ted->cur_y = row;
}

static void ted_scroll_set_redraw(text_editor *ted)
{
	ted->scr_w->redraw = 1;
	ted->scr_h->redraw = 1;
}

/* ------------------------------------------------------------------ */
/* colouring                                                           */

/* tab_all_num, tab_alpha: letters, '_' (and digits) by bit */
static const u8 tab_all_num[32] = { 0,0,0,0,0,0,0xff,0x03, 0xfe,0xff,0xff,0x87, 0xfe,0xff,0xff,0x07 };
static const u8 tab_alpha[32] = { 0,0,0,0,0,0,0,0, 0xfe,0xff,0xff,0x87, 0xfe,0xff,0xff,0x07 };
static int isalnum_s(const symbol *p) { return tab_all_num[p->c >> 3] >> (p->c & 7) & 1; }
static int isalpha_s(const symbol *p) { return tab_alpha[p->c >> 3] >> (p->c & 7) & 1; }

/* the next key word from p on marked; returns where to go on from
 * (the start when nothing more was found) */
static symbol *ted_text_find_sel_color(text_editor *ted, symbol *edx)
{
	symbol *begPos = NULL, *endPos = NULL;
	int find = 0;
	u8 f_color = 1;
	s32 word_n = 0, l_pos;
	for (;;) {
		edx = ted_iterat_next(ted, edx);
		if (edx <= ted->tex_1) break;
		word_n = ted->arr_key_pos[edx->c];
		if (word_n < 0) continue;
		/* the words with this first character: word_n .. l_pos - 1 */
		l_pos = word_n;
		while (l_pos < (s32)ted->key_words_count && COL(l_pos)->Text[0] == COL(word_n)->Text[0]) l_pos++;
		begPos = edx;
		for (int pos = 1;; pos++) {
			edx = ted_iterat_next(ted, edx);
			while (l_pos > word_n && (u8)COL(l_pos - 1)->Text[pos] != edx->c) l_pos--;
			if (!COL(word_n)->Text[pos]) {
				/* word_n ends here */
				endPos = edx;
				f_color = COL(word_n)->color;
				find = 1;
				u8 fl = COL(word_n)->flags;
				if (fl) {
					symbol *s = ted_iterat_perv(ted, begPos);
					if ((fl & 1) && isalnum_s(s)) find = 0;      /* not after a letter or digit */
					else if ((fl & 8) && isalpha_s(s)) find = 0; /* not after a letter */
					s = endPos;
					if ((fl & 2) && isalnum_s(s)) find = 0;      /* not before ... */
					else if ((fl & 16) && isalpha_s(s)) find = 0;
					if (fl & 4) {                                /* up to endc */
						s = ted_iterat_next_pos_char(ted, s, COL(word_n)->endc, COL(word_n)->escc);
						if (!(s > ted->tex_1)) {
							/* the file's end before endc */
							s = ted_iterat_perv(ted, s);
							find = 2;
						}
						endPos = s;
					}
				}
				if (word_n == l_pos) break;    /* surely the last word */
			}
			if (edx <= ted->tex_1) break;
			while (l_pos > word_n && (u8)COL(word_n)->Text[pos] != edx->c) word_n++;
			if (word_n == l_pos) break;
		}
		if (find) break;
		edx = begPos;
	}
	if (find) {
		begPos->col = f_color;
		endPos->col = 0xff;
		if (find != 2) return ted_get_text_perv_pos(ted, endPos);   /* on after it */
	}
	return ted->tex;
}

void ted_text_colored(text_editor *ted)
{
	ted->tim_co = ted->tim_ch - ted->tim_undo;
	for (symbol *p = ted_iterat_next(ted, ted->tex); p > ted->tex_1; p = ted_iterat_next(ted, p)) p->col = 0;
	if ((s32)ted->key_words_count < 1) return;
	symbol *p = ted->tex;
	do p = ted_text_find_sel_color(ted, p);
	while (p > ted->tex_1);
	/* words of one colour next to each other made one */
	u16 ax = 0;
	for (p = ted_iterat_next(ted, ted->tex); p > ted->tex_1; p = ted_iterat_next(ted, p)) {
		if (!p->col) continue;
		ax = (ax & 0xff00) | p->col;
		if ((ax >> 8) == (ax & 0xff)) p->col = 0;
		ax <<= 8;
	}
}

/* the key word that starts at p (and ends before end_pos) */
static void ted_find_help_id(text_editor *ted, symbol *p, symbol *end_pos)
{
	ted->help_id = -1;
	s32 word_n = ted->arr_key_pos[p->c];
	if (word_n < 0) return;
	s32 l_pos = word_n;
	char c0 = COL(word_n)->Text[0];
	while (l_pos < (s32)ted->key_words_count && COL(l_pos)->Text[0] == c0) l_pos++;
	p = ted_iterat_next(ted, p);
	for (int pos = 1;; pos++) {
		u8 dl = p->c;
		while (l_pos > word_n && (u8)COL(l_pos - 1)->Text[pos] != dl) l_pos--;
		while (l_pos > word_n && (u8)COL(word_n)->Text[pos] != dl) word_n++;
		if (word_n == l_pos) break;
		p = ted_iterat_next(ted, p);
		if (p == end_pos) break;
	}
	ted->help_id = word_n;
}

static void ted_show_help_f1(text_editor *ted)
{
	symbol *p = ted_get_pos_by_cursor(ted);
	symbol *e = ted_iterat_next_color_tag(ted, p);
	p = ted_iterat_perv_color_tag(ted, p);
	if (e > ted->tex && p > ted->tex_1) ted_find_help_id(ted, p, e);
	ted_draw_help_f1(ted);
}

/* ------------------------------------------------------------------ */
/* files                                                               */

static int ted_file70(void *file, u32 sub, u32 count, void *buf, const char *f_name, u32 *ebx)
{
	u8 *b = file;
	*(u32 *)b = sub;
	*(u32 *)(b + 4) = 0;
	*(u32 *)(b + 8) = 0;
	*(u32 *)(b + 12) = count;
	*(u32 *)(b + 16) = (u32)buf;
	b[20] = 0;
	memcpy(b + 21, &f_name, 4);
	struct kos_r2 r = kos_call2(70, (u32)b, 0, 0, 0, 0);
	if (ebx) *ebx = r.ebx;
	return r.eax;
}

/* the file f_name opened (file: 25 bytes for function 70's block).
 * Returns function 70's error. A file packed by kpack (KPCK) is not
 * unpacked here */
int ted_open_file(text_editor *ted, void *file, const char *f_name)
{
	u32 count, ebx;
	/* the memory checked first, made bigger if too small */
	int err = ted_file70(file, 5, 0, ted->tex, f_name, NULL);
	if (err) {
		count = ted->max_chars;
		if (err != 2) return err;              /* 2: no information on this file system */
	} else {
		s32 room = ted->max_chars - 2;
		count = *(u32 *)((u8 *)ted->tex + 32);   /* the file's size */
		if (!((s32)count < room)) ted_mem_resize(ted, count);
	}
	err = ted_file70(file, 0, count, ted->tex, f_name, &ebx);
	if ((err == 0 || err == 6) && ebx != (u32)-1)
		ted_on_open_file(ted, ebx);
	return err;
}

void ted_save_file(text_editor *ted, void *file, const char *f_name)
{
	u8 *m = malloc(ted->max_chars), *o = m;
	if (!m) return;
	for (symbol *p = ted_iterat_next(ted, ted->tex); p > ted->tex_1; p = ted_iterat_next(ted, p)) *o++ = p->c;
	u32 n = o - m;
	if (n) {
		int err = ted_file70(file, 2, n, m, f_name, NULL);
		ted->err_save = err;
		if (!err) {
			ted->tim_ls = ted->tim_ch;
		} else {
			int c = err < 10 ? '0' + err : '?';
			if (ted->fun_save_err) ted->fun_save_err(c);
		}
	}
	free(m);
}

/* ------------------------------------------------------------------ */
/* the buttons' functions                                              */

void ted_but_select_word(text_editor *ted)
{
	int l, c;
	symbol *p = ted_get_pos_by_cursor(ted);
	symbol *s = ted_iterat_perv_color_tag(ted, p);
	if (s > ted->tex_1) {
		ted_get_text_coords(ted, s, &l, &c);
		ted->sel.x0 = c;
		ted->sel.y0 = l;
	}
	s = ted_iterat_next_color_tag(ted, p);
	if (s > ted->tex_1) {
		ted_get_text_coords(ted, s, &l, &c);
		ted->sel.x1 = c;
		ted->sel.y1 = l;
	}
	panel_buttons(ted);
	ted_draw(ted);
}

void ted_but_cut(text_editor *ted)
{
	ted_but_copy(ted);
	ted_set_undo(ted);
	if (ted_sel_text_del(ted, ted_opt_ed_change_time) == 1) {
		ted_draw(ted);
		panel_buttons(ted);
	}
}

/* the selection into buffer (a clipboard slot: size, type 0 text,
 * encoding 1 cp866, the text with 13 10 line ends) and the clipboard */
void ted_but_copy(text_editor *ted)
{
	if (!ted_is_select(ted)) return;
	ted_sel_normalize(ted);
	symbol *e = ted_get_pos_by_coords(ted, ted->seln.x1, ted->seln.y1);
	symbol *p = ted_get_pos_by_coords(ted, ted->seln.x0, ted->seln.y0);
	u8 *b = (u8 *)ted->buffer;
	u32 n = 12;                                /* the slot's header */
	*(u32 *)(b + 4) = 0;
	*(u32 *)(b + 8) = 1;
	u8 *o = b + 12;
	while (p > ted->tex_1 && p != e) {
		n++;
		if (n + 1 >= ted->buffer_size) break;  /* the buffer full (its 13 10 and 0 kept inside) */
		*o++ = p->c;
		if (p->c == 13) {
			*o++ = 10;                         /* 13 10, as other programs have it */
			n++;
		}
		p = ted_iterat_next(ted, p);
	}
	*o = 0;
	if (o == b + 12) return;
	n = o - b;
	*(u32 *)b = n;
	bl_clip_write(b, n);
	ted_draw_buffer(ted);
	panel_buttons(ted);
}

void ted_but_paste(text_editor *ted)
{
	const char *t = NULL;
	u32 len = 0;
	struct bl_clip *slot = NULL;
	int n = bl_clip_count();
	for (int i = n - 1; n >= 1 && i >= 0; i--) {
		/* the slots from the last: the first of text in cp866 */
		struct bl_clip *c = bl_clip_read(i);
		if ((u32)c == 1 || (u32)c == (u32)-1 || !c) break;
		u32 sz = c->cbSize;
		if ((s32)sz < 1) { free(c); break; }
		if (c->contentType != 0 || c->encoding != 1) {
			free(c);
			if (i >= 1) continue;
			break;
		}
		/* a copy in the editor's buffer */
		u32 k = sz > ted->buffer_size ? ted->buffer_size : sz;
		*(u32 *)ted->buffer = k;
		memcpy(ted->buffer + 4, (u8 *)c + 4, k - 4);
		if (k < ted->buffer_size) ted->buffer[k] = 0;
		slot = c;
		t = c->data_;
		len = strnlen(t, sz > 12 ? sz - 12 : 0);   /* the slot need not end with a 0 */
		break;
	}
	if (!t) {
		/* the clipboard empty: the editor's own buffer */
		if ((s32)*(u32 *)ted->buffer < 1) return;
		t = ted->buffer + 12;
		len = strlen(t);
	}
	if ((s32)len >= 1) {
		ted_set_undo(ted);
		u32 opt = ted_opt_ed_change_time | ted_opt_ed_move_cursor;
		if (ted_sel_text_del(ted, opt) == 1) opt = ted_opt_ed_move_cursor;
		ted_text_add(ted, t, len, opt);
		ted_draw(ted);
		panel_buttons(ted);
	}
	free(slot);                                /* the original keeps it */
}

void ted_but_sumb_upper(text_editor *ted)
{
	if (ted_convert_sel_text(ted, fb_char_toupper)) ted_draw(ted);
}

void ted_but_sumb_lover(text_editor *ted)
{
	if (ted_convert_sel_text(ted, fb_char_todown)) ted_draw(ted);
}

void ted_but_reverse(text_editor *ted)
{
	if (ted_is_select(ted)) {
		ted_sel_normalize(ted);
		symbol *a = ted_get_pos_by_coords(ted, ted->seln.x0, ted->seln.y0);
		int x1 = ted->seln.x1;
		if (x1) x1--;
		symbol *b = ted_get_pos_by_coords(ted, x1, ted->seln.y1);
		ted_revers(ted, a, b);
	}
	ted_draw(ted);
}

void ted_but_undo(text_editor *ted)
{
	if (ted->tim_ch > ted->tim_undo) {
		ted->tim_undo++;
		ted_draw(ted);
		panel_buttons(ted);
	}
}

void ted_but_redo(text_editor *ted)
{
	if (ted->tim_undo >= 1) {
		ted->tim_undo--;
		ted_draw(ted);
		panel_buttons(ted);
	}
}

/* buffer_find compared with the text from p on: 1 if it is there, *last
 * its last character */
static int ted_get_find_rezult(text_editor *ted, u32 opt, const u8 *t, symbol *p, symbol **last)
{
	int any_case = opt & 0x40000000;
	u8 al = *t;
	if (any_case) al = fb_char_toupper(al);
	for (;;) {
		u8 cl = al;
		al = p->c;
		if (any_case) al = fb_char_toupper(al);
		if (al != cl) return 0;
		al = *++t;
		if (!al) { *last = p; return 1; }      /* the end of the text to find */
		if (any_case) al = fb_char_toupper(al);
		p = ted_iterat_next(ted, p);
		if (!(p > ted->tex_1)) return 0;
	}
}

/* the found text selected, the cursor after it */
static int _but_find_select(text_editor *ted, int found, symbol *first, symbol *last)
{
	if (!found) return 0;
	int l, c;
	ted_get_text_coords(ted, last, &l, &c);
	c++;                                       /* the cursor right */
	ted->sel.x1 = c;
	ted->sel.y1 = l;
	ted_go_to_pos(ted, c, l);
	ted_get_text_coords(ted, first, &l, &c);
	ted->sel.x0 = c;
	ted->sel.y0 = l;
	return 1;
}

static int ted_find_from(text_editor *ted, u32 f_opt, symbol *p, int backwards)
{
	symbol *last = NULL;
	for (;;) {
		if (ted_get_find_rezult(ted, f_opt, (const u8 *)ted->buffer_find, p, &last))
			return _but_find_select(ted, 1, p, last);
		p = backwards ? ted_iterat_perv(ted, p) : ted_iterat_next(ted, p);
		if (p <= ted->tex_1) return 0;
	}
}

/* from the start, or from the selection's end */
static int _but_find_first(text_editor *ted, u32 f_opt)
{
	symbol *p;
	if (ted_is_select(ted)) {
		ted_sel_normalize(ted);
		ted_go_to_pos(ted, ted->sel.x1, ted->sel.y1);
		p = ted_get_pos_by_cursor(ted);
	} else {
		p = ted_iterat_next(ted, ted->tex);
	}
	return ted_find_from(ted, f_opt, p, 0);
}

/* above the cursor */
static int _but_find_perv(text_editor *ted, u32 f_opt)
{
	symbol *p;
	if (ted_is_select(ted)) {
		ted_sel_normalize(ted);
		ted_go_to_pos(ted, ted->sel.x0, ted->sel.y0);
		p = ted_iterat_perv(ted, ted_get_pos_by_cursor(ted));   /* before the selection */
	} else {
		p = ted_get_pos_by_cursor(ted);
	}
	return ted_find_from(ted, f_opt, p, 1);
}

/* below the cursor */
static int _but_find_next(text_editor *ted, u32 f_opt)
{
	return ted_find_from(ted, f_opt, ted_get_pos_by_cursor(ted), 0);
}

int ted_but_find(text_editor *ted, u32 f_opt)
{
	int found;
	switch ((u8)f_opt) {
	case 2: found = _but_find_first(ted, f_opt); break;
	case 0: found = _but_find_next(ted, f_opt); break;
	case 1: found = _but_find_perv(ted, f_opt); break;
	default: return 0;
	}
	if (f_opt & 0x80000000u) return found;
	if (found) ted_draw(ted);                  /* found: the window again */
	else if (ted->fun_find_err) ted->fun_find_err();
	return found;
}

/* the text found replaced; 1 if replaced */
int ted_but_replace(text_editor *ted, const char *rpl_text, u32 r_opt, int n_tim)
{
	if (!ted_but_find(ted, r_opt | 0x80000000u)) return 0;
	u32 opt = 0;
	if (n_tim) {
		ted_set_undo(ted);
		opt = ted_opt_ed_change_time;
	}
	ted_sel_text_del(ted, opt);
	u32 len = strlen(rpl_text);
	if (!len) return 0;
	ted_text_add(ted, rpl_text, len, ted_opt_ed_move_cursor);
	return 1;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */

static void ted_key_ctrl_home(text_editor *ted)
{
	ted->cur_x = ted->cur_y = 0;
	ted->scr_w->position = 0;
	ted->scr_h->position = 0;
	ted_draw(ted);
	panel_buttons(ted);
}

static void ted_key_ctrl_end(text_editor *ted)
{
	scrollbar *w = ted->scr_w;
	s32 n = ted_get_num_lines(ted);
	w->position = n;                           /* the last line */
	if (n > (s32)w->cur_area) n = w->cur_area;
	w->position -= n;                          /* ... a page of it seen */
	ted->cur_y = n - 1;
	ted_cur_move_x_last_char(ted);
	ted_draw(ted);
	panel_buttons(ted);
}

static void ted_sel_key_up(text_editor *ted)
{
	if (ted->drag_k != 1) {
		ted_sel_start(ted);
		ted->drag_k = 1;
	}
	if (ted_cur_move_up(ted) == 8) ted_scroll_set_redraw(ted);
	ted_sel_move(ted);
	ted_draw(ted);
}

static void ted_sel_key_down(text_editor *ted)
{
	if (ted->drag_k != 1) {
		ted_sel_start(ted);
		ted->drag_k = 1;
	}
	if (ted_cur_move_down(ted) == 8) ted_scroll_set_redraw(ted);
	ted_sel_move(ted);
	ted_draw(ted);
}

static void ted_sel_key_lr(text_editor *ted, int right)
{
	if (ted->drag_k != 1) ted_sel_start(ted);
	int dl = right ? ted_cur_move_right(ted) : ted_cur_move_left(ted);
	ted_sel_move(ted);
	if (ted->drag_k != 1) {
		ted->drag_k = 1;
		dl = 8;
	}
	if (dl == 8) {
		ted_scroll_set_redraw(ted);
		ted_draw(ted);
	} else {
		ted_draw_cur_line(ted);
	}
}

/* KeyConvertToASCII: ah, the scan code, to the layout's character */
static u32 KeyConvertToASCII(u32 key, const u8 *table)
{
	u8 sc = key >> 8;
	return (key & 0xffff0000u) | (u32)table[sc] << 8 | sc;
}

/* a cursor key: drawn again where it moved */
static void ted_move_key(text_editor *ted, int (*move)(text_editor *))
{
	ted_draw_cursor_sumb(ted);
	if (move(ted) == 8) {
		ted_scroll_set_redraw(ted);
		ted_draw(ted);
	} else {
		ted_sel_end(ted);
	}
}

void ted_key(text_editor *ted, u32 key, const u8 *table, u32 control)
{
	if (*ted->el_focus != ted) return;         /* not in focus */
	u8 ah = key >> 8;
	if (ah == KEY_F1) { ted_show_help_f1(ted); return; }
	if (ah == KEY_F3) { ted_but_find(ted, 0); return; }
	if (control & KM_CTRL) {
		/* the program's functions: open, save, find, ... */
		if (ted->fun_on_key_ctrl_all) {
			u32 al = 0;
			switch (ah) {
			case 24: al = 'O'; break;
			case 31: al = 'S'; break;
			case 33: al = 'F'; break;
			case 34: al = 'G'; break;
			case 35: al = 'H'; break;
			case 49: al = 'N'; break;
			}
			if (al) {
				if (control & KM_SHIFT) al |= 0x100;
				ted->fun_on_key_ctrl_all(al);
				return;
			}
		}
		/* the editor's own */
		switch (ah) {
		case 30: ted_sel_all(ted); break;      /* Ctrl+A */
		case 44: ted_but_undo(ted); break;     /* Ctrl+Z */
		case 21: ted_but_redo(ted); break;     /* Ctrl+Y (not in the original) */
		case 45: ted_but_cut(ted); break;      /* Ctrl+X */
		case 46: ted_but_copy(ted); break;     /* Ctrl+C */
		case 47: ted_but_paste(ted); break;    /* Ctrl+V */
		case 199: ted_key_ctrl_home(ted); break;   /* Ctrl+Home (its release) */
		case 207: ted_key_ctrl_end(ted); break;    /* Ctrl+End */
		}
		return;
	}
	if (control & KM_SHIFT) {
		if (ah == 72) ted_sel_key_up(ted);
		if (ah == 75) ted_sel_key_lr(ted, 0);
		if (ah == 77) ted_sel_key_lr(ted, 1);
		if (ah == 80) ted_sel_key_down(ted);
	} else {
		if (ah == 72) ted_move_key(ted, ted_cur_move_up);
		if (ah == 80) ted_move_key(ted, ted_cur_move_down);
		if (ah == 75) ted_move_key(ted, ted_cur_move_left);
		if (ah == 77) ted_move_key(ted, ted_cur_move_right);
		if (ah == 71) ted_move_key(ted, ted_cur_move_x_first_char);   /* Home */
		if (ah == 79) ted_move_key(ted, ted_cur_move_x_last_char);    /* End */
		if (ah == 73 && ted_cur_move_page_up(ted)) {                  /* Page Up */
			ted_scroll_set_redraw(ted);
			ted_draw(ted);
			ted->drag_k = 0;
		}
		if (ah == 81 && ted_cur_move_page_down(ted)) {                /* Page Down */
			ted_scroll_set_redraw(ted);
			ted_draw(ted);
			ted->drag_k = 0;
		}
	}
	/* .key_MoveCur: the keys that would give stray characters */
	if (ah == 42 || ah == 54 || ah == 58 || ah == 69 || ah == 120 || ah > 0x80) return;
	if (table) {
		key = KeyConvertToASCII(key, table);
		ah = key >> 8;
	}
	if (EvChar[ah] == 1) {
		ted->key_new = ah;
		ted_set_undo(ted);
		u32 opt = ted_opt_ed_change_time | ted_opt_ed_move_cursor;
		if (ted_sel_text_del(ted, opt) == 1) opt = ted_opt_ed_move_cursor;
		if (ted->cur_ins != 1) {               /* overwriting */
			ted_text_del(ted, ted_opt_ed_change_time);
			opt = ted_opt_ed_move_cursor;
		}
		ted_text_add(ted, (const char *)&ted->key_new, 1, opt);
		if (ted->key_new == 13) {
			/* a new line */
			scrollbar *w = ted->scr_w;
			w->max_area++;
			if (!(ted->cur_y < (s32)w->cur_area)) {
				ted->cur_y--;                  /* the cursor stays */
				w->position++;                 /* the runner moves */
			}
			ted_draw(ted);
		} else {
			ted_draw_cur_line(ted);
		}
		panel_buttons(ted);
	}
	if (ah == 8) {                             /* Backspace */
		ted_set_undo(ted);
		if (ted_sel_text_del(ted, ted_opt_ed_change_time) != 1)
			ted_text_del(ted, ted_opt_ed_change_time | ted_opt_ed_move_cursor);
		ted_draw(ted);
		panel_buttons(ted);
		return;
	}
	if (ah == 182) {                           /* Delete */
		ted_set_undo(ted);
		if (ted_sel_text_del(ted, ted_opt_ed_change_time) != 1)
			ted_text_del(ted, ted_opt_ed_change_time);
		ted_draw(ted);
		panel_buttons(ted);
		return;
	}
	if (ah == 185) {                           /* Insert */
		ted_draw_cursor_sumb(ted);
		ted->cur_ins ^= 1;
		ted_draw_main_cursor(ted);
	}
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

static void text1(u32 ebx, u32 ecx, const void *s)
{
	kos_call(SF_DRAW_TEXT, ebx, ecx, (u32)s, 1, 0);
}

/* the colour of the text at p (the colour tag before it) */
static u32 ted_get_symb_color(text_editor *ted, symbol *p)
{
	u32 c = ted->color_wnd_text;
	if (ted->mode_color && p) {
		for (;;) {
			u32 col = p->col;
			if (col) {
				if (col < ted->colors_text_count) c = ted->text_colors[col];
				break;
			}
			p = ted_iterat_perv(ted, p);
			if (p <= ted->tex_1) break;
		}
	}
	return c | ted->font_size;
}

/* the character to draw for p: tabs and line ends as signs or blanks */
static const u8 *ted_convert_invis_symb(text_editor *ted, const symbol *p)
{
	if (ted->mode_invis == 1) {
		if (p->c == 9) return &ted_symbol_tab;
		if (p->c == 13) return &ted->symbol_new_line;
		return &p->c;
	}
	if (p->c == 9 || p->c == 13) return &ted_symbol_space;
	return &p->c;
}

/* the cell under the cursor drawn without it */
static void ted_draw_cursor_sumb(text_editor *ted)
{
	u32 ebx = (ted->wnd.left + ted->rec.left + ted->cur_x * ted->rec.width) << 16;
	ebx += ted->rec.width;
	u32 ecx = (ted->wnd.top + ted->rec.top + ted->cur_y * ted->rec.height) << 16;
	ecx += ted->rec.height;
	u32 edx = ted->color_wnd_work;
	ted_sel_normalize(ted);
	ted->seln.y0 -= ted->scr_w->position;
	ted->seln.y1 -= ted->scr_w->position;
	int y = ted->cur_y;
	if (y >= ted->seln.y0 && y <= ted->seln.y1) {
		edx = ted->color_select;
		int x = ted->cur_x + ted->scr_h->position;
		if (y == ted->seln.y0 && x < ted->seln.x0) edx = ted->color_wnd_work;
		if (y == ted->seln.y1 && x >= ted->seln.x1) edx = ted->color_wnd_work;
	}
	bl_rect(ebx, ecx, edx);
	symbol *p = ted_get_pos_by_cursor(ted);
	if (ted->gp_opt == 2) {
		ROR16(ecx);
		SETLO(ebx, ecx);
		ebx += 0x10001;
		text1(ebx, ted_get_symb_color(ted, p), ted_convert_invis_symb(ted, p));
	}
}

static void ted_draw_buffer(text_editor *ted)
{
	if ((s32)*(u32 *)ted->buffer < 1) return;
	u32 ebx = ted->rec.left;
	SETLO(ebx, LO(ebx) + 300);
	if ((s32)ebx >= (s32)ted->wnd.width) return;   /* no room for it */
	ebx += ted->wnd.left;
	ebx = (ebx << 16) + ted->wnd.top + 3;
	u32 ecx = ted->color_wnd_bord | 0x40000000u;
	const char *s = ted->buffer + 12;
	u32 n = strlen(s);
	if (n > 20) n = 20;                        /* cut to 20 characters */
	kos_call(SF_DRAW_TEXT, ebx, ecx, (u32)s, n, ted->color_wnd_work);
	ebx -= 50 << 16;
	kos_call(SF_DRAW_TEXT, ebx, ecx ^ 0x40000000u, (u32)txtBuf, strlen(txtBuf), 0);
}

static void ted_draw_help_f1(text_editor *ted)
{
	if (!((s32)ted->rec.top > 13)) return;     /* the header too low for it */
	u32 ebx = ((ted->wnd.left + ted->rec.left) << 16) + ted->wnd.width - ted->rec.left;
	u32 ecx = ((ted->wnd.top + 13) << 16) + 9; /* 9: font 0's height */
	bl_rect(ebx, ecx, ted->color_wnd_capt);
	if (ted->help_id == -1) return;
	TexColViv *k = COL(ted->help_id);
	ebx = ((ted->wnd.left + ted->rec.left) << 16) + ted->wnd.top + 13;
	ecx = ted->text_colors[k->color] | 0xc0000000u;
	kos_call(SF_DRAW_TEXT, ebx, ecx, (u32)k->Text, 0, ted->color_wnd_work);
	u32 n = strlen(k->Text);
	if (!k->f1) return;
	ebx += ((n + 1) * 6) << 16;                /* the system font's width */
	kos_call(SF_DRAW_TEXT, ebx, ted->color_wnd_bord | 0x80000000u, (u32)(ted->help_text_f1 + k->f1), 0, 0);
}

static void ted_draw_line_numbers(text_editor *ted)
{
	/* the top panel */
	bl_rect((ted->wnd.left << 16) + ted->wnd.width, (ted->wnd.top << 16) + ted->rec.top, ted->color_wnd_capt);
	/* the line numbers */
	u32 ecx = ted->scr_w->position + 1;
	u32 edx = ((3 + ted->wnd.left) << 16) + ted->wnd.top + ted->rec.top;
	for (;;) {
		bl_rect((ted->wnd.left << 16) + ted->rec.left, (u32)LO(edx) << 16 | (u16)ted->rec.height, ted->color_wnd_capt);
		kos_call(47, 0x40000, ecx, edx, ted->color_wnd_bord, 0);
		ecx++;
		edx += ted->rec.height;
		edx -= ted->wnd.top;
		if ((s32)(edx & 0xffff) >= (s32)ted->wnd.height) break;
		edx += ted->wnd.top;
	}
}

static void ted_draw_main_cursor(text_editor *ted)
{
	u32 ecx = (ted->wnd.top + ted->rec.top + ted->cur_y * ted->rec.height) << 16;
	ecx += ted->rec.height;
	u32 ebx = (ted->wnd.left + ted->rec.left + ted->cur_x * ted->rec.width) << 16;
	ebx += ted->rec.width;
	if (ted->cur_ins == 1) SETLO(ebx, LO(ebx) >> 2);   /* a narrow cursor */
	bl_rect(ebx, ecx, ted->color_cursor);
	symbol *p = ted_get_pos_by_cursor(ted);
	if (ted->gp_opt == 2) {
		ROR16(ecx);
		SETLO(ebx, ecx);
		ebx += 0x10001;
		text1(ebx, ted->color_cur_text | ted->font_size, ted_convert_invis_symb(ted, p));
	}
	/* the header: the line, the column, the changes undone */
	ebx = ((ted->wnd.left + ted->rec.left) << 16) + ted->wnd.top + 3;
	u32 c = ted->color_wnd_bord | 0x80000000u;
	bl_text(ebx, c, txtRow, 0);
	ebx += 0x500000;
	bl_text(ebx, c, txtCol, 0);
	if (ted->tim_undo) bl_text(ebx + 0x500000, c, txtOtm, 0);
	ted_draw_buffer(ted);
	ted_draw_help_f1(ted);
	u32 esi = ted->color_wnd_bord | 0x40000000u, edx = ebx;
	ROR16(edx); edx -= 35; ROR16(edx);
	kos_call(47, 0x40000, ted->scr_w->position + ted->cur_y + 1, edx, esi, ted->color_wnd_work);
	edx += 0x500000;
	kos_call(47, 0x40000, ted->scr_h->position + ted->cur_x + 1, edx, esi, ted->color_wnd_work);
	if (ted->tim_undo) {
		edx += 0x500000;
		kos_call(47, 0x40000, ted->tim_undo, edx, esi, ted->color_wnd_work);
	}
}

/* a line's background (or the selection's) before its text; clear_o 0:
 * all the lines below to the window's bottom */
static void ted_clear_line_before_draw(text_editor *ted, u32 ebx, int clear_o, s32 numb_lin)
{
	ebx -= 0x10001;                            /* the centring taken back */
	if (!clear_o) {
		ebx += ted->rec.height;
		ROR16(ebx);
		SETLO(ebx, 0);
		ebx += ted->wnd.left + ted->rec.left;
		ROR16(ebx);
	}
	u32 eax = ted->wnd.height + ted->wnd.top;
	if ((s16)LO(eax) < (s16)LO(ebx)) return;
	SETLO(eax, LO(eax) - LO(ebx));
	u32 ecx = (u32)LO(ebx) << 16;
	SETLO(ebx, 0);
	ebx += ted->wnd.width - ted->rec.left;
	ecx += ted->rec.height;
	u32 edx = ted->color_wnd_work;
	if (!clear_o || (s16)LO(eax) < (s16)LO(ecx)) {
		SETLO(ecx, eax);                       /* to the bottom: the background only */
	} else if (ted_is_select(ted) && !(ted->seln.y0 > numb_lin) && !(ted->seln.y1 < numb_lin)) {
		edx = ted->color_select;               /* a selected line */
	}
	bl_rect(ebx, ecx, edx);
	if (!ted_is_select(ted)) return;
	SETLO(ecx, 0);
	ecx += ted->rec.height;
	s32 pos = ted->scr_h->position;
	if (ted->seln.y0 == numb_lin) {
		/* the first line: its part before the selection */
		u32 b = ebx;
		s32 x = ted->seln.x0;
		if (x > pos) SETLO(b, (x - pos) * ted->rec.width);
		else SETLO(b, 0);
		bl_rect(b, ecx, ted->color_wnd_work);
	}
	if (ted->seln.y1 == numb_lin) {
		/* the last line: its part after the selection */
		s32 x = ted->seln.x1;
		if (x > pos) {
			u32 d = (x - pos) * ted->rec.width;
			SETLO(ebx, LO(ebx) - LO(d));
			ebx += d << 16;
		}
		bl_rect(ebx, ecx, ted->color_wnd_work);
	}
}

/* the first character seen of a line scrolled horizontally (the one
 * before it), and its colour */
static symbol *ted_opt_draw_line_left(text_editor *ted, symbol *p, u32 *color)
{
	s32 n = ted->scr_h->position;
	if (n) {
		symbol *last = p;
		if (p == ted->tex) p = ted_iterat_next(ted, p);
		else if (p->c == 13) p = ted_iterat_next(ted, p);
		while (p > ted->tex_1 && p->c != 13 && n) {
			last = p;
			p = ted_iterat_next(ted, p);
			n--;
		}
		p = last;
	}
	*color = ted_get_symb_color(ted, p);
	return p;
}

/* the last character of a line, past the window's right edge */
static symbol *ted_opt_draw_line_right(text_editor *ted, symbol *p, u32 *color)
{
	symbol *last = p;
	while (p > ted->tex_1 && p->c != 13) {
		last = p;
		p = ted_iterat_next(ted, p);
	}
	*color = ted_get_symb_color(ted, last);
	return last;
}

/* the character before the first one seen, or NULL */
static symbol *ted_get_first_visible_pos(text_editor *ted, u32 *color)
{
	scrollbar *w = ted->scr_w;
	symbol *p = ted->tex;
	u32 n = 0;
	while (n != w->position) {
		p = ted_iterat_next(ted, p);
		if (p <= ted->tex_1) break;
		if (p->c == 13) n++;
	}
	if (n != w->position) p = NULL;
	if ((s32)n > (s32)w->max_area) w->max_area = n;
	if (!p) {
		*color = ted->color_wnd_text | ted->font_size;
		return NULL;
	}
	return ted_opt_draw_line_left(ted, p, color);
}

static void ted_draw_cur_line(text_editor *ted)
{
	u32 ebx = (ted->wnd.left + ted->rec.left) << 16;
	SETLO(ebx, ted->cur_y * ted->rec.height);
	ebx += ted->wnd.top + ted->rec.top;
	ebx += 0x10001;                            /* the letters centred */
	ted_sel_normalize(ted);
	s32 line = ted->cur_y + ted->scr_w->position;
	ted_clear_line_before_draw(ted, ebx, 1, line);
	symbol *p = ted_get_pos_by_coords(ted, ted->scr_h->position, line);
	if (ted->gp_opt == 2) {
		u32 ecx = ted_get_symb_color(ted, p);
		while (p > ted->tex_1) {
			if (ted->mode_color && p->col) ecx = ted_get_symb_color(ted, p);
			if (p->c == 13) {
				if (ted->mode_invis == 1) text1(ebx, ecx, &ted->symbol_new_line);
				break;
			}
			text1(ebx, ecx, &p->c);
			ROR16(ebx);
			ebx += ted->rec.width;
			if ((s16)LO(ebx) >= (s16)(ted->wnd.width + ted->wnd.left)) break;
			ROR16(ebx);
			p = ted_iterat_next(ted, p);
		}
	}
	ted_draw_main_cursor(ted);
}

void ted_draw(text_editor *ted)
{
	u32 ecx;
	u32 ebx = (ted->wnd.left + ted->rec.left) << 16;
	ebx += ted->wnd.top + ted->rec.top;
	ebx += 0x10001;                            /* the letters centred */
	ted_sel_normalize(ted);                    /* for the selection */
	s32 line_num = ted->scr_w->position;
	ted_clear_line_before_draw(ted, ebx, 1, line_num);
	symbol *p = ted_get_first_visible_pos(ted, &ecx);
	if (p) {
		for (;;) {
			p = ted_iterat_next(ted, p);
			if (p <= ted->tex_1) break;
			/* the colours */
			if (ted->mode_color && p->col) ecx = ted_get_symb_color(ted, p);
			if (p->c == 13) {
				if (ted->mode_invis == 1) text1(ebx, ecx, &ted->symbol_new_line);
				ebx += ted->rec.height;
				if ((s16)LO(ebx) > (s16)(ted->wnd.height + ted->wnd.top)) break;
				ebx &= 0xffff;
				ROR16(ebx);
				ebx += ted->wnd.left + ted->rec.left + 1;
				ROR16(ebx);
				line_num++;
				ted_clear_line_before_draw(ted, ebx, 1, line_num);
				p = ted_opt_draw_line_left(ted, p, &ecx);
				continue;
			}
			text1(ebx, ecx, &p->c);
			ROR16(ebx);
			ebx += ted->rec.width;
			if ((s16)LO(ebx) >= (s16)(ted->wnd.left + ted->wnd.width))
				p = ted_opt_draw_line_right(ted, p, &ecx);
			ROR16(ebx);
		}
	}
	line_num++;
	ted_clear_line_before_draw(ted, ebx, 0, line_num);
	ted_draw_line_numbers(ted);
	ted_draw_main_cursor(ted);
	/* the scroll bars, all of them (all_redraw) */
	scrollbar *h = ted->scr_h, *w = ted->scr_w;
	h->all_redraw = w->all_redraw = 1;
	scrollbar_h_draw(h);
	scrollbar_v_draw(w);
	h->all_redraw = w->all_redraw = 0;
	/* the squares at the bottom left and right */
	ebx = (ted->wnd.left << 16) + ted->rec.left;
	ecx = (ted->wnd.top + ted->wnd.height) << 16 | (u16)(h->y_size + 1);
	bl_rect(ebx, ecx, ted->color_wnd_capt);
	ebx = (ted->wnd.left + ted->wnd.width) << 16 | (u16)(w->x_size + 1);
	bl_rect(ebx, ecx, ted->color_wnd_capt);
	if (ted->fun_draw_panels) ted->fun_draw_panels(ted);
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */

/* a click in the text: the focus, the cursor, a selection */
static void ted_wnd_main_click(text_editor *ted, u32 pos)
{
	*ted->el_focus = ted;
	u32 x = (pos >> 16) - ted->wnd.left - ted->rec.left;
	x = (u16)x / (u16)ted->rec.width;
	if ((s32)x > (s32)ted->scr_h->cur_area) x = ted->scr_h->cur_area;
	ted->cur_x = x;
	u32 y = (pos & 0xffff) - ted->wnd.top - ted->rec.top;
	y = (u16)y / (u16)ted->rec.height + 1;
	if ((s32)y > (s32)ted->scr_w->cur_area) y = ted->scr_w->cur_area;
	ted->cur_y = y - 1;
	if (ted->drag_m) {
		ted_sel_move(ted);
	} else {
		ted->drag_m = 1;
		ted_sel_start(ted);
	}
}

/* the wheel: 3 lines a step */
static void ted_wnd_main_mouse_scroll(text_editor *ted)
{
	s16 v = bl_mouse_scroll();
	if (!v) return;
	scrollbar *w = ted->scr_w;
	u32 np = v * 3 + w->position;
	u32 lim = (w->max_area * 2 - w->cur_area) >> 1;   /* half the window past the end */
	if (np >= lim) return;
	s32 y = ted->cur_y - np + w->position;
	if (y < 0) y = 0;                          /* the cursor above: the top line */
	y++;
	if (y > (s32)w->cur_area) y = w->cur_area; /* below: the bottom line */
	ted->cur_y = y - 1;
	w->position = np;
	ted_draw(ted);
}

void ted_mouse(text_editor *ted)
{
	scrollbar *h = ted->scr_h, *w = ted->scr_w;
	/* the scroll bars */
	if (!h->delta2 && w->max_area > w->cur_area) {
		scrollbar_v_mouse(w);
		if (w->redraw) {
			w->redraw = 0;
			ted_draw(ted);
			return;
		}
		if (w->delta2) return;
	}
	if (h->max_area > h->cur_area) {
		scrollbar_h_mouse(h);
		if (h->redraw) {
			h->redraw = 0;
			ted_draw(ted);
			return;
		}
	}
	if (w->delta2 || h->delta2) return;
	/* the text */
	if ((u8)bl_mouse_buttons() == 1) {
		u32 pos = bl_mouse_pos();
		s16 my = pos, mx = pos >> 16;
		if (my >= (s16)(ted->wnd.top + ted->rec.top) && !((s16)(ted->wnd.top + ted->wnd.height) < my) &&
		    mx >= (s16)(ted->wnd.left + ted->rec.left) && !((s16)(ted->wnd.left + ted->wnd.width) < mx)) {
			ted_draw_cursor_sumb(ted);
			ted_wnd_main_click(ted, pos);
			return;
		}
	}
	if (*ted->el_focus == ted) ted_wnd_main_mouse_scroll(ted);   /* the wheel */
	if (ted->drag_m) {
		ted->drag_m = 0;
		ted_draw(ted);
		panel_buttons(ted);
	}
}
