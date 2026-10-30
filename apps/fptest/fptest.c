/* fptest: the FP/SIMD registers of programs kept across task switches
 * and the kernel's own NEON (arch/arm64/fpu.c): two copies of this (the
 * second started with "peer") fill d0-d31 and FPSCR with values of
 * their own, give the processor to each other or have the kernel draw
 * (its NEON fill), and check that the values are still theirs. The
 * result on the debug board (the console): "fptest: ok, N rounds" or
 * "fptest: FAIL ...". A test: kolibri.run=/sys/fptest (docs/asm.md). */
#include "kos.h"

#define ROUNDS 3000

static u32 want[64], got[64];

static void load(const u32 *v, u32 fpscr)
{
	asm volatile(".fpu neon\n\tvldmia %0, {d0-d15}\n\tvldmia %1, {d16-d31}\n\tvmsr fpscr, %2"
		     :: "r"(v), "r"(v + 32), "r"(fpscr) : "memory");
}

static u32 store(u32 *v)
{
	u32 fpscr;
	asm volatile(".fpu neon\n\tvstmia %1, {d0-d15}\n\tvstmia %2, {d16-d31}\n\tvmrs %0, fpscr"
		     : "=r"(fpscr) : "r"(v), "r"(v + 32) : "memory");
	return fpscr;
}

int main(const char *params)
{
	int peer = params && !strcmp(params, "peer");
	u32 seed = peer ? 0x5eed0000u : 0xa5a50000u;
	/* the rounding mode and flush-to-zero as this copy's own */
	u32 fpscr = peer ? 0x01400000u : 0x00800000u;
	if (!peer) kos_run("/sys/fptest", "peer");
	for (int r = 0; r < ROUNDS; r++) {
		for (int i = 0; i < 64; i++) want[i] = seed + r * 64 + i;
		load(want, fpscr);
		if (r % 3 == 0) kos_call(68, 1, 0, 0, 0, 0);             /* yield */
		else if (r % 3 == 1) kos_rect(0, 0, 64, 64, 0x102030 + r); /* the kernel's NEON */
		else kos_call(26, 9, 0, 0, 0, 0);
		u32 f = store(got);
		for (int i = 0; i < 64; i++)
			if (got[i] != want[i]) {
				kdebug("fptest%s: FAIL in round %d: d%d %x, not %x\n", peer ? " (peer)" : "", r,
				       i / 2, got[i], want[i]);
				return 1;
			}
		if ((f & 0x01c00000u) != fpscr) {
			kdebug("fptest%s: FAIL in round %d: FPSCR %x\n", peer ? " (peer)" : "", r, f);
			return 1;
		}
	}
	kdebug("fptest%s: ok, %d rounds\n", peer ? " (peer)" : "", ROUNDS);
	return 0;
}
