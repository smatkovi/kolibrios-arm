/* KolibriOS for ARM: the PMICs of a Qualcomm SoC behind its SPMI PMIC
 * arbiter (version 5, the SM6115), and the battery's voltage and current
 * from the Pro1X's PMI632 (pmic.c). */
#ifndef PMIC_H
#define PMIC_H
#include "modem.h"

/* the arbiter's register regions (the device tree's reg-names core,
 * chnls, obsrvr, cnfg), mapped; this processor's execution environment
 * (qcom,ee); the accessors (32 bits) */
struct spmi_hw {
	addr_t core, chnls, obsrvr, cnfg;
	u32 core_size;
	u32 ee;
	u32 (*rd)(addr_t a);
	void (*wr)(addr_t a, u32 v);
	void (*delay_us)(u32 us);
};
/* the channels' map read; the version, or 0 (not version 5) */
u32 spmi_init(const struct spmi_hw *hw);
/* registers of a PMIC (slave id sid), up to 8 bytes at a time: 0, or -1
 * (no channel), -2 (not ours to write), -3 (refused), -4 (no answer) */
int spmi_read(u32 sid, u32 addr, u8 *buf, u32 len);
int spmi_write(u32 sid, u32 addr, const u8 *buf, u32 len);
u32 spmi_channels(u32 *ours);           /* the channels, those this EE may write */

/* the battery: the fuel gauge's (QG) current, the ADC5's voltage
 * (Linux's pro1x-adc-battery.c and qcom-spmi-adc5.c, FreeBSD's
 * pro1x_battery): the QG at sid/qg, the ADC at sid/adc with the battery's
 * channel and its pre-scaling (num/den) */
struct battery {
	u32 sid, qg, adc, chan, pre_num, pre_den;
	u32 na_per_lsb;                 /* by the QG's subtype */
	bool ok;
};
int battery_init(struct battery *b);
/* mV; mA positive while discharging. 0, or < 0 as spmi_read */
int battery_voltage(struct battery *b, u32 *mv);
int battery_current(struct battery *b, s32 *ma);

#endif
