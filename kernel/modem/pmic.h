/* KolibriOS for ARM: the PMICs of a Qualcomm SoC behind its SPMI PMIC
 * arbiter (version 5, the SM6115), and the battery's voltage and current
 * from the Pro1X's PMI632 (pmic.c). */
#ifndef PMIC_H
#define PMIC_H
#include "modem.h"

/* the arbiter's register regions (the device tree's reg-names core,
 * chnls, obsrvr, cnfg, intr; intr 0: no interrupts), mapped; this
 * processor's execution environment (qcom,ee); the accessors (32 bits) */
struct spmi_hw {
	addr_t core, chnls, obsrvr, cnfg, intr;
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

/* The peripherals' interrupts (Linux's spmi-pmic-arb.c, version 5): a
 * peripheral's up to 8 interrupts, configured in its own registers
 * (QPNPINT: RT_STS 0x10, SET_TYPE 0x11, POLARITY_HIGH/LOW 0x12/0x13,
 * LATCHED_CLR 0x14, EN_SET/EN_CLR 0x15/0x16, LATCHED_STS 0x18), gathered
 * by the arbiter per channel (ACC_ENABLE, IRQ_STATUS, IRQ_CLEAR at 0x100,
 * 0x104, 0x108 of the channel's chnls page) into this EE's ACC_STATUS
 * bitmap (intr + 0x10000 * ee: a bit per channel), whose OR is the
 * arbiter's one interrupt (periph_irq). */
#define SPMI_IRQ_EDGE_RISING 1
#define SPMI_IRQ_EDGE_FALLING 2
#define SPMI_IRQ_EDGE_BOTH 3
#define SPMI_IRQ_LEVEL_HIGH 4
#define SPMI_IRQ_LEVEL_LOW 8
/* every channel whose interrupts are this EE's taken out of the
 * arbiter's line (ACC_ENABLE 0, IRQ_STATUS cleared): what Linux left on.
 * The number that were on */
u32 spmi_irq_quiet(void);
/* a peripheral's interrupt on (sid, the peripheral's address, its
 * number 0-7, SPMI_IRQ_*): 0, -1 (no channel), -2 (not ours to write),
 * -5 (its interrupts not this EE's), or spmi_write's error */
int spmi_irq_enable(u32 sid, u32 addr, u32 irq, u32 type);
/* the interrupts the arbiter has for this EE, taken: each acknowledged
 * at the arbiter (IRQ_CLEAR) and in its peripheral (LATCHED_CLR); those
 * of channels not enabled (ACC_ENABLE 0) cleared and not reported. The
 * number reported */
struct spmi_irq {
	u32 sid, addr;                  /* the peripheral (its base address) */
	u8 bits;                        /* its interrupts that fired */
};
int spmi_irq_take(struct spmi_irq *out, int max);

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
