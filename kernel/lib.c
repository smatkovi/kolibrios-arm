/* KolibriOS for ARM: the kernel's small C library and debug output */
#include <stdarg.h>
#include "kernel.h"

/* AArch64: arch/arm64/string.S's (unless built with SIMD=c); these then
 * as memset_c, memcpy_c, memmove_c for kolibri.bench (bench.c) */
#if defined(__aarch64__) && !defined(SIMD_C)
#define memset memset_c
#define memcpy memcpy_c
#define memmove memmove_c
void *memset_c(void *d, int c, size_t n);
void *memcpy_c(void *d, const void *s, size_t n);
void *memmove_c(void *d, const void *s, size_t n);
#endif
void *memset(void *d, int c, size_t n)
{
	u8 *p = d;
	u32 w = (u8)c * 0x01010101u;
	while (n && ((addr_t)p & 3)) { *p++ = c; n--; }
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
	if ((((addr_t)dp | (addr_t)sp) & 3) == 0) {
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
#undef memset
#undef memcpy
#undef memmove

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

/* the debug board: what the kernel and programs (function 63.1) write,
 * for function 63.2 to read */
#define DBG_SIZE 4096
static char dbg[DBG_SIZE];
static u32 dbg_head, dbg_tail;

void dbg_put(char c)
{
	dbg[dbg_head % DBG_SIZE] = c;
	dbg_head++;
	if (dbg_head - dbg_tail > DBG_SIZE) dbg_tail = dbg_head - DBG_SIZE;
}

int dbg_get(void)
{
	if (dbg_tail == dbg_head) return -1;
	return (u8)dbg[dbg_tail++ % DBG_SIZE];
}

static void (*capture)(char c);

void kputc(char c)
{
	if (capture) { capture(c); return; }
	dbg_put(c);
	if (!plat || !plat->putc) return;
	if (c == '\n') plat->putc('\r');
	plat->putc(c);
}

void kputs(const char *s)
{
	while (*s) kputc(*s++);
}

static void put_num(unsigned long v, int base, int width, char pad, bool neg)
{
	char buf[24];
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
		case 'p': kputs("0x"); put_num(va_arg(ap, addr_t), 16, sizeof(addr_t) * 2, '0', false); break;
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

/* the panic's text, also for the screen */
static char pbuf[160];
static int plen;
static void pbuf_putc(char c) { if (plen + 1 < (int)sizeof(pbuf)) pbuf[plen++] = c; }

void panic(const char *fmt, ...)
{
	va_list ap;
	ARCH_IRQ_OFF();
	kputs("\nKERNEL PANIC: ");
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
	kputs("\n");
	/* the same on the screen, if there is one yet */
	extern void panic_screen(const char *msg);
	va_start(ap, fmt);
	capture = pbuf_putc;
	vkprintf(fmt, ap);
	capture = 0;
	va_end(ap);
	pbuf[plen] = 0;
	panic_screen(pbuf);
	if (plat && plat->panic_hook) plat->panic_hook();
	for (;;) asm volatile("wfi");
}
