/* KolibriOS for ARM: msgbox, KolibriOS's message boxes, translated to C
 * from programs/develop/libraries/msgbox/msgbox.asm (IgorA, the
 * KolibriOS team; GPL-2.0). The same data and functions:
 *
 *   data: db the button focused at first (1 ...), 0
 *         the caption, 0
 *         the text (lines ended by 13), 0
 *         the buttons' captions, each ended by 0, then a 0
 *   mb_create(data, thread)  shows it; data[0] gets the button pressed
 *                            (1 ...; 0: closed)
 *   mb_setfunctions(funcs)   funcs[i]: called after button i + 1
 *
 * In KolibriOS the box is a window of its own in a thread (thread is
 * that thread's stack); this kernel has no threads, so the box is drawn
 * in the program's window and the program gives it its events: a
 * program calls mb_event(event) first in its loop and draws the box
 * with mb_draw() at the end of its redraw (KolibriOS for ARM
 * additions). */
#ifndef MSGBOX_H
#define MSGBOX_H

typedef void (*mb_function)(void);

void mb_create(char *data, void *thread);
void mb_reinit(char *data);
void mb_setfunctions(mb_function *funcs);

/* 0: not the box's (no box, or a redraw: the program redraws and calls
 * mb_draw), 1: taken by the box, 2: the box closed (its function
 * called): the program redraws its window */
int mb_event(int event);
void mb_draw(void);
int mb_is_open(void);

#endif
