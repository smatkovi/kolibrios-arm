/* KolibriOS for ARM: the C library (lib/libc): the current folder
 * (function 30.1, 30.2) */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
#endif
