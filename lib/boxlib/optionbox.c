/* KolibriOS for ARM: box_lib's OptionBox, translated to C from
 * programs/develop/libraries/box_lib/optionbox.asm (Alexey Teplov <Lrz>,
 * 13.02.2009; IgorA, 12.09.2017; the KolibriOS team; GPL-2.0).
 *
 * The functions take the group's list: pointers to its boxes, ended by
 * NULL; each box points to the group's variable, which points to the
 * chosen box. */
#include "bl_sys.h"

static void option_box_draw_box(option_box *o)
{
	u32 b = (u32)o->left << 16 | o->ch_size, c = (u32)o->top << 16 | o->ch_size;
	bl_rect(b, c, o->border_color);                    /* the frame */
	bl_rect(b + (1 << 16) - 2, c + (1 << 16) - 2, o->color);   /* the inside */
	/* the text */
	u32 x = o->left + o->ch_size + o->ch_text_margin;
	u32 xy = (x << 16 | o->top) + (o->ch_size >> 1) + 4 - 7;
	bl_text(xy, o->text_color, o->text, o->ch_text_length);
}

/* the mark of the chosen box (draw_op) or the box cleared (clear_op) */
static void option_box_mark(option_box *o, u32 color)
{
	u32 q = o->ch_size >> 2;
	u16 w = o->ch_size - 2 * q;
	bl_rect((o->left + q) << 16 | w, (u32)(u16)(o->top + q) << 16 | w, color);
}

static void option_check(option_box **list)
{
	for (; *list; list++) {
		option_box *o = *list;
		option_box_mark(o, *o->option_group == o ? o->border_color : o->color);
	}
}

void option_box_draw(option_box **list)
{
	for (option_box **l = list; *l; l++) option_box_draw_box(*l);
	option_check(list);
}

static void option_box_mouse_op(option_box *o)
{
	u32 tw = o->ch_text_length * 6 + o->ch_text_margin;
	u32 pos = bl_mouse_pos();
	s16 my = pos, mx = pos >> 16;
	if (my < (s16)o->top || my > (s16)(o->top + o->ch_size)) return;
	if (mx < (s16)o->left || mx > (s16)(o->left + o->ch_size + tw)) return;
	*o->option_group = o;
}

void option_box_mouse(option_box **list)
{
	if (!bl_mouse_buttons()) return;
	for (option_box **l = list; *l; l++) option_box_mouse_op(*l);
	option_check(list);
}
