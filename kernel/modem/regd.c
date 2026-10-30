/* KolibriOS for ARM: the Wi-Fi's regulatory domain (regd.h), after
 * Linux 7.1: drivers/net/wireless/ath/regd.c (__ath_regd_init,
 * ath_regd_init_wiphy, the world domains' rules, ath_reg_apply_radar_flags,
 * ath_reg_apply_world_flags, ath_reg_notifier_apply), ath10k's mac.c
 * (ath10k_mac_init_rd, ath10k_mac_register's regulatory_hint,
 * ath10k_mac_update_channel_list) and cfg80211's net/wireless/reg.c
 * (handle_channel_custom, handle_channel, handle_channel_single_rule,
 * handle_channel_adjacent_rules, freq_reg_info_regd,
 * reg_rule_to_chan_bw_flags, reg_get_max_bandwidth, map_regdom_flags,
 * reg_process_ht_flags_channel).
 *
 * The order is Linux's: ath_regd_init applies a custom domain before the
 * wiphy is registered (its flags and power become the channels' "orig"
 * values: a world domain's own rules, or for an EEPROM with a country
 * ath's most restrictive world domain 0x64), then for a country EEPROM
 * the driver's hint (REGULATORY_STRICT_REG: the country's rules replace
 * the base), then the configured country as a user's hint (it can only
 * restrict: the orig flags stay, the power is the smaller), the radar
 * flags again (the notifier), the 5 GHz range of SERVICE_READY, HT40.
 * Not done: hints from a Country element (Linux follows an AP's country
 * for a world domain), beacon hints (a beacon seen lifts NO_IR on a
 * world domain's channel), the 5 and 10 MHz fallbacks of a channel that
 * does not fit 20 MHz. */
#include "regd.h"

/* ath10k's channels (mac.c: ath10k_2ghz_channels, ath10k_5ghz_channels),
 * max_power 30 */
static const u16 chans_2g[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const u16 chans_5g[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
				136, 140, 144, 149, 153, 157, 161, 165, 169, 173 };

/* ath's world domains (regd.c: REG_RULE(start - 10, end + 10, bw, 0, eirp,
 * flags), in kHz and mBm) */
#define RULE(s, e, bw, dbm, f) { (s) * 1000u, (e) * 1000u, (bw) * 1000u, (dbm) * 100, (f) }
static const struct regdb_rule ch01_11 = RULE(2412 - 10, 2462 + 10, 40, 20, 0);
static const struct regdb_rule ch12_13 = RULE(2467 - 10, 2472 + 10, 40, 20, FWDB_NO_IR);
static const struct regdb_rule ch14 = RULE(2484 - 10, 2484 + 10, 40, 20, FWDB_NO_IR | FWDB_NO_OFDM);
static const struct regdb_rule g5150_5350 = RULE(5150 - 10, 5350 + 10, 80, 30, FWDB_NO_IR);
static const struct regdb_rule g5470_5850 = RULE(5470 - 10, 5850 + 10, 80, 30, FWDB_NO_IR);
static const struct regdb_rule g5725_5850 = RULE(5725 - 10, 5850 + 10, 80, 30, FWDB_NO_IR);

#define DOM_RULES 8
struct dom {
	u32 n;
	const struct regdb_rule *r[DOM_RULES];
};
static const struct dom world_60_61_62 = { 5, { &ch01_11, &ch12_13, &ch14, &g5150_5350, &g5470_5850 } };
static const struct dom world_63_65 = { 4, { &ch01_11, &ch12_13, &g5150_5350, &g5725_5850 } };
static const struct dom world_64 = { 3, { &ch01_11, &g5150_5350, &g5725_5850 } };
static const struct dom world_66_69 = { 3, { &ch01_11, &g5150_5350, &g5470_5850 } };
static const struct dom world_67_68_6a_6c = { 4, { &ch01_11, &ch12_13, &g5150_5350, &g5470_5850 } };

static bool eq2(const char *a, const char *b) { return a[0] == b[0] && a[1] == b[1]; }

const struct regdb_country *regdb_find(const char *alpha2)
{
	for (u32 i = 0; i < regdb_ncountries; i++)
		if (eq2(regdb_countries[i].alpha2, alpha2)) return &regdb_countries[i];
	return NULL;
}

static struct dom country_dom(const struct regdb_country *c)
{
	struct dom d = { 0, { 0 } };
	/* (tools/gen-regdb.py checks that no country has more) */
	for (u32 i = 0; c && i < c->nrules && d.n < DOM_RULES; i++) d.r[d.n++] = &regdb_rules[c->first + i];
	return d;
}

/* --- ath's side (regd.c) ---------------------------------------------- */

static bool is_wwr_sku(u16 rd)
{
	return (rd & ATH_COUNTRY_ERD_FLAG) != ATH_COUNTRY_ERD_FLAG &&
	       ((rd & ATH_WORLD_SKU_MASK) == ATH_WORLD_SKU_PREFIX || rd == ATH_WORLD);
}

static u16 eeprom_rd(const struct regd *r) { return r->current_rd & ~ATH_WORLDWIDE_ROAMING_FLAG; }

static const struct ath_country *find_country(u16 code)
{
	for (u32 i = 0; i < ath_ncountries; i++)
		if (ath_countries[i].code == code) return &ath_countries[i];
	return NULL;
}

static const struct ath_country *find_country_by_rd(u16 rd)
{
	for (u32 i = 0; i < ath_ncountries; i++)
		if (ath_countries[i].regdmn == rd) return &ath_countries[i];
	return NULL;
}

static const struct ath_regpair *find_pair(u16 rd)
{
	if (rd == ATH_NO_ENUMRD) return NULL;
	for (u32 i = 0; i < ath_nregpairs; i++)
		if (ath_regpairs[i].reg_domain == rd) return &ath_regpairs[i];
	return NULL;
}

/* __ath_regd_init */
static int ath_init(struct regd *r, const char **why)
{
	/* ath_regd_sanitize: 0x8000 alone is taken as 0x64 */
	if (r->current_rd == ATH_COUNTRY_ERD_FLAG) r->current_rd = 0x64;
	u16 rd = eeprom_rd(r);
	/* ath_regd_is_eeprom_valid */
	bool valid = false;
	if (rd & ATH_COUNTRY_ERD_FLAG) valid = find_country(rd & ~ATH_COUNTRY_ERD_FLAG) != NULL;
	else
		for (u32 i = 0; i < ath_nregpairs && !valid; i++) valid = ath_regpairs[i].reg_domain == rd;
	if (!valid) {
		*why = "the EEPROM's regulatory domain is not one of ath's (Linux: \"Invalid EEPROM contents\")";
		return -1;
	}
	/* ath_regd_get_default_country */
	r->country_code = ATH_CTRY_DEFAULT;
	if ((rd & ATH_COUNTRY_ERD_FLAG) && find_country(rd & ~ATH_COUNTRY_ERD_FLAG))
		r->country_code = rd & ~ATH_COUNTRY_ERD_FLAG;
	if (r->country_code == ATH_CTRY_DEFAULT && rd == ATH_CTRY_DEFAULT) r->country_code = ATH_CTRY_UNITED_STATES;
	const struct ath_country *c = NULL;
	u16 regdmn = rd;
	if (r->country_code != ATH_CTRY_DEFAULT) {
		c = find_country(r->country_code);
		if (!c) {
			*why = "no ath country for the EEPROM's code";
			return -1;
		}
		regdmn = c->regdmn;
	}
	const struct ath_regpair *p = find_pair(regdmn);
	if (!p) {
		*why = "no ath domain pair for the EEPROM's code";
		return -1;
	}
	r->pair = *p;
	if (!c) c = find_country_by_rd(regdmn);
	r->alpha2[0] = c ? c->iso[0] : '0';
	r->alpha2[1] = c ? c->iso[1] : '0';
	r->alpha2[2] = 0;
	r->world = is_wwr_sku(eeprom_rd(r));
	return 0;
}

static const struct dom *world_dom(u16 reg_domain)
{
	switch (reg_domain) {
	case 0x60: case 0x61: case 0x62: return &world_60_61_62;
	case 0x63: case 0x65: return &world_63_65;
	case 0x64: return &world_64;
	case 0x66: case 0x69: return &world_66_69;
	case 0x67: case 0x68: case 0x6a: case 0x6c: return &world_67_68_6a_6c;
	default: return &world_64;      /* ath_default_world_regdomain (Linux WARNs) */
	}
}

/* ath_is_radar_freq */
static bool radar_freq(const struct regd *r, u16 f)
{
	if (r->country_code == ATH_CTRY_INDIA) return f >= 5500 && f <= 5700;
	return f >= 5260 && f <= 5700;
}

/* ath_reg_apply_radar_flags: always on 5260-5700 MHz */
static void radar_flags(struct regd *r)
{
	for (u32 i = 0; i < r->nchan; i++) {
		struct regd_chan *c = &r->ch[i];
		if (c->freq > 5000 && radar_freq(r, c->freq) && !(c->flags & REGD_DISABLED)) c->flags |= REGD_RADAR | REGD_NO_IR;
	}
}

/* --- cfg80211's side (reg.c) ---------------------------------------------- */

static u32 map_flags(u8 f)
{
	u32 c = 0;
	if (f & FWDB_NO_IR) c |= REGD_NO_IR;
	if (f & FWDB_DFS) c |= REGD_RADAR;
	if (f & FWDB_NO_OFDM) c |= REGD_NO_OFDM;
	if (f & FWDB_NO_OUTDOOR) c |= REGD_INDOOR_ONLY;
	return c;
}

static bool fits(const struct regdb_rule *r, u32 center_khz, u32 bw_khz)
{
	return center_khz - bw_khz / 2 >= r->start_khz && center_khz + bw_khz / 2 <= r->end_khz;
}

/* freq_reg_info_regd for 20 MHz (cfg80211's min_bw above 1 GHz) */
static const struct regdb_rule *rule_for(const struct dom *d, u32 center_khz)
{
	for (u32 i = 0; i < d->n; i++)
		if (fits(d->r[i], center_khz, 20000)) return d->r[i];
	return NULL;
}

/* reg_get_max_bandwidth with AUTO-BW: the contiguous rules' whole range */
static u32 max_bw(const struct dom *d, const struct regdb_rule *rule)
{
	if (!(rule->flags & FWDB_AUTO_BW)) return rule->max_bw_khz;
	u32 idx = 0;
	while (idx < d->n && d->r[idx] != rule) idx++;
	if (idx == d->n) return 0;
	u32 start = rule->start_khz, end = rule->end_khz;
	for (u32 no = idx; no > 0 && d->r[no - 1]->end_khz >= start; no--) start = d->r[no - 1]->start_khz;
	for (u32 no = idx; no + 1 < d->n && d->r[no + 1]->start_khz <= end; no++) end = d->r[no + 1]->end_khz;
	return end - start;
}

/* reg_rule_to_chan_bw_flags */
static u32 bw_flags(const struct dom *d, const struct regdb_rule *rule, u32 center_khz)
{
	u32 bw = max_bw(d, rule), f = 0;
	if (!fits(rule, center_khz, 20000) || bw < 20000) f |= REGD_NO_20MHZ;
	if (bw < 40000) f |= REGD_NO_HT40;
	if (bw < 80000) f |= REGD_NO_80MHZ;
	if (bw < 160000) f |= REGD_NO_160MHZ;
	return f;
}

/* handle_channel_custom (before the wiphy's registration: the result is
 * its orig flags and power) */
static void custom(struct regd_chan *c, const struct dom *d)
{
	const struct regdb_rule *rule = rule_for(d, c->freq * 1000u);
	if (!rule) {
		c->orig_flags |= REGD_DISABLED;
		c->flags = c->orig_flags;
		return;
	}
	c->flags |= map_flags(rule->flags) | bw_flags(d, rule, c->freq * 1000u);
	c->max_reg_power = c->max_power = (s8)(rule->eirp_mbm / 100);
}

/* handle_channel for a hint: the driver's (strict: the base replaced) or
 * the user's (on top of the base) */
static void hint(struct regd_chan *c, const struct dom *d, bool driver)
{
	u32 khz = c->freq * 1000u;
	const struct regdb_rule *rule = rule_for(d, khz), *r1 = NULL, *r2 = NULL;
	if (!rule) {
		r1 = rule_for(d, khz - 20000);
		r2 = rule_for(d, khz + 20000);
		bool adj = r1 && r2 && r1->end_khz == r2->start_khz && khz - 10000 >= r1->start_khz &&
			   khz + 10000 <= r2->end_khz;
		if (!adj) {
			if (driver) {
				c->orig_flags |= REGD_DISABLED;
				c->flags = c->orig_flags;
			} else {
				c->flags |= REGD_DISABLED;
			}
			return;
		}
	}
	u32 f;
	s8 reg;
	if (rule) {
		f = map_flags(rule->flags) | bw_flags(d, rule, khz);
		reg = (s8)(rule->eirp_mbm / 100);
	} else {
		/* handle_channel_adjacent_rules: both rules' flags, the
		 * smaller power, 20 MHz fits the combined range */
		f = (map_flags(r1->flags) | map_flags(r2->flags) | bw_flags(d, r1, khz) | bw_flags(d, r2, khz)) & ~REGD_NO_20MHZ;
		s8 p1 = (s8)(r1->eirp_mbm / 100), p2 = (s8)(r2->eirp_mbm / 100);
		reg = MIN(p1, p2);
	}
	if (driver) {
		c->flags = c->orig_flags = f;
		c->max_reg_power = c->max_power = c->orig_mpwr = reg;
		return;
	}
	c->flags = c->orig_flags | f;
	c->max_reg_power = reg;
	c->max_power = c->orig_mpwr ? MIN(c->orig_mpwr, reg) : reg;
}

static struct regd_chan *chan_at(struct regd *r, u16 freq)
{
	for (u32 i = 0; i < r->nchan; i++)
		if (r->ch[i].freq == freq) return &r->ch[i];
	return NULL;
}

const struct regd_chan *regd_find(const struct regd *r, u16 freq)
{
	return chan_at((struct regd *)r, freq);
}

/* reg_process_ht_flags_channel (the regdb's rules have no HT40+/- flags) */
static bool ht40_ok(const struct regd_chan *c)
{
	return c && !(c->flags & REGD_DISABLED) && (c->flags & REGD_NO_HT40) != REGD_NO_HT40;
}

static void ht_flags(struct regd *r)
{
	for (u32 i = 0; i < r->nchan; i++) {
		struct regd_chan *c = &r->ch[i];
		if (!ht40_ok(c)) {
			c->flags |= REGD_NO_HT40;
			continue;
		}
		/* the channels 20 MHz below and above, in the same band */
		struct regd_chan *b = chan_at(r, (u16)(c->freq - 20)), *a = chan_at(r, (u16)(c->freq + 20));
		if (b && (b->freq > 5000) != (c->freq > 5000)) b = NULL;
		if (a && (a->freq > 5000) != (c->freq > 5000)) a = NULL;
		if (ht40_ok(b)) c->flags &= ~REGD_NO_HT40MINUS;
		else c->flags |= REGD_NO_HT40MINUS;
		if (ht40_ok(a)) c->flags &= ~REGD_NO_HT40PLUS;
		else c->flags |= REGD_NO_HT40PLUS;
	}
}

int regd_init(struct regd *r, u32 eeprom, const char *country, u16 low_5ghz, u16 high_5ghz, const char **why)
{
	mzero(r, sizeof(*r));
	*why = NULL;
	r->eeprom = (u16)eeprom;
	r->current_rd = (u16)eeprom;
	if (ath_init(r, why)) return -1;
	if (country && country[0] && country[1] && !eq2(country, "00")) {
		r->user[0] = country[0], r->user[1] = country[1];
	}
	/* the channels, max_power 30 (CHAN2G, CHAN5G) */
	for (u32 i = 0; i < sizeof(chans_2g) / sizeof(chans_2g[0]); i++) {
		u16 ch = chans_2g[i];
		r->ch[r->nchan++] = (struct regd_chan){ ch == 14 ? 2484 : (u16)(2407 + 5 * ch), ch, 0, 0, 30, 0, 0 };
	}
	for (u32 i = 0; i < sizeof(chans_5g) / sizeof(chans_5g[0]); i++)
		r->ch[r->nchan++] = (struct regd_chan){ (u16)(5000 + 5 * chans_5g[i]), chans_5g[i], 0, 0, 30, 0, 0 };
	/* ath_regd_init_wiphy: the custom domain, the radar flags, the
	 * world flags (by the driver: ath_reg_apply_ir_flags for 0x68 forces
	 * NO_IR on 2467 and 2472; the beaconing flags change nothing without
	 * a beacon seen) */
	const struct dom *base = r->world ? world_dom(r->pair.reg_domain) : &world_64;
	for (u32 i = 0; i < r->nchan; i++) custom(&r->ch[i], base);
	radar_flags(r);
	if (r->world && r->pair.reg_domain == 0x68) {
		chan_at(r, 2467)->flags |= REGD_NO_IR;
		chan_at(r, 2472)->flags |= REGD_NO_IR;
	}
	/* wiphy_register: what is there now is the original */
	for (u32 i = 0; i < r->nchan; i++) {
		r->ch[i].orig_flags = r->ch[i].flags;
		r->ch[i].orig_mpwr = r->ch[i].max_power;
	}
	/* ath10k_mac_register: a country EEPROM's alpha2 as the driver's hint */
	if (!r->world) {
		const struct regdb_country *c = regdb_find(r->alpha2);
		if (c) {
			struct dom d = country_dom(c);
			for (u32 i = 0; i < r->nchan; i++) hint(&r->ch[i], &d, true);
			radar_flags(r);
			r->dfs_region = c->dfs_region;
		}
	}
	/* the configured country: a user's hint (ath does not follow it:
	 * CONFIG_ATH_REG_DYNAMIC_USER_REG_HINTS is off; cfg80211 does) */
	if (r->user[0]) {
		const struct regdb_country *c = regdb_find(r->user);
		r->user_found = c != NULL;
		if (c) {
			struct dom d = country_dom(c);
			for (u32 i = 0; i < r->nchan; i++) hint(&r->ch[i], &d, false);
			radar_flags(r);
			r->dfs_region = c->dfs_region;
		}
	}
	/* ath10k_mac_update_channel_list: SERVICE_READY's 5 GHz range */
	if (low_5ghz && high_5ghz)
		for (u32 i = 0; i < r->nchan; i++)
			if (r->ch[i].freq > 5000 && (r->ch[i].freq < low_5ghz || r->ch[i].freq > high_5ghz))
				r->ch[i].flags |= REGD_DISABLED;
	ht_flags(r);
	return 0;
}
