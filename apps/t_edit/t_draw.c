/* KolibriOS for ARM: t_edit's window, toolbar and panels, translated to
 * C from programs/other/t_edit/t_draw.inc (IgorA, the KolibriOS team;
 * GPL-2.0). The coordinates are the original's, x << 16 | y as the
 * system functions take them. */
#include <string.h>
#include "t_edit.h"

#define MIN_M_WND_H 100                /* the main window's least height */

#define PI_CLIENT_W (*(s32 *)(procinfo + 62))
#define PI_CLIENT_H (*(s32 *)(procinfo + 66))

/* a panel's button, its earlier definition first (draw_but_icon says why) */
static void panel_button(u32 ebx, u32 ecx, u32 id)
{
	kos_button_delete(id);
	kos_call(8, ebx, ecx, id, sc.work_button, 0);
}

static void text_z(u32 ebx, u32 color, const char *s)
{
	kos_call(4, ebx, color | 0x80000000u, (u32)s, 0, 0);
}

void draw_window(void)
{
	kos_begin_draw();
	u32 edx = sc.work | 0x73000000u;
	kos_call(0, (u32)wnd_s_pos[1] << 16 | wnd_s_pos[0], (u32)wnd_s_pos[3] << 16 | wnd_s_pos[2], edx, 0, (u32)hed);
	kos_call(9, (u32)procinfo, -1, 0, 0, 0);
	EvSize();                              /* edi = tedit0 there */
	u32 ebx = (u16)PI_CLIENT_W + 1;
	kos_call(13, ebx, tedit0.wnd.top, sc.work, 0, 0);   /* the top, the panel's place cleared */
	draw_but_toolbar();
	kmainmenu_draw(main_menu);
	ted_draw(&tedit0);
	mb_draw();                             /* a message box open (msgbox.h) */
	kos_end_draw();
}

/* ecx: the button's identifier, 0x40000000 if enabled; edx: x << 16 | y */
static void draw_but_icon(u32 ecx, u32 edx)
{
	u32 ebx = edx;
	u32 bc = (ebx & 0xffff) << 16 | 19;
	ebx = (ebx & 0xffff0000u) | 19;
	if ((s32)((ebx >> 16) + 20) >= PI_CLIENT_W) return;   /* the button does not fit */
	/* the toolbar is drawn again after the editor's changes, outside a
	 * redraw: the button's earlier definition goes first (KolibriOS keeps
	 * thousands of buttons, this kernel 64 a window) */
	kos_button_delete(ecx & 0xffff);
	kos_call(8, ebx, bc, ecx, sc.work_button, 0);
	const u8 *img = bmp_icon;
	if (!(ecx & 0x40000000u)) img += TOOLBAR_ICONS_SIZE;  /* the gray ones */
	img += ((ecx - ID_BUT_0) & 0xffff) * 1200;
	kos_call(7, (u32)img, 20 << 16 | 20, edx, 0, 0);
}

#define CX(id) (ecx = (ecx & 0xffff0000u) | (id))
#define SHOW(n, dx) do { if (panel_but[n] & 1) { edx += (dx) << 16; draw_but_icon(ecx, edx); } } while (0)

void draw_but_toolbar(void)
{
	text_editor *edi = &tedit0;
	u32 ecx = 0x40000000u + ID_BUT_NEW, edx = 5 << 16 | 25;
	if (panel_but[0] & 1) draw_but_icon(ecx, edx);        /* New */
	CX(ID_BUT_OPEN); SHOW(1, 25);                          /* Open */
	if (!ted_can_save(edi)) ecx &= 0xffff;
	CX(ID_BUT_SAVE); SHOW(2, 25);                          /* Save */
	ecx |= 0x40000000u;
	CX(ID_BUT_SAVE_AS); SHOW(3, 25);                       /* Save as */
	CX(ID_BUT_SELECT); SHOW(4, 30);                        /* Select word */
	if (!ted_is_select(edi)) ecx &= 0xffff;
	CX(ID_BUT_CUT); SHOW(5, 25);                           /* Cut */
	CX(ID_BUT_COPY); SHOW(6, 25);                          /* Copy */
	ecx |= 0x40000000u;
	if (!*(u32 *)buf) ecx &= 0xffff;
	CX(ID_BUT_PASTE); SHOW(7, 25);                         /* Paste */
	ecx |= 0x40000000u;
	CX(ID_BUT_FIND); SHOW(8, 25);                          /* Find */
	CX(ID_BUT_REPLACE); SHOW(9, 25);                       /* Replace */
	CX(ID_BUT_KEY_WORDS); SHOW(10, 25);                    /* Key words */
	if (!ted_is_select(edi)) ecx &= 0xffff;
	CX(ID_BUT_UPPER); SHOW(11, 30);                        /* Upper */
	CX(ID_BUT_LOWER); SHOW(12, 25);                        /* Lower */
	CX(ID_BUT_REVERSE); SHOW(13, 25);                      /* Reverse */
	ecx |= 0x40000000u;
	if (!((s32)edi->tim_ch > (s32)edi->tim_undo)) ecx &= 0xffff;
	CX(ID_BUT_UNDO); SHOW(14, 30);                         /* Undo */
	ecx |= 0x40000000u;
	if ((s32)edi->tim_undo < 1) ecx &= 0xffff;
	CX(ID_BUT_REDO); SHOW(15, 25);                         /* Redo */
	ecx |= 0x40000000u;
	CX(ID_BUT_INVISIBLE); SHOW(16, 30);                    /* the invisible ones on and off */
	CX(ID_BUT_SYNTAX_LIST); SHOW(17, 25);                  /* the syntax file */
	CX(ID_BUT_SYNTAX_MODE); SHOW(18, 25);                  /* colouring on and off */
	CX(ID_BUT_CONVERT_1251_866); SHOW(19, 30);             /* cp 1251 -> 866 */
	CX(ID_BUT_CONVERT_866_1251); SHOW(20, 25);             /* cp 866 -> 1251 */
}

/* the panel's background above and below its edit box */
static void panel_bg(text_editor *edi)
{
	kos_call(13, TED_PANEL_WIDTH, edi->wnd.top << 16 | 20, sc.work, 0, 0);
	u32 ecx = edi->wnd.top;
	ecx = (ecx & 0xffff0000u) | (u16)(ecx + 20 + 15);    /* 15: the text box's height */
	ecx <<= 16;
	ecx += edi->wnd.height;
	ecx = (ecx & 0xffff0000u) | (u16)((u16)ecx + edi->scr_h->y_size - (20 + 15 - 1));
	kos_call(13, TED_PANEL_WIDTH, ecx, sc.work, 0, 0);
}

static void p_find(text_editor *edi)
{
	panel_bg(edi);
	edit_box_draw(&edit_find);
	u32 ecx = (edi->wnd.top << 16) + (111 << 16) + 20;
	panel_button(5 << 16 | 85, ecx, 201);
	text_z((23 << 16 | 7) + edi->wnd.top, sc.work_text, txtFindCapt);
	text_z((15 << 16 | 118) + edi->wnd.top, sc.work_button_text, txtFindNext);
	option_box_draw(opt_grlist1);
	check_box_draw2(&ch1);
}

static void p_replace(text_editor *edi)
{
	panel_bg(edi);
	edit_box_draw(&edit_replace);
	edit_box_draw(&edit_find);
	u32 ecx = (edi->wnd.top << 16) + (133 << 16) + 20;
	panel_button(5 << 16 | 95, ecx, 202);   /* replace next */
	ecx += 26 << 16;
	panel_button(5 << 16 | 95, ecx, 203);   /* replace all */
	u32 ebx = edi->wnd.top + (23 << 16 | 7);
	text_z(ebx, sc.work_text, txtFindCapt);
	text_z(ebx + 93, sc.work_text, txtRerlaceCapt);
	ebx = edi->wnd.top + (15 << 16 | 140);
	text_z(ebx, sc.work_button_text, txtRerlace);
	text_z(ebx + 26, sc.work_button_text, txtRerlaceAll);
	option_box_draw(opt_grlist1);
}

static void p_goto(text_editor *edi)
{
	panel_bg(edi);
	edit_box_draw(&edit_goto);
	u32 ecx = (edi->wnd.top << 16) + (40 << 16) + 20;
	panel_button(5 << 16 | 85, ecx, 204);
	text_z((23 << 16 | 7) + edi->wnd.top, sc.work_text, txtGotoCapt);
	text_z((15 << 16 | 47) + edi->wnd.top, sc.work_button_text, txtGoto);
}

static void p_syntax(text_editor *edi)
{
	u32 ecx = edi->wnd.top << 16 | 20;
	kos_call(13, TED_PANEL_WIDTH, ecx, sc.work, 0, 0);    /* the top background */
	tl_draw(&tree1);
	ws_dir_lbox.all_redraw = 1;                            /* the child scroll bar all */
	scrollbar_v_draw(&ws_dir_lbox);
	ecx = (edi->wnd.top + tree1.box_height + 20) << 16;
	ecx += edi->wnd.height;
	ecx = (ecx & 0xffff0000u) | (u16)((u16)ecx + edi->scr_h->y_size - 20 - tree1.box_height + 1);
	kos_call(13, TED_PANEL_WIDTH, ecx, sc.work, 0, 0);    /* the bottom background */
	ecx = (edi->wnd.top + 25 + tree1.box_height) << 16 | 20;
	panel_button(5 << 16 | 65, ecx, 200);
	text_z((30 << 16 | 5) + edi->wnd.top, sc.work_text, txtFormatCapt);
	text_z((10 << 16 | 31) + tree1.box_height + edi->wnd.top, sc.work_button_text, txtFormatApply);
}

void draw_panels(text_editor *edit)
{
	switch (edit->panel_id) {
	case TED_PANEL_FIND:
		kos_button_delete(200);
		kos_button_delete(202);
		kos_button_delete(203);
		kos_button_delete(204);
		p_find(edit);
		break;
	case TED_PANEL_REPLACE:
		kos_button_delete(200);
		kos_button_delete(201);
		kos_button_delete(204);
		p_replace(edit);
		break;
	case TED_PANEL_GOTO:
		kos_button_delete(200);
		kos_button_delete(201);
		kos_button_delete(202);
		kos_button_delete(203);
		p_goto(edit);
		break;
	case TED_PANEL_SYNTAX:
		kos_button_delete(201);
		kos_button_delete(202);
		kos_button_delete(203);
		kos_button_delete(204);
		p_syntax(edit);
		break;
	}
}

/* the editor's size from the window's */
void EvSize(void)
{
	text_editor *edi = &tedit0;
	scrollbar *ebx = edi->scr_h, *esi = edi->scr_w;
	edi->wnd.width = PI_CLIENT_W - edi->wnd.left - esi->x_size;   /* minus the panel, the scroll bar */
	edi->wnd.height = PI_CLIENT_H;
	if (!((s32)edi->wnd.height > MIN_M_WND_H)) edi->wnd.height = MIN_M_WND_H;
	edi->wnd.height -= ebx->y_size;        /* minus the horizontal scroll bar */
	edi->wnd.height -= edi->wnd.top;       /* and the top */
	ted_init_scroll_bars(&tedit0, 2);
	tree1.box_top = edi->wnd.top + 20;
	edit_find.top = edi->wnd.top + 20;
	edit_replace.top = edi->wnd.top + 113;
	edit_goto.top = edi->wnd.top + 20;
}

/* the gray icons: (r + g + b) / 3 */
void img_to_gray(const u8 *buf_rgb, u8 *buf_g24, u32 pixels)
{
	for (; pixels; pixels--, buf_rgb += 3, buf_g24 += 3) {
		u8 g = (buf_rgb[0] + buf_rgb[1] + buf_rgb[2]) / 3;
		buf_g24[0] = buf_g24[1] = buf_g24[2] = g;
	}
}
