/* lib/json.h on the PC (make -C tests/messenger): the messenger's JSON
 * reader against the lines its backends write. */
#include <stdio.h>
#include <string.h>
#include "../../lib/json.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct jv nodes[256];
static char sb[4096];
static struct json j;

int main(void)
{
	json_init(&j, nodes, 256, sb, sizeof(sb));
	/* Yappari's Message, as Go writes it ("<" as \u003c) */
	int r = json_parse(&j, "{\"args\":[\"3EB0\",\"43@s.whatsapp.net\",\"43@s.whatsapp.net\",\"Anna\",1793247616,"
			       "false,\"text\",\"bis dann \\u003c3\",\"\",0],\"signal\":\"Message\"}");
	CHECK(r == 0 && json_type(&j, r) == J_OBJ, "an object");
	int a = json_get(&j, r, "args");
	CHECK(json_len(&j, a) == 10, "ten arguments (%d)", json_len(&j, a));
	CHECK(!strcmp(json_str(&j, json_get(&j, r, "signal")), "Message"), "the signal's name");
	CHECK(json_num(&j, json_at(&j, a, 4)) == 1793247616LL, "the time");
	CHECK(json_type(&j, json_at(&j, a, 5)) == J_FALSE && !json_true(&j, json_at(&j, a, 5)), "fromMe false");
	CHECK(!strcmp(json_str(&j, json_at(&j, a, 7)), "bis dann <3"), "\\u003c decoded: %s", json_str(&j, json_at(&j, a, 7)));
	CHECK(json_at(&j, a, 10) == -1 && json_get(&j, r, "nope") == -1, "past the end, a missing key");
	/* nested lists: GetContacts' result; UTF-8 as it is, \u00fc and a surrogate pair */
	r = json_parse(&j, " {\"id\": 12, \"result\": [[[\"a\", \"Gr\\u00fc\\u00dfe\", \"\"], [\"b\", \"Grüße\", \"\\ud83d\\ude00\"]]]} ");
	int list = json_at(&j, json_get(&j, r, "result"), 0);
	CHECK(json_num(&j, json_get(&j, r, "id")) == 12 && json_len(&j, list) == 2, "id 12, two contacts");
	const char *n1 = json_str(&j, json_at(&j, json_at(&j, list, 0), 1)), *n2 = json_str(&j, json_at(&j, json_at(&j, list, 1), 1));
	CHECK(!strcmp(n1, "Gr\xc3\xbc\xc3\x9f" "e") && !strcmp(n1, n2), "\\u00fc\\u00df into UTF-8, the same as written: %s %s", n1, n2);
	CHECK(!strcmp(json_str(&j, json_at(&j, json_at(&j, list, 1), 2)), "\xf0\x9f\x98\x80"), "a surrogate pair: U+1F600");
	/* numbers: negative, a fraction read past, a number as a string (Signal's ids) */
	r = json_parse(&j, "[-5, 2.5e3, \"1761750000000\", null, true]");
	CHECK(json_num(&j, json_at(&j, r, 0)) == -5 && json_num(&j, json_at(&j, r, 1)) == 2, "-5, 2.5e3 as 2");
	CHECK(json_num(&j, json_at(&j, r, 2)) == 1761750000000LL, "a number in a string");
	CHECK(json_type(&j, json_at(&j, r, 3)) == J_NULL && json_true(&j, json_at(&j, r, 4)), "null, true");
	CHECK(!strcmp(json_str(&j, json_at(&j, r, 0)), ""), "a number is no string");
	/* broken lines */
	CHECK(json_parse(&j, "{\"a\": [1, 2") == -1, "an array not closed");
	CHECK(json_parse(&j, "{\"a\" 1}") == -1, "no colon");
	CHECK(json_parse(&j, "hello") == -1, "not JSON");
	/* too many nodes: refused, not overrun */
	char big[2000];
	int o = 0;
	big[o++] = '[';
	for (int i = 0; i < 300; i++) o += sprintf(big + o, "%s1", i ? "," : "");
	big[o++] = ']';
	big[o] = 0;
	CHECK(json_parse(&j, big) == -1, "300 numbers in 256 nodes: refused");
	/* writing: quoted, and read back */
	char q[128];
	unsigned l = json_quote(q, sizeof(q), "say \"hi\"\n\tback\\slash \x01");
	CHECK(l == strlen(q) && !strcmp(q, "\"say \\\"hi\\\"\\n\\tback\\\\slash \\u0001\""), "quoted: %s", q);
	r = json_parse(&j, q);
	CHECK(!strcmp(json_str(&j, r), "say \"hi\"\n\tback\\slash \x01"), "and read back");
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
