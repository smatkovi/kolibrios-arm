/* KolibriOS for ARM: the modem (a Qualcomm MPSS: QMI over QRTR over GLINK
 * in the shared memory, docs/pro1x-modem.md). What the pieces in
 * kernel/modem share. Plain C without the kernel's services: the kernel
 * builds it, and tests/modem on the PC (MODEM_HOST). */
#ifndef MODEM_H
#define MODEM_H

#ifdef MODEM_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
#define mlog(...) printf(__VA_ARGS__)
#else
#include "kernel.h"
#define mlog(...) kprintf(__VA_ARGS__)
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define M_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* --- the shared memories: uncached, Device-GRE on the Pro1X (aligned
 * accesses only, no speculative reads) --------------------------------- */
static inline u32 sm_rd32(const volatile u8 *p) { return *(const volatile u32 *)p; }
static inline u16 sm_rd16(const volatile u8 *p) { return *(const volatile u16 *)p; }
static inline void sm_wr32(volatile u8 *p, u32 v) { *(volatile u32 *)p = v; }

/* the other processor sees what was written before what comes after */
static inline void sm_barrier(void)
{
#if defined(MODEM_HOST)
	__sync_synchronize();
#elif defined(__aarch64__)
	asm volatile("dsb sy" ::: "memory");
#else
	asm volatile("dsb" ::: "memory");
#endif
}

static inline void sm_copy_from(void *dst, const volatile u8 *src, u32 n)
{
	u8 *d = dst;
	while (n && ((unsigned long)src & 3)) { *d++ = *src++; n--; }
	if (!((unsigned long)d & 3))
		for (; n >= 4; n -= 4, d += 4, src += 4) *(u32 *)d = *(const volatile u32 *)src;
	while (n--) *d++ = *src++;
}

static inline void sm_copy_to(volatile u8 *dst, const void *src, u32 n)
{
	const u8 *s = src;
	while (n && ((unsigned long)dst & 3)) { *dst++ = *s++; n--; }
	if (!((unsigned long)s & 3))
		for (; n >= 4; n -= 4, dst += 4, s += 4) *(volatile u32 *)dst = *(const u32 *)s;
	while (n--) *dst++ = *s++;
}

/* --- little endian fields in normal memory (messages) ------------------ */
static inline u16 get16(const u8 *p) { return p[0] | p[1] << 8; }
static inline u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static inline u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }
static inline void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

static inline void mcopy(void *d, const void *s, u32 n)
{
	u8 *dp = d;
	const u8 *sp = s;
	while (n--) *dp++ = *sp++;
}

static inline void mzero(void *d, u32 n)
{
	u8 *dp = d;
	while (n--) *dp++ = 0;
}

static inline u32 mstrlen(const char *s)
{
	u32 n = 0;
	while (s[n]) n++;
	return n;
}

/* a string into a buffer of n, always terminated */
static inline void mstrcpy(char *d, const char *s, u32 n)
{
	u32 i = 0;
	for (; s && s[i] && i + 1 < n; i++) d[i] = s[i];
	if (n) d[i] = 0;
}

static inline int mstreq(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

#endif
