/* KolibriOS for ARM: the console library, KolibriOS's console.obj
 * (programs/develop/libraries/console_coff, by diamond (Evgeny
 * Grechnikov) and the KolibriOS team, GPL-2.0) in C, linked into the
 * program (lib/console). Its functions, as console_en.txt describes them
 * and ktcc's conio.h declares them; con_init_opt is ktcc's: con_init with
 * the sizes, -1 for the defaults (80 x 25 of 80 x 300 characters).
 *
 * KolibriOS's library draws the console window from a thread of its own;
 * here the program's thread does it whenever it calls the library: the
 * window redraws, takes keys and scrolls while the program writes, waits
 * in con_getch/con_gets/con_kbhit, or after con_exit(0) until it is
 * closed. Closing the window ends the program, as in KolibriOS. */
#ifndef CONIO_H
#define CONIO_H

typedef unsigned int dword;
typedef unsigned short word;

/* text colour */
#define CON_COLOR_BLUE    0x01
#define CON_COLOR_GREEN   0x02
#define CON_COLOR_RED     0x04
#define CON_COLOR_BRIGHT  0x08
/* background colour */
#define CON_BGR_BLUE      0x10
#define CON_BGR_GREEN     0x20
#define CON_BGR_RED       0x40
#define CON_BGR_BRIGHT    0x80
/* output: special characters and Esc sequences printed as they are */
#define CON_IGNORE_SPECIALS 0x100
/* the window was closed */
#define CON_WINDOW_CLOSED 0x200

int con_init(void);
int con_init_opt(dword wnd_width, dword wnd_height, dword scr_width, dword scr_height, const char *title);
void con_exit(int bCloseWindow);
void con_set_title(const char *title);
void con_write_asciiz(const char *str);
void con_write_string(const char *str, dword length);
int con_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
dword con_get_flags(void);
dword con_set_flags(dword new_flags);
int con_get_font_height(void);
int con_get_cursor_height(void);
int con_set_cursor_height(int new_height);
int con_getch(void);
word con_getch2(void);
int con_kbhit(void);
char *con_gets(char *str, int n);
typedef int (*con_gets2_callback)(int keycode, char **pstr, int *pn, int *ppos);
char *con_gets2(con_gets2_callback callback, char *str, int n);
void con_cls(void);
void con_get_cursor_pos(int *px, int *py);
void con_set_cursor_pos(int x, int y);
int con_get_input(char *buf, int buflen);

/* KolibriOS's C programs call the library through pointers set by
 * con_init_opt; here the functions are there from the start */
extern int __con_is_load;

#endif
