/* KolibriOS for ARM: the C library (lib/libc): formatted output.
 * vsnprintf knows the flags - + space 0 #, width and precision (also
 * as *), the sizes hh h l ll z j t, and d i u o x X c s p %; no floats
 * (%f, %e, %g print their argument as an integer part) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct out {
	char *buf;
	size_t n, len;
};

static void put(struct out *o, char c)
{
	if (o->len + 1 < o->n) o->buf[o->len] = c;
	o->len++;
}

static void pad(struct out *o, char c, int n)
{
	while (n-- > 0) put(o, c);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
	struct out o = { buf, n, 0 };
	for (; *fmt; fmt++) {
		if (*fmt != '%') { put(&o, *fmt); continue; }
		const char *start = fmt++;
		int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		int width = 0, prec = -1;
		if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) left = 1, width = -width; fmt++; }
		else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
			else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + *fmt++ - '0';
		}
		int size = 0;      /* -2 hh, -1 h, 1 l, 2 ll */
		for (;; fmt++) {
			if (*fmt == 'h') size--;
			else if (*fmt == 'l') size++;
			else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') size = *fmt == 'j' ? 2 : 0;
			else if (*fmt == 'L' || *fmt == 'q') size = 2;
			else break;
		}
		char c = *fmt;
		if (!c) { fmt--; break; }
		char num[72];
		const char *s = num;
		int len, neg = 0;
		const char *prefix = "";
		switch (c) {
		case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
			unsigned long long v;
			int base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
			if (c == 'p') {
				v = (uintptr_t)va_arg(ap, void *);
				alt = 1;
			} else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
				double d = va_arg(ap, double);
				if (d < 0) neg = 1, d = -d;
				v = (unsigned long long)d;
			} else if (c == 'd' || c == 'i') {
				long long sv = size >= 2 ? va_arg(ap, long long) : size == 1 ? va_arg(ap, long) : va_arg(ap, int);
				if (size == -1) sv = (short)sv;
				if (size <= -2) sv = (signed char)sv;
				if (sv < 0) neg = 1, v = -(unsigned long long)sv;
				else v = sv;
			} else {
				v = size >= 2 ? va_arg(ap, unsigned long long) : size == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
				if (size == -1) v = (unsigned short)v;
				if (size <= -2) v = (unsigned char)v;
			}
			const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			char *p = num + sizeof(num);
			*--p = 0;
			if (v || prec) do { *--p = digits[v % base]; v /= base; } while (v);
			len = num + sizeof(num) - 1 - p;
			int zeros = prec > len ? prec - len : 0;
			if (alt && base == 8 && !zeros && *p != '0') zeros = 1;
			while (zeros--) *--p = '0';
			s = p;
			len = strlen(s);
			if (neg) prefix = "-";
			else if (plus && (c == 'd' || c == 'i')) prefix = "+";
			else if (space && (c == 'd' || c == 'i')) prefix = " ";
			if (alt && base == 16 && (len > 1 || *s != '0')) prefix = c == 'X' ? "0X" : "0x";
			if (prec >= 0) zero = 0;
			break;
		}
		case 'c':
			num[0] = va_arg(ap, int);
			num[1] = 0;
			len = 1;
			zero = 0;
			break;
		case 's':
			s = va_arg(ap, const char *);
			if (!s) s = "(null)";
			len = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
			zero = 0;
			break;
		case '%':
			put(&o, '%');
			continue;
		default:
			for (const char *q = start; q <= fmt; q++) put(&o, *q);
			continue;
		}
		int pl = strlen(prefix), fill = width - len - pl;
		if (!left && !zero) pad(&o, ' ', fill);
		for (const char *q = prefix; *q; q++) put(&o, *q);
		if (!left && zero) pad(&o, '0', fill);
		for (int i = 0; i < len; i++) put(&o, s[i]);
		if (left) pad(&o, ' ', fill);
	}
	if (o.n) o.buf[o.len < o.n ? o.len : o.n - 1] = 0;
	return o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
	return vsnprintf(buf, 0x7fffffff, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, 0x7fffffff, fmt, ap);
	va_end(ap);
	return r;
}

/* the debug board (function 63.1) */
static void board(const char *s)
{
	for (; *s; s++) {
		register unsigned r0 asm("r0") = 63, r1 asm("r1") = 1, r2 asm("r2") = (unsigned char)*s;
		asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2) :: "r3", "r4", "r5", "memory");
	}
}

int vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	int r = vsnprintf(buf, sizeof(buf), fmt, ap);
	board(buf);
	return r;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}

int puts(const char *s)
{
	board(s);
	board("\n");
	return 0;
}

int putchar(int c)
{
	char s[2] = { c, 0 };
	board(s);
	return c;
}

/* sscanf: %d %u %x %s %c and literal characters, enough for settings */
int sscanf(const char *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = 0;
	for (; *fmt; fmt++) {
		if (*fmt == ' ') { while (*s == ' ' || *s == '\t' || *s == '\n') s++; continue; }
		if (*fmt != '%') { if (*s != *fmt) break; s++; continue; }
		fmt++;
		if (*fmt == 'l') fmt++;
		if (*fmt == 'c') { if (!*s) break; *va_arg(ap, char *) = *s++; n++; continue; }
		while (*s == ' ' || *s == '\t') s++;
		if (*fmt == 's') {
			char *d = va_arg(ap, char *);
			if (!*s) break;
			while (*s && *s != ' ' && *s != '\t' && *s != '\n') *d++ = *s++;
			*d = 0;
			n++;
			continue;
		}
		int base = *fmt == 'x' ? 16 : 10, neg = 0, any = 0;
		unsigned v = 0;
		if (*s == '-') neg = 1, s++;
		for (;; s++) {
			int d = *s >= '0' && *s <= '9' ? *s - '0' : base == 16 && (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
			if (d < 0) break;
			v = v * base + d;
			any = 1;
		}
		if (!any) break;
		*va_arg(ap, int *) = neg ? -(int)v : (int)v;
		n++;
	}
	va_end(ap);
	return n;
}
