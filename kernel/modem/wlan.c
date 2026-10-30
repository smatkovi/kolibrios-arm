/* KolibriOS for ARM: 802.11 frames of a station (wlan.h), after IEEE
 * 802.11-2016 9.3.3 (management frames), 9.4.2 (elements), and ath10k's
 * native Wi-Fi handling (htt_rx.c: ath10k_htt_rx_h_undecap_nwifi,
 * ath10k_htt_rx_nwifi_hdrlen; mac.c: ath10k_tx_h_nwifi). */
#include "wlan.h"

enum { IE_SSID = 0, IE_RATES = 1, IE_DS = 3, IE_TIM = 5, IE_HT_CAP = 45, IE_RSN = 48, IE_EXT_RATES = 50 };

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
			b->ht = true;
			break;
		case IE_RSN:
			if (l + 2u <= sizeof(b->rsn)) {
				mcopy(b->rsn, p, l + 2u);
				b->rsn_len = (u8)(l + 2);
			}
			break;
		}
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

u32 wlan_assoc_req(u8 *o, const struct wlan_bss *b, const u8 own[6], const u8 *rsn, u32 rsn_len)
{
	hdr(o, WLAN_FC_ASSOC_REQ, b->bssid, own, b->bssid);
	/* the capabilities as mac80211's ieee80211_send_assoc: ESS, the
	 * BSS's short preamble and (2.4 GHz) short slot time, privacy */
	u16 cap = WLAN_CAP_ESS;
	if (b->cap & WLAN_CAP_SHORT_PREAMBLE) cap |= WLAN_CAP_SHORT_PREAMBLE;
	if (b->freq < 5000 && (b->cap & WLAN_CAP_SHORT_SLOT)) cap |= WLAN_CAP_SHORT_SLOT;
	if (b->cap & WLAN_CAP_PRIVACY) cap |= WLAN_CAP_PRIVACY;
	put16(o + WLAN_HDR, cap);
	put16(o + WLAN_HDR + 2, WLAN_LISTEN_INTERVAL);
	u32 n = WLAN_HDR + 4;
	o[n++] = IE_SSID;
	o[n++] = b->ssid_len;
	mcopy(o + n, b->ssid, b->ssid_len);
	n += b->ssid_len;
	/* the BSS's rates (all of them 802.11a/b/g rates the WCN3990 has) */
	u32 k = MIN((u32)b->nrates, 8u);
	o[n++] = IE_RATES;
	o[n++] = (u8)k;
	mcopy(o + n, b->rates, k);
	n += k;
	if (b->nrates > k) {
		o[n++] = IE_EXT_RATES;
		o[n++] = (u8)(b->nrates - k);
		mcopy(o + n, b->rates + k, b->nrates - k);
		n += b->nrates - k;
	}
	if (rsn_len) {
		mcopy(o + n, rsn, rsn_len);
		n += rsn_len;
	}
	return n;
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
	u32 hl = to && from ? 32 : WLAN_HDR;
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
