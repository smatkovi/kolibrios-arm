/* lwIP on the PC for tests/wifi: the kernel's options, with lwIP's own
 * timers (timeouts.c; the kernel's, kernel/net/timers.c, are tests/net's) */
#define LWIP_TIMERS_CUSTOM 0
#include "../../../kernel/net/lwipopts.h"
