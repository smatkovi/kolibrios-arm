/* KolibriOS for ARM: the kernel's small C library and debug output */
#include <stdarg.h>
#include "kernel.h"

void *memset(void *d, int c, size_t n)
{
	u8 *p = d;
	u32 w = (u8)c * 0x01010101u;
	while (n && ((u32)p & 3)) { *p++ = c; n--; }
	while (n >= 16) {
		((u32 *)p)[0] = w; ((u32 *)p)[1] = w;
		((u32 *)p)[2] = w; ((u32 *)p)[3] = w;
		p += 16; n -= 16;
	}
	while (n--) *p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	u8 *dp = d;
	const u8 *sp = s;
	if ((((u32)dp | (u32)sp) & 3) == 0) {
		while (n >= 16) {
			((u32 *)dp)[0] = ((const u32 *)sp)[0];
			((u32 *)dp)[1] = ((const u32 *)sp)[1];
			((u32 *)dp)[2] = ((const u32 *)sp)[2];
			((u32 *)dp)[3] = ((const u32 *)sp)[3];
			dp += 16; sp += 16; n -= 16;
		}
	}
	while (n--) *dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	u8 *dp = d;
	const u8 *sp = s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	while (n--) dp[n] = sp[n];
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q) return *p - *q;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) return (u8)*a - (u8)*b;
		if (!*a) return 0;
	}
	return 0;
}

char *strncpy0(char *d, const char *s, size_t n)
{
	size_t i = 0;
	if (!n) return d;
	for (; i + 1 < n && s[i]; i++) d[i] = s[i];
	d[i] = 0;
	return d;
}

const struct platform *plat;

void kputc(char c)
{
	if (!plat || !plat->putc) return;
	if (c == '\n') plat->putc('\r');
	plat->putc(c);
}

void kputs(const char *s)
{
	while (*s) kputc(*s++);
}

static void put_num(u32 v, int base, int width, char pad, bool neg)
{
	char buf[12];
	int i = 0;
	do {
		int d = v % base;
		buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	} while (v);
	if (neg) buf[i++] = '-';
	while (i < width) { kputc(pad); width--; }
	while (i) kputc(buf[--i]);
}

static void vkprintf(const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		if (*fmt != '%') { kputc(*fmt); continue; }
		fmt++;
		char pad = ' ';
		int width = 0;
		if (*fmt == '0') { pad = '0'; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
		switch (*fmt) {
		case 'd': {
			int v = va_arg(ap, int);
			put_num(v < 0 ? -(u32)v : (u32)v, 10, width, pad, v < 0);
			break;
		}
		case 'u': put_num(va_arg(ap, u32), 10, width, pad, false); break;
		case 'x': put_num(va_arg(ap, u32), 16, width, pad, false); break;
		case 'p': kputs("0x"); put_num(va_arg(ap, u32), 16, 8, '0', false); break;
		case 's': { const char *s = va_arg(ap, const char *); kputs(s ? s : "(null)"); break; }
		case 'c': kputc(va_arg(ap, int)); break;
		case '%': kputc('%'); break;
		default: kputc('%'); kputc(*fmt); break;
		}
	}
}

int kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
	return 0;
}

void panic(const char *fmt, ...)
{
	va_list ap;
	asm volatile("cpsid if");
	kputs("\nKERNEL PANIC: ");
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
	kputs("\n");
	for (;;) asm volatile("wfi");
}
