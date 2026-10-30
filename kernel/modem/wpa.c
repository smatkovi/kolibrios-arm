/* KolibriOS for ARM: WPA2-PSK, the supplicant (wpa.h), after
 * wpa_supplicant 2.x: rsn_supp/wpa.c (wpa_supplicant_process_1_of_4,
 * _3_of_4, _send_2_of_4, _send_4_of_4, wpa_supplicant_process_1_of_2,
 * wpa_supplicant_verify_eapol_key_mic, wpa_supplicant_decrypt_key_data),
 * common/wpa_common.c (wpa_pmk_to_ptk, wpa_eapol_key_mic) and
 * rsn_supp/wpa_ie.c (the KDEs).
 *
 * An EAPOL-Key frame (IEEE 802.11-2016 12.7.2): the 802.1X header
 * (version, type 3, length, big endian), the descriptor type (2: RSN), key
 * info, key length, replay counter (8), nonce (32), IV (16), RSC (8),
 * reserved (8), MIC (16), key data length, key data; big endian. */
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

static const u8 oui_ccmp[4] = { 0x00, 0x0f, 0xac, 4 }, oui_psk[4] = { 0x00, 0x0f, 0xac, 2 };

u32 wpa_own_ie(u8 *o)
{
	static const u8 ie[] = {
		48, 20, 1, 0,                   /* RSN, version 1 */
		0x00, 0x0f, 0xac, 4,            /* group: CCMP */
		1, 0, 0x00, 0x0f, 0xac, 4,      /* pairwise: CCMP */
		1, 0, 0x00, 0x0f, 0xac, 2,      /* AKM: PSK */
		0, 0,                           /* capabilities */
	};
	mcopy(o, ie, sizeof(ie));
	return sizeof(ie);
}

/* wpa_parse_wpa_ie_rsn, as far as needed: the group cipher CCMP, CCMP
 * among the pairwise ones, PSK among the AKMs, management frame protection
 * not required */
int wpa_check_ap_ie(const u8 *ie, u32 len, const char **why)
{
	*why = "a short RSN element";
	if (len < 2 || ie[0] != 48 || ie[1] + 2u > len || ie[1] < 2 || ie[2] != 1 || ie[3] != 0) return -1;
	const u8 *p = ie + 4, *end = ie + 2 + ie[1];
	*why = "no PSK among the key managements (the defaults: 802.1X)";
	if (end - p < 4) return -1;
	*why = "the group cipher is not CCMP (TKIP: not done here)";
	if (crypto_memcmp(p, oui_ccmp, 4)) return -1;
	p += 4;
	bool ok = false;
	*why = "a short RSN element";
	if (end - p < 2) return -1;   /* (the pairwise default: CCMP, but PSK needs the AKM list) */
	u32 n = get16(p);
	p += 2;
	if ((u32)(end - p) < 4 * n) return -1;
	for (u32 i = 0; i < n; i++, p += 4)
		if (!crypto_memcmp(p, oui_ccmp, 4)) ok = true;
	*why = "no CCMP among the pairwise ciphers";
	if (!ok) return -1;
	*why = "a short RSN element";
	if (end - p < 2) return -1;
	n = get16(p);
	p += 2;
	if ((u32)(end - p) < 4 * n) return -1;
	ok = false;
	for (u32 i = 0; i < n; i++, p += 4)
		if (!crypto_memcmp(p, oui_psk, 4)) ok = true;
	*why = "no PSK among the key managements (SAE/WPA3 or 802.1X: not done here)";
	if (!ok) return -1;
	if (end - p >= 2 && (get16(p) & 1u << 6)) {
		*why = "management frame protection required (MFPR)";
		return -1;
	}
	*why = NULL;
	return 0;
}

void wpa_ptk(const u8 pmk[WPA_PMK_LEN], const u8 aa[6], const u8 spa[6], const u8 *anonce, const u8 *snonce,
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
	sha1_prf(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof(d), ptk, WPA_PTK_LEN);
	crypto_wipe(d, sizeof(d));
}

void wpa_mic(const u8 kck[16], const u8 *frame, u32 len, u8 mic[16])
{
	u8 h[SHA1_LEN];
	hmac_sha1(kck, 16, frame, len, h);
	mcopy(mic, h, 16);
	crypto_wipe(h, sizeof(h));
}

void wpa_init(struct wpa_sm *s, const u8 pmk[WPA_PMK_LEN], const u8 own[6], const u8 aa[6], const u8 *own_ie,
	      u32 own_ie_len, const u8 *ap_ie, u32 ap_ie_len, void (*random)(void *p, u32 n))
{
	crypto_wipe(s, sizeof(*s));
	mcopy(s->pmk, pmk, WPA_PMK_LEN);
	mcopy(s->own, own, 6);
	mcopy(s->aa, aa, 6);
	s->own_ie_len = (u8)MIN(own_ie_len, (u32)WPA_MAX_IE);
	mcopy(s->own_ie, own_ie, s->own_ie_len);
	s->ap_ie_len = (u8)MIN(ap_ie_len, (u32)WPA_MAX_IE);
	mcopy(s->ap_ie, ap_ie, s->ap_ie_len);
	s->random = random;
	s->gtk_idx = -1;
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
	put_be16(o + O_INFO, info);
	put_be16(o + O_KEYLEN, 0);              /* RSN: 0 in messages 2 and 4 */
	mcopy(o + O_REPLAY, replay, 8);
	if (nonce) mcopy(o + O_NONCE, nonce, WPA_NONCE_LEN);
	put_be16(o + O_DATALEN, (u16)dlen);
	if (dlen) mcopy(o + O_DATA, data, dlen);
	wpa_mic(kck, o, len, o + O_MIC);
	(void)s;
	return len;
}

static int drop(struct wpa_sm *s, const char *why)
{
	s->why = why;
	return -1;
}

/* the MIC of a received frame with kck */
static bool mic_ok(const u8 *p, u32 len, const u8 *kck)
{
	u8 f[512], m[16];
	if (len > sizeof(f)) return false;
	mcopy(f, p, len);
	mzero(f + O_MIC, 16);
	wpa_mic(kck, f, len, m);
	bool ok = !crypto_memcmp(m, p + O_MIC, 16);
	crypto_wipe(f, len);
	return ok;
}

/* the KDEs of the (unwrapped) key data: the GTK (00-0f-ac:1) into s, the
 * RSN IE compared with the beacon's (wpa_supplicant_validate_ie) */
static int key_data(struct wpa_sm *s, const u8 *d, u32 n, bool want_ie)
{
	bool ie_seen = false;
	for (u32 i = 0; i + 2 <= n;) {
		u8 t = d[i], l = d[i + 1];
		if (t == 0xdd && l == 0) break;         /* the padding */
		if (i + 2 + l > n) return drop(s, "a key data element past the end");
		const u8 *v = d + i + 2;
		if (t == 48) {
			if (2u + l != s->ap_ie_len || crypto_memcmp(d + i, s->ap_ie, s->ap_ie_len))
				return drop(s, "message 3's RSN element is not the beacon's");
			ie_seen = true;
		} else if (t == 0xdd && l >= 6 && v[0] == 0x00 && v[1] == 0x0f && v[2] == 0xac && v[3] == 1) {
			u32 gl = l - 6u;
			if (gl != 16) return drop(s, "a GTK not of CCMP's 16 bytes");
			s->gtk_idx = v[4] & 3;
			s->gtk_len = gl;
			mcopy(s->gtk, v + 6, gl);
		}
		i += 2u + l;
	}
	if (want_ie && !ie_seen) return drop(s, "message 3 without the AP's RSN element");
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
	if ((info & 7) != WPA_KEY_INFO_TYPE_HMAC_SHA1_AES) return drop(s, "not key descriptor version 2");
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
	if (info & WPA_KEY_INFO_KEY_TYPE) {
		if (!(info & WPA_KEY_INFO_MIC)) {
			/* message 1: the ANonce; the SNonce new for this
			 * association; the PTK (temporary until message 3) */
			if (!s->snonce_set) {
				s->random(s->snonce, WPA_NONCE_LEN);
				s->snonce_set = true;
			}
			mcopy(s->anonce, p + O_NONCE, WPA_NONCE_LEN);
			wpa_ptk(s->pmk, s->aa, s->own, s->anonce, s->snonce, s->tptk);
			s->tptk_set = true;
			u16 ri = WPA_KEY_INFO_TYPE_HMAC_SHA1_AES | WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC;
			*out_len = reply(s, out, ri, p + O_REPLAY, s->snonce, s->own_ie, s->own_ie_len, s->tptk);
			/* the counter is taken once a MIC was right (message 3);
			 * a retried message 1 gets the same SNonce */
			return 0;
		}
		/* message 3 */
		if (!s->tptk_set) return drop(s, "message 3 before message 1");
		if (crypto_memcmp(p + O_NONCE, s->anonce, WPA_NONCE_LEN)) return drop(s, "message 3's ANonce is not message 1's");
		if (!mic_ok(p, len, s->tptk)) return drop(s, "message 3's MIC is wrong (the PSK?)");
		if (!(info & WPA_KEY_INFO_INSTALL) || !(info & WPA_KEY_INFO_ENCR_KEY_DATA) || !(info & WPA_KEY_INFO_SECURE))
			return drop(s, "message 3 without Install, Secure or encrypted key data");
		int n = unwrap(s, s->tptk + 16, p + O_DATA, dlen, plain);
		if (n < 0) return -1;
		if (key_data(s, plain, (u32)n, true)) {
			crypto_wipe(plain, sizeof(plain));
			return -1;
		}
		crypto_wipe(plain, sizeof(plain));
		mcopy(s->replay, p + O_REPLAY, 8);
		s->replay_set = true;
		mcopy(s->ptk, s->tptk, WPA_PTK_LEN);
		s->ptk_set = true;
		mcopy(s->rsc, p + O_RSC, 8);
		u16 ri = WPA_KEY_INFO_TYPE_HMAC_SHA1_AES | WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE;
		*out_len = reply(s, out, ri, p + O_REPLAY, NULL, NULL, 0, s->ptk);
		s->done = true;
		return WPA_DO_PTK | (s->gtk_idx >= 0 ? WPA_DO_GTK : 0) | WPA_DO_DONE;
	}
	/* the group key handshake, message 1 of 2 */
	if (!s->ptk_set) return drop(s, "a group key before the pairwise key");
	if (!(info & WPA_KEY_INFO_MIC) || !mic_ok(p, len, s->ptk)) return drop(s, "a group key message's MIC is wrong");
	if (!(info & WPA_KEY_INFO_ENCR_KEY_DATA)) return drop(s, "a group key without encrypted key data");
	int n = unwrap(s, s->ptk + 16, p + O_DATA, dlen, plain);
	if (n < 0) return -1;
	s->gtk_idx = -1;
	int r = key_data(s, plain, (u32)n, false);
	crypto_wipe(plain, sizeof(plain));
	if (r) return -1;
	if (s->gtk_idx < 0) return drop(s, "a group key message without a GTK");
	mcopy(s->replay, p + O_REPLAY, 8);
	mcopy(s->rsc, p + O_RSC, 8);
	u16 ri = WPA_KEY_INFO_TYPE_HMAC_SHA1_AES | WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE;
	*out_len = reply(s, out, ri, p + O_REPLAY, NULL, NULL, 0, s->ptk);
	return WPA_DO_GTK;
}
