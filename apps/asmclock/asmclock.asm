; asmclock: a digital clock in assembler (FASMARM, ARM state): the time
; large (function 4, scaled), the colon blinking with the seconds, the
; date below, and the minute's seconds as a bar whose colours NEON works
; out, eight pixels at a time, into an image of B, G, R bytes (function
; 7). Its NEON registers are its own: the kernel saves them lazily
; (arch/arm64/fpu.c). Redrawn every half second (function 23).

	include 'kos.inc'

	KOS_APP start, i_end, 0x100000

BG	= 0x101418			; the work area
BAR_X	= 12
BAR_Y	= 112
BAR_W	= 280				; a multiple of 8
BAR_H	= 12

start:
redraw:
	kos_begin_draw
	kos_window 100, 80, 316, 166, KOS_WIN_SKINNED + BG, title
	kos_end_draw
	bl	draw

events:
	kos_wait_event_timeout 50
	cmp	r0, EV_REDRAW
	beq	redraw
	cmp	r0, EV_KEY
	beq	key
	cmp	r0, EV_BUTTON
	beq	button
	bl	draw				; half a second
	b	events

key:
	kos_get_key
	ubfx	r0, r0, 8, 8
	cmp	r0, 27				; Escape
	beq	quit
	b	events

button:
	kos_get_button
	lsr	r0, r0, 8
	cmp	r0, 1				; the close button
	bne	events
quit:
	kos_exit

; two ASCII digits of a BCD byte (r1) at r5, r5 moves on
bcd2:
	lsr	r2, r1, 4
	and	r2, r2, 15
	add	r2, r2, '0'
	strb	r2, [r5], 1
	and	r2, r1, 15
	add	r2, r2, '0'
	strb	r2, [r5], 1
	bx	lr

draw:
	push	{r4-r7, lr}
	kos_time				; 0x00SSMMHH
	mov	r4, r0
	movi	r5, clock
	and	r1, r4, 0xff
	bl	bcd2
	tst	r4, 0x10000			; an odd second: no colon
	moveq	r6, ':'
	movne	r6, ' '
	strb	r6, [r5], 1
	ubfx	r1, r4, 8, 8
	bl	bcd2
	strb	r6, [r5], 1
	ubfx	r1, r4, 16, 8
	bl	bcd2
	movi	r3, clock
	mcall	SF_DRAW_TEXT, (18 shl 16) + 8, \
		KOS_TEXT_ASCIIZ + KOS_TEXT_FILL + KOS_FONT_8x16 + 3 * KOS_TEXT_SCALE + 0xe0f0ff, r3, 0, BG
	kos_date				; 0x00DDMMYY
	movi	r5, date
	ubfx	r1, r0, 16, 8
	mov	r4, r0
	bl	bcd2
	add	r5, r5, 1			; the dot
	ubfx	r1, r4, 8, 8
	bl	bcd2
	add	r5, r5, 3			; ".20"
	and	r1, r4, 0xff
	bl	bcd2
	kos_text_bg 112, 84, date, 0xa0b0c0, 1, BG
	; the bar: (seconds + 1) * BAR_W / 60 pixels, that is * 14 / 3
	kos_time				; the seconds again (r4 holds the date)
	ubfx	r1, r0, 16, 8
	lsr	r2, r1, 4
	and	r1, r1, 15
	add	r2, r2, r2, lsl 2
	add	r1, r1, r2, lsl 1		; binary
	add	r1, r1, 1
	mov	r2, 14
	mul	r1, r1, r2
	movw	r2, 21846			; / 3: * 21846 / 65536
	mul	r1, r1, r2
	lsr	r6, r1, 16			; the filled width
	bl	bar_colours
	mov	r7, 0				; the line (r0-r5 are the calls')
.row:	cmp	r6, 0
	beq	.empty
	movi	r1, image
	lsl	r2, r6, 16
	orr	r2, r2, 1			; r6 x 1 pixels
	movw	r3, BAR_Y
	add	r3, r3, r7
	orr	r3, r3, BAR_X shl 16
	mcall	SF_PUT_IMAGE, r1, r2, r3
.empty:	add	r7, r7, 1
	cmp	r7, BAR_H
	blo	.row
	; the rest of the bar dark
	rsb	r2, r6, BAR_W
	cmp	r2, 0
	beq	.done
	add	r1, r6, BAR_X
	lsl	r1, r1, 16
	orr	r1, r1, r2
	mcall	SF_DRAW_RECT, r1, (BAR_Y shl 16) + BAR_H, 0x282c34
.done:	pop	{r4-r7, pc}

; the bar's line of pixels: blue falling, green and red rising along x;
; eight pixels a step in 16-bit lanes (q8: x .. x + 7), narrowed to
; bytes (vqmovn: FASMARM 1.44 encodes vmovn.iN with N the result's
; element size, ARM's vmovn.i16 is its vmovn.i8; docs/asm.md), stored
; interleaved as B, G, R (vst3.8)
bar_colours:
	movi	r0, ramp
	vld1.16	{d16, d17}, [r0]		; 0, 1, .. 7
	vmov.i16 q10, 8
	vmov.i8	d6, 0xff
	vmov.i8	d7, 0x40
	movi	r1, image
	mov	r2, BAR_W / 8
.step:	vshr.u16 q9, q8, 1			; x / 2
	vqmovn.u16 d4, q9
	vsub.i8	d0, d6, d4			; blue: 255 - x / 2
	vshr.u16 q11, q8, 2
	vqmovn.u16 d5, q11
	vadd.i8	d1, d5, d7			; green: 64 + x / 4
	vadd.i8	d2, d4, d7			; red: 64 + x / 2
	vst3.8	{d0, d1, d2}, [r1]!
	vadd.i16 q8, q8, q10
	subs	r2, r2, 1
	bne	.step
	bx	lr

	align	8
ramp	dh 0, 1, 2, 3, 4, 5, 6, 7
title	db 'Clock (assembler)', 0
clock	db 'HH:MM:SS', 0
date	db 'DD.MM.20YY', 0
	align	16
image	rb BAR_W * 3
i_end:
