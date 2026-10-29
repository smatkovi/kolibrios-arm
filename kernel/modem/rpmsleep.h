/* KolibriOS for ARM: the SoC's sleep on an RPM-based Qualcomm SoC (the
 * SM6115): what the RPM is to keep while this processor sleeps (the
 * sleep set), the hand-over of the wake-ups to the MPM, and the RPM's
 * count of the SoC's sleeps (rpmsleep.c). */
#ifndef RPMSLEEP_H
#define RPMSLEEP_H
#include "rpm.h"

/* the sleep set: the votes Linux sends when nothing uses a resource
 * (the XO, its buffers, CX and MX, the bus clocks: 0). The number
 * acknowledged; with_rf: the RF buffers as well (not while the WLAN part
 * is on) */
int rpm_sleep_votes(bool with_rf);

/* The vMPM (the MPM's registers as the RPM's message RAM has them,
 * Linux's irq-qcom-mpm.c): TIMER0/1 (the next wake-up, a count of the
 * generic timer), then ENABLE, FALLING_EDGE, RISING_EDGE, POLARITY,
 * STATUS, each pins/32 words. Before the SoC may sleep: the wake-up, no
 * pin enabled but those in pin_mask (one word per bank), the status
 * cleared. Then the RPM is told (the MPM's IPC bit) */
#define MPM_REG_ENABLE 0
#define MPM_REG_STATUS 4
void mpm_prepare(volatile u8 *vmpm, u32 pins, u64 wake, const u32 *pin_mask);
/* after the wake-up: the pins whose STATUS bits are set (bank 0..2 in
 * out[], pins/32 words), the RPM's copy */
void mpm_status(const volatile u8 *vmpm, u32 pins, u32 *out);

/* the RPM's sleep statistics (qcom,rpm-stats; Linux's qcom_stats.c,
 * rpm_data: the records' offset in the word at 0x14, two records of
 * 32 bytes and 16 appended: the name ("vmin", "vlow"), the count, last
 * entered, last left, the time accumulated, the masters' votes) */
struct rpm_stat {
	char name[5];
	u32 count;
	u64 entered, left, accumulated;
	u32 votes;
};
/* base: the region, mapped (32-bit accesses). The records read (0-2) */
int rpm_stats_read(const volatile u8 *base, u32 size, struct rpm_stat out[2]);

#endif
