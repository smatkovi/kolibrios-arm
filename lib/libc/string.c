/* KolibriOS for ARM: the C library (lib/libc): strings and memory */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (!(((size_t)p | (size_t)q | n) & 3)) {
		unsigned *pw = d;
		const unsigned *qw = s;
		for (n /= 4; n; n--) *pw++ = *qw++;
		return d;
	}
	while (n--) *p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (p == q || !n) return d;
	if (p < q || p >= q + n) return memcpy(d, s, n);
	while (n--) p[n] = q[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	if (!(((size_t)p | n) & 3)) {
		unsigned v = (unsigned char)c * 0x01010101u, *pw = d;
		for (n /= 4; n; n--) *pw++ = v;
		return d;
	}
	while (n--) *p++ = c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q) return *p - *q;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c) return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return p - s;
}

size_t strnlen(const char *s, size_t n)
{
	size_t i = 0;
	while (i < n && s[i]) i++;
	return i;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) d[i] = s[i];
	for (; i < n; i++) d[i] = 0;
	return d;
}

char *strcat(char *d, const char *s)
{
	strcpy(d + strlen(d), s);
	return d;
}

char *strncat(char *d, const char *s, size_t n)
{
	char *p = d + strlen(d);
	while (n-- && *s) *p++ = *s++;
	*p = 0;
	return d;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
		if (!*a) break;
	}
	return 0;
}

int strcasecmp(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
		if (x != y) return x - y;
		if (!x) break;
	}
	return 0;
}

int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) return (char *)s;
		if (!*s) return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = NULL;
	for (;; s++) {
		if (*s == (char)c) r = s;
		if (!*s) return (char *)r;
	}
}

char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	if (!l) return (char *)h;
	for (; *h; h++)
		if (*h == *n && !strncmp(h, n, l)) return (char *)h;
	return NULL;
}

size_t strspn(const char *s, const char *accept)
{
	size_t i = 0;
	while (s[i] && strchr(accept, s[i])) i++;
	return i;
}

size_t strcspn(const char *s, const char *reject)
{
	size_t i = 0;
	while (s[i] && !strchr(reject, s[i])) i++;
	return i;
}

char *strpbrk(const char *s, const char *accept)
{
	s += strcspn(s, accept);
	return *s ? (char *)s : NULL;
}

char *strtok(char *s, const char *delim)
{
	static char *next;
	if (!s) s = next;
	if (!s) return NULL;
	s += strspn(s, delim);
	if (!*s) { next = NULL; return NULL; }
	char *e = s + strcspn(s, delim);
	if (*e) *e++ = 0;
	else e = NULL;
	next = e;
	return s;
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d) memcpy(d, s, n);
	return d;
}

char *strndup(const char *s, size_t n)
{
	n = strnlen(s, n);
	char *d = malloc(n + 1);
	if (d) { memcpy(d, s, n); d[n] = 0; }
	return d;
}

char *strerror(int e)
{
	(void)e;
	return "error";
}

char *strupr(char *s)
{
	for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
	return s;
}

char *strlwr(char *s)
{
	for (char *p = s; *p; p++) *p = tolower((unsigned char)*p);
	return s;
}

char *strrev(char *s)
{
	size_t n = strlen(s);
	for (size_t i = 0; i < n / 2; i++) {
		char c = s[i];
		s[i] = s[n - 1 - i];
		s[n - 1 - i] = c;
	}
	return s;
}
