/* KolibriOS for ARM: box_lib's system functions (lib/boxlib), the ones
 * its assembler called with mcall, by KOSfuncs.inc's names */
#ifndef BL_SYS_H
#define BL_SYS_H

#include <kos.h>
#include "box_lib.h"

typedef short s16;
typedef signed char s8;

#define SF_DRAW_TEXT 4
#define SF_PUT_IMAGE 7
#define SF_DEFINE_BUTTON 8
#define SF_DRAW_RECT 13
#define SF_SYSTEM_GET 26
#define SF_MOUSE_GET 37
#define SF_DRAW_LINE 38
#define SF_CLIPBOARD 54
#define SF_KEYBOARD 66
#define SF_SYS_MISC 68

/* function 13: ebx = x << 16 | w, ecx = y << 16 | h */
static inline void bl_rect(u32 ebx, u32 ecx, u32 color) { kos_call(SF_DRAW_RECT, ebx, ecx, color, 0, 0); }
/* function 38: ebx = x0 << 16 | x1, ecx = y0 << 16 | y1 */
static inline void bl_line(u32 ebx, u32 ecx, u32 color) { kos_call(SF_DRAW_LINE, ebx, ecx, color, 0, 0); }
/* function 4: ebx = x << 16 | y, ecx = colour and flags, esi = length */
static inline void bl_text(u32 ebx, u32 ecx, const char *s, u32 len) { kos_call(SF_DRAW_TEXT, ebx, ecx, (u32)s, len, 0); }
static inline void bl_text_bg(u32 ebx, u32 ecx, const char *s, u32 len, u32 bg) { kos_call(SF_DRAW_TEXT, ebx, ecx, (u32)s, len, bg); }

/* function 37: 1 the position in the window, 2 the buttons, 3 the
 * buttons and their events, 7 the wheel */
static inline u32 bl_mouse_pos(void) { return kos_call(SF_MOUSE_GET, 1, 0, 0, 0, 0); }
static inline u32 bl_mouse_buttons(void) { return kos_call(SF_MOUSE_GET, 2, 0, 0, 0, 0); }
static inline u32 bl_mouse_buttons_ext(void) { return kos_call(SF_MOUSE_GET, 3, 0, 0, 0, 0); }
static inline u32 bl_mouse_scroll(void) { return kos_call(SF_MOUSE_GET, 7, 0, 0, 0, 0); }

/* function 66.3: the control keys (bit 0-1 Shift, 2-3 Ctrl, 4-5 Alt, 6
 * CapsLock, 7 NumLock, 8 ScrollLock, 9-10 Win) */
static inline u32 bl_control_keys(void) { return kos_call(SF_KEYBOARD, 3, 0, 0, 0, 0); }

/* function 54: the clipboard. A slot: u32 its size (this header
 * included), u32 the type (0 text), u32 the encoding (1 cp866), the data */
struct bl_clip { u32 cbSize, contentType, encoding; char data_[]; };
static inline int bl_clip_count(void) { return kos_call(SF_CLIPBOARD, 0, 0, 0, 0, 0); }
static inline struct bl_clip *bl_clip_read(int slot) { return (struct bl_clip *)kos_call(SF_CLIPBOARD, 1, slot, 0, 0, 0); }
static inline int bl_clip_write(const void *p, u32 size) { return kos_call(SF_CLIPBOARD, 2, size, (u32)p, 0, 0); }
/* the slot read is in the program's heap (function 68.12): 68.13 frees it */
static inline void bl_sys_free(void *p) { kos_call(SF_SYS_MISC, 13, (u32)p, 0, 0, 0); }

#endif
