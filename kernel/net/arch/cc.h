/* KolibriOS for ARM: lwIP's port to the kernel: the kernel's own types
 * and library (no libc), its debug output */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#define LWIP_NO_STDDEF_H 1      /* size_t from kernel.h */
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H 1
#include "kernel.h"
typedef long ptrdiff_t;

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic("lwIP: %s (%s:%d)", x, __FILE__, __LINE__)
#define lwip_htons(x) ((u16_t)__builtin_bswap16(x))
#define lwip_htonl(x) ((u32_t)__builtin_bswap32(x))

u32 net_rand(void);
#define LWIP_RAND() net_rand()

#endif
