/* KolibriOS for ARM: libini, KolibriOS's library for settings files
 * (programs/develop/libraries/libs-dev/libini, by Alexey "mike.dld"
 * Tsaplin and the KolibriOS team, GPL-2.0+): its functions with their
 * arguments and results, in C, linked into the program (lib/libini).
 *
 * A file of sections "[name]" with lines "key=value"; ';' starts a
 * comment; names compare as they are written (case matters). */
#ifndef LIBINI_H
#define LIBINI_H

/* callbacks: return non-zero to go on, 0 to stop */
typedef int (*ini_sec_cb)(const char *f_name, const char *sec_name);
typedef int (*ini_key_cb)(const char *f_name, const char *sec_name, const char *key_name, const char *key_value);

int ini_enum_sections(const char *f_name, ini_sec_cb callback);
int ini_enum_keys(const char *f_name, const char *sec_name, ini_key_cb callback);
/* 0, or -1 with def_val in buffer */
int ini_get_str(const char *f_name, const char *sec_name, const char *key_name, char *buffer, int buf_len, const char *def_val);
int ini_set_str(const char *f_name, const char *sec_name, const char *key_name, const char *buffer, int buf_len);
int ini_get_int(const char *f_name, const char *sec_name, const char *key_name, int def_val);
int ini_set_int(const char *f_name, const char *sec_name, const char *key_name, int val);
/* "r,g,b" as 0xRRGGBB */
unsigned ini_get_color(const char *f_name, const char *sec_name, const char *key_name, unsigned def_val);
int ini_set_color(const char *f_name, const char *sec_name, const char *key_name, unsigned val);
/* "Ctrl+Shift+F5", "Alt+X": the key's scan code, the modifiers as
 * function 66.4 has them (Shift 0x00f, Ctrl 0x0f0, Alt 0xf00: 1 either,
 * 2 both, 3 left, 4 right) */
int ini_get_shortcut(const char *f_name, const char *sec_name, const char *key_name, int def_val, unsigned *modifiers);
int ini_del_section(const char *f_name, const char *sec_name);

#endif
