/* KolibriOS for ARM: a modem in RAM (QEMU, tests/modem): the other end
 * of SMEM, GLINK, QRTR and the services phone.c uses */
#ifndef FAKE_H
#define FAKE_H
#include "phone.h"

#define FAKE_SMEM_SIZE (128 * 1024)
#define FAKE_RMTFS_SIZE (64 * 1024)
#define FAKE_RMTFS_PA 0x89b01000u

/* smem: FAKE_SMEM_SIZE bytes, rmtfs: FAKE_RMTFS_SIZE; fills hw for modem_start() */
void fake_init(struct modem_hw *hw, u8 *smem, u8 *rmtfs, u32 (*clock)(void), void (*set_clock)(u32));
void fake_poll(u32 ms);
u32 fake_next_ms(u32 ms);
/* what it saw, for the tests */
struct fake_stats { u32 rmtfs_ok, rmtfs_bad, sent, dialled, answered, ended, wlfw, wlan_cfg, wlan_mode; char last_text[200]; };
extern struct fake_stats fake_stats;
/* the modem opened IPCRTR before and Linux read that (the kexec case):
 * set before fake_init() */
extern bool fake_preopened;
/* the channel open from Linux's time (pro1x-kexec.sh -m, no unbind):
 * taken over (modem_hw.adopt_ipcrtr); set before fake_init() */
extern bool fake_adopted;
/* a call from number, now */
void fake_ring(const char *number);

#endif
