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

/* the layouts, as box_lib.mac's offsets have them */
_Static_assert(sizeof(edit_box) == ed_struc_size, "edit_box");
_Static_assert(offsetof(edit_box, cl_curs_x) == 60 && offsetof(edit_box, char_width) == 72, "edit_box");
_Static_assert(sizeof(check_box2) == ch_struc_size, "check_box2");
_Static_assert(sizeof(option_box) == 40 && offsetof(option_box, ch_text_margin) == 8, "option_box");

#endif
