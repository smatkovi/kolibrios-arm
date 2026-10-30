/* KolibriOS for ARM: the Wi-Fi's regulatory domain (kernel/modem/regd.c,
 * the tables of regdb.c) on the PC (make -C tests/wifi). The expected
 * values are worked out by hand from the sources the code follows (ath's
 * regd.c and regd_common.h of Linux 7.1, cfg80211's reg.c, wireless-regdb
 * 2026.09.03's db.txt), for:
 *  - the Pro1X's EEPROM (0x406c: Linux's log on the phone, "ath: EEPROM
 *    regdomain: 0x406c", "Regpair used: 0x6c") with country=AT, and
 *    without a country;
 *  - an EEPROM of 0 (ath: the United States), with AT on top: only what
 *    both allow;
 *  - a country in the EEPROM (Germany), 0x8000 alone (sanitized to 0x64),
 *    EU1_WORLD (0x68: 12 and 13 passive), India (radar from 5500 MHz),
 *    an invalid code, SERVICE_READY's 5 GHz range. */
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/modem/regd.h"

static int failed, passed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct regd_chan *ch(const struct regd *r, u16 f)
{
	const struct regd_chan *c = regd_find(r, f);
	if (!c) {
		printf("no channel %u\n", f);
		exit(1);
	}
	return c;
}

static bool on(const struct regd *r, u16 f) { return !(ch(r, f)->flags & REGD_DISABLED); }
static bool passive(const struct regd *r, u16 f) { return ch(r, f)->flags & (REGD_NO_IR | REGD_RADAR); }
static bool radar(const struct regd *r, u16 f) { return ch(r, f)->flags & REGD_RADAR; }
static int pwr(const struct regd *r, u16 f) { return ch(r, f)->max_power; }
static u32 count(const struct regd *r)
{
	u32 n = 0;
	for (u32 i = 0; i < r->nchan; i++) n += !(r->ch[i].flags & REGD_DISABLED);
	return n;
}

int main(void)
{
	struct regd r;
	const char *why;

	/* the tables themselves */
	CHECK(regdb_find("AT") && regdb_find("AT")->dfs_region == 2 && regdb_find("00") && regdb_find("US")->dfs_region == 1,
	      "regdb: AT (DFS-ETSI), 00, US (DFS-FCC)");
	const struct regdb_rule *at = &regdb_rules[regdb_find("AT")->first];
	CHECK(at[0].start_khz == 2400000 && at[0].end_khz == 2483500 && at[0].eirp_mbm == 2000 && at[1].eirp_mbm == 2301 &&
	      at[3].eirp_mbm == 2698 && at[4].eirp_mbm == 1397 && (at[2].flags & FWDB_DFS) && (at[1].flags & FWDB_AUTO_BW),
	      "regdb: AT's rules (100 mW 2000 mBm, 200 mW 2301, 500 mW 2698, 25 mW 1397, as db2fw.py truncates)");

	/* the Pro1X: 0x406c (world roaming, WORC_WORLD), AT */
	CHECK(regd_init(&r, 0x406c, "AT", 0, 0, &why) == 0, "0x406c taken");
	CHECK(r.world && r.pair.reg_domain == 0x6c && r.pair.ctl_2g == ATH_NO_CTL && r.pair.ctl_5g == ATH_NO_CTL &&
	      !strcmp(r.alpha2, "00") && r.country_code == 0 && r.user_found && r.dfs_region == 2,
	      "0x406c: a world domain, pair 0x6c, no CTL, alpha2 00 (Linux: \"Country alpha2 being used: 00\")");
	CHECK(count(&r) == 39, "AT on 0x6c: 39 channels (%u)", count(&r));
	CHECK(on(&r, 2412) && !passive(&r, 2412) && pwr(&r, 2412) == 20 && on(&r, 2462) && !passive(&r, 2462),
	      "1-11: active, 20 dBm");
	CHECK(on(&r, 2467) && passive(&r, 2467) && on(&r, 2472) && passive(&r, 2472), "12, 13: passive (the world domain's NO_IR)");
	CHECK(!on(&r, 2484), "14: off (no rule in 0x6c)");
	CHECK(on(&r, 5180) && passive(&r, 5180) && !radar(&r, 5180) && pwr(&r, 5180) == 23 && ch(&r, 5180)->max_reg_power == 23,
	      "36: passive, 23 dBm (AT's 200 mW under the world's 30)");
	CHECK(radar(&r, 5260) && passive(&r, 5260) && pwr(&r, 5260) == 20, "52: radar, 20 dBm");
	CHECK(radar(&r, 5500) && pwr(&r, 5500) == 26 && radar(&r, 5700), "100: radar, 26 dBm; 140 radar");
	CHECK(on(&r, 5720) && radar(&r, 5720) && pwr(&r, 5720) == 13, "144: across two of AT's rules (adjacent): DFS, 13 dBm");
	CHECK(on(&r, 5745) && passive(&r, 5745) && !radar(&r, 5745) && pwr(&r, 5745) == 13 && on(&r, 5845),
	      "149-169: passive, 13 dBm (25 mW)");
	CHECK(!on(&r, 5865), "173: off (beyond the world domain's 5860)");
	CHECK(!(ch(&r, 2412)->flags & REGD_NO_HT40PLUS) && (ch(&r, 2412)->flags & REGD_NO_HT40MINUS) &&
	      !(ch(&r, 2472)->flags & REGD_NO_HT40MINUS) && (ch(&r, 2472)->flags & REGD_NO_HT40PLUS),
	      "HT40: 1 only up, 13 only down");
	CHECK(!(ch(&r, 5180)->flags & REGD_NO_HT40PLUS) && !(ch(&r, 5200)->flags & REGD_NO_HT40MINUS) &&
	      (ch(&r, 5845)->flags & REGD_NO_HT40PLUS), "HT40: 36 up, 40 down; 169 not up (173 off)");
	CHECK(ch(&r, 5180)->flags & REGD_INDOOR_ONLY, "36: indoor only (NO-OUTDOOR)");

	/* without a country: the world domain alone */
	regd_init(&r, 0x406c, "00", 0, 0, &why);
	CHECK(!r.user[0] && count(&r) == 39 && pwr(&r, 5180) == 30 && passive(&r, 5180) && passive(&r, 5745) &&
	      pwr(&r, 2412) == 20 && radar(&r, 5260) && !radar(&r, 5720) && !on(&r, 5865),
	      "country 00: 0x6c's rules (5 GHz passive at 30 dBm, radar 5260-5700)");

	/* EEPROM 0: the United States (ath's default), AT on top */
	regd_init(&r, 0, "AT", 0, 0, &why);
	CHECK(!r.world && r.country_code == ATH_CTRY_UNITED_STATES && r.pair.reg_domain == 0x3a && r.pair.ctl_2g == 0x10 &&
	      r.pair.ctl_5g == 0x10 && !strcmp(r.alpha2, "US"), "EEPROM 0: US, FCC3_FCCA, CTL FCC");
	CHECK(on(&r, 2462) && !on(&r, 2467) && !on(&r, 2472) && pwr(&r, 2412) == 20, "US with AT: 1-11 only (US), 20 dBm (AT)");

	/* a country in the EEPROM: Germany (276) */
	regd_init(&r, 0x8000 | 276, "", 0, 0, &why);
	CHECK(!r.world && !strcmp(r.alpha2, "DE") && r.pair.reg_domain == 0x37 && r.pair.ctl_2g == 0x30 && r.pair.ctl_5g == 0x30 &&
	      on(&r, 2472) && !passive(&r, 2472) && on(&r, 5180) && !passive(&r, 5180) && radar(&r, 5500),
	      "Germany's EEPROM: ETSI1_WORLD, CTL ETSI; DE's rules as the base (13 active, 36 active)");

	/* 0x8000 alone: ath_regd_sanitize, 0x64 */
	regd_init(&r, 0x8000, "", 0, 0, &why);
	CHECK(r.world && r.pair.reg_domain == 0x64 && !on(&r, 2467) && on(&r, 2462) && !on(&r, 5500) && on(&r, 5745),
	      "0x8000: sanitized to 0x64 (1-11, no mid band)");

	/* EU1_WORLD: 12 and 13 passive by ath_reg_apply_ir_flags */
	regd_init(&r, 0x68, "", 0, 0, &why);
	CHECK(r.world && passive(&r, 2467) && passive(&r, 2472) && !passive(&r, 2462), "0x68: 12, 13 passive");

	/* India: ath's radar range 5500-5700 */
	regd_init(&r, 0x8000 | 356, "", 0, 0, &why);
	CHECK(!strcmp(r.alpha2, "IN") && radar(&r, 5500), "India: radar at 5500");

	/* SERVICE_READY's 5 GHz range */
	regd_init(&r, 0x406c, "AT", 5170, 5330, &why);
	CHECK(on(&r, 5180) && on(&r, 5320) && !on(&r, 5500) && count(&r) == 13 + 8, "5 GHz limited to 5170-5330 (%u)", count(&r));

	/* refused */
	CHECK(regd_init(&r, 0x1234, "AT", 0, 0, &why) == -1 && why, "0x1234: not ath's (%s)", why ? why : "");
	CHECK(regd_init(&r, 0x406c, "QQ", 0, 0, &why) == 0 && !r.user_found && pwr(&r, 5180) == 30,
	      "an unknown country: ignored (the world domain)");

	/* every country of ath's table: a domain pair, the init works */
	u32 bad = 0, nodb = 0;
	for (u32 i = 0; i < ath_ncountries; i++) {
		if (ath_countries[i].regdmn == 0) continue;     /* CTRY_DEBUG */
		if (regd_init(&r, 0x8000 | ath_countries[i].code, "", 0, 0, &why)) bad++;
		else if (!regdb_find(r.alpha2)) nodb++;
	}
	CHECK(bad == 0, "every ath country takes (%u refused)", bad);
	printf("regd: %u ath countries, %u of them not in the regulatory database\n", ath_ncountries, nodb);
	printf("%d passed, %d failed\n", passed, failed);
	return failed != 0;
}
