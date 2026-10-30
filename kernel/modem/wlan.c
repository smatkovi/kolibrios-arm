/* KolibriOS for ARM: 802.11 frames of a station (wlan.h), after IEEE
 * 802.11-2016 9.3.3 (management frames), 9.4.2 (elements), mac80211 of
 * Linux 7.1 (mlme.c: ieee80211_send_assoc, ieee80211_add_link_elems,
 * ieee80211_assoc_add_rates, ieee80211_add_ht_ie,
 * ieee80211_add_wmm_info_ie; util.c: ieee80211_put_srates_elem,
 * ieee80211_ie_build_ht_cap; scan.c: is_uapsd_supported; cfg80211's
 * util.c: cfg80211_classify8021d) and ath10k's native Wi-Fi handling
 * (htt_rx.c: ath10k_htt_rx_h_undecap_nwifi, ath10k_htt_rx_nwifi_hdrlen;
 * mac.c: ath10k_tx_h_nwifi). */
#include "wlan.h"

enum { IE_SSID = 0, IE_RATES = 1, IE_DS = 3, IE_TIM = 5, IE_PWR_CAP = 33, IE_SUPP_CHANS = 36, IE_HT_CAP = 45,
       IE_RSN = 48, IE_EXT_RATES = 50, IE_HT_OP = 61, IE_VENDOR = 221, IE_RSNX = 244 };
#define SEL_SAE_H2E_ONLY 123            /* wpa_supplicant's BSS_MEMBERSHIP_SELECTOR_SAE_H2E_ONLY */

/* the band's rates (ath10k's ath10k_g_rates, ath10k_a_rates: 802.11b
 * then 802.11g; 5 GHz 802.11a), in 500 kb/s */
static const u8 rates_24[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
static const u8 rates_5[] = { 12, 18, 24, 36, 48, 72, 96, 108 };

u16 wlan_fc(const u8 *f)
{
	return get16(f);
}

static void hdr(u8 *o, u16 fc, const u8 *a1, const u8 *a2, const u8 *a3)
{
	put16(o, fc);
	put16(o + 2, 0);                        /* the duration: the firmware's */
	mcopy(o + 4, a1, 6);
	mcopy(o + 10, a2, 6);
	mcopy(o + 16, a3, 6);
	put16(o + 22, 0);                       /* the sequence number: the firmware's */
}

int wlan_parse_bss(const u8 *f, u32 len, u16 freq, s32 snr, struct wlan_bss *b)
{
	u16 fc = get16(f) & 0x00fc;
	if (len < WLAN_HDR + 12 || (fc != WLAN_FC_BEACON && fc != WLAN_FC_PROBE_RESP)) return -1;
	mzero(b, sizeof(*b));
	mcopy(b->bssid, f + 16, 6);
	b->freq = freq;
	b->snr = snr;
	b->beacon_int = get16(f + WLAN_HDR + 8);
	b->cap = get16(f + WLAN_HDR + 10);
	b->dtim = 1;
	const u8 *p = f + WLAN_HDR + 12, *end = f + len;
	bool ssid_seen = false;
	while (end - p >= 2 && p + 2 + p[1] <= end) {
		u8 id = p[0], l = p[1];
		const u8 *v = p + 2;
		switch (id) {
		case IE_SSID:
			if (!ssid_seen && l <= 32) {
				mcopy(b->ssid, v, l);
				b->ssid_len = l;
				ssid_seen = true;
			}
			break;
		case IE_RATES: case IE_EXT_RATES:
			for (u8 i = 0; i < l && b->nrates < sizeof(b->rates); i++) b->rates[b->nrates++] = v[i];
			break;
		case IE_DS:
			if (l >= 1) b->chan = v[0];
			break;
		case IE_TIM:
			if (l >= 2 && v[1]) b->dtim = v[1];
			break;
		case IE_HT_CAP:
			if (l >= 26) {
				b->ht = true;
				b->ht_cap.cap = get16(v);
				b->ht_cap.ampdu = v[2];
				mcopy(b->ht_cap.mcs, v + 3, 16);
			}
			break;
		case IE_HT_OP:
			if (l >= 22) {
				b->ht_op = true;
				b->ht_primary = v[0];
				b->ht_param = v[1];
			}
			break;
		case IE_RSN:
			if (l + 2u <= sizeof(b->rsn)) {
				mcopy(b->rsn, p, l + 2u);
				b->rsn_len = (u8)(l + 2);
			}
			break;
		case IE_RSNX:
			if (l + 2u <= sizeof(b->rsnx)) {
				mcopy(b->rsnx, p, l + 2u);
				b->rsnx_len = (u8)(l + 2);
			}
			break;
		case IE_VENDOR:
			/* WMM (00-50-f2 type 2): information (subtype 0, 7 bytes)
			 * or parameter element (1, 24), version 1 */
			if (l >= 7 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 2 && v[5] == 1) {
				if (v[4] == 0 && l == 7) {
					b->wmm = true;
					if (!b->wmm_len) b->uapsd = v[6] & WMM_QOSINFO_AP_UAPSD;
				} else if (v[4] == 1 && l == 24) {
					b->wmm = true;
					b->uapsd = v[6] & WMM_QOSINFO_AP_UAPSD;
					mcopy(b->wmm_param, v, 24);
					b->wmm_len = 24;
				}
			}
			break;
		}
		if (id == IE_RATES || id == IE_EXT_RATES)
			for (u8 i = 0; i < l; i++)
				if (v[i] == (0x80 | SEL_SAE_H2E_ONLY)) b->h2e_only = true;
		p += 2 + l;
	}
	return ssid_seen ? 0 : -1;
}

u32 wlan_probe_ies(u8 *o, bool band5)
{
	/* mac80211's ieee80211_build_preq_ies_band: the band's rates, 8 in
	 * Supported Rates, the rest in Extended Supported Rates */
	static const u8 r24[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
	static const u8 r5[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
	const u8 *r = band5 ? r5 : r24;
	u32 n = band5 ? sizeof(r5) : sizeof(r24), k = MIN(n, 8u), len = 0;
	o[len++] = IE_RATES;
	o[len++] = (u8)k;
	mcopy(o + len, r, k);
	len += k;
	if (n > k) {
		o[len++] = IE_EXT_RATES;
		o[len++] = (u8)(n - k);
		mcopy(o + len, r + k, n - k);
		len += n - k;
	}
	return len;
}

u32 wlan_auth(u8 *o, const u8 bssid[6], const u8 own[6])
{
	hdr(o, WLAN_FC_AUTH, bssid, own, bssid);
	put16(o + WLAN_HDR, 0);                 /* open system */
	put16(o + WLAN_HDR + 2, 1);             /* transaction 1 */
	put16(o + WLAN_HDR + 4, 0);             /* status */
	return WLAN_HDR + 6;
}

static void put_rates(u8 *o, u32 *n, u8 id, const u8 *r, u32 k)
{
	o[(*n)++] = id;
	o[(*n)++] = (u8)k;
	mcopy(o + *n, r, k);
	*n += k;
}

u32 wlan_assoc_req(u8 *o, const struct wlan_bss *b, const u8 own[6], const struct wlan_assoc *a)
{
	hdr(o, WLAN_FC_ASSOC_REQ, b->bssid, own, b->bssid);
	/* the capabilities as ieee80211_send_assoc and
	 * ieee80211_add_link_elems: the BSS's privacy, ESS (pre-EHT), in 2.4
	 * GHz short slot and short preamble, spectrum management when the
	 * BSS has it (ath10k: SPECTRUM_MGMT) */
	u16 cap = WLAN_CAP_ESS;
	if (b->cap & WLAN_CAP_PRIVACY) cap |= WLAN_CAP_PRIVACY;
	if (b->freq < 5000) cap |= WLAN_CAP_SHORT_SLOT | WLAN_CAP_SHORT_PREAMBLE;
	if (b->cap & WLAN_CAP_SPECTRUM_MGMT) cap |= WLAN_CAP_SPECTRUM_MGMT;
	put16(o + WLAN_HDR, cap);
	put16(o + WLAN_HDR + 2, WLAN_LISTEN_INTERVAL);
	u32 n = WLAN_HDR + 4;
	o[n++] = IE_SSID;
	o[n++] = b->ssid_len;
	mcopy(o + n, b->ssid, b->ssid_len);
	n += b->ssid_len;
	/* ieee80211_assoc_add_rates: the band's rates the BSS has (none
	 * marked basic), 8 in Supported Rates, the rest in Extended */
	u8 r[12];
	u32 k = 0;
	const u8 *band = b->freq < 5000 ? rates_24 : rates_5;
	u32 nb = b->freq < 5000 ? sizeof(rates_24) : sizeof(rates_5);
	for (u32 i = 0; i < nb; i++)
		for (u32 j = 0; j < b->nrates; j++)
			if ((b->rates[j] & 0x7f) == band[i]) {
				r[k++] = band[i];
				break;
			}
	if (!k) {
		/* (none in common, or none told: all of ours) */
		mcopy(r, band, nb);
		k = nb;
	}
	put_rates(o, &n, IE_RATES, r, MIN(k, 8u));
	if (k > 8) put_rates(o, &n, IE_EXT_RATES, r + 8, k - 8);
	if (cap & WLAN_CAP_SPECTRUM_MGMT) {
		/* the power capability (0, the channel's max), the supported
		 * channels (each one by itself) */
		o[n++] = IE_PWR_CAP, o[n++] = 2, o[n++] = 0, o[n++] = (u8)a->max_power;
		o[n++] = IE_SUPP_CHANS;
		o[n++] = (u8)(2 * a->nchans);
		for (u32 i = 0; i < a->nchans; i++) o[n++] = a->chans[i], o[n++] = 1;
	}
	if (a->rsn_len) {
		mcopy(o + n, a->rsn, a->rsn_len);
		n += a->rsn_len;
	}
	if (a->ht) {
		/* ieee80211_ie_build_ht_cap: the info, the A-MPDU parameters,
		 * the MCS set, the rest (extended, beamforming, antenna
		 * selection) zero */
		o[n++] = IE_HT_CAP;
		o[n++] = 26;
		mzero(o + n, 26);
		put16(o + n, a->ht->cap);
		o[n + 2] = a->ht->ampdu;
		mcopy(o + n + 3, a->ht->mcs, 16);
		n += 26;
	}
	if (a->rsnx_len) {
		/* (an extra element after HT: wpa_supplicant's RSNXE goes with
		 * the RSN element into assoc_data->ie, split by mac80211 after
		 * the HT element: ieee80211_add_before_vht_elems) */
		mcopy(o + n, a->rsnx, a->rsnx_len);
		n += a->rsnx_len;
	}
	if (a->wmm_qos >= 0) {
		/* ieee80211_add_wmm_info_ie */
		static const u8 w[] = { IE_VENDOR, 7, 0x00, 0x50, 0xf2, 2, 0, 1 };
		mcopy(o + n, w, sizeof(w));
		n += sizeof(w);
		o[n++] = (u8)a->wmm_qos;
	}
	return n;
}

u32 wlan_auth_alg(u8 *o, const u8 bssid[6], const u8 own[6], u16 alg, u16 seq, u16 status, const u8 *body, u32 len)
{
	hdr(o, WLAN_FC_AUTH, bssid, own, bssid);
	put16(o + WLAN_HDR, alg);
	put16(o + WLAN_HDR + 2, seq);
	put16(o + WLAN_HDR + 4, status);
	mcopy(o + WLAN_HDR + 6, body, len);
	return WLAN_HDR + 6 + len;
}

u32 wlan_sa_query(u8 *o, const u8 bssid[6], const u8 own[6], u8 action, const u8 id[2], u64 pn)
{
	hdr(o, WLAN_FC_ACTION | WLAN_FC_PROTECTED, bssid, own, bssid);
	u8 *h = o + WLAN_HDR;
	/* the CCMP header: PN0, PN1, reserved, ExtIV and key id 0, PN2-PN5 */
	h[0] = (u8)pn, h[1] = (u8)(pn >> 8), h[2] = 0, h[3] = 0x20;
	h[4] = (u8)(pn >> 16), h[5] = (u8)(pn >> 24), h[6] = (u8)(pn >> 32), h[7] = (u8)(pn >> 40);
	u8 *b = h + 8;
	b[0] = 8;                               /* WLAN_ACTION_SA_QUERY */
	b[1] = action;
	b[2] = id[0], b[3] = id[1];
	mzero(b + 4, 8);                        /* the MIC's room */
	return WLAN_HDR + 8 + 4 + 8;
}

int wlan_ccmp_mgmt(const u8 *f, u32 len, u64 *pn, const u8 **body, u32 *blen)
{
	if (len < WLAN_HDR + 8 + 8 || !(get16(f) & WLAN_FC_PROTECTED)) return -1;
	const u8 *h = f + WLAN_HDR;
	if (!(h[3] & 0x20)) return -1;          /* ExtIV */
	*pn = h[0] | (u64)h[1] << 8 | (u64)h[4] << 16 | (u64)h[5] << 24 | (u64)h[6] << 32 | (u64)h[7] << 40;
	*body = h + 8;
	*blen = len - WLAN_HDR - 8 - 8;
	return 0;
}

static bool to_us(const u8 *f, const u8 bssid[6], const u8 own[6])
{
	for (int i = 0; i < 6; i++)
		if (f[4 + i] != own[i] || f[10 + i] != bssid[i]) return false;
	return true;
}

int wlan_auth_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status)
{
	if (len < WLAN_HDR + 6 || (get16(f) & 0x00fc) != WLAN_FC_AUTH || !to_us(f, bssid, own)) return -1;
	if (get16(f + WLAN_HDR) != 0 || get16(f + WLAN_HDR + 2) != 2) return -1;
	*status = get16(f + WLAN_HDR + 4);
	return 0;
}

int wlan_assoc_resp(const u8 *f, u32 len, const u8 bssid[6], const u8 own[6], u16 *status, u16 *aid)
{
	if (len < WLAN_HDR + 6 || (get16(f) & 0x00fc) != WLAN_FC_ASSOC_RESP || !to_us(f, bssid, own)) return -1;
	*status = get16(f + WLAN_HDR + 2);
	*aid = get16(f + WLAN_HDR + 4) & 0x3fff;
	return 0;
}

int wlan_assoc_resp_ies(const u8 *f, u32 len, u8 wmm[24], u8 *wmm_len, struct wlan_ht *ht, bool *have_ht)
{
	*wmm_len = 0;
	*have_ht = false;
	if (len < WLAN_HDR + 6) return -1;
	const u8 *p = f + WLAN_HDR + 6, *end = f + len;
	while (end - p >= 2 && p + 2 + p[1] <= end) {
		const u8 *v = p + 2;
		u8 l = p[1];
		if (p[0] == IE_VENDOR && l == 24 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 2 && v[4] == 1 &&
		    v[5] == 1) {
			mcopy(wmm, v, 24);
			*wmm_len = 24;
		} else if (p[0] == IE_HT_CAP && l >= 26) {
			*have_ht = true;
			ht->cap = get16(v);
			ht->ampdu = v[2];
			mcopy(ht->mcs, v + 3, 16);
		}
		p += 2 + l;
	}
	return 0;
}

int wlan_deauth(const u8 *f, u32 len, const u8 bssid[6], u16 *reason)
{
	u16 fc = get16(f) & 0x00fc;
	if (len < WLAN_HDR + 2 || (fc != WLAN_FC_DEAUTH && fc != WLAN_FC_DISASSOC)) return -1;
	for (int i = 0; i < 6; i++)
		if (f[10 + i] != bssid[i]) return -1;
	*reason = get16(f + WLAN_HDR);
	return 0;
}

static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

static bool is_snap(const u8 *p)
{
	for (int i = 0; i < 6; i++)
		if (p[i] != snap[i]) return false;
	return true;
}

u32 wlan_from_eth(u8 *o, u32 cap, const u8 *eth, u32 len, const u8 bssid[6], bool protect)
{
	if (len < 14 || WLAN_HDR + 8 + len - 14 > cap) return 0;
	hdr(o, WLAN_FC_DATA | WLAN_FC_TODS | (protect ? WLAN_FC_PROTECTED : 0), bssid, eth + 6, eth);
	mcopy(o + WLAN_HDR, snap, 6);
	mcopy(o + WLAN_HDR + 6, eth + 12, 2);
	mcopy(o + WLAN_HDR + 8, eth + 14, len - 14);
	return WLAN_HDR + 8 + len - 14;
}

u32 wlan_to_eth(u8 *o, u32 cap, const u8 *f, u32 len)
{
	u16 fc = get16(f);
	if (len < WLAN_HDR || (fc & 0x000c) != WLAN_FC_DATA) return 0;
	/* the native Wi-Fi header: 3 addresses, no QoS control; 4 addresses
	 * (both DS bits) padded to 32 bytes (no
	 * ATH10K_FW_FEATURE_NO_NWIFI_DECAP_4ADDR_PADDING in the firmware) */
	bool to = fc & WLAN_FC_TODS, from = fc & WLAN_FC_FROMDS;
	u32 hl = to && from ? 30 : WLAN_HDR;
	if (fc & WLAN_FC_QOS) hl += 2;      /* (a QoS header the firmware left: ieee80211_hdrlen) */
	hl = (hl + 3) & ~3u;
	if (len < hl + 8 || !is_snap(f + hl)) return 0;
	const u8 *da = to ? f + 16 : f + 4;                 /* ieee80211_get_DA */
	const u8 *sa = to && from ? f + 24 : from ? f + 16 : f + 10;   /* ieee80211_get_SA */
	u32 n = 14 + len - hl - 8;
	if (n > cap) return 0;
	mcopy(o, da, 6);
	mcopy(o + 6, sa, 6);
	mcopy(o + 12, f + hl + 6, 2);
	mcopy(o + 14, f + hl + 8, len - hl - 8);
	return n;
}

u8 wlan_classify(const u8 *eth, u32 len)
{
	if (len < 14) return 0;
	u16 type = (u16)(eth[12] << 8 | eth[13]);
	u32 dscp;
	if (type == 0x0800 && len >= 16) dscp = eth[15] & 0xfc;                   /* ipv4_get_dsfield */
	else if (type == 0x86dd && len >= 16) dscp = ((eth[14] << 4 | eth[15] >> 4) & 0xfc);   /* ipv6_get_dsfield */
	else if (type == 0x8917) return 7;                                         /* ETH_P_80221 */
	else return 0;
	u8 r = (u8)(dscp >> 5);
	/* RFC 8325 section 4 */
	switch (dscp >> 2) {
	case 10: case 12: case 14: r = 0; break;     /* AF11-13 */
	case 16: r = 0; break;                       /* CS2 */
	case 18: case 20: case 22: r = 3; break;     /* AF21-23 */
	case 24: r = 4; break;                       /* CS3 */
	case 26: case 28: case 30: r = 4; break;     /* AF31-33 */
	case 44: r = 6; break;                       /* VOICE-ADMIT */
	case 46: r = 6; break;                       /* EF */
	case 48: r = 7; break;                       /* CS6 */
	}
	return r;
}
