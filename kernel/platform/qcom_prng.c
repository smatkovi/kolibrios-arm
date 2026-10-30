/* KolibriOS for ARM: the Qualcomm PRNG (qcom_prng.h), as Linux's
 * drivers/crypto/qcom-rng.c reads a "qcom,prng-ee" (skip_init: TrustZone
 * set it up and owns its configuration; Linux registers it as the crypto
 * API's "stdrng", not as a hwrng):
 *
 *  - the clock voted on around each read (qcom_rng_generate's
 *    clk_prepare_enable / clk_disable_unprepare), with clk-branch.c's
 *    checks: voted on, then CLK_OFF clear (or the NOC FSM on) within
 *    200 us, unless the branch is under hardware gating (HW_CTL), when
 *    Linux checks nothing; voted off, 10 us
 *  - a word whenever STATUS says one is there, polled every 200 us for at
 *    most 10 ms (readl_poll_timeout); a word of zero is an error and the
 *    whole read fails (-EINVAL) */
#include "qcom_prng.h"

bool qprng_clock_on(struct qprng *q)
{
	q->wr(q->gcc + GCC_APCS_VOTE, q->rd(q->gcc + GCC_APCS_VOTE) | 1u << GCC_PRNG_VOTE_BIT);
	for (int i = 0; i <= 200; i++) {
		u32 c = q->rd(q->gcc + GCC_PRNG_AHB_CBCR);
		if ((c & CBCR_HW_CTL) || !(c & CBCR_CLK_OFF) || CBCR_NOC_FSM_STATUS(c) == FSM_STATUS_ON)
			return true;
		q->udelay(1);
	}
	q->clock_stuck++;
	qprng_clock_off(q);
	return false;
}

void qprng_clock_off(struct qprng *q)
{
	q->wr(q->gcc + GCC_APCS_VOTE, q->rd(q->gcc + GCC_APCS_VOTE) & ~(1u << GCC_PRNG_VOTE_BIT));
	q->udelay(10);
}

u32 qprng_read(struct qprng *q, void *out, u32 len)
{
	if (!qprng_clock_on(q)) return 0;
	u32 done = 0;
	while (done < len) {
		int t;
		for (t = 0; t <= 50 && !(q->rd(q->base + PRNG_STATUS) & PRNG_STATUS_DATA_AVAIL); t++)
			q->udelay(200);
		if (t > 50) {
			q->timeouts++;
			done = 0;
			break;
		}
		u32 v = q->rd(q->base + PRNG_DATA_OUT), n = len - done < 4 ? len - done : 4;
		if (!v) {
			q->zeros++;
			done = 0;
			break;
		}
		q->words++;
		memcpy((char *)out + done, &v, n);
		done += n;
	}
	qprng_clock_off(q);
	if (!done) memset(out, 0, len);
	return done;
}
