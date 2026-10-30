/* KolibriOS for ARM: box_lib, KolibriOS's library of window components,
 * translated to C from programs/develop/libraries/box_lib (box_lib.mac,
 * editbox.asm, checkbox.asm, optionbox.asm, scrollbar.asm, tree_list.asm,
 * t_edit.asm) by Alexey Teplov <Lrz>, Marat Zakiyanov (Mario79), Evtikhov
 * Maxim (Maxxxx32), Eugene Grechnikov (Diamond), hidnplayr, Igor
 * Afanasiev (IgorA) and the KolibriOS team. GPL-2.0.
 *
 * KolibriOS loads box_lib.obj as a shared library and its functions are
 * stdcall procedures taking a pointer to the component's structure; here
 * the library is linked into the program (lib/boxlib, LIBS_name :=
 * boxlib) and the functions are C functions with the same names and
 * arguments. The structures keep box_lib.mac's field names, order and
 * offsets (checked below), so that programs in assembler can share them.
 *
 * A function that took the key code in eax besides its stack argument
 * (edit_box_key) takes it as a second argument here. */
#ifndef BOX_LIB_H
#define BOX_LIB_H

#include <stddef.h>

typedef unsigned char bl_u8;
typedef unsigned short bl_u16;
typedef unsigned int bl_u32;

/* KolibriOS's system colours (function 48.3), macros.inc's
 * struct system_colors */
#ifndef SYSTEM_COLORS_DEFINED
#define SYSTEM_COLORS_DEFINED
typedef struct system_colors {
	bl_u32 frame;            /* nonset1 */
	bl_u32 grab;             /* nonset2 */
	bl_u32 work_dark;
	bl_u32 work_light;
	bl_u32 grab_text;        /* window title */
	bl_u32 work;
	bl_u32 work_button;
	bl_u32 work_button_text;
	bl_u32 work_text;
	bl_u32 work_graph;
} system_colors;
#endif

/* ------------------------------------------------------------------ */
/* EditBox (editbox.asm)                                               */

typedef struct edit_box {
	bl_u32 width;              /* +0  field width */
	bl_u32 left;               /* +4  left border X */
	bl_u32 top;                /* +8  top border Y */
	bl_u32 color;              /* +12 field colour */
	bl_u32 shift_color;        /* +16 selection colour */
	bl_u32 focus_border_color; /* +20 border colour, active */
	bl_u32 blur_border_color;  /* +24 border colour, inactive */
	bl_u32 text_color;         /* +28 colour and font (as in function 4) */
	bl_u32 max;                /* +32 characters at most */
	char *text;                /* +36 the buffer: max + 2 bytes */
	void *mouse_variable;      /* +40 a variable: the box holding the mouse */
	bl_u32 flags;              /* +44 ed_* below */
	bl_u32 size;               /* +48 characters now */
	bl_u32 pos;                /* +52 the caret's position */
	bl_u32 offset;             /* +56 the first visible character */
	bl_u16 cl_curs_x;          /* +60 the cursor's X */
	bl_u16 cl_curs_y;          /* +62 the cursor's Y */
	bl_u16 shift;              /* +64 the selection's other end */
	bl_u16 shift_old;          /* +66 (visible characters) */
	bl_u32 height;             /* +68 */
	bl_u32 char_width;         /* +72 */
} edit_box;

#define ed_struc_size 76

/* flags (box_lib.mac's names, and editbox.asm's ES.* in comments) */
#define ed_pass         1      /* ES.PASSWORD */
#define ed_focus        2      /* ES.FOCUS */
#define ed_shift        4      /* ES.SELECT: text selected */
#define ed_shift_on     8      /* ES.SHIFT_ON: Shift held */
#define ed_shift_bac    16     /* ES.DRAWN_ONCE */
#define ed_left_fl      32
#define ed_offset_fl    64
#define ed_insert       128    /* ES.INSERT_MODE: overwrite */
#define ed_mouse_on     256    /* ES.MOUSE_ON: the box holds the mouse */
#define ed_ctrl_on      512    /* ES.CTRL_ON */
#define ed_alt_on       0x400  /* ES.ALT_ON */
#define ed_disabled     0x800  /* ES.DISABLED */
#define ed_always_focus 0x4000 /* ES.ALWAYS_FOCUS */
#define ed_figure_only  0x8000 /* ES.NUMERIC: digits only */
#define ed_mous_adn_b   (ed_mouse_on | ed_shift_on | ed_shift_bac)
#define ed_shift_off    (~ed_shift)
#define ed_shift_mcl    (~ed_shift)
#define ed_shift_on_off (~ed_shift_on)
#define ed_shift_bac_cl (~ed_shift_bac)
#define ed_shift_cl     (~(ed_shift | ed_shift_on | ed_shift_bac))
#define ed_right_fl     (~ed_left_fl)
#define ed_offset_cl    (~ed_offset_fl)
#define ed_insert_cl    (~ed_insert)
#define ed_mouse_on_off (~ed_mouse_on)
#define ed_ctrl_off     (~ed_ctrl_on)
#define ed_alt_off      (~ed_alt_on)

/* box_lib.mac's struc edit_box as an initializer:
 * edit_box e = EDIT_BOX(150, 0, 20, 0xffffff, 0xff80, 0xff0000, 0xff, 0x4080, 300, buf, &mouse_dd, 0, 0, 0); */
#define EDIT_BOX(width, left, top, color, shift_color, focus_border_color, blur_border_color, text_color, \
		 max, text, mouse_variable, flags, size, pos) \
	{ width, left, top, color, shift_color, focus_border_color, blur_border_color, text_color, \
	  max, text, mouse_variable, flags, size, pos, 0, 0, 0, 0, 0, 0, 0 }

void edit_box_draw(edit_box *e);
/* key: function 2's result (al = 0, ah = the character, bits 16-23 the
 * scan code), as the original takes it in eax */
void edit_box_key(edit_box *e, bl_u32 key);
void edit_box_key_safe(edit_box *e, bl_u32 key);
void edit_box_mouse(edit_box *e);
void edit_box_set_text(edit_box *e, const char *text);

/* box_lib.mac's edit_boxes_set_sys_color: the boxes e[0..n-1] in the
 * system's colours */
static inline void edit_boxes_set_sys_color(edit_box *e, int n, const system_colors *sc)
{
	bl_u32 graph = (sc->work_graph >> 1) & 0x7f7f7f;
	bl_u32 blur = ((sc->work >> 1) & 0x7f7f7f) + graph;
	for (int i = 0; i < n; i++) {
		e[i].focus_border_color = graph;
		e[i].blur_border_color = blur;
		e[i].shift_color = 0xa4c4e4;
		e[i].text_color &= 0xff000000;
		e[i].color = 0xffffff;
	}
}

/* ------------------------------------------------------------------ */
/* CheckBox (checkbox.asm, check_box2)                                 */

typedef struct check_box2 {
	bl_u32 left_s;             /* +0  x << 16 | width */
	bl_u32 top_s;              /* +4  y << 16 | height */
	bl_u32 ch_text_margin;     /* +8  from the box to the text */
	bl_u32 color;              /* +12 inside the box */
	bl_u32 border_color;       /* +16 the frame (and the mark) */
	bl_u32 text_color;         /* +20 */
	const char *text;          /* +24 */
	bl_u32 flags;              /* +28 ch_flag_* */
	bl_u32 size_of_str;        /* +32 the text's width, set by init_checkbox2 */
} check_box2;

#define ch_struc_size   36
#define ch_flag_en      2      /* checked */
#define ch_flag_top     0      /* the text at the top */
#define ch_flag_middle  4      /* ... in the middle */
#define ch_flag_bottom  8      /* ... at the bottom */

/* box_lib.mac's struc check_box2 as an initializer */
#define CHECK_BOX2(left_s, top_s, ch_text_margin, color, border_color, text_color, text, flags) \
	{ left_s, top_s, ch_text_margin, color, border_color, text_color, text, flags, 0 }

void init_checkbox2(check_box2 *c);
void check_box_draw2(check_box2 *c);
void check_box_mouse2(check_box2 *c);

/* box_lib.mac's init_checkboxes2, checkboxes_draw2, checkboxes_mouse2,
 * check_boxes_set_sys_color2 for c[0..n-1] */
static inline void init_checkboxes2(check_box2 *c, int n) { for (int i = 0; i < n; i++) init_checkbox2(&c[i]); }
static inline void checkboxes_draw2(check_box2 *c, int n) { for (int i = 0; i < n; i++) check_box_draw2(&c[i]); }
static inline void checkboxes_mouse2(check_box2 *c, int n) { for (int i = 0; i < n; i++) check_box_mouse2(&c[i]); }
static inline void check_boxes_set_sys_color2(check_box2 *c, int n, const system_colors *sc)
{
	for (int i = 0; i < n; i++) {
		c[i].text_color = sc->work_text | 0x80000000u;
		c[i].border_color = sc->work_graph;
	}
}

/* ------------------------------------------------------------------ */
/* OptionBox (optionbox.asm)                                           */

typedef struct option_box {
	struct option_box **option_group; /* +0  the group's variable: its chosen box */
	bl_u16 left;               /* +4  */
	bl_u16 top;                /* +6  */
	bl_u32 ch_text_margin;     /* +8  from the box to the text */
	bl_u32 ch_size;            /* +12 the box's size (12, say) */
	bl_u32 color;              /* +16 */
	bl_u32 border_color;       /* +20 */
	bl_u32 text_color;         /* +24 */
	const char *text;          /* +28 */
	bl_u32 ch_text_length;     /* +32 the text's length in characters */
	bl_u32 flags;              /* +36 */
} option_box;

#define op_flag_en 2

/* box_lib.mac's struc option_box as an initializer */
#define OPTION_BOX(point_gr, left, top, op_text_margin, op_size, color, border_color, text_color, text, \
		   op_text_length, flags) \
	{ point_gr, left, top, op_text_margin, op_size, color, border_color, text_color, text, op_text_length, flags }

/* list: the group's boxes, terminated by NULL */
void option_box_draw(option_box **list);
void option_box_mouse(option_box **list);

static inline void option_boxes_set_sys_color(const system_colors *sc, option_box **list)
{
	for (; *list; list++) {
		(*list)->border_color = sc->work_button;
		(*list)->color = sc->work_button_text;
		(*list)->text_color = sc->work_text;
	}
}


/* ------------------------------------------------------------------ */
/* ScrollBar (scrollbar.asm)                                           */

typedef struct scrollbar {
	bl_u16 x_size;             /* +0  */
	bl_u16 x_pos;              /* +2  */
	bl_u16 y_size;             /* +4  */
	bl_u16 y_pos;              /* +6  */
	bl_u32 btn_height;         /* +8  the buttons' size along the bar */
	bl_u32 type;               /* +12 0 plain, 1 framed, 2 arrows of lines */
	bl_u32 max_area;           /* +16 the whole */
	bl_u32 cur_area;           /* +20 the part seen */
	bl_u32 position;           /* +24 the first seen, 0 .. max - cur */
	bl_u32 bg_color;           /* +28 */
	bl_u32 front_color;        /* +32 */
	bl_u32 line_color;         /* +36 */
	bl_u32 redraw;             /* +40 set when the mouse moved the position */
	bl_u16 delta;              /* +44 */
	bl_u16 delta2;             /* +46 the runner dragged */
	bl_u16 r_size_x;           /* +48 the runner's rectangle */
	bl_u16 r_start_x;          /* +50 */
	bl_u16 r_size_y;           /* +52 */
	bl_u16 r_start_y;          /* +54 */
	bl_u32 m_pos;              /* +56 */
	bl_u32 m_pos_2;            /* +60 */
	bl_u32 m_keys;             /* +64 */
	bl_u32 run_size;           /* +68 */
	bl_u32 position2;          /* +72 the runner's position in pixels */
	bl_u32 work_size;          /* +76 */
	bl_u32 all_redraw;         /* +80 1: the buttons and the frame too */
	bl_u32 ar_offset;          /* +84 the step of the buttons */
} scrollbar;

#define sb_offs_size_x   0
#define sb_offs_start_x  2
#define sb_offs_size_y   4
#define sb_offs_start_y  6
#define sb_offs_max_area 16
#define sb_offs_cur_area 20
#define sb_offs_position 24
#define sb_offs_bckg_col 28
#define sb_offs_frnt_col 32
#define sb_offs_line_col 36
#define sb_offs_redraw   40
#define sb_offs_delta2   46
#define sb_offs_all_redraw 80

/* box_lib.mac's struc scrollbar as an initializer */
#define SCROLLBAR(xsize, xpos, ysize, ypos, btn_height, max, cur, pos, bgcol, frcol, linecol, ar_offset) \
	{ xsize, xpos, ysize, ypos, btn_height, 0, max, cur, pos, bgcol, frcol, linecol, \
	  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ar_offset }

void scrollbar_v_draw(scrollbar *sb);
void scrollbar_v_mouse(scrollbar *sb);
void scrollbar_h_draw(scrollbar *sb);
void scrollbar_h_mouse(scrollbar *sb);

/* box_lib.asm's draw_edge: a raised frame, the colours mixed inwards */
void draw_edge(bl_u32 box_l, bl_u32 box_t, bl_u32 box_w, bl_u32 box_h, bl_u32 col_0, bl_u32 col_1, bl_u32 col_2);


/* ------------------------------------------------------------------ */
/* TreeList (tree_list.asm): a tree, or a list box (tl_list_box_mode)  */

/* a node; 0 and 1 are the list's head and end */
typedef struct TreeNode {
	bl_u16 type;               /* +0  the node's icon */
	bl_u8 level;               /* +2  */
	bl_u8 close;               /* +3  1: closed (a parent node) */
	bl_u32 perv;               /* +4  the previous node's index */
	bl_u32 next;               /* +8  the next node's index */
	bl_u32 t_create;           /* +12 the time of creation (for undo) */
	bl_u32 t_delete;           /* +16 the time of deletion */
} TreeNode;

typedef struct tree_list {
	bl_u32 box_left;           /* +0  */
	bl_u32 box_top;            /* +4  */
	bl_u32 box_width;          /* +8  */
	bl_u32 box_height;         /* +12 */
	bl_u8 *data_info;          /* +16 the nodes' data (info_size each) */
	bl_u16 info_size;          /* +20 a node's data: the program's and the caption */
	bl_u32 info_max_count;     /* +22 nodes at most (2 of them the head and end) */
	bl_u32 style;              /* +26 tl_* below */
	TreeNode *data_nodes;      /* +30 */
	bl_u8 *data_img;           /* +34 the nodes' icons, BGR, img_cx x img_cy each */
	bl_u16 img_cx;             /* +38 */
	bl_u16 img_cy;             /* +40 */
	bl_u8 *data_img_sys;       /* +42 the cursor, lines, plus and minus */
	bl_u32 ch_tim;             /* +46 changes */
	bl_u32 tim_undo;           /* +50 changes undone */
	bl_u32 cur_pos;            /* +54 the cursor's position */
	bl_u32 col_bkg;            /* +58 */
	bl_u32 col_zag;            /* +62 the caption's colour */
	bl_u32 col_txt;            /* +66 */
	bl_u16 capt_cy;            /* +70 the caption's height */
	bl_u16 info_capt_offs;     /* +72 the caption's offset in a node's data */
	bl_u16 info_capt_len;      /* +74 its length (0: to the data's end) */
	void **el_focus;           /* +76 the variable of the element in focus */
	scrollbar *p_scroll;       /* +80 the list's vertical scroll bar, or NULL */
	void (*on_press)(struct tree_list *tl);   /* +84 Enter or a double click */
} __attribute__((packed)) tree_list;

/* box_lib.mac's struc tree_list as an initializer */
#define TREE_LIST(info_size, info_max_count, style, img_cx, img_cy, col_bkg, col_zag, col_txt, \
		  box_l, box_t, box_w, box_h, capt_cy, info_capt_offs, info_capt_len, el_focus, p_scroll, on_press) \
	{ box_l, box_t, box_w, box_h, 0, info_size, info_max_count, style, 0, 0, img_cx, img_cy, 0, 0, 0, 0, \
	  col_bkg, col_zag, col_txt, capt_cy, info_capt_offs, info_capt_len, el_focus, p_scroll, on_press }

/* style */
#define tl_key_no_edit        1  /* not edited by the keyboard (levels, deleting) */
#define tl_draw_par_line      2  /* lines to the parent nodes */
#define tl_list_box_mode      4  /* no levels (a list box) */
#define tl_cursor_pos_limited 8  /* the cursor only on nodes */
/* the functions' errors and options */
#define tl_err_save_memory_size 2
#define tl_err_load_caption     1
#define tl_err_load_info_size   4
#define tl_load_mode_add        0x20000
#define tl_save_load_heder_size 26
#define tl_offs_box             0

void tl_data_init(tree_list *tl);
void tl_data_clear(tree_list *tl);
void tl_info_clear(tree_list *tl);
void tl_key(tree_list *tl, bl_u32 key);    /* key: function 2's result (eax there) */
void tl_mouse(tree_list *tl);
void tl_draw(tree_list *tl);
void tl_info_undo(tree_list *tl);
void tl_info_redo(tree_list *tl);
/* n_opt: the icon << 16 | closed << 8 | level */
void tl_node_add(tree_list *tl, bl_u32 n_opt, const void *n_info);
void tl_node_set_data(tree_list *tl, const void *n_info);
void *tl_node_get_data(tree_list *tl);
void tl_node_delete(tree_list *tl);
void tl_cur_beg(tree_list *tl);
void tl_cur_next(tree_list *tl);
void tl_cur_perv(tree_list *tl);
void tl_cur_page_up(tree_list *tl);
void tl_cur_page_down(tree_list *tl);
void tl_node_close_open(tree_list *tl);
void tl_node_lev_inc(tree_list *tl);
void tl_node_lev_dec(tree_list *tl);
void tl_node_move_up(tree_list *tl);
void tl_node_move_down(tree_list *tl);
TreeNode *tl_node_poi_get_info(tree_list *tl, bl_u32 node_ind);
TreeNode *tl_node_poi_get_next_info(tree_list *tl, TreeNode *node_p);
void *tl_node_poi_get_data(tree_list *tl, TreeNode *node_p);
int tl_save_mem(tree_list *tl, bl_u32 opt, void *h_mem, bl_u32 mem_size);
int tl_load_mem(tree_list *tl, bl_u32 opt, const void *h_mem, bl_u32 mem_size);
bl_u32 tl_get_mem_size(tree_list *tl, const void *h_mem);

/* the layouts, as box_lib.mac's offsets have them */
_Static_assert(sizeof(edit_box) == ed_struc_size, "edit_box");
_Static_assert(offsetof(edit_box, cl_curs_x) == 60 && offsetof(edit_box, char_width) == 72, "edit_box");
_Static_assert(sizeof(check_box2) == ch_struc_size, "check_box2");
_Static_assert(sizeof(option_box) == 40 && offsetof(option_box, ch_text_margin) == 8, "option_box");
_Static_assert(sizeof(TreeNode) == 20, "TreeNode");
_Static_assert(sizeof(tree_list) == 88 && offsetof(tree_list, info_size) == 20 && offsetof(tree_list, info_max_count) == 22 &&
	       offsetof(tree_list, style) == 26 && offsetof(tree_list, data_img_sys) == 42 && offsetof(tree_list, el_focus) == 76 &&
	       offsetof(tree_list, p_scroll) == 80, "tree_list");
_Static_assert(sizeof(scrollbar) == 88 && offsetof(scrollbar, redraw) == 40 && offsetof(scrollbar, all_redraw) == 80, "scrollbar");

#endif
