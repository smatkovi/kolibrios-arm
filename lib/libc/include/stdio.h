/* KolibriOS for ARM: the C library of the ported programs (lib/libc).
 * printf and puts write to the debug board (function 63); programs with
 * a console use the console library's con_printf (lib/console) */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define FILENAME_MAX 1024
typedef struct _kos_file FILE;

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsprintf(char *buf, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int sscanf(const char *s, const char *fmt, ...);

#endif
