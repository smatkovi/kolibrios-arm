/* lib.c's memcpy and memset (the kernel's with SIMD=c and on ARMv7),
 * under other names, for the bench's C side: compiled as the kernel
 * compiles them (tests/asm/Makefile) */
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;

void *c_memcpy(void *d, const void *s, unsigned n)
{
	u8 *dp = d;
	const u8 *sp = s;
	if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
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

void *c_memset(void *d, int c, unsigned n)
{
	u8 *p = d;
	u32 w = (u8)c * 0x01010101u;
	while (n && ((uintptr_t)p & 3)) { *p++ = c; n--; }
	while (n >= 16) {
		((u32 *)p)[0] = w; ((u32 *)p)[1] = w;
		((u32 *)p)[2] = w; ((u32 *)p)[3] = w;
		p += 16; n -= 16;
	}
	while (n--) *p++ = c;
	return d;
}
