/* KolibriOS for ARM: the C library (lib/libc): numbers, sorting, the end */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

int errno;

static unsigned long long to_ull(const char *s, char **end, int base, int *neg)
{
	const char *p = s;
	while (isspace((unsigned char)*p)) p++;
	*neg = 0;
	if (*p == '-' || *p == '+') *neg = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && isxdigit((unsigned char)p[2]))
		p += 2, base = 16;
	else if (base == 0)
		base = *p == '0' ? 8 : 10;
	unsigned long long v = 0;
	const char *digits = p;
	for (;; p++) {
		int c = (unsigned char)*p, d;
		if (isdigit(c)) d = c - '0';
		else if (isalpha(c)) d = tolower(c) - 'a' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
	}
	if (end) *end = (char *)(p == digits ? s : p);
	return v;
}

long strtol(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = to_ull(s, end, base, &neg);
	return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = to_ull(s, end, base, &neg);
	return neg ? -(unsigned long)v : (unsigned long)v;
}

long long strtoll(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = to_ull(s, end, base, &neg);
	return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = to_ull(s, end, base, &neg);
	return neg ? -v : v;
}

int atoi(const char *s) { return strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

char *itoa(int v, char *buf)
{
	sprintf(buf, "%d", v);
	return buf;
}

static void swap(char *a, char *b, size_t n)
{
	while (n--) {
		char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

/* a shell sort: no recursion, no memory */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	char *b = base;
	for (size_t gap = n / 2; gap; gap /= 2)
		for (size_t i = gap; i < n; i++)
			for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
				swap(b + (j - gap) * size, b + j * size, size);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	const char *b = base;
	while (n) {
		const char *m = b + (n / 2) * size;
		int c = cmp(key, m);
		if (!c) return (void *)m;
		if (c > 0) b = m + size, n -= n / 2 + 1;
		else n /= 2;
	}
	return NULL;
}

static unsigned long seed = 1;
int rand(void)
{
	seed = seed * 1103515245 + 12345;
	return (seed >> 1) & RAND_MAX;
}
void srand(unsigned s) { seed = s; }

char *getenv(const char *name)
{
	(void)name;
	return NULL;
}

void exit(int status)
{
	(void)status;
	register int r0 asm("r0") = -1;
	asm volatile("svc #0x40" : "+r"(r0) :: "r1", "r2", "r3", "r4", "r5", "memory");
	for (;;)
		;
}

void abort(void) { exit(1); }

void __assert_fail(const char *expr, const char *file, int line)
{
	printf("%s:%d: assertion failed: %s\n", file, line, expr);
	exit(1);
}
