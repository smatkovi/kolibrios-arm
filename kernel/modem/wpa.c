/* KolibriOS for ARM: WPA2 and WPA3 personal, the supplicant (wpa.h),
 * after wpa_supplicant 2.11: rsn_supp/wpa.c (wpa_supplicant_process_1_of_4,
 * _3_of_4, _send_2_of_4, _send_4_of_4, wpa_supplicant_process_1_of_2,
 * wpa_supplicant_verify_eapol_key_mic, wpa_supplicant_decrypt_key_data,
 * wpa_supplicant_validate_ie, ieee80211w_set_keys), common/wpa_common.c
 * (wpa_parse_wpa_ie_rsn, wpa_pmk_to_ptk, wpa_eapol_key_mic,
 * wpa_compare_rsn_ie), rsn_supp/wpa_ie.c (wpa_gen_wpa_ie_rsn,
 * wpa_gen_rsnxe, the KDEs) and wpa_supplicant.c (wpa_supplicant_set_suites,
 * wpas_set_mgmt_group_cipher).
 *
 * An EAPOL-Key frame (IEEE 802.11-2020 12.7.2): the 802.1X header
 * (version, type 3, length, big endian), the descriptor type (2: RSN), key
 * info, key length, replay counter (8), nonce (32), IV (16), RSC (8),
 * reserved (8), MIC (16 for these AKMs), key data length, key data; big
 * endian. */
#include "wpa.h"
#include "crypto.h"

/* the offsets in an EAPOL frame */
#define O_TYPE 1
#define O_LEN 2
#define O_DESC 4
#define O_INFO 5
#define O_KEYLEN 7
#define O_REPLAY 9
#define O_NONCE 17
#define O_IV 49
#define O_RSC 65
#define O_MIC 81
#define O_DATALEN 97
#define O_DATA 99
#define EAPOL_VERSION 1                 /* wpa_supplicant's default eapol_version */
#define EAPOL_KEY 3
#define DESC_RSN 2

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int wpa_pmk(const char *psk, const u8 *ssid, u32 ssid_len, u8 pmk[WPA_PMK_LEN])
{
	u32 n = mstrlen(psk);
	if (n == 64) {
		for (u32 i = 0; i < 32; i++) {
			int h = hexval(psk[2 * i]), l = hexval(psk[2 * i + 1]);
			if (h < 0 || l < 0) return -1;
			pmk[i] = (u8)(h << 4 | l);
		}
		return 0;
	}
	if (n < 8 || n > 63 || !ssid_len || ssid_len > 32) return -1;
	pbkdf2_sha1(psk, ssid, ssid_len, 4096, pmk, WPA_PMK_LEN);
	return 0;
}

static const u8 oui[3] = { 0x00, 0x0f, 0xac };
#define SUITE_CCMP 4
#define SUITE_BIP_CMAC_128 6

static bool is_suite(const u8 *p, u8 type)
{
	return p[0] == oui[0] && p[1] == oui[1] && p[2] == oui[2] && p[3] == type;
}

/* --- the AP's elements, the choice, ours ------------------------------------ */

int wpa_parse_ap(const u8 *ie, u32 len, const u8 *rsnx, u32 rsnx_len, struct wpa_ap *o, const char **why)
{
	mzero(o, sizeof(*o));
	*why = "a short RSN element";
	if (len < 2 || ie[0] != 48 || ie[1] + 2u > len || ie[1] < 2 || ie[2] != 1 || ie[3] != 0) return -1;
	const u8 *p = ie + 4, *end = ie + 2 + ie[1];
	/* the defaults of a truncated element (wpa_parse_wpa_ie_rsn): group
	 * and pairwise CCMP, AKM 802.1X, no capabilities */
	o->group_ccmp = o->pairwise_ccmp = true;
	o->akms = 1u << 1;
	if (end - p >= 4) {
		o->group_ccmp = is_suite(p, SUITE_CCMP);
		p += 4;
	}
	if (end - p >= 2) {
		u32 n = get16(p);
		p += 2;
		if ((u32)(end - p) < 4 * n || !n) return -1;
		o->pairwise_ccmp = false;
		for (u32 i = 0; i < n; i++, p += 4)
			if (is_suite(p, SUITE_CCMP)) o->pairwise_ccmp = true;
	}
	if (end - p >= 2) {
		u32 n = get16(p);
		p += 2;
		if ((u32)(end - p) < 4 * n || !n) return -1;
		o->akms = 0;
		for (u32 i = 0; i < n; i++, p += 4)
			if (p[0] == oui[0] && p[1] == oui[1] && p[2] == oui[2] && p[3] < 32) o->akms |= 1u << p[3];
	}
	if (end - p >= 2) {
		o->caps = get16(p);
		p += 2;
	}
	/* the PMKIDs, then the group management cipher (BIP-CMAC-128 by
	 * default) */
	o->bip = true;
	if (end - p >= 2) {
		u32 n = get16(p);
		p += 2;
		if ((u32)(end - p) < 16 * n) return -1;
		p += 16 * n;
	}
	if (end - p >= 4) o->bip = is_suite(p, SUITE_BIP_CMAC_128);
	if (rsnx && rsnx_len >= 3 && rsnx[0] == 244 && rsnx[1] >= 1) o->h2e = rsnx[2] & WPA_RSNX_SAE_H2E;
	*why = NULL;
	return 0;
}

int wpa_choose(const struct wpa_ap *ap, bool h2e_only, bool no_pmf, struct wpa_choice *c, const char **why)
{
	mzero(c, sizeof(*c));
	if (!ap->group_ccmp) {
		*why = "the group cipher is not CCMP (TKIP: not done here)";
		return -1;
	}
	if (!ap->pairwise_ccmp) {
		*why = "no CCMP among the pairwise ciphers";
		return -1;
	}
	/* wpa_supplicant_set_suites: SAE before PSK-SHA256 before PSK */
	if (ap->akms & (1u << WPA_AKM_SAE)) c->akm = WPA_AKM_SAE;
	else if (ap->akms & (1u << WPA_AKM_PSK_SHA256)) c->akm = WPA_AKM_PSK_SHA256;
	else if (ap->akms & (1u << WPA_AKM_PSK)) c->akm = WPA_AKM_PSK;
	else {
		*why = "no PSK or SAE among the key managements (802.1X: not done here)";
		return -1;
	}
	/* ieee80211w=1: PMF whenever the AP is capable; BIP-CMAC-128 only
	 * (wpas_set_mgmt_group_cipher: else none, which an AP that requires
	 * PMF refuses) */
	c->mfp = (ap->caps & WPA_CAP_MFPC) && ap->bip && !no_pmf;
	if ((ap->caps & WPA_CAP_MFPR) && no_pmf) {
		*why = "management frame protection required, but pmf=off";
		return -1;
	}
	if ((ap->caps & WPA_CAP_MFPR) && !c->mfp) {
		*why = "management frame protection required with a cipher other than BIP-CMAC-128";
		return -1;
	}
	/* sme_auth_build_sae_commit with sae_pwe 2: H2E when the AP's RSNX
	 * element has it */
	c->h2e = c->akm == WPA_AKM_SAE && ap->h2e;
	if (h2e_only && c->akm == WPA_AKM_SAE && !c->h2e) {
		*why = "SAE hash-to-element only, but not in the AP's RSNX element";
		return -1;
	}
	*why = NULL;
	return 0;
}

const char *wpa_akm_name(u8 akm)
{
	return akm == WPA_AKM_SAE ? "WPA3-SAE" : akm == WPA_AKM_PSK_SHA256 ? "WPA2-PSK-SHA256" : "WPA2-PSK";
}

u32 wpa_own_ie(u8 *o, const struct wpa_choice *c)
{
	u32 n = 0;
	o[n++] = 48;
	o[n++] = 0;
	o[n++] = 1, o[n++] = 0;                 /* version 1 */
	mcopy(o + n, oui, 3), o[n + 3] = SUITE_CCMP, n += 4;           /* group */
	o[n++] = 1, o[n++] = 0;
	mcopy(o + n, oui, 3), o[n + 3] = SUITE_CCMP, n += 4;           /* pairwise */
	o[n++] = 1, o[n++] = 0;
	mcopy(o + n, oui, 3), o[n + 3] = c->akm, n += 4;               /* AKM */
	put16(o + n, c->mfp ? WPA_CAP_MFPC : 0);                       /* rsn_supp_capab */
	n += 2;
	if (c->mfp) {
		/* no PMKID (sae_pmkid_in_assoc 0): count 0, then the group
		 * management cipher */
		o[n++] = 0, o[n++] = 0;
		mcopy(o + n, oui, 3), o[n + 3] = SUITE_BIP_CMAC_128, n += 4;
	}
	o[1] = (u8)(n - 2);
	return n;
}

u32 wpa_own_rsnx(u8 *o, const struct wpa_choice *c)
{
	if (c->akm != WPA_AKM_SAE) return 0;
	/* the field length (n - 1) in bits 0-3, SAE_H2E */
	o[0] = 244, o[1] = 1, o[2] = WPA_RSNX_SAE_H2E;
	return 3;
}

/* --- the keys ------------------------------------------------------------------- */

static bool sha256_akm(u8 akm) { return akm == WPA_AKM_PSK_SHA256 || akm == WPA_AKM_SAE; }

/* the key descriptor version an AKM's frames must have */
static u16 akm_ver(u8 akm)
{
	return akm == WPA_AKM_SAE ? WPA_KEY_INFO_TYPE_AKM_DEFINED :
	       akm == WPA_AKM_PSK_SHA256 ? WPA_KEY_INFO_TYPE_AES_128_CMAC : WPA_KEY_INFO_TYPE_HMAC_SHA1_AES;
}

void wpa_ptk(const u8 pmk[WPA_PMK_LEN], u8 akm, const u8 aa[6], const u8 spa[6], const u8 *anonce, const u8 *snonce,
	     u8 ptk[WPA_PTK_LEN])
{
	u8 d[6 + 6 + 32 + 32];
	bool lo;
	/* min(AA, SPA) || max(AA, SPA) || min(ANonce, SNonce) || max(...) */
	lo = true;
	for (int i = 0; i < 6; i++)
		if (aa[i] != spa[i]) { lo = aa[i] < spa[i]; break; }
	mcopy(d, lo ? aa : spa, 6);
	mcopy(d + 6, lo ? spa : aa, 6);
	lo = true;
	for (int i = 0; i < 32; i++)
		if (anonce[i] != snonce[i]) { lo = anonce[i] < snonce[i]; break; }
	mcopy(d + 12, lo ? anonce : snonce, 32);
	mcopy(d + 44, lo ? snonce : anonce, 32);
	if (sha256_akm(akm)) sha256_kdf_bits(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof(d), ptk, 8 * WPA_PTK_LEN);
	else sha1_prf(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof(d), ptk, WPA_PTK_LEN);
	crypto_wipe(d, sizeof(d));
}

void wpa_mic(const u8 kck[16], u8 akm, const u8 *frame, u32 len, u8 mic[16])
{
	if (sha256_akm(akm)) {
		aes_cmac(kck, frame, len, mic);
		return;
	}
	u8 h[SHA1_LEN];
	hmac_sha1(kck, 16, frame, len, h);
	mcopy(mic, h, 16);
	crypto_wipe(h, sizeof(h));
}

void wpa_init(struct wpa_sm *s, const u8 pmk[WPA_PMK_LEN], const struct wpa_choice *c, const u8 own[6], const u8 aa[6],
	      const u8 *own_ie, u32 own_ie_len, const u8 *ap_ie, u32 ap_ie_len, const u8 *ap_rsnx, u32 ap_rsnx_len,
	      void (*random)(void *p, u32 n))
{
	crypto_wipe(s, sizeof(*s));
	mcopy(s->pmk, pmk, WPA_PMK_LEN);
	s->akm = c->akm;
	s->mfp = c->mfp;
	mcopy(s->own, own, 6);
	mcopy(s->aa, aa, 6);
	s->own_ie_len = (u8)MIN(own_ie_len, (u32)WPA_MAX_IE);
	mcopy(s->own_ie, own_ie, s->own_ie_len);
	s->ap_ie_len = (u8)MIN(ap_ie_len, (u32)WPA_MAX_IE);
	mcopy(s->ap_ie, ap_ie, s->ap_ie_len);
	s->ap_rsnx_len = (u8)MIN(ap_rsnx_len, (u32)sizeof(s->ap_rsnx));
	if (ap_rsnx) mcopy(s->ap_rsnx, ap_rsnx, s->ap_rsnx_len);
	s->random = random;
	s->gtk_idx = -1;
	s->igtk_idx = -1;
}

const u8 *wpa_tk(const struct wpa_sm *s)
{
	return s->ptk + 32;
}

/* a reply: the header, key info, the replay counter, the nonce, key data;
 * the MIC with kck */
static u32 reply(struct wpa_sm *s, u8 *o, u16 info, const u8 *replay, const u8 *nonce, const u8 *data, u32 dlen,
		 const u8 *kck)
{
	u32 len = O_DATA + dlen;
	mzero(o, len);
	o[0] = EAPOL_VERSION;
	o[O_TYPE] = EAPOL_KEY;
	put_be16(o + O_LEN, (u16)(len - 4));
	o[O_DESC] = DESC_RSN;
	put_be16(o + O_INFO, (u16)(info | akm_ver(s->akm)));
	put_be16(o + O_KEYLEN, 0);              /* RSN: 0 in messages 2 and 4 */
	mcopy(o + O_REPLAY, replay, 8);
	if (nonce) mcopy(o + O_NONCE, nonce, WPA_NONCE_LEN);
	put_be16(o + O_DATALEN, (u16)dlen);
	if (dlen) mcopy(o + O_DATA, data, dlen);
	wpa_mic(kck, s->akm, o, len, o + O_MIC);
	return len;
}

static int drop(struct wpa_sm *s, const char *why)
{
	s->why = why;
	return -1;
}

/* the MIC of a received frame with kck */
static bool mic_ok(const struct wpa_sm *s, const u8 *p, u32 len, const u8 *kck)
{
	u8 f[512], m[16];
	if (len > sizeof(f)) return false;
	mcopy(f, p, len);
	mzero(f + O_MIC, 16);
	wpa_mic(kck, s->akm, f, len, m);
	bool ok = !crypto_memcmp(m, p + O_MIC, 16);
	crypto_wipe(f, len);
	return ok;
}

/* the KDEs of the (unwrapped) key data: the GTK (00-0f-ac:1) and the IGTK
 * (:9) into s; the RSN and RSNX elements compared with the beacon's
 * (wpa_supplicant_validate_ie); *got: what came */
static int key_data(struct wpa_sm *s, const u8 *d, u32 n, bool msg3, int *got)
{
	bool ie_seen = false, rsnx_seen = false;
	*got = 0;
	for (u32 i = 0; i + 2 <= n;) {
		u8 t = d[i], l = d[i + 1];
		if (t == 0xdd && l == 0) break;         /* the padding */
		if (i + 2 + l > n) return drop(s, "a key data element past the end");
		const u8 *v = d + i + 2;
		if (t == 48) {
			if (2u + l != s->ap_ie_len || crypto_memcmp(d + i, s->ap_ie, s->ap_ie_len))
				return drop(s, "message 3's RSN element is not the beacon's");
			ie_seen = true;
		} else if (t == 244) {
			if (2u + l != s->ap_rsnx_len || crypto_memcmp(d + i, s->ap_rsnx, s->ap_rsnx_len))
				return drop(s, "message 3's RSNX element is not the beacon's");
			rsnx_seen = true;
		} else if (t == 0xdd && l >= 6 && is_suite(v, 1)) {
			u32 gl = l - 6u;
			if (gl != 16) return drop(s, "a GTK not of CCMP's 16 bytes");
			s->gtk_idx = v[4] & 3;
			s->gtk_len = gl;
			mcopy(s->gtk, v + 6, gl);
			*got |= WPA_DO_GTK;
		} else if (t == 0xdd && l >= 4 && is_suite(v, 9) && s->mfp) {
			/* the IGTK KDE: key id (2, little endian), IPN (6), the key */
			if (l != 4 + 2 + 6 + 16) return drop(s, "an IGTK not of BIP-CMAC-128's 16 bytes");
			u16 id = get16(v + 4);
			if (id != 4 && id != 5) return drop(s, "an IGTK with a key id other than 4 or 5");
			s->igtk_idx = id;
			mcopy(s->ipn, v + 6, 6);
			mcopy(s->igtk, v + 12, 16);
			*got |= WPA_DO_IGTK;
		}
		i += 2u + l;
	}
	if (msg3 && !ie_seen) return drop(s, "message 3 without the AP's RSN element");
	if (msg3 && rsnx_seen != (s->ap_rsnx_len != 0)) return drop(s, "message 3's RSNX element is not the beacon's");
	return 0;
}

/* the encrypted key data: AES key unwrap with the KEK */
static int unwrap(struct wpa_sm *s, const u8 *kek, const u8 *c, u32 n, u8 *plain)
{
	if (n < 24 || n % 8 || n > 256) return drop(s, "key data not a whole number of blocks");
	if (aes_unwrap(kek, n / 8 - 1, c, plain)) return drop(s, "the key data does not unwrap (the PSK?)");
	return (int)(n - 8);
}

int wpa_rx(struct wpa_sm *s, const u8 *p, u32 len, u8 *out, u32 cap, u32 *out_len)
{
	*out_len = 0;
	s->why = NULL;
	if (len < O_DATA || p[O_TYPE] != EAPOL_KEY) return drop(s, "not an EAPOL-Key frame");
	u32 body = be16(p + O_LEN);
	if (body + 4 > len || body + 4 < O_DATA) return drop(s, "a short EAPOL-Key frame");
	len = body + 4;                             /* what follows: padding */
	if (p[O_DESC] != DESC_RSN) return drop(s, "not an RSN key descriptor");
	u16 info = be16(p + O_INFO);
	u32 dlen = be16(p + O_DATALEN);
	if (O_DATA + dlen > len) return drop(s, "key data past the frame");
	/* the descriptor version the AKM has (version 2 for PSK, 3 for
	 * PSK-SHA256, 0 for SAE) */
	if ((info & 7) != akm_ver(s->akm)) return drop(s, "not the key descriptor version of the AKM");
	if (info & (WPA_KEY_INFO_REQUEST | WPA_KEY_INFO_ERROR)) return drop(s, "a request or an error from the AP");
	if (!(info & WPA_KEY_INFO_ACK)) return drop(s, "no Ack");
	/* the replay counter goes up */
	if (s->replay_set) {
		int c = 0;
		for (int i = 0; i < 8 && !c; i++) c = (int)p[O_REPLAY + i] - (int)s->replay[i];
		if (c <= 0) return drop(s, "an old replay counter");
	}
	if (cap < O_DATA + s->own_ie_len) return drop(s, "no room for the reply");
	u8 plain[256];
	int got;
	if (info & WPA_KEY_INFO_KEY_TYPE) {
		if (!(info & WPA_KEY_INFO_MIC)) {
			/* message 1: the ANonce; the SNonce new for this
			 * association; the PTK (temporary until message 3) */
			if (!s->snonce_set) {
				s->random(s->snonce, WPA_NONCE_LEN);
				s->snonce_set = true;
			}
			mcopy(s->anonce, p + O_NONCE, WPA_NONCE_LEN);
			wpa_ptk(s->pmk, s->akm, s->aa, s->own, s->anonce, s->snonce, s->tptk);
			s->tptk_set = true;
			u16 ri = WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC;
			*out_len = reply(s, out, ri, p + O_REPLAY, s->snonce, s->own_ie, s->own_ie_len, s->tptk);
			/* the counter is taken once a MIC was right (message 3);
			 * a retried message 1 gets the same SNonce */
			return 0;
		}
		/* message 3 */
		if (!s->tptk_set) return drop(s, "message 3 before message 1");
		if (crypto_memcmp(p + O_NONCE, s->anonce, WPA_NONCE_LEN)) return drop(s, "message 3's ANonce is not message 1's");
		if (!mic_ok(s, p, len, s->tptk)) return drop(s, "message 3's MIC is wrong (the PSK?)");
		if (!(info & WPA_KEY_INFO_INSTALL) || !(info & WPA_KEY_INFO_ENCR_KEY_DATA) || !(info & WPA_KEY_INFO_SECURE))
			return drop(s, "message 3 without Install, Secure or encrypted key data");
		int n = unwrap(s, s->tptk + 16, p + O_DATA, dlen, plain);
		if (n < 0) return -1;
		if (key_data(s, plain, (u32)n, true, &got)) {
			crypto_wipe(plain, sizeof(plain));
			return -1;
		}
		crypto_wipe(plain, sizeof(plain));
		mcopy(s->replay, p + O_REPLAY, 8);
		s->replay_set = true;
		mcopy(s->ptk, s->tptk, WPA_PTK_LEN);
		s->ptk_set = true;
		mcopy(s->rsc, p + O_RSC, 8);
		u16 ri = WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE;
		*out_len = reply(s, out, ri, p + O_REPLAY, NULL, NULL, 0, s->ptk);
		s->done = true;
		return WPA_DO_PTK | (got & (WPA_DO_GTK | WPA_DO_IGTK)) | WPA_DO_DONE;
	}
	/* the group key handshake, message 1 of 2 */
	if (!s->ptk_set) return drop(s, "a group key before the pairwise key");
	if (!(info & WPA_KEY_INFO_MIC) || !mic_ok(s, p, len, s->ptk)) return drop(s, "a group key message's MIC is wrong");
	if (!(info & WPA_KEY_INFO_ENCR_KEY_DATA)) return drop(s, "a group key without encrypted key data");
	int n = unwrap(s, s->ptk + 16, p + O_DATA, dlen, plain);
	if (n < 0) return -1;
	s->gtk_idx = -1;
	int r = key_data(s, plain, (u32)n, false, &got);
	crypto_wipe(plain, sizeof(plain));
	if (r) return -1;
	if (s->gtk_idx < 0) return drop(s, "a group key message without a GTK");
	mcopy(s->replay, p + O_REPLAY, 8);
	mcopy(s->rsc, p + O_RSC, 8);
	u16 ri = WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE;
	*out_len = reply(s, out, ri, p + O_REPLAY, NULL, NULL, 0, s->ptk);
	return WPA_DO_GTK | (got & WPA_DO_IGTK);
}

/* --- BIP ------------------------------------------------------------------------- */

int wpa_bip_check(struct wpa_sm *s, const u8 *f, u32 len)
{
	if (s->igtk_idx < 0 || len < 24 + 18) return -1;
	const u8 *mmie = f + len - 18;
	if (mmie[0] != 76 || mmie[1] != 16 || get16(mmie + 2) != (u16)s->igtk_idx) return -1;
	/* the IPN (little endian) above the last one */
	int c = 0;
	for (int i = 5; i >= 0 && !c; i--) c = (int)mmie[4 + i] - (int)s->ipn[i];
	if (c <= 0) return -1;
	/* AAD: FC without retry, power management, more data; A1, A2, A3 */
	u8 aad[20], zero[8], mic[16];
	put16(aad, get16(f) & ~0x3800u);
	mcopy(aad + 2, f + 4, 18);
	mzero(zero, sizeof(zero));
	const u8 *v[3] = { aad, f + 24, zero };
	u32 l[3] = { 20, len - 24 - 8, 8 };
	aes_cmac_vec(s->igtk, 3, v, l, mic);
	if (crypto_memcmp(mic, mmie + 10, 8)) return -1;
	mcopy(s->ipn, mmie + 4, 6);
	return 0;
}
