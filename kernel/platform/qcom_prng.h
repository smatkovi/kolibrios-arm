/* KolibriOS for ARM: the Qualcomm PRNG (qcom_prng.c, docs/rng.md). Plain
 * C on its registers through the functions given, so that tests/rng
 * drives it against a model of the registers on the PC (QPRNG_HOST). */
#ifndef QCOM_PRNG_H
#define QCOM_PRNG_H

#ifdef QPRNG_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long addr_t;
#else
#include "kernel.h"
#endif

/* the PRNG's window (qcom,prng-ee: the execution environment's; the
 * configuration at 0x100 and 0x104 belongs to TrustZone) */
#define PRNG_DATA_OUT 0x000
#define PRNG_STATUS 0x004
#define PRNG_STATUS_DATA_AVAIL 1u

/* its clock in the GCC (gcc-sm6115.c's gcc_prng_ahb_clk): a branch voted
 * on in the APCS's vote register, with hardware clock gating */
#define GCC_APCS_VOTE 0x79004
#define GCC_PRNG_VOTE_BIT 13
#define GCC_PRNG_AHB_CBCR 0x21004
#define CBCR_CLK_OFF (1u << 31)
#define CBCR_NOC_FSM_STATUS(v) (((v) >> 28) & 7)
#define FSM_STATUS_ON 2
#define CBCR_HW_CTL (1u << 1)           /* hardware gating: no halt check */

struct qprng {
	addr_t base, gcc;               /* the PRNG's and the GCC's addresses */
	u32 (*rd)(addr_t a);
	void (*wr)(addr_t a, u32 v);
	void (*udelay)(u32 us);
	/* what happened */
	u32 words, zeros, timeouts, clock_stuck;
};

bool qprng_clock_on(struct qprng *q);   /* false: the clock did not come on (vote taken back) */
void qprng_clock_off(struct qprng *q);
u32 qprng_read(struct qprng *q, void *out, u32 len);   /* bytes; 0 after a zero word or a timeout */

#endif
