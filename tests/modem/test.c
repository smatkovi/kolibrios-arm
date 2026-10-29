/* KolibriOS for ARM: the modem's code on the PC (make -C tests/modem):
 * the SMS codec against known PDUs and itself, then the whole stack
 * (SMEM, GLINK, QRTR, QMI, rmtfs, phone.c) against the fake modem, tick
 * by tick as the kernel runs it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/modem/fake.h"
#include "../../kernel/modem/wlfw.h"
#include "../../kernel/modem/rpm.h"
#include "../../kernel/modem/rpmsleep.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u32 hexpdu(const char *h, u8 *out)
{
	u32 n = 0;
	for (; h[0] && h[1]; h += 2) {
		unsigned v;
		sscanf(h, "%2x", &v);
		out[n++] = v;
	}
	return n;
}

static void test_sms(void)
{
	struct sms_part p;
	u8 pdu[200];
	/* the classic example (SMSC +31624000000) */
	u32 n = hexpdu("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0, "decode the example");
	CHECK(!strcmp(p.number, "+31641600986"), "number %s", p.number);
	CHECK(!strcmp(p.text, "How are you?"), "text '%s'", p.text);
	CHECK(p.time == 1030390661u, "time %u", p.time);
	CHECK(!p.submit && !p.total, "single DELIVER");

	/* UCS-2 by hand: "Grüße" from +4366 */
	n = hexpdu("0004049166660008" "62019221000040" "0A004700720" "0FC00DF0065", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0, "decode UCS-2");
	CHECK(!strcmp(p.text, "Gr\xc3\xbc\xc3\x9f" "e"), "UCS-2 text '%s'", p.text);
	CHECK(p.time == 1793271600u, "UCS-2 time %u (2026-10-29 12:00 +01)", p.time);

	/* alphanumeric sender "Info" */
	n = hexpdu("000407D049B7F90D0000" "62019221000040" "03" "C87408", pdu);
	CHECK(sms_decode(pdu, n, &p) == 0 && !strcmp(p.number, "Info") && !strcmp(p.text, "Hi!"),
	      "alphanumeric '%s': '%s'", p.number, p.text);

	/* round trips through SUBMIT */
	static u8 out[6][SMS_PDU_MAX];
	u32 len[6];
	const char *texts[] = {
		"Hallo Welt",
		"Preis: 5\xe2\x82\xac [ok] {x} ^~|\\",
		"Gr\xc3\xbc\xc3\x9f" "e aus Wien \xf0\x9f\x99\x82",
		"@\xc2\xa3$ \xc3\xa4\xc3\xb6\xc3\xbc \xc3\x84\xc3\x96\xc3\x9c \xc3\x9f \xce\x94\xce\xa9",
	};
	for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		int parts = sms_encode("+436641234567", texts[i], 9, out, len, 6);
		CHECK(parts == 1, "'%s' in one part (%d)", texts[i], parts);
		CHECK(sms_decode(out[0], len[0], &p) == 0 && p.submit, "decode SUBMIT %u", i);
		CHECK(!strcmp(p.text, texts[i]), "round trip '%s' -> '%s'", texts[i], p.text);
		CHECK(!strcmp(p.number, "+436641234567"), "SUBMIT number %s", p.number);
	}
	CHECK(out[0][0] == 0 && (out[0][1] & 3) == 1, "default SMSC, SUBMIT");

	/* concatenation: 400 GSM characters, 3 parts */
	char longtext[420], joined[1200] = "";
	for (int i = 0; i < 400; i++) longtext[i] = 'a' + i % 26;
	longtext[400] = 0;
	longtext[152] = '{';                     /* an escape across the part border */
	int parts = sms_encode("0664123", longtext, 77, out, len, 6);
	CHECK(parts == 3, "400 characters in %d parts", parts);
	for (int i = 0; i < parts; i++) {
		CHECK(sms_decode(out[i], len[i], &p) == 0, "decode part %d", i);
		CHECK(p.ref == 77 && p.total == 3 && p.seq == i + 1, "part %d: ref %u total %u seq %u", i, p.ref, p.total, p.seq);
		strcat(joined, p.text);
	}
	CHECK(!strcmp(joined, longtext), "joined parts (%zu vs %zu)", strlen(joined), strlen(longtext));
	CHECK(!strcmp(p.number, "0664123"), "national number %s", p.number);
	/* 80 non-GSM characters: UCS-2 in 2 parts */
	char u[400] = "";
	for (int i = 0; i < 80; i++) strcat(u, "\xd0\x96");   /* Ж */
	CHECK(sms_encode("+43", u, 1, out, len, 6) == 2, "UCS-2 in 2 parts");
}

/* --- the stack against the fake modem -------------------------------- */

static u8 smem[FAKE_SMEM_SIZE] __attribute__((aligned(8)));
static u8 rmem[FAKE_RMTFS_SIZE] __attribute__((aligned(8)));
static u32 clock_set, ms;

static u32 test_clock(void) { return clock_set ? clock_set + ms / 1000 : 0; }
static void test_set_clock(u32 t) { clock_set = t - ms / 1000; }

static void run(u32 for_ms)
{
	for (u32 end = ms + for_ms; ms < end; ms += 10) {
		fake_poll(ms);
		modem_poll(ms);
	}
}

static int find_sms(const char *text_start, struct kos_sms *s)
{
	struct kos_modem m;
	modem_status(&m);
	for (u32 i = 0; i < m.sms_count; i++)
		if (modem_sms(i, s) == 0 && !strncmp(s->text, text_start, strlen(text_start))) return i;
	return -1;
}


/* the RPM's edge: a message RAM of our own, the RPM answering on the kick */
static u32 rpm_ram[0x7000 / 4];
static u32 rpm_got_type, rpm_got_id, rpm_got_uv, rpm_got_swen, rpm_got_lcid, rpm_requests;
static u32 rpm_got_set, rpm_got_key, rpm_got_value, rpm_sleep_reqs, rpm_refuse_type;
#define RAM(o) rpm_ram[(o) / 4]
#define TXF 0x100
#define RXF 0x600
#define FL 1024

static void fake_rpm(void)
{
	u32 t = RAM(TXF), h = RAM(TXF + 4);
	while (t != h) {
		u32 w = RAM(TXF + 8 + t), chunk = RAM(TXF + 8 + (t + 8) % FL);
		u32 m[64];
		for (u32 i = 0; i < chunk / 4 && i < 64; i++) m[i] = RAM(TXF + 8 + (t + 16 + 4 * i) % FL);
		rpm_got_lcid = w >> 16;
		rpm_got_type = m[4];
		rpm_got_id = m[5];
		rpm_got_uv = rpm_got_swen = ~0u;
		rpm_got_set = m[3];
		rpm_got_key = m[7];
		rpm_got_value = m[9];
		if (m[3] == RPM_SLEEP) rpm_sleep_reqs++;
		for (u32 i = 0; i < m[6] / 12; i++) {
			if (m[7 + 3 * i] == RPM_KEY_UV) rpm_got_uv = m[9 + 3 * i];
			if (m[7 + 3 * i] == RPM_KEY_SWEN) rpm_got_swen = m[9 + 3 * i];
		}
		rpm_requests++;
		t = (t + M_ALIGN(16 + chunk, 8)) % FL;
		/* the answer: msg# with the id, err for a resource that is not there */
		u32 a[16], n = 0;
		a[n++] = 0x00716572;
		a[n++] = 0;
		a[n++] = 0x2367736d;
		a[n++] = 4;
		a[n++] = m[2];
		if (m[5] == 99 || m[4] == rpm_refuse_type) {
			a[n++] = 0x00727265;
			a[n++] = 24;
			memcpy(&a[n], "resource does not exist", 24);
			n += 6;
		}
		a[1] = 4 * (n - 2);
		u32 rh = RAM(RXF + 4), hdr[4] = { 9 | 5 << 16, 0, 4 * n, 0 };
		for (u32 i = 0; i < 4; i++) RAM(RXF + 8 + (rh + 4 * i) % FL) = hdr[i];
		for (u32 i = 0; i < n; i++) RAM(RXF + 8 + (rh + 16 + 4 * i) % FL) = a[i];
		RAM(RXF + 4) = (rh + M_ALIGN(16 + 4 * n, 8)) % FL;
	}
	RAM(TXF) = t;
}

static void test_rpm(void)
{
	memset(rpm_ram, 0, sizeof(rpm_ram));
	u32 toc = sizeof(rpm_ram) - 256;
	RAM(toc) = 0x67727430;
	RAM(toc + 4) = 2;
	RAM(toc + 8) = 0x61703272, RAM(toc + 12) = TXF, RAM(toc + 16) = FL;
	RAM(toc + 20) = 0x72326170, RAM(toc + 24) = RXF, RAM(toc + 28) = FL;
	/* Linux's last request, on its channel 3, still in the FIFO near the end */
	u32 at = FL - 16;
	RAM(TXF + 8 + at) = 9 | 3 << 16;
	RAM(TXF + 8 + (at + 8) % FL) = 40;
	RAM(TXF + 8 + (at + 16) % FL) = 0x00716572;
	RAM(TXF) = RAM(TXF + 4) = (at + 56) % FL;
	RAM(RXF) = 8, RAM(RXF + 4) = 24;                 /* something old, unread */
	CHECK(rpm_init((volatile u8 *)rpm_ram, sizeof(rpm_ram), fake_rpm, 0) == 0, "RPM: the table of contents");
	CHECK(rpm_lcid() == 3, "RPM: Linux's channel id (%u)", rpm_lcid());
	CHECK(rpm_ldo(8, 400000, true) == 0, "RPM: L8 acknowledged");
	CHECK(rpm_got_lcid == 3 && rpm_got_type == RPM_LDOA && rpm_got_id == 8 && rpm_got_uv == 400000 &&
	      rpm_got_swen == 1, "RPM: the request (lcid %u type %x id %u uV %u swen %u)",
	      rpm_got_lcid, rpm_got_type, rpm_got_id, rpm_got_uv, rpm_got_swen);
	for (int i = 0; i < 60; i++) rpm_ldo(23, 3200000, true);     /* round the FIFOs */
	CHECK(rpm_requests == 61 && rpm_got_id == 23, "RPM: 61 requests through (%u)", rpm_requests);
	CHECK(rpm_ldo(99, 0, false) == -2, "RPM: a refusal");
	CHECK(rpm_got_set == RPM_ACTIVE, "RPM: an LDO in the active set");

	/* the sleep set: 12 resources at 0 (14 with the RF buffers), "mmxi"
	 * (mmnrt, mmrt) refused here */
	rpm_refuse_type = 0x69786d6du;
	CHECK(rpm_sleep_votes(false) == 10, "RPM: the sleep set without RF: 10 of 12 taken");
	CHECK(rpm_sleep_reqs == 12 && rpm_got_set == RPM_SLEEP && rpm_got_value == 0,
	      "RPM: 12 sleep-set requests (%u), at 0", rpm_sleep_reqs);
	CHECK(rpm_got_type == 0x00707571u && rpm_got_key == 0x007a484bu, "RPM: the last: qup's rate (%x %x)",
	      rpm_got_type, rpm_got_key);
	rpm_refuse_type = 0;
	CHECK(rpm_sleep_votes(true) == 14, "RPM: the sleep set with RF: 14");

	/* the vMPM (irq-qcom-mpm.c's layout, 96 pins: 3 words a register) */
	u32 vmpm[18];
	memset(vmpm, 0xa5, sizeof(vmpm));
	u32 mask[3] = { 0, 0, 1u << (86 - 64) };          /* SPMI's pin 86 */
	mpm_prepare((volatile u8 *)vmpm, 96, 0x123456789abull, mask);
	CHECK(vmpm[0] == 0x456789ab && vmpm[1] == 0x123, "vMPM: TIMER0/1 (%x %x)", vmpm[0], vmpm[1]);
	CHECK(vmpm[2] == 0 && vmpm[3] == 0 && vmpm[4] == 1u << 22, "vMPM: ENABLE");
	CHECK(vmpm[5] == 0xa5a5a5a5u && vmpm[13] == 0xa5a5a5a5u, "vMPM: the edges and polarity untouched");
	CHECK(vmpm[14] == 0 && vmpm[15] == 0 && vmpm[16] == 0, "vMPM: STATUS cleared");
	vmpm[16] = 1u << 22;
	u32 st[3];
	mpm_status((volatile u8 *)vmpm, 96, st);
	CHECK(st[0] == 0 && st[2] == 1u << 22, "vMPM: the status read back");

	/* the RPM's statistics: the records' offset in the word at 0x14 */
	static u32 stats[0x1000 / 4];
	memset(stats, 0, sizeof(stats));
	stats[0x14 / 4] = 0xabcd0000u | 0x200;
	u32 *r = &stats[0x200 / 4];
	r[0] = 0x6e696d76;                                /* "vmin" */
	r[1] = 7;
	r[6] = 0x1000, r[7] = 1;                          /* accumulated */
	r[8] = 0x5;                                       /* the votes */
	r[12] = 0x776f6c76;                               /* "vlow" */
	r[13] = 9;
	struct rpm_stat rs[2];
	CHECK(rpm_stats_read((volatile u8 *)stats, sizeof(stats), rs) == 2, "RPM stats: two records");
	CHECK(!strcmp(rs[0].name, "vmin") && rs[0].count == 7 && rs[0].accumulated == 0x100001000ull &&
	      rs[0].votes == 5, "RPM stats: vmin (%s %u)", rs[0].name, rs[0].count);
	CHECK(!strcmp(rs[1].name, "vlow") && rs[1].count == 9, "RPM stats: vlow (%s %u)", rs[1].name, rs[1].count);
	stats[0x14 / 4] = 0xffe0;
	CHECK(rpm_stats_read((volatile u8 *)stats, sizeof(stats), rs) == 0, "RPM stats: an offset outside");
}

static void test_stack(void)
{
	struct modem_hw hw;
	struct kos_modem m;
	struct kos_sms s;
	struct kos_call c;
	fake_init(&hw, smem, rmem, test_clock, test_set_clock);
	hw.wlfw = true;
	modem_start(&hw);
	run(20000);
	modem_status(&m);
	const struct wlfw_info *wi = wlfw_info();
	CHECK(wi->found && wi->registered && wi->fw_ready && !wi->failed, "WLFW registered, firmware ready");
	CHECK(wi->have_cap && wi->chip_id == 0x120 && wi->chip_family == 0x4007 && wi->board_id == 0xff &&
	      wi->soc_id == 0x40670000 && wi->fw_version == 0x324103d6, "WLFW capabilities (chip %x, fw %x)",
	      wi->chip_id, wi->fw_version);
	CHECK(!strcmp(wi->timestamp, "2021-12-02 08:20") && !strcmp(wi->build + 24, "WLAN.HL.3.2.4"),
	      "WLFW strings '%s' '%s'", wi->timestamp, wi->build);
	CHECK(fake_stats.wlfw == 2, "the fake WLFW answered twice (%u)", fake_stats.wlfw);
	CHECK(m.state == KOS_MODEM_READY, "ready (state %u: %s)", m.state, m.status);
	CHECK(m.online, "online");
	CHECK(m.registration == KOS_REG_HOME, "registered (%u)", m.registration);
	CHECK(!strcmp(m.operator_name, "KolibriNet"), "operator '%s'", m.operator_name);
	CHECK(m.signal == -67, "signal %d", m.signal);
	CHECK(m.sms_count == 3, "3 stored messages (%u)", m.sms_count);
	CHECK(m.sms_unread == 2, "2 unread (%u)", m.sms_unread);
	CHECK(fake_stats.rmtfs_ok == 1 && !fake_stats.rmtfs_bad, "rmtfs read (%u ok, %u bad)",
	      fake_stats.rmtfs_ok, fake_stats.rmtfs_bad);
	CHECK(clock_set && test_clock() >= 1793271600u && test_clock() < 1793271600u + 60,
	      "the network's time %u", test_clock());
	CHECK(find_sms("How are you?", &s) >= 0 && !strcmp(s.number, "+31641600986") &&
	      s.state == KOS_SMS_READ, "stored message 1");
	CHECK(find_sms("Gr\xc3\xbc\xc3\x9f" "e aus Wien", &s) >= 0 && s.state == KOS_SMS_UNREAD &&
	      s.time == 1793271600u, "stored message 2 (time %u)", s.time);

	/* send: sent, the echo comes back */
	u32 seq = m.sms_seq;
	CHECK(modem_send("+436649876543", "Hallo Kolibri \xc3\xa4") == 0, "send");
	run(500);
	int i = find_sms("Hallo Kolibri", &s);
	CHECK(i >= 0 && s.dir == KOS_SMS_OUT && s.state == KOS_SMS_SENT, "sent (state %u)", s.state);
	CHECK(!strcmp(fake_stats.last_text, "Hallo Kolibri \xc3\xa4"), "the modem got '%s'", fake_stats.last_text);
	run(2000);
	CHECK(find_sms("Echo: Hallo Kolibri", &s) >= 0 && s.dir == KOS_SMS_IN &&
	      !strcmp(s.number, "+436649876543") && s.state == KOS_SMS_UNREAD, "the echo arrived");
	modem_status(&m);
	CHECK(m.sms_seq != seq, "sms_seq changed");
	/* a long one: 3 parts, all sent */
	char longtext[400];
	for (int k = 0; k < 350; k++) longtext[k] = 'A' + k % 20;
	longtext[350] = 0;
	modem_send("+4312345", longtext);
	run(500);
	CHECK(find_sms("ABCDEF", &s) >= 0 && s.state == KOS_SMS_SENT, "3 parts sent (state %u)", s.state);
	CHECK(fake_stats.sent == 4, "the modem got 4 PDUs (%u)", fake_stats.sent);

	/* read, delete */
	i = find_sms("Echo: Hallo", &s);
	CHECK(modem_mark_read(i) == 0 && modem_sms(i, &s) == 0 && s.state == KOS_SMS_READ, "marked read");
	modem_status(&m);
	u32 before = m.sms_count;
	CHECK(modem_delete(i) == 0, "delete");
	run(100);
	modem_status(&m);
	CHECK(m.sms_count == before - 1, "deleted (%u)", m.sms_count);

	/* a call out: dialling, alerting, answered, ended */
	CHECK(modem_dial("+43123456") == 0, "dial");
	run(100);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_DIALING && !strcmp(c.number, "+43123456"), "dialling (%u %s)", c.state, c.number);
	run(1000);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_ALERTING, "alerting (%u)", c.state);
	run(2000);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_ACTIVE && c.dir == KOS_CALL_OUT, "active (%u)", c.state);
	run(3000);
	modem_call(&c);
	CHECK(c.seconds >= 2 && c.seconds <= 4, "talking %u s", c.seconds);
	CHECK(modem_hangup() == 0, "hang up");
	run(500);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_ENDED, "ended (%u)", c.state);
	run(5000);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_NONE, "gone (%u)", c.state);

	/* a call in ("ring" asks the fake for one): incoming, answered, ended */
	modem_send("+4369912345", "ring");
	run(3000);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_INCOMING && c.dir == KOS_CALL_IN && !strcmp(c.number, "+4369912345"),
	      "incoming (%u %s)", c.state, c.number);
	CHECK(modem_answer() == 0, "answer");
	run(500);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_ACTIVE, "answered (%u)", c.state);
	CHECK(fake_stats.answered == 1, "the modem answered");
	modem_hangup();
	run(500);
	modem_call(&c);
	CHECK(c.state == KOS_CALL_ENDED, "ended (%u)", c.state);
	CHECK(fake_stats.dialled == 1 && fake_stats.ended == 2, "dialled %u, ended %u",
	      fake_stats.dialled, fake_stats.ended);
}

/* the kexec case: the modem's OPEN was read by Linux; found in the FIFO */
static void test_preopened(void)
{
	struct modem_hw hw;
	struct kos_modem m;
	memset(&fake_stats, 0, sizeof(fake_stats));
	fake_preopened = true;
	fake_init(&hw, smem, rmem, test_clock, test_set_clock);
	modem_start(&hw);
	run(20000);
	modem_status(&m);
	CHECK(m.state == KOS_MODEM_READY && m.registration == KOS_REG_HOME,
	      "ready with the OPEN from the FIFO's past (state %u: %s)", m.state, m.status);
}

static void test_adopted(void)
{
	struct modem_hw hw;
	struct kos_modem m;
	memset(&fake_stats, 0, sizeof(fake_stats));
	fake_adopted = true;
	fake_init(&hw, smem, rmem, test_clock, test_set_clock);
	hw.adopt_ipcrtr = true;
	modem_start(&hw);
	run(20000);
	modem_status(&m);
	CHECK(modem_ipcrtr_adopted(), "Linux's IPCRTR taken over");
	CHECK(m.state == KOS_MODEM_READY && m.registration == KOS_REG_HOME,
	      "ready on the adopted channel (state %u: %s)", m.state, m.status);
	CHECK(fake_stats.rmtfs_ok == 1, "rmtfs on the adopted channel (%u)", fake_stats.rmtfs_ok);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "adopted")) {
		test_adopted();
		printf("%d passed, %d failed\n", passed, failed);
		return failed != 0;
	}
	if (argc > 1 && !strcmp(argv[1], "preopened")) {
		test_preopened();
		printf("%d passed, %d failed\n", passed, failed);
		return failed != 0;
	}
	test_sms();
	test_rpm();
	test_stack();
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
