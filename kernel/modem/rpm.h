/* KolibriOS for ARM: requests to the RPM (the SoC's resource power
 * manager: regulators, clocks) over its GLINK edge in the RPM's message
 * RAM, as Linux's qcom_glink_rpm.c and smd-rpm.c do. After a kexec the
 * channel "rpm_requests" is still open with Linux's id; requests go on it
 * (the RPM's GLINK needs no intents). */
#ifndef RPM_H
#define RPM_H
#include "modem.h"

#define RPM_LDOA 0x616f646cu                  /* "ldoa" */
#define RPM_SMPA 0x61706d73u                  /* "smpa" */
#define RPM_KEY_SWEN 0x6e657773u              /* "swen": on */
#define RPM_KEY_UV 0x00007675u                /* "uv": microvolts */
#define RPM_KEY_MA 0x0000616du                /* "ma": the load */

/* ram: the message RAM, mapped (32-bit accesses only); kick: the RPM's
 * interrupt; delay_us: a wait. 0, or -1 without its table of contents */
int rpm_init(volatile u8 *ram, u32 size, void (*kick)(void), void (*delay_us)(u32 us));
/* a request for the active state: key/value pairs of 32-bit values;
 * 0 acknowledged, -1 no answer, -2 refused */
int rpm_write(u32 type, u32 id, const u32 *keys, const u32 *values, int n);
/* ... for a set: what the RPM keeps while this processor is awake, or
 * asleep (QCOM_SMD_RPM_ACTIVE_STATE, _SLEEP_STATE of Linux's smd-rpm.h) */
#define RPM_ACTIVE 0
#define RPM_SLEEP 1
int rpm_write_set(u32 set, u32 type, u32 id, const u32 *keys, const u32 *values, int n);
bool rpm_ready(void);
int rpm_ldo(u32 ldo, u32 microvolts, bool on);
u32 rpm_lcid(void);

#endif
