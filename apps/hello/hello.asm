; hello: KolibriOS's first program, in assembler (FASMARM, ARM state):
; a window with its title and text, two buttons, the clicks counted,
; the last key shown (Enter clicks too, Escape closes). The same as apps/example in C, the KolibriOS way:
; an event loop over function 10, a redraw between functions 12.1 and
; 12.2. Built by the Makefile with tools/fetch-fasmarm.sh's FASMARM.

	include 'kos.inc'

	KOS_APP start, i_end, 0x100000

start:
redraw:
	kos_begin_draw
	kos_window 120, 60, 360, 200, KOS_WIN_SKINNED + 0xf0f0f0, title
	kos_text 12, 10, line1, 0x202020, 1
	kos_text 12, 28, line2, 0x202020, 1
	kos_button 12, 60, 140, 28, 10, 0x6a9ad8
	kos_text 30, 66, click, 0xffffff, 1
	kos_button 170, 60, 140, 28, 11, 0xc86060
	kos_text 204, 66, close, 0xffffff, 1
	bl	show_clicks
	bl	show_key
	kos_end_draw

events:
	kos_wait_event
	cmp	r0, EV_REDRAW
	beq	redraw
	cmp	r0, EV_KEY
	beq	key
	cmp	r0, EV_BUTTON
	bne	events
	kos_get_button
	lsr	r0, r0, 8			; the identifier
	cmp	r0, 1				; the window's close button
	cmpne	r0, 11				; or ours
	beq	quit
	cmp	r0, 10
	bne	events
click_it:
	movi	r2, clicks_n
	ldr	r1, [r2]
	add	r1, r1, 1
	str	r1, [r2]
	bl	show_clicks
	b	events

key:
	kos_get_key
	lsr	r0, r0, 8
	and	r0, r0, 0xff			; the ASCII code
	cmp	r0, 27				; Escape: as Close
	beq	quit
	movi	r2, key_n
	str	r0, [r2]
	bl	show_key
	ldr	r0, [key_n]
	cmp	r0, 13				; Enter: as Click me
	beq	click_it
	b	events

quit:
	kos_exit

; "Clicks: N" on the window's colour (the number over the old one)
show_clicks:
	push	{lr}
	kos_text_bg 12, 104, clicks_s, 0x103060, 1, 0xf0f0f0
	ldr	r2, [clicks_n]			; ecx: the number
	mcall	SF_DRAW_NUMBER, (6 shl 16) + 0x80000000, r2, (84 shl 16) + 104, \
		KOS_FONT_8x16 + KOS_TEXT_FILL + 0x103060, 0xf0f0f0
	pop	{pc}

; "Key: x, code N" once there was one
show_key:
	push	{lr}
	ldr	r1, [key_n]
	cmp	r1, 0
	beq	.none
	movi	r2, keytext
	strb	r1, [r2, 6]
	kos_text_bg 12, 128, keytext, 0x103060, 1, 0xf0f0f0
	ldr	r2, [key_n]
	mcall	SF_DRAW_NUMBER, (3 shl 16) + 0x80000000, r2, (148 shl 16) + 128, \
		KOS_FONT_8x16 + KOS_TEXT_FILL + 0x103060, 0xf0f0f0
	pop	{pc}
.none:
	kos_text_bg 12, 128, nokey, 0x103060, 1, 0xf0f0f0
	pop	{pc}

title	db 'Hello (assembler)', 0
line1	db 'KolibriOS on ARM: a program in', 0
line2	db 'assembler, FASMARM, svc 0x40.', 0
click	db 'Click me', 0
close	db 'Close', 0
clicks_s db 'Clicks: ', 0
keytext	db "Key: '?', code ", 0
nokey	db 'Press a key', 0
	align	4
clicks_n dw 0
key_n	dw 0
i_end:
