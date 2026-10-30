/* KolibriOS for ARM: msgbox, KolibriOS's message boxes, translated to C
 * from programs/develop/libraries/msgbox/msgbox.asm (IgorA, the
 * KolibriOS team; GPL-2.0). The same data and functions:
 *
 *   data: db the button focused at first (1 ...), 0
 *         the caption, 0
 *         the text (lines ended by 13), 0
 *         the buttons' captions, each ended by 0, then a 0
 *   mb_create(data, thread)  shows it: a thread of the program (51.1)
 *                            with a window of its own, on the stack
 *                            thread (its top: memory of the program's);
 *                            the caller goes on at once. When a button
 *                            is pressed, data[0] gets it (1 ...; 0: the
 *                            window closed) and the box's thread calls
 *                            its function, then ends
 *   mb_reinit(data)          the box's measures from data again
 *   mb_setfunctions(funcs)   funcs[i]: called after button i + 1 (by
 *                            the box's thread), for the box created last */
#ifndef MSGBOX_H
#define MSGBOX_H

typedef void (*mb_function)(void);

void mb_create(char *data, void *thread);
void mb_reinit(char *data);
void mb_setfunctions(mb_function *funcs);

#endif
