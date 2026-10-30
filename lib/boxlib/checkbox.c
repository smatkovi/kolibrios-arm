/* KolibriOS for ARM: box_lib's CheckBox (check_box2), translated to C
 * from programs/develop/libraries/box_lib/checkbox.asm (Alexey Teplov
 * <Lrz>, 13.02.2009, the KolibriOS team; GPL-2.0) */
#include "bl_sys.h"

/* the text's width (6 pixels a character) with the margin, and the
 * ASCIIZ flag in its colour */
void init_checkbox2(check_box2 *c)
{
	u32 w = c->ch_text_margin;
	for (const char *s = c->text; *s; s++) w += 6;
	c->size_of_str = w;
	c->text_color |= 0x80000000u;
}

static void check_box_draw_ch(check_box2 *c, u32 color)
{
	/* the mark: the box 2 pixels in */
	bl_rect(c->left_s + (2 << 16) - 4, c->top_s + (2 << 16) - 4, color);
}

void check_box_draw2(check_box2 *c)
{
	/* the frame: a rectangle in the frame's colour, the inside over it */
	bl_rect(c->left_s, c->top_s, c->border_color);
	bl_rect(c->left_s + (1 << 16) - 2, c->top_s + (1 << 16) - 2, c->color);
	if (c->flags & ch_flag_en) check_box_draw_ch(c, c->border_color);
	/* the text: right of the box, at its top, middle or bottom */
	u32 x = (c->left_s >> 16) + (c->left_s & 0xffff) + c->ch_text_margin;
	u16 y = c->top_s >> 16, d = (u16)(c->top_s - 8);
	if (c->flags & ch_flag_bottom) y += d;
	else if (c->flags & ch_flag_middle) y += (u16)(d >> 1);
	bl_text(x << 16 | y, c->text_color, c->text, 0);
}

/* a press of the left or the right button over the box or its text
 * turns the box over */
void check_box_mouse2(check_box2 *c)
{
	if (!(bl_mouse_buttons_ext() & (3 << 8))) return;
	u32 pos = bl_mouse_pos();
	u16 my = pos, mx = pos >> 16;
	u16 y = c->top_s >> 16, x = c->left_s >> 16;
	if (my < y || my > (u16)(y + (u16)c->top_s)) return;
	if (mx < x || mx > (u16)(x + (u16)c->left_s + (u16)c->size_of_str)) return;
	c->flags ^= ch_flag_en;
	check_box_draw_ch(c, (c->flags & ch_flag_en) ? c->border_color : c->color);
}
