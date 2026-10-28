/* KolibriOS for ARM: the little C library of programs */
#include <stdarg.h>
#include "kos.h"

void *memset(void *d, int c, unsigned n)
{
	u8 *p = d;
	while (n--) *p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
	u8 *p = d;
	const u8 *q = s;
	while (n--) *p++ = *q++;
	return d;
}

unsigned strlen(const char *s)
{
	unsigned n = 0;
	while (s[n]) n++;
	return n;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (u8)*a - (u8)*b;
}

char *utoa(u32 v, char *buf, int base)
{
	char t[12];
	int i = 0;
	do {
		int d = v % base;
		t[i++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	} while (v);
	char *p = buf;
	while (i) *p++ = t[--i];
	*p = 0;
	return buf;
}

static int vformat(char *out, const char *fmt, va_list ap)
{
	char *o = out;
	for (; *fmt; fmt++) {
		if (*fmt != '%') { *o++ = *fmt; continue; }
		fmt++;
		int width = 0;
		char pad = ' ';
		if (*fmt == '0') { pad = '0'; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
		char num[16];
		const char *s = num;
		switch (*fmt) {
		case 'd': {
			int v = va_arg(ap, int);
			if (v < 0) { *o++ = '-'; v = -v; if (width) width--; }
			utoa(v, num, 10);
			break;
		}
		case 'u': utoa(va_arg(ap, u32), num, 10); break;
		case 'x': utoa(va_arg(ap, u32), num, 16); break;
		case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
		case 'c': num[0] = va_arg(ap, int); num[1] = 0; break;
		default: num[0] = *fmt; num[1] = 0; break;
		}
		int len = strlen(s);
		while (len < width--) *o++ = pad;
		while (*s) *o++ = *s++;
	}
	*o = 0;
	return o - out;
}

int ksprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vformat(buf, fmt, ap);
	va_end(ap);
	return n;
}

void kdebug(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vformat(buf, fmt, ap);
	va_end(ap);
	for (char *p = buf; *p; p++) kos_debug_char(*p);
}
