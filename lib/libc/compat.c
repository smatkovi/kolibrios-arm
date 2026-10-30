/* KolibriOS for ARM: the C library (lib/libc): lib/klib.c's functions
 * for programs that include lib/kos.h too */
#include <stdio.h>
#include <stdarg.h>

typedef unsigned u32;

char *utoa(u32 v, char *buf, int base)
{
	sprintf(buf, base == 16 ? "%x" : base == 8 ? "%o" : "%u", v);
	return buf;
}

int ksprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

void kdebug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
