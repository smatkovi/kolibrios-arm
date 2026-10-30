/* lwIP's port for tests/wifi: the PC's libc */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwIP: %s (%s:%d)\n", x, __FILE__, __LINE__); abort(); } while (0)
#define LWIP_RAND() ((u32_t)rand())
#endif
