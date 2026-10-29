/* KolibriOS for ARM: the modem's code on the PC (make -C tests/modem):
 * the SMS codec against known PDUs and itself, then the whole stack
 * (SMEM, GLINK, QRTR, QMI, rmtfs, phone.c) against the fake modem, tick
 * by tick as the kernel runs it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/modem/fake.h"

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

static void test_stack(void)
{
	struct modem_hw hw;
	struct kos_modem m;
	struct kos_sms s;
	struct kos_call c;
	fake_init(&hw, smem, rmem, test_clock, test_set_clock);
	modem_start(&hw);
	run(20000);
	modem_status(&m);
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

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "preopened")) {
		test_preopened();
		printf("%d passed, %d failed\n", passed, failed);
		return failed != 0;
	}
	test_sms();
	test_stack();
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
