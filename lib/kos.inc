; KolibriOS for ARM: the system functions for programs in assembler, for
; FASMARM (flat assembler for ARM; tools/fetch-fasmarm.sh, docs/asm.md).
; What lib/kos.h is for C: KolibriOS's "int 0x40" with eax, ebx, ecx,
; edx, esi, edi is "svc 0x40" with r0-r5; the numbers and meanings are
; KolibriOS's (its docs/sysfuncs.txt), the names those of its
; KOSfuncs.inc. Programs are ARM code (AArch32, ARMv7-A), from address 0
; of their own memory, also under the AArch64 kernel (the Pro1X).
;
;	include 'kos.inc'
;	KOS_APP start, i_end, 0x100000	; entry, the image's end, memory
;	start:
;		...
;		mcall	SF_TERMINATE_PROCESS
;
; FASMARM's data sizes are ARM's: db 8 bits, dh 16, dw 32 (FASM's dd),
; dd 64. mcall's arguments are numbers, labels or registers; a number is
; loaded with movw (and movt above 0xffff), a register moved if it is not
; already the one. r0-r5 are loaded in that order: a register argument
; is best the one it goes to, or one of r6-r12 (r1 given for ecx would
; already hold ebx's value). r0 comes back with the result (r1 for
; some), r0-r5 may change; the rest stay.

	processor cpu32_all
	coprocessor copro_all
	code32

; --- the call ---------------------------------------------------------------

macro __mov reg, val
{
  if val eqtype r0
    if ~ val eq reg
	mov	reg, val
    end if
  else if (val) = -1
	mvn	reg, 0
  else
	movw	reg, (val) and 0xffff
    if ((val) shr 16) and 0xffff
	movt	reg, ((val) shr 16) and 0xffff
    end if
  end if
}

; a register from a number or a label (the program runs at 0: labels are
; addresses), e.g. "movi r1, title"
macro movi reg, val
{
	__mov	reg, val
}

; r0-r5 from the arguments that are given, then the call
macro mcall a, b, c, d, e, f
{
  if ~ a eq
	__mov	r0, a
  end if
  if ~ b eq
	__mov	r1, b
  end if
  if ~ c eq
	__mov	r2, c
  end if
  if ~ d eq
	__mov	r3, d
  end if
  if ~ e eq
	__mov	r4, e
  end if
  if ~ f eq
	__mov	r5, f
  end if
	svc	0x40
}

; --- the header (kernel/loader.c, lib/crt0.S) --------------------------------
; "MENUETA1", version 1, the entry, the end of the image, the memory the
; program gets (a whole number of MiB is what it gets), the stack's top,
; a 256-byte buffer for the parameters and a 1 KiB one for the path (0:
; none). With org 0: the program runs from address 0.

macro KOS_APP entry, img_end, memory, stack, params, path
{
	org	0
	db	'MENUETA1'
	dw	1, entry, img_end, memory
  if stack eq
	dw	(memory) - 16
  else
	dw	stack
  end if
  if params eq
	dw	0
  else
	dw	params
  end if
  if path eq
	dw	0
  else
	dw	path
  end if
}

; --- the functions (KolibriOS's KOSfuncs.inc) --------------------------------

SF_TERMINATE_PROCESS	= -1
SF_CREATE_WINDOW	= 0
SF_PUT_PIXEL		= 1
SF_GET_KEY		= 2
SF_GET_SYS_TIME		= 3
SF_DRAW_TEXT		= 4
SF_SLEEP		= 5
SF_PUT_IMAGE		= 7
SF_DEFINE_BUTTON	= 8
SF_THREAD_INFO		= 9
SF_WAIT_EVENT		= 10
SF_CHECK_EVENT		= 11
SF_REDRAW		= 12
  SSF_BEGIN_DRAW	= 1
  SSF_END_DRAW		= 2
SF_DRAW_RECT		= 13
SF_GET_SCREEN_SIZE	= 14
SF_GET_BUTTON		= 17
SF_SYSTEM		= 18
  SSF_TERMINATE_THREAD	= 2
  SSF_FOCUS_WINDOW	= 3
  SSF_GET_ACTIVE_WINDOW	= 7
  SSF_GET_FREE_RAM	= 16
  SSF_GET_TOTAL_RAM	= 17
SF_WAIT_EVENT_TIMEOUT	= 23
SF_SYSTEM_GET		= 26
  SSF_TIME_COUNT	= 9
SF_GET_SYS_DATE		= 29
SF_GET_IMAGE		= 36
SF_MOUSE_GET		= 37
  SSF_SCREEN_POSITION	= 0
  SSF_WINDOW_POSITION	= 1
  SSF_BUTTON		= 2
SF_DRAW_LINE		= 38
SF_SET_EVENTS_MASK	= 40
SF_DRAW_NUMBER		= 47
SF_STYLE_SETTINGS	= 48
  SSF_GET_SKIN_HEIGHT	= 4
SF_BOARD		= 63
  SSF_DEBUG_WRITE	= 1
  SSF_DEBUG_READ	= 2
SF_CHANGE_WINDOW	= 67
SF_SYS_MISC		= 68
  SSF_SWITCH_TASK	= 1
  SSF_HEAP_INIT		= 11
  SSF_MEM_ALLOC		= 12
SF_FILE			= 70
SF_SET_CAPTION		= 71

; the events (functions 10, 11, 23) and their masks (function 40)
EV_IDLE			= 0
EV_REDRAW		= 1
EV_KEY			= 2
EV_BUTTON		= 3
EV_BACKGROUND		= 5
EV_MOUSE		= 6
EV_IPC			= 7
EV_NETWORK		= 8
EV_DEBUG		= 9
EVM_REDRAW		= 1
EVM_KEY			= 2
EVM_BUTTON		= 4
EVM_MOUSE		= 0x20

; window styles (function 0's edx, as kos.h's): skinned, with its
; caption, coordinates relative to the work area; plus the work colour
KOS_WIN_SKINNED		= 0x33000000
KOS_WIN_FIXED		= 0x34000000
KOS_WIN_PLAIN		= 0x01000000

; text (function 4's ecx): a terminated string, fonts 0 (6x10) and 1
; (8x16, cp437), 3 (8x16, UTF-8), its background (edi) drawn, scale
KOS_TEXT_ASCIIZ		= 0x80000000
KOS_TEXT_FILL		= 0x40000000
KOS_FONT_6x10		= 0x00000000
KOS_FONT_8x16		= 0x10000000
KOS_FONT_UTF8		= 0x30000000
KOS_TEXT_SCALE		= 0x01000000	; times (scale - 1)

; --- the C header's helpers, for numbers and labels --------------------------
; (their arguments are packed at assembly time: numbers and labels; for
; values in registers, mcall with the packed registers)

macro kos_exit
{
	mcall	SF_TERMINATE_PROCESS
}

macro kos_begin_draw
{
	mcall	SF_REDRAW, SSF_BEGIN_DRAW
}

macro kos_end_draw
{
	mcall	SF_REDRAW, SSF_END_DRAW
}

; a window: x, y, width, height, style + work colour, title (label)
macro kos_window x, y, w, h, style, title
{
	mcall	SF_CREATE_WINDOW, ((x) shl 16) + (w) - 1, ((y) shl 16) + (h) - 1, style, 0, title
}

; a terminated string in font 0, 1 or 3 (0x80000000: kos_text's flag)
macro kos_text x, y, str, color, font
{
	mcall	SF_DRAW_TEXT, ((x) shl 16) + ((y) and 0xffff), \
		KOS_TEXT_ASCIIZ + ((font) shl 28) + ((color) and 0xffffff), str
}

; ... on its background
macro kos_text_bg x, y, str, color, font, bg
{
	mcall	SF_DRAW_TEXT, ((x) shl 16) + ((y) and 0xffff), \
		KOS_TEXT_ASCIIZ + KOS_TEXT_FILL + ((font) shl 28) + ((color) and 0xffffff), str, 0, bg
}

; ... larger: scale 1..8
macro kos_text_scaled x, y, str, color, font, scale
{
	mcall	SF_DRAW_TEXT, ((x) shl 16) + ((y) and 0xffff), \
		KOS_TEXT_ASCIIZ + ((font) shl 28) + (((scale) - 1) shl 24) + ((color) and 0xffffff), str
}

macro kos_rect x, y, w, h, color
{
	mcall	SF_DRAW_RECT, ((x) shl 16) + (w), ((y) shl 16) + (h), color
}

macro kos_line x0, y0, x1, y1, color
{
	mcall	SF_DRAW_LINE, ((x0) shl 16) + ((x1) and 0xffff), ((y0) shl 16) + ((y1) and 0xffff), color
}

; a button: x, y, width, height, identifier (24 bits), colour
macro kos_button x, y, w, h, id, color
{
	mcall	SF_DEFINE_BUTTON, ((x) shl 16) + (w) - 1, ((y) shl 16) + (h) - 1, id, color
}

; a number (a value, or a register: r2 or r6-r12) in decimal, without
; leading zeros, up to digits long; font as in kos_text
macro kos_number x, y, value, digits, color, font
{
	mcall	SF_DRAW_NUMBER, ((digits) shl 16) + 0x80000000, value, ((x) shl 16) + ((y) and 0xffff), \
		((font) shl 28) + ((color) and 0xffffff)
}

macro kos_wait_event
{
	mcall	SF_WAIT_EVENT
}

macro kos_wait_event_timeout t
{
	mcall	SF_WAIT_EVENT_TIMEOUT, t
}

macro kos_get_key		; r0: 1 if none, else (ascii shl 8) + (scan code shl 16)
{
	mcall	SF_GET_KEY
}

macro kos_get_button		; r0: 1 if none, else (identifier shl 8) + mouse buttons
{
	mcall	SF_GET_BUTTON
}

macro kos_time			; r0: 0x00SSMMHH in BCD
{
	mcall	SF_GET_SYS_TIME
}

macro kos_date			; r0: 0x00DDMMYY in BCD
{
	mcall	SF_GET_SYS_DATE
}

macro kos_debug_char c		; on the debug board (the kernel's console)
{
	mcall	SF_BOARD, SSF_DEBUG_WRITE, c
}
