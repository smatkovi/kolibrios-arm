/* KolibriOS for ARM: kernel/storage/mmc.c's sleep (CMD5) on the PC
 * against a model of an eMMC (make -C tests/storage mmc):
 *
 * The model follows JEDEC's states (idle, ready, ident, stand-by,
 * transfer, sleep) and answers the commands mmc.c sends; in its sleep it
 * takes CMD5 (awake) and CMD0 only, and the clock to it must be off
 * (the host's idle) while it sleeps and on for every command. The clock
 * is simulated. Checked: no sleep before a second without requests, then
 * CMD7 with RCA 0 and CMD5 with the sleep bit; a read wakes it (CMD5,
 * CMD7 with its RCA) and returns the right data; a card that does not
 * wake (as QEMU's model) is started anew from CMD0 and not put to sleep
 * again; kolibri.mmcsleep=off, an eMMC before 4.3 and QEMU's model:
 * never asleep. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../kernel/storage/storage.h"
#include "../../kernel/storage/mmc.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

u32 rtc_seconds(void) { return (u32)time(NULL); }
char *strncpy0(char *d, const char *s, size_t n)
{
	strncpy(d, s, n - 1);
	d[n - 1] = 0;
	return d;
}

static u64 clock_us;
static u64 now_us(void) { return clock_us += 10; }

static int sleep_allowed = 1;
bool storage_mmc_sleep(void) { return sleep_allowed; }

enum { IDLE, READY, IDENT, STBY, TRAN, SLEEP };
#define NBLK 4096
static struct {
	int state, rca, ext_rev, ext_sa;
	const char *name;
	int no_awake;                   /* takes nothing in its sleep but CMD0 (QEMU's) */
	int clock_off;
	int violations, sleeps, awakes, inits;
	char log[4096];
	u8 disk[NBLK * 512];
} m;

static void violation(const char *what, u32 idx)
{
	m.violations++;
	printf("model: VIOLATION: %s (CMD%u)\n", what, idx);
}

static void logcmd(u32 idx, u32 arg)
{
	char b[32];
	snprintf(b, sizeof(b), " %u:%x", idx, arg);
	if (strlen(m.log) + strlen(b) < sizeof(m.log)) strcat(m.log, b);
}

static int mcmd(void *ctx, u32 idx, u32 arg, u32 rsp, u32 *r, u8 *data, u32 n, bool write)
{
	logcmd(idx, arg);
	if (m.clock_off) violation("a command with the clock off", idx);
	if (m.state == SLEEP && idx != 0 && (idx != 5 || m.no_awake)) {
		if (idx != 5) violation("a command in the sleep", idx);
		return -1;
	}
	r[0] = (u32)(m.state == SLEEP ? STBY : m.state) << 9;
	switch (idx) {
	case 0:
		m.state = IDLE;
		m.inits++;
		return 0;
	case 1:
		if (m.state != IDLE && m.state != READY) return -1;
		m.state = READY;
		r[0] = 1u << 31 | 2u << 29 | 0xff8080;
		return 0;
	case 2:
		if (m.state != READY) return -1;
		m.state = IDENT;
		memset(r, 0, 16);
		/* the product name in bits 103:56 */
		r[0] = (u32)(u8)m.name[0];
		r[1] = (u32)(u8)m.name[1] << 24 | (u32)(u8)m.name[2] << 16 | (u32)(u8)m.name[3] << 8 | (u8)m.name[4];
		r[2] = (u32)(u8)m.name[5] << 24;
		return 0;
	case 3:
		if (m.state != IDENT) return -1;
		m.rca = arg >> 16;
		m.state = STBY;
		return 0;
	case 9:
		if (m.state != STBY) return -1;
		memset(r, 0, 16);
		return 0;
	case 7:
		if ((arg >> 16) == (u32)m.rca && m.state == STBY) m.state = TRAN;
		else if ((arg >> 16) != (u32)m.rca && m.state == TRAN) m.state = STBY;   /* deselected */
		else if (arg) violation("CMD7 in a wrong state", idx);
		return 0;
	case 5:
		if ((arg >> 16) != (u32)m.rca) violation("CMD5 to another RCA", idx);
		if (arg & 1u << 15) {
			if (m.state != STBY) { violation("sleep from a state but stand-by", idx); return -1; }
			m.state = SLEEP;
			m.sleeps++;
		} else {
			if (m.state != SLEEP) { violation("awake while awake", idx); return -1; }
			m.state = STBY;
			m.awakes++;
		}
		return 0;
	case 8:
		if (m.state != TRAN) return -1;          /* SD's CMD8 in idle: no answer */
		if (data) {
			memset(data, 0, 512);
			put_le32(data + 212, NBLK);
			data[192] = (u8)m.ext_rev;
			data[217] = (u8)m.ext_sa;
		}
		return 0;
	case 55: case 41:
		return -1;
	case 6: case 16: case 12:
		return m.state == TRAN ? 0 : -1;
	case 13:
		if ((arg >> 16) != (u32)m.rca) return -1;
		return 0;
	case 17: case 18: case 24: case 25:
		if (m.state != TRAN) { violation("data outside the transfer state", idx); return -1; }
		if (arg + n > NBLK) return -1;
		if (write) memcpy(m.disk + arg * 512, data, n * 512);
		else memcpy(data, m.disk + arg * 512, n * 512);
		return 0;
	default:
		violation("an unexpected command", idx);
		return -1;
	}
}

static void set_clock(void *ctx, u32 hz) {}
static void set_width(void *ctx, int bits) {}
static void idle(void *ctx, bool on) { m.clock_off = on; }

static void model(const char *name, int rev, int sa, int no_awake)
{
	memset(&m, 0, sizeof(m));
	m.name = name;
	m.ext_rev = rev, m.ext_sa = sa, m.no_awake = no_awake;
	for (u32 i = 0; i < sizeof(m.disk); i++) m.disk[i] = (u8)(i * 13 + i / 512);
}

static int read_ok(struct blkdev *d, u32 lba)
{
	static u8 b[512];
	if (d->read(d, lba, 1, b)) return 0;
	return !memcmp(b, m.disk + lba * 512, 512);
}

static void idle_for(u32 us)
{
	u64 end = clock_us + us;
	while (clock_us < end) {
		clock_us += 10000;
		mmc_tick();
	}
}

/* the driver keeps one card: each case runs in its own process */
static int one(const char *what)
{
	static struct mmc_host h = { NULL, 4, 128, mcmd, set_clock, set_width, now_us, NULL, idle };
	CHECK(mmc_start(&h, "mmc0", false) == 1, "%s: the card started", what);
	struct blkdev *d = blk_get(blk_count() - 1);
	CHECK(d && read_ok(d, 0), "%s: a read", what);
	m.log[0] = 0;
	return d != NULL;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	if (!strcmp(c, "sleep")) {
		model("SIMMC1", 5, 0x10, 0);
		struct blkdev *d = one(c) ? blk_get(blk_count() - 1) : NULL;
		idle_for(500000);
		CHECK(m.sleeps == 0 && !m.log[0], "half a second: awake, no command (%s)", m.log);
		CHECK(mmc_next_ms() <= 510 && mmc_next_ms() >= 490, "the sleep due in half a second (%u ms)", mmc_next_ms());
		idle_for(600000);
		CHECK(m.state == SLEEP && m.sleeps == 1 && m.clock_off, "a second: asleep, the clock off");
		CHECK(!strcmp(m.log, " 7:0 5:18000"), "CMD7 with RCA 0, CMD5 with the sleep bit (%s)", m.log);
		CHECK(mmc_next_ms() == 0xffffffffu, "asleep: no more ticks wanted");
		idle_for(5000000);
		CHECK(m.sleeps == 1, "asleep: nothing more");
		m.log[0] = 0;
		CHECK(d && read_ok(d, 1234), "a read wakes it, the data right");
		CHECK(!strncmp(m.log, " 5:10000 7:10000 13:10000", 25) && m.awakes == 1 && !m.clock_off,
		      "CMD5 without the sleep bit, CMD7 with its RCA, then CMD13 (%s)", m.log);
		idle_for(1100000);
		CHECK(m.sleeps == 2, "asleep again after another second");
		CHECK(d && read_ok(d, 77) && m.awakes == 2, "awake again");
	} else if (!strcmp(c, "no-awake")) {
		/* a card that takes only CMD0 in its sleep: started anew, never again */
		model("SIMMC2", 5, 0x10, 1);
		struct blkdev *d = one(c) ? blk_get(blk_count() - 1) : NULL;
		idle_for(1100000);
		CHECK(m.sleeps == 1, "asleep");
		int inits = m.inits;
		CHECK(d && read_ok(d, 99), "a read after a failed awake: the data right");
		CHECK(m.inits > inits && m.state == TRAN, "started anew from CMD0 (%s)", m.log);
		idle_for(3000000);
		CHECK(m.sleeps == 1 && mmc_next_ms() == 0xffffffffu, "not put to sleep again");
	} else if (!strcmp(c, "off") || !strcmp(c, "old") || !strcmp(c, "qemu")) {
		sleep_allowed = strcmp(c, "off");
		model(!strcmp(c, "qemu") ? "QEMU!!" : "SIMMC3", !strcmp(c, "old") ? 2 : 5, 0x10, 0);
		one(c);
		idle_for(3000000);
		CHECK(m.sleeps == 0 && m.state == TRAN && mmc_next_ms() == 0xffffffffu,
		      "%s: never asleep", !strcmp(c, "off") ? "kolibri.mmcsleep=off" : !strcmp(c, "old") ? "an eMMC 4.2" : "QEMU's model");
	} else {
		printf("mmc_sim sleep|no-awake|off|old|qemu\n");
		return 2;
	}
	CHECK(m.violations == 0, "no violation of the protocol (%d)", m.violations);
	printf("mmc_sim %s: %d passed, %d failed\n", c, passed, failed);
	return failed != 0;
}
