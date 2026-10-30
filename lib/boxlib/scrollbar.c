/* KolibriOS for ARM: box_lib's ScrollBar, translated to C from
 * programs/develop/libraries/box_lib/scrollbar.asm (Marat Zakiyanov aka
 * Mario79, 2009-2012, scroll type 1 by IgorA; BSD-style licence of its
 * header, box_lib GPL-2.0) and box_lib.asm's draw_edge.
 *
 * The drawing keeps the original's rectangles one by one (the vertical
 * and the horizontal bar are written out each, as there); the mouse's
 * handling is the same for both, along the bar's axis. */
#include "bl_sys.h"

/* box_lib.asm: draw_edge */
void draw_edge(u32 box_l, u32 box_t, u32 box_w, u32 box_h, u32 col_0, u32 col_1, u32 col_2)
{
	u32 esi = col_1 & 0xfefefe, edx, ebx, ecx;
#define MIX() (edx = ((edx & 0xfefefe) + esi) >> 1)
	/* the bottom line */
	edx = col_2;
	ebx = (box_l << 16) + box_w + 1;           /* + 1 for the diagonal's pixels */
	ecx = ((box_t + box_h) << 16) + 1;
	for (int i = 0; i < 3; i++) {
		MIX();
		ecx -= 1 << 16;
		ebx += 1 << 16;
		ebx -= 2;
		bl_rect(ebx, ecx, edx);
	}
	/* the right line */
	edx = col_2;
	ebx = ((box_l + box_w) << 16) + 1;
	ecx = (box_t << 16) + box_h;
	for (int i = 0; i < 3; i++) {
		MIX();
		ebx -= 1 << 16;
		ecx += 1 << 16;
		ecx -= 2;
		bl_rect(ebx, ecx, edx);
	}
	/* the top line */
	edx = col_0;
	ebx = (box_l << 16) + box_w;
	ecx = (box_t << 16) + 1;
	for (int i = 0; i < 3; i++) {
		MIX();
		ecx += 1 << 16;
		ebx += 1 << 16;
		ebx -= 2;
		bl_rect(ebx, ecx, edx);
	}
	/* the left line */
	edx = col_0;
	ebx = (box_l << 16) + 1;
	ecx = (box_t << 16) + box_h;
	for (int i = 0; i < 3; i++) {
		MIX();
		ebx += 1 << 16;
		ecx += 1 << 16;
		ecx -= 2;
		bl_rect(ebx, ecx, edx);
	}
#undef MIX
}

/* the 16-bit halves of a register as the assembler changes them */
#define LO(r) ((u16)(r))
#define SETLO(r, v) ((r) = ((r) & 0xffff0000u) | (u16)(v))

/* the runner's size and position from the areas (the start of .draw_1) */
static void sb_runner(scrollbar *sb, u32 size)
{
	u32 eax, ebx, edx;
	sb->work_size = size - 2 * sb->btn_height;
	eax = sb->work_size;
	ebx = sb->max_area;
	if (ebx > sb->cur_area) {
		edx = eax * sb->cur_area % ebx;
		eax = eax * sb->cur_area / ebx;
		if (edx * 2 >= ebx) eax++;
		if (eax < 10) eax = 10;
	}
	sb->run_size = eax;
	if (sb->delta == 1) return;
	eax = sb->work_size - sb->run_size;
	ebx = sb->max_area;
	if (ebx > sb->cur_area) {
		ebx -= sb->cur_area;
		edx = eax * sb->position % ebx;
		eax = eax * sb->position / ebx;
		if (edx * 2 >= ebx) eax++;
	} else {
		eax = 0;
	}
	sb->position2 = eax;
	edx = size - sb->btn_height - sb->btn_height - sb->run_size;
	if (sb->position2 > edx) sb->position2 = edx;
}

/* draw_up_arrow_vertical, draw_down_arrow_vertical: from the button's
 * filling rectangle */
static void up_arrow_v(scrollbar *sb, u32 ebx, u32 ecx)
{
	ebx += (u32)(sb->x_size >> 1) << 16;
	ecx += (u32)(LO(sb->btn_height) >> 1) << 16;
	ebx -= 4 << 16; SETLO(ebx, 7); SETLO(ecx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ecx -= 1 << 16; ebx += 1 << 16; SETLO(ebx, 5);
	bl_rect(ebx, ecx, sb->line_color);
	ecx -= 1 << 16; ebx += 1 << 16; SETLO(ebx, 3);
	bl_rect(ebx, ecx, sb->line_color);
	ecx -= 1 << 16; ebx += 1 << 16; SETLO(ebx, 1);
	bl_rect(ebx, ecx, sb->line_color);
}

static void down_arrow_v(scrollbar *sb, u32 ebx, u32 ecx)
{
	ebx += (u32)(sb->x_size >> 1) << 16;
	ecx += (u32)(LO(sb->btn_height) >> 1) << 16;
	ebx -= 4 << 16; SETLO(ebx, 7);
	ecx -= 2 << 16; SETLO(ecx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 1 << 16; ebx += 1 << 16; SETLO(ebx, 5);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 1 << 16; ebx += 1 << 16; SETLO(ebx, 3);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 1 << 16; ebx += 1 << 16; SETLO(ebx, 1);
	bl_rect(ebx, ecx, sb->line_color);
}

/* the type 2 arrows: two lines from the button's centre */
static void up_arrow_v2(scrollbar *sb)
{
	u16 cx = (sb->x_size >> 1) + sb->x_pos, cy = (LO(sb->btn_height) >> 1) + sb->y_pos;
	bl_line((u32)cx << 16 | (u16)(cx - 3), (u32)(u16)(cy - 2) << 16 | (u16)(cy + 1), sb->line_color);
	bl_line((u32)cx << 16 | (u16)(cx + 3), (u32)(u16)(cy - 2) << 16 | (u16)(cy + 1), sb->line_color);
}

static void down_arrow_v2(scrollbar *sb)
{
	u16 cx = (sb->x_size >> 1) + sb->x_pos;
	u16 cy = sb->y_pos + sb->y_size - (LO(sb->btn_height) >> 1);
	bl_line((u32)cx << 16 | (u16)(cx - 3), (u32)(u16)(cy + 1) << 16 | (u16)(cy - 2), sb->line_color);
	bl_line((u32)cx << 16 | (u16)(cx + 3), (u32)(u16)(cy + 1) << 16 | (u16)(cy - 2), sb->line_color);
}

static void up_arrow_h(scrollbar *sb, u32 ebx, u32 ecx)
{
	ebx += (u32)(LO(sb->btn_height) >> 1) << 16;
	ecx += (u32)(sb->y_size >> 1) << 16;
	ecx -= 4 << 16; SETLO(ecx, 7); SETLO(ebx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ebx -= 1 << 16; ecx += 1 << 16; SETLO(ecx, 5);
	bl_rect(ebx, ecx, sb->line_color);
	ebx -= 1 << 16; ecx += 1 << 16; SETLO(ecx, 3);
	bl_rect(ebx, ecx, sb->line_color);
	ebx -= 1 << 16; ecx += 1 << 16; SETLO(ecx, 1);
	bl_rect(ebx, ecx, sb->line_color);
}

static void down_arrow_h(scrollbar *sb, u32 ebx, u32 ecx)
{
	ebx += (u32)(LO(sb->btn_height) >> 1) << 16;
	ecx += (u32)(sb->y_size >> 1) << 16;
	ecx -= 4 << 16; SETLO(ecx, 7);
	ebx -= 2 << 16; SETLO(ebx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ebx += 1 << 16; ecx += 1 << 16; SETLO(ecx, 5);
	bl_rect(ebx, ecx, sb->line_color);
	ebx += 1 << 16; ecx += 1 << 16; SETLO(ecx, 3);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 1 << 16; ebx += 1 << 16; SETLO(ecx, 1);
	bl_rect(ebx, ecx, sb->line_color);
}

static void up_arrow_h2(scrollbar *sb)
{
	u16 cx = (LO(sb->btn_height) >> 1) + sb->x_pos, cy = (sb->y_size >> 1) + sb->y_pos;
	bl_line((u32)(u16)(cx - 2) << 16 | (u16)(cx + 1), (u32)cy << 16 | (u16)(cy - 3), sb->line_color);
	bl_line((u32)(u16)(cx - 2) << 16 | (u16)(cx + 1), (u32)cy << 16 | (u16)(cy + 3), sb->line_color);
}

static void down_arrow_h2(scrollbar *sb)
{
	/* as the original has it */
	u16 cx = sb->x_pos + sb->x_size - (LO(sb->btn_height) >> 1), cy = (sb->y_size >> 1) + sb->y_pos;
	bl_line((u32)cx << 16 | (u16)(cx - 3), (u32)(u16)(cy + 1) << 16 | (u16)(cy - 2), sb->line_color);
	bl_line((u32)cx << 16 | (u16)(cx - 3), (u32)(u16)(cy + 1) << 16 | (u16)(cy + 4), sb->line_color);
}

/* the marks in the runner's middle */
static void runner_center_v(scrollbar *sb, u32 ebx, u32 ecx)
{
	ebx += (u32)(sb->x_size >> 1) << 16;
	ecx += (sb->run_size >> 1) << 16;
	ecx -= 8 << 16; ebx -= 5 << 16; SETLO(ebx, 10);
	ecx += 4 << 16; SETLO(ecx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 3 << 16; ebx -= 1 << 16; SETLO(ebx, 12);
	bl_rect(ebx, ecx, sb->line_color);
	ebx += 1 << 16; SETLO(ebx, 10); ecx += 3 << 16;
	bl_rect(ebx, ecx, sb->line_color);
}

static void runner_center_h(scrollbar *sb, u32 ebx, u32 ecx)
{
	ecx += (u32)(sb->y_size >> 1) << 16;
	ebx += (sb->run_size >> 1) << 16;
	ebx -= 8 << 16; ecx -= 5 << 16; SETLO(ecx, 10);
	ebx += 4 << 16; SETLO(ebx, 1);
	bl_rect(ebx, ecx, sb->line_color);
	ebx += 3 << 16; ecx -= 1 << 16; SETLO(ecx, 12);
	bl_rect(ebx, ecx, sb->line_color);
	ecx += 1 << 16; SETLO(ecx, 10); ebx += 3 << 16;
	bl_rect(ebx, ecx, sb->line_color);
}

/* scroll_bar_vertical.draw_1 */
static void sb_v_draw_1(scrollbar *sb)
{
	u32 ebx, ecx, a, b, c, eax;
	int all = sb->all_redraw != 0;
	sb_runner(sb, sb->y_size);
	ebx = (u32)sb->x_pos << 16 | 1;
	ecx = (u32)sb->y_pos << 16 | sb->y_size;
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the left line */
	if (all) bl_rect(ebx + ((u32)sb->x_size << 16), ecx, sb->line_color);   /* the right line */
	a = ecx;
	ebx += 1 << 16; SETLO(ebx, sb->x_size); ebx--;
	SETLO(ecx, 1);
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the top button: its line */
	b = ecx;
	ecx += 1 << 16; ecx += sb->btn_height; ecx -= 2;
	if (all) {
		bl_rect(ebx, ecx, sb->front_color);                        /* ... its filling */
		if (LO(sb->type) == 2) up_arrow_v2(sb);
		else up_arrow_v(sb, ebx, ecx);
	}
	ecx = b + (sb->btn_height << 16);
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* ... its bottom line */
	ecx = a + ((sb->btn_height + 1) << 16);
	ecx -= 2 * sb->btn_height;
	c = ecx;
	SETLO(ecx, 0); ecx += sb->position2; SETLO(ecx, LO(ecx) - 1);
	if (!(ecx & 0x8000)) bl_rect(ebx, ecx, sb->bg_color);         /* the interval above */
	ecx = (((ecx >> 16) + sb->position2 - 1) << 16) + 1;
	bl_rect(ebx, ecx, sb->line_color);                             /* the runner: its top line */
	ecx += 1 << 16; SETLO(ecx, 0); ecx += sb->run_size;
	sb->r_size_x = ebx; sb->r_start_x = ebx >> 16;
	sb->r_size_y = ecx; sb->r_start_y = ecx >> 16;
	ecx -= 2;
	bl_rect(ebx, ecx, sb->front_color);                            /* ... its filling */
	if (LO(sb->type) != 2) runner_center_v(sb, ebx, ecx);
	if (LO(sb->type) == 1)                                         /* a framed runner */
		draw_edge(sb->x_pos, (u32)sb->r_start_y - 1, sb->x_size, sb->run_size - 1,
			  sb->bg_color, sb->front_color, sb->line_color);
	ecx = (((ecx >> 16) + sb->run_size - 2) << 16) + 1;
	bl_rect(ebx, ecx, sb->line_color);                             /* ... its bottom line */
	ecx += 1 << 16;
	SETLO(ecx, sb->y_pos + sb->y_size);
	ecx -= sb->btn_height; ecx--;
	eax = ecx >> 16;
	SETLO(ecx, LO(ecx) - (u16)eax);
	if (!(ecx & 0x8000)) bl_rect(ebx, ecx, sb->bg_color);         /* the interval below */
	ecx = c;
	eax = LO(ecx);
	ecx >>= 16; SETLO(ecx, LO(ecx) + eax); ecx -= 2;
	ecx = (ecx << 16) + 1;
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the bottom button: its top line */
	b = ecx;
	ecx += 1 << 16; SETLO(ecx, LO(ecx) + LO(sb->btn_height)); ecx -= 2;
	if (all) {
		bl_rect(ebx, ecx, sb->front_color);                        /* ... its filling */
		if (LO(sb->type) == 2) down_arrow_v2(sb);
		else down_arrow_v(sb, ebx, ecx);
	}
	ecx = b + (sb->btn_height << 16);
	SETLO(ecx, 1);
	if (all) {
		bl_rect(ebx, ecx, sb->line_color);                         /* ... its bottom line */
		if (LO(sb->type) == 1) {                                   /* framed buttons */
			draw_edge(sb->x_pos, sb->y_pos, sb->x_size, sb->btn_height,
				  sb->bg_color, sb->front_color, sb->line_color);
			draw_edge(sb->x_pos, (u16)(sb->y_pos + sb->y_size) - sb->btn_height - 1, sb->x_size,
				  sb->btn_height, sb->bg_color, sb->front_color, sb->line_color);
		}
	}
}

/* scroll_bar_horizontal.draw_1 */
static void sb_h_draw_1(scrollbar *sb)
{
	u32 ebx, ecx, a, b, c, eax;
	int all = sb->all_redraw != 0;
	sb_runner(sb, sb->x_size);
	ebx = (u32)sb->x_pos << 16 | sb->x_size;
	ecx = (u32)sb->y_pos << 16 | 1;
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the top line */
	if (all) bl_rect(ebx, ecx + ((u32)sb->y_size << 16), sb->line_color);   /* the bottom line */
	a = ebx;
	ecx += 1 << 16; SETLO(ecx, sb->y_size); ecx--;
	SETLO(ebx, 1);
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the left button: its line */
	b = ebx;
	ebx += 1 << 16; ebx += sb->btn_height; ebx -= 2;
	if (all) {
		bl_rect(ebx, ecx, sb->front_color);                        /* ... its filling */
		if (LO(sb->type) == 2) up_arrow_h2(sb);
		else up_arrow_h(sb, ebx, ecx);
	}
	ebx = b + (sb->btn_height << 16);
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* ... its right line */
	ebx = a + ((sb->btn_height + 1) << 16);
	ebx -= 2 * sb->btn_height;
	c = ebx;
	SETLO(ebx, 0); ebx += sb->position2; SETLO(ebx, LO(ebx) - 1);
	if (!(ebx & 0x8000)) bl_rect(ebx, ecx, sb->bg_color);         /* the interval on the left */
	ebx = (((ebx >> 16) + sb->position2 - 1) << 16) + 1;
	bl_rect(ebx, ecx, sb->line_color);                             /* the runner: its left line */
	ebx += 1 << 16; SETLO(ebx, 0); ebx += sb->run_size;
	sb->r_size_x = ebx; sb->r_start_x = ebx >> 16;
	sb->r_size_y = ecx; sb->r_start_y = ecx >> 16;
	ebx -= 2;
	bl_rect(ebx, ecx, sb->front_color);                            /* ... its filling */
	if (LO(sb->type) != 2) runner_center_h(sb, ebx, ecx);
	if (LO(sb->type) == 1)
		draw_edge((u32)sb->r_start_x - 1, sb->y_pos, sb->run_size - 1, sb->y_size,
			  sb->bg_color, sb->front_color, sb->line_color);
	ebx = (((ebx >> 16) + sb->run_size - 2) << 16) + 1;
	bl_rect(ebx, ecx, sb->line_color);                             /* ... its right line */
	ebx += 1 << 16;
	SETLO(ebx, sb->x_pos + sb->x_size);
	ebx -= sb->btn_height; ebx--;
	eax = ebx >> 16;
	SETLO(ebx, LO(ebx) - (u16)eax);
	if (!(ebx & 0x8000)) bl_rect(ebx, ecx, sb->bg_color);         /* the interval on the right */
	ebx = c;
	eax = LO(ebx);
	ebx >>= 16; SETLO(ebx, LO(ebx) + eax); ebx -= 2;
	ebx = (ebx << 16) + 1;
	if (all) bl_rect(ebx, ecx, sb->line_color);                    /* the right button: its left line */
	b = ebx;
	ebx += 1 << 16; SETLO(ebx, LO(ebx) + LO(sb->btn_height)); ebx -= 2;
	if (all) {
		bl_rect(ebx, ecx, sb->front_color);                        /* ... its filling */
		if (LO(sb->type) == 2) down_arrow_h2(sb);
		else down_arrow_h(sb, ebx, ecx);
	}
	ebx = b + (sb->btn_height << 16);
	SETLO(ebx, 1);
	if (all) {
		bl_rect(ebx, ecx, sb->line_color);                         /* ... its right line */
		if (LO(sb->type) == 1) {
			draw_edge(sb->x_pos, sb->y_pos, sb->btn_height, sb->y_size,
				  sb->bg_color, sb->front_color, sb->line_color);
			draw_edge((u16)(sb->x_pos + sb->x_size) - sb->btn_height - 1, sb->y_pos, sb->btn_height,
				  sb->y_size, sb->bg_color, sb->front_color, sb->line_color);
		}
	}
}

void scrollbar_v_draw(scrollbar *sb)
{
	sb->delta = 0;
	sb_v_draw_1(sb);
	sb->all_redraw = 0;
}

void scrollbar_h_draw(scrollbar *sb)
{
	sb->delta = 0;
	sb_h_draw_1(sb);
	sb->all_redraw = 0;
}

/* .mouse of both: along the bar (a: y of the vertical, x of the
 * horizontal) and across it (c) */
static void sb_mouse(scrollbar *sb, int vertical)
{
	u16 a_start = vertical ? sb->y_pos : sb->x_pos, a_size = vertical ? sb->y_size : sb->x_size;
	u16 c_start = vertical ? sb->x_pos : sb->y_pos, c_size = vertical ? sb->x_size : sb->y_size;
	void (*draw_1)(scrollbar *) = vertical ? sb_v_draw_1 : sb_h_draw_1;
	u32 eax, ebx, ecx, edx;
	u16 ma, mc, cx;

	sb->m_pos = bl_mouse_pos();
	if (!sb->m_pos_2) sb->m_pos_2 = sb->m_pos;
	sb->m_keys = bl_mouse_buttons();
	if (!sb->m_keys) {
		sb->m_pos_2 = sb->m_pos;
		sb->delta = sb->delta2 = 0;            /* .correct_1 */
		return;
	}
	sb->delta = 1;
	/* .continue_2 */
	eax = sb->m_pos;
	if (eax & 0x80000000u || eax & 0x8000) return;
	ma = vertical ? eax : eax >> 16;
	mc = vertical ? eax >> 16 : eax;
	cx = a_start;
	if (ma < cx) return;
	if (sb->delta2 && ma < (u16)(cx + sb->btn_height)) return;
	cx += a_size;
	if (ma > cx) return;
	if (sb->delta2) {
		cx -= sb->btn_height;
		if (ma > cx) return;
	}
	if (sb->delta2 != 1) {
		if (!sb->m_keys) return;
		if (mc < c_start || mc > (u16)(c_start + c_size)) return;
		u16 r_start = vertical ? sb->r_start_y : sb->r_start_x, r_size = vertical ? sb->r_size_y : sb->r_size_x;
		if (ma < r_start || ma > (u16)(r_start + r_size)) {
			sb->delta = sb->delta2 = 0;    /* .no_runner */
			goto no_runner_1;
		}
		sb->delta2 = 1;
	}
	/* the runner dragged */
	{
		u32 now = vertical ? (u16)sb->m_pos : sb->m_pos >> 16;
		u32 before = vertical ? (u16)sb->m_pos_2 : sb->m_pos_2 >> 16;
		if (now == before) return;             /* .correct */
		if (now < before) {
			sb->position2 -= before - now;
			if (sb->position2 & 0x80000000u) sb->position2 = 0;
		} else {
			sb->position2 += now - before;
			eax = a_size - sb->btn_height - sb->btn_height - sb->run_size;
			if (sb->position2 > eax) sb->position2 = eax;
		}
		sb->m_pos_2 = sb->m_pos;
		draw_1(sb);
	}
no_runner_1:
	ecx = (u16)(a_start + sb->btn_height);
	if (sb->delta != 1 && !(ma > (u16)ecx)) {
		/* the first button: back by ar_offset */
		if (!sb->m_keys) return;
		if (sb->position > sb->ar_offset) {
			sb->position -= sb->ar_offset;
		} else {
			sb->position = 0;
			sb->position2 = 0;
		}
		goto all_sb;
	}
	/* .scroll_sb */
	SETLO(ecx, LO(ecx) + a_size - sb->btn_height - sb->btn_height);
	if (sb->delta != 1 && ma > LO(ecx)) goto down_sb;
	ebx = sb->btn_height;
	SETLO(ebx, LO(ebx) + a_start);
	SETLO(ecx, LO(ecx) - LO(ebx));
	eax = (u16)(ma - LO(ebx));
	/* .scroll_sb_1 */
	ebx = sb->run_size;
	if (sb->delta != 1) {
		/* a click on the track: the runner's middle there */
		sb->position2 = eax - (ebx >> 1);
		if (sb->position2 & 0x80000000u) {
			sb->position2 = 0;
		} else {
			edx = a_size - sb->btn_height - sb->btn_height - sb->run_size;
			if (sb->position2 > edx) sb->position2 = edx;
		}
	}
	/* .continue */
	eax = sb->position2;
	ecx -= ebx;
	ebx = eax;
	if (!(sb->max_area > sb->cur_area)) {
		sb->position2 = 1;
		goto all_sb;
	}
	eax = (sb->max_area - sb->cur_area + 1) << 10;
	if (ecx) {
		edx = eax % ecx;
		eax /= ecx;
		if (edx * 2 >= ecx) eax++;
	} else {
		eax = 0;
	}
	eax = eax * ebx >> 10;
	if (sb->position == eax) return;
	/* .store_position */
	edx = sb->max_area - sb->cur_area;
	sb->position = edx > eax ? eax : edx;
all_sb:
	sb->redraw = 1;
	draw_1(sb);
	sb->m_pos_2 = sb->m_pos;
	return;
down_sb:
	/* the second button: on by ar_offset */
	if (!sb->m_keys) return;
	eax = sb->max_area - sb->cur_area;
	if (sb->position < eax - sb->ar_offset) {
		sb->position += sb->ar_offset;
	} else {
		sb->position = eax;
		sb->position2 = a_size - sb->btn_height - sb->btn_height - sb->run_size;
	}
	goto all_sb;
}

void scrollbar_v_mouse(scrollbar *sb)
{
	sb_mouse(sb, 1);
}

void scrollbar_h_mouse(scrollbar *sb)
{
	sb_mouse(sb, 0);
}
